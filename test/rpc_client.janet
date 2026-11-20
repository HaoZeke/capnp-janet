# Client vat against a capnp-C++ Adder on 127.0.0.1:<port>.
(import capnp)

(unless (get (dyn :args) 1)
  (os/exit 0))
(def port (scan-number ((dyn :args) 1)))
(def adder-id (int/u64 "0xea01e10cbc414411"))

(defn read-exact [stream n]
  (def buf @"")
  (while (< (length buf) n)
    (def chunk (:read stream (- n (length buf))))
    (if (or (nil? chunk) (empty? chunk))
      (error "short read"))
    (buffer/push buf chunk))
  buf)

(defn read-frame [stream]
  (def head (read-exact stream 4))
  (def nsegs (+ (head 0) (* (head 1) 256) (* (head 2) 65536) (* (head 3) 16777216) 1))
  (def raw-table (+ 4 (* nsegs 4)))
  (def table (if (zero? (% raw-table 8)) raw-table (+ raw-table 4)))
  (def sizes (read-exact stream (- table 4)))
  (var words 0)
  (for i 0 nsegs
    (def o (* i 4))
    (+= words (+ (sizes o)
                 (* (sizes (+ o 1)) 256)
                 (* (sizes (+ o 2)) 65536)
                 (* (sizes (+ o 3)) 16777216))))
  (def body (read-exact stream (* words 8)))
  (def frame @"")
  (buffer/push frame head)
  (buffer/push frame sizes)
  (buffer/push frame body)
  frame)

(var stream nil)
(var i 0)
(while (and (nil? stream) (< i 50))
  (set i (+ i 1))
  (set stream (try (net/connect "127.0.0.1" port) ([_] nil)))
  (when (nil? stream) (os/sleep 0.05)))
(if (nil? stream) (error "could not connect"))

(defn write-pending [conn]
  (each frame (capnp/rpc-flush conn)
    (:write stream frame)))

(def conn (capnp/rpc-connect))
(def boot (capnp/rpc-bootstrap conn))
(write-pending conn)
(capnp/rpc-handle conn (read-frame stream))
(assert (capnp/rpc-answered? conn boot) "bootstrap answered")
(assert (not (capnp/rpc-failed? conn boot)) "bootstrap ok")

(defn fill-add [params]
  (capnp/set-u64 params 0 20)
  (capnp/set-u64 params 8 22))

(def call (capnp/rpc-call conn 0 adder-id 0 2 0 fill-add))
(write-pending conn)
(capnp/rpc-handle conn (read-frame stream))
(assert (capnp/rpc-answered? conn call) "call answered")
(assert (not (capnp/rpc-failed? conn call)) "call ok")
(def result (capnp/rpc-answer conn call))
(assert (= 42 (capnp/get-u64 result 0)) "add(20, 22)")
(capnp/rpc-finish conn call)
(write-pending conn)
(print "ok janet rpc")
