# A builder handed to an RPC callback is a stack value. Stashing it,
# or a struct allocated from it, and using it after the callback
# returns must be an error. A heap builder stays usable.
(import capnp)

(defn died? [thunk]
  (def err
    (try
      (thunk)
      ([e] (string e))))
  (and (string? err) (string/find "no longer valid" err)))

(def client (capnp/rpc-connect))
(def server (capnp/rpc-connect))
(var saved-results nil)
(var saved-child nil)
(var saved-params nil)
(var saved-param-child nil)

(defn on-call [iface method params results]
  (set saved-results results)
  (set saved-child (capnp/alloc-struct results 1 0))
  (capnp/set-u64 saved-child 0 7))

(capnp/rpc-serve server on-call)
(def boot (capnp/rpc-bootstrap client))
(each frame (capnp/rpc-flush client)
  (capnp/rpc-handle server frame))
(each frame (capnp/rpc-flush server)
  (capnp/rpc-handle client frame))
(def cap-id (capnp/rpc-answer-cap client boot))

(defn fill [params]
  (set saved-params params)
  (set saved-param-child (capnp/init-struct params 0 1 0))
  (capnp/set-u64 params 0 42)
  (capnp/set-u64 saved-param-child 0 3))

(def call (capnp/rpc-call client cap-id (int/u64 "1") 0 2 1 fill))
(each frame (capnp/rpc-flush client)
  (capnp/rpc-handle server frame))
(each frame (capnp/rpc-flush server)
  (capnp/rpc-handle client frame))
(assert (capnp/rpc-answered? client call) "callback wrote the params")
(assert (died? (fn [] (capnp/set-u64 saved-results 0 1)))
        "results after return")
(assert (died? (fn [] (capnp/set-u64 saved-child 0 1)))
        "alloc-struct after return")
(assert (died? (fn [] (capnp/set-u64 saved-params 0 1)))
        "params after return")
(assert (died? (fn [] (capnp/set-u64 saved-param-child 0 1)))
        "init-struct after return")

(var boom-params nil)
(def boom
  (try
    (capnp/rpc-call client cap-id (int/u64 "1") 0 1 0
                    (fn [params]
                      (set boom-params params)
                      (error "boom")))
    ([e] (string e))))
(assert (string/find "boom" boom) "panic still propagates")
(def trace-fiber
  (fiber/new
    (fn []
      (capnp/rpc-call client cap-id (int/u64 "1") 0 1 0
                      (fn fill-boom [params] (error "boom")))) :e))
(resume trace-fiber)
(def trace @"")
(with-dyns [:err trace :err-color nil]
  (debug/stacktrace trace-fiber "boom"))
(assert (string/find "fill-boom" trace) trace)
(assert (empty? (capnp/rpc-flush client))
        "fill error leaves the call unsent")
(assert (died? (fn [] (capnp/set-u64 boom-params 0 1)))
        "params after a panic")

(def b (capnp/new-builder))
(def root (capnp/init-root b 0 2))
(capnp/set-text root 0 "ABCD1234")
(capnp/clear-pointer root 0)
(def child (capnp/init-struct root 1 1 1))
(capnp/set-u32 child 0 10)
(def msg (capnp/message-from-buffer (capnp/finish-builder b)))
(def got (capnp/getp (capnp/root msg) 1))
(assert (= 10 (capnp/get-u32 got 0 0)) "heap builder stays valid")
(print "ok builder scope")
