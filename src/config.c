#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

app_config_t g_config;

void config_set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->http_port = 8080;
    snprintf(cfg->http_bind_ip, sizeof(cfg->http_bind_ip), "%s", "0.0.0.0");

    cfg->gpio_chip = 0;
    cfg->led_line = 69;       /* Allwinner H618 PC5 on many Orange Pi Zero 3 images */
    cfg->button_line = 70;    /* Optional; change to 74/75 etc. see README */
    cfg->led_active_low = 0;
    cfg->button_active_low = 1;   /* most buttons connect GPIO to GND */

    snprintf(cfg->i2c_bus, sizeof(cfg->i2c_bus), "%s", "/dev/i2c-1");
    snprintf(cfg->i2c_sensor_type, sizeof(cfg->i2c_sensor_type), "%s", "aht20");

    snprintf(cfg->uart_dev, sizeof(cfg->uart_dev), "%s", "/dev/ttyS1");
    cfg->uart_baud = 115200;

    cfg->collect_interval_s = 2;
    cfg->simulate = 0;

    cfg->log_file[0] = '\0';
}

static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static int parse_key_value(app_config_t *cfg, const char *key, const char *value)
{
    if (strcmp(key, "http_port") == 0) {
        cfg->http_port = atoi(value);
        if (cfg->http_port <= 0 || cfg->http_port > 65535) cfg->http_port = 8080;
    } else if (strcmp(key, "http_bind_ip") == 0) {
        snprintf(cfg->http_bind_ip, sizeof(cfg->http_bind_ip), "%s", value);
    } else if (strcmp(key, "gpio_chip") == 0) {
        cfg->gpio_chip = atoi(value);
    } else if (strcmp(key, "led_line") == 0) {
        cfg->led_line = atoi(value);
    } else if (strcmp(key, "button_line") == 0) {
        cfg->button_line = atoi(value);
    } else if (strcmp(key, "led_active_low") == 0) {
        cfg->led_active_low = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "button_active_low") == 0) {
        cfg->button_active_low = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "i2c_bus") == 0) {
        snprintf(cfg->i2c_bus, sizeof(cfg->i2c_bus), "%s", value);
    } else if (strcmp(key, "i2c_sensor_type") == 0) {
        snprintf(cfg->i2c_sensor_type, sizeof(cfg->i2c_sensor_type), "%s", value);
    } else if (strcmp(key, "uart_dev") == 0) {
        snprintf(cfg->uart_dev, sizeof(cfg->uart_dev), "%s", value);
    } else if (strcmp(key, "uart_baud") == 0) {
        cfg->uart_baud = atoi(value);
        if (cfg->uart_baud <= 0) cfg->uart_baud = 115200;
    } else if (strcmp(key, "collect_interval_s") == 0) {
        cfg->collect_interval_s = atoi(value);
        if (cfg->collect_interval_s < 1) cfg->collect_interval_s = 2;
    } else if (strcmp(key, "simulate") == 0) {
        cfg->simulate = atoi(value) ? 1 : 0;
    } else if (strcmp(key, "log_file") == 0) {
        snprintf(cfg->log_file, sizeof(cfg->log_file), "%s", value);
    } else {
        return -1; /* unknown key */
    }
    return 0;
}

int config_load(app_config_t *cfg, const char *path)
{
    FILE *fp;
    char  line[512];
    int   lineno = 0;

    fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("cannot open config file: %s", path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p;
        char *key;
        char *value;
        char *section_tmp;

        lineno++;
        p = trim(line);

        /* blank line or comment */
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        /* section lines such as [gpio] are accepted but ignored */
        if (*p == '[') continue;

        key = p;
        value = strchr(p, '=');
        if (!value) {
            LOG_WARN("config line %d ignored (missing '='): %s", lineno, line);
            continue;
        }

        *value = '\0';
        value = trim(value + 1);

        section_tmp = strchr(key, ']');
        (void)section_tmp;
        key = trim(key);

        if (parse_key_value(cfg, key, value) != 0) {
            LOG_WARN("unknown config key line %d: %s", lineno, key);
        }
    }

    fclose(fp);
    return 0;
}

void config_print(const app_config_t *cfg)
{
    LOG_INFO("HTTP  : %s:%d", cfg->http_bind_ip, cfg->http_port);
    LOG_INFO("GPIO  : chip=%d led_line=%d button_line=%d active_low(led/btn)=%d/%d",
             cfg->gpio_chip, cfg->led_line, cfg->button_line,
             cfg->led_active_low, cfg->button_active_low);
    LOG_INFO("I2C   : bus=%s sensor=%s", cfg->i2c_bus, cfg->i2c_sensor_type);
    LOG_INFO("UART  : dev=%s baud=%d", cfg->uart_dev, cfg->uart_baud);
    LOG_INFO("Engine: collect_interval_s=%d simulate=%d", cfg->collect_interval_s, cfg->simulate);
}
