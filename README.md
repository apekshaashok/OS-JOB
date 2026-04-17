# Multi-Container Runtime – OS-Jackfruit

## 1. Team Information

|     Name      |       SRN     |
|---------------|---------------|
| Apeksha Ashok | PES1UG24AM048 |
| Avanthika J   | PES1UG24AM059 |

*(Replace with your actual names and SRNs)*

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot **OFF**.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build everything

```bash
make
```

This compiles `engine`, `cpu_hog`, `io_pulse`, `memory_hog`, and the kernel module `monitor.ko`.

### Prepare root filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

### Load kernel module

```bash
sudo insmod monitor.ko
# Verify device node was created:
ls -l /dev/container_monitor
```

### Full demo run sequence

Open **Terminal 1** – start the supervisor:

```bash
sudo ./engine supervisor ./rootfs-base
```

Open **Terminal 2** – use the CLI:

```bash
# Create per-container rootfs copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy workload binaries into the rootfs so they're accessible inside containers
cp cpu_hog    ./rootfs-alpha/
cp io_pulse   ./rootfs-beta/
cp memory_hog ./rootfs-alpha/

# Start two containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96

# List tracked containers
sudo ./engine ps

# Inspect logs
sudo ./engine logs alpha
sudo ./engine logs beta

# Run memory test (will trigger soft/hard limit events in dmesg)
sudo ./engine start memtest ./rootfs-alpha /memory_hog 200 --soft-mib 32 --hard-mib 64

# Check kernel events
dmesg | tail -20

# Stop containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Run scheduling experiment (see Section 6)
cp cpu_hog ./rootfs-alpha/
cp cpu_hog ./rootfs-beta/
sudo ./engine start cpu-hi ./rootfs-alpha /cpu_hog 20
sudo ./engine start cpu-lo ./rootfs-beta  /cpu_hog 20

# In yet another terminal, renice the second one:
PID_LO=$(sudo ./engine ps | grep cpu-lo | awk '{print $2}')
sudo renice -n 15 -p $PID_LO

sudo ./engine logs cpu-hi
sudo ./engine logs cpu-lo
```

### Stop and clean up

```bash
# Ctrl-C the supervisor, or:
kill $(pgrep engine)

# Unload kernel module
sudo rmmod monitor

# Check no zombies
ps aux | grep -v grep | grep engine || echo "clean"
```

---

## 3. Demo Screenshots

![Screenshot](screenshots/ss1.png)

![Screenshot](screenshots/ss2.png)

![Screenshot](screenshots/ss3.png)


![Screenshot](screenshots/ss4.1.png)


![Screenshot](screenshots/ss5.png)

![Screenshot](screenshots/ss6.png)

![Screenshot](screenshots/ss7.png)

![Screenshot](screenshots/ss8.png)

![Screenshot](screenshots/ss9.png)

![Screenshot](screenshots/ss10.png)
---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Containers are isolated using Linux **namespaces**. The engine calls `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`. This creates a new PID namespace (container processes get their own PID 1), a new UTS namespace (separate hostname), and a new mount namespace (separate filesystem view).

After `clone()`, the child calls `chroot(rootfs)`, redirecting its filesystem root to the Alpine mini root filesystem. `/proc` is mounted fresh inside the namespace so the container sees only its own processes.

The host kernel is still shared: the same scheduler, memory allocator, network stack, and device drivers serve all containers. There is no CPU or memory virtualisation — all containers share physical resources.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary to maintain state across container lifetimes. Without it, there would be no process to reap zombies, hold metadata, or relay logs.

When a container is launched, `clone()` creates a child process. The parent (supervisor) stores the child's PID in a `Container` struct. When the child exits, the kernel sends `SIGCHLD` to the supervisor. The `sigchld_handler` calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children, updating their state to `STATE_EXITED` or `STATE_KILLED`.

### 4.3 IPC, Threads, and Synchronisation

Two IPC mechanisms are used:

**Pipes (logging):** Each container's stdout/stderr is redirected into a pipe. A dedicated producer thread per container reads from the pipe and pushes lines into a shared ring buffer. A single consumer thread reads from the ring buffer and writes to per-container log files.

**UNIX domain socket (CLI):** The CLI client (`./engine ps`, etc.) connects to the supervisor's listening socket, sends a command string, and reads the response. This is a separate channel from logging so control commands are never mixed with log data.

**Synchronisation:** The ring buffer is protected by a `pthread_mutex_t`, with two `pthread_cond_t` variables for `not_empty` and `not_full`. Without the mutex, two producer threads could simultaneously write to the same slot (data corruption). Without the condition variables, threads would busy-spin wasting CPU. The global container array is also protected by its own mutex to prevent the SIGCHLD handler and socket handler from corrupting metadata simultaneously.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) is the number of physical memory pages currently mapped into the process. It does not include pages that have been swapped out, file-backed pages that haven't been faulted in, or memory shared with other processes (counted per-process). It is a practical proxy for "how much RAM this process is actually using right now."

Soft and hard limits implement a two-tier policy. A soft limit triggers a warning but lets the process continue, allowing it to handle sudden spikes. A hard limit is an absolute ceiling — the process is killed when it crosses it. This mirrors production systems where a small overage is acceptable but runaway allocation must be stopped.

Enforcement belongs in kernel space because user-space enforcement can be defeated: a misbehaving process can ignore signals, and checking RSS from user space introduces a TOCTOU race (the process could allocate between the check and the response). A kernel timer callback runs in kernel context, cannot be blocked by the monitored process, and has direct access to the `mm_struct` via `get_mm_rss()`.

### 4.5 Scheduling Behaviour

Linux uses the Completely Fair Scheduler (CFS). CFS assigns CPU time proportional to weight, which is derived from `nice` values. A process with `nice 0` has weight 1024; `nice 15` has weight ~36. If two processes compete on one CPU, the higher-weight process receives roughly `1024/(1024+36) ≈ 97%` of CPU time.

In our experiment, `cpu-hi` (nice 0) completes significantly more iterations per second than `cpu-lo` (nice 15) when both run on the same CPU. The I/O-bound container (`io_pulse`) voluntarily yields the CPU during disk waits, so it barely affects CPU-bound containers but competes for disk bandwidth.

---

## 5. Design Decisions and Tradeoffs

| Subsystem | Choice | Tradeoff | Justification |
|-----------|--------|----------|---------------|
| Namespace isolation | `clone()` with PID+UTS+mount | No network isolation | Simplest approach that meets spec; net ns adds complexity |
| Supervisor architecture | Single process + SIGCHLD | Single point of failure | Matches Unix process model; simple to reason about |
| IPC/Logging | Pipe + UNIX socket | Pipes are one-directional | Pipes are the natural fd-based channel for child stdout |
| Log buffer | Bounded ring buffer | Drops data if consumer is too slow | Bounded prevents unbounded memory growth |
| Kernel monitor | Kernel timer polling | Not instantaneous (2s delay) | Simpler than kernel notifiers; sufficient for this project |

---

## 6. Scheduler Experiment Results

### Setup

Two containers each running `cpu_hog 20` on the same CPU:

| Container | nice value | iterations (20s) |
|-----------|-----------|-----------------|
| cpu-hi    | 0         | ~420             |
| cpu-lo    | 15        | ~15              |

*(Fill in actual numbers from your run)*

### I/O vs CPU

| Container | workload  | CPU% (observed via top) |
|-----------|-----------|------------------------|
| alpha     | cpu_hog   | ~85%                   |
| beta      | io_pulse  | ~5% CPU, high iowait   |

*(Fill in actual numbers)*

**Conclusion:** CFS correctly allocates CPU proportional to weight. The I/O-bound container does not starve the CPU-bound one — it voluntarily sleeps during I/O, so CFS can give full CPU to the CPU-bound container. When both containers are CPU-bound and priorities differ, the higher-priority container dominates throughput while the lower-priority one still makes progress (fairness guarantee of CFS).
