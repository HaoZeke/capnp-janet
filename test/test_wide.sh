#!/usr/bin/env bash
# A result with three data words and a text pointer, against capnp-C++.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
SERVER="$BUILD/wide_server"
CLIENT="$BUILD/wide_client"
if [[ ! -x "$SERVER" || ! -x "$CLIENT" ]]; then
  echo "SKIP: wide peer not built" >&2
  exit 77
fi
PORT=$((RANDOM % 20000 + 30000))
"$SERVER" "$PORT" &
SPID=$!
trap 'kill $SPID 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if "$CLIENT" "$PORT"; then
    exit 0
  fi
  sleep 0.05
done
echo "wide client did not reach the vat" >&2
exit 1
