@0xa91c4e70b2d58f13;

struct Item {
  n @0 :Int32;
  name @1 :Text;
}

struct Box {
  items @0 :List(Item);
  nested @1 :List(List(Item));
  scores @2 :List(Int32);

  union {
    none @3 :Void;
    tag @4 :Text;
  }

  meta :group {
    rev @5 :UInt16;
    note @6 :Text;
  }
}

const sample :Box = (
  items = [(n = 1, name = "a"), (n = 2, name = "b")],
  nested = [[(n = 3, name = "c")], [(n = 4, name = "d"), (n = 5, name = "e")]],
  scores = [8, 9, 10],
  tag = "on",
  meta = (rev = 9, note = "z")
);
