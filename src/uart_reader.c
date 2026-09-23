#include "uart_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
    case 460800:  return B460800;
    case 921600:  return B921600;
    default:      return B115200;
    }
}

void uart_handle_init(uart_handle_t *u, const char *device, int baud, int simulate)
{
    memset(u, 0, sizeof(*u));
    u->fd = -1;
    u->baud = baud;
    u->simulate = simulate;
    if (device) {
        snprintf(u->device, sizeof(u->device), "%s", device);
    }
    u->baud_const = baud_to_speed(baud);
}

int uart_open(uart_handle_t *u)
{
    struct termios tio;

    if (!u || u->simulate) return -1;

    u->fd = open(u->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (u->fd < 0) {
        LOG_WARN("cannot open serial port %s: %s", u->device, strerror(errno));
        return -1;
    }

    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(u->fd, &tio) != 0) {
        LOG_WARN("tcgetattr(%s) failed: %s", u->device, strerror(errno));
        close(u->fd);
        u->fd = -1;
        return -1;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, u->baud_const);
    cfsetospeed(&tio, u->baud_const);

    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;

    /* VMIN=1, VTIME=5 gives the tty a soft inter-byte timeout.
     * uart_read_line() itself uses poll() so shutdown stays responsive. */
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 5;

    if (tcsetattr(u->fd, TCSANOW, &tio) != 0) {
        LOG_WARN("tcsetattr(%s) failed: %s", u->device, strerror(errno));
        close(u->fd);
        u->fd = -1;
        return -1;
    }

    /* The port is opened non-blocking only to avoid a blocking open()
     * before CLOCAL is set. Now switch it back to blocking; read() calls
     * are still guarded by poll() in uart_read_line(). */
    {
        int flags = fcntl(u->fd, F_GETFL, 0);
        if (flags < 0 || fcntl(u->fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
            LOG_WARN("cannot set serial port %s to blocking mode: %s",
                     u->device, strerror(errno));
            close(u->fd);
            u->fd = -1;
            return -1;
        }
    }

    /* clear any stale data */
    tcflush(u->fd, TCIOFLUSH);
    LOG_INFO("serial port %s opened at %d baud", u->device, u->baud);
    return 0;
}

static long long monotonic_millis(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int uart_read_line(uart_handle_t *u, char *buf, int buf_size)
{
    char ch;
    long long deadline;

    if (!u || !buf || buf_size < 2 || u->fd < 0) return -1;
    deadline = monotonic_millis();
    if (deadline < 0) return -1;
    deadline += 200;

    for (;;) {
        struct pollfd pfd;
        int ready;
        long long now = monotonic_millis();
        long long remaining;

        if (now < 0) return -1;
        remaining = deadline - now;
        if (remaining <= 0) return 0;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = u->fd;
        pfd.events = POLLIN;

        /* Bound each call so the owner can observe the shutdown flag. */
        ready = poll(&pfd, 1, (int)remaining);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) return 0;

        if (pfd.revents & (POLLERR | POLLNVAL)) {
            return -1;
        }
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & POLLHUP) return -1;
            continue;
        }

        {
            ssize_t n = read(u->fd, &ch, 1);
            if (n == 1) {
                if (ch == '\n') {
                    int length = u->line_len;
                    if (u->discard_line) {
                        u->discard_line = 0;
                        u->line_len = 0;
                        return 0;
                    }
                    if (length > 0 && u->line_buf[length - 1] == '\r') length--;
                    u->line_len = 0;
                    if (length == 0) return 0;
                    if (length >= buf_size) {
                        LOG_WARN("serial line exceeds output buffer; discarded");
                        return 0;
                    }
                    memcpy(buf, u->line_buf, (size_t)length);
                    buf[length] = '\0';
                    return length;
                }
                if (u->discard_line) continue;
                if (ch == '\0' || u->line_len >= UART_MAX_LINE - 1) {
                    LOG_WARN("serial line contains NUL or exceeds %d bytes; discarded",
                             UART_MAX_LINE - 1);
                    u->discard_line = 1;
                    u->line_len = 0;
                    continue;
                }
                u->line_buf[u->line_len++] = ch;
            } else if (n == 0) {
                return -1;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (errno == EINTR) continue;
                return -1;
            }
        }
    }
}

void uart_close(uart_handle_t *u)
{
    if (!u) return;
    if (u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
}
