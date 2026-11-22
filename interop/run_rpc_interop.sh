#!/usr/bin/env bash
# Orchestrates the RPC interop test: start the C++ peer, run the C
# client against it, tear down. Usage: run_rpc_interop.sh <server> <client>
set -u
SERVER=$1
CLIENT=$2
run() {
  local mode="$1" late="${2:-0}" portfile spid port rc
  portfile=$(mktemp)
  "$SERVER" 0 $mode >"$portfile" &
  spid=$!
  port=""
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if [[ -s "$portfile" ]]; then
      port=$(head -1 "$portfile")
      break
    fi
    sleep 0.05
  done
  if [[ -z "$port" ]]; then
    echo "server did not report a port" >&2
    kill $spid 2>/dev/null || true
    rm -f "$portfile"
    return 1
  fi
  sleep "$late"
  "$CLIENT" "$port" $mode
  rc=$?
  kill $spid 2>/dev/null || true
  rm -f "$portfile"
  return $rc
}

# The second run serves a bootstrap that capnp-C++ sends as senderPromise;
# the third connects late, which must not change that descriptor.
run "" && run promise && run promise 0.5
