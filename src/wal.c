/*
 * wal.c – Write-ahead log with group-commit writer thread.
 *
 * Operations (DEPOSIT, WITHDRAW, TRANSFER, REGISTER) call wal_append() with
 * accounts-layer locks held.  wal_append enqueues a record and blocks until
 * the writer thread has fsynced it to disk.  The writer batches multiple
 * in-flight records into a single fsync (group commit), so throughput scales
 * under concurrent clients.
 *
 * Recovery: wal_replay() iterates records from the WAL file on startup
 * (single-threaded, before wal_init).  It skips records already covered by
 * the snapshot (lsn <= from_lsn) and stops at the first CRC error.
 */

#include "wal.h"
#include "accounts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>

/* ── IEEE CRC-32 ─────────────────────────────────────────────────────────── */

static uint32_t        g_crc_table[256];
static pthread_once_t  g_crc_once = PTHREAD_ONCE_INIT;

static void crc32_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
        g_crc_table[i] = c;
    }
}

static uint32_t crc32_of(const void *data, size_t len) {
    pthread_once(&g_crc_once, crc32_build_table);
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ g_crc_table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}

/* ── Group-commit queue ───────────────────────────────────────────────────── */

#define WAL_QSIZE  256   /* power of 2; max records in-flight at once */

typedef struct {
    WALRecord       rec;
    int             done;    /* protected by g_qmtx */
    int             result;
} WALSlot;

static WALSlot          *g_q[WAL_QSIZE];
static uint64_t          g_prod      = 0;
static uint64_t          g_cons      = 0;
static int               g_shutdown  = 0;
static int               g_io_failed = 0;

static pthread_mutex_t   g_qmtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_work  = PTHREAD_COND_INITIALIZER; /* prod → writer */
static pthread_cond_t    g_space = PTHREAD_COND_INITIALIZER; /* writer → prod (queue has room) */
static pthread_cond_t    g_flush = PTHREAD_COND_INITIALIZER; /* writer → prod (record durable) */

static int                  g_wal_fd = -1;
static _Atomic uint64_t     g_last_lsn = 0;
static pthread_t            g_writer_tid;
/* Only the writer thread updates these; shutdown reads them after pthread_join. */
static uint64_t             g_records_written = 0;
static uint64_t             g_fsync_calls = 0;

static void copy_bounded(char *dst, size_t capacity, const char *src) {
    size_t len = strnlen(src, capacity - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ── Record builder ──────────────────────────────────────────────────────── */

static void build_record(WALRecord *r, WALType type,
                         uint64_t account_id,
                         const char *u1, const char *u2,
                         const char *hash, const char *salt,
                         int64_t amount, int64_t b1, int64_t b2,
                         uint64_t lsn) {
    memset(r, 0, sizeof(*r));
    r->magic        = WAL_MAGIC;
    r->version      = WAL_VERSION;
    r->type         = (uint8_t)type;
    r->rec_size     = (uint16_t)sizeof(WALRecord);
    r->lsn          = lsn;
    r->account_id   = account_id;
    if (u1)   copy_bounded(r->username1, sizeof(r->username1), u1);
    if (u2)   copy_bounded(r->username2, sizeof(r->username2), u2);
    if (hash) copy_bounded(r->password_hash, sizeof(r->password_hash), hash);
    if (salt) copy_bounded(r->salt, sizeof(r->salt), salt);
    r->amount_paise = amount;
    r->bal1_after   = b1;
    r->bal2_after   = b2;
    r->crc32 = crc32_of(r, offsetof(WALRecord, crc32));
}

/* ── Writer thread ────────────────────────────────────────────────────────── */

static void *writer_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_qmtx);
    for (;;) {
        /* Wait for work or shutdown */
        while (g_cons == g_prod && !g_shutdown)
            pthread_cond_wait(&g_work, &g_qmtx);

        if (g_cons == g_prod && g_shutdown)
            break;

        /* Snapshot the batch while holding the mutex (records are small) */
        uint64_t  from  = g_cons;
        uint64_t  to    = g_prod;
        uint64_t  n     = to - from;
        WALRecord batch[WAL_QSIZE];
        WALSlot *requests[WAL_QSIZE];
        for (uint64_t i = 0; i < n; i++) {
            requests[i] = g_q[(from + i) & (WAL_QSIZE - 1)];
            batch[i] = requests[i]->rec;
        }

        pthread_mutex_unlock(&g_qmtx);

        /* Write + fsync without holding the mutex so producers can keep enqueuing */
        int ok = 1;
        for (uint64_t i = 0; i < n && ok; i++) {
            if (write(g_wal_fd, &batch[i], sizeof(WALRecord)) !=
                    (ssize_t)sizeof(WALRecord))
                ok = 0;
        }
        if (ok) {
            g_fsync_calls++;
            if (fsync(g_wal_fd) < 0) ok = 0;
        }
        if (ok) g_records_written += n;

        /* Mark all slots in this batch as done */
        pthread_mutex_lock(&g_qmtx);
        for (uint64_t i = 0; i < n; i++) {
            WALSlot *s = requests[i];
            s->result = ok ? 0 : -1;
            s->done   = 1;
        }
        if (!ok) g_io_failed = 1;
        g_cons = to;
        pthread_cond_broadcast(&g_flush);
        pthread_cond_broadcast(&g_space);

        if (g_shutdown && g_cons == g_prod)
            break;
    }
    pthread_mutex_unlock(&g_qmtx);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int wal_init(const char *wal_path) {
    /* O_APPEND: every write goes to end regardless of concurrent truncates */
    g_wal_fd = open(wal_path, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (g_wal_fd < 0) { perror("wal_init: open"); return -1; }
    g_prod = g_cons = 0;
    g_shutdown = 0;
    g_io_failed = 0;
    g_records_written = 0;
    g_fsync_calls = 0;
    if (pthread_create(&g_writer_tid, NULL, writer_thread, NULL) != 0) {
        close(g_wal_fd);
        g_wal_fd = -1;
        return -1;
    }
    return 0;
}

int wal_shutdown(void) {
    pthread_mutex_lock(&g_qmtx);
    g_shutdown = 1;
    pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_qmtx);
    pthread_join(g_writer_tid, NULL);
    fprintf(stderr, "WAL stats: records=%" PRIu64 " fsyncs=%" PRIu64
            " avg_batch=%.2f\n", g_records_written, g_fsync_calls,
            g_fsync_calls ? (double)g_records_written / (double)g_fsync_calls : 0.0);
    int rc = g_io_failed ? -1 : 0;
    if (g_wal_fd >= 0) {
        if (fsync(g_wal_fd) < 0) { perror("wal_shutdown: fsync"); rc = -1; }
        if (close(g_wal_fd) < 0) { perror("wal_shutdown: close"); rc = -1; }
        g_wal_fd = -1;
    }
    return rc;
}

int wal_append(WALType type,
               uint64_t account_id,
               const char *username1, const char *username2,
               const char *password_hash, const char *salt,
               int64_t amount_paise,
               int64_t bal1_after, int64_t bal2_after) {
    pthread_mutex_lock(&g_qmtx);

    /* Block until there is a free slot */
    while (g_prod - g_cons >= WAL_QSIZE)
        pthread_cond_wait(&g_space, &g_qmtx);
    if (g_io_failed || g_shutdown) {
        pthread_mutex_unlock(&g_qmtx);
        return -1;
    }

    uint64_t lsn  = atomic_fetch_add_explicit(&g_last_lsn, 1,
                                               memory_order_relaxed) + 1;
    WALSlot request = {0};
    g_q[g_prod & (WAL_QSIZE - 1)] = &request;
    build_record(&request.rec, type, account_id, username1, username2,
                 password_hash, salt, amount_paise, bal1_after, bal2_after, lsn);
    g_prod++;
    pthread_cond_signal(&g_work);

    /* Block until this record has been fsynced */
    while (!request.done)
        pthread_cond_wait(&g_flush, &g_qmtx);
    int rc = request.result;
    pthread_mutex_unlock(&g_qmtx);
    return rc;
}

int wal_truncate(void) {
    if (g_wal_fd < 0) return -1;
    /* Acquire g_qmtx to establish happens-before with the writer thread's last write,
     * ensuring TSan sees that the queue is empty before we touch the fd. */
    pthread_mutex_lock(&g_qmtx);
    pthread_mutex_unlock(&g_qmtx);
    if (ftruncate(g_wal_fd, 0) < 0) { perror("wal_truncate: ftruncate"); return -1; }
    if (lseek(g_wal_fd, 0, SEEK_SET) < 0) { perror("wal_truncate: lseek"); return -1; }
    if (fsync(g_wal_fd) < 0) { perror("wal_truncate: fsync"); return -1; }
    return 0;
}

uint64_t wal_last_lsn(void) {
    return atomic_load_explicit(&g_last_lsn, memory_order_acquire);
}

/* ── Replay (single-threaded startup) ───────────────────────────────────────
 *
 * Reads the WAL file and calls accounts_apply_wal_record() for each valid
 * record whose lsn > from_lsn.  Stops at the first bad magic/CRC.
 */
int wal_replay(const char *wal_path, uint64_t from_lsn) {
    atomic_store_explicit(&g_last_lsn, from_lsn, memory_order_release);
    int fd = open(wal_path, O_RDWR);
    if (fd < 0) {
        if (errno == ENOENT) return 0;  /* no WAL yet, that's fine */
        perror("wal_replay: open");
        return -1;
    }

    WALRecord r;
    uint64_t  replayed = 0;
    uint64_t  skipped  = 0;
    off_t valid_end = 0;

    for (;;) {
        ssize_t n = read(fd, &r, sizeof(r));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;  /* clean EOF */
        if (n != (ssize_t)sizeof(r)) {
            fprintf(stderr, "WAL: truncated record (%" PRIuPTR " bytes) "
                    "after %" PRIu64 " records — stopping replay\n",
                    (size_t)n, replayed + skipped);
            break;
        }

        if (r.magic    != WAL_MAGIC ||
            r.version  != WAL_VERSION ||
            r.rec_size != (uint16_t)sizeof(WALRecord)) {
            fprintf(stderr, "WAL: bad header at lsn %" PRIu64
                    " — stopping replay\n", r.lsn);
            break;
        }

        uint32_t expected = crc32_of(&r, offsetof(WALRecord, crc32));
        if (r.crc32 != expected) {
            fprintf(stderr, "WAL: CRC mismatch at lsn %" PRIu64
                    " — stopping replay\n", r.lsn);
            break;
        }

        valid_end += (off_t)sizeof(r);
        if (r.lsn <= from_lsn) { skipped++; continue; }

        if (accounts_apply_wal_record(&r) != 0) {
            fprintf(stderr, "WAL: invalid account reference or record at lsn %" PRIu64 "\n", r.lsn);
            close(fd);
            return -1;
        }
        uint64_t prev = atomic_load_explicit(&g_last_lsn,
                                              memory_order_relaxed);
        while (r.lsn > prev &&
               !atomic_compare_exchange_weak_explicit(&g_last_lsn, &prev,
                                                       r.lsn,
                                                       memory_order_release,
                                                       memory_order_relaxed)) {}

        replayed++;
    }

    if (ftruncate(fd, valid_end) < 0 || fsync(fd) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    fprintf(stderr, "WAL: replayed %" PRIu64 " record(s)"
            " (skipped %" PRIu64 " already in snapshot)\n",
            replayed, skipped);
    return 0;
}
