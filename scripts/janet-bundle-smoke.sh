#!/usr/bin/env bash
# Install the module the way janet-lang/pkgs consumers do -- through
# bundle/init.janet and project.janet -- and exercise the documented API.
#
# The meson build compiles the C runtime but leaves the CFUN registration
# untested, so a wrong binding prefix reaches users with every C test green.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v janet >/dev/null 2>&1; then
  echo "janet-bundle-smoke: no janet on PATH" >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export JANET_PATH="$WORK/tree"
mkdir -p "$JANET_PATH"

# bundle/init.janet pulls declare-cc out of spork.
git clone --depth 1 https://github.com/janet-lang/spork.git "$WORK/spork" >/dev/null 2>&1
janet -e "(bundle/install \"$WORK/spork\")" >/dev/null

cd "$ROOT"
# declare-cc caches objects in _build/; a stale tree relinks the previous
# capnp.so and the smoke then reports on code that is no longer checked out.
rm -rf _build
janet -e '(bundle/install ".")' >/dev/null
echo "installed: $(janet -e '(prin (string/join (sort (bundle/list)) " "))')"

# Every binding must land at capnp/<name>. Janet's import applies the module
# prefix, so a module entry that registers pre-qualified names yields
# capnp/capnp/<name> and no documented call site resolves.
janet -e '
(import capnp)
(def names (sort (map string (filter |(string/has-prefix? "capnp/" (string $)) (keys (curenv))))))
(when (empty? names) (error "no capnp/ bindings after (import capnp)"))
(each n names
  (when (string/has-prefix? "capnp/capnp/" n)
    (errorf "double-prefixed binding %s: module entry must bind bare names" n)))
(printf "%d bindings, all singly prefixed" (length names))'

# The README/tutorial snippet, verbatim in behaviour.
janet -e '
(import capnp)
(def buf (capnp/build-message 1 1 @[[:u32 0 42] [:text 0 "hello"]]))
(def root (capnp/root (capnp/message-from-buffer buf)))
(assert (= 42 (capnp/get-u32 root 0)) "build-message u32 round-trip")
(assert (= "hello" (string (capnp/get-text root 0))) "build-message text round-trip")
(print "build-message round-trip ok")'

# Decode the golden that upstream `capnp encode` produced.
janet -e '
(import capnp)
(def root (capnp/root (capnp/message-from-buffer
                        (slurp "test/fixtures/addressbook_alice_bob.bin"))))
(def people (capnp/getp root 0))
(assert (= 2 (capnp/list-len people)) "golden people count")
(def alice (capnp/list-getp people 0))
(assert (= 123 (capnp/get-u32 alice 0)) "golden Alice id")
(assert (= "Alice" (string (capnp/get-text alice 0))) "golden Alice name")
(print "capnp encode golden decodes ok")'

echo "ok janet-bundle-smoke"
