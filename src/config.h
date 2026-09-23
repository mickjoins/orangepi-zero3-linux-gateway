#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_MAX_PATH  256

typedef struct {
    /* http server */
    int  http_port;
    char http_bind_ip[64];
    char http_access_token[128];

    /* gpio */
    int  gpio_chip;
    int  led_line;
    int  button_line;
    int  led_active_low;
    int  button_active_low;

    /* i2c */
    char i2c_bus[CONFIG_MAX_PATH];
    char i2c_sensor_type[32];

    /* uart */
    char uart_dev[CONFIG_MAX_PATH];
    int  uart_baud;

    /* behavior */
    int  collect_interval_s;   /* >= 1 */

    /* runtime flags */
    int  simulate;

    /* logging */
    char log_file[CONFIG_MAX_PATH];
} app_config_t;

extern app_config_t g_config;

void config_set_defaults(app_config_t *cfg);
int  config_load(app_config_t *cfg, const char *path);
void config_print(const app_config_t *cfg);

#endif /* CONFIG_H */
