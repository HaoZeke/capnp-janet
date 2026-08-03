# Interop with official Cap'n C++ / c-capnproto

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
- **Packed**: our unpacker accepts official packed; our pack roundtrips
  (byte-identical pack is not required — valid encodings differ by heuristic)
- **Canonical**: our `capnp_canonicalize` is **byte-identical** to
  `binary:canonical` on AddressBook

## c-capnproto golden-master (planned)

Same idea as
[capnp-fortran/interop](https://github.com/HaoZeke/capnp-fortran/tree/main/interop):

1. Clone reference (untracked):

   ```console
   git clone https://github.com/HaoZeke/c-capnproto third_party/c-capnproto
   ```

2. Build the same demo message with c-capnproto and with this runtime.
3. `memcmp` framed bytes; cross-decode both ways.

Enable with `-Dinterop=true` once the builder allocation order is locked for
that suite.
