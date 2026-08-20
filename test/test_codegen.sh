#!/usr/bin/env bash
# Smoke-test capnpc-janet against schema/addressbook.capnp.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${POLICYD_BUILD_DIR:-${CAPNP_JANET_BUILD:-$ROOT/build}}"
PLUGIN="$BUILD/capnpc-janet"
if [[ ! -x "$PLUGIN" ]]; then
  echo "SKIP: no $PLUGIN" >&2
  exit 0
fi
if ! command -v capnp >/dev/null 2>&1; then
  echo "SKIP: no capnp CLI" >&2
  exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$ROOT/schema/addressbook.capnp" "$TMP/"
cd "$TMP"
capnp compile "-o$PLUGIN" addressbook.capnp
test -f addressbook.janet
grep -q 'Person-data-words' addressbook.janet
grep -q 'Person-get-name' addressbook.janet
grep -q 'AddressBook-get-people' addressbook.janet
grep -q 'Person-PhoneNumber-data-words' addressbook.janet
grep -q 'Person-PhoneNumber-Type' addressbook.janet
# Nested names must not keep Cap'n dots (invalid Janet symbols).
if grep -q 'Person\.Phone' addressbook.janet; then
  echo "FAIL: dotted identifiers in generated Janet" >&2
  exit 1
fi

# UInt64/Int64 must emit get-u64 (not get-u32).
cp "$ROOT/schema/u64probe.capnp" "$TMP/"
capnp compile "-o$PLUGIN" u64probe.capnp
test -f u64probe.janet
grep -q 'U64Probe-get-id' u64probe.janet
grep -q 'U64Probe-get-signed' u64probe.janet
grep -q 'capnp/get-u64' u64probe.janet
if grep -E 'get-id|get-signed' u64probe.janet | grep -q 'get-u32'; then
  echo "FAIL: u64 fields still emit get-u32" >&2
  exit 1
fi
# Interfaces carry the id, the ordinal and both struct shapes, so a
# caller need not spell them. A parameter struct sized wrong drops
# arguments past the end without complaint.
cp "$ROOT/schema/adder.capnp" "$TMP/"
capnp compile "-o$PLUGIN" adder.capnp
test -f adder.janet
grep -q 'Adder-interface-id' adder.janet
grep -q '0xea01e10cbc414411' adder.janet
grep -q 'Adder-methods' adder.janet
# add(a :Int64, b :Int64) -> (sum :Int64): two data words in, one out.
grep -q ':add {:ordinal 0 :params-dwords 2 :params-pwords 0 :results-dwords 1' adder.janet

# Every scalar width uses its signed/float-aware reader and carries the schema
# default. The zeroed wire representation must decode to these values.
cp "$ROOT/schema/codegen-features.capnp" "$TMP/"
capnp compile "-o$PLUGIN" codegen-features.capnp
test -f codegen-features.janet
grep -Eq 'capnp/get-bool ptr [0-9]+ true' codegen-features.janet
grep -Eq 'capnp/get-i8 ptr [0-9]+ -7' codegen-features.janet
grep -Eq 'capnp/get-i16 ptr [0-9]+ -700' codegen-features.janet
grep -Eq 'capnp/get-i32 ptr [0-9]+ -70000' codegen-features.janet
grep -Eq 'capnp/get-i64 ptr [0-9]+ -700000' codegen-features.janet
grep -Eq 'capnp/get-u8 ptr [0-9]+ 250' codegen-features.janet
grep -Eq 'capnp/get-u16 ptr [0-9]+ 65000' codegen-features.janet
grep -Eq 'capnp/get-u32 ptr [0-9]+ 4000000000' codegen-features.janet
grep -Eq 'capnp/get-u64 ptr [0-9]+ 9007199254740991' codegen-features.janet
grep -Eq 'capnp/get-f32 ptr [0-9]+ 1\.5' codegen-features.janet
grep -Eq 'capnp/get-f64 ptr [0-9]+ 2\.5' codegen-features.janet
grep -Eq 'capnp/get-u16 ptr [0-9]+ 1' codegen-features.janet
grep -q '(defn CodegenFeatures-which \[ptr\]' codegen-features.janet
grep -q '(def CodegenFeatures-none-tag 0)' codegen-features.janet
grep -q '(def CodegenFeatures-number-tag 1)' codegen-features.janet
grep -q '(def CodegenFeatures-detail-tag 2)' codegen-features.janet
grep -q '(defn CodegenFeatures-detail \[ptr\]' codegen-features.janet
grep -q '(defn CodegenFeatures-detail-get-label \[ptr\]' codegen-features.janet

echo "ok codegen"
