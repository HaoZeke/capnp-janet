#!/usr/bin/env bash
# Run the standalone C tests under valgrind. A zeroed capnp_message_t
# is part of the from_flat contract; an uninitialized ready field shows
# up here as a conditional on an undefined value.
set -euo pipefail
if ! command -v valgrind >/dev/null 2>&1; then
  echo "SKIP: no valgrind" >&2
  exit 77
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
# Valgrind and AddressSanitizer cannot instrument the same binary. A
# pipe into `grep -q` could end `ldd` with SIGPIPE, which pipefail
# reports as a miss.
deps=$(ldd "$BUILD/test_wire" 2>/dev/null || true)
if [[ "$deps" == *libasan* ]]; then
  echo "SKIP: address sanitizer build" >&2
  exit 77
fi
export CAPNP_JANET_SOURCE_ROOT="${CAPNP_JANET_SOURCE_ROOT:-$ROOT}"
bins=(
  test_wire
  test_robust
  test_orphan
  test_list_text
  test_addressbook
  test_calculator
  test_packed
  test_lists
  test_list_evolution
  test_multiseg
  test_canonical
  test_copy
  test_rpc_join
  test_rpc_client
  test_transport
  test_abi_layout
)
for b in "${bins[@]}"; do
  exe="$BUILD/$b"
  if [[ ! -x "$exe" ]]; then
    echo "SKIP: $b not built" >&2
    exit 77
  fi
done
for b in "${bins[@]}"; do
  exe="$BUILD/$b"
  echo "valgrind $b"
  valgrind --error-exitcode=42 --quiet --track-origins=yes --leak-check=no \
    "$exe"
done
echo "ok valgrind"
