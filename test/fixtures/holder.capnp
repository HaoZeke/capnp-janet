@0xd1a2b3c4d5e6f701;

interface Svc {
  ping @0 () -> ();
}

struct Holder {
  svc @0 :Svc;
  n @1 :UInt32;
}
