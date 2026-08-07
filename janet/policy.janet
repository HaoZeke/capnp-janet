# Named Policy.capnp readers for compiled pack images.
# Import after the capnp native module is on the lookup (host register or
# `(import capnp)` in the janet CLI). Indices match grok-policyd layout.janet
# until capnpc-janet emits this file from schema.

(def shell-view-under-workspace-bit 0)
(def shell-view-argv-ptr 1)
(def shell-view-path-probes-ptr 2)
(def path-probe-exists-bit 0)
(def path-probe-head-ptr 2)
(def audio-check-action-u16 0)

(defn shell-view/under-workspace? [root]
  (capnp/get-bool root shell-view-under-workspace-bit))

(defn shell-view/argv [root]
  (def lp (capnp/getp root shell-view-argv-ptr))
  (seq [i :range [0 (capnp/list-len lp)]]
    (capnp/list-get-text lp i)))

(defn path-probe/exists? [el]
  (capnp/get-bool el path-probe-exists-bit))

(defn path-probe/head [el]
  (capnp/get-text el path-probe-head-ptr))

(defn shell-view/path-probes [root]
  (def lp (capnp/getp root shell-view-path-probes-ptr))
  (seq [i :range [0 (capnp/list-len lp)]]
    (capnp/list-getp lp i)))

(defn audio-check/action [root]
  (capnp/get-u16 root audio-check-action-u16 0))
