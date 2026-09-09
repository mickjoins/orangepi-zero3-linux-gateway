#ifndef SYSINFO_H
#define SYSINFO_H

typedef struct {
    double uptime_s;
    int    cpu_count;
    double cpu_usage_pct;       /* percent 0..100 */
    double loadavg_1m;
    double loadavg_5m;
    double loadavg_15m;
    long   mem_total_kb;
    long   mem_avail_kb;
    double mem_usage_pct;
    double soc_temp_c;
    long   cpu_freq_khz;
    char   hostname[64];
} system_info_t;

int sysinfo_collect(system_info_t *info);

#endif /* SYSINFO_H */
