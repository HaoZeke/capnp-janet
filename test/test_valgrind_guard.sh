#!/usr/bin/env bash
# test_valgrind.sh must skip a sanitizer build even when `ldd` prints
# more after the libasan line than a pipe holds.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v valgrind >/dev/null 2>&1; then
  echo "SKIP: no valgrind" >&2
  exit 77
fi
FAKE=$(mktemp -d)
trap 'rm -rf "$FAKE"' EXIT
cat >"$FAKE/ldd" <<'EOF'
#!/bin/sh
echo "libasan.so.8 => /usr/lib/libasan.so.8"
yes "libc.so.6 => /usr/lib/libc.so.6" | head -c 1000000
EOF
chmod +x "$FAKE/ldd"
rc=0
PATH="$FAKE:$PATH" bash "$ROOT/test/test_valgrind.sh" >/dev/null 2>&1 || rc=$?
if [[ $rc -ne 77 ]]; then
  echo "FAIL: test_valgrind.sh exited $rc on a sanitizer build, want 77" >&2
  exit 1
fi
echo "ok valgrind guard"
