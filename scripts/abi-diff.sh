#!/usr/bin/env bash
# Build libcapnp_janet.so at 40876e91 and at this tree, then compare
# them with abidiff. Functions listed in abi-additions.suppr may be added.
# Exit 77 when the tool or the base commit is absent.
set -euo pipefail

BASE=40876e91a615b8bb9cbec6a91228a5af96f1ba11
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v abidiff >/dev/null 2>&1; then
  echo "SKIP: no abidiff" >&2
  exit 77
fi
if ! command -v meson >/dev/null 2>&1 || ! command -v git >/dev/null 2>&1; then
  echo "SKIP: need meson and git" >&2
  exit 77
fi
if ! git -C "$ROOT" cat-file -e "${BASE}^{commit}" 2>/dev/null; then
  echo "SKIP: base commit not in this checkout" >&2
  exit 77
fi

WORK=$(mktemp -d)
cleanup() {
  git -C "$ROOT" worktree remove --force "$WORK/src" >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

git -C "$ROOT" worktree add --detach "$WORK/src" "$BASE" >/dev/null

build_lib() {
  local src="$1"
  local dir="$2"
  if ! meson setup "$dir" "$src" --buildtype=debug --default-library=shared \
      >"$dir.log" 2>&1; then
    cat "$dir.log" >&2
    exit 1
  fi
  if ! meson compile -C "$dir" capnp_janet >"$dir.build" 2>&1; then
    cat "$dir.build" >&2
    exit 1
  fi
}

build_lib "$WORK/src" "$WORK/old"
build_lib "$ROOT" "$WORK/new"

oldso=$(find "$WORK/old" -name 'libcapnp_janet.so' -print -quit)
newso=$(find "$WORK/new" -name 'libcapnp_janet.so' -print -quit)
if [[ -z "${oldso}" || -z "${newso}" ]]; then
  echo "shared library was not produced" >&2
  exit 1
fi

set +e
abidiff --fail-no-debug-info --suppressions "$ROOT/scripts/abi-additions.suppr" \
  "$oldso" "$newso"
rc=$?
set -e
if [[ "$rc" -ne 0 ]]; then
  echo "abidiff exit ${rc}" >&2
  exit 1
fi
echo "abidiff: no ABI change"
