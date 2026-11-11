#!/usr/bin/env bash
set -euo pipefail
make -s stress
SERVER_BIN="${SERVER_BIN:-./bin/ledgercore_server}"
SERVER_BIN="$(cd "$(dirname "$SERVER_BIN")" && pwd)/$(basename "$SERVER_BIN")"
PORT="${STRESS_PORT:-18082}"
WORK_DIR=$(mktemp -d)
(cd "$WORK_DIR" && exec "$SERVER_BIN" "$PORT") >"$WORK_DIR/server.log" 2>&1 &
SERVER_PID=$!
cleanup() {
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT
sleep 0.8
status=0
./bin/ledgercore_stress -p "$PORT" -c "${STRESS_CONNS:-32}" \
    -t "${STRESS_SECS:-3}" || status=$?
kill -TERM "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
if [[ "$status" -ne 0 ]] || grep -Eq 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:' "$WORK_DIR/server.log"; then
    cat "$WORK_DIR/server.log"
    exit 1
fi
