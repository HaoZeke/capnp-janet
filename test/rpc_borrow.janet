# The handler keeps the params pointer. The call message is freed when
# dispatch returns, so the pointer must own a copy.
(import capnp)

(def client (capnp/rpc-connect))
(def server (capnp/rpc-connect))
(var saved nil)

(defn on-call [iface method params results]
  (set saved params)
  (def body (capnp/alloc-struct results 1 0))
  (capnp/set-u64 body 0 7))

(capnp/rpc-serve server on-call)
(def boot (capnp/rpc-bootstrap client))
(each frame (capnp/rpc-flush client)
  (capnp/rpc-handle server frame))
(each frame (capnp/rpc-flush server)
  (capnp/rpc-handle client frame))
(assert (capnp/rpc-answered? client boot) "bootstrap")
(def cap-id (capnp/rpc-answer-cap client boot))

(defn fill [params]
  (capnp/set-u64 params 0 42)
  (capnp/set-u64 params 8 1))

(def call (capnp/rpc-call client cap-id (int/u64 "1") 0 2 0 fill))
(each frame (capnp/rpc-flush client)
  (capnp/rpc-handle server frame))
(assert (= 42 (capnp/get-u64 saved 0 0)) "params outlive dispatch")
(print "ok janet borrow")
