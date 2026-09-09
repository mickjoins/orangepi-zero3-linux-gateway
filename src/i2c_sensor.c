#include "i2c_sensor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#include "log.h"

/* Lightweight SMBus byte-read transaction implemented with the raw
 * kernel I2C_SMBUS ioctl (no external libi2c dependency). */
static int i2c_smbus_read_byte_ioctl(int fd)
{
    struct i2c_smbus_ioctl_data args;
    union i2c_smbus_data data;

    args.read_write = I2C_SMBUS_READ;
    args.command = 0;
    args.size = I2C_SMBUS_BYTE;
    args.data = &data;

    if (ioctl(fd, I2C_SMBUS, &args) < 0) return -1;
    return data.byte & 0xff;
}

static int i2c_open(const char *bus, int addr)
{
    int fd = open(bus, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("cannot open i2c bus %s: %s", bus, strerror(errno));
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        LOG_ERROR("cannot select i2c addr 0x%02x on %s: %s",
                  addr, bus, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int i2c_bus_scan(const char *bus, int *addr_list, int max_addrs)
{
    int fd;
    int found = 0;
    int addr;

    if (!addr_list || max_addrs <= 0) return -1;
    if (!bus || bus[0] == '\0') return -1;

    fd = open(bus, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("cannot open i2c bus %s: %s", bus, strerror(errno));
        return -1;
    }

    for (addr = 0x03; addr <= 0x77; addr++) {
        int res;

        if (ioctl(fd, I2C_SLAVE, addr) < 0) continue;

        /* SMBus quick read is a very light bus transaction. */
        res = i2c_smbus_read_byte_ioctl(fd);
        if (res >= 0) {
            if (found < max_addrs) {
                addr_list[found] = addr;
            }
            found++;
        }
    }

    close(fd);
    return found;
}

int i2c_aht20_read(const char *bus, sensor_sample_t *out)
{
    unsigned char cmd_init[1]  = { 0xBE };
    unsigned char cmd_trig[3]  = { 0xAC, 0x33, 0x00 };
    unsigned char data[7];
    int  fd;
    int  retries;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    fd = i2c_open(bus, 0x38);
    if (fd < 0) return -1;

    /* AHT20 initialization (safe to send even if already calibrated). */
    if (write(fd, cmd_init, sizeof(cmd_init)) != (ssize_t)sizeof(cmd_init)) {
        LOG_WARN("AHT20 init write failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    usleep(50 * 1000);

    /* Trigger a measurement. */
    if (write(fd, cmd_trig, sizeof(cmd_trig)) != (ssize_t)sizeof(cmd_trig)) {
        LOG_WARN("AHT20 trigger write failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    usleep(80 * 1000);

    for (retries = 0; retries < 5; retries++) {
        memset(data, 0, sizeof(data));
        if (read(fd, data, sizeof(data)) == (ssize_t)sizeof(data)) {
            if ((data[0] & 0x80) == 0) break; /* not busy */
        }
        usleep(50 * 1000);
    }

    close(fd);

    if (retries >= 5) {
        LOG_WARN("AHT20 read timeout (busy flag still set)");
        return -1;
    }
    if ((data[0] & 0x80) != 0) {
        LOG_WARN("AHT20 busy after read");
        return -1;
    }

    /* Datasheet formulas:
     * RH%  = (raw_humidity / 2^20) * 100
     * T(C) = (raw_temperature / 2^20) * 200 - 50
     */
    unsigned int raw_h = ((unsigned int)data[1] << 12) |
                         ((unsigned int)data[2] << 4)  |
                         ((unsigned int)data[3] >> 4);
    unsigned int raw_t = (((unsigned int)data[3] & 0x0F) << 16) |
                         ((unsigned int)data[4] << 8)  |
                         ((unsigned int)data[5]);

    out->humidity_pct = (double)raw_h * 100.0 / 1048576.0;
    out->temperature_c = (double)raw_t * 200.0 / 1048576.0 - 50.0;
    out->valid = 1;
    out->timestamp = time(NULL);

    LOG_DEBUG("AHT20 raw h=0x%06x t=0x%06x -> %.2f C, %.2f %%RH",
              raw_h, raw_t, out->temperature_c, out->humidity_pct);
    return 0;
}
