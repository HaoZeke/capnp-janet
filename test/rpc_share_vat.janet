# The guest must keep the host vat alive after the host value is dropped.
(import capnp)

(defn make-guest []
  (var host (capnp/rpc-connect))
  (def guest (capnp/rpc-connect))
  (capnp/rpc-share-vat host guest)
  (set host nil)
  guest)

(def guest (make-guest))
(gccollect)
(var scratch @[])
(for i 0 2000
  (array/push scratch (string i)))
(set scratch nil)
(gccollect)
(def provisions (capnp/rpc-provisions guest))
(assert (indexed? provisions) "guest vat survived collection")
(assert (empty? provisions) "no provisions were recorded")
(print "ok share-vat")
