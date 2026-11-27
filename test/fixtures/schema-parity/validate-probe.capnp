# Source schema for validate-probe.bin.
#
# scripts/gen-validate-probe.sh compiles this file with capnp 1.4.0
# (`capnp compile -o-`) to a CodeGeneratorRequest, then writes the probe.
# That compile has 12 nodes: this file, plus the 11 types below. The
# probe omits the file node. It numbers the rest 256-266, sets each
# display name to probe.capnp:<name>, leaves scopeId 0, leaves ordinals
# implicit, leaves preferredListEncoding empty, then applies the mutation.
#
# Checks are SchemaLoader::Validator in capnp 1.4.0 schema-loader.c++.
#
# node            id    mutation and the check that rejects it
# file            --    Valid Node::FILE. Omitted from the probe.
#                       validate() case FILE calls verifyVoid.
# Keep            256   Valid control. No Validator failure.
# BadOrd          257   Explicit ordinals 2 then 1.
#                       ordinal.getExplicit() >= nextOrdinal,
#                       "fields were not ordered by ordinal".
# BadDisc         258   Both fields discriminantValue 0, slot offset 1,
#                       discriminantOffset 0, discriminantCount 2.
#                       field.getDiscriminantValue()
#                         < sawDiscriminantValue.size()
#                       && !sawDiscriminantValue[that],
#                       "invalid discriminantValue".
# BadValue        259   Int32 slot whose default Value tag is text.
#                       value.which() == expectedValueType,
#                       "Value did not match type." (12 == 4).
# NotGeneric      260   Parameter T kept, isGeneric left false.
#                       node.getIsGeneric(),
#                       "if parameter list is non-empty, isGeneric must be true".
# LoneGroup       261   isGroup set, scopeId 0.
#                       scopeId != 0, "group node missing scopeId".
# BadEnum         262   Enumerants a and b both codeOrder 0.
#                       enumerant.getCodeOrder() < enumerants.size()
#                       && !sawCodeOrder[enumerant.getCodeOrder()],
#                       "invalid codeOrder".
# HoldEnum        263   No mutation of this node. Field e typeId is 262.
#                       The enum check on 262 names enumerant b.
# BadEnumRange    264   Enumerant b codeOrder 5.
#                       Same codeOrder check; 5 is outside the list.
# HoldRange       265   No mutation of this node. Field e typeId is 264.
#                       The enum check on 264 names enumerant b.
# DiscOnPlain     266   discriminantValue 0 while discriminantCount is 0.
#                       Same discriminantValue check; the saw list length is 0.
@0x9a3c1e7b5d2046f8;

struct Keep {
  x @0 :Int32;
}

struct BadOrd {
  x @0 :Int32;
  y @1 :Int32;
}

struct BadDisc {
  union {
    a @0 :Int32;
    b @1 :Int32;
  }
}

struct BadValue {
  x @0 :Int32;
}

struct NotGeneric(T) {
  x @0 :Int32;
}

struct LoneGroup {
  x @0 :Int32;
}

enum BadEnum {
  a @0;
  b @1;
}

struct HoldEnum {
  e @0 :BadEnum;
}

enum BadEnumRange {
  a @0;
  b @1;
}

struct HoldRange {
  e @0 :BadEnumRange;
}

struct DiscOnPlain {
  x @0 :Int32;
}
