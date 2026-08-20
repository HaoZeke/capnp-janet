@0xcbe7b6e5a402d913;

enum Tone {
  quiet @0;
  loud @1;
}

struct CodegenFeatures {
  enabled @0 :Bool = true;
  tiny @1 :Int8 = -7;
  small @2 :Int16 = -700;
  count @3 :Int32 = -70000;
  total @4 :Int64 = -700000;
  byte @5 :UInt8 = 250;
  words @6 :UInt16 = 65000;
  wide @7 :UInt32 = 4000000000;
  widest @8 :UInt64 = 9007199254740991;
  ratio32 @9 :Float32 = 1.5;
  ratio64 @10 :Float64 = 2.5;
  tone @11 :Tone = loud;

  union {
    none @12 :Void;
    number @13 :UInt32;
    detail :group {
      label @14 :Text;
    }
  }

  payload @15 :Data;
  child @16 :Child;
  children @17 :List(Child);
  huge @18 :UInt64 = 18446744073709551615;
  lowest @19 :Int64 = -9223372036854775808;
  flags @20 :List(Bool);
  scores @21 :List(Int32);
  samples @22 :List(Float32);
  names @23 :List(Text);
  empty @24 :List(Void);
  bigValues @25 :List(UInt64);
  tones @26 :List(Tone);

  struct Child {
    code @0 :UInt32;
    name @1 :Text;
  }
}
