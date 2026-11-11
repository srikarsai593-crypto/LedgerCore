# LedgerCore

## About

LedgerCore is a multi-threaded TCP banking server written in C11. It provides account registration,
authentication, balance management, and transfers through a lightweight line-based protocol. The
project focuses on systems-level concerns—including concurrent access, durable transaction recovery,
and secure transport—while keeping its implementation and testing approachable.

The server uses POSIX sockets and pthreads, a write-ahead log for crash recovery, PBKDF2-HMAC-SHA256
for password hashing, and optional TLS 1.2+ via OpenSSL. A companion client, load generator, and
automated test suite are included.

---

## Features

| Area | Detail |
|---|---|
| **Concurrency** | 64-worker bounded thread pool; `pthread_rwlock_t` + per-account `pthread_mutex_t`; strict lock-ordering to prevent deadlocks |
| **Durability** | Binary WAL (write-ahead log) with CRC-32, group commit, and crash recovery; atomic `rename()` snapshots |
| **Auth** | PBKDF2-HMAC-SHA256 (600 000 iterations for new hashes, 32-byte key, `RAND_bytes` salt, `CRYPTO_memcmp` constant-time verify) |
| **Transport** | Optional TLS 1.2+ via OpenSSL `LCConn` abstraction; plaintext mode; `--tls` client flag |
| **Precision** | Integer money in `int64_t` paise (1/100 of a unit) — no floating-point arithmetic |
| **Networking** | epoll-based accept loop on Linux with self-pipe shutdown; `accept()`-loop fallback on macOS |
| **Testing** | 48 unit assertions, concurrency conservation proof, crash-recovery test, TLS smoke test, TSan + ASan builds |
| **Load testing** | C load generator with p50/p95/p99/max latency reporting |

---

## Requirements

| Tool | Version |
|---|---|
| GCC | 11+ (or Clang 12+) |
| OpenSSL | 3.x (via `pkg-config`; falls back to `brew --prefix openssl@3` on macOS) |
| GNU Make | any modern version |
| POSIX OS | Linux (primary) or macOS (development) |

On Ubuntu/Debian:

```bash
sudo apt-get install gcc make libssl-dev
```

On macOS:

```bash
brew install openssl@3
```

---

## Building

```bash
make              # server + client  →  bin/ledgercore_server, bin/ledgercore_client
make unit         # unit test runner →  bin/ledgercore_unit
make loadgen      # load generator  →  bin/ledgercore_loadgen
make tsan         # ThreadSanitizer builds
make asan         # AddressSanitizer + UBSan builds
make clean        # remove obj/, bin/, and generated repository data/certs
```

Normal targets build with `-Wall -Wextra -Werror -pthread -O2 -std=c11 -D_GNU_SOURCE`.
Sanitizer targets use `-O1 -g`.

---

## Running

### Plain-text mode

```bash
# Server
./bin/ledgercore_server <port>
# e.g.
./bin/ledgercore_server 18080

# Client (interactive)
./bin/ledgercore_client [host [port]]
./bin/ledgercore_client 127.0.0.1 18080
```

### TLS mode

Generate a self-signed dev certificate (valid 10 years, localhost + 127.0.0.1 SANs):

```bash
make gen-cert          # creates ledgercore.crt and ledgercore.key
# or directly:
bash gen_cert.sh
```

Start the server with cert and key:

```bash
./bin/ledgercore_server 18080 ledgercore.crt ledgercore.key
```

Connect with TLS:

```bash
./bin/ledgercore_client 127.0.0.1 18080 --tls --cafile ledgercore.crt
```

Run the TLS smoke test end-to-end:

```bash
make tls-smoke
```

### Graceful shutdown

Send `SIGINT` (Ctrl-C) or `SIGTERM`.  The server stops accepting new connections, drains
in-flight work from the thread pool, checkpoints the WAL, flushes the audit log, and exits cleanly.

### Persistent files

| File | Contents |
|---|---|
| `ledgercore.snap` | Atomic text snapshot of all accounts |
| `ledgercore.wal` | Write-ahead log (binary, CRC-32 records) |
| `ledgercore_audit.log` | Append-only human-readable audit trail |
| `ledgercore.lock` | Advisory lockfile preventing duplicate server instances |

---

## Protocol Reference

Line-based text protocol (newline-terminated).  The client sends one command per line;
the server replies with `OK …` or `ERR …`.  Multi-line responses end with `END n records`.

| Command | Auth required | Description |
|---|---|---|
| `REGISTER <user> <pass>` | No | Create a new account; password ≥ 8 chars |
| `LOGIN <user> <pass>` | No | Start an authenticated session |
| `LOGOUT` | Yes | End the session (remains connected) |
| `BALANCE` | Yes | Show current balance |
| `DEPOSIT <amount>` | Yes | Add funds (integer units; e.g. `DEPOSIT 100` → adds 100.00) |
| `WITHDRAW <amount>` | Yes | Remove funds (rejected if insufficient balance) |
| `TRANSFER <to_user> <amount>` | Yes | Atomic transfer to another account |
| `HISTORY` | Yes | Last 50 transactions from the audit log |
| `QUIT` | No | Close the connection |

Example session:

```
> REGISTER alice s3cret99
< OK account created
> REGISTER bob b0bpass8
< OK account created
> LOGIN alice s3cret99
< OK logged in as alice
> DEPOSIT 500
< OK deposited 500.00 new_balance 500.00
> TRANSFER bob 100
< OK transferred 100.00 to bob new_balance 400.00
> HISTORY
< OK history for alice:
< 1|2026-10-30T12:00:00|REGISTER|alice|new account|0.00|OK
< 2|2026-10-30T12:00:01|LOGIN|alice||0.00|OK
< 3|2026-10-30T12:00:02|DEPOSIT|alice||500.00|OK
< 4|2026-10-30T12:00:03|TRANSFER|alice|to bob|100.00|OK
< END 4 records
> QUIT
< OK goodbye
```

---

## Testing

### Unit tests (48 assertions)

```bash
make unit
./bin/ledgercore_unit
```

Covers: account register + auth, deposit/withdraw, transfer, balance reads,
WAL crash recovery, double-restart recovery, a torn final WAL record, and a
bad CRC in the middle of the WAL, malformed snapshots, and WAL references to
unknown accounts. Torn or bad-CRC replay stops and truncates at the last valid
record; a semantic WAL error fails startup.
Uses `/tmp/ledgercore_unit.{snap,wal}` — no side effects in the project directory.

### Concurrency conservation test

```bash
bash test_concurrency.sh
```

Spawns 60 concurrent clients (30 depositors + 30 transferrers) against two accounts
seeded with 100 000 each, then verifies `alice + bob` equals the seeded total
plus exactly the acknowledged deposits.

```
RESULT: PASS – money exactly conserved under 60 concurrent clients
```

### Multi-account transfer stress

```bash
bash tests/stress.sh
SERVER_BIN=./bin/ledgercore_server_tsan bash tests/stress.sh
```

Seeds 20 accounts, then uses 32 persistent connections to make random
transfers for 3 seconds. It checks every balance is nonnegative and the
final total equals the seeded total exactly. CI also runs this workload
against the ThreadSanitizer server.

### Crash recovery test

```bash
bash tests/crash_test.sh
```

The C harness seeds 10 accounts, starts 16 clients making random deposits,
withdrawals, and transfers, then sends `SIGKILL` at a random point during load.
After restart, it checks all 10 balances against every acknowledged operation.
Each client has at most one command in flight at the kill point; the verifier
checks every possible applied/not-applied combination of those ambiguous
commands. Transfers conserve the total in every combination. The local default
is 20 iterations; CI runs 5 with `CRASH_ITERS=5`.

### TLS smoke test

```bash
bash tests/tls_smoke.sh [port]
```

Requires the server to be running with TLS on the given port. Tests `REGISTER`,
`LOGIN`, `DEPOSIT`, `BALANCE`, `HISTORY`, bad-password rejection, and rejection
of a certificate signed by an unrelated CA.

```
RESULT: PASS – verified TLS, banking commands, bad-auth, and wrong-CA rejection
```

### All tests in one command

```bash
make check
```

Runs unit tests, the two concurrency tests, and crash recovery in sequence.

### Sanitizer builds

```bash
# ThreadSanitizer — detects data races
make tsan
TSAN_OPTIONS=halt_on_error=1 ./bin/ledgercore_server_tsan 18080 &
# ... drive it with clients ...

# AddressSanitizer + UBSan — detects memory errors and undefined behaviour
make asan
# On Linux, leak detection is available:
ASAN_OPTIONS="halt_on_error=1 detect_leaks=1" ./bin/ledgercore_server_asan 18080 &
```

---

## Load Testing

```bash
make bench
# or manually:
./bin/ledgercore_loadgen -h 127.0.0.1 -p 18080 -t 12 -c 16 -w 2
./bin/ledgercore_loadgen -h 127.0.0.1 -p 18080 -t 8 -c 32 -w 1 -m -L
```

Flags:

| Flag | Default | Description |
|---|---|---|
| `-h HOST` | `127.0.0.1` | Server hostname |
| `-p PORT` | `18080` | Server port |
| `-t SECS` | `10` | Test duration in seconds |
| `-c CONNS` | `8` | Number of concurrent connections |
| `-w WARM` | `2` | Warm-up period in seconds (excluded from stats) |
| `-m` | off | Mix deposit, withdraw, transfer, and balance requests |
| `-L` | off | Log in once per connection; time transactions only |

`make bench` uses `-m -L`. Each worker registers and seeds one account before
timing starts. The mixed workload selects among four commands; with one
connection, transfer choices become balance reads because there is no recipient.
`-L` keeps the session logged in, so PBKDF2 is outside the measured
transaction latency.
Latency uses `clock_gettime(CLOCK_MONOTONIC)`.

Measured locally after task 7 on 2 November 2026, with 8-second runs and a
1-second warm-up. These measurements precede the task 8 username index:

| Machine/CPU | Disk | Connections | Ops/sec | p50 (µs) | p99 (µs) | Avg WAL batch |
|---|---|---:|---:|---:|---:|---:|
| MacBook Pro, Apple M3 Pro | Internal Apple SSD AP0512Z | 1 | 27,685.6 | 46 | 67 | 1.00 |
| MacBook Pro, Apple M3 Pro | Internal Apple SSD AP0512Z | 8 | 64,264.8 | 119 | 347 | 1.76 |
| MacBook Pro, Apple M3 Pro | Internal Apple SSD AP0512Z | 32 | 41,044.6 | 669 | 2,500 | 3.41 |
| MacBook Pro, Apple M3 Pro | Internal Apple SSD AP0512Z | 64 | 34,906.7 | 1,579 | 6,258 | 10.25 |

The WAL batch figure includes registration, seeding, warm-up, and measurement.
Throughput rose from 1 to 8 connections and fell at 32 and 64 on this machine;
larger batches alone did not make the workload scale linearly. Every acknowledged
write still waits for a durable WAL `fsync()`.

Before/after task 1 comparison on the same machine and SSD, with the old
`LOGIN → DEPOSIT → LOGOUT` benchmark and 100,000 PBKDF2 iterations. Each run
used 8 seconds with a 1-second warm-up; that load generator version reported a
6-second measurement window due to a timing bug fixed in task 7. These rows
are comparable to each other, not to the mixed workload above.

| Lock design | Connections | Ops/sec | p50 (µs) | p99 (µs) | Avg WAL batch |
|---|---:|---:|---:|---:|---:|
| Before task 1 | 1 | 101.7 | 9,718 | 10,614 | 1.00 |
| After task 1 | 1 | 98.6 | 10,151 | 10,700 | 1.00 |
| Before task 1 | 8 | 571.6 | 13,225 | 28,661 | 1.00 |
| After task 1 | 8 | 571.0 | 13,377 | 25,777 | 1.01 |
| Before task 1 | 32 | 704.4 | 44,525 | 96,255 | 1.00 |
| After task 1 | 32 | 676.4 | 45,815 | 106,486 | 1.14 |
| Before task 1 | 64 | 721.0 | 85,744 | 198,242 | 1.00 |
| After task 1 | 64 | 675.3 | 95,274 | 213,792 | 1.25 |

Moving checkpointing off the request path in task 2 reduced the maximum latency
at 64 connections from 332,613 to 259,992 µs in one additional run. These
single-run measurements show the observed direction, not a guaranteed speedup.

---

## Architecture

### Thread and I/O model

```
main thread
  └── accept loop (epoll on Linux, blocking accept() on macOS)
        └── accept() → push ClientConn* to 256-slot job queue
              └── 64 worker threads → optional TLS handshake → command loop
```

On Linux, the accept loop uses `epoll_create1` with `EPOLLIN` on both the listen socket
and a self-pipe.  The signal handler writes 1 byte to the pipe, which wakes `epoll_wait`
without races. The accepted connection is then served by one worker thread,
including its command reads and optional TLS handshake.

### Lock ordering (deadlock-free by design)

Mutating operations acquire locks in this order:

1. **`g_ckpt_rwlock`** in read mode — prevents a checkpoint from observing a
   WAL record before its balance is applied.
2. **`g_reg_rwlock`** — write-locked for `REGISTER`, read-locked for other
   mutations.
3. **`account.lock`** — protects each balance.
   For `TRANSFER`, both account locks are acquired in ascending `account_id` order
   to prevent a cycle in the lock graph. A checkpoint takes `g_ckpt_rwlock` in
   write mode after in-flight mutations finish.

### WAL and crash recovery

Every successful balance mutation follows this sequence:

1. Build a 342-byte WAL record with a CRC-32 checksum.
2. The WAL writer writes and `fsync()`s the record. It can batch records already
   queued together.
3. Apply the new balance in memory and acknowledge the command.
4. A background worker wakes after 500 mutations or every 3 seconds, writes
   and syncs a text snapshot, atomically renames it, syncs its directory, then
   truncates the WAL. Requests only signal it. Shutdown stops the worker and
   takes one final checkpoint.

On startup, the server replays any WAL records whose LSN exceeds the snapshot's
checkpoint LSN, restoring all acknowledged operations after a crash.
LSN tracking uses `_Atomic uint64_t g_last_lsn` (safe under TSan).
Snapshot rows and headers are validated before use; invalid account references
in replay fail startup. A registry-protected username hash table keeps account
lookup bounded as the account count grows. Registration derives its password
hash before acquiring the registry write lock.

### Design trade-offs

- **WAL and snapshots:** the WAL makes an acknowledged write recoverable after
  a crash; snapshots bound replay time and WAL file growth. A checkpoint
  excludes in-flight mutations, syncs the snapshot and its directory, then
  truncates the WAL.
- **Group commit:** multiple independent mutations can queue while one WAL
  writer prepares an `fsync()` batch. Each caller still waits for its own
  record to become durable, so storage latency remains a throughput limit.
- **Ordered account locks:** transfers acquire both account locks in ascending
  `account_id` order, so opposite-direction transfers cannot deadlock. The
  checkpoint lock stays outermost so its snapshot captures applied writes.
- **Integer paise:** balances use `int64_t` smallest units, giving exact
  addition and subtraction within checked bounds.

### Password security

`src/auth.c` implements PBKDF2-HMAC-SHA256:

- 16-byte random salt via `RAND_bytes`
- 600 000 iterations for new hashes, 32-byte derived key
- Stored in separate salt and hash fields in the account snapshot and WAL
- New hashes carry their iteration count (`pbkdf2_sha256$600000$<hex>`);
  the old 100,000-iteration bare-hex format remains readable
- Verification uses `CRYPTO_memcmp` (constant-time) to prevent timing attacks
- Login copies the salt and hash under the account lock, then runs PBKDF2
  after releasing it, so balance operations are not blocked by hashing

### TLS abstraction (`LCConn`)

`src/tlsutil.h` defines a `LCConn` — a `{int fd; SSL *ssl}` pair.  All I/O goes through:

```c
ssize_t lc_readline (LCConn *c, char *buf, size_t maxlen);
int     lc_sendline (LCConn *c, const char *line);
int     lc_sendlinef(LCConn *c, const char *fmt, ...);
void    lc_close   (LCConn *c);
```

When `ssl == NULL`, these use plain `read()`/`write()`. Supplying certificate and
key paths starts the server in TLS mode; otherwise the port serves plaintext.
The TLS client verifies the server certificate and host name by default. Use
`--cafile ledgercore.crt` for the generated development certificate, or omit it
to use the system trust store. `--insecure` disables verification explicitly
and prints a warning; it is intended only for local testing.

---

## Source File Layout

```
src/
  server.c      Main loop, accept/epoll, client dispatch, signal handling
  client.c      Interactive CLI client, --tls flag, LCConn I/O
  accounts.c    In-memory account store, snapshot persistence
  accounts.h
  auth.c        PBKDF2-HMAC-SHA256 hash, salt generation, constant-time verify
  auth.h
  audit.c       Append-only audit log with atomic sequence counter
  audit.h
  wal.c         Binary WAL: append, replay, group commit, checkpoint, CRC-32
  wal.h
  tlsutil.c     LCConn TLS abstraction, OpenSSL context setup
  tlsutil.h
  netutil.c     readline/sendline helpers (plain fd)
  netutil.h
  protocol.h    Shared constants (max username/password length, amount limits)
  threadpool.c  Bounded circular-queue thread pool (64 workers, 256-slot queue)

tests/
  unit.c        48-assertion unit test suite (no server process required)
  loadgen.c     C load generator with latency percentiles
  crash_test.c  Concurrent crash recovery verifier
  crash_test.sh Builds and runs the C verifier
  stress.c      20-account random transfer stress client
  stress.sh     Runs stress client against isolated server data
  tls_smoke.sh  TLS end-to-end smoke test

gen_cert.sh     Self-signed dev certificate generator (10-year, localhost SANs)
test_concurrency.sh  Money-conservation concurrency stress test
Makefile
.github/workflows/ci.yml  Two-job CI: build+test+TLS+bench, TSan+ASan
```

---

## CI

Two GitHub Actions jobs on pushes and pull requests to `main`:

**`build-and-test`** (ubuntu-latest):
- Installs `libssl-dev`, builds server + client + unit + loadgen
- Runs unit tests, both concurrency tests, and crash recovery test
- Generates a cert and runs the TLS smoke test
- Runs a benchmark smoke (8 s, 8 connections)

**`sanitizers`** (ubuntu-latest):
- Builds with `-fsanitize=thread` and runs the 20-account transfer stress test
- Builds with `-fsanitize=address,undefined` and runs the ASan smoke test

---

## Limitations and Known Trade-offs

- **Single WAL file**: suitable for a single-node server; no replication.
- **fsync bottleneck**: each acknowledged mutation waits for WAL durability;
  batching reduces the number of syncs when independent operations overlap.
- **Max accounts**: compile-time constant `MAX_ACCOUNTS` (default 2048) in `protocol.h`.
- **HISTORY**: scans at most 100,000 audit lines per request and returns at most
  50 matching entries. Older entries can be omitted when the scan limit is reached.
