# Real-Time Telemetry System (Bluesky Jetstream Firehose)
**Course:** Real-Time Embedded Systems (RTES) — Final Project 2026  
**Target Hardware:** Raspberry Pi Zero W (BCM2835 ARMv6zk 32-bit, Raspberry Pi OS Lite)

---

## 1. System Architecture

The application is implemented in C using POSIX Threads (`pthreads`), mutexes (`pthread_mutex_t`), and condition variables (`pthread_cond_t`) following the **Producer-Consumer-Monitor** paradigm:

```
                  ┌────────────────────────────────────────┐
                  │   Bluesky Jetstream WebSocket API      │
                  │   (wss://jetstream1.us-east.bsky...)   │
                  └──────────────────┬─────────────────────┘
                                     │ TLS Frames (JSON)
                                     ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│ THREAD 1: Producer (Asynchronous / libwebsockets event-loop)                   │
│ - Collects raw JSON frames from the network.                                   │
│ - Enqueues into preallocated Bounded Circular Queue without heap allocation.   │
│ - Signals Consumer thread via pthread_cond_signal(&not_empty).                 │
│ - Non-blocking return; automatically reconnects if network drops.              │
└────────────────────────────────────┬───────────────────────────────────────────┘
                                     │
                                     ▼
                 ┌───────────────────────────────────────┐
                 │ Bounded Circular Queue (Ring Buffer)  │
                 │ - 2,048 preallocated slots x 16 KB    │
                 │ - Mutex + Condition Variables         │
                 │ - Drop oldest policy on burst overflow│
                 └───────────────────┬───────────────────┘
                                     │
                                     ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│ THREAD 2: Consumer (Event-Driven Worker)                                       │
│ - Awakens on pthread_cond_wait(&not_empty).                                    │
│ - Dequeues JSON payload.                                                       │
│ - Parses JSON structure using cJSON to identify the "kind" field.              │
│ - Increments 4 global counters: commit, identity, account, info.               │
│ - STRICT RULE: Zero printf or file writes in this thread.                      │
└────────────────────────────────────┬───────────────────────────────────────────┘
                                     │ Protected Counters
                                     ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│ THREAD 3: Monitor / Logger (Strictly Periodic @ 1.000000000s)                  │
│ - High-accuracy sleep: clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME).        │
│ - Zero drift accumulation over long execution runs (24+ hours).                │
│ - Elevates to SCHED_FIFO (priority 80) if permissions allow.                   │
│ - Snapshots and clears 1-second message counters under mutex.                  │
│ - Measures Circular Buffer Occupancy (%) and CPU Utilization (%) via /proc/stat│
│ - Logs exact timestamp: clock_gettime(CLOCK_REALTIME, &ts).                   │
│ - Appends CSV line to metrics_log.txt with immediate fflush().                 │
└────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Raspberry Pi Zero W Specifics & Real-Time Guarantees

1. **Single-Core ARMv6zk CPU (BCM2835 @ 1GHz)**:
   - All 3 threads share a single physical CPU core.
   - To avoid CPU starvation and heap fragmentation over the 24-hour run, the circular queue uses **preallocated fixed slots** (`2048 x 16KB`). No `malloc`/`free` calls are performed during the queue push/pop cycle.
2. **Clock Drift Elimination**:
   - Instead of relative `sleep()` or `usleep()`, the monitor thread uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL)`.
   - `next_wake.tv_sec` increments by exactly `1` second on every iteration, guaranteeing **0.000000s cumulative drift**.
3. **Jitter Minimization**:
   - The monitor thread requests the real-time scheduling policy `SCHED_FIFO` with priority 80.
4. **Resilience & Network Drop Handling**:
   - `libwebsockets` connection errors or server disconnects trigger automatic reconnection attempts every 3 seconds without crashing.

---

## 3. Log File Format (`metrics_log.txt`)

Conforms strictly to Section 3 of `rtes.txt`:
```csv
Seconds,Nanoseconds,Commit_Count,Identity_Count,Account_Count,Info_Count,Buffer_Occupancy_Pct,CPU_Pct
```
- **Seconds & Nanoseconds**: Exact Unix epoch timestamp via `clock_gettime(CLOCK_REALTIME)`.
- **Message Quad-tuple**: Message count per category received during that 1-second window.
- **Buffer_Occupancy_Pct**: Instantaneous percentage of the circular queue slots in use.
- **CPU_Pct**: System CPU utilization over the last second computed from `/proc/stat` jiffies delta.

---

## 4. Dependencies & Setup

### A. On the Raspberry Pi Zero W (Target System)
Install the shared runtime libraries:
```bash
sudo apt update
sudo apt install -y libwebsockets-dev libssl-dev build-essential
```

### B. On the Development PC (Host System)
For cross-compiling, install the ARM 32-bit cross-compiler (e.g. on Ubuntu/Debian/WSL):
```bash
sudo apt update
sudo apt install -y gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf make
```

---

## 5. Compilation

### Option 1: Cross-Compilation (Recommended)
From your PC, run:
```bash
make
```
If you are linking against a sysroot from your Pi:
```bash
make SYSROOT=/path/to/pi_sysroot
```
> **Notice on Architecture Flags**: The `Makefile` specifies `-march=armv6zk -mtune=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard`. Standard `arm-linux-gnueabihf-gcc` defaults to ARMv7-A; these flags ensure the binary runs on the Raspberry Pi Zero W without `Illegal instruction` errors.

Transfer the generated binary to the Pi:
```bash
scp rtes_telemetry pi@<pi-ip-address>:~/
```

### Option 2: Native Compilation (Directly on Raspberry Pi)
If compiling directly on the Pi:
```bash
make native
```

---

## 6. Running the 24-Hour Telemetry Test

To capture data continuously for 24 hours on the physical hardware:

### Running with Real-Time Scheduling (`SCHED_FIFO`)
Running with `sudo` gives the Monitor thread permission to use `SCHED_FIFO` priority 80:
```bash
sudo ./rtes_telemetry
```

### Unattended 24-Hour Run via `tmux`
To prevent the process from stopping if your SSH session disconnects:
```bash
# Start a new tmux session
tmux new -s rtes

# Launch the program
sudo ./rtes_telemetry

# Detach from session by pressing Ctrl+B, then D
# Re-attach anytime with:
tmux attach -t rtes
```

---

## 7. Deliverable 5.A: Post-Processing & Plots

Once the 24-hour log file (`metrics_log.txt`) is collected, copy it to your computer and generate the required figures:

### In MATLAB
Run the included script:
```matlab
plot_metrics
```
This generates:
1. **Jitter Plot**: Time difference $(dt - 1.0\text{s})$ in milliseconds over time.
2. **Load & Buffer Plot**: Dual-axis plot of message rate (Hz) and buffer occupancy (%).
3. **CPU Diagram**: Correlation between message rate (Hz) and CPU usage / Idle (%).

### In Python (Alternative)
```bash
pip install numpy pandas matplotlib
python plot_metrics.py
```
This produces `jitter_plot.png`, `load_buffer_plot.png`, and `cpu_load_plot.png`.
