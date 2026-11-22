@0xd1a2b3c4d5e6f702;

struct Node {
  next @0 :Node;
  v @1 :UInt32;
  items @2 :List(UInt64);
}
