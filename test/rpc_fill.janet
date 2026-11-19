# fill must be a function or a cfunction. Keywords and structs used to
# reach janet_unwrap_function and abort the process.
(import capnp)

(defn must-error [label thunk]
  (def err
    (try
      (thunk)
      ([e] (string e))))
  (assert (string? err) (string "expected an error for " label))
  (assert (string/find "fill must be a function" err) err))

(def c (capnp/rpc-connect))
(must-error ":foo" (fn [] (capnp/rpc-call c 0 1 0 1 0 :foo)))
(must-error "{}" (fn [] (capnp/rpc-call c 0 1 0 1 0 {})))
(assert (empty? (capnp/rpc-flush c)) "bad fill sends nothing")

(def printed @"")
(def qprint
  (with-dyns [:out printed]
    (capnp/rpc-call c 0 1 0 1 0 print)))
(assert (number? qprint) "print is a cfunction fill")
(assert (pos? (length printed)) "print ran")

(def qtype (capnp/rpc-call c 0 1 0 1 0 type))
(assert (number? qtype) "type is a cfunction fill")

(defn fill [params]
  (capnp/set-u64 params 0 7))
(def qfun (capnp/rpc-call c 0 1 0 1 0 fill))
(assert (number? qfun) "function fill")
(assert (pos? (length (capnp/rpc-flush c))) "function fill sent a frame")

(def s (capnp/rpc-stream 4))
(must-error "stream :foo"
            (fn [] (capnp/rpc-stream-send c s 0 1 0 1 0 :foo)))
(must-error "stream {}"
            (fn [] (capnp/rpc-stream-send c s 0 1 0 1 0 {})))
(assert (number? (capnp/rpc-stream-send c s 0 1 0 1 0 print))
        "stream print")
(assert (number? (capnp/rpc-stream-send c s 0 1 0 1 0 type))
        "stream cfunction")
(assert (number? (capnp/rpc-stream-send c s 0 1 0 1 0 fill))
        "stream function")

(print "ok fill")
