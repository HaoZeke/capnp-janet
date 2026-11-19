#!/usr/bin/env bash
# TCP vat against capnp-C++ in both directions.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
DIAL="$BUILD/test_transport"
SERVER="$BUILD/rpc_peer_server"
CLIENT="$BUILD/rpc_peer_client"
if [[ ! -x "$DIAL" || ! -x "$SERVER" || ! -x "$CLIENT" ]]; then
  echo "SKIP: transport peer tools not built" >&2
  exit 0
fi

PORT=$((RANDOM % 20000 + 30000))
"$SERVER" "$PORT" &
SPID=$!
trap 'kill $SPID 2>/dev/null || true' EXIT
"$DIAL" dial "$PORT"
kill $SPID 2>/dev/null || true
wait $SPID 2>/dev/null || true
trap - EXIT

PORT=$((RANDOM % 20000 + 30000))
"$DIAL" serve "$PORT" &
SPID=$!
trap 'kill $SPID 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if "$CLIENT" "$PORT"; then
    kill $SPID 2>/dev/null || true
    wait $SPID 2>/dev/null || true
    echo "ok transport peer"
    exit 0
  fi
  sleep 0.05
done
echo "cpp client did not reach the vat" >&2
exit 1
