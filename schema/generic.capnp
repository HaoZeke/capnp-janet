@0xb7c3d19e04a65f21;

struct Box(T) {
  value @0 :T;
}

struct Pair(T, U) {
  left @0 :T;
  right @1 :U;
}

struct Holder {
  textBox @0 :Box(Text);
  dataBox @1 :Box(Data);
  pair @2 :Pair(Text, Data);
}
