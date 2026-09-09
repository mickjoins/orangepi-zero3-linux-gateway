#ifndef GPIO_DEV_H
#define GPIO_DEV_H

#include "config.h"

/* Called in button monitor thread context whenever the button is polled:
 * pressed == 1 edge, pressed == 0 edge. Keep the callback short. */
typedef void (*gpio_button_cb_t)(int pressed, void *user);

/* Initialize LED output and optional button input.
 * In simulate mode this initializes an in-memory fake GPIO without hardware. */
int gpio_dev_init(const app_config_t *cfg, gpio_button_cb_t button_cb, void *user);

/* Set / get LED logical state (0=off, 1=on). active-low is handled inside. */
int gpio_dev_set_led(int value);
int gpio_dev_get_led(void);

/* Read the logical button state into *pressed. Returns 0 on success. */
int gpio_dev_read_button(int *pressed);

/* 1 if real/simulated GPIO backend is usable. */
int gpio_dev_available(void);

void gpio_dev_close(void);

#endif /* GPIO_DEV_H */
