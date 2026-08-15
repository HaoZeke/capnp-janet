# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Pre-1.0 minor releases may include breaking API changes.

## [Unreleased]

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

[Unreleased]: https://github.com/HaoZeke/capnp-janet/commits/main
