#!/usr/bin/env bash
# test_concurrency.sh – LedgerCore concurrent-client stress test
#
# What it does:
#   1. Starts the server on a test port.
#   2. Creates two shared accounts (alice, bob) each seeded with $SEED_AMOUNT.
#   3. Spawns $NUM_CLIENTS background clients split:
#        Group A (depositors): deposit $OP_AMOUNT into alice.
#        Group B (transferrers): transfer $OP_AMOUNT from alice to bob.
#   4. Waits for all background clients to finish.
#   5. Checks alice_final + bob_final == initial_total + acknowledged deposits.
#      (proves no money created or lost under heavy concurrency)

set -uo pipefail

BINDIR="./bin"
SERVER_BIN="$BINDIR/ledgercore_server"
SERVER_BIN="$(cd "$(dirname "$SERVER_BIN")" && pwd)/$(basename "$SERVER_BIN")"
WORK_DIR=$(mktemp -d)
RESULT_DIR="$WORK_DIR/results"
mkdir -p "$RESULT_DIR"
PORT=18080
HOST="127.0.0.1"
SEED_AMOUNT=100000    # each account starts here
NUM_CLIENTS=60        # total background clients
OP_AMOUNT=10

# ── locate netcat ─────────────────────────────────────────────────────────
NC_CMD=""
for c in nc ncat netcat; do
    if command -v "$c" &>/dev/null; then NC_CMD="$c"; break; fi
done
[[ -z "$NC_CMD" ]] && { echo "FATAL: netcat not found"; exit 1; }

# ── cleanup on exit ───────────────────────────────────────────────────────
cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

# ── send a multi-line script to the server via nc ─────────────────────────
# Works with macOS nc (uses -w timeout; server closes socket on QUIT).
send_cmds() {
    local script="$1"
    printf '%s\n' "$script" | "$NC_CMD" -w 15 "$HOST" "$PORT" 2>/dev/null || true
}

# ── extract balance value from a server response string ───────────────────
extract_balance() {
    # Matches "balance 12345.67" or "new_balance 12345.67"
    echo "$1" | grep -oE 'balance [0-9]+(\.[0-9]+)?' | awk '{print $2}' | tail -1
}

# ── 1. Start server ────────────────────────────────────────────────────────
(cd "$WORK_DIR" && exec "$SERVER_BIN" "$PORT") &
SERVER_PID=$!
sleep 0.6

echo "Server started (PID $SERVER_PID, port $PORT)"

# ── 2. Register accounts and seed initial balances ─────────────────────────
echo "Registering accounts and seeding $SEED_AMOUNT each..."

send_cmds "$(printf 'REGISTER alice alicepass\nQUIT')"      > /dev/null
send_cmds "$(printf 'REGISTER bob bobpass8\nQUIT')"          > /dev/null
SEED_A=$(send_cmds "$(printf 'LOGIN alice alicepass\nDEPOSIT %d\nLOGOUT\nQUIT' "$SEED_AMOUNT")")
SEED_B=$(send_cmds "$(printf 'LOGIN bob bobpass8\nDEPOSIT %d\nLOGOUT\nQUIT' "$SEED_AMOUNT")")
if ! grep -q 'OK deposited' <<< "$SEED_A" || ! grep -q 'OK deposited' <<< "$SEED_B"; then
    echo "FATAL: failed to seed both accounts"
    exit 1
fi

sleep 0.3

# ── 3. Spawn concurrent background clients ────────────────────────────────
HALF=$((NUM_CLIENTS / 2))
echo "Spawning $NUM_CLIENTS background clients ($HALF depositors + $HALF transferrers)..."

PIDS=()

for ((i=0; i<HALF; i++)); do
    (resp=$(send_cmds "$(printf 'LOGIN alice alicepass\nDEPOSIT %d\nLOGOUT\nQUIT' "$OP_AMOUNT")");
     if grep -q 'OK deposited' <<< "$resp"; then touch "$RESULT_DIR/deposit.$i"; fi) &
    PIDS+=($!)
done

for ((i=0; i<HALF; i++)); do
    send_cmds "$(printf 'LOGIN alice alicepass\nTRANSFER bob %d\nLOGOUT\nQUIT' "$OP_AMOUNT")" > /dev/null &
    PIDS+=($!)
done

echo "Waiting for ${#PIDS[@]} background processes to finish..."
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done
sleep 0.5   # let any in-flight server writes complete

# ── 4. Query final balances ───────────────────────────────────────────────
ALICE_RESP=$(send_cmds "$(printf 'LOGIN alice alicepass\nBALANCE\nLOGOUT\nQUIT')")
BOB_RESP=$(  send_cmds "$(printf 'LOGIN bob bobpass8\nBALANCE\nLOGOUT\nQUIT')")

echo ""
echo "Raw alice response: $ALICE_RESP"
echo "Raw bob   response: $BOB_RESP"
echo ""

ALICE_BAL=$(extract_balance "$ALICE_RESP")
BOB_BAL=$(  extract_balance "$BOB_RESP")

if [[ -z "$ALICE_BAL" || -z "$BOB_BAL" ]]; then
    echo "FATAL: could not read balances (alice='$ALICE_BAL' bob='$BOB_BAL')"
    echo "Server log:"
    tail -30 "$WORK_DIR/ledgercore_audit.log" 2>/dev/null || true
    exit 1
fi

# ── 5. Verify money conservation ──────────────────────────────────────────
# Group A adds HALF*OP_AMOUNT to alice; Group B moves HALF*OP_AMOUNT alice→bob.
# Net: alice += HALF*OP - HALF*OP (or less if some transfers failed due to
# race on insufficient funds). Bob gets whatever was transferred.
# Conservation law: alice + bob = 2*SEED + HALF*OP (all deposits land somewhere)

# Total that was deposited into alice in the test:
DEPOSITED=$(( HALF * OP_AMOUNT ))

# The absolute lower bound: 2*SEED (no deposits counted if all failed)
# The absolute upper bound: 2*SEED + DEPOSITED (all deposits counted, no transfers)
# Conservation: alice + bob must always == 2*SEED + (net_deposits_that_landed)
# Since both deposits (to alice) and transfers (alice→bob) are atomic,
# the TOTAL across both accounts = 2*SEED + (count of successful deposits)*OP.
# We can verify this without knowing how many succeeded: alice+bob >= 2*SEED.

to_paise() {
    local whole="${1%%.*}" frac="${1#*.}"
    printf '%d' "$((10#$whole * 100 + 10#$frac))"
}
ACTUAL_PAISE=$(( $(to_paise "$ALICE_BAL") + $(to_paise "$BOB_BAL") ))
SUCCESSFUL_DEPOSITS=$(find "$RESULT_DIR" -type f -name 'deposit.*' | wc -l | tr -d ' ')
EXPECTED_PAISE=$(( (SEED_AMOUNT * 2 + SUCCESSFUL_DEPOSITS * OP_AMOUNT) * 100 ))

echo "──────────────────────────────────────────────────────"
printf "  alice balance   : %s\n"  "$ALICE_BAL"
printf "  bob   balance   : %s\n"  "$BOB_BAL"
printf "  alice + bob     : %d paise\n" "$ACTUAL_PAISE"
printf "  expected        : %d paise\n" "$EXPECTED_PAISE"
printf "  deposits acked  : %d of %d\n" "$SUCCESSFUL_DEPOSITS" "$HALF"
echo "──────────────────────────────────────────────────────"

if [[ "$ACTUAL_PAISE" -eq "$EXPECTED_PAISE" ]]; then
    echo "  RESULT: PASS – money exactly conserved under $NUM_CLIENTS concurrent clients"
else
    echo "  RESULT: FAIL – total $ACTUAL_PAISE does not match acknowledged deposits ($EXPECTED_PAISE)!"
    exit 1
fi

echo ""
echo "Last 20 audit log entries:"
tail -20 "$WORK_DIR/ledgercore_audit.log" 2>/dev/null || echo "(no log)"

echo ""
echo "Test complete."
