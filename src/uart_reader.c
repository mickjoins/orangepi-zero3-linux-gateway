#include "uart_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
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

int uart_read_line(uart_handle_t *u, char *buf, int buf_size)
{
    int  pos = 0;
    char ch;

    if (!u || !buf || buf_size < 2 || u->fd < 0) return -1;

    while (pos < buf_size - 1) {
        struct pollfd pfd;
        int ready;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = u->fd;
        pfd.events = POLLIN;

        /* 200 ms timeout: makes shutdown and partial-line handling responsive
         * without relying on VMIN/VTIME semantics of the underlying tty. */
        ready = poll(&pfd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1; /* real error */
        }
        if (ready == 0) {
            if (pos > 0) {
                buf[pos] = '\0';
                return pos; /* partial line on timeout */
            }
            return 0;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (pos > 0) {
                buf[pos] = '\0';
                return pos; /* partial line on hangup */
            }
            return -1;
        }

        {
            ssize_t n = read(u->fd, &ch, 1);
            if (n == 1) {
                if (ch == '\n') {
                    /* strip optional trailing '\r' */
                    if (pos > 0 && buf[pos - 1] == '\r') pos--;
                    buf[pos] = '\0';
                    return pos;
                }
                buf[pos++] = ch;
            } else if (n == 0) {
                break; /* EOF */
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (errno == EINTR) continue;
                return -1; /* real error */
            }
        }
    }

    if (pos > 0) {
        buf[pos] = '\0';
        return pos; /* partial line */
    }
    return 0;
}

void uart_close(uart_handle_t *u)
{
    if (!u) return;
    if (u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
}
