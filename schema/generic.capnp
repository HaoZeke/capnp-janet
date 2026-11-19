@0xb7c3d19e04a65f21;

annotation marked(struct, field) :Int32;

struct Box(T) {
  value @0 :T;
}

struct Pair(T, U) {
  left @0 :T;
  right @1 :U;
}

struct Holder $marked(7) {
  textBox @0 :Box(Text) $marked(3);
  dataBox @1 :Box(Data);
  pair @2 :Pair(Text, Data);
}
