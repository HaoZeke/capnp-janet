#!/usr/bin/env bash
# Janet host, stream helper, and level-3 handoff.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
SERVER="$BUILD/rpc_peer_server"
CLIENT="$BUILD/rpc_peer_client"
MODULE="${CAPNP_JANET_MODULE:-$BUILD/capnp.so}"
if ! command -v janet >/dev/null 2>&1 || [[ ! -f "$MODULE" || ! -x "$CLIENT" || ! -x "$SERVER" ]]; then
  echo "SKIP: janet host tools not built" >&2
  exit 77
fi
MODDIR=$(cd "$(dirname "$MODULE")" && pwd)
if command -v ldd >/dev/null 2>&1; then
  ASAN_RUNTIME=$(ldd "$MODULE" 2>/dev/null | awk '$1 ~ /^libasan/ {print $3; exit}') || true
  if [[ -n "$ASAN_RUNTIME" ]]; then
    export LD_PRELOAD="$ASAN_RUNTIME${LD_PRELOAD:+:$LD_PRELOAD}"
  fi
fi
export JANET_PATH="$MODDIR"
export LD_LIBRARY_PATH="$MODDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

janet "$ROOT/test/rpc_fill.janet"
janet "$ROOT/test/rpc_share_vat.janet"
janet "$ROOT/test/rpc_borrow.janet"
janet "$ROOT/test/rpc_level3.janet"
janet "$ROOT/test/builder_scope.janet"
# These scripts block or error when jpm runs them with no arguments.
janet "$ROOT/test/rpc_host.janet"
janet "$ROOT/test/rpc_client.janet"
janet "$ROOT/test/rpc_stream.janet"
janet "$ROOT/test/generated_builder_smoke.janet"

PORTFILE=$(mktemp)
janet "$ROOT/test/rpc_host.janet" serve >"$PORTFILE" &
HPID=$!
trap 'kill $HPID 2>/dev/null || true; rm -f "$PORTFILE"' EXIT
PORT=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if [[ -s "$PORTFILE" ]]; then
    PORT=$(head -1 "$PORTFILE")
    break
  fi
  sleep 0.05
done
[[ -n "$PORT" ]]
"$CLIENT" "$PORT"
wait $HPID

SPORTFILE=$(mktemp)
"$SERVER" 0 >"$SPORTFILE" &
SPID=$!
trap 'kill $SPID $HPID 2>/dev/null || true; rm -f "$PORTFILE" "$SPORTFILE"' EXIT
SPORT=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if [[ -s "$SPORTFILE" ]]; then
    SPORT=$(head -1 "$SPORTFILE")
    break
  fi
  sleep 0.05
done
[[ -n "$SPORT" ]]
janet "$ROOT/test/rpc_stream.janet" "$SPORT"
kill $SPID 2>/dev/null || true
echo "ok janet host suite"
