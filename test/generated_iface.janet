# Inherited methods from test/fixtures/iface.capnp, which capnpc-c++
# exposes on `Derived::Client`, each with the id of its own interface.
(import ./codegen_util :as cg)

(def generated (cg/generate-from ["test/fixtures/iface.capnp"]))
(import iface :as i)
(cg/done generated)

(defn check [what got want]
  (unless (deep= got want)
    (errorf "%s: got %q, want %q" what got want)))

(check "superclasses" i/Derived-superclasses
       [i/Mid-interface-id i/Other-interface-id])
(check "method names" (sort (keys i/Derived-methods))
       @[:mid :other :ping :pong])
(check "own ping" (get-in i/Derived-methods [:ping :interface-id])
       i/Derived-interface-id)
(check "own ping ordinal" (get-in i/Derived-methods [:ping :ordinal]) 1)
(check "mid id" (get-in i/Derived-methods [:mid :interface-id])
       i/Mid-interface-id)
(check "mid shape" (get-in i/Derived-methods [:mid :params-dwords]) 1)
(check "other id" (get-in i/Derived-methods [:other :interface-id])
       i/Other-interface-id)
(check "Mid methods" (sort (keys i/Mid-methods)) @[:mid :ping])
(check "Mid ping id" (get-in i/Mid-methods [:ping :interface-id])
       i/Base-interface-id)
(print "ok generated iface")
