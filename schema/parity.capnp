@0xa71e92c34b508d61;

using Json = import "/capnp/compat/json.capnp";

enum Color {
  red @0;
  green @1 $Json.name("emerald");
  blue @2;
}

struct Point {
  x @0 :Int32 = 10;
  y @1 :Int32 = -20;
}

struct Box(T) {
  value @0 :T;
}

struct Pair(A, B) {
  left @0 :A;
  right @1 :B;
}

struct FlatSub {
  subName @0 :Text;
  subCode @1 :UInt16 = 7;
}

struct DiscSub $Json.discriminator(name = "kind") {
  union {
    num @0 :UInt32;
    flat @1 :FlatSub $Json.flatten();
  }
}

struct Parity {
  vVoid @0 :Void;
  vBool @1 :Bool = true;
  vI8 @2 :Int8 = -5;
  vI16 @3 :Int16 = -500;
  vI32 @4 :Int32 = -50000;
  vI64 @5 :Int64 = -9000000000000;
  vU8 @6 :UInt8 = 200;
  vU16 @7 :UInt16 = 60000;
  vU32 @8 :UInt32 = 4000000000;
  vU64 @9 :UInt64 = 18446744073709551615;
  vF32 @10 :Float32 = 1.5;
  vF64 @11 :Float64 = -2.25;
  vText @12 :Text = "default-text";
  vData @13 :Data = 0x"41 42";
  vEnum @14 :Color = green;
  vStruct @15 :Point = (x = 3, y = 4);

  lVoid @16 :List(Void);
  lBool @17 :List(Bool);
  lI8 @18 :List(Int8);
  lI16 @19 :List(Int16);
  lI32 @20 :List(Int32) = [9, 8];
  lI64 @21 :List(Int64);
  lU8 @22 :List(UInt8);
  lU16 @23 :List(UInt16);
  lU32 @24 :List(UInt32);
  lU64 @25 :List(UInt64);
  lF32 @26 :List(Float32);
  lF64 @27 :List(Float64);
  lText @28 :List(Text);
  lData @29 :List(Data);
  lEnum @30 :List(Color);
  lStruct @31 :List(Point);
  lNested @32 :List(List(Point));

  renamed @33 :Text $Json.name("custom_key");
  b64Data @34 :Data $Json.base64;
  hexData @35 :Data $Json.hex;

  flatGroup :group $Json.flatten(prefix = "fg.") {
    alpha @36 :UInt32;
    beta @37 :Text $Json.name("beta_key");
  }

  discUnion :union $Json.flatten() $Json.discriminator(name = "u_tag") {
    optA :group $Json.flatten() {
      aVal @38 :Text;
    }
    optB :group $Json.flatten() $Json.name("b_tag") {
      bNum @39 :UInt32;
    }
  }

  valUnion :union $Json.flatten() $Json.discriminator(valueName = "v_payload") {
    vTxt @40 :Text;
    vNum @41 :UInt32 $Json.name("num_arm");
  }

  voidUnion :union $Json.discriminator(name = "type") {
    none @42 :Void;
    some @43 :UInt32;
  }

  extDisc @44 :DiscSub;
  rawJson @45 :Json.Value;

  boxedText @46 :Box(Text);
  paired @47 :Pair(Data, List(Int32));
  nestedBox @48 :Box(Box(Point));

  union {
    uQuiet @49 :Void;
    uNote @50 :Text;
    uCount @51 :UInt64;
  }
}

const kColor :Color = blue;
const kBig :UInt64 = 18446744073709551615;
const kPoint :Point = (x = 77, y = -88);
