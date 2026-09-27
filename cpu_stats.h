#ifndef CPU_STATS_H
#define CPU_STATS_H

/**
 * Initialize CPU stats reader.
 * Performs the baseline first read from /proc/stat.
 * Returns 0 on success, -1 on failure (e.g., non-Linux platform).
 */
int cpu_stats_init(void);

/**
 * Reads /proc/stat, calculates the delta of total and idle jiffies since the
 * previous sample, and returns the CPU utilization percentage [0.0 - 100.0%].
 */
double cpu_stats_get_usage_pct(void);

#endif /* CPU_STATS_H */
