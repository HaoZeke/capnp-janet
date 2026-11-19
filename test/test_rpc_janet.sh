#!/usr/bin/env bash
# Janet client vat against the capnp-C++ Adder peer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
SERVER="${CAPNP_JANET_RPC_SERVER:-$BUILD/rpc_peer_server}"
MODULE="${CAPNP_JANET_MODULE:-$BUILD/capnp.so}"
if ! command -v janet >/dev/null 2>&1; then
  echo "SKIP: no janet" >&2
  exit 0
fi
if [[ ! -x "$SERVER" || ! -f "$MODULE" ]]; then
  echo "SKIP: need rpc_peer_server and capnp.so" >&2
  exit 0
fi
PORT=$((RANDOM % 20000 + 30000))
"$SERVER" "$PORT" &
SPID=$!
trap 'kill $SPID 2>/dev/null || true' EXIT
MODDIR=$(cd "$(dirname "$MODULE")" && pwd)
export JANET_PATH="$MODDIR"
export LD_LIBRARY_PATH="$MODDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
janet "$ROOT/test/rpc_client.janet" "$PORT"
