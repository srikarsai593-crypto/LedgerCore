#!/usr/bin/env bash
# tests/tls_smoke.sh – TLS smoke test for LedgerCore
#
# Usage: bash tests/tls_smoke.sh [port]
#   Assumes the server is already running with TLS on the given port.
set -uo pipefail

PORT="${1:-18080}"
CAFILE="${2:-ledgercore.crt}"
HOST="127.0.0.1"
CLIENT="./bin/ledgercore_client"

tls_cmd() {
    # Pipe a newline-separated command script through the TLS client.
    # The client prints "> " prompts inline with "< RESPONSE" on the same stdout line
    # ("> < OK ..."), so strip the "> " prefix before grepping for "< ".
    printf '%s\nQUIT\n' "$1" | "$CLIENT" "$HOST" "$PORT" --tls --cafile "$CAFILE" 2>/dev/null \
        | sed 's/^> //' | grep '^< ' | sed 's/^< //'
}

echo "TLS smoke test (port=$PORT)"
echo "────────────────────────────────────────────"

# 1. Register
OUT=$(tls_cmd "REGISTER alice_tls alicetls8")
echo "REGISTER → $OUT"
if ! echo "$OUT" | grep -q "^OK account created"; then
    echo "  RESULT: FAIL – REGISTER did not return 'OK account created'"
    exit 1
fi

# 2. Deposit + balance check
OUT=$(tls_cmd "$(printf 'LOGIN alice_tls alicetls8\nDEPOSIT 100\nBALANCE\nHISTORY\nLOGOUT')")
echo "LOGIN+DEPOSIT+BALANCE → $OUT"
if ! echo "$OUT" | grep -q "balance 100.00" ||
   ! echo "$OUT" | grep -q '^END [0-9][0-9]* records'; then
    echo "  RESULT: FAIL – balance or TLS history response was incorrect"
    exit 1
fi

# 3. Verify bad-password is rejected
OUT=$(tls_cmd "LOGIN alice_tls wrongpassword")
echo "BAD LOGIN → $OUT"
if ! echo "$OUT" | grep -q "^ERR"; then
    echo "  RESULT: FAIL – bad password was not rejected"
    exit 1
fi

# A different trust anchor must not authenticate the server.
WRONG_CA_DIR=$(mktemp -d)
trap 'rm -rf "$WRONG_CA_DIR"' EXIT
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$WRONG_CA_DIR/wrong.key" -out "$WRONG_CA_DIR/wrong.crt" \
    -subj '/CN=wrong-ledgercore-ca' >/dev/null 2>&1
if printf 'QUIT\n' | "$CLIENT" "$HOST" "$PORT" --tls \
        --cafile "$WRONG_CA_DIR/wrong.crt" >"$WRONG_CA_DIR/client.log" 2>&1; then
    echo "  RESULT: FAIL – client accepted an unrelated CA"
    exit 1
fi

echo "────────────────────────────────────────────"
echo "  RESULT: PASS – verified TLS, banking commands, bad-auth, and wrong-CA rejection"
