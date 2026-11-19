# Contributing

## Build

```console
pixi install
pixi run configure
pixi run build
pixi run test
```

All builds and heavy tests for this maintainer run on remote builders; keep
changes small and leave `meson test` green.

## Style

- C11, no GNU extensions required
- Public headers under `src/` with `CAPNP_` / `capnp_` prefixes
- Present-tense comments only (what/why), no temporal "after the fix" notes
- Wire layout matches the Cap'n Proto encoding spec and capnp-fortran parity tests

## Scope

Prefer wire-format and codegen work here. Application policy packs belong in
their downstream repositories, not this library.

## Sanitizers and the Janet module

The Janet interpreter in this environment is not built with a sanitizer.
`capnp.so` is. Loading that module fails unless the sanitizer runtime is
the first library in the process. Preloading it into the shell aborts the
shell, and preloading it into a binary that is already linked with the
runtime initializes it twice. Put the preload on `janet` only.

Address and leak checks, with the module built by
`meson setup build-janet-asan -Djanet=enabled -Db_sanitize=address,undefined`:

```console
JANET=$(command -v janet)
ASAN=$(gcc-13 -print-file-name=libasan.so)
UBSAN=$(gcc-13 -print-file-name=libubsan.so)
mkdir -p /tmp/jwrap-both
printf '#!/bin/sh\nexport LD_PRELOAD=%s:%s\nexec %s "$@"\n' \
  "$ASAN" "$UBSAN" "$JANET" > /tmp/jwrap-both/janet
chmod +x /tmp/jwrap-both/janet
PATH="/tmp/jwrap-both:$PATH" \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  meson test -C build-janet-asan
```

Undefined checks alone use the same wrapper with only `libubsan.so` and
`meson setup build-janet-ubsan -Djanet=enabled -Db_sanitize=undefined`.
