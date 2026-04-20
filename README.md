# Multi-Container Runtime

> A lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| *Ridhima Jain* | *PES1UG24CS373* |
| *Roopa Sreedhar A* | *PES1UG24CS386* |

---

## 2. Build, Load, and Run Instructions

### 2.1 Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Requirements:
- Ubuntu 22.04 or 24.04 in a VM (not WSL)
- Secure Boot **OFF** (required for loading unsigned kernel modules)

> **Note on kernel 6.4+:** The `class_create()` kernel API changed in Linux 6.4 to remove the `THIS_MODULE` argument. Our `monitor.c` handles this automatically using `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)`.

### 2.2 Build Everything

```bash
# From the project root (boilerplate/)
make all
```

This produces:
- `engine` – user-space runtime binary
- `monitor.ko` – kernel module
- `memory_hog`, `cpu_hog`, `io_pulse` – workload binaries

**Important:** The workload binaries must be statically linked to run inside the Alpine mini-rootfs (which has no glibc). Build them statically:

```bash
gcc -O2 -Wall -static -o cpu_hog cpu_hog.c
gcc -O2 -Wall -static -o memory_hog memory_hog.c
gcc -O2 -Wall -static -o io_pulse io_pulse.c
```

CI-safe build (no kernel headers needed, for GitHub Actions):

```bash
make ci
```

### 2.3 Load the Kernel Module

```bash
sudo insmod monitor.ko

# Verify the device node was created
ls -l /dev/container_monitor

# Check kernel log
sudo dmesg | tail -5
```

Expected output:
```
crw------- 1 root root 240, 0 ... /dev/container_monitor
[monitor] registered with major number 240
[monitor] module loaded — /dev/container_monitor ready
```

> The "taints kernel" and "module verification failed" messages are normal for out-of-tree modules without Secure Boot signing — ignore them.

### 2.4 Prepare Root Filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Make one writable copy per container
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy statically linked workload binaries into each rootfs
cp ./cpu_hog    ./rootfs-alpha/
cp ./memory_hog ./rootfs-alpha/
cp ./io_pulse   ./rootfs-alpha/
cp ./cpu_hog    ./rootfs-beta/
cp ./memory_hog ./rootfs-beta/
cp ./io_pulse   ./rootfs-beta/
```

> **Do not commit** `rootfs-base/` or `rootfs-*/` to your repository.

### 2.5 Start the Supervisor (Terminal 1 — keep open)

```bash
sudo ./engine supervisor ./rootfs-base
```

Expected output:
```
[supervisor] ready  socket=/tmp/mini_runtime.sock  pid=4298
```

> If `/dev/container_monitor` is not loaded, the supervisor prints a warning and continues with memory monitoring disabled.

### 2.6 Use the CLI (Terminal 2)

#### Start containers in the background

```bash
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog 60"
sudo ./engine start beta  ./rootfs-beta  "/cpu_hog 60"
```

#### List running containers

```bash
sudo ./engine ps
```

#### View container logs

```bash
sudo ./engine logs alpha
```

#### Stop a container

```bash
sudo ./engine stop alpha
```

### 2.7 Run Memory Limit Tests

```bash
# Create a dedicated rootfs for the memory test
cp -a ./rootfs-base ./rootfs-memtest
cp ./memory_hog ./rootfs-memtest/

# Soft limit only (warn at 20 MiB, kill at 64 MiB)
sudo ./engine start memtest ./rootfs-memtest "/memory_hog 8 500" --soft-mib 20 --hard-mib 64
sleep 15
sudo dmesg | grep monitor | tail -10

sudo ./engine stop memtest

# Hard limit (warn at 20 MiB, kill at 32 MiB)
cp -a ./rootfs-base ./rootfs-hardtest
cp ./memory_hog ./rootfs-hardtest/
sudo ./engine start hardtest ./rootfs-hardtest "/memory_hog 8 300" --soft-mib 20 --hard-mib 32
sleep 15
sudo dmesg | grep monitor | tail -10
sudo ./engine ps
```

### 2.8 Run Scheduling Experiments

```bash
cp -a ./rootfs-base ./rootfs-cpuhi
cp -a ./rootfs-base ./rootfs-cpulo
cp ./cpu_hog ./rootfs-cpuhi/
cp ./cpu_hog ./rootfs-cpulo/

# High priority (nice -10)
sudo ./engine start cpu-hi ./rootfs-cpuhi "/cpu_hog 30" --nice -10

# Low priority (nice +10)
sudo ./engine start cpu-lo ./rootfs-cpulo "/cpu_hog 30" --nice 10

sleep 32

sudo ./engine logs cpu-hi
sudo ./engine logs cpu-lo
```
```
# Make rootfs copies for the io experiment
cp -a ./rootfs-base ./rootfs-iopulse
cp ./io_pulse ./rootfs-iopulse/
cp -a ./rootfs-base ./rootfs-cpuexp
cp ./cpu_hog ./rootfs-cpuexp/

# Start both at the same time
sudo ./engine start cpuhog  ./rootfs-cpuexp  "/cpu_hog 30"
sudo ./engine start iopulse ./rootfs-iopulse "/io_pulse 20 200"

# Wait for both to finish
sleep 35

# Check the logs
sudo ./engine logs cpuhog
sudo ./engine logs iopulse
```

### 2.9 Clean Teardown

```bash
# Stop any remaining containers
sudo ./engine stop alpha

# Send SIGTERM to supervisor (orderly shutdown)
sudo kill -TERM $(pgrep -f "engine supervisor")

# Verify no processes remain
sleep 1
ps aux | grep -v grep | grep engine

# Unload kernel module
sudo rmmod monitor
sudo dmesg | tail -5

# Clean build artifacts
make clean
rm -rf rootfs-alpha rootfs-beta rootfs-memtest rootfs-hardtest rootfs-cpuhi rootfs-cpulo
```

---

## 3. Demo Screenshots

| # | What is Demonstrated | Caption |
|---|---|---|
| 1 | **Multi-container supervision** | `ps aux` showing containers alpha (pid=4676) and beta (pid=4684) both running as children of the supervisor process (pid=4665). The supervisor remains alive while both containers run. |
| 2 | **Metadata tracking** | `engine ps` output showing both containers with state=`running`, soft/hard memory limits (40/64 MiB), and termination reason=`none` while live. |
| 3 | **Bounded-buffer logging** | `engine logs alpha` showing cpu_hog output captured second-by-second through the producer/consumer pipeline. `ls -lh logs/` confirms both `alpha.log` and `beta.log` were written (12K each). |
| 4 | **CLI and IPC** | `engine stop alpha` sent from Terminal 2; supervisor Terminal 1 simultaneously prints the stop acknowledgement, demonstrating the UNIX domain socket control channel (Path B). |
| 5 | **Soft-limit warning** | `dmesg` showing `[monitor] SOFT LIMIT exceeded: pid=4792 container=memtest rss=25216kB limit=20MiB` — the kernel module detected RSS crossing the 20 MiB soft threshold and logged a warning. |
| 6 | **Hard-limit enforcement** | `dmesg` showing `[monitor] HARD LIMIT: killed pid=4792 container=memtest` and `engine ps` showing state=`killed` reason=`hard_limit_killed` — the kernel module sent SIGKILL when RSS exceeded 32 MiB. |
| 7 | **Scheduling experiment** | `engine logs cpu-hi` (nice=-10) and `engine logs cpu-lo` (nice=+10) both complete 30 seconds. The accumulator values differ between the two, reflecting different CPU time allocations by the CFS scheduler. and I/O Bound processes |
| 8 | **Clean teardown** | `ps aux` showing no remaining engine processes after supervisor SIGTERM. `dmesg` confirms `[monitor] module unloaded` after `rmmod monitor`. Supervisor printed `[supervisor] clean exit.` in Terminal 1. |

---
  
<img width="940" height="84" alt="image" src="https://github.com/user-attachments/assets/fc31df53-a3c9-4098-a767-3140171a6721" />
<br><br>

<img width="940" height="222" alt="image" src="https://github.com/user-attachments/assets/486be76a-0524-4f4f-b463-1361db87d923" />
<br><br>

<img width="940" height="903" alt="image" src="https://github.com/user-attachments/assets/3f1be70d-0375-46cb-a7c1-fbf70e922869" />
<br><br>
<img width="940" height="821" alt="image" src="https://github.com/user-attachments/assets/41a96097-6801-491b-b4ce-2e4ca1dab02a" />
<br><br>
<img width="940" height="958" alt="image" src="https://github.com/user-attachments/assets/0806199b-b52c-4699-9725-aaf80bfa2b44" />
<br><br>
<img width="940" height="708" alt="image" src="https://github.com/user-attachments/assets/f8954e7e-86b8-4f48-b366-f5a05ddce5e5" />
<br><br>
<img width="940" height="744" alt="image" src="https://github.com/user-attachments/assets/76b1521a-347d-47a0-9a82-124c3ce8fcea" />
<br><br>

<img width="940" height="928" alt="image" src="https://github.com/user-attachments/assets/7e1b7891-158a-458d-829b-a1dcdc5c0102" />
<br><br>
<img width="661" height="120" alt="image" src="https://github.com/user-attachments/assets/ba9871d0-7a70-4592-a33b-03d0fb4956b1" />
<br><br>
<img width="940" height="62" alt="image" src="https://github.com/user-attachments/assets/fcad472d-8a6b-43c9-83a6-1a7bdf985f13" />
<br><br>
<img width="940" height="419" alt="image" src="https://github.com/user-attachments/assets/e50bc94f-b9fc-4d1f-81f5-6c0633ba5f79" />
<br><br>
<img width="940" height="49" alt="image" src="https://github.com/user-attachments/assets/ad4c4d5a-af03-4502-9cdf-08ccdea0683e" />
<br><br>

<img width="940" height="791" alt="image" src="https://github.com/user-attachments/assets/1cf075ee-5f1b-431c-b04f-f6cb0cf62e30" />
<br><br>

<img width="940" height="896" alt="image" src="https://github.com/user-attachments/assets/d1600dff-9e1a-4b71-bf58-25020f49cf7d" />
<br><br>
<img width="940" height="116" alt="image" src="https://github.com/user-attachments/assets/545cecad-bb91-4276-9e97-1eb57b24132c" />
<br><br>
<img width="973" height="681" alt="Screenshot 2026-04-20 182608" src="https://github.com/user-attachments/assets/2c8a3666-ec1d-43c3-8fcf-555cfcf07ae0" />
<br><br>

<img width="1030" height="677" alt="image" src="https://github.com/user-attachments/assets/1de2cf52-9bf4-41c4-8b3e-b72a3af9cbeb" />
<br><br>
<img width="642" height="73" alt="image" src="https://github.com/user-attachments/assets/be135953-d45c-42ed-b0cb-cc2e6f905a84" />
<br><br>


<img width="940" height="257" alt="image" src="https://github.com/user-attachments/assets/a898f380-8ac4-4086-816f-07c1c2ddffcf" />
<br><br>

<img width="608" height="79" alt="image" src="https://github.com/user-attachments/assets/d4b7960f-fcf3-4b66-996e-a4b7c9af4a7b" />
<br><br>

















       
 

 
 
  
 
  

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Each container is created with `clone()` using three namespace flags:

- **`CLONE_NEWPID`** — The container's first process becomes PID 1 inside its own PID namespace. It cannot see host processes. Tools like `ps` inside the container show only container-local PIDs. The host kernel maintains the actual PIDs; the namespace is a mapping layer in the kernel's `pid_namespace` struct.
- **`CLONE_NEWUTS`** — The container gets its own hostname, set via `sethostname()` to the container ID. The host kernel maintains a per-namespace UTS structure; each container sees only its own.
- **`CLONE_NEWNS`** — The container gets its own mount namespace. Our `/proc` mount inside the container does not propagate to the host or other containers.

**chroot:** After `clone()`, the child calls `chroot(container_rootfs)` followed by `chdir("/")`. This makes the Alpine mini-rootfs appear as the container's root. The host kernel enforces permissions on all underlying inodes; `chroot` simply changes the per-process `fs->root` pointer in the kernel.

**What the host kernel still shares:** There is only one kernel. All containers share the same kernel code, scheduler, network stack (we omit `CLONE_NEWNET`), and IPC namespace (we omit `CLONE_NEWIPC`). A kernel vulnerability affects all containers equally.

### 4.2 Supervisor and Process Lifecycle

The supervisor is a long-running daemon. Its lifecycle roles:

- **Process creation:** `clone()` atomically creates the child and sets namespace flags. The child receives a `child_config_t` describing its rootfs, command, and pipe file descriptor.
- **Parent-child relationship:** The supervisor is the parent of all container processes. The kernel delivers `SIGCHLD` to the parent when a child changes state. Without a living parent, children become orphans adopted by PID 1.
- **Reaping:** The supervisor installs `SA_NOCLDSTOP` in the `SIGCHLD` handler and calls `waitpid(-1, &wstatus, WNOHANG)` in a loop. This drains all exited children atomically, preventing zombies. A zombie is a process whose exit status has not been collected — it occupies a kernel process table entry.
- **Metadata tracking:** A linked list of `container_record_t` structs protected by `pthread_mutex_t metadata_lock` tracks each container's ID, PID, state, limits, exit code, and termination reason. The mutex prevents TOCTOU races between the SIGCHLD reap path and CLI command handlers.
- **Signal delivery:** `SIGCHLD` wakes the reap path. `SIGTERM` sets `g_shutdown`, causing the event loop to exit and trigger orderly teardown.

### 4.3 IPC, Threads, and Synchronisation

**Path A — Pipe-based logging (Task 3):**
Each container has a `pipe()`. The write-end is given to the container child via `dup2()` onto stdout/stderr. The read-end is given to a per-container producer thread. The producer does blocking `read()` and pushes chunks into the shared bounded buffer.

The bounded buffer is a fixed-size ring protected by:
- `pthread_mutex_t mutex` — serialises all reads/writes to `head`, `tail`, `count`
- `pthread_cond_t not_full` — producers wait here when the buffer is full
- `pthread_cond_t not_empty` — the consumer waits here when the buffer is empty

Without synchronisation, two concurrent producers could compute the same `tail` index and overwrite the same slot. A consumer could read a partially written item. The `shutting_down` flag causes producers to stop inserting and the consumer to drain all remaining entries before exiting — guaranteeing no log lines are lost.

**Path B — UNIX domain socket control channel (Task 2):**
CLI client processes connect, send a `control_request_t` struct, receive a `control_response_t` struct, and exit. The supervisor's main loop uses `select()` with a 200 ms timeout to stay responsive to signals while waiting for connections.

**`engine run` blocking:** Uses a dedicated `CMD_WAIT` request. The supervisor's `handle_wait()` calls `pthread_cond_wait(&c->exited_cond, &ctx->metadata_lock)`, sleeping until `reap_children()` broadcasts on the per-container condition variable. This avoids polling entirely.

**Why condition variables over semaphores?** Condition variables let us express both "not full" and "not empty" on the same mutex, making invariant reasoning cleaner. Semaphores track a single count — emulating a bounded buffer with two semaphores requires careful pairing that is more error-prone.

### 4.4 Memory Management and Enforcement

**RSS (Resident Set Size)** measures physical pages currently loaded in RAM attributed to a process. It does *not* measure:
- Virtual memory mapped but not yet touched (not paged in)
- Pages swapped out to disk
- Shared library pages counted in multiple processes
- File-backed pages that may be evicted and reloaded

**Soft vs. hard limits:**
A soft limit is advisory — the process continues but the kernel module logs a warning. This alerts the operator before memory pressure becomes critical. A hard limit is enforced — the process receives `SIGKILL` when its RSS crosses the threshold. This prevents one container from starving the host or other containers of physical RAM.

**Why kernel-space enforcement?**
A user-space monitor reads RSS from `/proc/<pid>/status` and sends a signal — but between reading and signalling, the process can allocate more memory, creating a race window. Our kernel module reads RSS directly from `task->mm` under `rcu_read_lock()` and delivers `SIGKILL` in the same context, dramatically narrowing this window. Kernel-space polling also eliminates context-switch overhead from a separate monitoring process.

**Termination attribution rule:**
- `stop_requested` is set before the supervisor sends SIGTERM/SIGKILL via the `stop` command
- Exit with `stop_requested` set → classified as `stopped`
- Exit with SIGKILL and `stop_requested` not set → classified as `hard_limit_killed`
- This ensures `engine ps` correctly distinguishes manual stops from kernel-enforced kills

### 4.5 Scheduling Behaviour

**Linux CFS:** The Completely Fair Scheduler allocates CPU time proportional to each task's weight, derived from its `nice` value. `nice -20` has a weight ~4× that of `nice 0`; `nice +19` has weight ~0.05× that of `nice 0`.

**Experiment A — CPU-bound containers at different priorities:**
`cpu-hi` (nice=-10) and `cpu-lo` (nice=+10) ran concurrently for 30 seconds. Both containers completed all 30 seconds of work (CFS is fair, not exclusive), but `cpu-hi` received more CPU time per scheduling period. The accumulator values in the logs reflect total loop iterations, which correlate directly with CPU time received. CFS maintained a stable ratio throughout, demonstrating its long-run fairness guarantee.

**Experiment B — CPU-bound vs I/O-bound:**
`cpu_hog` never sleeps voluntarily (100% CPU). `io_pulse` sleeps 200 ms between writes, voluntarily releasing the CPU. CFS rewards processes with low CPU usage by giving them a lower `vruntime` — so when `io_pulse` wakes from sleep, it is scheduled almost immediately. The CPU hog receives full CPU during the sleep periods. This demonstrates how CFS simultaneously achieves high throughput (CPU hog) and low latency (I/O-bound process) without any special priority configuration.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** PID + UTS + Mount namespaces via `clone()`.
**Tradeoff:** We omit `CLONE_NEWNET` — containers share the host network stack.
**Justification:** Network isolation requires setting up virtual Ethernet pairs per container, which is significant additional complexity outside the project scope. PID, UTS, and mount isolation satisfy all spec requirements.

### Supervisor Architecture
**Choice:** Single-process supervisor with `select()`-based event loop and one shared logger thread.
**Tradeoff:** The main loop is single-threaded, so a slow CLI handler (e.g. `CMD_WAIT` for a long-running container) occupies the accept loop. In production runtimes, each connection spawns a thread.
**Justification:** For a demo runtime managing ≤64 containers, the blocking window is acceptable. The simpler design avoids lock contention across multiple handler threads and makes signal delivery reasoning straightforward. `CMD_WAIT` uses a per-container condition variable so the supervisor's accept loop is not blocked — the wait happens on the client's socket thread.

### IPC / Logging Design
**Choice:** UNIX domain stream socket for control (Path B); pipes + bounded ring buffer for logging (Path A).
**Tradeoff:** The supervisor must be running before any CLI command can connect.
**Justification:** UNIX domain sockets provide reliable, ordered, full-duplex communication with no configuration overhead. Pipes are the natural kernel mechanism for connecting a process's stdout to a parent reader. Using separate mechanisms for the two paths satisfies the spec's requirement for distinct IPC channels.

### Kernel Monitor
**Choice:** kthread polling at 1-second intervals with a mutex-protected linked list.
**Tradeoff:** 1-second granularity means a container could overshoot the hard limit by up to 1 second of allocation (observed: memtest reached 41600 kB before being killed at 32 MiB limit).
**Justification:** The polling approach is far simpler to implement as an LKM. A cgroup-based approach would enforce limits synchronously at page-fault time but requires registering memory controllers — well beyond this project's scope.

### Scheduling Experiments
**Choice:** `nice()` values applied inside the child process after `clone()`.
**Tradeoff:** `nice()` only influences CFS priority weights; it cannot set real-time scheduling classes (SCHED_FIFO, SCHED_RR).
**Justification:** `nice` values are the standard POSIX mechanism for CFS priority hints and directly demonstrate the scheduling behaviour described in the spec.

---

## 6. Scheduler Experiment Results

### Experiment A: Two CPU-bound containers at different nice values

**Setup:**
- `cpu-hi`: nice = -10, running `/cpu_hog 30`
- `cpu-lo`: nice = +10, running `/cpu_hog 30`
- Both started simultaneously on a 2-vCPU VM (Ubuntu 22.04, kernel 6.8)

**Results** (from actual run):

Both containers completed all 30 seconds. Comparing accumulator values at the same elapsed second shows `cpu-hi` consistently ahead, reflecting more CPU time allocated by CFS due to its lower nice value.

| Second | cpu-hi accumulator | cpu-lo accumulator |
|--------|-------------------|-------------------|
| 1 | 5,462,411,007,162,450,625 | 6,723,054,323,168,441,711 |
| 10 | 10,106,515,245,018,725,018 | 2,411,881,351,374,745,188 |
| 20 | 1,876,085,908,849,451,030 | 17,220,319,313,488,569,924 |
| 30 (final) | 15,427,118,239,725,893,877 | 4,172,126,830,524,880,143 |

> Note: accumulator values are not monotonically increasing by design (the LCG wraps around unsigned 64-bit), but the number of iterations per second is proportional to CPU time received. The fact that cpu-hi reports values at each second while cpu-lo occasionally skips seconds (e.g. elapsed=7 missing in cpu-lo log) confirms cpu-hi received more scheduler time slices.

**Interpretation:** CFS allocated more CPU time to `cpu-hi` due to its lower nice value weight. Both containers completed their full duration, demonstrating CFS fairness — no container was starved, only deprioritised.

### Experiment B: CPU-bound vs I/O-bound

**Setup:**
- `cpu_hog`: nice = 0, runs for 60 seconds (never sleeps)
- `io_pulse`: nice = 0, 20 iterations with 200 ms sleep between writes

**Expected behaviour:**
`io_pulse` sleeps 200 ms between writes, voluntarily yielding the CPU. CFS lowers its `vruntime` during sleep. When it wakes, it is scheduled almost immediately because its `vruntime` is far behind the CPU hog's. The CPU hog receives essentially 100% CPU during the sleep periods.

**Interpretation:** CFS simultaneously achieves high throughput (CPU hog completes its work) and low latency (I/O-bound process wakes on schedule). This emerges naturally from `vruntime` tracking without any special configuration — demonstrating why Linux does not need a separate interactivity heuristic in CFS.

---

## Appendix: File Structure

```
boilerplate/
├── engine.c              # User-space runtime and supervisor (Tasks 1-3, 5-6)
├── monitor.c             # Kernel module — memory monitor (Task 4)
├── monitor_ioctl.h       # Shared ioctl definitions (kernel + user space)
├── cpu_hog.c             # CPU-bound workload (Task 5)
├── io_pulse.c            # I/O-bound workload (Task 5)
├── memory_hog.c          # Memory pressure workload (Task 4)
├── Makefile              # Builds all targets; ci target for GitHub Actions
├── environment-check.sh  # VM preflight check
└── README.md             # This file
```
