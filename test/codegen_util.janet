# Shared by the generated-code tests: find or build `capnpc-janet`,
# compile schemas into a fresh directory and put that directory on the
# module path. `jpm test` builds no plugin, so this compiles one with the
# C compiler. Only a missing `capnp` CLI makes a caller skip.

(defn- dirname [p]
  (def marks (string/find-all "/" p))
  (if (empty? marks) "." (string/slice p 0 (last marks))))

(defn- basename [p]
  (def marks (string/find-all "/" p))
  (if (empty? marks) p (string/slice p (+ 1 (last marks)))))

(defn- abs-path [p]
  (if (string/has-prefix? "/" p) p (string (os/cwd) "/" p)))

(def root (abs-path (string (dirname (dyn :current-file)) "/..")))

(defn- on-path [name]
  (var hit nil)
  (each d (string/split ":" (or (os/getenv "PATH") ""))
    (def c (string d "/" name))
    (when (and (not hit) (not (empty? d)) (= :file (os/stat c :mode)))
      (set hit c)))
  hit)

(defn- run [args &opt env]
  (def null (file/open "/dev/null" :w))
  (def opts @{:out null})
  (when env (put opts :env env))
  (def rc (try (os/execute args :p opts) ([_] 1)))
  (file/close null)
  rc)

(defn- fresh-dir []
  (def dir (string (or (os/getenv "TMPDIR") "/tmp") "/capnp-janet-gen-"
                   (string/join (map |(string/format "%02x" $)
                                     (os/cryptorand 8)))))
  (os/mkdir dir)
  dir)

(def- lib-sources
  ["src/capnp_message.c" "src/capnp_builder.c" "src/capnp_packed.c"
   "src/capnp_canonical.c" "src/capnp_rpc.c" "src/capnp_transport.c"
   "src/capnp_dyn.c"])

(defn- built-plugin []
  (var hit nil)
  (each b [(os/getenv "CAPNP_JANET_BUILD") "build" "build-janet"]
    (when (and b (not hit))
      (def c (string (if (string/has-prefix? "/" b) b (string root "/" b))
                     "/capnpc-janet"))
      (when (= :file (os/stat c :mode))
        (set hit c))))
  hit)

(defn- compile-plugin [dir]
  (def out (string dir "/capnpc-janet"))
  (def rc (run [(or (os/getenv "CC") "cc") "-std=c11" "-pthread"
                (string "-I" root "/include") "-o" out
                (string root "/app/capnpc_janet.c")
                ;(map |(string root "/" $) lib-sources)]))
  (unless (= rc 0)
    (error "could not compile capnpc-janet"))
  out)

(defn generate-from
  "Compile @schemas (paths under the repository root) with capnpc-janet
  into a new directory, put it first on the module path and return it."
  [schemas]
  (def capnp (on-path "capnp"))
  (unless capnp
    (print "skip: " (dyn :current-file) " needs the capnp CLI")
    (os/exit 0))
  (def dir (fresh-dir))
  (def plugin (or (built-plugin) (compile-plugin dir)))
  (def inc (string (dirname (dirname capnp)) "/include"))
  (each s schemas
    (spit (string dir "/" (basename s)) (slurp (string root "/" s))))
  (def env (os/environ))
  (def old-ld (or (get env "LD_LIBRARY_PATH") ""))
  (put env "LD_LIBRARY_PATH"
       (if (empty? old-ld) (dirname plugin)
         (string (dirname plugin) ":" old-ld)))
  (def back (os/cwd))
  (os/cd dir)
  (def rc (run ["capnp" "compile" (string "-I" inc) (string "-o" plugin)
                ;(map basename schemas)] env))
  (os/cd back)
  (unless (= rc 0)
    (error (string "capnp compile failed for " (string/join schemas " "))))
  (array/insert module/paths 0
                [(string dir "/:all:.janet") :source (fn [p] p)])
  dir)

(defn done
  "Remove a directory made by `generate-from` once its modules are loaded."
  [dir]
  (each f (os/dir dir)
    (os/rm (string dir "/" f)))
  (os/rmdir dir))
