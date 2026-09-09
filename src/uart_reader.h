#ifndef UART_READER_H
#define UART_READER_H

#include <termios.h>

typedef struct {
    int      fd;
    char     device[256];
    int      baud;
    int      simulate;
    speed_t  baud_const;
} uart_handle_t;

void uart_handle_init(uart_handle_t *u, const char *device, int baud, int simulate);

/* Open and configure the serial port. Returns 0 on success. */
int uart_open(uart_handle_t *u);

/* Read one complete line (terminated by '\n'). At most one second timeout.
 * Returns: length of line without terminator on success,
 *          0 on timeout, -1 on error. */
int uart_read_line(uart_handle_t *u, char *buf, int buf_size);

void uart_close(uart_handle_t *u);

#endif /* UART_READER_H */
