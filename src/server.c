/*
 * server.c – LedgerCore TCP server
 *
 * Architecture:
 *   Main thread  →  accept loop  →  job queue  →  64 worker threads
 *   Session state (logged-in user, failed-login counter) lives per-worker.
 *
 * TLS:  pass cert and key file paths as argv[2] and argv[3] to enable TLS.
 *       Without them the server runs in plaintext mode (useful for tests).
 *
 * Shutdown: SIGINT/SIGTERM → stop accepting → drain pool → flush logs.
 */

#include "protocol.h"
#include "netutil.h"
#include "tlsutil.h"
#include "accounts.h"
#include "wal.h"
#include "audit.h"
#include "threadpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <inttypes.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif

/* ── globals ─────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running     = 1;
static int                   g_listen_fd   = -1;
static int                   g_lockfile_fd = -1;
static ThreadPool            g_pool;
static SSL_CTX              *g_ssl_ctx     = NULL; /* NULL = plaintext mode */
/* Self-pipe: signal handler writes 1 byte; accept loop reads it to wake up */
static int                   g_pipe[2]     = {-1, -1};

/* ── startup lockfile ────────────────────────────────────────────────────── */

static int acquire_lockfile(void) {
    g_lockfile_fd = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (g_lockfile_fd < 0) { perror("lockfile: open"); return -1; }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;

    if (fcntl(g_lockfile_fd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN)
            fprintf(stderr, "LedgerCore: another instance is already running "
                            "(lock held on %s)\n", LOCK_FILE);
        else
            perror("lockfile: fcntl");
        close(g_lockfile_fd);
        g_lockfile_fd = -1;
        return -1;
    }
    if (ftruncate(g_lockfile_fd, 0) < 0 ||
        dprintf(g_lockfile_fd, "%ld\n", (long)getpid()) < 0) {
        perror("lockfile: write pid");
        close(g_lockfile_fd);
        g_lockfile_fd = -1;
        return -1;
    }
    return 0;
}

static void release_lockfile(void) {
    if (g_lockfile_fd >= 0) {
        close(g_lockfile_fd);
        g_lockfile_fd = -1;
    }
}

/* ── signal handler ──────────────────────────────────────────────────────── */

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
    if (g_pipe[1] >= 0) {
        char b = 1;
        ssize_t sent = write(g_pipe[1], &b, 1);
        (void)sent;
    } else if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

/* ── amount helpers ──────────────────────────────────────────────────────── */

static int parse_amount(const char *str, int64_t *out_paise) {
    if (!str || *str == '\0') return 0;
    if (*str == '-' || *str == '+') return 0;

    const char *dot     = strchr(str, '.');
    const char *int_end = dot ? dot : (str + strlen(str));

    if (int_end == str) return 0;

    int64_t rupees = 0;
    for (const char *p = str; p < int_end; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
        if (rupees > (INT64_MAX / 10 - 1)) return 0;
        rupees = rupees * 10 + (*p - '0');
    }

    int64_t cents = 0;
    if (dot) {
        const char *frac = dot + 1;
        size_t frac_len = strlen(frac);
        if (frac_len == 0 || frac_len > 2) return 0;
        for (size_t i = 0; i < frac_len; i++) {
            if (!isdigit((unsigned char)frac[i])) return 0;
        }
        cents = (frac_len == 1)
                    ? (int64_t)(frac[0] - '0') * 10
                    : (int64_t)(frac[0] - '0') * 10 + (frac[1] - '0');
    }

    if (rupees > (INT64_MAX - cents) / 100) return 0;
    int64_t paise = rupees * 100 + cents;
    if (paise <= 0) return 0;

    *out_paise = paise;
    return 1;
}

static void fmt_amount(char *buf, size_t len, int64_t paise) {
    const char *sign = "";
    if (paise < 0) { sign = "-"; paise = -paise; }
    snprintf(buf, len, "%s%" PRId64 ".%02" PRId64,
             sign, paise / 100, paise % 100);
}

/* ── input validation ────────────────────────────────────────────────────── */

static int valid_username(const char *u) {
    size_t len = strlen(u);
    if (len < MIN_USERNAME_LEN || len > MAX_USERNAME_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = u[i];
        if (!isalnum((unsigned char)c) && c != '_') return 0;
    }
    return 1;
}

static int valid_password(const char *p) {
    size_t len = strlen(p);
    if (len < MIN_PASSWORD_LEN || len >= MAX_PASSWORD) return 0;
    for (size_t i = 0; i < len; i++) {
        if (isspace((unsigned char)p[i])) return 0;
    }
    return 1;
}

/* ── per-connection handler ───────────────────────────────────────────────── */

typedef struct {
    LCConn c;
    char   peer_addr[INET_ADDRSTRLEN];
} ClientConn;

static void handle_client(void *arg) {
    ClientConn *conn = (ClientConn *)arg;
    LCConn     *c    = &conn->c;

    if (g_ssl_ctx) {
        c->ssl = SSL_new(g_ssl_ctx);
        if (!c->ssl || SSL_set_fd(c->ssl, c->fd) != 1 ||
            SSL_accept(c->ssl) <= 0) {
            lc_close(c);
            free(conn);
            return;
        }
    }

    char session_user[MAX_USERNAME];
    memset(session_user, 0, sizeof(session_user));
    int logged_in     = 0;
    int failed_logins = 0;

    char line[MAX_LINE];
    ssize_t n;

    lc_sendline(c, "OK LedgerCore ready. Send HELP for command list.");

    while ((n = lc_readline(c, line, sizeof(line))) > 0) {

        if (n == (ssize_t)(sizeof(line) - 1)) {
            lc_drain_line(c);
            lc_sendline(c, "ERR line too long");
            continue;
        }

        char *saveptr = NULL;
        char *cmd = strtok_r(line, " \t", &saveptr);
        if (!cmd) { lc_sendline(c, "ERR empty command"); continue; }

        /* ── QUIT ─────────────────────────────────────────────────── */
        if (strcasecmp(cmd, "QUIT") == 0) {
            lc_sendline(c, "OK goodbye");
            break;
        }

        /* ── REGISTER <user> <pass> ───────────────────────────────── */
        if (strcasecmp(cmd, "REGISTER") == 0) {
            char *user = strtok_r(NULL, " \t", &saveptr);
            char *pass = strtok_r(NULL, " \t", &saveptr);
            if (!user || !pass) {
                lc_sendline(c, "ERR usage: REGISTER <username> <password>");
                continue;
            }
            if (!valid_username(user)) {
                lc_sendline(c, "ERR username must be 3-32 chars, [A-Za-z0-9_] only");
                continue;
            }
            if (!valid_password(pass)) {
                lc_sendline(c,
                    "ERR password must be 8-127 chars with no whitespace");
                continue;
            }
            if (accounts_register(user, pass) == 0) {
                audit_log("REGISTER", user, "new account", 0, "OK");
                lc_sendline(c, "OK account created");
            } else {
                audit_log("REGISTER", user, "taken or limit", 0, "ERR");
                lc_sendline(c, "ERR username already taken or server limit reached");
            }
            continue;
        }

        /* ── LOGIN <user> <pass> ──────────────────────────────────── */
        if (strcasecmp(cmd, "LOGIN") == 0) {
            char *user = strtok_r(NULL, " \t", &saveptr);
            char *pass = strtok_r(NULL, " \t", &saveptr);
            if (!user || !pass) {
                lc_sendline(c, "ERR usage: LOGIN <username> <password>");
                continue;
            }
            if (logged_in) {
                lc_sendline(c, "ERR already logged in; LOGOUT first");
                continue;
            }
            if (accounts_authenticate(user, pass)) {
                failed_logins = 0;
                logged_in = 1;
                size_t user_len = strnlen(user, MAX_USERNAME - 1);
                memcpy(session_user, user, user_len);
                session_user[user_len] = '\0';
                audit_log("LOGIN", user, conn->peer_addr, 0, "OK");
                lc_sendlinef(c, "OK logged in as %s", user);
            } else {
                failed_logins++;
                audit_log("LOGIN", user ? user : "?", "bad credentials", 0, "ERR");
                if (failed_logins >= MAX_FAILED_LOGINS) {
                    lc_sendline(c, "ERR too many failed attempts, disconnecting");
                    break;
                }
                lc_sendlinef(c, "ERR invalid username or password (%d/%d attempts)",
                             failed_logins, MAX_FAILED_LOGINS);
            }
            continue;
        }

        /* ── LOGOUT ───────────────────────────────────────────────── */
        if (strcasecmp(cmd, "LOGOUT") == 0) {
            if (!logged_in) { lc_sendline(c, "ERR not logged in"); continue; }
            audit_log("LOGOUT", session_user, "", 0, "OK");
            memset(session_user, 0, sizeof(session_user));
            logged_in = 0;
            lc_sendline(c, "OK logged out");
            continue;
        }

        if (!logged_in) { lc_sendline(c, "ERR not logged in"); continue; }

        /* ── BALANCE ──────────────────────────────────────────────── */
        if (strcasecmp(cmd, "BALANCE") == 0) {
            int64_t bal = 0;
            if (accounts_get_balance(session_user, &bal) == 0) {
                char buf[32];
                fmt_amount(buf, sizeof(buf), bal);
                lc_sendlinef(c, "OK balance %s", buf);
            } else {
                lc_sendline(c, "ERR account not found");
            }
            continue;
        }

        /* ── DEPOSIT <amount> ─────────────────────────────────────── */
        if (strcasecmp(cmd, "DEPOSIT") == 0) {
            char *amt_str = strtok_r(NULL, " \t", &saveptr);
            int64_t paise = 0;
            if (!parse_amount(amt_str, &paise)) {
                lc_sendline(c, "ERR invalid amount (positive decimal, max 2 d.p.)");
                continue;
            }
            int64_t bal = 0;
            if (accounts_deposit(session_user, paise, &bal) == 0) {
                char a_buf[32], b_buf[32];
                fmt_amount(a_buf, sizeof(a_buf), paise);
                fmt_amount(b_buf, sizeof(b_buf), bal);
                audit_log("DEPOSIT", session_user, "", paise, "OK");
                lc_sendlinef(c, "OK deposited %s new_balance %s", a_buf, b_buf);
            } else {
                audit_log("DEPOSIT", session_user, "failed", paise, "ERR");
                lc_sendline(c, "ERR deposit failed");
            }
            continue;
        }

        /* ── WITHDRAW <amount> ────────────────────────────────────── */
        if (strcasecmp(cmd, "WITHDRAW") == 0) {
            char *amt_str = strtok_r(NULL, " \t", &saveptr);
            int64_t paise = 0;
            if (!parse_amount(amt_str, &paise)) {
                lc_sendline(c, "ERR invalid amount (positive decimal, max 2 d.p.)");
                continue;
            }
            int64_t bal = 0;
            if (accounts_withdraw(session_user, paise, &bal) == 0) {
                char a_buf[32], b_buf[32];
                fmt_amount(a_buf, sizeof(a_buf), paise);
                fmt_amount(b_buf, sizeof(b_buf), bal);
                audit_log("WITHDRAW", session_user, "", paise, "OK");
                lc_sendlinef(c, "OK withdrew %s new_balance %s", a_buf, b_buf);
            } else {
                char b_buf[32];
                fmt_amount(b_buf, sizeof(b_buf), bal);
                audit_log("WITHDRAW", session_user, "insufficient", paise, "ERR");
                lc_sendlinef(c, "ERR insufficient funds (balance %s)", b_buf);
            }
            continue;
        }

        /* ── TRANSFER <to_user> <amount> ─────────────────────────── */
        if (strcasecmp(cmd, "TRANSFER") == 0) {
            char *to_user = strtok_r(NULL, " \t", &saveptr);
            char *amt_str = strtok_r(NULL, " \t", &saveptr);
            int64_t paise = 0;
            if (!to_user || !parse_amount(amt_str, &paise)) {
                lc_sendline(c, "ERR usage: TRANSFER <username> <amount>");
                continue;
            }
            if (strncmp(to_user, session_user, MAX_USERNAME) == 0) {
                lc_sendline(c, "ERR cannot transfer to yourself");
                continue;
            }
            int64_t src_bal = 0;
            if (accounts_transfer(session_user, to_user, paise, &src_bal) == 0) {
                char a_buf[32], b_buf[32];
                fmt_amount(a_buf, sizeof(a_buf), paise);
                fmt_amount(b_buf, sizeof(b_buf), src_bal);
                char detail[128];
                snprintf(detail, sizeof(detail), "to:%s", to_user);
                audit_log("TRANSFER", session_user, detail, paise, "OK");
                lc_sendlinef(c, "OK transferred %s to %s new_balance %s",
                             a_buf, to_user, b_buf);
            } else {
                char b_buf[32];
                fmt_amount(b_buf, sizeof(b_buf), src_bal);
                char detail[128];
                snprintf(detail, sizeof(detail), "to:%s insufficient", to_user);
                audit_log("TRANSFER", session_user, detail, paise, "ERR");
                lc_sendlinef(c, "ERR transfer failed (balance %s)", b_buf);
            }
            continue;
        }

        /* ── HISTORY ──────────────────────────────────────────────── */
        if (strcasecmp(cmd, "HISTORY") == 0) {
            lc_sendlinef(c, "OK history for %s:", session_user);
            int cnt = audit_history(session_user, HISTORY_PER_ACCT, c);
            lc_sendlinef(c, "END %d records", cnt);
            continue;
        }

        /* ── HELP ─────────────────────────────────────────────────── */
        if (strcasecmp(cmd, "HELP") == 0) {
            lc_sendline(c, "OK commands: REGISTER LOGIN LOGOUT BALANCE "
                           "DEPOSIT WITHDRAW TRANSFER HISTORY QUIT");
            continue;
        }

        lc_sendlinef(c, "ERR unknown command: %s", cmd);
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            lc_sendline(c, "ERR idle timeout, disconnecting");
    }

    lc_close(c);
    free(conn);
}

/* ── accept loop ─────────────────────────────────────────────────────────── */

static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

/* Accept one connection and hand off to the pool. Returns 1 when accepted. */
static int accept_and_dispatch(void) {
    struct sockaddr_in ca;
    socklen_t cal = sizeof(ca);
    int cfd = accept(g_listen_fd, (struct sockaddr *)&ca, &cal);
    if (cfd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            if (errno != EBADF) perror("accept");
        return 0;
    }

    struct timeval tv = { .tv_sec = CONN_IDLE_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ClientConn *conn = malloc(sizeof(ClientConn));
    if (!conn) {
        fprintf(stderr, "malloc failed\n");
        close(cfd);
        return 1;
    }
    conn->c.fd  = cfd;
    conn->c.ssl = NULL;
    inet_ntop(AF_INET, &ca.sin_addr, conn->peer_addr, sizeof(conn->peer_addr));

    if (threadpool_submit(&g_pool, handle_client, conn) < 0) {
        if (!g_ssl_ctx) lc_sendline(&conn->c, "ERR server busy, try again");
        lc_close(&conn->c);
        free(conn);
    }
    return 1;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    /* Optional TLS: ledgercore_server <port> <cert.pem> <key.pem> */
    if (argc >= 4) {
        g_ssl_ctx = tls_server_ctx(argv[2], argv[3]);
        if (!g_ssl_ctx) return 1;
        fprintf(stdout, "TLS enabled (cert=%s)\n", argv[2]);
    }

    if (acquire_lockfile() < 0) return 1;

    if (accounts_init(SNAP_FILE) < 0) {
        fprintf(stderr, "Failed to load snapshot\n");
        return 1;
    }
    if (wal_replay(WAL_FILE, accounts_snap_lsn()) < 0) {
        fprintf(stderr, "WAL replay failed\n");
        return 1;
    }
    if (wal_init(WAL_FILE) < 0) {
        fprintf(stderr, "Failed to open WAL\n");
        return 1;
    }

    if (audit_init(AUDIT_FILE) < 0) {
        fprintf(stderr, "Failed to open audit log\n");
        return 1;
    }
    if (threadpool_init(&g_pool) < 0) {
        fprintf(stderr, "Failed to start thread pool\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    g_listen_fd = make_listen_socket(port);
    if (g_listen_fd < 0) return 1;
    if (accounts_checkpoint_start() < 0) {
        fprintf(stderr, "Failed to start checkpoint worker\n");
        return 1;
    }

    fprintf(stdout, "LedgerCore server listening on port %d  "
                    "(pool=%d threads, queue=%d jobs%s)\n",
            port, THREADPOOL_SIZE, QUEUE_CAPACITY,
            g_ssl_ctx ? ", TLS" : "");
    fflush(stdout);

#ifdef __linux__
    /* ── epoll accept loop (Linux) ────────────────────────────────────────── */
    {
        if (fcntl(g_listen_fd, F_SETFL,
                  fcntl(g_listen_fd, F_GETFL) | O_NONBLOCK) < 0) {
            perror("listen: nonblock");
            return 1;
        }
        if (pipe(g_pipe) < 0) { perror("pipe"); return 1; }
        fcntl(g_pipe[1], F_SETFL, fcntl(g_pipe[1], F_GETFL) | O_NONBLOCK);

        int ep = epoll_create1(EPOLL_CLOEXEC);
        if (ep < 0) { perror("epoll_create1"); return 1; }

        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = g_listen_fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, g_listen_fd, &ev);
        ev.data.fd = g_pipe[0];
        epoll_ctl(ep, EPOLL_CTL_ADD, g_pipe[0], &ev);

        struct epoll_event events[32];
        int done = 0;
        while (g_running && !done) {
            int n = epoll_wait(ep, events, 32, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n && !done; i++) {
                if (events[i].data.fd == g_pipe[0])  { done = 1; break; }
                if (events[i].data.fd == g_listen_fd)
                    while (g_running && accept_and_dispatch()) {}
            }
        }
        close(ep);
    }
#else
    /* ── blocking accept loop (macOS / other POSIX) ─────────────────────── */
    while (g_running) {
        accept_and_dispatch();
    }
#endif

    fprintf(stdout, "\nShutting down...\n");
    fflush(stdout);

    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    if (g_pipe[0]   >= 0) { close(g_pipe[0]);   g_pipe[0]   = -1; }
    if (g_pipe[1]   >= 0) { close(g_pipe[1]);   g_pipe[1]   = -1; }

    threadpool_destroy(&g_pool);
    int persistence_ok = accounts_shutdown() == 0;
    if (wal_shutdown() != 0) persistence_ok = 0;
    audit_log("SERVER", "system", "shutdown", 0, "OK");
    audit_close();

    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }

    release_lockfile();

    if (!persistence_ok) fprintf(stderr, "LedgerCore shutdown: persistence failed\n");
    else fprintf(stdout, "LedgerCore shut down cleanly.\n");
    return persistence_ok ? 0 : 1;
}
