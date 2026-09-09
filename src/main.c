#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "gpio_dev.h"
#include "http_server.h"
#include "i2c_sensor.h"
#include "log.h"
#include "shared.h"
#include "sysinfo.h"
#include "uart_reader.h"
#include "version.h"

/* ------------------------------------------------------------------ */
/* global threads                                                      */
/* ------------------------------------------------------------------ */

static pthread_t g_collector_thread;
static int g_collector_thread_started = 0;

static pthread_t g_uart_thread;
static int g_uart_thread_started = 0;

/* ------------------------------------------------------------------ */
/* forwards                                                            */
/* ------------------------------------------------------------------ */

static void print_usage(FILE *fp, const char *argv0);
static void daemonize(void);

/* ------------------------------------------------------------------ */
/* signal handling                                                     */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
    (void)sig;
    shared_set_running(0);
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    if (sigaction(SIGHUP, &sa, NULL) != 0) return -1;

    /* Writes to already-closed client sockets must return EPIPE, not kill
     * the whole gateway. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) != 0) return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* button callback                                                     */
/* ------------------------------------------------------------------ */

static void on_button_event(int pressed, void *user)
{
    (void)user;

    shared_button_set(pressed);

    if (pressed) {
        int led = shared_get_led() > 0 ? 0 : 1;
        if (gpio_dev_set_led(led) == 0) {
            shared_set_led(led);
            LOG_INFO("button pressed -> LED now %s", led ? "ON" : "OFF");
        }
    }
}

/* ------------------------------------------------------------------ */
/* fake sensor data for simulator                                      */
/* ------------------------------------------------------------------ */

static void make_simulated_sensor(sensor_sample_t *s)
{
    static int tick = 0;
    double angle = (double)(tick++) * 0.35;

    s->valid = 1;
    s->temperature_c = 25.0 + 4.0 * sin(angle);
    s->humidity_pct = 52.0 + 12.0 * cos(angle * 0.7);
    s->timestamp = time(NULL);
}

/* ------------------------------------------------------------------ */
/* background threads                                                  */
/* ------------------------------------------------------------------ */

static void *collector_thread(void *arg)
{
    (void)arg;

    LOG_INFO("collector thread started");

    while (shared_is_running()) {
        system_info_t  sys;
        sensor_sample_t sensor;
        int i;

        if (sysinfo_collect(&sys) == 0) {
            shared_set_sysinfo(&sys);
        }

        memset(&sensor, 0, sizeof(sensor));
        if (g_config.simulate) {
            make_simulated_sensor(&sensor);
        } else {
            if (strcmp(g_config.i2c_sensor_type, "aht20") == 0) {
                if (i2c_aht20_read(g_config.i2c_bus, &sensor) != 0) {
                    LOG_WARN("sensor read failed");
                    sensor.valid = 0;
                }
            } else {
                LOG_WARN("unsupported i2c_sensor_type '%s', use aht20",
                         g_config.i2c_sensor_type);
                sensor.valid = 0;
            }
        }
        shared_set_sensor(&sensor);

        if (sensor.valid) {
            LOG_DEBUG("sensor: %.2f C, %.2f %%RH",
                      sensor.temperature_c, sensor.humidity_pct);
        }

        /* interruptible sleep in 100 ms slices */
        for (i = 0; i < g_config.collect_interval_s * 10 && shared_is_running(); i++) {
            usleep(100 * 1000);
        }
    }

    LOG_INFO("collector thread exited");
    return NULL;
}

static void *serial_thread(void *arg)
{
    uart_handle_t uart;
    (void)arg;

    LOG_INFO("serial thread started");

    uart_handle_init(&uart, g_config.uart_dev, g_config.uart_baud, g_config.simulate);

    if (g_config.simulate) {
        int seq = 0;
        while (shared_is_running()) {
            char line[256];
            int i;

            double t = 25.0 + 2.0 * sin(seq * 0.2);
            double h = 55.0 + 10.0 * cos(seq * 0.12);
            snprintf(line, sizeof(line),
                     "{\"sim\":1,\"seq\":%d,\"temperature\":%.1f,\"humidity\":%.1f}",
                     seq, t, h);
            seq++;
            shared_set_serial_line(line);
            LOG_DEBUG("sim serial: %s", line);

            for (i = 0; i < 50 && shared_is_running(); i++) {
                usleep(100 * 1000);
            }
        }
    } else {
        if (uart_open(&uart) != 0) {
            LOG_WARN("serial port unavailable, serial reader disabled");
            uart_close(&uart);
            return NULL;
        }

        while (shared_is_running()) {
            char buf[512];
            int  n = uart_read_line(&uart, buf, sizeof(buf));

            if (n > 0) {
                shared_set_serial_line(buf);
                LOG_DEBUG("serial: %s", buf);
            } else if (n < 0) {
                LOG_WARN("serial read error, serial reader exiting");
                break;
            }
        }
    }

    uart_close(&uart);
    LOG_INFO("serial thread exited");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* one-shot actions                                                    */
/* ------------------------------------------------------------------ */

static int action_scan_i2c(void)
{
    int addrs[128];
    int n;

    if (g_config.simulate) {
        printf("I2C scan (simulated): [0x38, 0x3c, 0x50]\n");
        return 0;
    }

    n = i2c_bus_scan(g_config.i2c_bus, addrs, 128);
    if (n < 0) {
        fprintf(stderr, "I2C bus scan failed on %s\n", g_config.i2c_bus);
        return 1;
    }

    printf("I2C scan %s: %d device(s)\n", g_config.i2c_bus, n);
    for (int i = 0; i < n; i++) {
        printf("  0x%02x\n", addrs[i]);
    }
    return 0;
}

static int action_led(const char *state)
{
    int value;
    int rc;

    if (strcmp(state, "on") == 0) value = 1;
    else if (strcmp(state, "off") == 0) value = 0;
    else {
        fprintf(stderr, "invalid LED state '%s' (use on/off)\n", state);
        return 1;
    }

    if (gpio_dev_init(&g_config, NULL, NULL) != 0) {
        fprintf(stderr, "GPIO init failed\n");
        return 1;
    }

    rc = gpio_dev_set_led(value);
    if (rc == 0) {
        shared_set_led(value);
        printf("LED: %s\n", value ? "ON" : "OFF");
    } else {
        fprintf(stderr, "LED set failed\n");
    }
    gpio_dev_close();
    return rc == 0 ? 0 : 1;
}

static int action_status(void)
{
    system_info_t sys;
    sensor_sample_t sensor;
    int led = -1;

    sysinfo_collect(&sys);

    if (gpio_dev_init(&g_config, NULL, NULL) != 0) {
        led = -1;
    } else {
        led = gpio_dev_get_led();
    }

    if (g_config.simulate) {
        make_simulated_sensor(&sensor);
    } else {
        if (i2c_aht20_read(g_config.i2c_bus, &sensor) != 0) {
            memset(&sensor, 0, sizeof(sensor));
        }
    }

    printf("{\n");
    printf("  \"app\": \"%s\",\n", APP_NAME);
    printf("  \"version\": \"%s\",\n", APP_VERSION);
    printf("  \"hostname\": \"%s\",\n", sys.hostname);
    printf("  \"uptime_s\": %.0f,\n", sys.uptime_s);
    printf("  \"cpu_usage_pct\": %.2f,\n", sys.cpu_usage_pct);
    printf("  \"soc_temp_c\": %.2f,\n", sys.soc_temp_c);
    printf("  \"mem_usage_pct\": %.2f,\n", sys.mem_usage_pct);
    printf("  \"sensor\": {\"valid\":%d,\"temperature_c\":%.2f,\"humidity_pct\":%.2f},\n",
           sensor.valid, sensor.temperature_c, sensor.humidity_pct);
    printf("  \"led\": %d\n", led >= 0 ? led : -1);
    printf("}\n");

    gpio_dev_close();
    return 0;
}

/* ------------------------------------------------------------------ */
/* server mode                                                         */
/* ------------------------------------------------------------------ */

static int run_server(int daemon_mode)
{
    if (daemon_mode) {
        daemonize();
    }

    log_init(APP_NAME, g_config.log_file, daemon_mode);
    shared_init();
    shared_set_running(1);

    if (install_signal_handlers() != 0) {
        LOG_ERROR("cannot install signal handlers: %s", strerror(errno));
        return 1;
    }

    config_print(&g_config);

    /* GPIO backend init is best-effort in server mode; HTTP GPIO handlers
     * return JSON with led=-1 when it is unavailable. */
    if (gpio_dev_init(&g_config, on_button_event, NULL) != 0) {
        LOG_WARN("GPIO not available; LED/button features are disabled");
    }

    /* start collector */
    if (pthread_create(&g_collector_thread, NULL, collector_thread, NULL) != 0) {
        LOG_ERROR("cannot start collector thread");
        return 1;
    }
    g_collector_thread_started = 1;

    /* start serial reader */
    if (pthread_create(&g_uart_thread, NULL, serial_thread, NULL) != 0) {
        LOG_ERROR("cannot start serial thread");
        return 1;
    }
    g_uart_thread_started = 1;

    /* start HTTP server */
    if (http_server_start(g_config.http_port, g_config.http_bind_ip) != 0) {
        LOG_ERROR("cannot start http server");
        return 1;
    }

    LOG_INFO("%s v%s started (simulate=%d)", APP_NAME, APP_VERSION, g_config.simulate);

    /* main thread waits until SIGINT/SIGTERM */
    while (shared_is_running()) {
        sleep(1);
    }

    LOG_INFO("shutting down...");

    shared_set_running(0);

    http_server_stop();

    if (g_collector_thread_started) {
        pthread_join(g_collector_thread, NULL);
        g_collector_thread_started = 0;
    }
    if (g_uart_thread_started) {
        pthread_join(g_uart_thread, NULL);
        g_uart_thread_started = 0;
    }

    gpio_dev_close();
    shared_destroy();
    LOG_INFO("bye");
    return 0;
}

/* ------------------------------------------------------------------ */
/* option parsing                                                      */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int opt;
    int opt_index = 0;
    char *config_path = NULL;
    const char *led_state = NULL;
    int daemon_mode = 0;
    int scan_i2c = 0;
    int show_status = 0;
    int simulate_override = 0;
    int port_override = -1;

    static const struct option long_opts[] = {
        { "config",    required_argument, NULL, 'c' },
        { "port",      required_argument, NULL, 'p' },
        { "simulate",  no_argument,       NULL, 's' },
        { "daemon",    no_argument,       NULL, 'd' },
        { "scan-i2c",  no_argument,       NULL, 'i' },
        { "led",       required_argument, NULL, 'l' },
        { "status",    no_argument,       NULL, 't' },
        { "version",   no_argument,       NULL, 'v' },
        { "help",      no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    config_set_defaults(&g_config);

    while ((opt = getopt_long(argc, argv, "c:p:sdl:itvh", long_opts, &opt_index)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'p': port_override = atoi(optarg); break;
        case 's': simulate_override = 1; break;
        case 'd': daemon_mode = 1; break;
        case 'i': scan_i2c = 1; break;
        case 'l': led_state = optarg; break;
        case 't': show_status = 1; break;
        case 'v':
            printf("%s v%s\n", APP_NAME, APP_VERSION);
            return 0;
        case 'h':
            print_usage(stdout, argv[0]);
            return 0;
        default:
            print_usage(stderr, argv[0]);
            return 1;
        }
    }

    if (config_path) {
        if (config_load(&g_config, config_path) != 0) {
            fprintf(stderr, "failed to load config file: %s\n", config_path);
            return 1;
        }
    }

    /* CLI explicit --simulate/--port must override values from the file. */
    if (simulate_override) {
        g_config.simulate = 1;
    }
    if (port_override > 0) {
        g_config.http_port = port_override;
    }

    /* one-shot actions work before the normal log/sw initialization */
    if (scan_i2c) {
        return action_scan_i2c();
    }
    if (led_state) {
        return action_led(led_state);
    }
    if (show_status) {
        return action_status();
    }

    return run_server(daemon_mode);
}

/* ------------------------------------------------------------------ */
/* usage / daemon                                                      */
/* ------------------------------------------------------------------ */

static void print_usage(FILE *fp, const char *argv0)
{
    fprintf(fp,
        "Usage: %s [options]\n"
        "\n"
        "Orange Pi Zero 3 embedded Linux edge gateway, version " APP_VERSION "\n"
        "\n"
        "Options:\n"
        "  -c, --config FILE    config file (default: built-in defaults)\n"
        "  -p, --port N         override HTTP listen port\n"
        "  -s, --simulate       simulate GPIO/sensor/serial on host PC\n"
        "  -d, --daemon         run as a daemon (log to syslog)\n"
        "  -i, --scan-i2c       scan the I2C bus and exit\n"
        "  -l, --led on|off     set LED state and exit\n"
        "  -t, --status         print system/sensor status JSON once and exit\n"
        "  -v, --version        print version and exit\n"
        "  -h, --help           show this help\n"
        "\n"
        "Server mode endpoints:\n"
        "  GET  /                web dashboard\n"
        "  GET  /api/status      full status JSON\n"
        "  GET  /api/sensor      latest sensor reading\n"
        "  GET  /api/serial      latest serial line\n"
        "  GET/POST /api/gpio?state=on|off|toggle\n"
        "\n"
        "Default config values:\n"
        "  http_port=8080  gpio_chip=0  led_line=69  button_line=70\n"
        "  i2c_bus=/dev/i2c-1  sensor=aht20  uart=/dev/ttyS1  baud=115200\n",
        argv0);
}

static void daemonize(void)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        exit(1);
    }
    if (pid > 0) {
        _exit(0); /* parent exits */
    }

    if (setsid() < 0) {
        fprintf(stderr, "setsid failed: %s\n", strerror(errno));
        exit(1);
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "second fork failed: %s\n", strerror(errno));
        exit(1);
    }
    if (pid > 0) {
        _exit(0);
    }

    if (chdir("/") != 0) {
        /* cannot happen in a healthy system; keep running with original cwd */
    }
    umask(0);

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
}
