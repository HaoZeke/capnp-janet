#!/usr/bin/env bash
# Regenerate Cap'n sample golden frames with the system `capnp` CLI.
# Requires: capnp encode (Cap'n Proto 0.9+).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIX="$ROOT/test/fixtures"
mkdir -p "$FIX"

capnp encode "$ROOT/schema/addressbook.capnp" AddressBook \
  >"$FIX/addressbook_alice_bob.bin" <<'EOF'
( people = [
  ( id = 123,
    name = "Alice",
    email = "alice@example.com",
    phones = [
      (number = "555-1212", type = mobile)
    ],
    employment = (school = "MIT")
  ),
  ( id = 456,
    name = "Bob",
    email = "bob@example.com",
    phones = [
      (number = "555-4567", type = home),
      (number = "555-7654", type = work)
    ],
    employment = (unemployed = void)
  )
] )
EOF

capnp encode "$ROOT/schema/calculator.capnp" EvaluateRequest \
  >"$FIX/calculator_add_2_3.bin" <<'EOF'
( expression = (
    call = (
      op = add,
      params = [
        (literal = 2.0),
        (literal = 3.0)
      ]
    )
  )
)
EOF

capnp encode "$ROOT/schema/calculator.capnp" EvaluateResponse \
  >"$FIX/calculator_value_5.bin" <<'EOF'
( value = 5.0 )
EOF

# Nested: (2 + 3) * 4
capnp encode "$ROOT/schema/calculator.capnp" EvaluateRequest \
  >"$FIX/calculator_mul_add.bin" <<'EOF'
( expression = (
    call = (
      op = multiply,
      params = [
        (call = (
          op = add,
          params = [
            (literal = 2.0),
            (literal = 3.0)
          ]
        )),
        (literal = 4.0)
      ]
    )
  )
)
EOF

echo "wrote fixtures under $FIX"
ls -la "$FIX"
