#!/usr/bin/env bash
# Orchestrates the RPC interop test: start the C++ peer, run the C
# client against it, tear down. Usage: run_rpc_interop.sh <server> <client>
set -u
SERVER=$1
CLIENT=$2
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
if [[ -z "$PORT" ]]; then
  echo "server did not report a port" >&2
  exit 1
fi

"$CLIENT" "$PORT"
RC=$?
exit $RC
