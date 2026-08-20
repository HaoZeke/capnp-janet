# Interop with official Cap'n Proto

## Official CLI goldens (wired)

`scripts/gen-sample-fixtures.sh` uses system `capnp encode` / `capnp convert`:

| Fixture | Source |
|---------|--------|
| `test/fixtures/addressbook_alice_bob.bin` | `capnp encode` AddressBook |
| `test/fixtures/addressbook_alice_bob.packed` | `capnp convert binary:packed` |
| `test/fixtures/addressbook_alice_bob.canonical` | `capnp convert binary:canonical` |
| `test/fixtures/calculator_*.bin` | Expression trees |

Assertions:

- **Reader**: decode encode frames (AddressBook / calculator suites)
- **Packed**: `capnp_pack` is **byte-identical** to Cap'n C++
  `PackedOutputStream` / `capnp convert binary:packed` on AddressBook and the
  dense / zero / one-zero-verbatim fixtures in `test/test_packed.c`. Unpack of
  official packed frames is also required. Heuristic after tag `0xff`: include
  up to 255 following words that each contain **fewer than two** zero bytes
  (matches C++ `serialize-packed.c++`).
- **Canonical**: our `capnp_canonicalize` is **byte-identical** to
  `binary:canonical` on AddressBook

## Live capnp-C++ RPC peer

When the C++ toolchain, `capnp-rpc`, and compiler tools are present, Meson
generates C++ bindings for `schema/adder.capnp` and builds two processes:

- `rpc_peer_server`: an official `capnp::EzRpcServer` implementation.
- `rpc_client_c`: the C vat from this runtime.

The client bootstraps the remote capability, calls `add(-20, 22)`, and requires
the C++ peer to return `2`. Run the isolated gate with:

```console
meson test -C build rpc_interop_cpp --print-errorlogs
```

Level 3 `Provide`, `Accept`, embargo, and handoff frames use
`schema/rpc-threeparty.capnp`. `scripts/gen-rpc-frames.sh --check` compares the
checked-in frames with the official `capnp encode` output.
