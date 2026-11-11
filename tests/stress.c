/* Network transfer stress test: 20 accounts, 32+ concurrent connections.
 * All writes are transfers, so their exact total must remain unchanged. */
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

#define ACCOUNT_COUNT 20
#define SEED_PAISE 100000

typedef struct {
    int port, id, failed;
    unsigned rng;
    uint64_t completed;
} Worker;

static pthread_mutex_t gate_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int ready_count, start_flag;
static _Atomic int stop_load;

static int connect_server(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    struct timeval tv = {.tv_sec = 30};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t got = read(fd, &c, 1);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        if (c == '\n') { buf[n] = '\0'; return 0; }
        if (c != '\r') buf[n++] = c;
    }
    return -1;
}

static int send_line(int fd, const char *cmd) {
    char line[256];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    if (n < 0 || (size_t)n >= sizeof(line)) return -1;
    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t wrote = write(fd, line + sent, (size_t)n - sent);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return -1;
        sent += (size_t)wrote;
    }
    return 0;
}

static int command(int fd, const char *cmd, char *reply, size_t cap) {
    return send_line(fd, cmd) == 0 ? read_line(fd, reply, cap) : -1;
}

static int session(int port) {
    char greeting[256];
    int fd = connect_server(port);
    if (fd < 0) return -1;
    if (read_line(fd, greeting, sizeof(greeting)) != 0) { close(fd); return -1; }
    return fd;
}

static int is_ok(const char *reply) { return strncmp(reply, "OK", 2) == 0; }

static int seed(int port) {
    char cmd[256], reply[256];
    for (int i = 0; i < ACCOUNT_COUNT; i++) {
        int fd = session(port);
        if (fd < 0) return -1;
        snprintf(cmd, sizeof(cmd), "REGISTER stress_%02d stresspass8", i);
        if (command(fd, cmd, reply, sizeof(reply)) != 0 || !is_ok(reply)) return -1;
        snprintf(cmd, sizeof(cmd), "LOGIN stress_%02d stresspass8", i);
        if (command(fd, cmd, reply, sizeof(reply)) != 0 || !is_ok(reply)) return -1;
        if (command(fd, "DEPOSIT 1000", reply, sizeof(reply)) != 0 || !is_ok(reply)) return -1;
        send_line(fd, "QUIT");
        close(fd);
    }
    return 0;
}

static void *worker_main(void *arg) {
    Worker *w = arg;
    char cmd[256], reply[256];
    int fd = session(w->port);
    if (fd < 0) w->failed = 1;
    if (!w->failed) {
        snprintf(cmd, sizeof(cmd), "LOGIN stress_%02d stresspass8",
                 w->id % ACCOUNT_COUNT);
        if (command(fd, cmd, reply, sizeof(reply)) != 0 || !is_ok(reply))
            w->failed = 1;
    }
    pthread_mutex_lock(&gate_mtx);
    ready_count++;
    pthread_cond_broadcast(&gate_cond);
    while (!start_flag) pthread_cond_wait(&gate_cond, &gate_mtx);
    pthread_mutex_unlock(&gate_mtx);
    if (w->failed) { if (fd >= 0) close(fd); return NULL; }

    while (!atomic_load(&stop_load)) {
        int src = w->id % ACCOUNT_COUNT;
        int dst = (src + 1 + (int)(rand_r(&w->rng) % (ACCOUNT_COUNT - 1)))
                  % ACCOUNT_COUNT;
        int amount = (int)(rand_r(&w->rng) % 10) + 1;
        snprintf(cmd, sizeof(cmd), "TRANSFER stress_%02d %d", dst, amount);
        if (command(fd, cmd, reply, sizeof(reply)) != 0) {
            w->failed = 1; break;
        }
        if (is_ok(reply)) w->completed++;
        else if (strncmp(reply, "ERR", 3) != 0) { w->failed = 1; break; }
    }
    send_line(fd, "QUIT");
    close(fd);
    return NULL;
}

static int read_balances(int port, int64_t *total) {
    char cmd[256], reply[256];
    *total = 0;
    for (int i = 0; i < ACCOUNT_COUNT; i++) {
        int fd = session(port);
        if (fd < 0) return -1;
        snprintf(cmd, sizeof(cmd), "LOGIN stress_%02d stresspass8", i);
        if (command(fd, cmd, reply, sizeof(reply)) != 0 || !is_ok(reply)) return -1;
        int64_t units;
        int paise;
        if (command(fd, "BALANCE", reply, sizeof(reply)) != 0 ||
            sscanf(reply, "OK balance %" SCNd64 ".%2d", &units, &paise) != 2 ||
            units < 0 || paise < 0 || paise > 99) return -1;
        *total += units * 100 + paise;
        send_line(fd, "QUIT");
        close(fd);
    }
    return 0;
}

int main(int argc, char **argv) {
    int port = 18080, conns = 32, seconds = 3;
    int opt;
    while ((opt = getopt(argc, argv, "p:c:t:")) != -1) {
        if (opt == 'p') port = atoi(optarg);
        else if (opt == 'c') conns = atoi(optarg);
        else if (opt == 't') seconds = atoi(optarg);
        else return 2;
    }
    if (port < 1 || port > 65535 || conns < 32 || conns > 256 ||
        seconds < 1 || seconds > 600) return 2;
    signal(SIGPIPE, SIG_IGN);
    if (seed(port) != 0) { fprintf(stderr, "stress: seeding failed\n"); return 1; }

    Worker *workers = calloc((size_t)conns, sizeof(*workers));
    pthread_t *threads = calloc((size_t)conns, sizeof(*threads));
    if (!workers || !threads) return 2;
    ready_count = start_flag = 0;
    atomic_store(&stop_load, 0);
    for (int i = 0; i < conns; i++) {
        workers[i].port = port;
        workers[i].id = i;
        workers[i].rng = (unsigned)time(NULL) ^ (unsigned)getpid() ^ (unsigned)i;
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0)
            return 2;
    }
    pthread_mutex_lock(&gate_mtx);
    while (ready_count < conns) pthread_cond_wait(&gate_cond, &gate_mtx);
    start_flag = 1;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mtx);
    sleep((unsigned)seconds);
    atomic_store(&stop_load, 1);
    uint64_t completed = 0;
    int failed = 0;
    for (int i = 0; i < conns; i++) {
        pthread_join(threads[i], NULL);
        completed += workers[i].completed;
        if (workers[i].failed) failed = 1;
    }
    int64_t total = 0;
    if (read_balances(port, &total) != 0) failed = 1;
    const int64_t expected = (int64_t)ACCOUNT_COUNT * SEED_PAISE;
    printf("stress: %d accounts, %d connections, %" PRIu64
           " successful transfers, total=%" PRId64 " expected=%" PRId64 " paise\n",
           ACCOUNT_COUNT, conns, completed, total, expected);
    free(workers);
    free(threads);
    if (failed || completed == 0 || total != expected) {
        fprintf(stderr, "stress: FAIL\n"); return 1;
    }
    puts("stress: PASS – exact conservation and no negative balances");
    return 0;
}
