#include "netutil.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

ssize_t readline_fd(int fd, char *buf, size_t maxlen) {
    size_t n = 0;
    char c;
    ssize_t rc;

    if (maxlen == 0) return -1;

    while (n < maxlen - 1) {
        rc = read(fd, &c, 1);
        if (rc == 1) {
            if (c == '\n') break;
            if (c == '\r') continue;   /* tolerate CRLF */
            buf[n++] = c;
        } else if (rc == 0) {
            /* EOF */
            if (n == 0) return 0;
            break;
        } else {
            if (errno == EINTR) continue;
            return -1;
        }
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

int sendline_fd(int fd, const char *line) {
    size_t len = strlen(line);
    /* Write line content */
    const char *p = line;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t w = write(fd, p, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        remaining -= (size_t)w;
    }
    /* Write newline */
    char nl = '\n';
    while (write(fd, &nl, 1) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

int drain_line_fd(int fd) {
    char c;
    for (;;) {
        ssize_t rc = read(fd, &c, 1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) return -1;   /* EOF */
        if (c == '\n') return 0;
    }
}

int sendlinef(int fd, const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return sendline_fd(fd, buf);
}
