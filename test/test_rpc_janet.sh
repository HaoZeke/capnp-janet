#!/usr/bin/env bash
# Janet client vat against the capnp-C++ Adder peer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
SERVER="${CAPNP_JANET_RPC_SERVER:-$BUILD/rpc_peer_server}"
MODULE="${CAPNP_JANET_MODULE:-$BUILD/capnp.so}"
if ! command -v janet >/dev/null 2>&1; then
  echo "SKIP: no janet" >&2
  exit 77
fi
if [[ ! -x "$SERVER" || ! -f "$MODULE" ]]; then
  echo "SKIP: need rpc_peer_server and capnp.so" >&2
  exit 77
fi
if command -v ldd >/dev/null 2>&1; then
  ASAN_RUNTIME=$(ldd "$MODULE" 2>/dev/null | awk '$1 ~ /^libasan/ {print $3; exit}') || true
  if [[ -n "$ASAN_RUNTIME" ]]; then
    export LD_PRELOAD="$ASAN_RUNTIME${LD_PRELOAD:+:$LD_PRELOAD}"
  fi
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
MODDIR=$(cd "$(dirname "$MODULE")" && pwd)
export JANET_PATH="$MODDIR"
export LD_LIBRARY_PATH="$MODDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
janet "$ROOT/test/rpc_client.janet" "$PORT"
