#include "tlsutil.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

/* ── low-level byte I/O ──────────────────────────────────────────────────── */

static ssize_t lc_read1(LCConn *c, char *ch) {
    if (c->ssl) {
        int n = SSL_read(c->ssl, ch, 1);
        if (n == 1) return 1;
        int err = SSL_get_error(c->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;  /* clean close */
        if (err == SSL_ERROR_SYSCALL && errno == 0)  return 0;
        return -1;
    }
    for (;;) {
        ssize_t n = read(c->fd, ch, 1);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        return -1;
    }
}

static int lc_write_all(LCConn *c, const char *data, size_t len) {
    if (c->ssl) {
        while (len > 0) {
            int w = SSL_write(c->ssl, data, (int)len);
            if (w <= 0) return -1;
            data += w;
            len  -= (size_t)w;
        }
        return 0;
    }
    while (len > 0) {
        ssize_t w = write(c->fd, data, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        data += w;
        len  -= (size_t)w;
    }
    return 0;
}

/* ── public API ──────────────────────────────────────────────────────────── */

ssize_t lc_readline(LCConn *c, char *buf, size_t maxlen) {
    size_t n = 0;
    char ch;
    ssize_t rc;

    if (maxlen == 0) return -1;
    while (n < maxlen - 1) {
        rc = lc_read1(c, &ch);
        if (rc == 1) {
            if (ch == '\n') break;
            if (ch == '\r') continue;
            buf[n++] = ch;
        } else if (rc == 0) {
            if (n == 0) return 0;  /* EOF with no data */
            break;
        } else {
            return -1;
        }
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

int lc_sendline(LCConn *c, const char *line) {
    size_t len = strlen(line);
    char buf[MAX_LINE + 1];
    if (len >= sizeof(buf) - 1) len = sizeof(buf) - 2;
    memcpy(buf, line, len);
    buf[len]     = '\n';
    buf[len + 1] = '\0';
    return lc_write_all(c, buf, len + 1);
}

int lc_sendlinef(LCConn *c, const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return lc_sendline(c, buf);
}

int lc_drain_line(LCConn *c) {
    char ch;
    for (;;) {
        ssize_t rc = lc_read1(c, &ch);
        if (rc < 0) { if (errno == EINTR) continue; return -1; }
        if (rc == 0) return -1;
        if (ch == '\n') return 0;
    }
}

void lc_close(LCConn *c) {
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

/* ── TLS context helpers ──────────────────────────────────────────────────── */

SSL_CTX *tls_server_ctx(const char *certfile, const char *keyfile) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { ERR_print_errors_fp(stderr); return NULL; }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_file(ctx, certfile, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "TLS: cannot load cert '%s'\n", certfile);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx); return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "TLS: cannot load key '%s'\n", keyfile);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx); return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        fprintf(stderr, "TLS: cert/key mismatch\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx); return NULL;
    }
    return ctx;
}

SSL_CTX *tls_client_ctx(const char *cafile, int insecure) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { ERR_print_errors_fp(stderr); return NULL; }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        int ok = cafile ? SSL_CTX_load_verify_locations(ctx, cafile, NULL)
                        : SSL_CTX_set_default_verify_paths(ctx);
        if (ok != 1) {
            fprintf(stderr, "TLS: cannot load trust store%s%s\n",
                    cafile ? " " : "", cafile ? cafile : "");
            ERR_print_errors_fp(stderr);
            SSL_CTX_free(ctx);
            return NULL;
        }
    }

    return ctx;
}
