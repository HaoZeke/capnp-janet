/* Read a capnp 1.4.0 CodeGeneratorRequest for validate-probe.capnp and
 * write validate-probe.bin. The node list and each mutation are in the
 * comment block of that schema. Builder order matches the checked-in
 * bytes: NotGeneric inits its parameter list and the name T before the
 * struct body. preferredListEncoding stays unset, so the wire value is
 * empty.
 */
#include <capnp/message.h>
#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>

#include <kj/io.h>

#include <fcntl.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace schema = capnp::schema;

static void die(const char *msg) {
  fprintf(stderr, "gen-validate-probe: %s\n", msg);
  exit(1);
}

static std::string textOf(capnp::Text::Reader t) {
  return std::string(t.cStr(), t.size());
}

static std::string shortName(capnp::Text::Reader t) {
  std::string s = textOf(t);
  std::string::size_type p = s.rfind(':');
  if (p == std::string::npos)
    return s;
  return s.substr(p + 1);
}

static schema::Node::Reader need(schema::CodeGeneratorRequest::Reader req,
                                 const char *name) {
  int hits = 0;
  schema::Node::Reader found;
  for (auto n : req.getNodes()) {
    if (shortName(n.getDisplayName()) == name) {
      hits++;
      found = n;
    }
  }
  if (hits != 1)
    die(name);
  return found;
}

static void needInt(schema::Field::Reader f, const char *name) {
  if (textOf(f.getName()) != name || !f.isSlot() ||
      !f.getSlot().getType().isInt32())
    die(name);
}

static void needEnumPair(schema::Node::Reader n, const char *name) {
  if (!n.isEnum())
    die(name);
  auto es = n.getEnum().getEnumerants();
  if (es.size() != 2 || textOf(es[0].getName()) != "a" ||
      es[0].getCodeOrder() != 0 || textOf(es[1].getName()) != "b" ||
      es[1].getCodeOrder() != 1)
    die(name);
}

static void check(schema::CodeGeneratorRequest::Reader req) {
  static const char *names[] = {
      "Keep",     "BadOrd",      "BadDisc",    "BadValue",
      "NotGeneric", "LoneGroup", "BadEnum",    "HoldEnum",
      "BadEnumRange", "HoldRange", "DiscOnPlain"};
  int files = 0, count = 0;
  for (auto n : req.getNodes()) {
    count++;
    if (n.isFile())
      files++;
  }
  /* File node plus the 11 types. */
  if (count != 12 || files != 1)
    die("compiled request node count");
  for (const char *name : names)
    need(req, name);

  auto keep = need(req, "Keep");
  auto fields = keep.getStruct().getFields();
  if (!keep.isStruct() || fields.size() != 1)
    die("Keep");
  needInt(fields[0], "x");

  auto badOrd = need(req, "BadOrd");
  fields = badOrd.getStruct().getFields();
  if (fields.size() != 2)
    die("BadOrd");
  needInt(fields[0], "x");
  needInt(fields[1], "y");
  if (!fields[0].getOrdinal().isExplicit() ||
      fields[0].getOrdinal().getExplicit() != 0 ||
      !fields[1].getOrdinal().isExplicit() ||
      fields[1].getOrdinal().getExplicit() != 1)
    die("BadOrd ordinals");

  auto badDisc = need(req, "BadDisc");
  fields = badDisc.getStruct().getFields();
  if (badDisc.getStruct().getDiscriminantCount() != 2 || fields.size() != 2)
    die("BadDisc");
  needInt(fields[0], "a");
  needInt(fields[1], "b");
  if (fields[0].getDiscriminantValue() != 0 ||
      fields[1].getDiscriminantValue() != 1)
    die("BadDisc discriminants");

  auto badValue = need(req, "BadValue");
  fields = badValue.getStruct().getFields();
  if (fields.size() != 1)
    die("BadValue");
  needInt(fields[0], "x");
  if (!fields[0].getSlot().getDefaultValue().isInt32())
    die("BadValue default");

  auto notGeneric = need(req, "NotGeneric");
  auto params = notGeneric.getParameters();
  fields = notGeneric.getStruct().getFields();
  if (!notGeneric.getIsGeneric() || params.size() != 1 ||
      textOf(params[0].getName()) != "T" || fields.size() != 1)
    die("NotGeneric");
  needInt(fields[0], "x");

  auto lone = need(req, "LoneGroup");
  fields = lone.getStruct().getFields();
  if (lone.getStruct().getIsGroup() || fields.size() != 1)
    die("LoneGroup");
  needInt(fields[0], "x");

  auto badEnum = need(req, "BadEnum");
  auto badRange = need(req, "BadEnumRange");
  needEnumPair(badEnum, "BadEnum");
  needEnumPair(badRange, "BadEnumRange");

  auto holdEnum = need(req, "HoldEnum");
  fields = holdEnum.getStruct().getFields();
  if (fields.size() != 1 || textOf(fields[0].getName()) != "e" ||
      !fields[0].getSlot().getType().isEnum() ||
      fields[0].getSlot().getType().getEnum().getTypeId() != badEnum.getId())
    die("HoldEnum");

  auto holdRange = need(req, "HoldRange");
  fields = holdRange.getStruct().getFields();
  if (fields.size() != 1 || textOf(fields[0].getName()) != "e" ||
      !fields[0].getSlot().getType().isEnum() ||
      fields[0].getSlot().getType().getEnum().getTypeId() != badRange.getId())
    die("HoldRange");

  auto plain = need(req, "DiscOnPlain");
  fields = plain.getStruct().getFields();
  if (fields.size() != 1 ||
      fields[0].getDiscriminantValue() != schema::Field::NO_DISCRIMINANT)
    die("DiscOnPlain");
  needInt(fields[0], "x");
}

static void intSlot(schema::Field::Builder f, capnp::Text::Reader name,
                    uint16_t code, uint32_t offset) {
  f.setName(name);
  f.setCodeOrder(code);
  auto slot = f.initSlot();
  slot.setOffset(offset);
  slot.initType().setInt32();
  slot.initDefaultValue().setInt32(0);
}

static schema::Node::Struct::Builder openStruct(schema::Node::Builder n,
                                               uint64_t id, const char *name) {
  n.setId(id);
  n.setDisplayName(name);
  auto st = n.initStruct();
  st.setDataWordCount(1);
  st.setPointerCount(0);
  return st;
}

static void emit(schema::CodeGeneratorRequest::Reader req) {
  auto keep = need(req, "Keep");
  auto badOrd = need(req, "BadOrd");
  auto badDisc = need(req, "BadDisc");
  auto badValue = need(req, "BadValue");
  auto notGeneric = need(req, "NotGeneric");
  auto lone = need(req, "LoneGroup");
  auto badEnum = need(req, "BadEnum");
  auto holdEnum = need(req, "HoldEnum");
  auto badRange = need(req, "BadEnumRange");
  auto holdRange = need(req, "HoldRange");
  auto plain = need(req, "DiscOnPlain");

  capnp::MallocMessageBuilder message;
  auto out = message.initRoot<schema::CodeGeneratorRequest>();
  auto nodes = out.initNodes(11);

  {
    auto st = openStruct(nodes[0], 256, "probe.capnp:Keep");
    intSlot(st.initFields(1)[0], keep.getStruct().getFields()[0].getName(), 0,
            0);
  }
  {
    auto src = badOrd.getStruct().getFields();
    auto st = openStruct(nodes[1], 257, "probe.capnp:BadOrd");
    auto fields = st.initFields(2);
    intSlot(fields[0], src[0].getName(), 0, 0);
    fields[0].initOrdinal().setExplicit(2);
    intSlot(fields[1], src[1].getName(), 1, 1);
    fields[1].initOrdinal().setExplicit(1);
  }
  {
    auto src = badDisc.getStruct().getFields();
    auto st = openStruct(nodes[2], 258, "probe.capnp:BadDisc");
    st.setDiscriminantCount(2);
    auto fields = st.initFields(2);
    intSlot(fields[0], src[0].getName(), 0, 1);
    fields[0].setDiscriminantValue(0);
    intSlot(fields[1], src[1].getName(), 1, 1);
    fields[1].setDiscriminantValue(0);
  }
  {
    auto src = badValue.getStruct().getFields()[0];
    auto st = openStruct(nodes[3], 259, "probe.capnp:BadValue");
    auto f = st.initFields(1)[0];
    f.setName(src.getName());
    f.setCodeOrder(0);
    auto slot = f.initSlot();
    slot.setOffset(0);
    slot.initType().setInt32();
    slot.initDefaultValue().setText("nope");
  }
  {
    auto n = nodes[4];
    n.setId(260);
    n.setDisplayName("probe.capnp:NotGeneric");
    auto params = n.initParameters(1);
    params[0].setName(notGeneric.getParameters()[0].getName());
    auto st = n.initStruct();
    st.setDataWordCount(1);
    st.setPointerCount(0);
    intSlot(st.initFields(1)[0],
            notGeneric.getStruct().getFields()[0].getName(), 0, 0);
  }
  {
    auto st = openStruct(nodes[5], 261, "probe.capnp:LoneGroup");
    st.setIsGroup(true);
    intSlot(st.initFields(1)[0], lone.getStruct().getFields()[0].getName(), 0,
            0);
  }
  {
    auto src = badEnum.getEnum().getEnumerants();
    auto n = nodes[6];
    n.setId(262);
    n.setDisplayName("probe.capnp:BadEnum");
    auto es = n.initEnum().initEnumerants(2);
    es[0].setName(src[0].getName());
    es[0].setCodeOrder(0);
    es[1].setName(src[1].getName());
    es[1].setCodeOrder(0);
  }
  {
    auto st = openStruct(nodes[7], 263, "probe.capnp:HoldEnum");
    auto f = st.initFields(1)[0];
    f.setName(holdEnum.getStruct().getFields()[0].getName());
    f.setCodeOrder(0);
    auto slot = f.initSlot();
    slot.setOffset(0);
    slot.initType().initEnum().setTypeId(262);
    slot.initDefaultValue().setEnum(0);
  }
  {
    auto src = badRange.getEnum().getEnumerants();
    auto n = nodes[8];
    n.setId(264);
    n.setDisplayName("probe.capnp:BadEnumRange");
    auto es = n.initEnum().initEnumerants(2);
    es[0].setName(src[0].getName());
    es[0].setCodeOrder(0);
    es[1].setName(src[1].getName());
    es[1].setCodeOrder(5);
  }
  {
    auto st = openStruct(nodes[9], 265, "probe.capnp:HoldRange");
    auto f = st.initFields(1)[0];
    f.setName(holdRange.getStruct().getFields()[0].getName());
    f.setCodeOrder(0);
    auto slot = f.initSlot();
    slot.setOffset(0);
    slot.initType().initEnum().setTypeId(264);
    slot.initDefaultValue().setEnum(0);
  }
  {
    auto st = openStruct(nodes[10], 266, "probe.capnp:DiscOnPlain");
    auto f = st.initFields(1)[0];
    f.setName(plain.getStruct().getFields()[0].getName());
    f.setCodeOrder(0);
    f.setDiscriminantValue(0);
    auto slot = f.initSlot();
    slot.setOffset(0);
    slot.initType().setInt32();
    slot.initDefaultValue().setInt32(0);
  }

  kj::FdOutputStream outFd(1);
  capnp::writeMessage(outFd, message);
}

int main(int argc, char **argv) {
  if (argc != 2)
    die("usage: gen-validate-probe request.bin");
  kj::AutoCloseFd fd(open(argv[1], O_RDONLY));
  if (fd.get() < 0)
    die("open request");
  capnp::StreamFdMessageReader reader(kj::mv(fd));
  auto req = reader.getRoot<schema::CodeGeneratorRequest>();
  check(req);
  emit(req);
  return 0;
}
