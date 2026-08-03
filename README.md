# capnp-janet

A native Cap'n Proto serialization runtime for [Janet](https://janet-lang.org),
plus a C library embedders can link without the Janet VM. Same family as
[capnp-fortran](https://github.com/HaoZeke/capnp-fortran) and
[c-capnproto](https://github.com/HaoZeke/c-capnproto): wire-format first,
zero-copy segment views, schema evolution defaults, and a path to a
`capnpc-janet` schema compiler plugin.

**Status:** early (`0.1.0-dev`). Reader + small builder land first. Codegen
and full parity track the table below.

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
- Single-segment builder: structs, Text/Data, lists, nested slots, stream serialize
- Packed codec (`capnp_pack` / `capnp_unpack`); unpacks official C++ packed frames
- Canonical form (`capnp_canonicalize`) — **byte-identical** to
  `capnp convert binary:canonical` on AddressBook
- Sample parity tests: AddressBook + calculator Expression (C++/pycapnp shapes)
- Janet module (`capnp/...`) and plain C API for static embeds
- Traversal word budget and depth limit (C++ defaults)

## Parity

| Feature | capnp-c | capnp-C++ | capnp-fortran | capnp-janet |
|---------|---------|-----------|---------------|-------------|
| Wire format read (struct/list/far/cap) | yes | yes | yes | **yes** |
| Stream framing | yes | yes | yes | **yes** |
| Packed codec | yes | yes | yes | **yes** (roundtrip; pack heuristic may differ) |
| Zero-copy reads from caller buffer | yes | yes | yes | **yes** |
| Traversal and depth limits | no | yes | yes | **yes** |
| Schema-evolution reads (defaults past end) | partial | yes | yes | **yes** |
| Builder / deep copy | limited | yes | yes | **builder + copy_flat** (no orphans/setp-clone yet) |
| Canonical form | no | yes | yes | **yes** (byte-parity tested) |
| Code generator (`capnp compile -o`) | yes | yes | yes | **planned** (`capnpc-janet`) |
| RPC | no | yes | yes | out of scope for v0.x |

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

1. **v0.1** — reader + minimal builder + C/Janet API + wire tests
2. **v0.2** — packed + canonical + primitive lists + AddressBook/calculator samples (**this tree**)
3. **v0.3** — `capnpc-janet`: `capnp compile -o` plugin, generated accessors
4. **v0.4** — orphans, cross-message setp clone, c-capnproto golden-master interop
5. Dynamic API as needed; RPC remains out of scope

## Related

- [capnp-fortran](https://github.com/HaoZeke/capnp-fortran) — native F2018 Cap'n
- [c-capnproto](https://github.com/HaoZeke/c-capnproto) — C reference encoder
- [Cap'n Proto](https://capnproto.org) — encoding and RPC specs

## License

MIT — see [LICENSE](LICENSE).
