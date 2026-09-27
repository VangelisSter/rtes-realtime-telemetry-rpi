#include "cpu_stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long s_prev_idle = 0;
static unsigned long long s_prev_total = 0;
static int s_initialized = 0;

static int read_proc_stat(unsigned long long *out_idle, unsigned long long *out_total) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* Verify line starts with "cpu " */
    if (strncmp(line, "cpu ", 4) != 0) {
        return -1;
    }

    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    unsigned long long guest = 0, guest_nice = 0;

    int scanned = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                         &user, &nice, &system, &idle, &iowait, &irq, &softirq,
                         &steal, &guest, &guest_nice);

    if (scanned < 4) {
        return -1;
    }

    /* Total Idle includes idle and iowait */
    unsigned long long idle_all = idle + iowait;

    /* Total includes all user, nice, system, idle, iowait, irq, softirq, steal */
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;

    *out_idle = idle_all;
    *out_total = total;
    return 0;
}

int cpu_stats_init(void) {
    if (read_proc_stat(&s_prev_idle, &s_prev_total) == 0) {
        s_initialized = 1;
        return 0;
    }
    s_initialized = 0;
    return -1;
}

double cpu_stats_get_usage_pct(void) {
    if (!s_initialized) {
        if (cpu_stats_init() != 0) {
            return 0.0;
        }
        return 0.0;
    }

    unsigned long long curr_idle = 0;
    unsigned long long curr_total = 0;

    if (read_proc_stat(&curr_idle, &curr_total) != 0) {
        return 0.0;
    }

    if (curr_total <= s_prev_total) {
        return 0.0;
    }

    unsigned long long delta_total = curr_total - s_prev_total;
    unsigned long long delta_idle = (curr_idle >= s_prev_idle) ? (curr_idle - s_prev_idle) : 0;

    s_prev_idle = curr_idle;
    s_prev_total = curr_total;

    if (delta_total == 0) {
        return 0.0;
    }

    if (delta_idle > delta_total) {
        delta_idle = delta_total;
    }

    double usage = 100.0 * (1.0 - ((double)delta_idle / (double)delta_total));
    if (usage < 0.0) usage = 0.0;
    if (usage > 100.0) usage = 100.0;

    return usage;
}
