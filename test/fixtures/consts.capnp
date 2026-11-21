@0xc4f1a2b3d4e5f607;

struct Point {
  x @0 :Int32;
  y @1 :Int32;
  label @2 :Text;
}

const kPoint :Point = (x = 77, y = -88, label = "pt");
const kInts :List(Int32) = [1, -2, 3];
const kWords :List(Text) = ["a", "bc"];
const kPoints :List(Point) = [(x = 1), (y = 2, label = "q")];
const kEmpty :List(UInt8) = [];
