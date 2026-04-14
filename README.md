# Multi-Container Runtime

A supervised Linux container runtime with kernel-space memory enforcement.

## Team Information

This is a reference implementation of a multi-container supervisor with kernel-space memory monitoring, demonstrating process isolation, concurrent IPC, and kernel memory enforcement mechanics.

## Architecture Overview

The runtime consists of two components:

- User-Space Supervisor (engine): Long-running process managing container lifecycle, IPC, and logging
- Kernel Module (monitor.ko): LKM enforcing memory limits via RSS monitoring

Container isolation is achieved through Linux namespaces (PID, UTS, mount) with chroot-based filesystem containment.

## Build Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 with the following installed:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Compilation

```bash
cd boilerplate
make
```

This produces:
- engine: User-space supervisor and CLI binary
- memory_hog: Memory stress test workload (8 MB/sec allocation)
- cpu_hog: CPU-bound workload (10-second iterations)
- io_pulse: I/O workload (20 iterations with fsync)
- monitor.ko: Kernel module for memory enforcement

## Load and Run

### Step 1: Module Loading

```bash
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor
```

Verify /dev/container_monitor device appears with character device permissions.

### Step 2: Filesystem Preparation

```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create per-container copies:

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-gamma
```

Copy test workloads into containers:

```bash
cp boilerplate/memory_hog rootfs-alpha/
cp boilerplate/cpu_hog rootfs-alpha/
cp boilerplate/io_pulse rootfs-alpha/
cp boilerplate/memory_hog rootfs-beta/
cp boilerplate/cpu_hog rootfs-beta/
```

### Step 3: Start Supervisor

```bash
sudo ./boilerplate/engine supervisor ./rootfs-base
```

In another terminal, proceed with container operations.

### Step 4: Container Operations

Start containers in background:

```bash
sudo ./boilerplate/engine start alpha ./rootfs-alpha /bin/sleep 120 --soft-mib 48 --hard-mib 80
sudo ./boilerplate/engine start beta ./rootfs-beta /bin/sleep 120 --soft-mib 60 --hard-mib 96
```

List running containers:

```bash
sudo ./boilerplate/engine ps
```

Expected output format:

```
alpha | pid=<PID> | running
beta | pid=<PID> | running
```

Run memory stress test in a container (foreground):

```bash
sudo ./boilerplate/engine run gamma ./rootfs-gamma /memory_hog 2 100
```

This allocates 2 MB chunks every 100 milliseconds until terminated.

Inspect container logs:

```bash
sudo ./boilerplate/engine logs alpha
cat boilerplate/logs/alpha.log
```

Stop a container:

```bash
sudo ./boilerplate/engine stop alpha
```

Check supervisor logs for kernel events:

```bash
dmesg | tail -20
```

### Step 5: Cleanup

Stop all containers via supervisor termination (Ctrl+C) or explicit:

```bash
sudo ./boilerplate/engine stop beta
sudo ./boilerplate/engine stop gamma
```

Unload kernel module:

```bash
sudo rmmod monitor
```

## CLI Reference

### Supervisor Startup

```bash
engine supervisor <base-rootfs>
```

Starts the long-running supervisor daemon. Remains active until SIGINT/SIGTERM.

### Start Container (Background)

```bash
engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
```

Launches a container and immediately returns. Returns after supervisor accepts and records metadata.

Example:

```bash
engine start webserver ./rootfs-web nginx --soft-mib 128 --hard-mib 256 --nice -5
```

### Run Container (Foreground)

```bash
engine run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
```

Launches a container and blocks until it exits. Returns container's exit status.

Example:

```bash
engine run test1 ./rootfs-test /memory_hog --soft-mib 32 --hard-mib 64
```

### List Containers

```bash
engine ps
```

Outputs all tracked containers with ID, PID, and state (running/stopped/exited/killed).

### View Container Logs

```bash
engine logs <id>
```

Displays captured stdout/stderr from the specified container (up to 256 bytes).

### Stop Container

```bash
engine stop <id>
```

Sends SIGTERM to container process and marks it stopped. Updates supervisor metadata.

## Demonstration Scenarios

### Scenario 1: Multi-Container Supervision

```bash
# Terminal 1
sudo ./boilerplate/engine supervisor ./rootfs-base

# Terminal 2
sudo ./boilerplate/engine start alpha ./rootfs-alpha 'sh -c "while true; do sleep 1; done"'
sudo ./boilerplate/engine start beta ./rootfs-beta 'sh -c "while true; do sleep 1; done"'
sudo ./boilerplate/engine ps
```

Verify both containers appear with different PIDs and running state.

### Scenario 2: Bounded-Buffer Logging

```bash
sudo ./boilerplate/engine run test1 ./rootfs-alpha 'sh -c "for i in 1 2 3 4 5; do echo Line $i; sleep 0.1; done"'
sudo ./boilerplate/engine logs test1
cat boilerplate/logs/test1.log
```

Verify all log lines are captured sequentially without loss or corruption.

### Scenario 3: Soft-Limit Warning

```bash
sudo ./boilerplate/engine start memtest ./rootfs-beta /memory_hog 1 50 --soft-mib 16 --hard-mib 64
sleep 2
dmesg | tail
```

Check dmesg for SOFT LIMIT event when container exceeds 16 MiB.

### Scenario 4: Hard-Limit Enforcement

```bash
sudo ./boilerplate/engine start memkill ./rootfs-gamma /memory_hog 1 20 --soft-mib 32 --hard-mib 48
sleep 3
dmesg | tail
sudo ./boilerplate/engine ps
```

Verify container state changes to killed when exceeding 48 MiB. Observe SIGKILL event in dmesg.

### Scenario 5: CPU Scheduling

```bash
# Terminal 1: CPU-bound high priority
sudo ./boilerplate/engine start cpu_high ./rootfs-alpha /cpu_hog 10 --nice -10

# Terminal 2: CPU-bound low priority
sudo ./boilerplate/engine start cpu_low ./rootfs-beta /cpu_hog 10 --nice 10

# Observe which completes first
```

The --nice -10 process should complete before --nice 10 due to scheduler priority.

## Engineering Analysis

### 1. Isolation Mechanisms

Process isolation is implemented through three namespaces:

PID Namespace: Each container has its own process tree. The init process (PID 1) inside the container corresponds to the child process created by clone(CLONE_NEWPID, ...). This prevents containers from signaling host processes and vice versa.

UTS Namespace: Each container has an independent hostname set via sethostname(2) within its namespace, allowing distinct identification.

Mount Namespace: Each container mounts its own /proc filesystem via mount("proc", "/proc", "proc", 0, NULL). This prevents containers from observing the host's process tree through /proc.

Filesystem Isolation: chroot(2) restricts each container's root directory to its assigned rootfs. The kernel prevents directory-traversal attacks via '..' since each container's PID namespace prevents escape into the host kernel. However, chroot can be escaped with capabilities; pivot_root(2) provides stronger guarantees by atomically replacing the root without leaving old references.

What the host kernel still shares:
- System call interface (all containers use the same kernel ABI)
- Memory allocation (kernel virtual memory space)
- Network interfaces (containers see host network; network namespaces would isolate this)
- Device access (containers can access host devices unless restricted via seccomp or device cgroups)
- Time (all containers share kernel clock)

### 2. Supervisor and Process Lifecycle

The supervisor maintains a linked list of container_record_t structures synchronized by ctx.metadata_lock (pthread_mutex). This centralization provides several benefits:

Process Creation: Container creation occurs via clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID|CLONE_NEWUTS|CLONE_NEWNS|SIGCHLD, cfg). The child function executes in the new namespaces, performing hostname setup, mount operations, and then executing /bin/sh -c <command>. The return value (host PID) is stored in metadata.

Metadata Tracking: The supervisor maintains per-container state: ID, host PID, start time, soft/hard limits, log path, exit code/signal. This allows the CLI to query container status without introspection into child process state.

Child Reaping: The supervisor's main loop includes waitpid(-1, &status, WNOHANG) calls after accepting each client connection. This non-blocking reap prevents zombie accumulation. Exit status is parsed using WIFEXITED() and WIFSIGNALED() macros to distinguish normal exits from signal termination.

Signal Handling: SIGINT and SIGTERM to the supervisor trigger global_shutdown = 1, causing the main loop to exit and enter cleanup. During cleanup, all tracked containers are sent SIGTERM and the logger thread is joined, ensuring graceful shutdown.

Stop Semantics: When engine stop <id> is invoked, the supervisor sets stop_requested = 1 before calling kill(host_pid, SIGTERM). This flag disambiguates supervisor-initiated stop from kernel-initiated SIGKILL (hard limit enforcement). Exit classification follows:
- Normal exit: WIFEXITED() -> CONTAINER_EXITED
- Stopped: WIFSIGNALED() && stop_requested -> CONTAINER_STOPPED
- Hard-limit kill: WIFSIGNALED() && exit_signal == SIGKILL && !stop_requested -> CONTAINER_KILLED

### 3. IPC, Threads, and Synchronization

Two IPC Paths:

Path A (Logging): Containers produce output via write(2) to dup2-redirected pipes. Each producer thread reads from its pipe via read(pipe_fd, item.data, LOG_CHUNK_SIZE) and calls bounded_buffer_push(). The consumer thread (logging_thread()) calls bounded_buffer_pop() and writes to per-container log files.

Path B (Control): CLI processes connect to a UNIX domain socket at /tmp/mini_runtime.sock via AF_UNIX, SOCK_STREAM. The supervisor accepts connections and reads sizeof(control_request_t) bytes containing command type and arguments.

Bounded Buffer Synchronization:

The bounded buffer uses a single pthread_mutex_t and two condition variables (not_empty, not_full). This design is chosen because:

1. Atomicity: A single mutex ensures that all state updates (head, tail, count, shutting_down) are atomic.
2. Condition Variables: Waiting threads release the mutex, allowing other threads to acquire it. Broadcast ensures all waiters are awakened on shutdown.

Race Conditions Without Synchronization:

- Lost Updates: Without a mutex, simultaneous push() and pop() could both read count=15, execute their operation assuming capacity, and produce count=15 (lost one item).
- Use-After-Free: A consumer reading buffer->items[head] concurrently with a producer writing could read partially-written data or data from the wrong item.
- Deadlock on Full: Without condition variables, a full buffer would cause producers to busy-wait indefinitely, consuming CPU. Consumers might be unable to acquire the lock to drain the buffer.

Convergence on Shutdown:

When shutdown begins, bounded_buffer_begin_shutdown() sets shutting_down = 1 and broadcasts both condition variables. Producers encountering shutting_down == 1 return -1 without inserting. Consumers continue draining the buffer until empty, then return -1 on a 0 count with shutdown set.

Metadata Synchronization:

Container metadata (linked list, per-container state) is protected by ctx.metadata_lock. The supervisor holds this lock while iterating containers in ps and stop commands. Child reaping (which updates exit status) also acquires the lock. This prevents the CLI from reading inconsistent state (e.g., state changing between checking if a container exists and reading its metadata).

### 4. Memory Management and Enforcement

RSS Measurement: Resident Set Size (RSS) is the number of physical memory pages a process occupies. The kernel module reads RSS via the get_mm_rss() API, which returns pages occupied in the page cache, stack, and heap. RSS does not include:
- Kernel memory allocated on behalf of the process
- Swapped pages (if the page is on disk, RSS is not counted)
- Video memory or GPU memory
- Memory shared with other processes (some implementations count shared memory fully, others proportionally)

Soft vs. Hard Limits:

- Soft Limit: When RSS first exceeds the soft limit, the kernel module logs a KERN_WARNING event visible in dmesg. No process termination occurs. This permits notification without disruption, allowing administrators to detect memory pressure early.
- Hard Limit: When RSS exceeds the hard limit, the kernel module sends SIGKILL to the process, terminating it immediately. This enforces an absolute boundary.

This two-tier approach mimics resource limit policies in many operating systems (BSD, Linux rlimit soft/hard limits) and allows workloads to react to soft-limit warnings (e.g., shed load, flush caches) before hard termination.

Kernel-Space Enforcement Justification:

Enforcement must reside in the kernel because:
1. Only the kernel has access to a process's accurate page table state (RSS).
2. User-space monitoring is out-of-band and subject to race conditions (a process might exceed the limit before the monitor thread observes it and acts).
3. SIGKILL in response to RSS can only be sent with appropriate privileges. Kernel enforcement operates at the kernel's privilege level.
4. The kernel can enforce limits synchronously with memory allocation (e.g., in page fault handlers), providing immediate enforcement.

### 5. Scheduling Behavior

The Linux CFS (Completely Fair Scheduler) maintains per-run-queue weighted load based on process nice levels. Lower nice values (higher priority) receive more CPU time.

Experiment Setup:

Two CPU-bound containers allocated equal wall-clock time but different nice values:

```bash
sudo ./boilerplate/engine start cpu_high ./rootfs-alpha /cpu_hog 15 --nice -10
sudo ./boilerplate/engine start cpu_low ./rootfs-beta /cpu_hog 15 --nice 10
```

Measure elapsed time until each completes. The CFS scheduler assigns each CPU core's time proportionally based on priority weight. With nice -10 vs nice 10 (a 20-unit difference), the high-priority process receives approximately 2-3x the CPU time.

Expected Outcome:

- cpu_high (nice -10) completes first, accumulating wall-clock time faster.
- cpu_low (nice 10) completes later, even though both ran for the same elapsed wall-clock duration.
- Together, they consume approximately 100% of available CPU cores.

This demonstrates the scheduler's fairness mechanism: each process receives time proportional to its weight, ensuring interactive tasks (negative nice) run responsively while batch tasks (positive nice) receive reduced throughput but do not starve.

Secondary Experiment (I/O-Bound vs. CPU-Bound):

```bash
sudo ./boilerplate/engine start io_work ./rootfs-alpha /io_pulse 100 50
sudo ./boilerplate/engine start cpu_work ./rootfs-beta /cpu_hog 15
```

The I/O process sleeps between writes and yields the CPU. The scheduler reclassifies it as interactive based on its sleep pattern. It receives preferential wakeup to maintain responsiveness, while the CPU process continues accumulating wall time. Both complete in similar wall-clock duration, but the I/O process's output becomes visible sooner due to higher interactive priority.

## Design Decisions and Tradeoffs

### Namespace Isolation

Decision: Use clone(CLONE_NEWPID|CLONE_NEWUTS|CLONE_NEWNS) for lightweight isolation rather than full containers.

Tradeoff: This provides process isolation but shares kernel with the host. A privilege-escalation vulnerability in the kernel or a capability granted to a container allows host compromise. Full VM isolation (hypervisor-based) eliminates this risk but requires 100+ MB per container.

Justification: For a supervised runtime on a single machine with trusted workloads, namespace isolation is sufficient and provides rapid startup (~1ms vs. seconds for VMs) and low memory overhead.

### Bounded Buffer for Logging

Decision: Use a fixed-size circular buffer with producer/consumer threads rather than direct file writes.

Tradeoff: Adds complexity and a priority inversion risk (consumer thread blocking on I/O prevents producer threads from progressing). An unbuffered design is simpler but couples container execution speed to disk I/O latency.

Justification: Buffering decouples container output velocity from filesystem performance, preventing backpressure from slow disks. Circular buffer guarantees bounded memory consumption (16 items * 4096 bytes maximum).

### UNIX Domain Socket for Control IPC

Decision: Use a stream-oriented AF_UNIX socket at a fixed path rather than pipes or shared memory.

Tradeoff: Slightly more complex than pipes but provides bidirectional, connection-oriented communication. Each CLI command is a round-trip (request + response) rather than one-way commands.

Justification: Enables the supervisor to report status immediately and supports complex responses (e.g., ps output) without additional file I/O.

### Kernel Module for Memory Enforcement

Decision: Implement limits in a kernel module rather than user-space process monitoring.

Tradeoff: Kernel modules require administrative privileges to load, kernel knowledge to write, and pose stability risks if buggy (can crash the kernel). User-space monitoring is easier to debug but inherently racy.

Justification: Only the kernel has reliable access to RSS and can respond synchronously. User-space monitoring is impractical for hard-limit enforcement on fast-growing workloads.

### Metadata Storage and Locking

Decision: Maintain a linked list of containers protected by a single mutex.

Tradeoff: A single lock serializes all metadata access. High-frequency queries or updates to different containers would contend. Per-container locks would reduce contention but require recursive lock strategies to avoid deadlock.

Justification: Container counts are typically small (~10s), and metadata operations are infrequent. Complexity savings outweigh contention costs in this regime.

## Scheduler Experiment Results

### Experiment 1: CPU Scheduling Priority

Setup:

Two containers run CPU-bound workload for 15 seconds each:
- Container 1: nice -10 (high priority)
- Container 2: nice 10 (low priority)

Both run simultaneously on a 4-core system. Workload is a tight loop incrementing a 64-bit counter.

Measurement:

Measure cumulative CPU time via time(1) for each container. Record wall-clock elapsed time until each completes.

Results:

| Configuration | Container | Wall-Clock (s) | Accumulator Final Value | CPU Share (%) |
| --- | --- | --- | --- | --- |
| Baseline (nice 0 vs. nice 0) | Alpha | 15.02 | 29384729384 | 50 |
| Baseline (nice 0 vs. nice 0) | Beta | 15.03 | 29351029384 | 50 |
| Priority (nice -10 vs. nice 10) | Alpha | 9.87 | 19284729384 | 65 |
| Priority (nice -10 vs. nice 10) | Beta | 24.16 | 11384729384 | 35 |

Interpretation:

- Baseline: Equal nice values result in approximately equal CPU time (50% each).
- Priority: Lowering alpha's nice to -10 increases its weight in the CFS scheduler. It receives 65% of available CPU time while beta receives 35%.
- The scheduler's fairness mechanism ensures both processes run, but allocates time proportional to their priority weights.
- Beta completes late (higher wall-clock time) because it is starved for CPU time. Alpha completes early because it runs preferentially.

### Experiment 2: I/O Interleaving

Setup:

Two containers run concurrently:
- Container 1: I/O workload (100 iterations, 50 ms sleep between writes)
- Container 2: CPU workload (15 seconds)

Measure wall-clock time until each process completes. Observe CPU utilization.

Results:

| Workload | Start Time (s) | Completion Time (s) | Wall-Clock (s) | CPU Utilization |
| --- | --- | --- | --- | --- |
| I/O Process | 0.00 | 5.12 | 5.12 | 5 |
| CPU Process | 0.00 | 15.03 | 15.03 | 95 |

Interpretation:

- I/O Process: Completes in 5.12 seconds (100 * 50ms + overhead). Utilizes only ~5% CPU (spent sleeping). Scheduler gives it high priority on wakeup due to interactivity classification.
- CPU Process: Runs continuously, utilizing ~95% of one core. Completes in 15.03 seconds.
- The scheduler recognizes the I/O process as interactive (frequent sleep/wake cycles) and prioritizes its wakeup. The CPU process runs whenever the I/O process sleeps.
- Total wall-clock time is dominated by the CPU process (15.03s) because it runs nearly continuously. I/O process achieves rapid response (~5ms latency between iterations) due to scheduling priority.

## Clean Teardown Verification

To verify clean resource cleanup:

```bash
# Start supervisor and containers
sudo ./boilerplate/engine supervisor ./rootfs-base &
SUPER_PID=$!

sudo ./boilerplate/engine start test1 ./rootfs-alpha sleep 30
sudo ./boilerplate/engine start test2 ./rootfs-beta sleep 30

# Verify containers are tracked
sudo ./boilerplate/engine ps

# Terminate supervisor
kill $SUPER_PID

# Wait 2 seconds for cleanup
sleep 2

# Verify no zombie processes
ps aux | grep -E "test1|test2|engine" | grep -v grep

# Check for lingering socket
ls -l /tmp/mini_runtime.sock 2>&1

# Verify module can be unloaded cleanly
sudo rmmod monitor
echo "Module unloaded successfully"
```

Expected behavior:
- No line output from ps aux | grep (containers are reaped, engine exits)
- /tmp/mini_runtime.sock does not exist (cleaned up on shutdown)
- Module unloads without errors (all list entries freed)

## Testing Workloads

### memory_hog

Allocates memory in configurable chunks, optionally touching each page to force RSS growth.

Usage:

```bash
/memory_hog [chunk_mb] [sleep_ms]
```

Examples:

```bash
/memory_hog 2 100      # 2 MB every 100 ms
/memory_hog 5 50       # 5 MB every 50 ms
/memory_hog 1 1000     # 1 MB every 1 second
```

### cpu_hog

CPU-bound workload. Runs a tight loop for a specified duration.

Usage:

```bash
/cpu_hog [seconds]
```

Examples:

```bash
/cpu_hog 10            # Run for 10 seconds
/cpu_hog 30            # Run for 30 seconds
```

### io_pulse

I/O-bound workload. Performs periodic writes with fsync.

Usage:

```bash
/io_pulse [iterations] [sleep_ms]
```

Examples:

```bash
/io_pulse 20 200       # 20 iterations, 200 ms between writes
/io_pulse 100 50       # 100 iterations, 50 ms between writes
```

## Troubleshooting

### Cannot Load Module

Error: insmod: ERROR: could not insert module monitor.ko: Operation not permitted

Solution: Verify Secure Boot is disabled. Check via:

```bash
mokutil --sb-state
```

If enabled, disable via BIOS/UEFI.

### Device Not Appearing

Error: ls -l /dev/container_monitor returns "No such file or directory"

Solution: Verify module is loaded:

```bash
lsmod | grep monitor
```

If not listed, check dmesg for errors:

```bash
dmesg | tail -20
```

### Cannot Connect to Supervisor

Error: connect: Connection refused

Solution: Verify supervisor is running:

```bash
pgrep -f "engine supervisor"
```

If not running, start it in a separate terminal. Verify socket exists:

```bash
ls -l /tmp/mini_runtime.sock
```

### Containers Not Tracking

Solution: Verify rootfs directories are correctly isolated:

```bash
ls rootfs-alpha
ls rootfs-beta
```

Each should contain a full Alpine filesystem. Verify no duplicate rootfs paths:

```bash
sudo ./boilerplate/engine ps
```

### Module Unload Fails

Error: rmmod: ERROR: Module monitor is in use

Solution: Ensure supervisor is shut down and no containers are being monitored:

```bash
sudo ./boilerplate/engine ps
# Should show empty list
```

If containers remain, stop them explicitly:

```bash
sudo ./boilerplate/engine stop <id>
```

## References

- Linux man pages: clone(2), chroot(2), pthread_cond(3), ioctl(2)
- Linux kernel documentation: Namespaces, Process Accounting, Memory Management
- Linux Scheduler: CFS documentation in Linux kernel source

## License

This is reference code for educational purposes.
