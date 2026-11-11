/* Concurrent crash/recovery test. One outstanding command per client means
 * only the last command on each connection can be ambiguous after SIGKILL. */
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define ACCOUNTS 10
#define CLIENTS 16
#define MAX_OPS 2048
#define SEED_PAISE 100000

enum { DEPOSIT = 0, WITHDRAW = 1, TRANSFER = 2 };
enum { AMBIGUOUS = 0, ACKED = 1, REJECTED = 2 };

typedef struct { int type, src, dst, amount, status; } Operation;
typedef struct {
    int port, id, failed, count;
    unsigned rng;
    Operation ops[MAX_OPS];
} Worker;

static pthread_mutex_t start_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
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
        close(fd);
        return -1;
    }
    struct timeval tv = {.tv_sec = 15};
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

static int open_session(int port) {
    int fd = connect_server(port);
    char reply[256];
    if (fd < 0) return -1;
    if (read_line(fd, reply, sizeof(reply)) != 0) { close(fd); return -1; }
    return fd;
}

static int starts_ok(const char *reply) { return strncmp(reply, "OK", 2) == 0; }

static int seed_accounts(int port) {
    char cmd[256], reply[256];
    for (int i = 0; i < ACCOUNTS; i++) {
        int fd = open_session(port);
        if (fd < 0) return -1;
        snprintf(cmd, sizeof(cmd), "REGISTER acct_%02d testpass8", i);
        if (command(fd, cmd, reply, sizeof(reply)) != 0 || !starts_ok(reply)) return -1;
        snprintf(cmd, sizeof(cmd), "LOGIN acct_%02d testpass8", i);
        if (command(fd, cmd, reply, sizeof(reply)) != 0 || !starts_ok(reply)) return -1;
        if (command(fd, "DEPOSIT 1000", reply, sizeof(reply)) != 0 || !starts_ok(reply)) return -1;
        send_line(fd, "QUIT");
        close(fd);
    }
    return 0;
}

static void *client_worker(void *arg) {
    Worker *w = arg;
    char cmd[256], reply[256];
    int fd = open_session(w->port);
    if (fd < 0) w->failed = 1;
    if (!w->failed) {
        snprintf(cmd, sizeof(cmd), "LOGIN acct_%02d testpass8", w->id % ACCOUNTS);
        if (command(fd, cmd, reply, sizeof(reply)) != 0 || !starts_ok(reply))
            w->failed = 1;
    }
    pthread_mutex_lock(&start_mtx);
    ready_count++;
    /* Main and workers share this condition: broadcast so the final ready
     * notification cannot wake only another worker waiting for start_flag. */
    pthread_cond_broadcast(&start_cond);
    while (!start_flag) pthread_cond_wait(&start_cond, &start_mtx);
    pthread_mutex_unlock(&start_mtx);
    if (w->failed) { if (fd >= 0) close(fd); return NULL; }

    while (!atomic_load(&stop_load) && w->count < MAX_OPS) {
        Operation op = {0};
        op.type = (int)(rand_r(&w->rng) % 3);
        op.src = w->id % ACCOUNTS;
        op.dst = (op.src + 1 + (int)(rand_r(&w->rng) % (ACCOUNTS - 1))) % ACCOUNTS;
        op.amount = (int)(rand_r(&w->rng) % 20) + 1;
        op.status = AMBIGUOUS;
        if (op.type == DEPOSIT)
            snprintf(cmd, sizeof(cmd), "DEPOSIT %d", op.amount);
        else if (op.type == WITHDRAW)
            snprintf(cmd, sizeof(cmd), "WITHDRAW %d", op.amount);
        else
            snprintf(cmd, sizeof(cmd), "TRANSFER acct_%02d %d", op.dst, op.amount);

        /* Publish intent before sending. A killed server may have durably
         * applied a command whose reply never reached this client. */
        Operation *record = &w->ops[w->count++];
        *record = op;
        if (command(fd, cmd, reply, sizeof(reply)) != 0) break;
        if (starts_ok(reply)) record->status = ACKED;
        else if (strncmp(reply, "ERR", 3) == 0) record->status = REJECTED;
        else { w->failed = 1; break; }
    }
    close(fd);
    return NULL;
}

static pid_t start_server(const char *server_path, const char *dir, int port) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    if (chdir(dir) != 0) _exit(127);
    int log_fd = open("server.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd >= 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    }
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    execl(server_path, server_path, port_str, (char *)NULL);
    _exit(127);
}

static int wait_server(int port) {
    for (int i = 0; i < 100; i++) {
        int fd = connect_server(port);
        if (fd >= 0) { close(fd); return 0; }
        usleep(50000);
    }
    return -1;
}

static int query_balances(int port, int64_t balances[ACCOUNTS]) {
    char cmd[256], reply[256];
    for (int i = 0; i < ACCOUNTS; i++) {
        int fd = open_session(port);
        if (fd < 0) return -1;
        snprintf(cmd, sizeof(cmd), "LOGIN acct_%02d testpass8", i);
        if (command(fd, cmd, reply, sizeof(reply)) != 0 || !starts_ok(reply)) return -1;
        int64_t units;
        int paise;
        if (command(fd, "BALANCE", reply, sizeof(reply)) != 0 ||
            sscanf(reply, "OK balance %" SCNd64 ".%2d", &units, &paise) != 2 ||
            units < 0 || paise < 0 || paise > 99) return -1;
        balances[i] = units * 100 + paise;
        send_line(fd, "QUIT");
        close(fd);
    }
    return 0;
}

static void apply_op(int64_t balances[ACCOUNTS], const Operation *op) {
    int64_t amount = (int64_t)op->amount * 100;
    if (op->type == DEPOSIT) balances[op->src] += amount;
    else if (op->type == WITHDRAW) balances[op->src] -= amount;
    else { balances[op->src] -= amount; balances[op->dst] += amount; }
}

static int matches_recovery(const int64_t acked[ACCOUNTS],
                            const int64_t recovered[ACCOUNTS],
                            const Operation ambiguous[CLIENTS], int n_ambiguous) {
    uint64_t possibilities = UINT64_C(1) << n_ambiguous;
    for (uint64_t mask = 0; mask < possibilities; mask++) {
        int64_t candidate[ACCOUNTS];
        memcpy(candidate, acked, sizeof(candidate));
        for (int i = 0; i < n_ambiguous; i++)
            if (mask & (UINT64_C(1) << i)) apply_op(candidate, &ambiguous[i]);
        if (memcmp(candidate, recovered, sizeof(candidate)) == 0) return 1;
    }
    return 0;
}

static int run_iteration(const char *server_path, const char *dir,
                         int port, unsigned *rng, int iteration) {
    pid_t server_pid = start_server(server_path, dir, port);
    if (server_pid <= 0 || wait_server(port) != 0 || seed_accounts(port) != 0) {
        fprintf(stderr, "iteration %d: server start or seeding failed (%s)\n", iteration, dir);
        if (server_pid > 0) {
            kill(server_pid, SIGTERM);
            waitpid(server_pid, NULL, 0);
        }
        return -1;
    }

    Worker workers[CLIENTS] = {0};
    pthread_t threads[CLIENTS];
    pthread_mutex_lock(&start_mtx);
    ready_count = 0;
    start_flag = 0;
    pthread_mutex_unlock(&start_mtx);
    atomic_store(&stop_load, 0);
    int created = 0;
    for (int i = 0; i < CLIENTS; i++) {
        workers[i].port = port;
        workers[i].id = i;
        workers[i].rng = rand_r(rng) ^ (unsigned)i;
        if (pthread_create(&threads[i], NULL, client_worker, &workers[i]) != 0) {
            kill(server_pid, SIGKILL);
            waitpid(server_pid, NULL, 0);
            pthread_mutex_lock(&start_mtx);
            start_flag = 1;
            pthread_cond_broadcast(&start_cond);
            pthread_mutex_unlock(&start_mtx);
            for (int j = 0; j < created; j++) pthread_join(threads[j], NULL);
            return -1;
        }
        created++;
    }
    pthread_mutex_lock(&start_mtx);
    while (ready_count < CLIENTS) pthread_cond_wait(&start_cond, &start_mtx);
    start_flag = 1;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mtx);
    usleep(50000 + rand_r(rng) % 200000);
    kill(server_pid, SIGKILL);
    waitpid(server_pid, NULL, 0);
    atomic_store(&stop_load, 1);
    for (int i = 0; i < CLIENTS; i++) pthread_join(threads[i], NULL);

    int64_t acked[ACCOUNTS], recovered[ACCOUNTS];
    Operation ambiguous[CLIENTS];
    int n_ambiguous = 0, n_acked = 0;
    for (int i = 0; i < ACCOUNTS; i++) acked[i] = SEED_PAISE;
    for (int i = 0; i < CLIENTS; i++) {
        if (workers[i].failed) return -1;
        for (int j = 0; j < workers[i].count; j++) {
            Operation *op = &workers[i].ops[j];
            if (op->status == ACKED) { apply_op(acked, op); n_acked++; }
            else if (op->status == AMBIGUOUS) {
                if (n_ambiguous >= CLIENTS) return -1;
                ambiguous[n_ambiguous++] = *op;
            }
        }
    }

    server_pid = start_server(server_path, dir, port);
    if (server_pid <= 0 || wait_server(port) != 0 ||
        query_balances(port, recovered) != 0) {
        fprintf(stderr, "iteration %d: recovery or balance query failed (%s)\n", iteration, dir);
        if (server_pid > 0) {
            kill(server_pid, SIGTERM);
            waitpid(server_pid, NULL, 0);
        }
        return -1;
    }
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    if (!matches_recovery(acked, recovered, ambiguous, n_ambiguous)) {
        fprintf(stderr, "iteration %d: recovered balances disagree with acknowledged and in-flight operations (%s)\n",
                iteration, dir);
        for (int i = 0; i < ACCOUNTS; i++)
            fprintf(stderr, "  acct_%02d acknowledged=%" PRId64 " recovered=%" PRId64 "\n",
                    i, acked[i], recovered[i]);
        return -1;
    }
    printf("iteration %d: PASS (%d acknowledged, %d in flight)\n",
           iteration, n_acked, n_ambiguous);
    return 0;
}

int main(int argc, char **argv) {
    int iterations = argc > 1 ? atoi(argv[1]) : 20;
    if (iterations < 1 || iterations > 100) return 2;
    char server_path[PATH_MAX];
    if (!realpath("bin/ledgercore_server", server_path)) {
        perror("realpath server"); return 2;
    }
    char root[] = "/tmp/ledgercore_crash_XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    unsigned rng = (unsigned)time(NULL) ^ (unsigned)getpid();
    for (int i = 0; i < iterations; i++) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/iter_%02d", root, i);
        if (mkdir(dir, 0700) != 0) return 2;
        if (run_iteration(server_path, dir, 18400 + ((int)getpid() % 1000) + i,
                          &rng, i + 1) != 0) {
            fprintf(stderr, "crash test artifacts: %s\n", dir);
            return 1;
        }
        const char *files[] = {"server.log", "ledgercore.snap", "ledgercore.snap.tmp",
                               "ledgercore.wal", "ledgercore_audit.log", "ledgercore.lock"};
        for (size_t j = 0; j < sizeof(files) / sizeof(files[0]); j++) {
            char path[PATH_MAX];
            int written = snprintf(path, sizeof(path), "%s/%s", dir, files[j]);
            if (written < 0 || (size_t)written >= sizeof(path)) return 2;
            unlink(path);
        }
        rmdir(dir);
    }
    rmdir(root);
    printf("RESULT: PASS – %d concurrent crash/recovery iterations\n", iterations);
    return 0;
}
