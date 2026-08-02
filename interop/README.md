# c-capnproto interop (planned)

Same golden-master idea as
[capnp-fortran/interop](https://github.com/HaoZeke/capnp-fortran/tree/main/interop):

1. Clone reference sources (untracked):

   ```console
   git clone https://github.com/HaoZeke/c-capnproto third_party/c-capnproto
   ```

2. Build the same demo message with c-capnproto and with this runtime.
3. `memcmp` framed bytes; cross-decode both ways.

Not wired into `meson.build` until the builder allocation order is locked to
c-capnproto's. Track in the project issue list.
