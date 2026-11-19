#!/usr/bin/env bash
# capnp-eval matches `capnp eval` on consts and field defaults across text,
# JSON, and binary formats.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
BIN="$BUILD/capnp-eval"
if [[ ! -x "$BIN" ]]; then
  echo "SKIP: capnp-eval not built" >&2
  exit 77
fi
INC="$(dirname "$(dirname "$(command -v capnp)")")/include"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
check() {
  local schema="$1" expr="$2"
  shift 2
  capnp eval -I"$INC" "$@" "$schema" "$expr" >"$TMP/ref.txt"
  "$BIN" eval -I"$INC" "$@" "$schema" "$expr" >"$TMP/ours.txt"
  cmp "$TMP/ref.txt" "$TMP/ours.txt"
}
check "$ROOT/schema/codegen-features.capnp" answer
check "$ROOT/schema/codegen-features.capnp" greeting
check "$ROOT/schema/defaults.capnp" Point.x
check "$ROOT/schema/defaults.capnp" Wrap.origin
check "$ROOT/schema/defaults.capnp" Wrap.origin.x
check "$ROOT/schema/defaults.capnp" Wrap.labels
check "$ROOT/schema/defaults.capnp" 'Wrap.labels[0]'
DEEP="$ROOT/schema/evaldeep.capnp"
check "$DEEP" 'sample.items[1].name'
check "$DEEP" 'sample.nested[1][0].n'
check "$DEEP" 'sample.nested[0][0].name'
check "$DEEP" 'sample.scores[2]'
check "$DEEP" 'sample.meta.rev'
check "$DEEP" 'sample.meta.note'
check "$DEEP" 'sample.tag'
check "$DEEP" 'sample.meta'
check "$DEEP" 'sample.items'
check "$DEEP" 'sample'
check "$DEEP" 'sample' --short
check "$DEEP" 'sample' -ojson
check "$DEEP" 'sample' -ojson --short
check "$DEEP" 'sample' -b
check "$DEEP" 'sample' -p
check "$DEEP" 'sample' --flat
check "$DEEP" 'sample' -ocanonical
PARITY="$ROOT/schema/parity.capnp"
check "$PARITY" kColor
check "$PARITY" kBig
check "$PARITY" kPoint
check "$PARITY" Parity.vEnum
check "$PARITY" Parity.vStruct
check "$PARITY" Parity.vData
check "$PARITY" 'Parity.lI32[1]'

# A legal path that used to overflow the fixed 4096-byte compile command
# and leave the shell quote open.
LONG="$TMP/long"
part=$(python3 -c 'print("d"*255)')
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  LONG="$LONG/$part"
done
mkdir -p "$LONG"
tail=$(python3 -c 'print("e"*180)')
cp "$ROOT/schema/codegen-features.capnp" "$LONG/$tail.capnp"
SCHEMA_LONG="$LONG/$tail.capnp"
test "${#SCHEMA_LONG}" -gt 4000
check "$SCHEMA_LONG" answer
echo "ok eval"
