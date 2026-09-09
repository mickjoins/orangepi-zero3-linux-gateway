#ifndef LOG_H
#define LOG_H

void log_init(const char *ident, const char *log_file, int use_syslog);

/* level: 0=debug, 1=info, 2=warn, 3=error */
void log_write(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) log_write(0, __VA_ARGS__)
#define LOG_INFO(...)  log_write(1, __VA_ARGS__)
#define LOG_WARN(...)  log_write(2, __VA_ARGS__)
#define LOG_ERROR(...) log_write(3, __VA_ARGS__)

#endif /* LOG_H */
