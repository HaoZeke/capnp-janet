# Struct and list constants and 64-bit annotations from
# test/fixtures/consts.capnp read back as
# `capnp eval` prints them.
(import capnp)
(import ./codegen_util :as cg)

(def generated
  (cg/generate-from ["test/fixtures/consts.capnp" "schema/parity.capnp"]))
(import consts :as c)
(import parity)
(cg/done generated)

(defn check [what got want]
  (unless (deep= got want)
    (errorf "%s: got %q, want %q" what got want)))

(check "kPoint.x" (c/Point-get-x c/kPoint) 77)
(check "kPoint.y" (c/Point-get-y c/kPoint) -88)
(check "kPoint.label" (c/Point-get-label c/kPoint) "pt")
(check "kInts" (seq [i :range [0 (capnp/list-len c/kInts)]]
                 (capnp/list-get-i32 c/kInts i)) @[1 -2 3])
(check "kWords" (seq [i :range [0 (capnp/list-len c/kWords)]]
                  (capnp/list-get-text c/kWords i)) @["a" "bc"])
(check "kPoints[0].x" (c/Point-get-x (capnp/list-getp c/kPoints 0)) 1)
(check "kPoints[1].y" (c/Point-get-y (capnp/list-getp c/kPoints 1)) 2)
(check "kPoints[1].label" (c/Point-get-label (capnp/list-getp c/kPoints 1)) "q")
(check "kEmpty" (capnp/list-len c/kEmpty) 0)
(check "Point $bigS" (get-in c/Point-annotations [0 :value])
       (int/s64 "-9007199254740993"))
(check "Point $bigU" (get-in c/Point-annotations [1 :value])
       (int/u64 "18446744073709551615"))
(print "ok generated consts")
