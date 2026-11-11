/*
 * tests/loadgen.c – LedgerCore load generator / benchmark
 *
 * Usage: ledgercore_loadgen [options]
 *   -h HOST    server host (default 127.0.0.1)
 *   -p PORT    server port (default 18080)
 *   -t SECS    duration   (default 10)
 *   -c CONNS   concurrent connections / threads (default 8)
 *   -w WARM    warm-up seconds discarded from results (default 2)
 *   -m         mixed deposit/withdraw/transfer/balance workload
 *   -L         log in once per connection (transaction-only timing)
 *
 * Each thread: register once, then loop:
 *   LOGIN → DEPOSIT 1 → LOGOUT
 * measuring wall-clock latency of each LOGIN..LOGOUT round trip.
 *
 * Reports: ops/sec, p50/p95/p99 latency (µs).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdatomic.h>

/* ── config ──────────────────────────────────────────────────────────────── */

#define DEFAULT_HOST  "127.0.0.1"
#define DEFAULT_PORT  18080
#define DEFAULT_SECS  10
#define DEFAULT_CONNS 8
#define DEFAULT_WARM  2

#define MAX_LATENCIES  4000000   /* max per-op samples stored per thread */
#define BUF_SZ         512

/* ── timing ──────────────────────────────────────────────────────────────── */

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* ── socket helpers ──────────────────────────────────────────────────────── */

static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    /* Allow concurrent PBKDF2 logins to queue on small CI runners. */
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* Read exactly one '\n'-terminated line; returns 0 on success, -1 on error */
static int read_line(int fd, char *buf, size_t sz) {
    size_t n = 0;
    while (n < sz - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) return -1;
        if (c == '\r') continue;
        if (c == '\n') { buf[n] = '\0'; return 0; }
        buf[n++] = c;
    }
    buf[n] = '\0';
    return 0;
}

static int send_cmd(int fd, const char *cmd) {
    char buf[BUF_SZ];
    int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    return (write(fd, buf, (size_t)n) == n) ? 0 : -1;
}

/* Return 1=OK, 0=ERR, -1=transport failure. */
static int cmd_status(int fd, const char *cmd) {
    char reply[BUF_SZ];
    if (send_cmd(fd, cmd) < 0) return -1;
    if (read_line(fd, reply, sizeof(reply)) < 0) return -1;
    if (strncmp(reply, "OK", 2) == 0) return 1;
    if (strncmp(reply, "ERR", 3) == 0) return 0;
    return -1;
}

static int cmd_ok(int fd, const char *cmd) { return cmd_status(fd, cmd) == 1; }

/* ── comparison for qsort ────────────────────────────────────────────────── */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ── shared state ────────────────────────────────────────────────────────── */

typedef struct {
    const char *host;
    int         port;
    int         id;          /* thread index */
    int         conns;
    int         mixed;
    int         prelogged;
    int         warm_secs;
    /* results written by thread */
    uint64_t   *latencies;   /* µs per LOGIN..LOGOUT round trip */
    size_t      n_lat;       /* count after warm-up */
    uint64_t    ops_warm;    /* ops during warm-up (discarded) */
    int         error;       /* non-zero if thread failed */
} WorkerArgs;

static _Atomic int  g_start = 0;
static _Atomic int  g_ready = 0;
static _Atomic int  g_stop  = 0;   /* set to 1 when time is up */

/* ── worker thread ───────────────────────────────────────────────────────── */

static void *worker(void *arg) {
    WorkerArgs *w = (WorkerArgs *)arg;

    /* Register account once; name is "user_<id>_<pid>" */
    char user[64], pass[32], cmd[BUF_SZ];
    snprintf(user, sizeof(user), "bench_%d_%d", w->id, (int)getpid());
    snprintf(pass, sizeof(pass), "bench_pw_%d", w->id);

    int fd = tcp_connect(w->host, w->port);
    if (fd < 0) {
        w->error = 1;
        atomic_fetch_add(&g_ready, 1);
        return NULL;
    }

    char buf[BUF_SZ];
    snprintf(cmd, sizeof(cmd), "REGISTER %s %s", user, pass);
    int setup_ok = read_line(fd, buf, sizeof(buf)) == 0 &&
                   cmd_ok(fd, cmd);
    snprintf(cmd, sizeof(cmd), "LOGIN %s %s", user, pass);
    setup_ok = setup_ok && cmd_ok(fd, cmd) &&
               cmd_ok(fd, "DEPOSIT 1000") && cmd_ok(fd, "LOGOUT");
    send_cmd(fd, "QUIT");
    read_line(fd, buf, sizeof(buf));
    close(fd);
    if (!setup_ok) {
        w->error = 1;
        atomic_fetch_add(&g_ready, 1);
        return NULL;
    }

    atomic_fetch_add(&g_ready, 1);
    while (!atomic_load(&g_start)) usleep(1000);

    uint64_t t_thread_start = now_us();
    uint64_t warm_end = t_thread_start + (uint64_t)w->warm_secs * 1000000ULL;
    uint64_t ops_warm = 0;
    size_t n = 0;

    /* Persistent connection: re-connect on error */
    fd = -1;
    int logged_in = 0;
    unsigned rng = (unsigned)getpid() ^ (unsigned)w->id * 2654435761u;

    while (!atomic_load(&g_stop)) {
        if (fd < 0) {
            fd = tcp_connect(w->host, w->port);
            if (fd < 0) { usleep(1000); continue; }
            if (read_line(fd, buf, sizeof(buf)) != 0) {
                close(fd); fd = -1; continue;
            }
            logged_in = 0;
        }

        if (w->prelogged && !logged_in) {
            snprintf(cmd, sizeof(cmd), "LOGIN %s %s", user, pass);
            if (!cmd_ok(fd, cmd)) { close(fd); fd = -1; continue; }
            logged_in = 1;
        }
        uint64_t t0 = now_us();

        if (!w->prelogged) {
            snprintf(cmd, sizeof(cmd), "LOGIN %s %s", user, pass);
            if (!cmd_ok(fd, cmd)) { close(fd); fd = -1; continue; }
        }

        if (!w->mixed) {
            strcpy(cmd, "DEPOSIT 1");
        } else {
            int kind = (int)(rand_r(&rng) % 4);
            if (kind == 0) strcpy(cmd, "DEPOSIT 1");
            else if (kind == 1) strcpy(cmd, "WITHDRAW 1");
            else if (kind == 2 && w->conns > 1) {
                int target = (w->id + 1 + (int)(rand_r(&rng) % (unsigned)(w->conns - 1)))
                             % w->conns;
                snprintf(cmd, sizeof(cmd), "TRANSFER bench_%d_%d 1",
                         target, (int)getpid());
            } else strcpy(cmd, "BALANCE");
        }
        int status = cmd_status(fd, cmd);
        if (status < 0) { close(fd); fd = -1; continue; }

        if (!w->prelogged && !cmd_ok(fd, "LOGOUT")) {
            close(fd); fd = -1; continue;
        }
        if (status == 0) continue;

        uint64_t lat = now_us() - t0;

        if (now_us() < warm_end) {
            ops_warm++;
        } else {
            if (n < MAX_LATENCIES)
                w->latencies[n++] = lat;
        }
    }

    if (fd >= 0) {
        send_cmd(fd, "QUIT");
        read_line(fd, buf, sizeof(buf));
        close(fd);
    }

    w->n_lat    = n;
    w->ops_warm = ops_warm;
    return NULL;
}

/* ── percentile helper ───────────────────────────────────────────────────── */

static uint64_t percentile(uint64_t *sorted, size_t n, double pct) {
    if (n == 0) return 0;
    size_t idx = (size_t)(pct / 100.0 * (double)n);
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *host  = DEFAULT_HOST;
    int port  = DEFAULT_PORT;
    int secs  = DEFAULT_SECS;
    int conns = DEFAULT_CONNS;
    int warm  = DEFAULT_WARM;
    int mixed = 0, prelogged = 0;

    int opt;
    while ((opt = getopt(argc, argv, "h:p:t:c:w:mL")) != -1) {
        switch (opt) {
        case 'h': host  = optarg;      break;
        case 'p': port  = atoi(optarg); break;
        case 't': secs  = atoi(optarg); break;
        case 'c': conns = atoi(optarg); break;
        case 'w': warm  = atoi(optarg); break;
        case 'm': mixed = 1; break;
        case 'L': prelogged = 1; break;
        default:
            fprintf(stderr, "usage: %s [-h host] [-p port] [-t secs] "
                            "[-c conns] [-w warm_secs] [-m] [-L]\n", argv[0]);
            return 1;
        }
    }

    if (warm >= secs || warm < 0 || secs < 1 || conns < 1 || conns > 256) {
        fprintf(stderr, "warm-up (%ds) must be < total duration (%ds)\n",
                warm, secs);
        return 1;
    }

    printf("LedgerCore loadgen: host=%s port=%d duration=%ds "
           "connections=%d warm-up=%ds workload=%s session=%s\n",
           host, port, secs, conns, warm,
           mixed ? "mixed" : "deposit", prelogged ? "prelogged" : "login-per-op");

    /* Allocate per-thread state */
    WorkerArgs *args   = calloc((size_t)conns, sizeof(WorkerArgs));
    pthread_t  *tids   = calloc((size_t)conns, sizeof(pthread_t));
    uint64_t  **lats   = calloc((size_t)conns, sizeof(uint64_t *));

    if (!args || !tids || !lats) { perror("calloc"); return 1; }

    for (int i = 0; i < conns; i++) {
        lats[i] = calloc(MAX_LATENCIES, sizeof(uint64_t));
        if (!lats[i]) { perror("calloc latencies"); return 1; }
        args[i].host       = host;
        args[i].port       = port;
        args[i].id         = i;
        args[i].conns      = conns;
        args[i].mixed      = mixed;
        args[i].prelogged  = prelogged;
        args[i].warm_secs  = warm;
        args[i].latencies  = lats[i];
    }

    /* Start threads — they spin on g_start */
    for (int i = 0; i < conns; i++)
        pthread_create(&tids[i], NULL, worker, &args[i]);

    /* Keep registration and seeding outside the timed workload. */
    while (atomic_load(&g_ready) < conns) usleep(1000);
    for (int i = 0; i < conns; i++) {
        if (args[i].error) {
            fprintf(stderr, "worker %d failed during setup\n", i);
            atomic_store(&g_start, 1);
            atomic_store(&g_stop, 1);
            for (int j = 0; j < conns; j++) pthread_join(tids[j], NULL);
            return 1;
        }
    }

    uint64_t t_start = now_us();
    atomic_store(&g_start, 1);

    sleep((unsigned)secs);
    atomic_store(&g_stop, 1);

    for (int i = 0; i < conns; i++)
        pthread_join(tids[i], NULL);

    uint64_t elapsed_us = now_us() - t_start - (uint64_t)warm * 1000000ULL;

    /* Aggregate latencies */
    size_t total_lats = 0;
    for (int i = 0; i < conns; i++) total_lats += args[i].n_lat;

    uint64_t *all = NULL;
    if (total_lats > 0) {
        all = malloc(total_lats * sizeof(uint64_t));
        if (!all) { perror("malloc"); return 1; }
        size_t off = 0;
        for (int i = 0; i < conns; i++) {
            memcpy(all + off, lats[i], args[i].n_lat * sizeof(uint64_t));
            off += args[i].n_lat;
        }
        qsort(all, total_lats, sizeof(uint64_t), cmp_u64);
    }

    double ops_per_sec = (elapsed_us > 0)
        ? (double)total_lats / ((double)elapsed_us / 1e6)
        : 0.0;

    printf("\n────────────────────────────────────────────\n");
    printf("  Measurement window : %.2f s\n", (double)elapsed_us / 1e6);
    printf("  Operations         : %zu\n",   total_lats);
    printf("  Throughput         : %.1f ops/sec\n", ops_per_sec);
    if (all) {
        printf("  Latency  p50       : %" PRIu64 " µs\n",
               percentile(all, total_lats, 50.0));
        printf("  Latency  p95       : %" PRIu64 " µs\n",
               percentile(all, total_lats, 95.0));
        printf("  Latency  p99       : %" PRIu64 " µs\n",
               percentile(all, total_lats, 99.0));
        printf("  Latency  max       : %" PRIu64 " µs\n",
               all[total_lats - 1]);
    }
    printf("────────────────────────────────────────────\n");

    int any_error = 0;
    for (int i = 0; i < conns; i++)
        if (args[i].error) { fprintf(stderr, "worker %d failed\n", i); any_error = 1; }

    free(all);
    for (int i = 0; i < conns; i++) free(lats[i]);
    free(lats); free(args); free(tids);
    return any_error;
}
