# capnp-janet

A native Cap'n Proto serialization runtime for [Janet](https://janet-lang.org),
plus a C library embedders can link without the Janet VM. Docs:
<https://capnp-janet.rgoswami.me/> (org-mode source in `docs/orgmode/`).
Same family as
[capnp-fortran](https://capnp-fortran.rgoswami.me) and
[c-capnproto](https://c-capnproto.rgoswami.me): wire-format first,
zero-copy segment views, schema evolution defaults, and the `capnpc-janet`
schema compiler plugin.

**Status:** `0.2.5`. Reader, multi-segment builder, packed and canonical
codecs, and typed `capnpc-janet` readers/builders. Wire encode of AddressBook
matches official `capnp encode` when fields are set in schema order.

## Why this exists

GrokOS (and other Cap'n-first systems) need policy packs and glue in a small
embeddable language that can **read Cap'n messages directly** — not C DTO
tables projected field-by-field. Janet is the pack VM; Cap'n remains the
only host↔pack surface. This repo is the public Cap'n implementation those
packs import.

## Features (current)

- Stream-framed deserialize (copy) and zero-copy view of caller buffers
- Struct / list / far / double-far / capability pointer resolution
- Data field readers (`u8`/`u16`/`u32`/`u64`/`f32`/`f64`/`bool`) with past-end defaults
- Text, Data, `List(Text)`, primitive lists (`u8`/`u16`/`u32`/`u64`/`f32`/`f64`)
- `List(Bool)` bit-lists (`capnp_list_get_bool` / `capnp_builder_set_list_bool`)
- `List(Void)` length-only (`capnp_builder_set_list_void` + `capnp_list_len`)
- Schema-evolution list upgrade/downgrade views (see below)
- Multi-segment builder: growable arena, far / double-far pointer writes, stream serialize
- Packed codec (`capnp_pack` / `capnp_unpack`); **byte-identical** to
  Cap'n C++ `PackedOutputStream` / `capnp convert binary:packed` (AddressBook golden)
- Canonical form (`capnp_canonicalize`) — **byte-identical** to
  `capnp convert binary:canonical` on AddressBook
- Sample parity tests: AddressBook + calculator Expression (C++/pycapnp shapes)
- Janet module with arena-backed nested builders, typed primitive lists, and
  lossless `Int64`/`UInt64`; plain C API for static embeds
- Generated scalar-default, union, group, struct, and list reader/builder helpers
- `capnp_janet_lookup_into` + `janet/policy.janet` for compiled `.jimage` packs
- Traversal word budget and depth limit (C++ defaults)

## Parity

| Feature | capnp-c | capnp-C++ | capnp-fortran | capnp-janet |
|---------|---------|-----------|---------------|-------------|
| Wire format read (struct/list/far/cap) | yes | yes | yes | **yes** |
| Stream framing | yes | yes | yes | **yes** |
| Packed codec | yes | yes | yes | **yes** (byte-identical C++ pack heuristic) |
| Zero-copy reads from caller buffer | yes | yes | yes | **yes** |
| Traversal and depth limits | yes (0.2+) | yes | yes | **yes** |
| Schema-evolution reads (defaults past end, list up/downgrade) | partial | yes | yes | **yes** (supported cases below) |
| Builder / deep copy | limited | yes | yes | **multi-seg builder + far/double-far + copy_flat + setp** (no orphans yet) |
| Canonical form | yes (0.3.0) | yes | yes | **yes** (byte-parity tested) |
| Code generator (`capnp compile -o`) | yes | yes | yes | **yes** (typed readers/builders, unions, groups, scalar defaults, lists, interfaces) |
| RPC levels 1-4 | no | L1-L2 (no L4) | L1-L2 (no L4) | **L1-L4** (C vat, `include/capnp-janet/capnp_rpc.h`; L3 both halves with `Accept.embargo`, over `schema/rpc-threeparty.capnp`, tested as a three-vat handoff; L4 `Join`, which upstream C++ lacks) |

## Schema-evolution list views

Cap'n Proto allows a writer and a reader to disagree on whether a list is a
primitive list or a list of structs that only use field `@0` (encoding.html
"list upgrades"). This runtime implements the same cases as Cap'n C++ and
[capnp-fortran](https://github.com/HaoZeke/capnp-fortran) parity tests:

| Writer encoded | Reader asks for | API |
|----------------|-----------------|-----|
| `List(UInt8/16/32/64)` | `List(Struct)` with scalar `@0` | `capnp_list_get_struct` then `capnp_get_u*` |
| `List(pointer)` e.g. `List(Text)` | `List(Struct)` with pointer `@0` | `capnp_list_get_struct` then `capnp_get_text` / `capnp_getp` |
| `List(Struct)` with data word 0 | `List(UInt*)` field `@0` | `capnp_list_get_u8` / `u16` / `u32` / `u64` / `f64` |
| `List(Struct)` with pointer 0 | `List(Text)` / pointer list | `capnp_list_get_text` (already accepted composite) |

**Guarantees on upgrade views.** Synthetic structs limit `data_bits` to the
element width: reading a wider field (e.g. `u64` on a `u32` element view)
returns the caller default and never spills into the next element.

**Explicit non-goals (refuse with `CAPNP_ERR_KIND` or default, no silent
partial).** Matching C++/fortran:

- `List(Bool)` and `List(Void)` do **not** upgrade to struct views.
- Cross-width primitive demotion without a composite (e.g. treat `List(u8)` as
  `List(u32)`) is not supported; only exact-width prim lists or composite
  field-`@0` downgrade.
- `capnp_getp` remains struct-only; for composite first-pointer access use
  `capnp_list_getp` / `capnp_list_get_struct` then `capnp_getp` on the element
  (fortran overloads `getp` on lists; the C surface keeps the split).
- Full matrix of every esize pair is not claimed: implement the fortran
  `t_list_upgrade_views` / `t_list_downgrade_views` shapes above; other pairs
  return a kind error or the supplied default.

Tests: `test/test_list_evolution.c` (meson target `list_evolution`).


## Install

Janet + jpm (native module `(import capnp)`):

```console
$ jpm install https://github.com/HaoZeke/capnp-janet.git
# after janet-lang/pkgs lists it:
$ jpm install capnp
```

Janet 1.36+ bundle installer (same `project.janet`, needs `spork`):

```console
$ janet --install .
```

Toolchain via [pixi](https://pixi.sh) (conda-forge):

```console
$ pixi install
$ pixi run configure
$ pixi run build
$ pixi run test
```

Or bare Meson:

```console
$ meson setup build
$ meson compile -C build
$ meson test -C build
```

Optional Janet module: configure with a `janet` pkg-config package visible,
or `-Djanet=enabled`.

### C embed (e.g. policyd)

```c
#include <capnp-janet/capnp_message.h>

capnp_message_t msg;
capnp_message_view_flat(&msg, bytes, len);
capnp_ptr_t root;
capnp_root(&msg, &root);
const char *s; size_t n;
capnp_get_text(&root, /*ptr*/0, &s, &n);
```

Link `libcapnp_janet` (pkg-config: `capnp-janet`).

### Janet

Build a framed root struct in one growable arena, then read it back:

```janet
(import capnp)

(def builder (capnp/new-builder))
(def body (capnp/init-root builder 1 1))
(capnp/set-u32 body 0 42)
(capnp/set-text body 0 "hello")
(def buf (capnp/finish-builder builder))
(def root (capnp/root (capnp/message-from-buffer buf)))
(print (capnp/get-u32 root 0))       # 42
(print (capnp/get-text root 0))      # hello
```

`init-struct`, `init-struct-list`, and `struct-list-at` return mutable views
into the same arena. Typed setters accept the schema default as an optional
final argument and store the required XOR wire value. `build-message` remains
as a compact compatibility helper for flat demo structs.

Lists come out of a decoded message. A list pointer carries Janet's
`length`, index and iteration protocols, so it answers to the same forms
as an array rather than to a parallel set of functions:

```janet
(import capnp)

(def msg (capnp/message-from-buffer
           (slurp "test/fixtures/addressbook_alice_bob.bin")))
(def people (capnp/getp (:root msg) 0))

(length people)                       # 2
(:text (in people 0) 0)               # "Alice"
(:u32 (people 1) 0)                   # 456
(get people 99)                       # nil

(map |(string (:text $ 0)) people)    # @["Alice" "Bob"]
(seq [p :in people] (:u32 p 0))       # @[123 456]
(each p people (print (:text p 0)))
```

`(:verb receiver ...)` reaches the same accessors as the `capnp/` functions:
`:kind`, `:ptr`, every signed/unsigned scalar width, `:f32`, `:f64`, `:bool`,
`:text`, and typed `:*-at` primitive-list readers. A message answers to `:root`.
The long forms (`capnp/get-u32`, `capnp/list-len`, `capnp/list-getp`, …)
remain and behave identically.

Embedders that amalgamate Janet call `capnp_janet_register(env)` after
`janet_init` (see `src/janet_mod.c`).

## Samples and tests

Wire tests include the classic Cap'n samples (same content as C++/pycapnp):

| Suite | Schema | What |
|-------|--------|------|
| `addressbook` | `schema/addressbook.capnp` | Alice/Bob book, phones, employment union; builder + `capnp encode` golden |
| `calculator` | `schema/calculator.capnp` | Expression trees (literal / call / nested), evaluate in-process; goldens |
| `wire` / `list_text` | demo shapes | core reader/builder |

The calculator schema here is a serialization-only Expression subset of the
official sample: it exercises the wire format, not the vat. The RPC surface
lives in `capnp_rpc.h` and is tested against a live capnp-C++ peer
(`interop/run_rpc_interop.sh`), and the level 3 tests decode frames the
reference `capnp` CLI encoded (`test/fixtures/rpc-{provide,accept,introduce}.bin`,
regenerated by `scripts/gen-rpc-frames.sh`).

Regenerate goldens (needs system `capnp`):

```console
$ ./scripts/gen-sample-fixtures.sh
```

## Layout

```
src/           C runtime + Janet module
test/          C wire + sample tests
test/fixtures/ capnp encode goldens
schema/        demo + addressbook + calculator
app/           capnpc-janet schema compiler plugin
interop/       official CLI and live capnp-C++ interop
docs/orgmode/  architecture notes
```

## Compatibility boundaries

The supported surface includes readers, arena builders, packed and canonical
codecs, generated typed helpers, and the C RPC vat through level 4. It does not
expose C++ orphan ownership, generated pointer-default constants, a dynamic
schema API, or Janet wrappers for the C RPC vat. Calls outside the documented
list-evolution matrix return a kind error or the supplied default.

## Related

- [capnp-fortran](https://github.com/HaoZeke/capnp-fortran) — native F2018 Cap'n
- [c-capnproto](https://github.com/HaoZeke/c-capnproto) — C reference encoder
- [capnp-ts](https://github.com/HaoZeke/capnp-ts) — TypeScript port
- [Cap'n Proto](https://capnproto.org) — encoding and RPC specs

## License

MIT — see [LICENSE](LICENSE).
