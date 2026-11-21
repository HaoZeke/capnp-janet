# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Pre-1.0 minor releases may include breaking API changes.

## [Unreleased]

### Added

- `capnp_builder_orphan_free` zeroes an orphan that will not be adopted,
  including text reachable from it, and leaves the orphan dead.
- `capnp_dyn_error` returns the text of the last decode failure.
- `capnp_u32_grow` grows a `uint32_t` capacity without wrapping.
- Janet arena builders expose root, nested-struct, struct-list, pointer, and
  typed primitive-list construction without rebuilding intermediate messages.
- Janet scalar access covers every signed/unsigned width, Float32/Float64,
  schema-default XOR, and exact `Int64`/`UInt64` values.
- `capnpc-janet` emits executable typed readers and builders for scalars,
  Text/Data, structs, primitive and struct lists, unions, groups, and interface
  metadata. Generated field names follow Janet's kebab-case convention.
- The builder can lay out segments the way C++ `MallocMessageBuilder`
  does. The entry points are internal to `capnp-eval`, so the shared
  library ABI is unchanged.

### Fixed

- The text parser rejects runs of operator characters such as `=-5` or
  `=.5`, which the C++ lexer reads as one unknown operator.
- `capnp-eval` rejects `-q` everywhere and `--quiet` outside `convert` and
  `decode`, matching the option sets of the C++ `capnp` subcommands.
- The traversal budget is spent the way C++ spends it: two words for the
  root pointer and nothing for pointer words inside their parent, so a
  message readable under a C++ limit is readable under the same limit here.
- The reader checks the nesting limit only when it lands a struct or list,
  as C++ does. A struct at the limit reads its null pointers, Text and Data,
  and a far pointer no longer costs an extra level.
- `capnp-eval convert`, `decode` and `encode` lift the nesting and
  traversal limits, as `capnp` does for every conversion. A struct nested
  more than 64 deep, or more than 8M words read, used to fail. Canonical
  output follows the message's nesting limit and no longer stops at 64.
- A `Finish` with `releaseResultCaps` releases the export reference each
  capability in the stored Return took, as C++ does. Such references used
  to stay held until the connection closed.
- `capnp_rpc_send_finish` sets `releaseResultCaps` to false once the Return
  has arrived, as C++ does. It used to leave the default of true, so a C++
  server released the capabilities in the answer, the bootstrap included,
  and the next call on them failed.
- JSON `$Json.base64` and `$Json.hex` strings decode as `capnp convert`
  does: characters outside the alphabet are skipped, a bad hex digit reads
  as 0, and an odd hex character or partial base64 byte is dropped. These
  inputs used to be rejected.
- `capnpc-janet` writes only a file's own nodes into its module. Every
  module used to repeat the nodes of every other requested or imported file.
- Generated getters return the schema default for a null Text, Data, struct
  or list field. Text and Data defaults were not emitted, and the struct and
  list default check never matched because it took `(v :kind)` for a call.
- A double-far landing pad only has to be a far pointer, as in C++. Pads
  with their own double-far bit set, and tags with a non-zero offset, used
  to be rejected.
- List readers follow C++ for wider elements: scalars read the low bytes of
  each element, `List(Bool)` reads bit `index` from the start of any data
  list, and a `List(Void)` reads as empty structs. They used to return the
  default or fail.
- `capnp-eval convert` to binary or packed matches `capnp convert` byte for
  byte across segments. `--segment-size` up to 32 words used to fail, and a
  message over 1024 words came out as one segment.
- The text and JSON encoders fail on a corrupt pointer, a pointer of the
  wrong kind, or a list whose elements are too narrow for the field. They
  used to print the default and succeed; `capnp convert` 1.4.0 fails.
- The text and JSON encoders print a null root as an empty struct, with
  every scalar default, as `capnp convert` does.
- `capnp-eval convert` writes a null root as an empty struct in binary,
  packed and flat output, and rejects a list or capability root in every
  output format, as `capnp convert` 1.4.0 does.
- The RPC vat no longer answers Unimplemented or Abort with another
  Unimplemented. Abort, or Unimplemented for anything but Resolve, fails
  every unanswered question and returns an error, as the C++ 1.4.0 vat
  ends the connection there.
- `capnp_builder_new_orphan_list` rejects an element count above `2^29 - 1`
  when the orphan is created. It used to accept a larger count for void,
  bit, byte, two-byte and four-byte elements and fail only at adopt. C++
  1.4.0 rejects the same counts at creation.
- `capnp_builder_new_orphan_struct_list` rejects an element count above
  `2^29 - 1`. The tag stores that count, and a larger value is truncated
  by the 30-bit offset mask. C++ 1.4.0 rejects the same counts.
- `capnp_builder_set_text` rejects a length of `2^29 - 1` or more before
  allocating. The NUL makes the list one byte longer than the text, and
  that byte count has to fit in 29 bits.
- An empty list is not left pointing past the segment after a tail
  reclaim, and a zero-word list or struct aimed past the segment end
  reads as empty.
- `capnp_builder_adopt` zeroes the object an occupied slot already
  named before it links the orphan.
- A JSON lone surrogate is reported as `lone surrogate`. A valid pair
  is still stored as UTF-8. C++ 1.4.0 stores CESU-8 for each `\u` unit
  and accepts a lone surrogate.
- A discriminant that does not land in the struct data section is
  `CAPNP_ERR_SCHEMA` rather than an allocation error.
- Scalar byte-range validation rejects offset arithmetic overflow before
  touching the message buffer.
- `List(Text)` builders preserve explicit lengths, including embedded NUL
  bytes, and Float32 access avoids strict-aliasing and host-endian assumptions.
- Generated group initialization clears overlapping pointer storage and selects
  the union arm, matching the C++ builder contract.
- Standalone generated-module tests retain the native library search path and
  preload an instrumented runtime when required.

### Changed

- `capnp_builder_orphan_free` leaves the segment tail where it is, as
  C++ `OrphanBuilder::euthanize` does. `capnp_orphan_t` identity lives in
  an arena-owned table, so the public struct keeps its released layout.
  C++ `Orphan` is move-only; keep one handle per orphan.
- `capnp_rpc_fill_fn` returns `int`. A callback that returns `void` is
  an incompatible pointer type. Return nonzero when the callback fails
  so the call is not sent.
- Decode, encode, and eval take a mutable `capnp_dyn_t`. Decode and
  eval record `capnp_dyn_error` on that object.
- `capnp_message_t` must be zero-initialized or passed to
  `capnp_message_init` before it is parsed or freed. A second parse
  releases the previous buffer.
- Generated modules import `capnp` themselves, and CI executes their builders
  through the native module rather than checking source text alone.
- Interop documentation describes the wired official-CLI and live C++ gates;
  the unused `interop` Meson option is removed.

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

[Unreleased]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.5...main
[0.2.5]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.4...v0.2.5
[0.2.4]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.3...v0.2.4
[0.2.3]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/HaoZeke/capnp-janet/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/HaoZeke/capnp-janet/releases/tag/v0.2.0
