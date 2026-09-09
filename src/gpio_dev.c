#include "gpio_dev.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

#include "log.h"
#include "shared.h"

typedef enum {
    GPIO_BACKEND_NONE = 0,
    GPIO_BACKEND_CHARDEV,
    GPIO_BACKEND_SYSFS,
    GPIO_BACKEND_SIM
} gpio_backend_t;

static gpio_backend_t g_backend = GPIO_BACKEND_NONE;

/* When 1, use the kernel GPIO v2 character-device ioctls. */
static int g_use_v2 = 0;

/* chardev file descriptors */
static int g_led_fd = -1;
static int g_btn_fd = -1;

/* config snapshots */
static int g_led_active_low = 0;
static int g_btn_active_low = 0;
static int g_led_line      = -1;
static int g_button_line   = -1;
static int g_button_ready  = 0;

/* fake state for simulation */
static int g_sim_led = 0;
static int g_sim_button = 0;

/* button monitor */
static pthread_t g_btn_thread;
static int g_btn_thread_started = 0;
static gpio_button_cb_t g_button_cb = NULL;
static void *g_button_user = NULL;

/* ------------------------------------------------------------------ */
/* sysfs fallback helpers (legacy /sys/class/gpio)                     */
/* ------------------------------------------------------------------ */

static int sysfs_gpio_export(int gpio)
{
    FILE *fp;
    char  path[128];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);
    if (access(path, F_OK) == 0) return 0;

    fp = fopen("/sys/class/gpio/export", "w");
    if (!fp) return -1;

    fprintf(fp, "%d", gpio);
    fclose(fp);

    for (int i = 0; i < 50; i++) {
        if (access(path, F_OK) == 0) return 0;
        usleep(20 * 1000);
    }
    return -1;
}

static int sysfs_write_int(int gpio, const char *attr, int value)
{
    char path[128];
    FILE *fp;

    if (gpio < 0 || attr == NULL || attr[0] == '\0') return -1;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/%s", gpio, attr);
    fp = fopen(path, "w");
    if (!fp) return -1;

    fprintf(fp, "%d", value);
    fclose(fp);
    return 0;
}

static int sysfs_write_str(int gpio, const char *attr, const char *value)
{
    char path[128];
    FILE *fp;

    if (gpio < 0 || attr == NULL || value == NULL) return -1;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/%s", gpio, attr);
    fp = fopen(path, "w");
    if (!fp) return -1;

    fprintf(fp, "%s", value);
    fclose(fp);
    return 0;
}

static int sysfs_read_int(int gpio, const char *attr)
{
    char path[128];
    char buf[32];
    FILE *fp;

    if (gpio < 0 || attr == NULL || attr[0] == '\0') return -1;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/%s", gpio, attr);
    fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return atoi(buf);
}

static int sysfs_led_init(int led_line, int button_line)
{
    int led_ok = 0;
    int btn_ok = 0;

    if (led_line >= 0) {
        if (sysfs_gpio_export(led_line) == 0 &&
            sysfs_write_str(led_line, "direction", "out") == 0 &&
            sysfs_write_int(led_line, "value", g_led_active_low ? 1 : 0) == 0) {
            led_ok = 1;
        } else {
            LOG_ERROR("sysfs LED init failed for line %d", led_line);
        }
    }

    if (button_line >= 0) {
        if (sysfs_gpio_export(button_line) == 0 &&
            sysfs_write_str(button_line, "direction", "in") == 0) {
            btn_ok = 1;
        } else {
            LOG_WARN("sysfs button init failed for line %d", button_line);
        }
    }

    g_button_ready = btn_ok;
    return (led_ok || btn_ok) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* chardev v2 request helpers                                          */
/* ------------------------------------------------------------------ */

static int chardev_v2_request_output(int chip_fd, int line, int active_low, int *fd_out)
{
    struct gpio_v2_line_request req;

    memset(&req, 0, sizeof(req));
    req.offsets[0] = (__u32)line;
    snprintf(req.consumer, sizeof(req.consumer), "%s", "opiz3-gateway-led");
    req.num_lines = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    if (active_low) req.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        LOG_WARN("GPIO v2 output request failed on line %d: %s",
                 line, strerror(errno));
        return -1;
    }
    *fd_out = req.fd;
    return 0;
}

static int chardev_v2_request_input(int chip_fd, int line, int active_low, int *fd_out)
{
    struct gpio_v2_line_request req;

    memset(&req, 0, sizeof(req));
    req.offsets[0] = (__u32)line;
    snprintf(req.consumer, sizeof(req.consumer), "%s", "opiz3-gateway-btn");
    req.num_lines = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
    if (active_low) {
        req.config.flags |= GPIO_V2_LINE_FLAG_ACTIVE_LOW;
        req.config.flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    }

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        LOG_WARN("GPIO v2 input request failed on line %d: %s",
                 line, strerror(errno));
        return -1;
    }
    *fd_out = req.fd;
    return 0;
}

static int gpio_chardev_v2_init(const app_config_t *cfg)
{
    char chip_path[64];
    int  fd;
    int  led_ok = 0;
    int  btn_ok = 0;

    snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%d", cfg->gpio_chip);
    fd = open(chip_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_WARN("cannot open %s for GPIO v2: %s", chip_path, strerror(errno));
        return -1;
    }

    if (cfg->led_line >= 0) {
        if (chardev_v2_request_output(fd, cfg->led_line, g_led_active_low, &g_led_fd) == 0) {
            led_ok = 1;
        }
    }

    if (cfg->button_line >= 0) {
        if (chardev_v2_request_input(fd, cfg->button_line, g_btn_active_low, &g_btn_fd) == 0) {
            btn_ok = 1;
        }
    }

    close(fd);

    if (!led_ok && !btn_ok) {
        if (g_led_fd >= 0) {
            close(g_led_fd);
            g_led_fd = -1;
        }
        if (g_btn_fd >= 0) {
            close(g_btn_fd);
            g_btn_fd = -1;
        }
        return -1;
    }

    g_button_ready = btn_ok;

    LOG_INFO("GPIO char-dev v2 backend ready: chip=%s led=%s button=%s",
             chip_path,
             led_ok ? "ok" : "unavailable",
             btn_ok ? "ok" : "unavailable");
    return 0;
}

/* ------------------------------------------------------------------ */
/* chardev request helpers (legacy v1, kept for older kernels)         */
/* ------------------------------------------------------------------ */

static int chardev_request_output(int chip_fd, int line, int active_low, int *fd_out)
{
    struct gpiohandle_request req;

    memset(&req, 0, sizeof(req));
    req.lineoffsets[0] = (__u32)line;
    req.flags = GPIOHANDLE_REQUEST_OUTPUT;
    if (active_low) req.flags |= GPIOHANDLE_REQUEST_ACTIVE_LOW;
    req.default_values[0] = 0;
    req.lines = 1;
    snprintf(req.consumer_label, sizeof(req.consumer_label), "%s", "opiz3-gateway-led");

    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        LOG_WARN("GPIO char-dev output request failed on line %d: %s",
                 line, strerror(errno));
        return -1;
    }
    *fd_out = req.fd;
    return 0;
}

static int chardev_request_input(int chip_fd, int line, int active_low, int *fd_out)
{
    struct gpiohandle_request req;

    memset(&req, 0, sizeof(req));
    req.lineoffsets[0] = (__u32)line;
    req.flags = GPIOHANDLE_REQUEST_INPUT;
    if (active_low) {
        req.flags |= GPIOHANDLE_REQUEST_ACTIVE_LOW;
        req.flags |= GPIOHANDLE_REQUEST_BIAS_PULL_UP;
    }
    req.lines = 1;
    snprintf(req.consumer_label, sizeof(req.consumer_label), "%s", "opiz3-gateway-btn");

    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        LOG_WARN("GPIO char-dev input request failed on line %d: %s",
                 line, strerror(errno));
        return -1;
    }
    *fd_out = req.fd;
    return 0;
}

static int gpio_chardev_init(const app_config_t *cfg)
{
    char chip_path[64];
    int  fd;
    int  led_ok = 0;
    int  btn_ok = 0;

    snprintf(chip_path, sizeof(chip_path), "/dev/gpiochip%d", cfg->gpio_chip);
    fd = open(chip_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOG_WARN("cannot open %s: %s", chip_path, strerror(errno));
        return -1;
    }

    if (cfg->led_line >= 0) {
        if (chardev_request_output(fd, cfg->led_line, g_led_active_low, &g_led_fd) == 0) {
            led_ok = 1;
        }
    }

    if (cfg->button_line >= 0) {
        if (chardev_request_input(fd, cfg->button_line, g_btn_active_low, &g_btn_fd) == 0) {
            btn_ok = 1;
        }
    }

    close(fd);

    if (!led_ok && !btn_ok) {
        if (g_led_fd >= 0) {
            close(g_led_fd);
            g_led_fd = -1;
        }
        if (g_btn_fd >= 0) {
            close(g_btn_fd);
            g_btn_fd = -1;
        }
        return -1;
    }

    g_button_ready = btn_ok;

    LOG_INFO("GPIO char-dev backend ready: chip=%s led=%s button=%s",
             chip_path,
             led_ok ? "ok" : "unavailable",
             btn_ok ? "ok" : "unavailable");
    return 0;
}

/* ------------------------------------------------------------------ */
/* button monitor thread                                               */
/* ------------------------------------------------------------------ */

static void *button_monitor_thread(void *arg)
{
    int last = -1;
    (void)arg;

    LOG_INFO("button monitor thread started");

    while (shared_is_running()) {
        int pressed = 0;

        if (gpio_dev_read_button(&pressed) == 0) {
            if (pressed != last) {
                last = pressed;
                if (g_button_cb) {
                    g_button_cb(pressed, g_button_user);
                }
            }
        }
        usleep(50 * 1000); /* 20 Hz polling */
    }

    LOG_INFO("button monitor thread exited");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* public                                                              */
/* ------------------------------------------------------------------ */

int gpio_dev_init(const app_config_t *cfg, gpio_button_cb_t button_cb, void *user)
{
    g_led_active_low = cfg->led_active_low;
    g_btn_active_low = cfg->button_active_low;
    g_led_line = cfg->led_line;
    g_button_line = cfg->button_line;
    g_button_cb = button_cb;
    g_button_user = user;

    if (cfg->simulate) {
        g_backend = GPIO_BACKEND_SIM;
        g_sim_led = 0;
        LOG_INFO("GPIO simulated backend ready");
        return 0;
    }

    g_backend = GPIO_BACKEND_NONE;
    g_button_ready = 0;

    if (gpio_chardev_init(cfg) == 0) {
        g_backend = GPIO_BACKEND_CHARDEV;
        g_use_v2 = 0;
    } else if (gpio_chardev_v2_init(cfg) == 0) {
        g_backend = GPIO_BACKEND_CHARDEV;
        g_use_v2 = 1;
    } else if (sysfs_led_init(cfg->led_line, cfg->button_line) == 0) {
        g_backend = GPIO_BACKEND_SYSFS;
        LOG_INFO("GPIO sysfs backend ready (legacy /sys/class/gpio)");
    } else {
        LOG_ERROR("GPIO backend init failed; GPIO functions disabled.");
        return -1;
    }

    if (cfg->button_line >= 0 && g_button_ready && g_button_cb) {
        int rc = pthread_create(&g_btn_thread, NULL, button_monitor_thread, NULL);
        if (rc == 0) {
            g_btn_thread_started = 1;
        } else {
            LOG_WARN("cannot create button monitor thread: %s", strerror(rc));
        }
    }
    return 0;
}

int gpio_dev_set_led(int value)
{
    int logical = value ? 1 : 0;

    switch (g_backend) {
    case GPIO_BACKEND_SIM:
        g_sim_led = logical;
        return 0;
    case GPIO_BACKEND_CHARDEV:
        if (g_led_fd < 0) return -1;
        if (g_use_v2) {
            struct gpio_v2_line_values vals;

            memset(&vals, 0, sizeof(vals));
            vals.mask = 1ULL;
            vals.bits = logical ? 1ULL : 0ULL;
            if (ioctl(g_led_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0) {
                LOG_ERROR("GPIO v2 set LED failed: %s", strerror(errno));
                return -1;
            }
            return 0;
        } else {
            struct gpiohandle_data data;
            memset(&data, 0, sizeof(data));
            data.values[0] = (__u8)logical;
            if (ioctl(g_led_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
                LOG_ERROR("GPIO set LED failed: %s", strerror(errno));
                return -1;
            }
            return 0;
        }
    case GPIO_BACKEND_SYSFS: {
        /* physical value inversion is handled by hardware config only in
         * chardev; on sysfs we invert manually before writing. */
        int val;
        if (g_led_line < 0) return -1;
        val = g_led_active_low ? (logical ? 0 : 1) : logical;
        return sysfs_write_int(g_led_line, "value", val);
    }
    default:
        LOG_ERROR("GPIO backend not available");
        return -1;
    }
}

int gpio_dev_get_led(void)
{
    switch (g_backend) {
    case GPIO_BACKEND_SIM:
        return g_sim_led;
    case GPIO_BACKEND_CHARDEV:
        if (g_led_fd < 0) return -1;
        if (g_use_v2) {
            struct gpio_v2_line_values vals;

            memset(&vals, 0, sizeof(vals));
            vals.mask = 1ULL;
            if (ioctl(g_led_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
                LOG_ERROR("GPIO v2 get LED failed: %s", strerror(errno));
                return -1;
            }
            return (vals.bits & 1ULL) ? 1 : 0;
        } else {
            struct gpiohandle_data data;
            memset(&data, 0, sizeof(data));
            if (ioctl(g_led_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0) {
                LOG_ERROR("GPIO get LED failed: %s", strerror(errno));
                return -1;
            }
            return data.values[0] ? 1 : 0;
        }
    case GPIO_BACKEND_SYSFS: {
        int val;
        if (g_led_line < 0) return -1;
        val = sysfs_read_int(g_led_line, "value");
        if (val < 0) return -1;
        if (g_led_active_low) val = val ? 0 : 1;
        return val ? 1 : 0;
    }
    default:
        return -1;
    }
}

int gpio_dev_read_button(int *pressed)
{
    if (!pressed) return -1;
    *pressed = 0;

    switch (g_backend) {
    case GPIO_BACKEND_SIM:
        *pressed = g_sim_button;
        return 0;
    case GPIO_BACKEND_CHARDEV:
        if (g_btn_fd < 0) return -1;
        if (g_use_v2) {
            struct gpio_v2_line_values vals;

            memset(&vals, 0, sizeof(vals));
            vals.mask = 1ULL;
            if (ioctl(g_btn_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
                LOG_ERROR("GPIO v2 read button failed: %s", strerror(errno));
                return -1;
            }
            *pressed = (vals.bits & 1ULL) ? 1 : 0;
            return 0;
        } else {
            struct gpiohandle_data data;
            memset(&data, 0, sizeof(data));
            if (ioctl(g_btn_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0) {
                LOG_ERROR("GPIO read button failed: %s", strerror(errno));
                return -1;
            }
            *pressed = data.values[0] ? 1 : 0;
            return 0;
        }
    case GPIO_BACKEND_SYSFS: {
        int val;
        if (g_button_line < 0) return -1;
        val = sysfs_read_int(g_button_line, "value");
        if (val < 0) return -1;
        val = val ? 1 : 0;
        *pressed = g_btn_active_low ? (val ? 0 : 1) : val;
        return 0;
    }
    default:
        return -1;
    }
}

int gpio_dev_available(void)
{
    return g_backend != GPIO_BACKEND_NONE;
}

void gpio_dev_close(void)
{
    if (g_btn_thread_started) {
        pthread_join(g_btn_thread, NULL);
        g_btn_thread_started = 0;
    }

    if (g_led_fd >= 0) { close(g_led_fd); g_led_fd = -1; }
    if (g_btn_fd >= 0) { close(g_btn_fd); g_btn_fd = -1; }
    g_backend = GPIO_BACKEND_NONE;
}
