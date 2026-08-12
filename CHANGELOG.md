# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Pre-1.0 minor releases may include breaking API changes.

## [Unreleased]

### Added

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
