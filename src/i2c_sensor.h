#ifndef I2C_SENSOR_H
#define I2C_SENSOR_H

#include "shared.h"

/* Scan an I2C bus for responding 7-bit addresses (0x03..0x77).
 * Returns the number of addresses found, -1 on error. */
int i2c_bus_scan(const char *bus, int *addr_list, int max_addrs);

/* Read an AHT20/AHT21 temperature & humidity sensor.
 * Returns 0 on success. */
int i2c_aht20_read(const char *bus, sensor_sample_t *out);

#endif /* I2C_SENSOR_H */
