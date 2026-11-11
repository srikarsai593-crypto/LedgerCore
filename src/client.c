/*
 * client.c – LedgerCore interactive command-line client
 *
 * Usage: ledgercore_client [host [port]] [--tls] [--cafile PATH] [--insecure]
 *
 *   --tls      connect with TLS and verify the server certificate
 *   --cafile   trust certificates from PATH instead of system defaults
 *   --insecure disable certificate verification for local testing
 *
 * Connects to the server, then enters a read-eval loop:
 *   - read a line from stdin
 *   - send it to the server
 *   - print every response line until a line starting with OK/ERR/END
 */

#include "protocol.h"
#include "tlsutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/ssl.h>

#define DEFAULT_HOST "127.0.0.1"

static int connect_to_server(const char *host, int port) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "client: cannot resolve host '%s'\n", host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[]) {
    const char *host  = DEFAULT_HOST;
    int         port  = DEFAULT_PORT;
    int         use_tls = 0;
    int         insecure = 0;
    const char *cafile = NULL;

    /* Parse positional arguments and --tls in any order. */
    int positionals = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tls") == 0) {
            use_tls = 1;
        } else if (strcmp(argv[i], "--cafile") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--cafile requires a path\n");
                return 1;
            }
            cafile = argv[i];
            use_tls = 1;
        } else if (strcmp(argv[i], "--insecure") == 0) {
            insecure = 1;
            use_tls = 1;
        } else if (positionals++ == 0) {
            host = argv[i];
        } else if (positionals == 2) {
            char *end = NULL;
            long parsed = strtol(argv[i], &end, 10);
            if (!end || *end != '\0' || parsed < 1 || parsed > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (int)parsed;
        } else {
            fprintf(stderr, "Usage: %s [host [port]] [--tls] "
                            "[--cafile PATH] [--insecure]\n", argv[0]);
            return 1;
        }
    }
    if (insecure && cafile) {
        fprintf(stderr, "--insecure cannot be combined with --cafile\n");
        return 1;
    }

    int fd = connect_to_server(host, port);
    if (fd < 0) return 1;

    LCConn vc;
    vc.fd  = fd;
    vc.ssl = NULL;

    if (use_tls) {
        if (insecure)
            fprintf(stderr, "WARNING: TLS certificate verification disabled\n");
        SSL_CTX *ctx = tls_client_ctx(cafile, insecure);
        if (!ctx) { close(fd); return 1; }
        SSL *ssl = SSL_new(ctx);
        SSL_CTX_free(ctx);
        if (!ssl) { close(fd); return 1; }
        struct in_addr numeric_host;
        if (SSL_set_fd(ssl, fd) != 1 ||
            (inet_pton(AF_INET, host, &numeric_host) != 1 &&
             SSL_set_tlsext_host_name(ssl, host) != 1) ||
            (!insecure && SSL_set1_host(ssl, host) != 1)) {
            fprintf(stderr, "TLS setup failed\n");
            SSL_free(ssl);
            close(fd);
            return 1;
        }
        if (SSL_connect(ssl) <= 0) {
            fprintf(stderr, "TLS handshake or certificate verification failed\n");
            SSL_free(ssl);
            close(fd);
            return 1;
        }
        if (!insecure && SSL_get_verify_result(ssl) != X509_V_OK) {
            fprintf(stderr, "TLS certificate verification failed\n");
            SSL_free(ssl);
            close(fd);
            return 1;
        }
        vc.ssl = ssl;
        fprintf(stdout, "TLS connected to %s:%d\n", host, port);
    } else {
        fprintf(stdout, "Connected to %s:%d  (type HELP for commands, QUIT to exit)\n",
                host, port);
    }

    char line[MAX_LINE];
    ssize_t n;

    /* Print the server's initial greeting */
    n = lc_readline(&vc, line, sizeof(line));
    if (n <= 0) { fprintf(stderr, "server closed immediately\n"); lc_close(&vc); return 1; }
    printf("< %s\n", line);

    /* Interactive loop */
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        if (lc_sendline(&vc, line) < 0) {
            perror("lc_sendline");
            break;
        }

        int quitting = (strcasecmp(line, "QUIT") == 0);
        int history  = (strcasecmp(line, "HISTORY") == 0);

        for (;;) {
            n = lc_readline(&vc, line, sizeof(line));
            if (n < 0) { perror("lc_readline"); goto done; }
            if (n == 0) { printf("(server closed connection)\n"); goto done; }

            printf("< %s\n", line);

            if (strncmp(line, "END", 3) == 0) break;
            if (strncmp(line, "ERR", 3) == 0 ||
                (!history && strncmp(line, "OK", 2) == 0)) break;
        }

        if (quitting) break;
    }

done:
    lc_close(&vc);
    return 0;
}
