#!/usr/bin/env bash
# Janet host and stream helper.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
SERVER="$BUILD/rpc_peer_server"
CLIENT="$BUILD/rpc_peer_client"
MODULE="${CAPNP_JANET_MODULE:-$BUILD/capnp.so}"
# Meson sets CAPNP_JANET_NEED_MODULE when it builds capnp.so, and then
# a missing tool is a failure.
skip=77
[[ "${CAPNP_JANET_NEED_MODULE:-}" == 1 ]] && skip=1
if ! command -v janet >/dev/null 2>&1 || [[ ! -f "$MODULE" || ! -x "$CLIENT" || ! -x "$SERVER" ]]; then
  echo "SKIP: janet host tools not built" >&2
  exit $skip
fi
MODDIR=$(cd "$(dirname "$MODULE")" && pwd)
# Janet loads an instrumented capnp.so, so that process needs the
# sanitizer runtime first. Host tools and the C++ peer keep their own
# linkage.
ASAN_RUNTIME=""
if command -v ldd >/dev/null 2>&1; then
  ASAN_RUNTIME=$(ldd "$MODULE" 2>/dev/null | awk '$1 ~ /^libasan/ {print $3; exit}') || true
fi
run_janet() {
  if [[ -n "$ASAN_RUNTIME" ]]; then
    LD_PRELOAD="$ASAN_RUNTIME${LD_PRELOAD:+:$LD_PRELOAD}" janet "$@"
  else
    janet "$@"
  fi
}
export JANET_PATH="$MODDIR"
export LD_LIBRARY_PATH="$MODDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

run_janet "$ROOT/test/rpc_fill.janet"
run_janet "$ROOT/test/rpc_borrow.janet"
run_janet "$ROOT/test/rpc_fail.janet"
run_janet "$ROOT/test/builder_scope.janet"
# These scripts block or error when jpm runs them with no arguments.
run_janet "$ROOT/test/rpc_host.janet"
run_janet "$ROOT/test/rpc_client.janet"
run_janet "$ROOT/test/rpc_stream.janet"
run_janet "$ROOT/test/generated_builder_smoke.janet"

PORTFILE=$(mktemp)
run_janet "$ROOT/test/rpc_host.janet" serve >"$PORTFILE" &
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
run_janet "$ROOT/test/rpc_stream.janet" "$SPORT"
kill $SPID 2>/dev/null || true
echo "ok janet host suite"
