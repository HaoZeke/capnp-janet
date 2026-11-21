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
NODE="$ROOT/test/fixtures/node.capnp"
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

# compare NAME SCHEMA ROOT FORMATS: converts $TMP/in.bin from $in_fmt with
# "${extra[@]}"
extra=()
in_fmt=binary
compare() {
  local name="$1" schema="$2" root="$3" fmts="$4" fmt ref_rc our_rc
  for fmt in $fmts; do
    ref_rc=0
    our_rc=0
    capnp convert -I"$INC" "$in_fmt:$fmt" "${extra[@]}" "$schema" "$root" \
      <"$TMP/in.bin" >"$TMP/ref.out" 2>/dev/null || ref_rc=$?
    "$BIN" convert -I"$INC" "$in_fmt:$fmt" "${extra[@]}" "$schema" "$root" \
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

# Bit 31 of a composite list tag word is ignored, as C++ masks to 29 bits.
check composite-tag-bit31 "$PARITY" Parity "text json binary canonical" -- \
  $(parity_ptr 18 0000000f00000035 0000000180000004 0)

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
# MallocMessageBuilder has no segment count limit: 603 segments here.
MANY="(lText = [$(seq -s, -f '"s%g"' 0 599)])"
extra=("--segment-size=1")
check_text many-segments "$PARITY" Parity "binary packed" "$MANY"
extra=()

# JSON base64 and hex strings decode as kj does with its error flag
# ignored: stray characters and padding skipped, bad hex digits read as 0.
in_fmt=json
i=0
for v in '"QU"' '"Q"' '"QU=JD"' '"QU!JD"' '"=QUJD"' '"QUJDRA="' '"-_"'; do
  printf '{"b64Data": %s}' "$v" >"$TMP/in.bin"
  compare "json-base64-$((i++))" "$PARITY" Parity "text binary"
done
for v in '"0aF"' '"zz41"' '"4g"' '"g1"'; do
  printf '{"hexData": %s}' "$v" >"$TMP/in.bin"
  compare "json-hex-$((i++))" "$PARITY" Parity "text binary"
done

# Stream input takes at most 511 segments.
in_fmt=binary
for n in 511 512; do
  python3 -c 'import struct, sys
n = int(sys.argv[1])
hdr = struct.pack("<I", n - 1) + struct.pack("<I", 4) + struct.pack("<I", 1) * (n - 1)
hdr += b"\0" * (len(hdr) % 8)
body = struct.pack("<QQQQ", 0x0002000100000000, 7, 0, 0) + b"\0" * 8 * (n - 1)
open(sys.argv[2], "wb").write(hdr + body)' "$n" "$TMP/in.bin"
  compare "segments-$n" "$NODE" Node "text binary"
done

# The C++ lexer joins adjacent operator characters into one token, so `=-5`
# is a parse error while `= -5` and `= - 5` are fine.
in_fmt=text
i=0
for v in '(vI32 =-5)' '(vI32 =- 5)' '(vF64 =-inf)' '(vF64 =.5)' '(vI32 =+5)' \
  '(vF64 = -.5)' '(vI32 = - 5)' '(vI32=#c
-5)' '(lI32=[1, - 2])' '(vF64 = -inf)'; do
  printf '%s' "$v" >"$TMP/in.bin"
  compare "text-operator-$((i++))" "$PARITY" Parity "text binary"
done

# kj parses integer literals modulo 2^64.
for v in '(vU64 = 99999999999999999999)' '(vU64 = 18446744073709551616)' \
  '(vU64 = 0x1ffffffffffffffff)' '(vU64 = 02777777777777777777777)' \
  '(vI64 = -99999999999999999999)' "(vU8 = 1$(printf '0%.0s' {1..200}))"; do
  printf '%s' "$v" >"$TMP/in.bin"
  compare "text-integer-$((i++))" "$PARITY" Parity "text binary"
done

# kj lexes `0x` hex, `0` octal, then decimal integers before floats, and
# only a lowercase `x` starts hex. `08` is the two tokens `0` and `8`.
for v in '(vI32 = 08)' '(vF64 = 08.5)' '(vF64 = 09e1)' '(vI32 = 0X1F)' \
  '(vData = 0X"01")' '(vI32 = 007)' '(vF64 = 00.5)' '(vF64 = 0e5)' \
  '(vF64 = 1.)' '(vF64 = 1e)' '(vI32 = 0x1F)' '(vI32 = 0x1g)'; do
  printf '%s' "$v" >"$TMP/in.bin"
  compare "text-number-$((i++))" "$PARITY" Parity "text binary"
done

# Float literals have no length limit in either codec.
Z=$(printf '0%.0s' {1..130})
for v in "(vF64 = ${Z}1.5)" "(vF64 = 1.${Z}5)" "(vF32 = 1${Z}e-130)"; do
  printf '%s' "$v" >"$TMP/in.bin"
  compare "text-long-float-$((i++))" "$PARITY" Parity "text binary"
done
in_fmt=json
for v in "{\"vF64\": 1.${Z}5}" "{\"vF64\": 1${Z}}" \
  "{\"vF64\": -0.${Z}${Z}1e300}"; do
  printf '%s' "$v" >"$TMP/in.bin"
  compare "json-long-float-$((i++))" "$PARITY" Parity "text binary"
done
in_fmt=binary

# `capnp convert` lifts the nesting and traversal limits. A chain of 200
# structs and nine reads of one 1M-word list both pass the defaults.
python3 - "$TMP/in.bin" <<'EOF'
import struct, sys
d = 200
w = [0x0002000100000000]
for i in range(d + 1):
    w += [7 if i == d else 0, 0x0002000100000004 if i < d else 0, 0]
out = open(sys.argv[1], "wb")
out.write(struct.pack("<II", 0, len(w)) + b"".join(struct.pack("<Q", x) for x in w))
EOF
compare deep-chain "$NODE" Node "text json binary canonical"
python3 - "$TMP/in.bin" <<'EOF'
import struct, sys
n, count = 9, 1 << 20
w = [0x0002000100000000]
shared = 1 + 3 * n
for i in range(n):
    base = 1 + 3 * i
    off = shared - (base + 2) - 1
    nxt = 0x0002000100000004 if i < n - 1 else 0
    w += [0, nxt, ((count << 3 | 5) << 32) | ((off << 2) | 1)]
w += [0] * count
out = open(sys.argv[1], "wb")
out.write(struct.pack("<II", 0, len(w)) + b"".join(struct.pack("<Q", x) for x in w))
EOF
compare amplified-reads "$NODE" Node "binary"

if [[ $fails -ne 0 ]]; then
  echo "convert parity: $fails failure(s)" >&2
  exit 1
fi
echo "ok convert parity"
