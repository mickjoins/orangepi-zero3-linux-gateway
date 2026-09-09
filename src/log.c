#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static FILE *g_log_file = NULL;
static int   g_use_syslog = 0;
static char  g_ident[64] = "opiz3-gateway";

void log_init(const char *ident, const char *log_file, int use_syslog)
{
    if (ident && ident[0]) {
        snprintf(g_ident, sizeof(g_ident), "%s", ident);
    }

    if (log_file && log_file[0]) {
        FILE *fp = fopen(log_file, "a");
        if (fp) {
            g_log_file = fp;
            setvbuf(g_log_file, NULL, _IOLBF, 0);
            return;
        }
        /* Fall back to stderr if the log file cannot be opened. */
    }

    if (use_syslog) {
        g_use_syslog = 1;
        openlog(g_ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }
}

void log_write(int level, const char *fmt, ...)
{
    va_list ap;
    char    msg[1024];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (g_log_file) {
        time_t now = time(NULL);
        struct tm tm_now;
        char ts[32];

        localtime_r(&now, &tm_now);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
        fprintf(g_log_file, "[%s] %s\n", ts, msg);
    } else if (g_use_syslog) {
        static const int prio_map[] = { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR };
        int prio = (level >= 0 && level <= 3) ? prio_map[level] : LOG_INFO;
        syslog(prio, "%s", msg);
    } else {
        static const char *tag_map[] = { "DEBUG", "INFO", "WARN", "ERROR" };
        const char *tag = (level >= 0 && level <= 3) ? tag_map[level] : "INFO";
        fprintf(stderr, "[%s] %s\n", tag, msg);
    }
}
