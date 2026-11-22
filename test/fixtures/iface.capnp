@0xd3a8f2b1c4e5f601;
# Interface inheritance, including a diamond and a shadowed method name.

interface Base { ping @0 () -> (); }
interface Mid extends(Base) { mid @0 (x :UInt32) -> (y :UInt32); }
interface Other extends(Base) { other @0 () -> (); }
interface Derived extends(Mid, Other) {
  pong @0 () -> ();
  ping @1 () -> ();
}
