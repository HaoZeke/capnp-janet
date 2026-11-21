#!/usr/bin/env bash
# Random Parity messages from a fixed seed, encoded and converted by
# `capnp` and capnp-eval, with every output byte-compared.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${CAPNP_JANET_BUILD:-$ROOT/build}"
if [[ ! -x "$BUILD/capnp-eval" ]]; then
  echo "SKIP: capnp-eval not built" >&2
  exit 77
fi
if ! command -v capnp >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: needs capnp and python3" >&2
  exit 77
fi
python3 "$ROOT/test/random_parity.py" "$BUILD" "${RANDOM_PARITY_CASES:-60}" \
  "${RANDOM_PARITY_SEED:-20}"
