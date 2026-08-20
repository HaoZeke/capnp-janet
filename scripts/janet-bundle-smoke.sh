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

# Scalar schema defaults are XOR bases on the wire. A zeroed data word must
# therefore read as the requested logical default for every exposed width.
janet -e '
(import capnp)
(def root (:root (capnp/message-from-buffer
                   (capnp/build-message 1 0 @[]))))
(assert (= 255 (capnp/get-u8 root 0 255)) "u8 schema default")
(assert (= -7 (capnp/get-i8 root 0 -7)) "i8 schema default")
(assert (= -700 (capnp/get-i16 root 0 -700)) "i16 schema default")
(assert (= -70000 (capnp/get-i32 root 0 -70000)) "i32 schema default")
(assert (= -700000 (capnp/get-i64 root 0 -700000)) "i64 schema default")
(assert (= 2.5 (capnp/get-f32 root 0 2.5)) "f32 schema default")
(assert (= 2.5 (capnp/get-f64 root 0 2.5)) "f64 schema default")
(assert (capnp/get-bool root 0 true) "bool schema default")
(print "scalar schema defaults ok")'

# The arena-backed builder surface is what generated Janet modules target.
# Values are XORed with schema defaults before storage, exactly as the wire
# format requires, while pointer children share one growable message arena.
janet -e '
(import capnp)
(def builder (capnp/new-builder))
(def root (capnp/init-root builder 7 3))
(capnp/set-u8 root 0 9 11)
(capnp/set-i8 root 1 -5 -7)
(capnp/set-u16 root 2 2000 1000)
(capnp/set-i16 root 4 -2000 -1000)
(capnp/set-u32 root 8 70000 30000)
(capnp/set-i32 root 12 -70000 -30000)
(capnp/set-u64 root 16 700000 300000)
(capnp/set-i64 root 24 -700000 -300000)
(capnp/set-f32 root 32 1.5 2.5)
(capnp/set-f64 root 40 -1.5 -2.5)
(capnp/set-bool root 384 false true)
(capnp/set-text root 0 "arena")
(capnp/set-data root 1 "\x00\x01\xfe\xff")
(capnp/set-text root 2 "stale")
(capnp/clear-pointer root 2)
(def decoded (:root (capnp/message-from-buffer (capnp/finish-builder builder))))
(assert (= 9 (:u8 decoded 0 11)) "builder u8 default round-trip")
(assert (= -5 (:i8 decoded 1 -7)) "builder i8 default round-trip")
(assert (= 2000 (:u16 decoded 2 1000)) "builder u16 default round-trip")
(assert (= -2000 (:i16 decoded 4 -1000)) "builder i16 default round-trip")
(assert (= 70000 (:u32 decoded 8 30000)) "builder u32 default round-trip")
(assert (= -70000 (:i32 decoded 12 -30000)) "builder i32 default round-trip")
(assert (= 700000 (:u64 decoded 16 300000)) "builder u64 default round-trip")
(assert (= -700000 (:i64 decoded 24 -300000)) "builder i64 default round-trip")
(assert (= 1.5 (:f32 decoded 32 2.5)) "builder f32 default round-trip")
(assert (= -1.5 (:f64 decoded 40 -2.5)) "builder f64 default round-trip")
(assert (not (:bool decoded 384 true)) "builder bool default round-trip")
(assert (= "arena" (string (:text decoded 0))) "builder text round-trip")
(assert (= "\x00\x01\xfe\xff" (string (capnp/get-data decoded 1)))
        "builder data round-trip")
(assert (= "" (string (:text decoded 2))) "builder pointer clear")
(print "arena scalar and pointer builders ok")'

# Janet's integer abstracts carry values beyond the exact-double range, so
# the full Cap'n Proto 64-bit domain survives both defaults and values.
janet -e '
(import capnp)
(def umax (int/u64 "18446744073709551615"))
(def unext (int/u64 "18446744073709551614"))
(def smin (int/s64 "-9223372036854775808"))
(def snext (int/s64 "-9223372036854775807"))
(def builder (capnp/new-builder))
(def root (capnp/init-root builder 2 0))
(capnp/set-u64 root 0 unext umax)
(capnp/set-i64 root 8 snext smin)
(def decoded (:root (capnp/message-from-buffer (capnp/finish-builder builder))))
(assert (= unext (capnp/get-u64 decoded 0 umax)) "exact UInt64 round-trip")
(assert (= snext (capnp/get-i64 decoded 8 smin)) "exact Int64 round-trip")
(print "exact 64-bit integers ok")'

janet -e '
(import capnp)
(def builder (capnp/new-builder))
(def root (capnp/init-root builder 0 2))
(def child (capnp/init-struct root 0 1 1))
(capnp/set-u32 child 0 77)
(capnp/set-text child 0 "nested")
(def people (capnp/init-struct-list root 1 2 1 1))
(def alice (capnp/struct-list-at people 0))
(capnp/set-u32 alice 0 123)
(capnp/set-text alice 0 "Alice")
(def bob (capnp/struct-list-at people 1))
(capnp/set-u32 bob 0 456)
(capnp/set-text bob 0 "Bob")
(def decoded (:root (capnp/message-from-buffer (capnp/finish-builder builder))))
(def decoded-child (:ptr decoded 0))
(assert (= 77 (:u32 decoded-child 0)) "nested struct scalar")
(assert (= "nested" (string (:text decoded-child 0))) "nested struct text")
(def decoded-people (:ptr decoded 1))
(assert (= 2 (length decoded-people)) "struct-list length")
(assert (deep= @[123 456] (map |(:u32 $ 0) decoded-people)) "struct-list values")
(assert (deep= @["Alice" "Bob"] (map |(string (:text $ 0)) decoded-people))
        "struct-list pointers")
(print "nested builder views ok")'

echo "ok janet-bundle-smoke"
