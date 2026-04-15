/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define SOCKET_PATH "/tmp/mini_runtime.sock"
#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
    void *stack;
    int stop_requested;
    char rootfs[PATH_MAX];
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int shutting_down;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    int fd;
    char container_id[CONTAINER_ID_LEN];
    supervisor_ctx_t *ctx;
} producer_arg_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 *
 *
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    // Wait while buffer is full
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    // If shutdown started, stop accepting new items
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Insert item
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    // Wake up consumers
    pthread_cond_signal(&buffer->not_empty);

    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    // Wait while buffer is empty
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    // If empty AND shutting down → exit cleanly
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    // Remove item
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    // Wake up producers
    pthread_cond_signal(&buffer->not_full);

    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}
/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    // Ensure logs directory exists
    mkdir(LOG_DIR, 0755);

    while (1) {
        // Pop item from buffer
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0) {
            // Shutdown condition: buffer empty + shutting_down
            break;
        }

        // Build log file path: logs/<container_id>.log
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        // Open file (append mode)
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("open log file");
            continue;
        }

        // Write log data
        ssize_t written = write(fd, item.data, item.length);
        if (written < 0) {
            perror("write log");
        }

        close(fd);
    }

    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    // Set hostname (UTS namespace)
    if (sethostname(cfg->id, strlen(cfg->id)) != 0) {
        perror("sethostname");
        return 1;
    }

    // Make mount namespace private (IMPORTANT)
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("mount MS_PRIVATE");
        return 1;
    }

    // Enter container root filesystem
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    // Mount /proc inside container
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
        return 1;
    }

    // Redirect stdout and stderr to logging pipe
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }

    close(cfg->log_write_fd);

    // Set nice value if needed
    if (cfg->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) != 0) {
            perror("setpriority");
        }
    }
    // Execute command
    execl("/bin/sh", "sh", "-c", cfg->command, NULL);

    perror("exec failed");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static volatile sig_atomic_t global_shutdown = 0;

static void handle_signal(int sig)
{
    (void)sig;
    global_shutdown = 1;
}

void *log_producer(void *arg) {
    producer_arg_t *p = arg;

    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (1) {
    ssize_t r = read(p->fd, item.data, LOG_CHUNK_SIZE);
    if (r <= 0) break;

    item.length = r;

    strncpy(item.container_id, p->container_id, CONTAINER_ID_LEN);
    item.container_id[CONTAINER_ID_LEN - 1] = '\0';

    if (bounded_buffer_push(&p->ctx->log_buffer, &item) != 0)
    break;
}

    close(p->fd);
    free(p);
    return NULL;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.shutting_down = 0;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* 1) open /dev/container_monitor */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("monitor open (continuing without it)");
    }

    /* 2) create control socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    /* Make socket non-blocking so accept() can be interrupted by signals */
    int flags = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);

    unlink(SOCKET_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen");
        goto cleanup;
    }

    /* 3) install signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    /* 4) spawn logger thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create");
        goto cleanup;
    }

    /* 5) main event loop */
    while (!global_shutdown) {
        if (global_shutdown) break;
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No pending connections, check shutdown flag and sleep briefly */
                usleep(100000);  /* 100ms */
                continue;
            }
            break;
        }

        // TODO: handle commands (run / ps / stop / logs)
        control_request_t req;
ssize_t n = read(client_fd, &req, sizeof(req));

if (n != sizeof(req)) {
    close(client_fd);
    continue;
}

control_response_t resp;
memset(&resp, 0, sizeof(resp));
resp.status = 0;

if (req.kind == CMD_START || req.kind == CMD_RUN) {

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        close(client_fd);
        continue;
    }

    child_config_t *cfg = malloc(sizeof(*cfg));
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->id, req.container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req.rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req.command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req.nice_value;
    cfg->log_write_fd = pipefd[1];

    void *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        close(pipefd[0]);
        close(pipefd[1]);
        free(cfg);
        close(client_fd);
        continue;
    }
        container_record_t *tmp = ctx.containers;
        while (tmp) {
            if (strcmp(tmp->rootfs, req.rootfs) == 0 &&
                tmp->state == CONTAINER_RUNNING) {

                snprintf(resp.message, sizeof(resp.message),
                        "Rootfs already in use\n");

                (void)write(client_fd, &resp, sizeof(resp));
                close(client_fd);

                goto next_client;
            }
            tmp = tmp->next;
        }
    pid_t pid = clone(child_fn,
                    (char *)stack + STACK_SIZE,
                    CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                    cfg);
    free(cfg);
    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        free(stack);
        close(client_fd);
        continue;
    }

    close(pipefd[1]); // parent reads

    // Register with monitor
    if (ctx.monitor_fd >= 0) {
        register_with_monitor(ctx.monitor_fd,
                              req.container_id,
                              pid,
                              req.soft_limit_bytes,
                              req.hard_limit_bytes);
    }

    // Add metadata
    pthread_mutex_lock(&ctx.metadata_lock);

    // check if already exists → remove old
container_record_t **cur = &ctx.containers;
while (*cur) {
    if (strcmp((*cur)->id, req.container_id) == 0) {
        container_record_t *tmp = *cur;
        *cur = tmp->next;
        if (tmp->stack) free(tmp->stack);
        free(tmp);
        break;
    }
    cur = &(*cur)->next;
    next_client:
;
}

container_record_t *rec = malloc(sizeof(*rec));
memset(rec, 0, sizeof(*rec));

strncpy(rec->id, req.container_id, CONTAINER_ID_LEN - 1);
strncpy(rec->rootfs, req.rootfs, PATH_MAX - 1);   

rec->host_pid = pid;
rec->state = CONTAINER_RUNNING;
rec->started_at = time(NULL);
rec->soft_limit_bytes = req.soft_limit_bytes;
rec->hard_limit_bytes = req.hard_limit_bytes;

snprintf(rec->log_path, sizeof(rec->log_path),  
         "%s/%s.log", LOG_DIR, rec->id);

rec->next = ctx.containers;
ctx.containers = rec;
rec->stack = stack;
pthread_mutex_unlock(&ctx.metadata_lock);
    // Logging producer
    producer_arg_t *p = malloc(sizeof(*p));
    if (!p) {
    close(pipefd[0]);
    return 1;
}
    p->fd = pipefd[0];
    p->ctx = &ctx;
    strncpy(p->container_id, req.container_id, CONTAINER_ID_LEN);

    pthread_t tid;
    pthread_create(&tid, NULL, log_producer, p);
    pthread_detach(tid);

if (req.kind == CMD_RUN) {
    snprintf(resp.message, sizeof(resp.message),
             "Running container %s (pid=%d)\n",
             req.container_id, pid);
} else {
    snprintf(resp.message, sizeof(resp.message),
             "Started container %s\n",
             req.container_id);
}
}

else if (req.kind == CMD_PS) {

    pthread_mutex_lock(&ctx.metadata_lock);

    char buf[1024] = {0};
    container_record_t *cur = ctx.containers;

    while (cur) {
        char line[128];
        snprintf(line, sizeof(line),
                 "%s | pid=%d | %s\n",
                 cur->id, cur->host_pid,
                 state_to_string(cur->state));
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        cur = cur->next;
    }

    pthread_mutex_unlock(&ctx.metadata_lock);

    strncpy(resp.message, buf, sizeof(resp.message) - 1);
}
else if (req.kind == CMD_STOP) {

    pthread_mutex_lock(&ctx.metadata_lock);

    container_record_t *cur = ctx.containers;

    while (cur) {
        if (strcmp(cur->id, req.container_id) == 0) {
            cur->stop_requested = 1;
            // send SIGTERM
            kill(cur->host_pid, SIGTERM);

            // unregister from monitor
            if (ctx.monitor_fd >= 0) {
                unregister_from_monitor(ctx.monitor_fd,
                                        cur->id,
                                        cur->host_pid);
            }



            cur->state = CONTAINER_STOPPED;

            snprintf(resp.message, sizeof(resp.message),
                     "Stopped %s\n", cur->id);

            break;
        }
        cur = cur->next;
    }

    if (!cur) {
        snprintf(resp.message, sizeof(resp.message),
                 "Container not found\n");
    }

    pthread_mutex_unlock(&ctx.metadata_lock);
}
else if (req.kind == CMD_LOGS) {

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(resp.message, sizeof(resp.message),
                 "No logs found\n");
    } else {
        ssize_t r = read(fd, resp.message, sizeof(resp.message) - 1);
        if (r > 0)
            resp.message[r] = '\0';
        close(fd);
    }
}

// send response
(void)write(client_fd, &resp, sizeof(resp));
close(client_fd);

        // Reap children
        while (1) {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid <= 0)
                break;

            // TODO: update metadata
            pthread_mutex_lock(&ctx.metadata_lock);

            container_record_t *cur = ctx.containers;

            while (cur) {
                    if (cur->host_pid == pid) {

                        if (WIFEXITED(status)) {
                            cur->state = CONTAINER_EXITED;
                            cur->exit_code = WEXITSTATUS(status);
                            cur->exit_signal = 0;
                        } else if (WIFSIGNALED(status)) {
                            cur->exit_signal = WTERMSIG(status);

                            if (cur->stop_requested)
                                cur->state = CONTAINER_STOPPED;
                            else if (cur->exit_signal == SIGKILL)
                                cur->state = CONTAINER_KILLED;
                            else
                                cur->state = CONTAINER_EXITED;
                        }
                        if (cur->stack) {
                            free(cur->stack);
                            cur->stack = NULL;
                        }
                        break;
                    }
                    cur = cur->next;
                }

                pthread_mutex_unlock(&ctx.metadata_lock);
        }
    }


pthread_mutex_lock(&ctx.metadata_lock);

container_record_t *cur = ctx.containers;
while (cur) {
    kill(cur->host_pid, SIGTERM);
    cur = cur->next;
}

pthread_mutex_unlock(&ctx.metadata_lock);    
cleanup:
    ctx.shutting_down = 1;
    bounded_buffer_begin_shutdown(&ctx.log_buffer);

    pthread_join(ctx.logger_thread, NULL);

    if (ctx.server_fd >= 0)
        close(ctx.server_fd);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    unlink(SOCKET_PATH);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    // Send request
    ssize_t n = write(fd, req, sizeof(*req));
    if (n != sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    control_response_t resp;
    n = read(fd, &resp, sizeof(resp));

    if (n == sizeof(resp)) {
        printf("%s", resp.message);
    } else {
        fprintf(stderr, "Invalid response from supervisor\n");
    }
    close(fd);
    return 0;
}

static int is_engine_flag(const char *arg)
{
    return strcmp(arg, "--soft-mib") == 0 ||
           strcmp(arg, "--hard-mib") == 0 ||
           strcmp(arg, "--nice") == 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    int i;
    size_t cmd_len = 0;
    char *cmd_ptr;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command...> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    // Combine command and its arguments into a single string
    cmd_ptr = req.command;
    for (i = 4; i < argc && !is_engine_flag(argv[i]); i++) {
        if (i > 4) {
            // Add space between arguments
            if (cmd_len + 1 >= sizeof(req.command)) {
                fprintf(stderr, "Command too long\n");
                return 1;
            }
            cmd_ptr += snprintf(cmd_ptr, sizeof(req.command) - cmd_len, " ");
            cmd_len += strlen(" ");
        }
        size_t arg_len = strlen(argv[i]);
        if (cmd_len + arg_len >= sizeof(req.command)) {
            fprintf(stderr, "Command too long\n");
            return 1;
        }
        cmd_ptr += snprintf(cmd_ptr, sizeof(req.command) - cmd_len, "%s", argv[i]);
        cmd_len += arg_len;
    }

    if (parse_optional_flags(&req, argc, argv, i) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    int i;
    size_t cmd_len = 0;
    char *cmd_ptr;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command...> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    // Combine command and its arguments into a single string
    cmd_ptr = req.command;
    for (i = 4; i < argc && !is_engine_flag(argv[i]); i++) {
        if (i > 4) {
            // Add space between arguments
            if (cmd_len + 1 >= sizeof(req.command)) {
                fprintf(stderr, "Command too long\n");
                return 1;
            }
            cmd_ptr += snprintf(cmd_ptr, sizeof(req.command) - cmd_len, " ");
            cmd_len += strlen(" ");
        }
        size_t arg_len = strlen(argv[i]);
        if (cmd_len + arg_len >= sizeof(req.command)) {
            fprintf(stderr, "Command too long\n");
            return 1;
        }
        cmd_ptr += snprintf(cmd_ptr, sizeof(req.command) - cmd_len, "%s", argv[i]);
        cmd_len += arg_len;
    }

    if (parse_optional_flags(&req, argc, argv, i) != 0)
        return 1;

    return send_control_request(&req);
}

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
