#include <capnp-janet/capnp_dyn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void fail(const char *name, const char *why) {
  fprintf(stderr, "%s FAIL %s\n", name, why);
  g_fail = 1;
}

static int read_file(const char *path, uint8_t **out, size_t *n) {
  FILE *f = fopen(path, "rb");
  long sz;
  uint8_t *buf;
  if (!f)
    return -1;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return -1;
  }
  rewind(f);
  buf = (uint8_t *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return -1;
  }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  *out = buf;
  *n = (size_t)sz;
  return 0;
}

static int load_dyn(const char *path, capnp_dyn_t **out) {
  uint8_t *buf = NULL;
  size_t n = 0;
  int rc;
  *out = NULL;
  if (read_file(path, &buf, &n))
    return -1;
  rc = capnp_dyn_init(out, buf, n);
  free(buf);
  return rc;
}

static int decode(capnp_dyn_t *d, const char *type, const char *text, int *rc,
                  const char **err) {
  uint8_t *msg = NULL;
  size_t mn = 0;
  *rc = capnp_dyn_decode_text(d, type, text, strlen(text), &msg, &mn);
  free(msg);
  *err = capnp_dyn_error(d);
  if (!*err)
    *err = "";
  return 0;
}

static int unknown_type(int rc, const char *err) {
  return rc != 0 && err && strstr(err, "unknown type") != NULL;
}

static void expect_ok(const char *name, int rc, const char *err) {
  if (rc != 0)
    fail(name, err && err[0] ? err : "decode failed");
}

static void expect_unknown(const char *name, int rc, const char *err) {
  if (!unknown_type(rc, err))
    fail(name, err && err[0] ? err : "node still loaded");
}

static void join_path(char *dst, size_t n, const char *dir, const char *file) {
  snprintf(dst, n, "%s/%s", dir, file);
}

/* G1: wrapped discriminant offset loads, and SlotS still decodes. */
static void check_g1(const char *dir) {
  char path[512];
  capnp_dyn_t *d = NULL;
  int rc;
  const char *err = "";
  join_path(path, sizeof(path), dir, "g1-disc.bin");
  rc = load_dyn(path, &d);
  if (rc != 0) {
    fail("g1", "init rejected wrapped discriminant");
    capnp_dyn_free(d);
    return;
  }
  decode(d, "SlotS", "(x = 1, y = 2)", &rc, &err);
  expect_ok("g1 slots", rc, err);
  decode(d, "UnionU", "(a = 1)", &rc, &err);
  if (unknown_type(rc, err))
    fail("g1", "UnionU absent");
  capnp_dyn_free(d);
}

/* G2: one-member union is absent. SlotS still decodes. */
static void check_g2(const char *dir) {
  char path[512];
  capnp_dyn_t *d = NULL;
  int rc;
  const char *err = "";
  join_path(path, sizeof(path), dir, "g2-count.bin");
  rc = load_dyn(path, &d);
  if (rc != 0) {
    fail("g2", "init failed");
    capnp_dyn_free(d);
    return;
  }
  decode(d, "UnionU", "(a = 1)", &rc, &err);
  expect_unknown("g2 union", rc, err);
  decode(d, "SlotS", "(x = 1, y = 2)", &rc, &err);
  expect_ok("g2 slots", rc, err);
  capnp_dyn_free(d);
}

/* G3: duplicate field names drop SlotS. UnionU still decodes. */
static void check_g3(const char *dir) {
  char path[512];
  capnp_dyn_t *d = NULL;
  int rc;
  const char *err = "";
  join_path(path, sizeof(path), dir, "g3-dup.bin");
  rc = load_dyn(path, &d);
  if (rc != 0) {
    fail("g3", "init failed");
    capnp_dyn_free(d);
    return;
  }
  decode(d, "SlotS", "(x = 1)", &rc, &err);
  expect_unknown("g3 slots", rc, err);
  decode(d, "UnionU", "(a = 1)", &rc, &err);
  expect_ok("g3 union", rc, err);
  capnp_dyn_free(d);
}

/* G4: codeOrder 9 drops SlotS. UnionU still decodes. */
static void check_g4(const char *dir) {
  char path[512];
  capnp_dyn_t *d = NULL;
  int rc;
  const char *err = "";
  join_path(path, sizeof(path), dir, "g4-codeorder.bin");
  rc = load_dyn(path, &d);
  if (rc != 0) {
    fail("g4", "init failed");
    capnp_dyn_free(d);
    return;
  }
  decode(d, "SlotS", "(x = 1, y = 2)", &rc, &err);
  expect_unknown("g4 slots", rc, err);
  decode(d, "UnionU", "(a = 1)", &rc, &err);
  expect_ok("g4 union", rc, err);
  capnp_dyn_free(d);
}

/* G5: slot offset 100 drops SlotS at load. */
static void check_g5(const char *dir) {
  char path[512];
  capnp_dyn_t *d = NULL;
  int rc;
  const char *err = "";
  join_path(path, sizeof(path), dir, "g5-oob.bin");
  rc = load_dyn(path, &d);
  if (rc != 0) {
    fail("g5", "init failed");
    capnp_dyn_free(d);
    return;
  }
  decode(d, "SlotS", "(x = 1, y = 2)", &rc, &err);
  expect_unknown("g5 slots", rc, err);
  decode(d, "UnionU", "(a = 1)", &rc, &err);
  expect_ok("g5 union", rc, err);
  capnp_dyn_free(d);
}

/* G6: duplicate enumerant names drop Hue. SlotS still decodes. */
static void check_g6(const char *dir) {
  char path[512];
  capnp_dyn_t *d = NULL;
  int rc;
  const char *err = "";
  join_path(path, sizeof(path), dir, "g6-enumdup.bin");
  rc = load_dyn(path, &d);
  if (rc != 0) {
    fail("g6", "init failed");
    capnp_dyn_free(d);
    return;
  }
  decode(d, "Just", "(e = cyan)", &rc, &err);
  if (rc == 0)
    fail("g6", "Just decoded with duplicate enumerants");
  decode(d, "SlotS", "(x = 1, y = 2)", &rc, &err);
  expect_ok("g6 slots", rc, err);
  capnp_dyn_free(d);
}

/* G10: wrapped field offset loads, and decode reports bad value. */
static void check_g10(const char *dir) {
  char path[512];
  capnp_dyn_t *d = NULL;
  int rc;
  const char *err = "";
  join_path(path, sizeof(path), dir, "g10-offset.bin");
  rc = load_dyn(path, &d);
  if (rc != 0) {
    fail("g10", "init rejected wrapped field offset");
    capnp_dyn_free(d);
    return;
  }
  decode(d, "UnionU", "(a = 1)", &rc, &err);
  expect_ok("g10 union", rc, err);
  decode(d, "SlotS", "(x = 1, y = 2)", &rc, &err);
  if (rc != CAPNP_ERR_KIND || !err || strcmp(err, "bad value: x") != 0)
    fail("g10", err && err[0] ? err : "missing bad value");
  capnp_dyn_free(d);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "test/fixtures/schema-parity";
  check_g1(dir);
  check_g2(dir);
  check_g3(dir);
  check_g4(dir);
  check_g5(dir);
  check_g6(dir);
  check_g10(dir);
  if (g_fail) {
    fprintf(stderr, "schema parity failed\n");
    return 1;
  }
  printf("schema parity ok\n");
  return 0;
}
