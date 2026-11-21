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

# compare NAME SCHEMA ROOT FORMATS: converts $TMP/in.bin with "${extra[@]}"
extra=()
compare() {
  local name="$1" schema="$2" root="$3" fmts="$4" fmt ref_rc our_rc
  for fmt in $fmts; do
    ref_rc=0
    our_rc=0
    capnp convert -I"$INC" "binary:$fmt" "${extra[@]}" "$schema" "$root" \
      <"$TMP/in.bin" >"$TMP/ref.out" 2>/dev/null || ref_rc=$?
    "$BIN" convert -I"$INC" "binary:$fmt" "${extra[@]}" "$schema" "$root" \
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

# check_framed NAME SCHEMA ROOT FORMATS -- HEX...: HEX starts with the
# segment table
check_framed() {
  local name="$1" schema="$2" root="$3" fmts="$4"
  shift 5
  python3 -c 'import struct, sys
sys.stdout.buffer.write(b"".join(struct.pack("<Q", int(x, 16)) for x in sys.argv[1:]))' \
    "$@" >"$TMP/in.bin"
  compare "$name" "$schema" "$root" "$fmts"
}

# check NAME SCHEMA ROOT FORMATS -- HEX...
check() {
  local name="$1" schema="$2" root="$3" fmts="$4"
  shift 5
  words "$@" >"$TMP/in.bin"
  compare "$name" "$schema" "$root" "$fmts"
}

# check_text NAME SCHEMA ROOT FORMATS TEXT: input encoded by `capnp encode`
check_text() {
  printf '%s' "$5" | capnp encode -I"$INC" "$2" "$3" >"$TMP/in.bin"
  compare "$1" "$2" "$3" "$4"
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

# parity_ptr SLOT PTR CONTENT...: a Parity root with every field zero but
# pointer SLOT, followed by CONTENT. The root is 1 + 10 + 32 words.
parity_ptr() {
  local slot="$1" ptr="$2" i
  shift 2
  echo 0020000a00000000
  for ((i = 0; i < 42; i++)); do
    if ((i == 10 + slot)); then echo "$ptr"; else echo 0; fi
  done
  echo "$@"
}

# Wider list elements: low bytes per element, bools by bit index from the
# start, and a void list read as empty structs. Offsets point past the root.
check u8-from-int16-list "$PARITY" Parity "text json" -- \
  $(parity_ptr 9 0000001300000059 0000000003040102)
check bool-from-byte-list "$PARITY" Parity "text json" -- \
  $(parity_ptr 4 000000120000006d 0000000000000203)
check bool-from-int64-list "$PARITY" Parity "text json" -- \
  $(parity_ptr 4 000000150000006d 0000000000000006 0)
check structs-from-void-list "$PARITY" Parity "text json" -- \
  $(parity_ptr 18 0000001000000035)

# A double-far landing pad only has to be a far pointer.
check_framed pad-with-double-bit "$PARITY" Parity "text json binary" -- \
  0000000100000001 3 0000000100000006 0000000100000016 0000000100000014 2a
check_framed tag-with-offset "$PARITY" Parity "text json binary" -- \
  0000000100000001 3 0000000100000006 0000000100000012 0000000100000014 2a
check_framed pad-not-far "$PARITY" Parity "text json binary" -- \
  0000000100000001 3 0000000100000006 0000000100000010 0000000100000014 2a

# A null root reads as an empty struct; any other non-struct root fails.
check null-root "$PARITY" Parity "text json binary packed flat canonical" -- 0
check list-root "$PARITY" Parity "text json binary packed flat canonical" -- \
  0000000100000001 0000000000000005

# Binary output is a copy into C++'s MallocMessageBuilder: fixed-size
# segments under --segment-size, growing ones from 1024 words otherwise.
SMALL='(vText = "hello", lI32 = [1,2,3], vStruct = (x = 1, y = 2), lText = ["a","bb"], lBool = [true, false, true])'
for ss in 1 2 3 5 8 16 64; do
  extra=("--segment-size=$ss")
  check_text "segment-size-$ss" "$PARITY" Parity "binary packed" "$SMALL"
done
extra=()
BIG="(vData = 0x\"$(printf 'ab%.0s' $(seq 5000))\", lU16 = [$(seq -s, 1 3000)], lText = [\"x\", \"y\"])"
check_text big-message "$PARITY" Parity "binary packed" "$BIG"

if [[ $fails -ne 0 ]]; then
  echo "convert parity: $fails failure(s)" >&2
  exit 1
fi
echo "ok convert parity"
