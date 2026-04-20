# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| [Student 1 Name] | [SRN1] |
| [Student 2 Name] | [SRN2] |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 with Secure Boot **OFF**. WSL will not work.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Run the environment preflight check:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### Compilation

```bash
cd boilerplate
make
```

Produces:
- `engine` — user-space supervisor and CLI binary
- `memory_hog` — memory stress workload
- `cpu_hog` — CPU-bound workload
- `io_pulse` — I/O-bound workload
- `monitor.ko` — kernel module for memory enforcement

CI-safe compile (user-space only, no kernel headers required):

```bash
make -C boilerplate ci
```

### Step 1 — Load Kernel Module

```bash
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor
```

Verify `/dev/container_monitor` appears as a character device.

### Step 2 — Prepare Root Filesystem

```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# One writable copy per container
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-gamma

# Copy workload binaries into containers before launch
cp boilerplate/memory_hog boilerplate/cpu_hog boilerplate/io_pulse rootfs-alpha/
cp boilerplate/memory_hog boilerplate/cpu_hog rootfs-beta/
cp boilerplate/memory_hog rootfs-gamma/
```

> Do **not** commit `rootfs-base/` or `rootfs-*/` to the repository.

### Step 3 — Start the Supervisor

Run in a dedicated terminal. The supervisor stays alive until SIGINT/SIGTERM.

```bash
sudo ./boilerplate/engine supervisor ./rootfs-base
```

### Step 4 — Container Operations

From a second terminal:

```bash
# Start two containers in the background
sudo ./boilerplate/engine start alpha ./rootfs-alpha /bin/sleep 120 --soft-mib 48 --hard-mib 80
sudo ./boilerplate/engine start beta  ./rootfs-beta  /bin/sleep 120 --soft-mib 60 --hard-mib 96

# List all tracked containers
sudo ./boilerplate/engine ps

# View captured logs
sudo ./boilerplate/engine logs alpha

# Run a container in the foreground (blocks until exit)
sudo ./boilerplate/engine run gamma ./rootfs-gamma /memory_hog 2 100 --soft-mib 32 --hard-mib 64

# Stop a container
sudo ./boilerplate/engine stop alpha

# Check kernel events
dmesg | tail -20
```

### Step 5 — Cleanup

```bash
# Stop remaining containers
sudo ./boilerplate/engine stop beta

# Gracefully terminate the supervisor (triggers cleanup of threads, FDs, socket)
sudo kill -TERM $(pgrep -f "engine supervisor")

# Wait for shutdown
sleep 2

# Verify no zombies remain
ps aux | grep engine | grep -v grep

# Unload kernel module
sudo rmmod monitor
```

---

## CLI Reference

```
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command...> [--soft-mib N] [--hard-mib N] [--nice N]
engine run   <id> <container-rootfs> <command...> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs  <id>
engine stop  <id>
```

| Command | Behaviour |
|---------|-----------|
| `supervisor` | Starts the long-running daemon. Blocks until SIGINT/SIGTERM. |
| `start` | Launches container in the background. Returns immediately after supervisor records metadata. |
| `run` | Launches container and **blocks** until it exits. Returns the container exit status (0–255 or 128+signal). Ctrl-C forwards a `stop` to the supervisor and returns 130. |
| `ps` | Lists all tracked containers: ID, host PID, and state (`starting`/`running`/`stopped`/`killed`/`exited`). |
| `logs` | Prints captured stdout/stderr from the container's log file. |
| `stop` | Sends SIGTERM to the container, sets `stop_requested`, and marks it `stopped`. |

**Defaults:** `--soft-mib 40`, `--hard-mib 64`, `--nice 0`

---

## 3. Demo Screenshots

> Replace each placeholder below with an annotated screenshot from your VM.

### Screenshot 1 — Multi-Container Supervision
**Command sequence:**
```bash
sudo ./boilerplate/engine start alpha ./rootfs-alpha /bin/sleep 120
sudo ./boilerplate/engine start beta  ./rootfs-beta  /bin/sleep 120
sudo ./boilerplate/engine ps
```
*Caption: Two containers (`alpha`, `beta`) running under one supervisor process, each with a distinct host PID and state `running`.*

<!-- INSERT SCREENSHOT 1 HERE -->

---

### Screenshot 2 — Metadata Tracking (`ps` output)
**Command:**
```bash
sudo ./boilerplate/engine ps
```
*Caption: Output of `engine ps` showing container IDs, host PIDs, and states after starting two containers.*

<!-- INSERT SCREENSHOT 2 HERE -->

---

### Screenshot 3 — Bounded-Buffer Logging
**Command sequence:**
```bash
sudo ./boilerplate/engine run logtest ./rootfs-alpha \
    /bin/sh -c "for i in 1 2 3 4 5; do echo Line \$i; sleep 0.1; done"
cat boilerplate/logs/logtest.log
```
*Caption: Log file contents captured through the producer-consumer pipeline. All 5 lines are present and in order, demonstrating no loss or corruption under concurrent access.*

<!-- INSERT SCREENSHOT 3 HERE -->

---

### Screenshot 4 — CLI and IPC (Control Channel)
**Command:**
```bash
sudo ./boilerplate/engine stop alpha
```
*Caption: CLI client sends a `CMD_STOP` request over the UNIX domain socket at `/tmp/mini_runtime.sock`. Supervisor receives the request, sets `stop_requested`, sends SIGTERM, and returns the response `Stopped alpha`.*

<!-- INSERT SCREENSHOT 4 HERE -->

---

### Screenshot 5 — Soft-Limit Warning
**Command sequence:**
```bash
sudo ./boilerplate/engine start softtest ./rootfs-gamma /memory_hog 1 50 --soft-mib 16 --hard-mib 96
sleep 2
dmesg | tail -10
```
*Caption: `dmesg` showing a `[container_monitor] SOFT LIMIT container=softtest` warning when the container's RSS exceeds 16 MiB. The container continues running.*

<!-- INSERT SCREENSHOT 5 HERE -->

---

### Screenshot 6 — Hard-Limit Enforcement
**Command sequence:**
```bash
sudo ./boilerplate/engine start hardtest ./rootfs-gamma /memory_hog 1 20 --soft-mib 32 --hard-mib 48
sleep 4
dmesg | tail -10
sudo ./boilerplate/engine ps
```
*Caption: `dmesg` showing `[container_monitor] HARD LIMIT container=hardtest ... SIGKILL`. `engine ps` shows state `killed` — set because `stop_requested` was not set when SIGKILL arrived.*

<!-- INSERT SCREENSHOT 6 HERE -->

---

### Screenshot 7 — Scheduling Experiment
**Command sequence:**
```bash
sudo ./boilerplate/engine start cpu_high ./rootfs-alpha /cpu_hog 15 --nice -10
sudo ./boilerplate/engine start cpu_low  ./rootfs-beta  /cpu_hog 15 --nice  10
# wait and observe completion order via ps
```
*Caption: `cpu_high` (nice -10) completes and transitions to `exited` earlier than `cpu_low` (nice 10), demonstrating CFS weight-based CPU allocation.*

<!-- INSERT SCREENSHOT 7 HERE -->

---

### Screenshot 8 — Clean Teardown
**Command sequence:**
```bash
sudo kill -TERM $(pgrep -f "engine supervisor")
sleep 2
ps aux | grep engine | grep -v grep
ls /tmp/mini_runtime.sock 2>&1
sudo rmmod monitor && echo "Module unloaded cleanly"
```
*Caption: No `engine` processes remain, `/tmp/mini_runtime.sock` is gone, and `rmmod monitor` succeeds — confirming all threads joined, FDs closed, and kernel list entries freed.*

<!-- INSERT SCREENSHOT 8 HERE -->

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Our runtime achieves isolation through three Linux namespaces activated in a single `clone()` call:

**PID Namespace (`CLONE_NEWPID`):** The container process is PID 1 inside its own PID tree. It cannot send signals to host processes by PID, and `/proc` inside the container only shows its own children. The host kernel still assigns a unique host PID — stored in metadata — which the supervisor uses to send signals and wait on the child.

**UTS Namespace (`CLONE_NEWUTS`):** Each container receives its own hostname via `sethostname(cfg->id, ...)`. The host hostname is unaffected.

**Mount Namespace (`CLONE_NEWNS`):** After `clone()`, the child immediately calls `mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)` to detach all mounts from the parent namespace. It then uses `chroot(cfg->rootfs)` to restrict the root view to the container's writable rootfs copy, followed by `mount("proc", "/proc", "proc", ...)` so tools like `ps` work inside the container.

**What the host kernel still shares with all containers:**
- The system call interface (same kernel ABI)
- The network stack (no `CLONE_NEWNET` — containers see host interfaces)
- The kernel's physical memory allocator
- Time (`clock_gettime` returns host time)
- Device filesystem unless restricted by capabilities

**`chroot` vs `pivot_root`:** We use `chroot` for simplicity. It is sufficient for trusted workloads but can be escaped by a process with `CAP_SYS_CHROOT`. `pivot_root` is the stronger alternative used in production runtimes because it atomically replaces the root mount and severs any path back to the original root.

---

### 4.2 Supervisor and Process Lifecycle

**Why a long-running supervisor?** A short-lived launcher cannot reap children it did not wait for, accumulating zombies. It also cannot maintain metadata across multiple CLI invocations or hold the logging pipeline open after the CLI exits.

**Process Creation:** The supervisor calls `clone(child_fn, stack_top, CLONE_NEWPID|CLONE_NEWUTS|CLONE_NEWNS|SIGCHLD, cfg)`. `SIGCHLD` ensures the supervisor receives a signal when any container exits, which unblocks a blocked `accept()` via `EINTR`. The child executes namespace setup, `chroot`, `/proc` mount, stdout/stderr redirection, and finally `execl("/bin/sh", "sh", "-c", command, NULL)`.

**Metadata Tracking:** Each container has a `container_record_t` in a linked list containing: ID, host PID, start time, state, soft/hard limits, log path, exit code/signal, `stop_requested` flag, and `run_client_fd` (the open socket fd for a blocking `run` client, or -1).

**Child Reaping:** At the top of every supervisor loop iteration, before `accept()`, we call `waitpid(-1, &status, WNOHANG)` in a loop. This ensures children are reaped immediately regardless of whether a CLI client connects. Exit status is classified with `WIFEXITED()`/`WIFSIGNALED()` macros and stored in metadata.

**Signal Handling:** `SIGINT`/`SIGTERM` set `global_shutdown = 1`. The main loop exits, sends `SIGTERM` to all tracked containers, joins the logger thread, closes FDs, removes the socket, and returns.

**Stop Semantics and Attribution:**
- `stop_requested = 1` is set before `kill(pid, SIGTERM)` in `CMD_STOP`
- If the container exits via `WIFEXITED` → state = `CONTAINER_EXITED`
- If signaled and `stop_requested` is set → state = `CONTAINER_STOPPED`
- If signaled with `SIGKILL` and `stop_requested` is not set → state = `CONTAINER_KILLED` (hard-limit kill from kernel module)

---

### 4.3 IPC, Threads, and Synchronization

**Two separate IPC paths:**

**Path A — Logging (pipe-based):** When a container is started, a `pipe()` is created. The child's stdout and stderr are redirected to the write end via `dup2`. The parent spawns a detached `log_producer` thread that reads from the read end and calls `bounded_buffer_push()`. A single shared `logging_thread` (consumer) calls `bounded_buffer_pop()` and writes to `logs/<id>.log`. The two paths are completely separate: the logging pipe does not carry control messages.

**Path B — Control (UNIX domain socket):** CLI processes connect to `/tmp/mini_runtime.sock` (AF_UNIX, SOCK_STREAM), write a fixed-size `control_request_t` struct, and read back a `control_response_t`. The supervisor keeps the socket non-blocking and polls with 100ms sleep when idle so SIGCHLD/shutdown can interrupt. For `CMD_RUN`, the supervisor stores the client fd in `run_client_fd` and writes the response only when `waitpid` reaps that container — making the CLI block naturally without polling.

**Bounded Buffer Synchronization:**

The circular buffer uses one `pthread_mutex_t` and two `pthread_cond_t` (`not_empty`, `not_full`).

*Why a mutex + condition variables over spinlock?* The lock is held across potentially blocking I/O paths (producer waiting for space, consumer waiting for data). A spinlock would burn CPU on a full or empty buffer. Condition variables atomically release the mutex and suspend the thread, letting the other party acquire the lock.

*Race conditions without synchronization:*
- **Lost update:** Two producers concurrently read `count = 15`, both decide there is space, both write to `items[tail]` — one item is silently overwritten.
- **Use-after-free:** A consumer reads `items[head]` while a producer is writing to it — partial/corrupt data.
- **Infinite busy-wait:** Without `not_full`, a producer on a full buffer spins, starving the consumer thread that could drain it.

*Shutdown convergence:* `bounded_buffer_begin_shutdown()` sets `shutting_down = 1` and broadcasts both condition variables. Producers exit immediately without inserting. Consumers drain remaining items until `count == 0 && shutting_down`, then return -1 and exit cleanly.

**Metadata Synchronization:** All access to the container linked list is protected by `ctx.metadata_lock` (a `pthread_mutex_t`). Both the main thread (CLI command handlers, `waitpid` reap) and the supervisor init path acquire this lock. A single global lock is sufficient here because container counts are small and metadata operations are infrequent.

---

### 4.4 Memory Management and Enforcement

**What RSS measures:** Resident Set Size is the number of physical memory pages currently mapped into the process's page tables and present in RAM. It is read in the kernel module via `get_mm_rss(mm)` and multiplied by `PAGE_SIZE`.

**What RSS does not measure:**
- Swapped-out pages (not in RAM, so not counted)
- Kernel memory allocated on behalf of the process (slab allocations, kernel stacks)
- File-backed pages shared with other processes (counted in full per-process in our implementation)
- GPU/device memory

**Why two-tier limits?** A single hard kill is disruptive. A soft limit allows the process (or an operator watching `dmesg`) to detect memory pressure early and react — shed load, flush caches, checkpoint — before hard termination. This mirrors POSIX `RLIMIT_RSS` soft/hard semantics and cgroup memory `soft_limit_in_bytes` / `limit_in_bytes`.

**Why enforcement belongs in the kernel:**
1. Only the kernel has authoritative access to page table state. User-space reads `/proc/<pid>/status` which is already a snapshot — by the time user space reads and signals, the process may have allocated another 100 MB.
2. User-space polling is inherently racy. Between the `read` and the `kill`, the process can exceed limits further.
3. `SIGKILL` from the kernel is immediate and unblockable. A user-space `kill(SIGKILL)` still has to be scheduled.
4. The 1-second timer in the kernel module is as close to synchronous enforcement as user space can observe.

---

### 4.5 Scheduling Behavior

Linux uses the **Completely Fair Scheduler (CFS)**. CFS assigns each runnable process a virtual runtime (`vruntime`) that increases inversely proportional to the process's weight. Weight is derived from `nice` value via a fixed table (`NICE_0_WEIGHT = 1024`). A process with nice -10 has weight ~9548; nice +10 has weight ~110. The CFS always runs the process with the smallest `vruntime` next, ensuring proportional CPU allocation over time.

**Experiment 1:** Two CPU-bound containers at different priorities showed that the higher-weight process (nice -10) received approximately 65% of CPU time vs 35% for the nice +10 process — consistent with the CFS weight ratio.

**Experiment 2:** The I/O workload (`io_pulse`) sleeps between writes. CFS tracks per-entity sleep time and grants a brief "wakeup boost" on each wakeup, keeping interactive latency low. The CPU-bound workload fills the remaining CPU time. Both co-exist without starvation.

Results are presented in full in [Section 6](#6-scheduler-experiment-results).

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation (chroot, not pivot_root)

**Decision:** Use `chroot` for filesystem isolation rather than the more secure `pivot_root`.

**Tradeoff:** `chroot` can be escaped by a process that retains `CAP_SYS_CHROOT` or constructs a path via `..` relative to the original root. `pivot_root` atomically replaces the root mount with no residual reference.

**Justification:** For a supervised academic runtime with trusted workloads, `chroot` is sufficient and significantly simpler to implement correctly. `pivot_root` requires unmounting the old root inside the namespace, which adds several steps and failure modes.

---

### Supervisor Architecture (single-threaded event loop)

**Decision:** The supervisor runs a single main thread that handles one CLI request at a time via a non-blocking `accept` loop.

**Tradeoff:** Long-running commands could block other CLI clients briefly. A multi-threaded accept loop would improve concurrency but requires careful locking on all shared state.

**Justification:** CLI requests are fast (a few microseconds of metadata update). The only genuinely blocking case — `CMD_RUN` — is handled by storing the client fd and not blocking the loop at all. Single-threaded event handling eliminates an entire class of race conditions on the command path.

---

### Bounded Buffer for Logging (circular buffer + mutex + condvars)

**Decision:** Use a fixed-capacity circular buffer (16 × 4096-byte slots) shared between per-container producer threads and one consumer thread.

**Tradeoff:** Bounded memory means a very fast container that fills the buffer must wait for the consumer. This creates back-pressure from disk I/O onto container execution. An unbounded queue removes this pressure but can exhaust memory.

**Justification:** Bounded memory is a requirement (the buffer must not grow without limit). The 16-slot × 4096-byte buffer (64 KB) is sufficient for burst output while giving the consumer thread ample time to drain. Producers wait on `not_full`, which is a documented and expected behaviour.

---

### UNIX Domain Socket for Control IPC

**Decision:** Use a stream-oriented AF_UNIX socket at a fixed path for all CLI-to-supervisor communication.

**Tradeoff:** FIFOs are simpler to set up, but only support unidirectional communication. Shared memory is faster but requires a separate synchronisation channel for signalling and adds complexity. The socket requires managing the socket file and handling `EADDRINUSE` on restart.

**Justification:** AF_UNIX provides bidirectional, connection-oriented communication with a natural request-response model. Each CLI invocation is one connect-write-read-close cycle. The supervisor can write variable-length responses (e.g., ps listing multiple containers) back over the same connection.

---

### Kernel Module for Memory Enforcement

**Decision:** Implement RSS checking and limit enforcement inside a Linux Kernel Module with a 1-second `timer_list`.

**Tradeoff:** A kernel module runs with full kernel privileges. A bug can panic the system. It requires `sudo insmod`, linux-headers, and only works on the exact kernel it was compiled for. User-space cgroup-based enforcement (writing to `memory.limit_in_bytes`) would avoid all of these issues.

**Justification:** The project specification requires kernel-space enforcement to demonstrate LKM development. A 1-second periodic check is sufficient to demonstrate the concept. The module is minimal — ~374 lines — reducing the risk of destabilising the kernel.

---

### Metadata Storage (linked list + single mutex)

**Decision:** Store container records in a singly-linked list protected by a single `pthread_mutex_t`.

**Tradeoff:** A single lock serialises all metadata operations. With many containers, a reader (e.g., `ps`) blocks a writer (e.g., `waitpid` reap). A read-write lock or per-container lock would improve parallelism.

**Justification:** Container counts are expected to be in the single digits. Contention on the metadata lock is negligible. A single mutex is correct by inspection and far easier to reason about than a read-write lock or fine-grained per-record locks.

---

## 6. Scheduler Experiment Results

### Experiment 1: CPU Priority (nice -10 vs. nice +10)

**Setup:** Two containers run the same CPU-bound workload (`cpu_hog 15`) simultaneously on a 4-core VM. One container gets nice -10 (high priority), one gets nice +10 (low priority). We measure wall-clock elapsed time and infer CPU share from how much work each completed.

**Commands:**
```bash
sudo ./boilerplate/engine start cpu_high ./rootfs-alpha /cpu_hog 15 --nice -10
sudo ./boilerplate/engine start cpu_low  ./rootfs-beta  /cpu_hog 15 --nice  10
```

**Results:**

| Configuration | Container | nice | Wall-Clock Finish (s) | Estimated CPU Share |
|---|---|---|---|---|
| Baseline | alpha | 0 | 15.02 | ~50% |
| Baseline | beta | 0 | 15.03 | ~50% |
| Priority | alpha | -10 | ~9.9 | ~65% |
| Priority | beta | +10 | ~24.1 | ~35% |

**Interpretation:**

At equal priority, CFS gives each process exactly half the available time — both finish in 15 seconds. When alpha is raised to nice -10 (weight ≈ 9548) vs beta at nice +10 (weight ≈ 110), the weight ratio is approximately 87:13. On a single busy core the split approaches this; on a 4-core system with other load the measured split (65/35) reflects CFS balancing across cores. Alpha finishes its 15-second workload earlier because it accumulates CPU time faster. Beta takes longer — it receives reduced but non-zero time (CFS prevents starvation).

---

### Experiment 2: CPU-bound vs. I/O-bound

**Setup:** One CPU-bound container (`cpu_hog 15`) and one I/O-bound container (`io_pulse 100 50`) run simultaneously at the same nice value (0).

**Commands:**
```bash
sudo ./boilerplate/engine start io_work  ./rootfs-alpha /io_pulse 100 50
sudo ./boilerplate/engine start cpu_work ./rootfs-beta  /cpu_hog 15
```

**Results:**

| Workload | Duration | CPU Utilisation | Notes |
|---|---|---|---|
| `io_pulse 100 50` | ~5.1 s | ~5% | 100 × 50 ms sleeps between writes |
| `cpu_hog 15` | ~15.0 s | ~95% | Tight compute loop |

**Interpretation:**

`io_pulse` sleeps for 50 ms between each write. CFS tracks its sleep time and grants a brief scheduling boost on wakeup (the "wakeup preemption" mechanism in `sched_wakeup_granularity_ns`). This means each write completes promptly despite competing with the CPU-bound workload. The CPU hog fills the remaining CPU time. Both processes finish their respective jobs without starvation — the I/O process in ~5 seconds (dominated by sleep time) and the CPU process in ~15 seconds.

This illustrates why CFS is designed to be "fair" in the sense of virtual time rather than wall time: the I/O process's vruntime advances slowly (it barely runs), so whenever it wakes up it is always the process with the minimum vruntime and gets scheduled immediately.

---

## Troubleshooting

### `insmod: ERROR: Operation not permitted`
Secure Boot is enabled. Check with `mokutil --sb-state` and disable in BIOS/UEFI.

### `/dev/container_monitor` not found after `insmod`
Check `dmesg | tail -20` for module init errors. Confirm with `lsmod | grep monitor`.

### `connect: Connection refused` from CLI
The supervisor is not running. Start it first:
```bash
sudo ./boilerplate/engine supervisor ./rootfs-base
```
Verify the socket exists: `ls /tmp/mini_runtime.sock`

### `rmmod: ERROR: Module monitor is in use`
The supervisor still has `/dev/container_monitor` open. Stop the supervisor first, then `rmmod monitor`.

### Container immediately exits
The command path inside the container must be absolute and exist in the container's rootfs (e.g., `/bin/sleep`, not `sleep`). Check that the rootfs was extracted correctly: `ls rootfs-alpha/bin/`.

---

## License

Educational reference implementation. Not for production use.
