@0xe4a1c0d19b77f305;

struct Inner {
  n @0 :Int32 = -3;
  label @1 :Text = "inner";
}

struct Bucket {
  name @0 :Text;
  scores @1 :List(Int32);
}

struct Conform {
  flag @0 :Bool = true;
  tiny @1 :Int8 = -5;
  wide @2 :UInt32 = 7;
  ratio @3 :Float64 = 1.5;
  title @4 :Text = "untitled";
  blob @5 :Data;
  inner @6 :Inner;
  buckets @7 :List(Bucket);
  matrix @8 :List(List(Text));

  union {
    quiet @9 :Void;
    note @10 :Text;
  }

  meta :group {
    rev @11 :UInt16 = 4;
    comment @12 :Text;
  }
}

const answer :UInt32 = 42;
const greeting :Text = "hello";
