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
  exit 0
fi
MODDIR=$(cd "$(dirname "$MODULE")" && pwd)
export JANET_PATH="$MODDIR"
export LD_LIBRARY_PATH="$MODDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

janet "$ROOT/test/rpc_level3.janet"

PORTFILE=$(mktemp)
janet "$ROOT/test/rpc_host.janet" >"$PORTFILE" &
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

PORT=$((RANDOM % 20000 + 30000))
"$SERVER" "$PORT" &
SPID=$!
trap 'kill $SPID $HPID 2>/dev/null || true; rm -f "$PORTFILE"' EXIT
sleep 0.2
janet "$ROOT/test/rpc_stream.janet" "$PORT"
kill $SPID 2>/dev/null || true
echo "ok janet host suite"
