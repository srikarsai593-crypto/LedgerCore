#ifndef NETUTIL_H
#define NETUTIL_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Read one '\n'-terminated line from fd into buf (NUL-terminated, newline stripped).
 * Returns number of bytes placed in buf (excluding NUL), 0 on EOF, -1 on error.
 */
ssize_t readline_fd(int fd, char *buf, size_t maxlen);

/*
 * Write a NUL-terminated string followed by '\n' to fd.
 * Returns 0 on success, -1 on error.
 */
int sendline_fd(int fd, const char *line);

/*
 * Printf-style sendline: formats into a local buffer then sends.
 * Returns 0 on success, -1 on error.
 */
int sendlinef(int fd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Consume bytes from fd until a '\n' is found (or error/EOF).
 * Use this after readline_fd fills its buffer without finding a newline,
 * to discard the oversized line before reading the next command.
 * Returns 0 on success, -1 on error/EOF.
 */
int drain_line_fd(int fd);

#endif /* NETUTIL_H */
