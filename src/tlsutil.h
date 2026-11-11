#ifndef TLSUTIL_H
#define TLSUTIL_H

#include <stddef.h>
#include <sys/types.h>
#include <openssl/ssl.h>

/*
 * LCConn: a thin abstraction over a file descriptor or an SSL* session.
 * Set ssl to NULL for plaintext; all lc_* functions dispatch accordingly.
 */
typedef struct {
    int  fd;
    SSL *ssl;   /* NULL = plaintext, non-NULL = TLS */
} LCConn;

/*
 * Read one '\n'-terminated line into buf (NUL-terminated, newline stripped).
 * Returns number of bytes placed in buf, 0 on EOF, -1 on error.
 */
ssize_t lc_readline(LCConn *c, char *buf, size_t maxlen);

/* Write a NUL-terminated string followed by '\n'. Returns 0 / -1. */
int lc_sendline(LCConn *c, const char *line);

/* Printf-style sendline. Returns 0 / -1. */
int lc_sendlinef(LCConn *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Drain bytes until '\n' (for oversized lines). Returns 0 / -1. */
int lc_drain_line(LCConn *c);

/* SSL_shutdown (if TLS) + close(fd). Safe to call with ssl==NULL. */
void lc_close(LCConn *c);

/* ── TLS context helpers ──────────────────────────────────────────────────── */

/*
 * Create a server SSL_CTX from PEM cert + key files.
 * Returns NULL on failure (error printed to stderr).
 */
SSL_CTX *tls_server_ctx(const char *certfile, const char *keyfile);

/*
 * Create a client SSL_CTX. Verification is on unless insecure is nonzero.
 * cafile may be NULL to use the system trust store.
 */
SSL_CTX *tls_client_ctx(const char *cafile, int insecure);

#endif /* TLSUTIL_H */
