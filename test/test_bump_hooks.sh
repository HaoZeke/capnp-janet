#!/usr/bin/env bash
# The cog pre-bump hooks move every version the release commit carries,
# so the version-lockstep hook in prek.toml passes after `cog bump`.
# The towncrier steps need towncrier and touch no version string.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if ! python3 -c 'import tomllib' 2>/dev/null; then
  echo "SKIP: needs python3 with tomllib" >&2
  exit 77
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for f in VERSION.txt towncrier.toml docs/source/conf.py README.md \
  docs/orgmode/index.org; do
  mkdir -p "$TMP/$(dirname "$f")"
  cp "$ROOT/$f" "$TMP/$f"
done
V=9.8.7
python3 - "$ROOT/cog.toml" "$V" <<'EOF' >"$TMP/hooks.sh"
import sys, tomllib
for hook in tomllib.load(open(sys.argv[1], "rb"))["pre_bump_hooks"]:
    if hook.startswith(("towncrier ", "python3 scripts/release_notes.py")):
        continue
    print(hook.replace("{{version}}", sys.argv[2]))
EOF
(cd "$TMP" && bash -e hooks.sh)
fail=0
want() {
  if [[ "$2" != "$V" ]]; then
    echo "FAIL: $1 is '$2' after the bump hooks, want $V" >&2
    fail=1
  fi
}
want VERSION.txt "$(tr -d '[:space:]' <"$TMP/VERSION.txt")"
want towncrier.toml "$(sed -n 's/^version = "\(.*\)"/\1/p' "$TMP/towncrier.toml")"
want "docs/source/conf.py release" \
  "$(sed -n 's/^release[[:space:]]*=[[:space:]]*"\(.*\)"/\1/p' "$TMP/docs/source/conf.py" | head -1)"
want "README.md status" \
  "$(sed -n 's/^\*\*Status:\*\* `\([^`]*\)`.*/\1/p' "$TMP/README.md")"
want "docs/orgmode/index.org status" \
  "$(sed -n 's/.*Status: =\([^=]*\)=\..*/\1/p' "$TMP/docs/orgmode/index.org")"
exit "$fail"
