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
  exit 77
fi
if ! command -v capnp >/dev/null 2>&1; then
  echo "SKIP: no capnp CLI" >&2
  exit 77
fi
INC="$(dirname "$(dirname "$(command -v capnp)")")/include"
TMP=$(mktemp -d)
if grep -q 'Wait!' "$ROOT/src/capnp_dyn.c"; then
  echo "encoder still has a scratch comment" >&2
  exit 1
fi
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

# A repeated text field keeps the last value, matching C++ 1.4.0.
capnp encode "$SCHEMA" Conform >"$TMP/duptxt-ref.bin" <<'EOF'
(tiny = 1, tiny = 2)
EOF
"$CLI" encode "$SCHEMA" Conform >"$TMP/duptxt-our.bin" <<'EOF'
(tiny = 1, tiny = 2)
EOF
cmp "$TMP/duptxt-ref.bin" "$TMP/duptxt-our.bin"
"$BIN" canon "$TMP/duptxt-ref.bin" "$TMP/duptxt-ref.can"
"$BIN" canon "$TMP/duptxt-our.bin" "$TMP/duptxt-our.can"
cmp "$TMP/duptxt-ref.can" "$TMP/duptxt-our.can"
capnp decode "$SCHEMA" Conform <"$TMP/duptxt-our.bin" | grep -q 'tiny = 2'
"$BIN" reject "$TMP/request.bin" Conform text '(nope = 1)' nope
# A repeated JSON name keeps the last value, matching capnp 1.4.0.
# Unknown names stay ignored, so a repeated unknown name is not an error.
printf '%s\n' '{"nope":1,"nope":2}' >"$TMP/dup-unk.json"
"$BIN" json-decode "$TMP/request.bin" "$TMP/dup-unk.json" "$TMP/dup-unk.bin"
printf '%s\n' '{"vI8":1,"vI8":2}' | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dup-ref.bin"
printf '%s\n' '{"vI8":1,"vI8":2}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dup-our.bin"
"$BIN" canon "$TMP/dup-ref.bin" "$TMP/dup-ref.can"
"$BIN" canon "$TMP/dup-our.bin" "$TMP/dup-our.can"
cmp "$TMP/dup-ref.can" "$TMP/dup-our.can"
capnp decode -I"$INC" "$PARITY_SCHEMA" Parity <"$TMP/dup-our.bin" | grep -q 'vI8 = 2'
printf '%s\n' '{"vText":"a","vText":"bb"}' | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupt-ref.bin"
printf '%s\n' '{"vText":"a","vText":"bb"}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupt-our.bin"
"$BIN" canon "$TMP/dupt-ref.bin" "$TMP/dupt-ref.can"
"$BIN" canon "$TMP/dupt-our.bin" "$TMP/dupt-our.can"
cmp "$TMP/dupt-ref.can" "$TMP/dupt-our.can"
# An empty list between two text values must survive a tail reclaim.
# Binary, canonical, text, and JSON match capnp 1.4.0, which accepts a
# zero-word list aimed at the segment end.
empty_list() {
  local json="$1" name="$2"
  printf '%s\n' "$json" | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/$name-ref.bin"
  printf '%s\n' "$json" | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/$name-our.bin"
  cmp "$TMP/$name-ref.bin" "$TMP/$name-our.bin"
  "$BIN" canon "$TMP/$name-ref.bin" "$TMP/$name-ref.can"
  "$BIN" canon "$TMP/$name-our.bin" "$TMP/$name-our.can"
  cmp "$TMP/$name-ref.can" "$TMP/$name-our.can"
  printf '%s\n' "$json" | capnp convert -I"$INC" json:json "$PARITY_SCHEMA" Parity >"$TMP/$name-ref.json"
  printf '%s\n' "$json" | "$CLI" convert -I"$INC" json:json "$PARITY_SCHEMA" Parity >"$TMP/$name-our.json"
  cmp "$TMP/$name-ref.json" "$TMP/$name-our.json"
  printf '%s\n' "$json" | capnp convert -I"$INC" json:text "$PARITY_SCHEMA" Parity >"$TMP/$name-ref.txt"
  printf '%s\n' "$json" | "$CLI" convert -I"$INC" json:text "$PARITY_SCHEMA" Parity >"$TMP/$name-our.txt"
  cmp "$TMP/$name-ref.txt" "$TMP/$name-our.txt"
  capnp decode -I"$INC" "$PARITY_SCHEMA" Parity <"$TMP/$name-our.bin" >/dev/null
}
empty_list '{"vText":"hello world!!!!!","lI32":[],"vText":"a"}' elist
grep -q '"lI32": \[\]' "$TMP/elist-our.json"
empty_list '{"vText":"hello world!!!!!","lText":[],"vText":"a"}' elist-text
empty_list '{"lI64":[1,2,3,4],"lI64":[]}' elist-i64
# A valid surrogate pair is one UTF-8 scalar. A lone surrogate is an
# error. C++ 1.4.0 stores each \u unit as CESU-8 and accepts a lone one.
printf '%s\n' '{"vText":"\uD83D\uDE00"}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/surr.bin"
"$BIN" canon "$TMP/surr.bin" "$TMP/surr.can"
# U+1F600 is f0 9f 98 80. The canonical body is not framed.
python3 - "$TMP/surr.can" <<'PY'
import pathlib, sys
b = pathlib.Path(sys.argv[1]).read_bytes()
if bytes([0xF0, 0x9F, 0x98, 0x80]) not in b:
    raise SystemExit("missing utf-8 smile")
if bytes([0xED, 0xA0, 0xBD]) in b:
    raise SystemExit("stored cesu-8")
PY
if printf '%s\n' '{"vText":"\uD83D"}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/lone.bin" 2>"$TMP/lone.err"
then
  echo "lone surrogate was accepted" >&2
  exit 1
fi
grep -q "lone surrogate" "$TMP/lone.err"
if printf '%s\n' '{"vText":"\uDE00"}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/lone2.bin" 2>"$TMP/lone2.err"
then
  echo "lone low surrogate was accepted" >&2
  exit 1
fi
grep -q "lone surrogate" "$TMP/lone2.err"
printf '%s\n' '{"uNote":"first","uNote":"second"}' | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupu-ref.bin"
printf '%s\n' '{"uNote":"first","uNote":"second"}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupu-our.bin"
"$BIN" canon "$TMP/dupu-ref.bin" "$TMP/dupu-ref.can"
"$BIN" canon "$TMP/dupu-our.bin" "$TMP/dupu-our.can"
cmp "$TMP/dupu-ref.can" "$TMP/dupu-our.can"
# Switching arms selects the last one and leaves the previous arm
# reachable. Raw and canonical bytes match C++ 1.4.0.
printf '%s\n' '{"uNote":"first","uCount":9}' | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupa-ref.bin"
printf '%s\n' '{"uNote":"first","uCount":9}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupa-our.bin"
cmp "$TMP/dupa-ref.bin" "$TMP/dupa-our.bin"
"$BIN" canon "$TMP/dupa-ref.bin" "$TMP/dupa-ref.can"
"$BIN" canon "$TMP/dupa-our.bin" "$TMP/dupa-our.can"
cmp "$TMP/dupa-ref.can" "$TMP/dupa-our.can"
capnp decode -I"$INC" "$PARITY_SCHEMA" Parity <"$TMP/dupa-our.bin" | grep -q 'uCount = 9'
grep -a -q first "$TMP/dupa-our.bin"
capnp encode -I"$INC" "$PARITY_SCHEMA" Parity >"$TMP/dupat-ref.bin" <<'EOF'
(uNote = "first", uCount = 9)
EOF
"$CLI" encode -I"$INC" "$PARITY_SCHEMA" Parity >"$TMP/dupat-our.bin" <<'EOF'
(uNote = "first", uCount = 9)
EOF
cmp "$TMP/dupat-ref.bin" "$TMP/dupat-our.bin"
"$BIN" canon "$TMP/dupat-ref.bin" "$TMP/dupat-ref.can"
"$BIN" canon "$TMP/dupat-our.bin" "$TMP/dupat-our.can"
cmp "$TMP/dupat-ref.can" "$TMP/dupat-our.can"
printf '%s\n' '{"valUnion":"vTxt","v_payload":"a","v_payload":"zz"}' | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupv-ref.bin"
printf '%s\n' '{"valUnion":"vTxt","v_payload":"a","v_payload":"zz"}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupv-our.bin"
"$BIN" canon "$TMP/dupv-ref.bin" "$TMP/dupv-ref.can"
"$BIN" canon "$TMP/dupv-our.bin" "$TMP/dupv-our.can"
cmp "$TMP/dupv-ref.can" "$TMP/dupv-our.can"
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
"$BIN" disc "$TMP/u_req.bin" huge

# A discriminator tag clears only the chosen arm. The previous arm
# stays reachable, matching Builder::clear.
cat >"$TMP/utag.capnp" <<'EOF'
@0x9a8b7c6d5e4f3013;
using Json = import "/capnp/compat/json.capnp";
struct UTag $Json.discriminator(name = "kind") {
  union {
    note @0 :Text;
    count @1 :UInt32;
  }
}
EOF
printf '%s\n' '{"note":"first","kind":"count"}' | capnp convert -I"$INC" json:binary "$TMP/utag.capnp" UTag >"$TMP/utag-ref.bin"
printf '%s\n' '{"note":"first","kind":"count"}' | "$CLI" convert -I"$INC" json:binary "$TMP/utag.capnp" UTag >"$TMP/utag-our.bin"
cmp "$TMP/utag-ref.bin" "$TMP/utag-our.bin"
"$BIN" canon "$TMP/utag-ref.bin" "$TMP/utag-ref.can"
"$BIN" canon "$TMP/utag-our.bin" "$TMP/utag-our.can"
cmp "$TMP/utag-ref.can" "$TMP/utag-our.can"
grep -a -q first "$TMP/utag-our.bin"
capnp decode -I"$INC" "$TMP/utag.capnp" UTag <"$TMP/utag-our.bin" | grep -q 'count = 0'

# A repeated struct that omits a defaulted field must not keep the
# previous field. The same reuse must not leak a shrunk tail text into
# a later struct.
printf '%s\n' '{"vStruct":{"x":1,"y":2},"vStruct":{"x":9}}' | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupst-ref.bin"
printf '%s\n' '{"vStruct":{"x":1,"y":2},"vStruct":{"x":9}}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupst-our.bin"
cmp "$TMP/dupst-ref.bin" "$TMP/dupst-our.bin"
"$BIN" canon "$TMP/dupst-ref.bin" "$TMP/dupst-ref.can"
"$BIN" canon "$TMP/dupst-our.bin" "$TMP/dupst-our.can"
cmp "$TMP/dupst-ref.can" "$TMP/dupst-our.can"
capnp encode -I"$INC" "$PARITY_SCHEMA" Parity >"$TMP/dupstt-ref.bin" <<'EOF'
(vStruct = (x = 1, y = 2), vStruct = (x = 9))
EOF
"$CLI" encode -I"$INC" "$PARITY_SCHEMA" Parity >"$TMP/dupstt-our.bin" <<'EOF'
(vStruct = (x = 1, y = 2), vStruct = (x = 9))
EOF
cmp "$TMP/dupstt-ref.bin" "$TMP/dupstt-our.bin"
printf '%s\n' '{"lStruct":[{"x":1,"y":2}],"lStruct":[{"x":5}]}' | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupls-ref.bin"
printf '%s\n' '{"lStruct":[{"x":1,"y":2}],"lStruct":[{"x":5}]}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupls-our.bin"
cmp "$TMP/dupls-ref.bin" "$TMP/dupls-our.bin"
capnp encode -I"$INC" "$PARITY_SCHEMA" Parity >"$TMP/duplst-ref.bin" <<'EOF'
(lStruct = [(x = 1, y = 2)], lStruct = [(x = 5)])
EOF
"$CLI" encode -I"$INC" "$PARITY_SCHEMA" Parity >"$TMP/duplst-our.bin" <<'EOF'
(lStruct = [(x = 1, y = 2)], lStruct = [(x = 5)])
EOF
cmp "$TMP/duplst-ref.bin" "$TMP/duplst-our.bin"
printf '%s\n' '{"uNote":"ABCD1234EFGH5678","uNote":"","vStruct":{}}' | capnp convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupsh-ref.bin"
printf '%s\n' '{"uNote":"ABCD1234EFGH5678","uNote":"","vStruct":{}}' | "$CLI" convert -I"$INC" json:binary "$PARITY_SCHEMA" Parity >"$TMP/dupsh-our.bin"
cmp "$TMP/dupsh-ref.bin" "$TMP/dupsh-our.bin"
capnp encode -I"$INC" "$PARITY_SCHEMA" Parity >"$TMP/dupsht-ref.bin" <<'EOF'
(uNote = "ABCD1234EFGH5678", uNote = "", vStruct = ())
EOF
"$CLI" encode -I"$INC" "$PARITY_SCHEMA" Parity >"$TMP/dupsht-our.bin" <<'EOF'
(uNote = "ABCD1234EFGH5678", uNote = "", vStruct = ())
EOF
cmp "$TMP/dupsht-ref.bin" "$TMP/dupsht-our.bin"

# JSON decodes a non-flattened group and a named union into a
# temporary and keeps only the active arm. Text stays in place.
cat >"$TMP/grp.capnp" <<'EOF'
@0x9a8b7c6d5e4f3099;
using Json = import "/capnp/compat/json.capnp";
struct Wrap {
  grp :group {
    union {
      guText @0 :Text;
      guNum @1 :UInt32;
    }
    plain @2 :UInt16;
  }
  discTag :union $Json.discriminator(name = "tag") {
    dText @3 :Text;
    dNum @4 :UInt32;
  }
}
EOF
printf '%s\n' '{"grp":{"guText":"first","guNum":9}}' | capnp convert -I"$INC" json:binary "$TMP/grp.capnp" Wrap >"$TMP/grp-ref.bin"
printf '%s\n' '{"grp":{"guText":"first","guNum":9}}' | "$CLI" convert -I"$INC" json:binary "$TMP/grp.capnp" Wrap >"$TMP/grp-our.bin"
cmp "$TMP/grp-ref.bin" "$TMP/grp-our.bin"
"$BIN" canon "$TMP/grp-ref.bin" "$TMP/grp-ref.can"
"$BIN" canon "$TMP/grp-our.bin" "$TMP/grp-our.can"
cmp "$TMP/grp-ref.can" "$TMP/grp-our.can"
printf '%s\n' '{"discTag":{"dText":"first","tag":"dNum"}}' | capnp convert -I"$INC" json:binary "$TMP/grp.capnp" Wrap >"$TMP/ntag-ref.bin"
printf '%s\n' '{"discTag":{"dText":"first","tag":"dNum"}}' | "$CLI" convert -I"$INC" json:binary "$TMP/grp.capnp" Wrap >"$TMP/ntag-our.bin"
cmp "$TMP/ntag-ref.bin" "$TMP/ntag-our.bin"
"$BIN" canon "$TMP/ntag-ref.bin" "$TMP/ntag-ref.can"
"$BIN" canon "$TMP/ntag-our.bin" "$TMP/ntag-our.can"
cmp "$TMP/ntag-ref.can" "$TMP/ntag-our.can"
printf '%s\n' '{"grp":{"plain":4,"guText":"keep","guNum":3}}' | capnp convert -I"$INC" json:binary "$TMP/grp.capnp" Wrap >"$TMP/gplain-ref.bin"
printf '%s\n' '{"grp":{"plain":4,"guText":"keep","guNum":3}}' | "$CLI" convert -I"$INC" json:binary "$TMP/grp.capnp" Wrap >"$TMP/gplain-our.bin"
cmp "$TMP/gplain-ref.bin" "$TMP/gplain-our.bin"
capnp encode -I"$INC" "$TMP/grp.capnp" Wrap >"$TMP/gtxt-ref.bin" <<'EOF'
(grp = (guText = "first", guNum = 9))
EOF
"$CLI" encode -I"$INC" "$TMP/grp.capnp" Wrap >"$TMP/gtxt-our.bin" <<'EOF'
(grp = (guText = "first", guNum = 9))
EOF
cmp "$TMP/gtxt-ref.bin" "$TMP/gtxt-our.bin"
# A group that holds a composite list must keep the element size
# capnp convert builds. A trimmed copy is shorter.
printf '%s\n' '{"call":{"op":"multiply","params":[{"literal":2},{"literal":3}]}}' | capnp convert -I"$INC" json:binary "$ROOT/schema/calculator.capnp" Expression >"$TMP/expr-ref.bin"
printf '%s\n' '{"call":{"op":"multiply","params":[{"literal":2},{"literal":3}]}}' | "$CLI" convert -I"$INC" json:binary "$ROOT/schema/calculator.capnp" Expression >"$TMP/expr-our.bin"
cmp "$TMP/expr-ref.bin" "$TMP/expr-our.bin"

# A second discriminator tag is an error. A single tag still matches,
# and a repeated ordinary field still keeps the last value.
set +e
printf '%s\n' '{"u_tag":"optA","aVal":"x","u_tag":"b_tag","bNum":9}' | capnp convert -I"$INC" json:binary "$ROOT/schema/parity.capnp" Parity >"$TMP/duptag-ref.bin" 2>"$TMP/duptag-ref.err"
ref=$?
printf '%s\n' '{"u_tag":"optA","aVal":"x","u_tag":"b_tag","bNum":9}' | "$CLI" convert -I"$INC" json:binary "$ROOT/schema/parity.capnp" Parity >"$TMP/duptag-our.bin" 2>"$TMP/duptag-our.err"
our=$?
set -e
test "$ref" -ne 0
test "$our" -ne 0
test ! -s "$TMP/duptag-ref.bin"
test ! -s "$TMP/duptag-our.bin"
printf '%s\n' '{"u_tag":"b_tag","bNum":9}' | capnp convert -I"$INC" json:binary "$ROOT/schema/parity.capnp" Parity >"$TMP/onetag-ref.bin"
printf '%s\n' '{"u_tag":"b_tag","bNum":9}' | "$CLI" convert -I"$INC" json:binary "$ROOT/schema/parity.capnp" Parity >"$TMP/onetag-our.bin"
cmp "$TMP/onetag-ref.bin" "$TMP/onetag-our.bin"

echo "ok dyn"
