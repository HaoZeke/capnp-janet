@0xc4e19ab07d35f602;

struct Point {
  x @0 :Int32 = 1;
  y @1 :Int32 = 2;
}

struct Wrap {
  origin @0 :Point = (x = 4, y = 5);
  labels @1 :List(Text) = ["a", "b"];
}
