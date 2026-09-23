#ifndef UART_READER_H
#define UART_READER_H

#include <termios.h>

#define UART_MAX_LINE 512

typedef struct {
    int      fd;
    char     device[256];
    int      baud;
    int      simulate;
    speed_t  baud_const;
    char     line_buf[UART_MAX_LINE];
    int      line_len;
    int      discard_line;
} uart_handle_t;

void uart_handle_init(uart_handle_t *u, const char *device, int baud, int simulate);

/* Open and configure the serial port. Returns 0 on success. */
int uart_open(uart_handle_t *u);

/* Read one complete line (terminated by '\n'). Partial lines are retained
 * across calls; an overlong line is discarded through its newline.
 * Returns: positive length for a complete nonempty line,
 *          0 when no complete line is available, -1 on error or hangup. */
int uart_read_line(uart_handle_t *u, char *buf, int buf_size);

void uart_close(uart_handle_t *u);

#endif /* UART_READER_H */
