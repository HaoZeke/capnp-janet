@0xaaaaaaaaaaaaaaaa;
using Json = import "/capnp/compat/json.capnp";
struct Named {
  fieldName @0 :Int32 $Json.name("field_name");
  blob @1 :Data $Json.base64;
  hexb @2 :Data $Json.hex;
  flat :group $Json.flatten() {
    inner @3 :Int32;
  }
}
struct Disc {
  tag :union $Json.discriminator("which") {
    left @0 :Int32;
    right @1 :Text;
  }
}
const n0 :Named = (fieldName = 4, blob = 0x"6869", hexb = 0x"ab", flat = (inner = 9));
const d0 :Disc = (tag = (right = "z"));
const d1 :Disc = ();
