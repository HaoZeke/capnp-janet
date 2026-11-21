# Stream three add calls at the capnp-C++ Adder, then echo 70000 bytes.
(import capnp)

(unless (get (dyn :args) 1)
  (os/exit 0))
(def port (scan-number ((dyn :args) 1)))
(def adder (int/u64 "0xea01e10cbc414411"))
(def conn (capnp/rpc-dial-tcp "127.0.0.1" port))
(def boot (capnp/rpc-bootstrap conn))
(capnp/rpc-recv conn)
(def cap-id (capnp/rpc-answer-cap conn boot))
(def stream (capnp/rpc-stream 4))

(defn fill [params]
  (capnp/set-u64 params 0 20)
  (capnp/set-u64 params 8 22))

(def qs @[])
(for i 0 3
  (array/push qs (capnp/rpc-stream-send conn stream cap-id adder 0 2 0 fill)))
(for i 0 3
  (capnp/rpc-recv conn))
(each q qs
  (def result (capnp/rpc-answer conn q))
  (assert (= 42 (capnp/get-u64 result 0 0)) "stream add"))
(capnp/rpc-stream-finish conn stream)
# A Return over 64 KiB, as C++ sends any size.
(def echo (capnp/rpc-call conn cap-id adder 1 1 0
                          (fn [params] (capnp/set-u32 params 0 70000))))
(capnp/rpc-recv conn)
(def echoed (capnp/get-data (capnp/rpc-answer conn echo) 0))
(assert (= 70000 (length echoed)) "echo over 64 KiB")
(print "ok janet stream")
