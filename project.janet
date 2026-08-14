(declare-project
  :name "capnp"
  :description "Native Cap'n Proto runtime for Janet (wire, packed, canonical)."
  :author "Rohit Goswami <rgoswami@ieee.org>"
  :license "MIT"
  :url "https://capnp-janet.rgoswami.me/"
  :repo "git+https://github.com/HaoZeke/capnp-janet.git")

(declare-native
  :name "capnp"
  :cflags @["-std=c11" "-Iinclude"]
  :source @["src/janet_mod.c"
            "src/capnp_message.c"
            "src/capnp_builder.c"
            "src/capnp_packed.c"
            "src/capnp_canonical.c"
            "src/capnp_rpc.c"]
  :headers @["include/capnp-janet/capnp_builder.h"
             "include/capnp-janet/capnp_canonical.h"
             "include/capnp-janet/capnp_copy.h"
             "include/capnp-janet/capnp_janet.h"
             "include/capnp-janet/capnp_kinds.h"
             "include/capnp-janet/capnp_message.h"
             "include/capnp-janet/capnp_packed.h"
             "include/capnp-janet/capnp_pointer.h"
             "include/capnp-janet/capnp_rpc.h"])
