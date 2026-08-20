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

  choice :union {
    none @12 :Void;
    number @13 :UInt32;
    detail :group {
      label @14 :Text;
    }
  }
}
