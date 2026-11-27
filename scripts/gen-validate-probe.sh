#!/usr/bin/env bash
# Regenerate test/fixtures/schema-parity/validate-probe.bin.
#
# Same shape as scripts/gen-rpc-frames.sh: capnp on PATH, optional
# --check. Compiles validate-probe.capnp with capnp 1.4.0, then
# scripts/gen-validate-probe.cc applies the mutations listed in that
# schema and writes the probe.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
schema=$here/test/fixtures/schema-parity/validate-probe.capnp
fix=$here/test/fixtures/schema-parity/validate-probe.bin
src=$here/scripts/gen-validate-probe.cc
cxx=${CXX:-g++}

command -v capnp >/dev/null || {
  echo "gen-validate-probe: no capnp CLI on PATH" >&2
  exit 1
}
ver=$(capnp --version)
case $ver in
  *'1.4.0'*) ;;
  *)
    echo "gen-validate-probe: need capnp 1.4.0, have $ver" >&2
    exit 1
    ;;
esac
command -v "$cxx" >/dev/null || {
  echo "gen-validate-probe: no C++ compiler ($cxx)" >&2
  exit 1
}
pkg-config --exists capnp || {
  echo "gen-validate-probe: no capnp pkg-config" >&2
  exit 1
}

check=0
[[ "${1:-}" == "--check" ]] && check=1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

capnp compile -o- "$schema" >"$work/request.bin"
# shellcheck disable=SC2046
"$cxx" -std=c++17 -O2 -o "$work/gen" "$src" $(pkg-config --cflags --libs capnp)
"$work/gen" "$work/request.bin" >"$work/validate-probe.bin"

if [[ $check -eq 1 ]]; then
  if cmp -s "$work/validate-probe.bin" "$fix"; then
    echo "ok   validate-probe.bin matches $ver"
  else
    echo "FAIL validate-probe.bin differs from $ver" >&2
    exit 1
  fi
  exit 0
fi

cp "$work/validate-probe.bin" "$fix"
echo "wrote $fix with $ver"
