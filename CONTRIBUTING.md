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
