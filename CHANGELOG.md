# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Pre-1.0 minor releases may include breaking API changes.

## [Unreleased]

## [0.2.5] - 2026-08-15

### Fixed

- The RPC tests keep their connections in static storage rather than on
  the stack. A `capnp_rpc_conn_t` is about a megabyte, since the answer
  and question tables hold their replies inline, and Windows gives a
  thread a 1 MiB stack where Linux gives 8: the three RPC tests died
  with STATUS_STACK_OVERFLOW on MSYS2, and the handoff test wanted six
  connections at once. `capnp_rpc.h` now says so, so a caller does not
  meet it the hard way.

## [0.2.4] - 2026-08-15

### Fixed

- `capnp_builder_seg_data` is exported too. 0.2.3 annotated the public
  API by pattern and this one declaration, which puts the `*` on the
  name rather than the type, slipped through: a Windows consumer linking
  the shared library got an undefined symbol instead of a missing import
  library. Checked exhaustively this time, and against the symbols the
  tests actually use.

## [0.2.3] - 2026-08-15

### Fixed

- A shared build links on Windows. MSVC writes an import library only
  when something is `__declspec(dllexport)`, so the build produced no
  `capnp_janet.lib` and every consumer failed with LNK1181. The public
  headers now carry `CAPNP_JANET_EXPORT`, defined by
  `CAPNP_JANET_BUILDING` while building and `CAPNP_JANET_STATIC` for a
  static archive, as c-capnproto already does. Found by WrapDB's MSVC
  runners.

### Changed

- `capnp_rpc.h` no longer says level 3 is absent by construction. It has
  been present since 0.2.0, over `schema/rpc-threeparty.capnp`.

## [0.2.2] - 2026-08-15

### Fixed

- Primitive lists are written little-endian element by element. The
  builder bulk-copied the caller's array into the message, which is the
  host's byte order, so on a big-endian host every element came back
  swapped: 1 read as 16777216. The round-trip tests could not see it,
  because the reader undid exactly what the writer did; a test that pins
  the serialized bytes can, and now does. Found by WrapDB's s390x
  runners.
- `capnpc-janet` builds under MSVC. It reached for `getcwd` through
  `<unistd.h>`, which MSVC does not have; it is `_getcwd` in
  `<direct.h>` there, while MinGW and the MSYS2 environments keep the
  POSIX spelling.

## [0.2.1] - 2026-08-15

### Fixed

- `bash` is optional. It was a hard `find_program(required: true)` for
  one test script, so configure failed outright on a musl host, which
  ships busybox `ash` and no bash: the library could not be built at all
  because of a smoke test. The test registers only when bash is present.
  Found by WrapDB's Alpine runners, on every architecture.

## [0.2.0] - 2026-08-15

First tagged release. RPC levels 1 through 4, level 3 over the
three-party network layer this family shares, and a meson build a
consumer can pull in as a subproject.

### Changed

- `VERSION` is now `VERSION.txt`. On a case-insensitive filesystem a
  file called `VERSION` in an implicit include directory answers
  `#include <version>`, which is how libc++ breaks on macOS; c-capnproto
  met exactly that and this avoids it before a release goes out.

### Added

- RPC level 3, both halves. `Provide` holds a capability under the
  recipient's nonce and `Accept` claims it; an `Accept` with `embargo`
  waits for `Disembargo` with `context.provide`. A `thirdPartyHosted`
  CapDescriptor records an introduction, handed over by
  `capnp_rpc_pending_introductions` and finished by
  `capnp_rpc_introduction_done`, which releases the vine.
  `capnp_rpc_send_provide`, `capnp_rpc_send_accept` and
  `capnp_rpc_send_disembargo_provide` are the introducer's side.
- `schema/rpc-threeparty.capnp`, the network layer that names a third
  vat, shared verbatim with c-capnproto, capnp-fortran and capnp-ts.
  `rpc.capnp` leaves those ids to the network, and `rpc-twoparty.capnp`
  declares them empty because a two-party connection has no third to
  name. A vat speaks one layer or the other, not both.
- `capnp_rpc_set_vat`: level 3 arrangements belong to a vat rather than
  a connection, since a handoff is made on one and claimed on another.
- `capnp_rpc_answer_cap_id`, without which a capability returned in an
  answer could not be called.
- Level 3 goldens the reference `capnp` CLI encodes
  (`test/fixtures/rpc-{provide,accept,introduce}.bin`), regenerated and
  verified by `scripts/gen-rpc-frames.sh`.
- Janet bundle config (`bundle/info.jdn`, `bundle/init.janet`) so
  `janet --install .` wraps the existing `project.janet` via
  `jpm-shim-env`, same shape as jhydro.
- Sphinx docs from `docs/orgmode/` via ox-rst (`emacs --batch -l docs/export.el`).
  Generated RST is not tracked.

### Fixed

- Builder empty struct pointer is B=-1 (`0xFFFFFFFC`), matching
  encoding.html and c-capnproto 0.3.0.

### Added

- Schema-order AddressBook encode memcmp against the official
  `capnp encode` golden.
- CHANGELOG.md.

## [0.2.0-dev] - 2026-08-10

Packed codec matches Cap'n C++ `PackedOutputStream` (0xFF extra words
with fewer than two zero bytes). Canonical AddressBook matches
`capnp convert binary:canonical`. Multi-segment builder, far/double-far,
`capnpc-janet` v1, list upgrade views.

[Unreleased]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.0...main
[0.2.5]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.4...v0.2.5
[0.2.4]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.3...v0.2.4
[0.2.3]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/HaoZeke/capnp-janet/releases/tag/v0.2.0
