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
  exit 77
fi

PORTFILE=$(mktemp)
"$SERVER" 0 >"$PORTFILE" &
SPID=$!
trap 'kill $SPID 2>/dev/null || true; rm -f "$PORTFILE"' EXIT
PORT=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if [[ -s "$PORTFILE" ]]; then
    PORT=$(head -1 "$PORTFILE")
    break
  fi
  sleep 0.05
done
[[ -n "$PORT" ]]
"$DIAL" dial "$PORT"
# A second capnp-C++ vat with no bootstrap capability.
NBFILE=$(mktemp)
"$SERVER" 0 none >"$NBFILE" &
NBPID=$!
trap 'kill $SPID $NBPID 2>/dev/null || true; rm -f "$PORTFILE" "$NBFILE"' EXIT
NBPORT=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if [[ -s "$NBFILE" ]]; then
    NBPORT=$(head -1 "$NBFILE")
    break
  fi
  sleep 0.05
done
[[ -n "$NBPORT" ]]
# The same frames to capnp-C++ and to the vat: replies must agree.
"$DIAL" vat-diff "$PORT" "$NBPORT"
kill $SPID $NBPID 2>/dev/null || true
wait $SPID $NBPID 2>/dev/null || true
trap - EXIT
rm -f "$PORTFILE" "$NBFILE"

# Returns capnp-C++ refuses: its Abort reason must be the vat's.
PORTFILE=$(mktemp)
"$DIAL" bad-return 0 >"$PORTFILE" &
SPID=$!
trap 'kill $SPID 2>/dev/null || true; rm -f "$PORTFILE"' EXIT
PORT=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if [[ -s "$PORTFILE" ]]; then
    PORT=$(head -1 "$PORTFILE")
    break
  fi
  sleep 0.05
done
[[ -n "$PORT" ]]
for _ in 1 2 3 4 5 6; do
  "$CLIENT" "$PORT" abort
done
"$CLIENT" "$PORT" hold
wait $SPID
trap - EXIT
tail -n +2 "$PORTFILE"
rm -f "$PORTFILE"

PORTFILE=$(mktemp)
"$DIAL" serve 0 >"$PORTFILE" &
SPID=$!
trap 'kill $SPID 2>/dev/null || true; rm -f "$PORTFILE"' EXIT
PORT=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if [[ -s "$PORTFILE" ]]; then
    PORT=$(head -1 "$PORTFILE")
    break
  fi
  sleep 0.05
done
[[ -n "$PORT" ]]
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
