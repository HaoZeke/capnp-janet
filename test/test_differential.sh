#!/usr/bin/env bash
# Compare this runtime with official capnp encode / convert on a schema that
# has unions, a group, defaults, List(List(Text)), and far pointers.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
BIN="$BUILD/test_conform"
SCHEMA="$ROOT/schema/conform.capnp"
if [[ ! -x "$BIN" ]]; then
  echo "SKIP: no $BIN" >&2
  exit 0
fi
if ! command -v capnp >/dev/null 2>&1; then
  echo "SKIP: no capnp CLI" >&2
  exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

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
"$BIN" read "$TMP/in.bin"

capnp encode "$SCHEMA" Conform >"$TMP/empty.bin" <<'EOF'
()
EOF
"$BIN" read-empty "$TMP/empty.bin"

"$BIN" canon-schema "$TMP/schema.can"
"$BIN" canon-reverse "$TMP/reverse.can"
cmp "$TMP/schema.can" "$TMP/reverse.can"

capnp convert binary:canonical "$SCHEMA" Conform <"$TMP/in.bin" >"$TMP/official.can"
cmp "$TMP/schema.can" "$TMP/official.can"

"$BIN" canon-far "$TMP/far.can"
cmp "$TMP/far.can" "$TMP/official.can"

capnp convert binary:packed "$SCHEMA" Conform <"$TMP/in.bin" >"$TMP/official.packed"
"$BIN" pack "$TMP/in.bin" "$TMP/our.packed"
cmp "$TMP/our.packed" "$TMP/official.packed"
echo "ok differential"
