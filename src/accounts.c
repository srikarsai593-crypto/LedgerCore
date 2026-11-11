/*
 * accounts.c – in-memory account store backed by WAL + snapshot.
 *
 * Snapshot format (text, one header line + one line per account):
 *   # LedgerCore snapshot lsn=<N> count=<M>
 *   account_id:username:password_hash:salt:balance_paise
 *
 * Durability model:
 *   Every mutating operation appends a WAL record and blocks until it is
 *   fsynced (via wal_append).  The accounts file is only rewritten at
 *   checkpoints (every WAL_CHECKPOINT_INTERVAL ops or on clean shutdown).
 *   Recovery: load snapshot → replay WAL records with lsn > snap_lsn.
 *
 * Lock ordering:
 *   g_ckpt_rwlock(R) → g_reg_rwlock(W or R) → account.lock(s, ascending id)
 * A checkpoint takes g_ckpt_rwlock(W) after each mutation has both reached
 * durable WAL and applied its new balance, so its snapshot is a clean prefix.
 */

#include "accounts.h"
#include "auth.h"
#include "wal.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <time.h>
#include <ctype.h>
#include <openssl/crypto.h>

/* ── global state ─────────────────────────────────────────────────────────── */

static Account          g_accounts[MAX_ACCOUNTS];
#define NAME_MAP_CAP 4096  /* power of two; at most half full */
static int              g_name_slots[NAME_MAP_CAP]; /* account index + 1 */
static int              g_num_accounts = 0;
static uint64_t         g_next_id      = 1;
static uint64_t         g_snap_lsn     = 0;  /* LSN at time of last snapshot */

static pthread_rwlock_t g_reg_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t g_ckpt_rwlock = PTHREAD_RWLOCK_INITIALIZER;

static char g_snap_file[256];

/* Op counter for checkpoint triggering */
static atomic_uint_fast64_t g_op_count = 0;
static pthread_mutex_t g_ckpt_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ckpt_cond = PTHREAD_COND_INITIALIZER;
static pthread_t       g_ckpt_tid;
static int             g_ckpt_started = 0;
static int             g_ckpt_stop = 0;

static void *checkpoint_worker(void *unused) {
    (void)unused;
    uint64_t last_done = 0;
    pthread_mutex_lock(&g_ckpt_mtx);
    while (!g_ckpt_stop) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 3;
        while (!g_ckpt_stop &&
               atomic_load(&g_op_count) - last_done < WAL_CHECKPOINT_INTERVAL) {
            if (pthread_cond_timedwait(&g_ckpt_cond, &g_ckpt_mtx,
                                       &deadline) == ETIMEDOUT) break;
        }
        if (g_ckpt_stop) break;
        uint64_t before = atomic_load(&g_op_count);
        if (before == last_done) continue;
        pthread_mutex_unlock(&g_ckpt_mtx);

        /* The checkpoint write lock waits for all WAL+apply readers to finish.
         * Use the pre-checkpoint count conservatively: later operations may
         * start just after the lock is released and need another checkpoint. */
        if (accounts_checkpoint() == 0) last_done = before;
        else fprintf(stderr, "background checkpoint failed; WAL retained\n");

        pthread_mutex_lock(&g_ckpt_mtx);
    }
    pthread_mutex_unlock(&g_ckpt_mtx);
    return NULL;
}

int accounts_checkpoint_start(void) {
    pthread_mutex_lock(&g_ckpt_mtx);
    if (g_ckpt_started) {
        pthread_mutex_unlock(&g_ckpt_mtx);
        return 0;
    }
    g_ckpt_stop = 0;
    if (pthread_create(&g_ckpt_tid, NULL, checkpoint_worker, NULL) != 0) {
        pthread_mutex_unlock(&g_ckpt_mtx);
        return -1;
    }
    g_ckpt_started = 1;
    pthread_mutex_unlock(&g_ckpt_mtx);
    return 0;
}

void accounts_checkpoint_stop(void) {
    pthread_mutex_lock(&g_ckpt_mtx);
    if (!g_ckpt_started) {
        pthread_mutex_unlock(&g_ckpt_mtx);
        return;
    }
    g_ckpt_stop = 1;
    pthread_cond_signal(&g_ckpt_cond);
    pthread_mutex_unlock(&g_ckpt_mtx);
    pthread_join(g_ckpt_tid, NULL);
    pthread_mutex_lock(&g_ckpt_mtx);
    g_ckpt_started = 0;
    pthread_mutex_unlock(&g_ckpt_mtx);
}

static void copy_bounded(char *dst, size_t capacity, const char *src) {
    size_t len = strnlen(src, capacity - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int sync_snapshot_directory(void) {
    char dir[sizeof(g_snap_file)];
    const char *slash = strrchr(g_snap_file, '/');
    if (slash) {
        size_t len = (size_t)(slash - g_snap_file);
        if (len >= sizeof(dir)) return -1;
        if (len == 0) len = 1;
        memcpy(dir, g_snap_file, len);
        dir[len] = '\0';
    } else {
        strcpy(dir, ".");
    }
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) < 0) rc = -1;
    return rc;
}

/* ── internal helpers ─────────────────────────────────────────────────────── */

static Account *find_account_unlocked(const char *username) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)username; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    for (size_t i = 0; i < NAME_MAP_CAP; i++) {
        int slot = g_name_slots[(hash + i) & (NAME_MAP_CAP - 1)];
        if (!slot) return NULL;
        Account *a = &g_accounts[slot - 1];
        if (a->active && strcmp(a->username, username) == 0) return a;
    }
    return NULL;
}

/* Caller holds the registry write lock, or is in single-threaded startup. */
static void index_account_unlocked(int index) {
    const unsigned char *p = (const unsigned char *)g_accounts[index].username;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *p; p++) { hash ^= *p; hash *= UINT64_C(1099511628211); }
    for (size_t i = 0; i < NAME_MAP_CAP; i++) {
        size_t pos = (hash + i) & (NAME_MAP_CAP - 1);
        if (!g_name_slots[pos]) { g_name_slots[pos] = index + 1; return; }
    }
}

static int valid_hex(const char *s, size_t len) {
    if (strlen(s) != len) return 0;
    for (size_t i = 0; i < len; i++)
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static int valid_credential_fields(const char *user, const char *hash,
                                   const char *salt) {
    size_t n = strlen(user);
    if (n < MIN_USERNAME_LEN || n > MAX_USERNAME_LEN ||
        !valid_hex(salt, SALT_HEX_LEN - 1)) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((user[i] >= 'A' && user[i] <= 'Z') ||
              (user[i] >= 'a' && user[i] <= 'z') ||
              (user[i] >= '0' && user[i] <= '9') || user[i] == '_')) return 0;
    if (strncmp(hash, "pbkdf2_sha256$", 14) == 0) {
        const char *digits = hash + 14, *end = strchr(digits, '$');
        if (!end || end == digits || (size_t)(end - digits) > 7) return 0;
        for (const char *p = digits; p < end; p++) if (!isdigit((unsigned char)*p)) return 0;
        long iterations = strtol(digits, NULL, 10);
        return iterations >= 1 && iterations <= 10000000 && valid_hex(end + 1, 64);
    }
    return valid_hex(hash, 64);
}

static Account *find_by_id_unlocked(uint64_t id) {
    for (int i = 0; i < g_num_accounts; i++) {
        Account *a = &g_accounts[i];
        if (a->active && a->account_id == id)
            return a;
    }
    return NULL;
}

/* ── snapshot I/O ─────────────────────────────────────────────────────────── */

/*
 * Write all active accounts to SNAP_FILE_TMP, fsync, rename to g_snap_file,
 * then truncate the WAL. Caller holds the checkpoint write lock.
 */
static int checkpoint_locked(void) {
    char tmp[280];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_snap_file);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("checkpoint: open"); return -1; }

    FILE *fp = fdopen(fd, "w");
    if (!fp) { perror("checkpoint: fdopen"); close(fd); return -1; }

    uint64_t snap_lsn = wal_last_lsn();
    fprintf(fp, "# LedgerCore snapshot lsn=%" PRIu64 " count=%d\n",
            snap_lsn, g_num_accounts);

    for (int i = 0; i < g_num_accounts; i++) {
        Account *a = &g_accounts[i];
        if (!a->active) continue;
        fprintf(fp, "%" PRIu64 ":%s:%s:%s:%" PRId64 "\n",
                a->account_id, a->username,
                a->password_hash, a->salt,
                a->balance_paise);
    }

    if (fflush(fp) != 0) { fclose(fp); return -1; }
    if (fsync(fd)   != 0) { fclose(fp); return -1; }
    if (fclose(fp) != 0) return -1;

    if (rename(tmp, g_snap_file) < 0) { perror("checkpoint: rename"); return -1; }
    if (sync_snapshot_directory() < 0) {
        perror("checkpoint: fsync directory");
        return -1;
    }

    g_snap_lsn = snap_lsn;
    return wal_truncate();
}

int accounts_checkpoint(void) {
    pthread_rwlock_wrlock(&g_ckpt_rwlock);
    int rc = checkpoint_locked();
    pthread_rwlock_unlock(&g_ckpt_rwlock);
    return rc;
}

static int load_snapshot(void) {
    FILE *fp = fopen(g_snap_file, "r");
    if (!fp) {
        if (errno == ENOENT) return 0;  /* first run */
        perror("load_snapshot");
        return -1;
    }

    char line[MAX_LINE], extra;
    unsigned count;
    uint64_t lsn;
    if (!fgets(line, sizeof(line), fp) || !strchr(line, '\n') ||
        sscanf(line, "# LedgerCore snapshot lsn=%" SCNu64 " count=%u %c",
               &lsn, &count, &extra) != 2 || count > MAX_ACCOUNTS) goto corrupt;
    char canonical[MAX_LINE];
    int header_len = snprintf(canonical, sizeof(canonical),
                              "# LedgerCore snapshot lsn=%" PRIu64 " count=%u\n",
                              lsn, count);
    if (header_len < 0 || header_len >= (int)sizeof(canonical) ||
        strcmp(line, canonical) != 0) goto corrupt;
    g_snap_lsn = lsn;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (!len || line[len - 1] != '\n' || g_num_accounts >= (int)count)
            goto corrupt;
        line[len - 1] = '\0';
        char *parts[5], *p = line;
        for (int j = 0; j < 5; j++) {
            parts[j] = strsep(&p, ":");
            if (!parts[j]) goto corrupt;
        }
        if (p || !valid_credential_fields(parts[1], parts[2], parts[3])) goto corrupt;
        for (const char *q = parts[0]; *q; q++)
            if (!isdigit((unsigned char)*q)) goto corrupt;
        for (const char *q = parts[4]; *q; q++)
            if (!isdigit((unsigned char)*q)) goto corrupt;
        char *end;
        errno = 0;
        uint64_t id = strtoull(parts[0], &end, 10);
        if (errno || end == parts[0] || *end || !id || id == UINT64_MAX) goto corrupt;
        errno = 0;
        int64_t balance = strtoll(parts[4], &end, 10);
        if (errno || end == parts[4] || *end || balance < 0 ||
            find_account_unlocked(parts[1]) || find_by_id_unlocked(id)) goto corrupt;
        Account *a = &g_accounts[g_num_accounts];
        memset(a, 0, sizeof(*a));
        a->account_id = id;
        copy_bounded(a->username, sizeof(a->username), parts[1]);
        copy_bounded(a->password_hash, sizeof(a->password_hash), parts[2]);
        copy_bounded(a->salt, sizeof(a->salt), parts[3]);
        a->balance_paise = balance;
        if (pthread_mutex_init(&a->lock, NULL) != 0) goto corrupt;
        a->active = 1;
        index_account_unlocked(g_num_accounts);
        if (id >= g_next_id) g_next_id = id + 1;
        g_num_accounts++;
    }
    if (ferror(fp) || g_num_accounts != (int)count) goto corrupt;
    if (fclose(fp) != 0) return -1;
    return 0;

corrupt:
    fprintf(stderr, "load_snapshot: invalid snapshot %s\n", g_snap_file);
    fclose(fp);
    return -1;
}

/* ── lifecycle ─────────────────────────────────────────────────────────────── */

int accounts_init(const char *snap_file) {
    for (int i = 0; i < g_num_accounts; i++)
        if (g_accounts[i].active) pthread_mutex_destroy(&g_accounts[i].lock);
    g_num_accounts = 0;
    g_next_id = 1;
    g_snap_lsn = 0;
    memset(g_name_slots, 0, sizeof(g_name_slots));
    atomic_store(&g_op_count, 0);
    copy_bounded(g_snap_file, sizeof(g_snap_file), snap_file);
    memset(g_accounts, 0, sizeof(g_accounts));
    return load_snapshot();
}

uint64_t accounts_snap_lsn(void) {
    return g_snap_lsn;
}

int accounts_shutdown(void) {
    accounts_checkpoint_stop();
    int rc = accounts_checkpoint();

    pthread_rwlock_wrlock(&g_reg_rwlock);
    for (int i = 0; i < g_num_accounts; i++) {
        if (g_accounts[i].active) {
            pthread_mutex_destroy(&g_accounts[i].lock);
            g_accounts[i].active = 0;
        }
    }
    g_num_accounts = 0;
    pthread_rwlock_unlock(&g_reg_rwlock);
    return rc;
}

/* ── checkpoint trigger ───────────────────────────────────────────────────── */

/* The request path only signals; the checkpoint worker does the I/O. */
static void maybe_checkpoint(void) {
    uint64_t count = atomic_fetch_add(&g_op_count, 1) + 1;
    if (count % WAL_CHECKPOINT_INTERVAL == 0) {
        pthread_mutex_lock(&g_ckpt_mtx);
        if (g_ckpt_started) pthread_cond_signal(&g_ckpt_cond);
        pthread_mutex_unlock(&g_ckpt_mtx);
    }
}

/* ── WAL record application (used during replay) ─────────────────────────── */

int accounts_apply_wal_record(const WALRecord *r) {
    if (strnlen(r->username1, sizeof(r->username1)) == sizeof(r->username1) ||
        strnlen(r->username2, sizeof(r->username2)) == sizeof(r->username2))
        return -1;
    switch ((WALType)r->type) {

    case WAL_REGISTER: {
        if (g_num_accounts >= MAX_ACCOUNTS || !r->account_id ||
            r->account_id == UINT64_MAX ||
            strnlen(r->username1, sizeof(r->username1)) == sizeof(r->username1) ||
            strnlen(r->password_hash, sizeof(r->password_hash)) == sizeof(r->password_hash) ||
            strnlen(r->salt, sizeof(r->salt)) == sizeof(r->salt) ||
            !valid_credential_fields(r->username1, r->password_hash, r->salt) ||
            find_by_id_unlocked(r->account_id) ||
            find_account_unlocked(r->username1)) return -1;
        Account *a = &g_accounts[g_num_accounts];
        memset(a, 0, sizeof(*a));
        a->account_id = r->account_id;
        copy_bounded(a->username, sizeof(a->username), r->username1);
        copy_bounded(a->password_hash, sizeof(a->password_hash), r->password_hash);
        copy_bounded(a->salt, sizeof(a->salt), r->salt);
        a->balance_paise = 0;
        if (pthread_mutex_init(&a->lock, NULL) != 0) return -1;
        a->active = 1;
        index_account_unlocked(g_num_accounts);
        if (a->account_id >= g_next_id)
            g_next_id = a->account_id + 1;
        g_num_accounts++;
        return 0;
    }

    case WAL_DEPOSIT:
    case WAL_WITHDRAW: {
        Account *a = find_account_unlocked(r->username1);
        if (!a || a->account_id != r->account_id ||
            r->bal1_after < 0 || r->amount_paise <= 0) return -1;
        a->balance_paise = r->bal1_after;
        return 0;
    }

    case WAL_TRANSFER: {
        Account *src = find_account_unlocked(r->username1);
        Account *dst = find_account_unlocked(r->username2);
        if (!src || !dst || src == dst || src->account_id != r->account_id ||
            r->bal1_after < 0 || r->bal2_after < 0 ||
            r->amount_paise <= 0) return -1;
        src->balance_paise = r->bal1_after;
        dst->balance_paise = r->bal2_after;
        return 0;
    }
    }
    return -1;
}

/* ── public operations ────────────────────────────────────────────────────── */

int accounts_register(const char *username, const char *password) {
    char salt[SALT_HEX_LEN], hash[MAX_HASH];
    /* PBKDF2 is intentionally outside the registry write lock. Concurrent
     * registrations may derive hashes while balance operations proceed. */
    if (generate_salt(salt, sizeof(salt)) != 0 ||
        hash_password(password, salt, hash, sizeof(hash)) != 0) return -1;
    pthread_rwlock_rdlock(&g_ckpt_rwlock);
    pthread_rwlock_wrlock(&g_reg_rwlock);

    if (find_account_unlocked(username) || g_num_accounts >= MAX_ACCOUNTS) {
        pthread_rwlock_unlock(&g_reg_rwlock);
        pthread_rwlock_unlock(&g_ckpt_rwlock);
        return -1;
    }

    Account *a = &g_accounts[g_num_accounts];
    memset(a, 0, sizeof(*a));
    a->account_id    = g_next_id;
    copy_bounded(a->username, sizeof(a->username), username);
    copy_bounded(a->salt, sizeof(a->salt), salt);
    copy_bounded(a->password_hash, sizeof(a->password_hash), hash);
    a->balance_paise = 0;
    pthread_mutex_init(&a->lock, NULL);

    int rc = wal_append(WAL_REGISTER, a->account_id,
                        a->username, NULL,
                        a->password_hash, a->salt,
                        0, 0, 0);
    if (rc == 0) {
        a->active = 1;
        index_account_unlocked(g_num_accounts);
        g_num_accounts++;
        g_next_id++;
    } else {
        pthread_mutex_destroy(&a->lock);
        memset(a, 0, sizeof(*a));
    }
    pthread_rwlock_unlock(&g_reg_rwlock);
    pthread_rwlock_unlock(&g_ckpt_rwlock);
    if (rc == 0) maybe_checkpoint();
    return rc;
}

int accounts_authenticate(const char *username, const char *password) {
    pthread_rwlock_rdlock(&g_reg_rwlock);
    Account *a = find_account_unlocked(username);
    if (!a) { pthread_rwlock_unlock(&g_reg_rwlock); return 0; }

    char salt[SALT_HEX_LEN], hash[MAX_HASH];
    pthread_mutex_lock(&a->lock);
    memcpy(salt, a->salt, sizeof(salt));
    memcpy(hash, a->password_hash, sizeof(hash));
    pthread_mutex_unlock(&a->lock);
    pthread_rwlock_unlock(&g_reg_rwlock);

    /* Credentials are immutable after registration. Verify the local copies
     * without holding an account lock across the expensive PBKDF2 call. */
    int ok = verify_password(password, salt, hash);
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(hash, sizeof(hash));
    return ok;
}

int accounts_get_balance(const char *username, int64_t *bal_out) {
    pthread_rwlock_rdlock(&g_reg_rwlock);
    Account *a = find_account_unlocked(username);
    if (!a) { pthread_rwlock_unlock(&g_reg_rwlock); return -1; }

    pthread_mutex_lock(&a->lock);
    *bal_out = a->balance_paise;
    pthread_mutex_unlock(&a->lock);

    pthread_rwlock_unlock(&g_reg_rwlock);
    return 0;
}

int accounts_deposit(const char *username, int64_t paise, int64_t *bal_out) {
    if (paise <= 0) return -1;

    pthread_rwlock_rdlock(&g_ckpt_rwlock);
    pthread_rwlock_rdlock(&g_reg_rwlock);

    Account *a = find_account_unlocked(username);
    if (!a) {
        pthread_rwlock_unlock(&g_reg_rwlock);
        pthread_rwlock_unlock(&g_ckpt_rwlock);
        return -1;
    }

    pthread_mutex_lock(&a->lock);
    if (paise > INT64_MAX - a->balance_paise) {
        *bal_out = a->balance_paise;
        pthread_mutex_unlock(&a->lock);
        pthread_rwlock_unlock(&g_reg_rwlock);
        pthread_rwlock_unlock(&g_ckpt_rwlock);
        return -1;
    }
    int64_t next_balance = a->balance_paise + paise;
    int rc = wal_append(WAL_DEPOSIT, a->account_id,
                        username, NULL, NULL, NULL,
                        paise, next_balance, 0);
    if (rc == 0) {
        a->balance_paise = next_balance;
    }
    *bal_out = a->balance_paise;
    pthread_mutex_unlock(&a->lock);

    pthread_rwlock_unlock(&g_reg_rwlock);
    pthread_rwlock_unlock(&g_ckpt_rwlock);
    if (rc == 0) maybe_checkpoint();
    return rc;
}

int accounts_withdraw(const char *username, int64_t paise, int64_t *bal_out) {
    if (paise <= 0) return -1;

    pthread_rwlock_rdlock(&g_ckpt_rwlock);
    pthread_rwlock_rdlock(&g_reg_rwlock);

    Account *a = find_account_unlocked(username);
    if (!a) {
        pthread_rwlock_unlock(&g_reg_rwlock);
        pthread_rwlock_unlock(&g_ckpt_rwlock);
        *bal_out = 0;
        return -1;
    }

    pthread_mutex_lock(&a->lock);
    *bal_out = a->balance_paise;
    if (a->balance_paise < paise) {
        pthread_mutex_unlock(&a->lock);
        pthread_rwlock_unlock(&g_reg_rwlock);
        pthread_rwlock_unlock(&g_ckpt_rwlock);
        return -1;
    }
    int64_t next_balance = a->balance_paise - paise;
    int rc = wal_append(WAL_WITHDRAW, a->account_id,
                        username, NULL, NULL, NULL,
                        paise, next_balance, 0);
    if (rc == 0) {
        a->balance_paise = next_balance;
    }
    *bal_out = a->balance_paise;
    pthread_mutex_unlock(&a->lock);

    pthread_rwlock_unlock(&g_reg_rwlock);
    pthread_rwlock_unlock(&g_ckpt_rwlock);
    if (rc == 0) maybe_checkpoint();
    return rc;
}

int accounts_transfer(const char *src_user, const char *dst_user,
                      int64_t paise, int64_t *src_bal_out) {
    if (paise <= 0) return -1;

    pthread_rwlock_rdlock(&g_ckpt_rwlock);
    pthread_rwlock_rdlock(&g_reg_rwlock);

    Account *src = find_account_unlocked(src_user);
    Account *dst = find_account_unlocked(dst_user);

    if (!src || !dst || src == dst) {
        pthread_rwlock_unlock(&g_reg_rwlock);
        pthread_rwlock_unlock(&g_ckpt_rwlock);
        if (src_bal_out) *src_bal_out = 0;
        return -1;
    }

    Account *first  = (src->account_id < dst->account_id) ? src : dst;
    Account *second = (src->account_id < dst->account_id) ? dst : src;

    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);

    *src_bal_out = src->balance_paise;
    int rc = -1;
    if (src->balance_paise >= paise &&
        paise <= INT64_MAX - dst->balance_paise) {
        int64_t next_src = src->balance_paise - paise;
        int64_t next_dst = dst->balance_paise + paise;
        rc = wal_append(WAL_TRANSFER, src->account_id,
                        src_user, dst_user, NULL, NULL,
                        paise, next_src, next_dst);
        if (rc == 0) {
            src->balance_paise = next_src;
            dst->balance_paise = next_dst;
        }
        *src_bal_out = src->balance_paise;
    }

    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
    pthread_rwlock_unlock(&g_reg_rwlock);
    pthread_rwlock_unlock(&g_ckpt_rwlock);
    if (rc == 0) maybe_checkpoint();
    return rc;
}
