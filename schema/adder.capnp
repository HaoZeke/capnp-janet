@0xbf5e831ac9f0d2a1;
# RPC interop test interface: served by capnp-C++ (interop/rpc_peer_server.c++),
# called by the C vat (interop/rpc_client.c). The explicit interface id is
# mirrored as ADDER_IFACE in the C client.

using Cxx = import "/capnp/c++.capnp";

interface Adder @0xea01e10cbc414411 {
  add @0 (a :Int64, b :Int64) -> (sum :Int64);
  # `n` bytes of 0xab, for a Return longer than any fixed buffer.
  echo @1 (n :UInt32) -> (data :Data);
  # This same capability, for a call pipelined on the answer.
  self @2 () -> (adder :Adder);
  # The sum `cb.add(20, 22)` returns, for a capability in the params.
  callBack @3 (cb :Adder) -> (sum :Int64);
  # A tail call to `target.relay(a, b)`.
  relay @4 (target :Adder, a :Int64, b :Int64) -> (sum :Int64);
  # Never returns; a Finish cancels it, and capnp-C++ answers canceled.
  hang @5 () -> () $Cxx.allowCancellation;
  # The sum `cb.self().add(1, 2)` returns, for a capability in results.
  callSelf @6 (cb :Adder) -> (sum :Int64);
}
