# A call whose params content pointer is out of bounds fails when the
# handler reads its params, as capnp-C++ getParams() does. A handler that
# never reads them answers.
(import capnp)

(def client (capnp/rpc-connect))
(def server (capnp/rpc-connect))

(defn on-call [iface method params results]
  (def body (capnp/alloc-struct results 1 0))
  (when (= method 0)
    (capnp/set-u64 body 0 (capnp/get-u64 params 0 0)))
  (when (= method 1)
    (capnp/set-u64 body 0 42)))

(capnp/rpc-serve server on-call)
(def boot (capnp/rpc-bootstrap client))
(each frame (capnp/rpc-flush client)
  (capnp/rpc-handle server frame))
(each frame (capnp/rpc-flush server)
  (capnp/rpc-handle client frame))
(def cap-id (capnp/rpc-answer-cap client boot))

(defn corrupt-call [method]
  (capnp/rpc-call client cap-id (int/u64 "1") method 1 0
                  (fn [params] (capnp/set-u64 params 0 7)))
  (def frame (buffer (first (capnp/rpc-flush client))))
  (var hit false)
  (for w 1 (/ (- (length frame) 8) 8)
    (def at (+ 8 (* w 8)))
    (when (and (not hit)
               (= 0 (band (frame at) 3))
               (= 1 (frame (+ at 4)))
               (= 0 (frame (+ at 5)) (frame (+ at 6)) (frame (+ at 7))))
      (set hit true)
      (put frame at 0)
      (put frame (+ at 1) 0)
      (put frame (+ at 2) 16)
      (put frame (+ at 3) 0)))
  (assert hit "found the params content pointer")
  (capnp/rpc-handle server frame)
  (def frames (capnp/rpc-flush server))
  (assert (= 1 (length frames)) "one Return")
  (capnp/getp (capnp/root (capnp/message-from-buffer (frames 0))) 0))

(def reads (corrupt-call 0))
(assert (= 1 (capnp/get-u16 reads 6 0)) "reading the params throws")
(def reason (capnp/get-text (capnp/getp reads 0) 0))
(assert (string/find "out-of-bounds struct pointer" reason) reason)

(def ignores (corrupt-call 1))
(assert (= 0 (capnp/get-u16 ignores 6 0)) "ignoring the params answers")
(print "ok janet poison params")
