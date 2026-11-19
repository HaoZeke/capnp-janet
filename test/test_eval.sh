#!/usr/bin/env bash
# capnp-eval matches `capnp eval` on consts and field defaults.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
BIN="$BUILD/capnp-eval"
if [[ ! -x "$BIN" ]]; then
  echo "SKIP: capnp-eval not built" >&2
  exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
check() {
  local schema="$1" expr="$2"
  capnp eval "$schema" "$expr" >"$TMP/ref.txt"
  "$BIN" "$schema" "$expr" >"$TMP/ours.txt"
  diff -u "$TMP/ref.txt" "$TMP/ours.txt"
}
check "$ROOT/schema/codegen-features.capnp" answer
check "$ROOT/schema/codegen-features.capnp" greeting
check "$ROOT/schema/defaults.capnp" Point.x
check "$ROOT/schema/defaults.capnp" Wrap.origin
check "$ROOT/schema/defaults.capnp" Wrap.origin.x
check "$ROOT/schema/defaults.capnp" Wrap.labels
check "$ROOT/schema/defaults.capnp" 'Wrap.labels[0]'
echo "ok eval"
