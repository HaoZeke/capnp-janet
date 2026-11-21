(import capnp)

# Compile schema/codegen-features.capnp when the plugin and capnp are
# available, then import that module. A failed import is a failure.
# jpm walks this file with neither tool present, and that walk exits 0.
(defn dirname [p]
  (def marks (string/find-all "/" p))
  (if (empty? marks) "." (string/slice p 0 (last marks))))

(defn abs-path [p]
  (if (string/has-prefix? "/" p) p (string (os/cwd) "/" p)))

(defn find-plugin []
  (def cands @[])
  (when-let [build (os/getenv "CAPNP_JANET_BUILD")]
    (array/push cands (string build "/capnpc-janet")))
  (array/push cands "build/capnpc-janet" "build-janet/capnpc-janet")
  (var hit nil)
  (each c cands
    (when (and (not hit) (os/stat c))
      (set hit (abs-path c))))
  hit)

(defn run-quiet [args &opt env]
  (default env nil)
  (def null (file/open "/dev/null" :w))
  (def opts @{:out null :err null})
  (when env (put opts :env env))
  (try
    (os/execute args :p opts)
    ([_] 1)))

(defn has-capnp []
  (= 0 (run-quiet ["capnp" "--version"])))

(defn module-dir []
  (def roots @[])
  (when-let [jp (os/getenv "JANET_PATH")]
    (each part (string/split ":" jp)
      (when (not (empty? part))
        (array/push roots part))))
  (each r @["build" "build-janet"]
    (array/push roots r))
  (var hit nil)
  (each r roots
    (when (and (not hit) (os/stat (string r "/codegen-features.janet")))
      (set hit r)))
  hit)

(defn load-generated [dir]
  (array/insert module/paths 0
                 [(string dir "/:all:.janet") :source (fn [p] p)])
  (def err
    (try
      (do (import codegen-features) nil)
      ([e] e)))
  (when err
    (eprint err)
    (os/exit 1)))

(def plugin (find-plugin))
(cond
  (and plugin (has-capnp))
  (do
    (def here (or (dyn :current-file) "test/generated_builder_smoke.janet"))
    (def schema (string (dirname here) "/../schema/codegen-features.capnp"))
    (def dest (string "/tmp/capnp-janet-smoke-"
                      (string/join (map |(string/format "%02x" $)
                                        (os/cryptorand 8)))))
    (os/mkdir dest)
    (spit (string dest "/codegen-features.capnp") (slurp schema))
    (def env (os/environ))
    (def plugin-dir (dirname plugin))
    (def old-ld (or (get env "LD_LIBRARY_PATH") ""))
    (put env "LD_LIBRARY_PATH"
         (if (empty? old-ld) plugin-dir (string plugin-dir ":" old-ld)))
    (def back (os/cwd))
    (os/cd dest)
    (def rc (run-quiet ["capnp" "compile" (string "-o" plugin)
                        "codegen-features.capnp"]
                       env))
    (os/cd back)
    (when (or (not= rc 0)
              (not (os/stat (string dest "/codegen-features.janet"))))
      (eprint "codegen-features: capnp compile failed")
      (os/exit 1))
    (load-generated dest))
  (module-dir)
  (load-generated (module-dir))
  (os/exit 0))

(def builder (capnp/new-builder))
(def root (codegen-features/CodegenFeatures-init-root builder))

(codegen-features/CodegenFeatures-set-enabled root false)
(codegen-features/CodegenFeatures-set-tiny root -8)
(codegen-features/CodegenFeatures-set-small root -701)
(codegen-features/CodegenFeatures-set-count root -70001)
(codegen-features/CodegenFeatures-set-total root -700001)
(codegen-features/CodegenFeatures-set-byte root 249)
(codegen-features/CodegenFeatures-set-words root 64999)
(codegen-features/CodegenFeatures-set-wide root 3000000000)
(codegen-features/CodegenFeatures-set-widest root 9007199254740990)
(def huge-value (int/u64 "18446744073709551614"))
(def lowest-value (int/s64 "-9223372036854775807"))
(codegen-features/CodegenFeatures-set-huge root huge-value)
(codegen-features/CodegenFeatures-set-lowest root lowest-value)
(codegen-features/CodegenFeatures-set-ratio32 root 3.5)
(codegen-features/CodegenFeatures-set-ratio64 root 4.5)
(codegen-features/CodegenFeatures-set-tone root 0)
(codegen-features/CodegenFeatures-set-flags root @[true false true])
(codegen-features/CodegenFeatures-set-scores root @[-2 0 5])
(codegen-features/CodegenFeatures-set-samples root @[1.5 -2.25])
(codegen-features/CodegenFeatures-set-names root @["one" "t\x00wo"])
(codegen-features/CodegenFeatures-set-empty root 4)
(codegen-features/CodegenFeatures-set-big-values root @[0 huge-value])
(codegen-features/CodegenFeatures-set-tones root @[0 1])

(codegen-features/CodegenFeatures-init-detail root)
(codegen-features/CodegenFeatures-detail-set-label root "stale")
(codegen-features/CodegenFeatures-set-number root 42)
(def number-view
  (:root (capnp/message-from-buffer (capnp/finish-builder builder))))
(assert (= codegen-features/CodegenFeatures-number-tag
           (codegen-features/CodegenFeatures-which number-view))
        "generated union scalar tag")
(assert (= 42 (codegen-features/CodegenFeatures-get-number number-view))
        "generated union scalar value")

# C++ initDetail() clears the group's pointer storage before returning it.
(codegen-features/CodegenFeatures-init-detail root)
(def cleared-detail
  (:root (capnp/message-from-buffer (capnp/finish-builder builder))))
(assert (= codegen-features/CodegenFeatures-detail-tag
           (codegen-features/CodegenFeatures-which cleared-detail))
        "generated group tag")
(assert (= "" (string
                 (codegen-features/CodegenFeatures-detail-get-label
                   cleared-detail)))
        "generated group initialization clears pointers")
(codegen-features/CodegenFeatures-detail-set-label root "detail")

(codegen-features/CodegenFeatures-set-payload root "\x00payload\xff")
(def child (codegen-features/CodegenFeatures-init-child root))
(codegen-features/CodegenFeatures-Child-set-code child 77)
(codegen-features/CodegenFeatures-Child-set-name child "child")
(def children (codegen-features/CodegenFeatures-init-children root 2))
(def first (capnp/struct-list-at children 0))
(codegen-features/CodegenFeatures-Child-set-code first 1)
(codegen-features/CodegenFeatures-Child-set-name first "one")
(def second (capnp/struct-list-at children 1))
(codegen-features/CodegenFeatures-Child-set-code second 2)
(codegen-features/CodegenFeatures-Child-set-name second "two")

(def decoded (:root (capnp/message-from-buffer (capnp/finish-builder builder))))
(assert (not (codegen-features/CodegenFeatures-get-enabled decoded)))
(assert (= -8 (codegen-features/CodegenFeatures-get-tiny decoded)))
(assert (= -701 (codegen-features/CodegenFeatures-get-small decoded)))
(assert (= -70001 (codegen-features/CodegenFeatures-get-count decoded)))
(assert (= -700001 (codegen-features/CodegenFeatures-get-total decoded)))
(assert (= 249 (codegen-features/CodegenFeatures-get-byte decoded)))
(assert (= 64999 (codegen-features/CodegenFeatures-get-words decoded)))
(assert (= 3000000000 (codegen-features/CodegenFeatures-get-wide decoded)))
(assert (= 9007199254740990
           (codegen-features/CodegenFeatures-get-widest decoded)))
(assert (= huge-value (codegen-features/CodegenFeatures-get-huge decoded)))
(assert (= lowest-value (codegen-features/CodegenFeatures-get-lowest decoded)))
(assert (= 3.5 (codegen-features/CodegenFeatures-get-ratio32 decoded)))
(assert (= 4.5 (codegen-features/CodegenFeatures-get-ratio64 decoded)))
(assert (= 0 (codegen-features/CodegenFeatures-get-tone decoded)))
(def flags (codegen-features/CodegenFeatures-get-flags decoded))
(assert (deep= @[true false true]
               (map |(codegen-features/CodegenFeatures-get-flags-at flags $)
                    (range 3))))
(def scores (codegen-features/CodegenFeatures-get-scores decoded))
(assert (deep= @[-2 0 5]
               (map |(codegen-features/CodegenFeatures-get-scores-at scores $)
                    (range 3))))
(def samples (codegen-features/CodegenFeatures-get-samples decoded))
(assert (deep= @[1.5 -2.25]
               (map |(codegen-features/CodegenFeatures-get-samples-at samples $)
                    (range 2))))
(def names (codegen-features/CodegenFeatures-get-names decoded))
(assert (= "t\x00wo"
           (string (codegen-features/CodegenFeatures-get-names-at names 1))))
(assert (= 4 (length (codegen-features/CodegenFeatures-get-empty decoded))))
(def big-values (codegen-features/CodegenFeatures-get-big-values decoded))
(assert (= huge-value
           (codegen-features/CodegenFeatures-get-big-values-at big-values 1)))
(def tones (codegen-features/CodegenFeatures-get-tones decoded))
(assert (= 1 (codegen-features/CodegenFeatures-get-tones-at tones 1)))
(assert (= "detail" (string
                       (codegen-features/CodegenFeatures-detail-get-label
                         decoded))))
(assert (= "\x00payload\xff"
           (string (codegen-features/CodegenFeatures-get-payload decoded))))
(def decoded-child (codegen-features/CodegenFeatures-get-child decoded))
(assert (= 77 (codegen-features/CodegenFeatures-Child-get-code decoded-child)))
(assert (= "child" (string
                      (codegen-features/CodegenFeatures-Child-get-name
                        decoded-child))))
(def decoded-children (codegen-features/CodegenFeatures-get-children decoded))
(assert (deep= @[1 2]
               (map codegen-features/CodegenFeatures-Child-get-code
                    decoded-children)))
(assert (deep= @["one" "two"]
               (map |(string
                       (codegen-features/CodegenFeatures-Child-get-name $))
                    decoded-children)))

(print "ok generated-builder")
