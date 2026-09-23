#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "uart_reader.h"

static void write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        assert(n > 0);
        offset += (size_t)n;
    }
}

int main(void)
{
    uart_handle_t uart;
    char line[UART_MAX_LINE];
    char oversized[UART_MAX_LINE + 100];
    int fds[2];
    int n;

    assert(pipe(fds) == 0);
    uart_handle_init(&uart, "test pipe", 115200, 0);
    uart.fd = fds[0];

    write_all(fds[1], "first", 5);
    assert(uart_read_line(&uart, line, sizeof(line)) == 0);
    write_all(fds[1], " line\r\n", 7);
    n = uart_read_line(&uart, line, sizeof(line));
    assert(n == 10 && strcmp(line, "first line") == 0);

    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = '\n';
    write_all(fds[1], oversized, sizeof(oversized));
    write_all(fds[1], "next\n", 5);
    for (int attempt = 0; attempt < 20; attempt++) {
        n = uart_read_line(&uart, line, sizeof(line));
        assert(n == 0 || (n == 4 && strcmp(line, "next") == 0));
        if (n == 4) break;
    }
    assert(n == 4);

    write_all(fds[1], "incomplete", 10);
    assert(uart_read_line(&uart, line, sizeof(line)) == 0);
    close(fds[1]);
    assert(uart_read_line(&uart, line, sizeof(line)) == -1);
    uart_close(&uart);

    puts("UART line framing tests passed");
    return 0;
}
