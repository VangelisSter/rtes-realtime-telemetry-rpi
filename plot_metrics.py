#!/usr/bin/env python3
"""
plot_metrics.py - RTES Telemetry Data Post-Processing Script
Target: Bluesky Jetstream Telemetry Log (metrics_log.txt)
Deliverable 5.A:
  1. Jitter Plot (ms vs time)
  2. Dual-Axis Load (Hz) & Buffer Occupancy (%) Plot
  3. CPU Busy & Idle (%) vs Message Rate (Hz) Correlation Plot
"""

import os
import sys
import glob
import datetime
import numpy as np
import pandas as pd
import argparse
import matplotlib.pyplot as plt

def main():
    parser = argparse.ArgumentParser(
        description="RTES Telemetry Data Post-Processing Script"
    )
    parser.add_argument(
        "logfile",
        nargs="?",
        default=None,
        help="Path to telemetry log .txt file (default: metrics_log.txt or newest metrics_log_*.txt)"
    )
    args = parser.parse_args()

    log_file = args.logfile
    if not log_file:
        if os.path.exists("metrics_log.txt"):
            log_file = "metrics_log.txt"
        else:
            candidates = sorted(glob.glob("metrics_log_*.txt"))
            if candidates:
                log_file = candidates[-1]
                print(f"[INFO] Auto-detected latest telemetry log: '{log_file}'")
            else:
                print("Error: No telemetry log file found (metrics_log.txt or metrics_log_*.txt).", file=sys.stderr)
                print("Usage: python plot_metrics.py <logfile.txt>", file=sys.stderr)
                sys.exit(1)

    if not os.path.exists(log_file):
        print(f"Error: Log file '{log_file}' not found.", file=sys.stderr)
        print("Usage: python plot_metrics.py <logfile.txt>", file=sys.stderr)
        sys.exit(1)

    print(f"Loading telemetry data from: {log_file}")
    df = pd.read_csv(log_file)

    required_cols = [
        "Seconds", "Nanoseconds", "Commit_Count", "Identity_Count",
        "Account_Count", "Info_Count", "Buffer_Occupancy_Pct", "CPU_Pct"
    ]
    for col in required_cols:
        if col not in df.columns:
            print(f"Error: Missing column '{col}' in log file.", file=sys.stderr)
            sys.exit(1)

    # Compute high-resolution timestamps
    t_exact = df["Seconds"].values + df["Nanoseconds"].values * 1e-9
    t_rel_sec = t_exact - t_exact[0]
    t_rel_hours = t_rel_sec / 3600.0

    # Total message rate (Hz)
    total_rate = (
        df["Commit_Count"] + df["Identity_Count"] +
        df["Account_Count"] + df["Info_Count"]
    ).values

    # Jitter calculation
    dt = np.diff(t_exact)
    jitter_ms = (dt - 1.0) * 1000.0
    t_jitter_hours = t_rel_hours[1:]

    # Summary metrics
    dt_start = datetime.datetime.fromtimestamp(t_exact[0])
    dt_end = datetime.datetime.fromtimestamp(t_exact[-1])

    print("\n" + "="*50)
    print("           TELEMETRY SUMMARY REPORT               ")
    print("="*50)
    print(f"Log File              : {log_file}")
    print(f"Start Time            : {dt_start.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"End Time              : {dt_end.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Total Duration        : {len(df)} s ({t_rel_hours[-1]:.2f} hours)")
    print(f"Total Messages        : {np.sum(total_rate):,}")
    print(f"Average Message Rate  : {np.mean(total_rate):.2f} Hz")
    print(f"Peak Message Rate     : {np.max(total_rate)} Hz")
    print(f"Mean Buffer Occupancy : {df['Buffer_Occupancy_Pct'].mean():.2f} %")
    print(f"Peak Buffer Occupancy : {df['Buffer_Occupancy_Pct'].max():.2f} %")
    print(f"Mean CPU Usage        : {df['CPU_Pct'].mean():.2f} %")
    print(f"Peak CPU Usage        : {df['CPU_Pct'].max():.2f} %")
    print(f"Mean Jitter           : {np.mean(jitter_ms):.4f} ms")
    print(f"Max Absolute Jitter   : {np.max(np.abs(jitter_ms)):.4f} ms")
    print(f"Jitter Std Dev        : {np.std(jitter_ms):.4f} ms")
    print("="*50 + "\n")

    # 1. Jitter Plot
    plt.figure(figsize=(10, 5))
    plt.plot(t_jitter_hours, jitter_ms, color="#1f77b4", linewidth=0.8, label="Jitter = (dt - 1.0s)")
    plt.axhline(0, color="red", linestyle="--", linewidth=1.2, label="Ideal (0 ms)")
    plt.axhline(np.mean(jitter_ms), color="black", linestyle="-", linewidth=1.0,
                label=f"Mean Jitter: {np.mean(jitter_ms):.3f} ms")
    plt.title("Periodic Thread Jitter over Time", fontsize=14, fontweight="bold")
    plt.xlabel("Time Elapsed (Hours)", fontsize=12)
    plt.ylabel("Jitter (ms)", fontsize=12)
    plt.xlim(0, t_jitter_hours[-1] if len(t_jitter_hours) > 0 else 1)
    plt.grid(True, linestyle="--", alpha=0.7)
    plt.legend(loc="upper right")
    plt.tight_layout()
    plt.savefig("jitter_plot.png", dpi=300)
    print("Saved: jitter_plot.png")

    # 2. Dual-Axis Network Load & Buffer Occupancy
    fig, ax1 = plt.subplots(figsize=(10, 5))
    color_rate = "#1f77b4"
    ax1.set_xlabel("Time Elapsed (Hours)", fontsize=12)
    ax1.set_ylabel("Message Rate (Hz)", color=color_rate, fontsize=12)
    line1 = ax1.plot(t_rel_hours, total_rate, color=color_rate, linewidth=0.8, label="Message Rate (Hz) [Left Axis]")
    ax1.tick_params(axis="y", labelcolor=color_rate)
    ax1.set_xlim(0, t_rel_hours[-1] if len(t_rel_hours) > 0 else 1)
    ax1.set_ylim(0, max(total_rate) * 1.2 + 1)
    ax1.grid(True, linestyle="--", alpha=0.5)

    ax2 = ax1.twinx()
    color_buffer = "#d62728"
    ax2.set_ylabel("Buffer Occupancy (%)", color=color_buffer, fontsize=12)
    line2 = ax2.plot(t_rel_hours, df["Buffer_Occupancy_Pct"], color=color_buffer, linewidth=1.0, label="Buffer Occupancy (%) [Right Axis]")
    ax2.tick_params(axis="y", labelcolor=color_buffer)

    # Scale to ~110% of maximum observed buffer occupancy (with a sensible lower bound)
    max_buffer = df["Buffer_Occupancy_Pct"].max()
    buffer_ylim = max(0.1, max_buffer * 1.10)
    ax2.set_ylim(0, buffer_ylim)

    # Combined legend for both axes
    lines = line1 + line2
    labels = [l.get_label() for l in lines]
    ax1.legend(lines, labels, loc="upper right", framealpha=0.9)

    plt.title("Network Message Rate (Hz) & Circular Buffer Occupancy (%)", fontsize=14, fontweight="bold")
    plt.tight_layout()
    plt.savefig("load_buffer_plot.png", dpi=300)
    print("Saved: load_buffer_plot.png")

    # 3. CPU vs Rate Correlation
    fig, (ax_cpu, ax_idle) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
    ax_cpu.scatter(total_rate, df["CPU_Pct"], color="#2ca02c", alpha=0.4, s=15)
    ax_cpu.set_ylabel("CPU Usage (%)", fontsize=11)
    ax_cpu.set_ylim(0, 100)
    ax_cpu.set_title("CPU Utilization vs Incoming Message Rate (Hz)", fontsize=13, fontweight="bold")
    ax_cpu.grid(True, linestyle="--", alpha=0.7)

    idle_pct = 100.0 - df["CPU_Pct"]
    ax_idle.scatter(total_rate, idle_pct, color="#d62728", alpha=0.4, s=15)
    ax_idle.set_xlabel("Incoming Message Rate (Hz)", fontsize=12)
    ax_idle.set_ylabel("CPU Idle (%)", fontsize=11)
    ax_idle.set_ylim(0, 100)
    ax_idle.set_title("CPU Idle (%) vs Incoming Message Rate (Hz)", fontsize=13, fontweight="bold")
    ax_idle.grid(True, linestyle="--", alpha=0.7)

    plt.tight_layout()
    plt.savefig("cpu_load_plot.png", dpi=300)
    print("Saved: cpu_load_plot.png")

    print("All post-processing plots generated successfully.")

if __name__ == "__main__":
    main()
