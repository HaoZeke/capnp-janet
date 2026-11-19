#!/usr/bin/env bash
# Dynamic schema, text, JSON, and CLI conversion parity against the official
# capnp tool.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
BIN="$BUILD/test_dyn"
CLI="$BUILD/capnp-eval"
SCHEMA="$ROOT/schema/conform.capnp"
PARITY_SCHEMA="$ROOT/schema/parity.capnp"
if [[ ! -x "$BIN" || ! -x "$CLI" ]]; then
  echo "SKIP: dyn tools not built" >&2
  exit 0
fi
if ! command -v capnp >/dev/null 2>&1; then
  echo "SKIP: no capnp CLI" >&2
  exit 0
fi
INC="$(dirname "$(dirname "$(command -v capnp)")")/include"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# -o- is the compiler's schema parse: a CodeGeneratorRequest on stdout.
capnp compile -o- "$SCHEMA" >"$TMP/request.bin"
capnp compile -I"$INC" -o- "$PARITY_SCHEMA" >"$TMP/parity_req.bin"
test -s "$TMP/request.bin"
test -s "$TMP/parity_req.bin"

capnp encode "$SCHEMA" Conform >"$TMP/in.bin" <<'EOF'
( flag = false,
  tiny = 3,
  wide = 99,
  ratio = 2.5,
  title = "hi",
  blob = "abc",
  inner = (n = 8, label = "in"),
  buckets = [
    (name = "a", scores = [1, 2]),
    (name = "b", scores = [3])
  ],
  matrix = [["x", "yy"], ["z"]],
  note = "hey",
  meta = (rev = 9, comment = "c") )
EOF
"$BIN" read "$TMP/request.bin" "$TMP/in.bin"
"$BIN" edges "$TMP/request.bin" "$TMP/parity_req.bin"
"$BIN" canon "$TMP/in.bin" "$TMP/official.can"

"$BIN" text-encode "$TMP/request.bin" "$TMP/in.bin" "$TMP/our.txt"
printf '\n' >>"$TMP/our.txt"
capnp decode "$SCHEMA" Conform <"$TMP/in.bin" >"$TMP/official.txt"
cmp "$TMP/our.txt" "$TMP/official.txt"

"$BIN" text-decode "$TMP/request.bin" "$TMP/official.txt" "$TMP/decoded.bin"
"$BIN" canon "$TMP/decoded.bin" "$TMP/decoded.can"
cmp "$TMP/decoded.can" "$TMP/official.can"

"$BIN" json-encode "$TMP/request.bin" "$TMP/in.bin" "$TMP/our.json"
printf '\n' >>"$TMP/our.json"
capnp convert binary:json "$SCHEMA" Conform <"$TMP/in.bin" >"$TMP/official.json"
cmp "$TMP/our.json" "$TMP/official.json"

"$BIN" json-decode "$TMP/request.bin" "$TMP/official.json" "$TMP/from-json.bin"
"$BIN" canon "$TMP/from-json.bin" "$TMP/json-decoded.can"
cmp "$TMP/json-decoded.can" "$TMP/official.can"

# Differential check on Parity schema (every scalar/list type, unions, groups,
# defaults, generics, $Json annotations, non-finite floats, 64-bit ints, UTF-8).
cat >"$TMP/parity.txt" <<'EOF'
( vVoid = void,
  vBool = false,
  vI8 = -128,
  vI16 = -32768,
  vI32 = -2147483648,
  vI64 = -9223372036854775808,
  vU8 = 255,
  vU16 = 65535,
  vU32 = 4294967295,
  vU64 = 18446744073709551615,
  vF32 = inf,
  vF64 = -inf,
  vText = "café\t\n\r\"\\\x01",
  vData = "\x00\x7f\x80\xff",
  vEnum = green,
  vStruct = (x = -1, y = 2),
  lVoid = [void, void],
  lBool = [true, false, true, true, false, false, true, false, true],
  lI8 = [-128, 0, 127],
  lI16 = [-32768, 0, 32767],
  lI32 = [-2147483648, 0, 2147483647],
  lI64 = [-9223372036854775808, 0, 9223372036854775807],
  lU8 = [0, 128, 255],
  lU16 = [0, 32768, 65535],
  lU32 = [0, 3000000000, 4294967295],
  lU64 = [0, 9007199254740993, 18446744073709551615],
  lF32 = [0, 1.5, inf, -inf, nan],
  lF64 = [0, 1e306, inf, -inf, nan],
  lText = ["", "a\"b", "line\nnext"],
  lData = [0x"00 ff", "xyz"],
  lEnum = [red, green, blue],
  lStruct = [(x = 1, y = 2), (x = 3, y = 4)],
  lNested = [[(x = 5, y = 6)], []],
  renamed = "hello_json",
  b64Data = "fred",
  hexData = "plugh",
  flatGroup = (alpha = 11, beta = "twelve"),
  discUnion = (optB = (bNum = 99)),
  valUnion = (vNum = 1234),
  voidUnion = (none = void),
  extDisc = (flat = (subName = "sub", subCode = 42)),
  rawJson = (array = [(number = 12.5), (string = "raw"), (object = [(name = "k", value = (boolean = true))]), (null = void)]),
  boxedText = (value = "in_box"),
  paired = (left = "data_left", right = [10, 20, 30]),
  nestedBox = (value = (value = (x = 9, y = 8))),
  uCount = 12345678901234567890 )
EOF

capnp encode -I"$INC" "$PARITY_SCHEMA" Parity <"$TMP/parity.txt" >"$TMP/parity.bin"
"$CLI" encode -I"$INC" "$PARITY_SCHEMA" Parity <"$TMP/parity.txt" >"$TMP/our_parity.bin"
capnp convert binary:canonical <"$TMP/parity.bin" >"$TMP/parity.can"
"$CLI" convert binary:canonical <"$TMP/our_parity.bin" >"$TMP/our_parity.can"
cmp "$TMP/parity.can" "$TMP/our_parity.can"

for fmt in binary packed flat flat-packed canonical text json; do
  capnp convert -I"$INC" "binary:$fmt" "$PARITY_SCHEMA" Parity <"$TMP/parity.bin" >"$TMP/ref.$fmt"
  "$CLI" convert -I"$INC" "binary:$fmt" "$PARITY_SCHEMA" Parity <"$TMP/parity.bin" >"$TMP/our.$fmt"
  cmp "$TMP/ref.$fmt" "$TMP/our.$fmt"
done

for fmt in text json; do
  capnp convert --short -I"$INC" "binary:$fmt" "$PARITY_SCHEMA" Parity <"$TMP/parity.bin" >"$TMP/ref.short.$fmt"
  "$CLI" convert --short -I"$INC" "binary:$fmt" "$PARITY_SCHEMA" Parity <"$TMP/parity.bin" >"$TMP/our.short.$fmt"
  cmp "$TMP/ref.short.$fmt" "$TMP/our.short.$fmt"
done

for from in binary packed flat flat-packed canonical text json; do
  "$CLI" convert -I"$INC" "$from:canonical" "$PARITY_SCHEMA" Parity <"$TMP/ref.$from" >"$TMP/from_$from.can"
  cmp "$TMP/from_$from.can" "$TMP/parity.can"
done

# Generic root type on CLI
capnp convert --short -I"$INC" text:json "$PARITY_SCHEMA" 'Box(Text)' <<< '(value = "generic_root")' >"$TMP/ref_box.json"
"$CLI" convert --short -I"$INC" text:json "$PARITY_SCHEMA" 'Box(Text)' <<< '(value = "generic_root")' >"$TMP/our_box.json"
cmp "$TMP/ref_box.json" "$TMP/our_box.json"

# Also verify negative zero (-0.0) in initial text encode matches capnp encode
capnp encode -I"$INC" "$PARITY_SCHEMA" Parity <<< '(lF32 = [-0.0], lF64 = [-0.0])' >"$TMP/negz_ref.bin"
"$CLI" encode -I"$INC" "$PARITY_SCHEMA" Parity <<< '(lF32 = [-0.0], lF64 = [-0.0])' >"$TMP/negz_our.bin"
cmp "$TMP/negz_ref.bin" "$TMP/negz_our.bin"

# More than eight list elements forces both piece tables to grow. A
# failed growth must not free a block the list still owns.
capnp encode "$SCHEMA" Conform >"$TMP/grow.bin" <<'EOF'
( flag = false,
  tiny = 1,
  wide = 1,
  ratio = 1,
  title = "t",
  blob = "b",
  inner = (n = 1, label = "l"),
  buckets = [(name = "a", scores = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])],
  matrix = [["x"]],
  note = "n",
  meta = (rev = 1, comment = "c") )
EOF
"$BIN" grow "$TMP/request.bin" "$TMP/grow.bin"

"$BIN" reject "$TMP/request.bin" Conform text '(tiny = 1, tiny = 2)' tiny
"$BIN" reject "$TMP/request.bin" Conform json '{"tiny":1,"tiny":2}' tiny
"$BIN" reject "$TMP/request.bin" Conform text '(nope = 1)' nope
"$BIN" reject "$TMP/request.bin" Conform json '{"nope":1,"nope":2}' nope
"$BIN" reject "$TMP/request.bin" Conform text '(tiny = 1) junk' junk
"$BIN" reject "$TMP/request.bin" Conform json '{"tiny":"abc"}' 'bad value for field tiny: "abc"'
"$BIN" reject "$TMP/request.bin" Conform json '{"tiny":999}' 'bad value for field tiny: 999'
"$BIN" reject "$TMP/request.bin" Conform json '{"tiny":1.5}' 'bad value for field tiny: 1.5'
"$BIN" reject "$TMP/request.bin" Conform json '{"inner":1}' 'bad value for field inner: 1'
"$BIN" reject "$TMP/request.bin" Conform json '{"buckets":[{"scores":["no"]}]}' 'bad value for field scores: "no"'
printf '%s\n' '{"tiny":1,"nope":9}' >"$TMP/unk.json"
"$BIN" json-decode "$TMP/request.bin" "$TMP/unk.json" "$TMP/unk.bin"

cat >"$TMP/u.capnp" <<'EOF'
@0x9a8b7c6d5e4f3012;
using Json = import "/capnp/compat/json.capnp";
struct U $Json.discriminator(name = "kind", valueName = "payload") {
  union {
    a @0 :Void;
    b @1 :UInt32;
  }
  extra @2 :Text;
}
EOF
capnp compile -I"$INC" -o- "$TMP/u.capnp" >"$TMP/u_req.bin"
"$BIN" disc "$TMP/u_req.bin" ok
"$BIN" disc "$TMP/u_req.bin" oob

echo "ok dyn"
