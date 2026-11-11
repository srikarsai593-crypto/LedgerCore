#ifndef WAL_H
#define WAL_H

#include "protocol.h"
#include <stdint.h>

/* ── Record types ─────────────────────────────────────────────────────────── */

typedef enum {
    WAL_REGISTER = 1,
    WAL_DEPOSIT  = 2,
    WAL_WITHDRAW = 3,
    WAL_TRANSFER = 4,
} WALType;

#define WAL_MAGIC   0x4C435257u   /* "LCRW" (little-endian) */
#define WAL_VERSION 1u

/*
 * Fixed-size binary WAL record (342 bytes, packed).
 *
 * Field usage by type:
 *   REGISTER : account_id, username1, password_hash, salt
 *   DEPOSIT  : username1, amount_paise, bal1_after
 *   WITHDRAW : username1, amount_paise, bal1_after
 *   TRANSFER : username1=src, username2=dst, amount_paise, bal1_after=src_bal, bal2_after=dst_bal
 *
 * crc32 covers bytes [0, offsetof(WALRecord,crc32)) using IEEE CRC-32.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;                     /* WALType */
    uint16_t rec_size;                 /* sizeof(WALRecord) for version safety */
    uint64_t lsn;                      /* monotonically increasing, 1-based */
    uint64_t account_id;               /* REGISTER: new account's id */
    char     username1[MAX_USERNAME];
    char     username2[MAX_USERNAME];  /* TRANSFER: destination; others: empty */
    char     password_hash[MAX_HASH];  /* REGISTER only */
    char     salt[SALT_HEX_LEN];       /* REGISTER only */
    int64_t  amount_paise;
    int64_t  bal1_after;               /* balance of username1 after the op */
    int64_t  bal2_after;               /* TRANSFER: dst balance after; else 0 */
    uint32_t crc32;
} WALRecord;

/* ── API ──────────────────────────────────────────────────────────────────── */

/*
 * wal_init: open/create the WAL file and start the group-commit writer thread.
 * Must be called AFTER wal_replay (so g_last_lsn is already set from replay).
 * Returns 0 on success, -1 on error.
 */
int  wal_init(const char *wal_path);

/*
 * wal_shutdown: flush all pending records, fsync, and stop the writer thread.
 */
int wal_shutdown(void);

/*
 * wal_append: enqueue a record and block until it is durable (fsynced).
 * Safe to call with accounts-layer locks held (WAL layer acquires only its own lock).
 * Returns 0 on success, -1 on I/O error.
 */
int wal_append(WALType type,
               uint64_t account_id,
               const char *username1, const char *username2,
               const char *password_hash, const char *salt,
               int64_t amount_paise,
               int64_t bal1_after, int64_t bal2_after);

/*
 * wal_truncate: recreate the WAL file as empty.  Call while the WAL is still
 * running (before wal_shutdown), immediately after writing a checkpoint snapshot.
 * Returns 0 on success, -1 on error.
 */
int wal_truncate(void);

/*
 * wal_replay: replay all valid records from wal_path whose lsn > from_lsn.
 * Calls accounts_apply_wal_record() for each.
 * Records with bad magic, version mismatch, or CRC error stop replay.
 * Returns 0 (success / no WAL file), -1 on fatal I/O error.
 * Must be called single-threaded before wal_init.
 */
int wal_replay(const char *wal_path, uint64_t from_lsn);

/* Returns the LSN of the last record appended or replayed. */
uint64_t wal_last_lsn(void);

#endif /* WAL_H */
