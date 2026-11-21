#!/usr/bin/env bash
# capnp-eval convert against `capnp convert` on hand-built wire messages.
# Exit status has to agree; when both succeed the output has to match.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
BIN="$BUILD/capnp-eval"
if [[ ! -x "$BIN" ]] || ! command -v capnp >/dev/null; then
  echo "SKIP: needs capnp-eval and capnp" >&2
  exit 77
fi
INC="$(dirname "$(dirname "$(command -v capnp)")")/include"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PARITY="$ROOT/schema/parity.capnp"
HOLDER="$ROOT/test/fixtures/holder.capnp"
fails=0

# words HEX... -> single-segment stream message on stdout
words() {
  python3 - "$@" <<'EOF'
import struct, sys
w = [int(x, 16) for x in sys.argv[1:]]
sys.stdout.buffer.write(struct.pack("<II", 0, len(w)))
sys.stdout.buffer.write(b"".join(struct.pack("<Q", x) for x in w))
EOF
}

# check NAME SCHEMA ROOT FORMATS -- HEX...
check() {
  local name="$1" schema="$2" root="$3" fmts="$4" fmt ref_rc our_rc
  shift 5
  words "$@" >"$TMP/in.bin"
  for fmt in $fmts; do
    ref_rc=0
    our_rc=0
    capnp convert -I"$INC" "binary:$fmt" "$schema" "$root" \
      <"$TMP/in.bin" >"$TMP/ref.out" 2>/dev/null || ref_rc=$?
    "$BIN" convert -I"$INC" "binary:$fmt" "$schema" "$root" \
      <"$TMP/in.bin" >"$TMP/our.out" 2>/dev/null || our_rc=$?
    if [[ $((ref_rc == 0)) -ne $((our_rc == 0)) ]]; then
      echo "FAIL $name $fmt: status capnp=$ref_rc capnp-eval=$our_rc" >&2
      fails=$((fails + 1))
    elif [[ $ref_rc -eq 0 ]] && ! cmp -s "$TMP/ref.out" "$TMP/our.out"; then
      echo "FAIL $name $fmt: output differs" >&2
      fails=$((fails + 1))
    fi
  done
}

# Corrupt pointers read as defaults in C++ and then fail the conversion.
check text-oob "$PARITY" Parity "text json" -- \
  0001000000000000 0000001a00000029
check struct-as-text "$PARITY" Parity "text json" -- \
  0001000000000000 0000000100000000 0
check text-no-nul "$PARITY" Parity "text json" -- \
  0001000000000000 0000001200000001 0000000000006261
check list-as-struct "$PARITY" Parity "text json" -- \
  0003000000000000 0 0 0000000a00000001 0000000000000007
check bytes-as-int32-list "$PARITY" Parity "text json" -- \
  0008000000000000 0 0 0 0 0 0 0 0000000a00000001 0000000000000007
check bits-as-uint8-list "$PARITY" Parity "text json" -- \
  000a000000000000 0 0 0 0 0 0 0 0 0 0000000900000001 0000000000000001
check ptr-structs-as-int16-list "$PARITY" Parity "text json" -- \
  0007000000000000 0 0 0 0 0 0 0000000f00000001 0001000000000004 0
check struct-as-cap "$HOLDER" Holder "text" -- \
  0001000100000000 0000000000000007 0000000100000000 0

if [[ $fails -ne 0 ]]; then
  echo "convert parity: $fails failure(s)" >&2
  exit 1
fi
echo "ok convert parity"
