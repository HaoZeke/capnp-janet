# A handler picks the exception its call fails with, as a capnp-C++
# server does by throwing.
(import capnp)

(def client (capnp/rpc-connect))
(def server (capnp/rpc-connect))

(defn on-call [iface method params results]
  (if (= method 9)
    (do
      (assert (capnp/rpc-fail 3 "Method not implemented.") "fail in handler")
      1)
    (capnp/set-u64 (capnp/alloc-struct results 1 0) 0 7)))

(assert (not (capnp/rpc-fail 0 "outside")) "fail outside a handler")
(capnp/rpc-serve server on-call)
(def boot (capnp/rpc-bootstrap client))
(each frame (capnp/rpc-flush client)
  (capnp/rpc-handle server frame))
(each frame (capnp/rpc-flush server)
  (capnp/rpc-handle client frame))
(def cap-id (capnp/rpc-answer-cap client boot))

(def call (capnp/rpc-call client cap-id (int/u64 "1") 9 0 0 (fn [params] nil)))
(each frame (capnp/rpc-flush client)
  (capnp/rpc-handle server frame))
(def frames (capnp/rpc-flush server))
(assert (= 1 (length frames)) "one Return")
(def root (capnp/root (capnp/message-from-buffer (frames 0))))
(def ret (capnp/getp root 0))
(assert (= 1 (capnp/get-u16 ret 6 0)) "an exception")
(def exc (capnp/getp ret 0))
(assert (= "Method not implemented." (capnp/get-text exc 0)) "its reason")
(assert (= 3 (capnp/get-u16 exc 4 0)) "its type")
(each frame frames
  (capnp/rpc-handle client frame))
(assert (capnp/rpc-failed? client call) "the call failed")
(print "ok janet rpc-fail")
