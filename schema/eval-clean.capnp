@0x8a2e1c4b5d6e7f80;
enum Color { red @0; green @1; blue @2; }
struct Point { x @0 :Int32 = 3; y @1 :Int32; }
struct U {
  union { none @0 :Void; n @1 :Int32; t @2 :Text; }
  extra @3 :Bool = true;
}
struct G { grp :group { a @0 :Int16; b @1 :Int16 = 9; } }
struct Defs {
  t @0 :Text = "def";
  s @1 :Point = (x = 8, y = 9);
  lst @2 :List(Int32) = [1, 2, 3];
  b @3 :Bool = true;
  e @4 :Color = blue;
}
struct Box(T) { value @0 :T; }
struct Kw { voidField @0 :Int32; interfaceField @1 :Text; structField @2 :Bool; }
const pi :Float64 = 3.141592653589793;
const min64 :Int64 = -9223372036854775808;
const maxu :UInt64 = 18446744073709551615;
const msg :Text = "a\"b\\c";
const raw :Data = 0x"00ff7f";
const col :Color = blue;
const pt :Point = (x = 1, y = -2);
const empty :Point = ();
const udef :U = ();
const uset :U = (n = 4);
const uvoid :U = (none = void);
const nest :List(List(Int32)) = [[1, -1], [], [2147483647]];
const flags :List(Bool) = [true, false, true, true, false, false, false, false, true];
const fnan :Float32 = nan;
const finf :Float32 = inf;
const fninf :Float64 = -inf;
const fneg0 :Float64 = -0.0;
const f32neg0 :Float32 = -0.0;
const defs :Defs = ();
const box :Box(Text) = (value = "hi");
const kw :Kw = (voidField = 1, interfaceField = "q", structField = true);
const g0 :G = (grp = (a = 1, b = 2));
const bigf :Float64 = 1.0000000000000002;
const tiny :Float64 = 1e-320;
const f32pi :Float32 = 3.141592653589793;
