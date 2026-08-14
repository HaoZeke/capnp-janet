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

# The abstract types carry get/next/length/tostring, so a message and a
# pointer answer to the same forms as a built-in structure. Without those
# slots every one of these raises, and callers fall back to the
# capnp/list-* functions.
janet -e '
(import capnp)
(def msg (capnp/message-from-buffer
           (slurp "test/fixtures/addressbook_alice_bob.bin")))
(def root (:root msg))
(assert (= :struct (capnp/kind root)) "method call on a message")
(def people (capnp/getp root 0))
(assert (= 2 (length people)) "length protocol")
(assert (= "Alice" (string (:text (in people 0) 0))) "index protocol")
(assert (= "Bob" (string (:text (people 1) 0))) "call-as-index protocol")
(assert (nil? (get people 99)) "out-of-range reads nil")
(assert (deep= @["Alice" "Bob"] (map |(string (:text $ 0)) people))
        "map over a list pointer")
(assert (deep= @[123 456] (seq [p :in people] (:u32 p 0)))
        "seq over a list pointer")
(var n 0)
(each p people (set n (+ n 1)) (assert (= :struct (capnp/kind p)) "each element"))
(assert (= 2 n) "each visited every element")
(assert (string/find "list 2" (string people)) "tostring shows the length")
(print "sequence and method protocols ok")'

# build-message accepts any indexed value, so a tuple literal works and
# callers are not forced into @[...] for a constant field list.
janet -e '
(import capnp)
(def buf (capnp/build-message 1 1 [[:u32 0 7] [:text 0 "tuple"]]))
(def root (:root (capnp/message-from-buffer buf)))
(assert (= 7 (:u32 root 0)) "tuple-literal build-message u32")
(assert (= "tuple" (string (:text root 0))) "tuple-literal build-message text")
(print "tuple-literal build-message ok")'

echo "ok janet-bundle-smoke"
