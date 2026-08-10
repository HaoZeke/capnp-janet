# capnp-janet

A native Cap'n Proto serialization runtime for [Janet](https://janet-lang.org),
plus a C library embedders can link without the Janet VM. Same family as
[capnp-fortran](https://github.com/HaoZeke/capnp-fortran) and
[c-capnproto](https://github.com/HaoZeke/c-capnproto): wire-format first,
zero-copy segment views, schema evolution defaults, and a path to a
`capnpc-janet` schema compiler plugin.

**Status:** `0.2.0-dev`. Reader, multi-segment builder, packed and
canonical codecs, and `capnpc-janet` v1. Wire encode of AddressBook
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
- Data field readers (`u8`/`u16`/`u32`/`u64`/`f64`/`bool`) with past-end defaults
- Text, Data, `List(Text)`, primitive lists (`u8`/`u16`/`u32`/`u64`/`f64`)
- `List(Bool)` bit-lists (`capnp_list_get_bool` / `capnp_builder_set_list_bool`)
- `List(Void)` length-only (`capnp_builder_set_list_void` + `capnp_list_len`)
- Schema-evolution list upgrade/downgrade views (see below)
- Multi-segment builder: growable arena, far / double-far pointer writes, stream serialize
- Packed codec (`capnp_pack` / `capnp_unpack`); **byte-identical** to
  Cap'n C++ `PackedOutputStream` / `capnp convert binary:packed` (AddressBook golden)
- Canonical form (`capnp_canonicalize`) — **byte-identical** to
  `capnp convert binary:canonical` on AddressBook
- Sample parity tests: AddressBook + calculator Expression (C++/pycapnp shapes)
- Janet module (`capnp/...` including `get-u64`) and plain C API for static embeds
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
| Code generator (`capnp compile -o`) | yes | yes | yes | **yes** (`capnpc-janet` v1: structs/enums + getters) |
| RPC | no | yes | yes | out of scope for v0.x |

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
  `t_list_upgrade_views` / `t_list_downgrade_views` shapes above; anything else
  is out of scope until a product need lands.

Tests: `test/test_list_evolution.c` (meson target `list_evolution`).


## Install

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

```janet
(import capnp)

(def buf (capnp/build-demo 42 "hello" ["a" "bb"]))
(def msg (capnp/message-from-buffer buf))
(def root (capnp/root msg))
(print (capnp/get-u32 root 0))       # 42
; (capnp/get-u64 root byte-offset) for UInt64/Int64 fields
(print (capnp/get-text root 0))      # hello
(def items (capnp/getp root 1))
(print (capnp/list-get-text items 1)) # bb
```

Embedders that amalgamate Janet call `capnp_janet_register(env)` after
`janet_init` (see `src/janet_mod.c`).

## Samples and tests

Wire tests include the classic Cap'n samples (same content as C++/pycapnp):

| Suite | Schema | What |
|-------|--------|------|
| `addressbook` | `schema/addressbook.capnp` | Alice/Bob book, phones, employment union; builder + `capnp encode` golden |
| `calculator` | `schema/calculator.capnp` | Expression trees (literal / call / nested), evaluate in-process; goldens |
| `wire` / `list_text` | demo shapes | core reader/builder |

Calculator **RPC** (interfaces, pipelining) is out of scope; the schema is a
serialization-only Expression subset of the official calculator sample.

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
app/           capnpc-janet plugin (stub)
interop/       c-capnproto golden-master notes
docs/orgmode/  architecture notes
```

## Roadmap

1. **v0.1** — reader + minimal builder + C/Janet API + wire tests (done)
2. **v0.2** — packed + canonical + primitive lists + samples + schema-order
   AddressBook encode (this tree)
3. **v0.3** — `capnpc-janet` plugin (structs/enums/getters) (landed v1)
4. **v0.4** — orphans, c-capnproto golden-master, richer codegen (unions/defaults)
5. Dynamic API as needed; RPC remains out of scope

## Related

- [capnp-fortran](https://github.com/HaoZeke/capnp-fortran) — native F2018 Cap'n
- [c-capnproto](https://github.com/HaoZeke/c-capnproto) — C reference encoder
- [Cap'n Proto](https://capnproto.org) — encoding and RPC specs

## License

MIT — see [LICENSE](LICENSE).
