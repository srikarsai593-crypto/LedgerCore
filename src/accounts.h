#ifndef ACCOUNTS_H
#define ACCOUNTS_H

#include "protocol.h"
#include "wal.h"
#include <pthread.h>
#include <stdint.h>

/*
 * Locking design:
 *
 * Every mutating operation follows this ordering (never reversed):
 *   g_ckpt_rwlock(R) → g_reg_rwlock(W or R) → account.lock(s, ascending id)
 *
 * Read-only operations use:
 *   g_reg_rwlock(R) → account.lock
 *
 * The checkpoint takes g_ckpt_rwlock(W), excluding every in-flight mutation
 * until its WAL record is durable and its new balance is applied. The snapshot
 * can then read all balances without per-account locks. Independent accounts
 * can append concurrently, allowing WAL group commit to batch fsyncs.
 *
 * Transfers lock both accounts in ascending account_id order to prevent
 * deadlock (classic two-resource ordering).
 *
 * All public operations manage their own locks.  Callers never touch locks.
 */

typedef struct {
    uint64_t        account_id;
    char            username[MAX_USERNAME];
    char            password_hash[MAX_HASH];
    char            salt[SALT_HEX_LEN];
    int64_t         balance_paise;   /* balance in smallest currency unit */
    int             active;
    pthread_mutex_t lock;
} Account;

/* ---------- lifecycle ---------- */

/*
 * Call once at startup.  Loads the snapshot from snap_file; stores the
 * snapshot LSN internally (retrieve with accounts_snap_lsn()).
 * Returns 0 on success, -1 on error.
 */
int  accounts_init(const char *snap_file);

/* Returns the snapshot LSN loaded by accounts_init (0 if no snapshot). */
uint64_t accounts_snap_lsn(void);

/*
 * Write a snapshot of all accounts to disk (atomic rename), then call
 * wal_truncate() to empty the WAL. Acquires g_ckpt_rwlock(W) internally.
 * This is also called by accounts_shutdown().
 * Returns 0 on success, -1 on error.
 */
int  accounts_checkpoint(void);

/* Start after wal_init; the worker checkpoints every 500 ops or 3 seconds. */
int  accounts_checkpoint_start(void);

/* Stop without a final checkpoint (used by the in-process crash test). */
void accounts_checkpoint_stop(void);

/* Call at shutdown: final persist + destroy per-account mutexes. Returns -1 on checkpoint error. */
int accounts_shutdown(void);

/* ---------- operations (all locking handled internally) ---------- */

/*
 * Create a new account.
 * Returns 0 on success, -1 if username is taken or account limit reached.
 */
int accounts_register(const char *username, const char *password);

/*
 * Check credentials.
 * Returns 1 on match, 0 on failure.
 */
int accounts_authenticate(const char *username, const char *password);

/*
 * Read current balance.
 * Returns 0 on success and writes balance (paise) to *bal_out;
 * returns -1 if account not found.
 */
int accounts_get_balance(const char *username, int64_t *bal_out);

/*
 * Deposit paise into account.
 * Returns 0 on success; -1 on invalid amount or unknown account.
 * *bal_out receives the new balance.
 */
int accounts_deposit(const char *username, int64_t paise, int64_t *bal_out);

/*
 * Withdraw paise from account.
 * Returns 0 on success; -1 on insufficient funds, invalid amount, or
 * unknown account.  *bal_out is always set to the current balance (useful
 * for error messages even on failure).
 */
int accounts_withdraw(const char *username, int64_t paise, int64_t *bal_out);

/*
 * Transfer paise from src_user to dst_user atomically.
 * Returns 0 on success; -1 on insufficient funds, unknown account, or
 * src==dst.  *src_bal_out is set to src's balance at the point of failure
 * (or the new balance on success).
 */
int accounts_transfer(const char *src_user, const char *dst_user,
                      int64_t paise, int64_t *src_bal_out);

/*
 * Apply a single WAL record to the in-memory account store.
 * Called by wal_replay() during single-threaded startup; no locking needed.
 */
int accounts_apply_wal_record(const WALRecord *r);

#endif /* ACCOUNTS_H */
