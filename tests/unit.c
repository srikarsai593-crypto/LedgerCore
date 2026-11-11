/*
 * tests/unit.c – LedgerCore unit tests
 *
 * Tests the accounts layer (register, authenticate, deposit, withdraw,
 * transfer) and WAL crash-recovery round-trip.
 *
 * Build: see Makefile target 'unit'
 * Run:   ./bin/ledgercore_unit
 */

#include "../src/accounts.h"
#include "../src/wal.h"
#include "../src/protocol.h"
#include "../src/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>

/* ── test framework ───────────────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define CHECK(name, expr) do { \
    if (expr) { \
        printf("  PASS  %s\n", name); g_pass++; \
    } else { \
        printf("  FAIL  %s  (line %d)\n", name, __LINE__); g_fail++; \
    } \
} while (0)

/* ── temp file paths (per test suite) ────────────────────────────────────── */

static const char *SNAP = "/tmp/ledgercore_unit.snap";
static const char *WAL  = "/tmp/ledgercore_unit.wal";

static void init_fresh(void) {
    unlink(SNAP);
    unlink(WAL);
    accounts_init(SNAP);
    wal_replay(WAL, accounts_snap_lsn());
    wal_init(WAL);
    accounts_checkpoint_start();
}

static void shutdown_clean(void) {
    accounts_shutdown();   /* checkpoint: writes snap, truncates wal */
    wal_shutdown();
}

/* "crash" shutdown: flush WAL records to disk but skip the checkpoint */
static void shutdown_crash(void) {
    accounts_checkpoint_stop();
    wal_shutdown();        /* fsyncs WAL but does NOT write snapshot */
    /* accounts_shutdown() NOT called: no checkpoint */
}

static void reinit_after_crash(void) {
    accounts_init(SNAP);
    wal_replay(WAL, accounts_snap_lsn());
    wal_init(WAL);
    accounts_checkpoint_start();
}

/* ── test suites ──────────────────────────────────────────────────────────── */

static void test_register_and_auth(void) {
    printf("\n[register + authenticate]\n");
    init_fresh();

    CHECK("register alice",   accounts_register("alice",  "alicepass")  == 0);
    CHECK("register bob",     accounts_register("bob",    "bobpass8")   == 0);
    CHECK("register dupe",    accounts_register("alice",  "alicepass")  != 0);

    CHECK("auth alice ok",    accounts_authenticate("alice", "alicepass") == 1);
    CHECK("auth alice wrong", accounts_authenticate("alice", "wrongpass") == 0);
    CHECK("auth bob ok",      accounts_authenticate("bob",   "bobpass8")  == 1);
    CHECK("auth unknown",     accounts_authenticate("carol", "pass1234")  == 0);

    char formatted_hash[MAX_HASH];
    CHECK("new hash has work factor",
          hash_password("alicepass", "00112233445566778899aabbccddeeff",
                        formatted_hash, sizeof(formatted_hash)) == 0 &&
          strncmp(formatted_hash, "pbkdf2_sha256$600000$", 21) == 0);
    CHECK("legacy 100k hash verifies",
          verify_password("alicepass", "00112233445566778899aabbccddeeff",
              "203e5dceb257e1fb116834ff95166d913e163153e8d75824238029c020bdfcbf") == 1);

    shutdown_clean();
}

static void test_deposit_withdraw(void) {
    printf("\n[deposit + withdraw]\n");
    init_fresh();

    accounts_register("alice", "alicepass");

    int64_t bal = 0;
    CHECK("deposit 10000",     accounts_deposit("alice", 10000, &bal) == 0);
    CHECK("balance after dep", bal == 10000);

    CHECK("deposit 0 reject",  accounts_deposit("alice", 0, &bal) != 0);
    CHECK("deposit neg reject", accounts_deposit("alice", -1, &bal) != 0);
    CHECK("deposit unknown",   accounts_deposit("nobody", 100, &bal) != 0);

    CHECK("withdraw 3000",     accounts_withdraw("alice", 3000, &bal) == 0);
    CHECK("balance after wdw", bal == 7000);

    CHECK("withdraw 0 reject", accounts_withdraw("alice", 0, &bal) != 0);
    CHECK("withdraw neg rej",  accounts_withdraw("alice", -1, &bal) != 0);
    CHECK("withdraw exceed",   accounts_withdraw("alice", 99999, &bal) != 0);
    CHECK("balance unchanged after nsf", bal == 7000);

    shutdown_clean();
}

static void test_transfer(void) {
    printf("\n[transfer]\n");
    init_fresh();

    accounts_register("alice", "alicepass");
    accounts_register("bob",   "bobpass8");

    int64_t b = 0;
    accounts_deposit("alice", 50000, &b);
    accounts_deposit("bob",   10000, &b);

    int64_t src_bal = 0;
    CHECK("transfer ok",       accounts_transfer("alice", "bob", 20000, &src_bal) == 0);
    CHECK("src after xfer",    src_bal == 30000);

    int64_t dst_bal = 0;
    accounts_get_balance("bob", &dst_bal);
    CHECK("dst after xfer",    dst_bal == 30000);

    CHECK("xfer to self",      accounts_transfer("alice", "alice", 1000, &src_bal) != 0);
    CHECK("xfer insufficient", accounts_transfer("alice", "bob", 999999, &src_bal) != 0);
    CHECK("xfer 0 reject",     accounts_transfer("alice", "bob", 0, &src_bal) != 0);
    CHECK("xfer unknown dst",  accounts_transfer("alice", "carol", 100, &src_bal) != 0);

    /* Conservation: total must equal initial total */
    int64_t a2 = 0, b2 = 0;
    accounts_get_balance("alice", &a2);
    accounts_get_balance("bob",   &b2);
    CHECK("conservation",      a2 + b2 == 60000);

    shutdown_clean();
}

static void test_balance_read(void) {
    printf("\n[balance read]\n");
    init_fresh();

    accounts_register("alice", "alicepass");
    int64_t bal = -1;
    CHECK("balance zero",    accounts_get_balance("alice", &bal) == 0 && bal == 0);
    accounts_deposit("alice", 42, &bal);
    bal = -1;
    CHECK("balance 42",      accounts_get_balance("alice", &bal) == 0 && bal == 42);
    CHECK("balance unknown", accounts_get_balance("nobody", &bal) != 0);

    shutdown_clean();
}

static void test_wal_crash_recovery(void) {
    printf("\n[WAL crash recovery]\n");
    init_fresh();

    /* Register alice → triggers checkpoint (op 0), snapshot: alice bal=0, WAL truncated */
    accounts_register("alice", "alicepass");

    /* Subsequent ops go into WAL only (no checkpoint triggered for ops 1..N < 500) */
    int64_t b = 0;
    accounts_deposit("alice", 55000, &b);
    accounts_deposit("alice", 12000, &b);
    accounts_withdraw("alice", 3000, &b);   /* balance should be 64000 */

    /* Register bob (goes into WAL, not snapshot) */
    accounts_register("bob", "bobpass8");
    accounts_deposit("bob", 20000, &b);
    accounts_transfer("alice", "bob", 4000, &b);  /* alice=60000, bob=24000 */

    /* "Crash": flush WAL but do NOT write checkpoint */
    shutdown_crash();

    /* Recovery: load snapshot (alice bal=0) + replay WAL */
    reinit_after_crash();

    int64_t alice_bal = 0, bob_bal = 0;
    int rc_a = accounts_get_balance("alice", &alice_bal);
    int rc_b = accounts_get_balance("bob",   &bob_bal);

    CHECK("alice found after recovery",   rc_a == 0);
    CHECK("bob found after recovery",     rc_b == 0);
    CHECK("alice balance recovered",      alice_bal == 60000);
    CHECK("bob balance recovered",        bob_bal   == 24000);
    CHECK("alice auth after recovery",    accounts_authenticate("alice", "alicepass") == 1);
    CHECK("bob auth after recovery",      accounts_authenticate("bob",   "bobpass8")  == 1);

    /* Conservation preserved */
    CHECK("conservation after recovery",  alice_bal + bob_bal == 84000);

    shutdown_clean();
}

static void test_double_restart_recovery(void) {
    printf("\n[double restart (crash after recovery)]\n");
    init_fresh();

    accounts_register("alice", "alicepass");
    int64_t b = 0;
    accounts_deposit("alice", 100000, &b);  /* WAL record, no checkpoint yet */

    shutdown_crash();

    /* First recovery */
    reinit_after_crash();
    accounts_deposit("alice", 50000, &b);   /* WAL record on top of recovered state */
    shutdown_crash();

    /* Second recovery */
    reinit_after_crash();
    int64_t bal = 0;
    accounts_get_balance("alice", &bal);
    CHECK("balance after double recovery", bal == 150000);

    shutdown_clean();
}

static void test_wal_torn_and_corrupt(void) {
    printf("\n[WAL torn tail and corrupt CRC]\n");
    init_fresh();
    accounts_register("alice", "alicepass");
    int64_t bal = 0;
    accounts_deposit("alice", 10000, &bal);
    shutdown_crash();

    struct stat st;
    if (stat(WAL, &st) != 0) { perror("stat WAL"); exit(2); }
    off_t valid_bytes = st.st_size;
    int fd = open(WAL, O_WRONLY | O_APPEND);
    WALRecord torn = {0};
    if (fd < 0 || write(fd, &torn, sizeof(torn) / 2) !=
                      (ssize_t)(sizeof(torn) / 2)) { perror("write torn WAL"); exit(2); }
    close(fd);

    reinit_after_crash();
    CHECK("torn tail keeps valid balance",
          accounts_get_balance("alice", &bal) == 0 && bal == 10000);
    CHECK("torn tail truncated", stat(WAL, &st) == 0 && st.st_size == valid_bytes);
    shutdown_clean();

    init_fresh();
    accounts_register("alice", "alicepass");
    accounts_deposit("alice", 10000, &bal);
    accounts_deposit("alice", 20000, &bal);
    shutdown_crash();
    fd = open(WAL, O_RDWR);
    unsigned char byte;
    off_t offset = (off_t)sizeof(WALRecord) + offsetof(WALRecord, amount_paise);
    if (fd < 0 || pread(fd, &byte, 1, offset) != 1) { perror("read WAL"); exit(2); }
    byte ^= 0x01;
    if (pwrite(fd, &byte, 1, offset) != 1 || fsync(fd) != 0) {
        perror("corrupt WAL"); exit(2);
    }
    close(fd);

    reinit_after_crash();
    CHECK("CRC error stops before corrupt record",
          accounts_get_balance("alice", &bal) == 0 && bal == 0);
    CHECK("CRC error truncates subsequent records",
          stat(WAL, &st) == 0 && st.st_size == (off_t)sizeof(WALRecord));
    shutdown_clean();
}

static void test_reject_invalid_persistence(void) {
    printf("\n[invalid persistence]\n");
    unlink(SNAP);
    unlink(WAL);
    FILE *fp = fopen(SNAP, "w");
    if (!fp) { perror("snapshot test"); exit(2); }
    fputs("# LedgerCore snapshot lsn=1 count=1\n"
          "1:alice:bad_hash:00112233445566778899aabbccddeeff:100\n", fp);
    fclose(fp);
    CHECK("malformed snapshot rejected", accounts_init(SNAP) != 0);
    fp = fopen(SNAP, "w");
    if (!fp) { perror("snapshot test"); exit(2); }
    fputs("# LedgerCore snapshot lsn=1 count=1\n"
          "1:alice:203e5dceb257e1fb116834ff95166d913e163153e8d75824238029c020bdfcbf:00112233445566778899aabbccddeeff:-1\n", fp);
    fclose(fp);
    CHECK("negative snapshot balance rejected", accounts_init(SNAP) != 0);
    fp = fopen(SNAP, "w");
    if (!fp) { perror("snapshot test"); exit(2); }
    fputs("# LedgerCore snapshot lsn=1 count=2\n"
          "1:alice:203e5dceb257e1fb116834ff95166d913e163153e8d75824238029c020bdfcbf:00112233445566778899aabbccddeeff:1\n"
          "2:alice:203e5dceb257e1fb116834ff95166d913e163153e8d75824238029c020bdfcbf:00112233445566778899aabbccddeeff:1\n", fp);
    fclose(fp);
    CHECK("duplicate snapshot username rejected", accounts_init(SNAP) != 0);
    unlink(SNAP);

    init_fresh();
    CHECK("unknown account WAL record appended",
          wal_append(WAL_DEPOSIT, 999, "missing", NULL, NULL, NULL,
                     100, 100, 0) == 0);
    shutdown_crash();
    CHECK("unknown account WAL replay rejected",
          accounts_init(SNAP) == 0 && wal_replay(WAL, accounts_snap_lsn()) != 0);
    unlink(WAL);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("LedgerCore unit tests\n");
    printf("===================\n");

    test_register_and_auth();
    test_deposit_withdraw();
    test_transfer();
    test_balance_read();
    test_wal_crash_recovery();
    test_double_restart_recovery();
    test_wal_torn_and_corrupt();
    test_reject_invalid_persistence();

    printf("\n===================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);

    /* Cleanup temp files */
    unlink(SNAP);
    unlink(WAL);
    char snap_tmp[256];
    snprintf(snap_tmp, sizeof(snap_tmp), "%s.tmp", SNAP);
    unlink(snap_tmp);

    return g_fail ? 1 : 0;
}
