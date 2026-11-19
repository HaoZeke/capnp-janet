# Listen on TCP and implement Adder for a capnp-C++ client.
(import capnp)

(def [listener port] (capnp/rpc-listen-tcp "127.0.0.1" 0))
(print port)
(file/flush stdout)
(def conn (capnp/rpc-accept listener))

(defn on-call [iface method params results]
  (def a (capnp/get-u64 params 0 0))
  (def b (capnp/get-u64 params 8 0))
  (capnp/set-u64 results 0 (+ a b)))

(capnp/rpc-serve conn on-call)
(for i 0 8
  (try
    (capnp/rpc-recv conn)
    ([_] (break))))
(print "ok janet host")
