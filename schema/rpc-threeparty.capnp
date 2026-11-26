@0xd94d1d3f0d9f8a11;
# Third-party ids for Cap'n Proto RPC descriptors.
# Provide, Accept, and Join are answered with an Unimplemented echo.
#
# rpc.capnp leaves ProvisionId, RecipientId, ThirdPartyCapId and the join
# keys as AnyPointer on purpose: what a "vat" is, and how one vat names
# another, belongs to the network. rpc-twoparty.capnp answers that by
# declaring most of them empty, because a connection between exactly two
# vats has no third to name. This layer answers it differently.
#
# The structs below are the wire shapes of Provide.recipient, Accept.provision,
# and a thirdPartyHosted id. This vat answers the Provide, Accept, and Join
# messages with an Unimplemented echo.

struct VatId {
  # Where a vat can be reached. A network with a directory would carry an
  # opaque id instead; an address is what this one has.

  host @0 :Text;
  port @1 :UInt16;
}

struct ProvisionId {
  # Accept.provision: which pending Provide is being claimed.

  nonce @0 :UInt64;
}

struct RecipientId {
  # Provide.recipient: who the provider should expect an Accept from.

  vat @0 :VatId;
  nonce @1 :UInt64;
}

struct ThirdPartyCapId {
  # ThirdPartyCapDescriptor.id: how the recipient reaches the provider,
  # and which pending Provide to claim once there.

  vat @0 :VatId;
  nonce @1 :UInt64;
}

struct JoinKeyPart {
  # Level 4 over this network, same shape the two-party layer uses.

  joinId @0 :UInt32;
  partCount @1 :UInt16;
  partNum @2 :UInt16;
}

struct JoinResult {
  joinId @0 :UInt32;
  succeeded @1 :Bool;
  cap @2 :Capability;
}
