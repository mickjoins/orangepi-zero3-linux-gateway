#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int read_first_line(const char *path, char *buf, size_t buflen)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    if (!fgets(buf, (int)buflen, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static long read_file_long(const char *path)
{
    char buf[128];
    if (read_first_line(path, buf, sizeof(buf)) != 0) return -1;
    return atol(buf);
}

static long read_cpufreq_first_core(void)
{
    char path[128];
    long val;
    int  i;

    /* Some SoCs expose per-core cpufreq; try cpu0 first. */
    for (i = 0; i < 8; i++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq", i);
        val = read_file_long(path);
        if (val > 0) return val;
    }
    return -1;
}

static double read_soc_temp(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        char path[160];
        long raw;
        snprintf(path, sizeof(path),
                 "/sys/class/thermal/thermal_zone%d/temp", i);
        raw = read_file_long(path);
        if (raw > 0) {
            /* Most Allwinner platforms report milli-degrees Celsius. */
            return (double)raw / 1000.0;
        }
    }
    return -1000.0; /* invalid marker */
}

static void read_meminfo(long *total_kb, long *avail_kb)
{
    FILE *fp;
    char  line[256];
    long  total = -1, avail = -1;

    fp = fopen("/proc/meminfo", "r");
    if (!fp) goto out;

    while (fgets(line, sizeof(line), fp)) {
        long  val = -1;
        char  key[64];

        if (sscanf(line, "%63s %ld", key, &val) == 2) {
            if (strcmp(key, "MemTotal:") == 0) total = val;
            else if (strcmp(key, "MemAvailable:") == 0) avail = val;
        }
        if (total > 0 && avail > 0) break;
    }
    fclose(fp);

out:
    *total_kb = total;
    *avail_kb = avail;
}

/* Read one set of aggregate cpu jiffies from /proc/stat. */
static int read_cpu_jiffies(unsigned long long *total, unsigned long long *idle)
{
    FILE *fp;
    unsigned long long user, nice, system, idle_t, iowait, irq, softirq, steal;
    int n;

    fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    n = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle_t, &iowait, &irq, &softirq, &steal);
    fclose(fp);
    if (n < 4) return -1;

    *total = user + nice + system + idle_t + iowait + irq + softirq + steal;
    *idle  = idle_t + iowait;
    return 0;
}

static double measure_cpu_usage(void)
{
    unsigned long long total1 = 0, idle1 = 0, total2 = 0, idle2 = 0;
    unsigned long long total_delta, idle_delta;

    if (read_cpu_jiffies(&total1, &idle1) != 0) return -1.0;
    usleep(200 * 1000); /* 200 ms sampling window */
    if (read_cpu_jiffies(&total2, &idle2) != 0) return -1.0;

    total_delta = (total2 > total1) ? (total2 - total1) : 0;
    idle_delta  = (idle2  > idle1)  ? (idle2  - idle1)  : 0;

    if (total_delta == 0) return 0.0;
    return 100.0 * (1.0 - (double)idle_delta / (double)total_delta);
}

/* ------------------------------------------------------------------ */
/* public                                                              */
/* ------------------------------------------------------------------ */

int sysinfo_collect(system_info_t *info)
{
    char buf[256];
    long nproc;

    if (!info) return -1;

    memset(info, 0, sizeof(*info));
    info->soc_temp_c = -1000.0;
    info->cpu_freq_khz = -1;
    info->mem_total_kb = -1;
    info->mem_avail_kb = -1;

    if (gethostname(info->hostname, sizeof(info->hostname) - 1) != 0) {
        snprintf(info->hostname, sizeof(info->hostname), "unknown");
    }

    nproc = sysconf(_SC_NPROCESSORS_ONLN);
    info->cpu_count = (int)(nproc > 0 ? nproc : 1);

    if (read_first_line("/proc/uptime", buf, sizeof(buf)) == 0) {
        info->uptime_s = strtod(buf, NULL);
    }

    if (read_first_line("/proc/loadavg", buf, sizeof(buf)) == 0) {
        sscanf(buf, "%lf %lf %lf", &info->loadavg_1m,
               &info->loadavg_5m, &info->loadavg_15m);
    }

    read_meminfo(&info->mem_total_kb, &info->mem_avail_kb);
    if (info->mem_total_kb > 0) {
        info->mem_usage_pct = 100.0 *
            (1.0 - (double)info->mem_avail_kb / (double)info->mem_total_kb);
    }

    info->cpu_usage_pct = measure_cpu_usage();
    info->soc_temp_c = read_soc_temp();
    info->cpu_freq_khz = read_cpufreq_first_core();

    return 0;
}
