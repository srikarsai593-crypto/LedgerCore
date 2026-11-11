#ifndef PROTOCOL_H
#define PROTOCOL_H

/* Line-based text protocol constants */
#define PROTO_OK  "OK"
#define PROTO_ERR "ERR"

/* Buffer sizes (always larger than the corresponding content limits) */
#define MAX_USERNAME   64    /* buffer; content validated to MAX_USERNAME_LEN */
#define MAX_PASSWORD   128
#define MAX_LINE       512
#define MAX_HASH       129   /* 128 hex chars + NUL */
#define SALT_LEN       16    /* bytes of random salt */
#define SALT_HEX_LEN   33    /* hex-encoded + NUL */

/* Content-length limits for validation */
#define MIN_USERNAME_LEN  3
#define MAX_USERNAME_LEN  32
#define MIN_PASSWORD_LEN  8

/* Thread pool / queue sizing */
#define THREADPOOL_SIZE   64
#define QUEUE_CAPACITY    256

/* Account limits */
#define MAX_ACCOUNTS      2048
#define HISTORY_PER_ACCT  50

/* Server defaults */
#define DEFAULT_PORT      8080
#define BACKLOG           128

/* Per-connection idle timeout (SO_RCVTIMEO), seconds */
#define CONN_IDLE_TIMEOUT_SEC  60

/* Max failed LOGIN attempts before the connection is closed */
#define MAX_FAILED_LOGINS  5

/* Persistence files */
#define SNAP_FILE      "ledgercore.snap"       /* account snapshot (text)   */
#define SNAP_FILE_TMP  "ledgercore.snap.tmp"   /* atomic-write temp         */
#define WAL_FILE       "ledgercore.wal"        /* write-ahead log (binary)  */
#define AUDIT_FILE     "ledgercore_audit.log"
/* Exclusive lock file — prevents two server instances sharing one data dir */
#define LOCK_FILE      "ledgercore.lock"

/* WAL tuning */
#define WAL_CHECKPOINT_INTERVAL  500   /* checkpoint every N appended records */

#endif /* PROTOCOL_H */
