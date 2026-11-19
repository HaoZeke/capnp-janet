#!/usr/bin/env bash
# Dynamic schema, text, and JSON against the official capnp tool.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
BIN="$BUILD/test_dyn"
SCHEMA="$ROOT/schema/conform.capnp"
if [[ ! -x "$BIN" ]]; then
  echo "SKIP: dyn tools not built" >&2
  exit 0
fi
if ! command -v capnp >/dev/null 2>&1; then
  echo "SKIP: no capnp CLI" >&2
  exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# -o- is the compiler's schema parse: a CodeGeneratorRequest on stdout.
capnp compile -o- "$SCHEMA" >"$TMP/request.bin"
test -s "$TMP/request.bin"
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
"$BIN" canon "$TMP/in.bin" "$TMP/official.can"

"$BIN" text-encode "$TMP/request.bin" "$TMP/in.bin" "$TMP/our.txt"
capnp encode "$SCHEMA" Conform <"$TMP/our.txt" >"$TMP/from-text.bin"
"$BIN" canon "$TMP/from-text.bin" "$TMP/from-text.can"
cmp "$TMP/from-text.can" "$TMP/official.can"

capnp decode "$SCHEMA" Conform <"$TMP/in.bin" >"$TMP/official.txt"
"$BIN" text-decode "$TMP/request.bin" "$TMP/official.txt" "$TMP/decoded.bin"
"$BIN" canon "$TMP/decoded.bin" "$TMP/decoded.can"
cmp "$TMP/decoded.can" "$TMP/official.can"

"$BIN" json-encode "$TMP/request.bin" "$TMP/in.bin" "$TMP/our.json"
capnp convert json:canonical "$SCHEMA" Conform <"$TMP/our.json" >"$TMP/from-json.can"
cmp "$TMP/from-json.can" "$TMP/official.can"

capnp convert binary:json "$SCHEMA" Conform <"$TMP/in.bin" >"$TMP/official.json"
"$BIN" json-decode "$TMP/request.bin" "$TMP/official.json" "$TMP/from-json.bin"
"$BIN" canon "$TMP/from-json.bin" "$TMP/json-decoded.can"
cmp "$TMP/json-decoded.can" "$TMP/official.can"
echo "ok dyn"
