#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", msg);                                      \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static void test_roundtrip_demo(void) {
  capnp_builder_t b;
  capnp_builder_init(&b);
  capnp_bptr_t root, body;
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 1, 2, &body) == CAPNP_OK, "struct");
  CHECK(capnp_builder_set_u32(&body, 0, 42) == CAPNP_OK, "u32");
  CHECK(capnp_builder_set_text(&body, 1, 0, "hello", 5) == CAPNP_OK,
        "text");
  const char *items[] = {"a", "bb", "ccc"};
  CHECK(capnp_builder_set_list_text(&body, 1, 1, items, 3) == CAPNP_OK,
        "list text");

  uint8_t *flat = NULL;
  size_t flat_len = 0;
  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "serialize");
  capnp_builder_free(&b);

  capnp_message_t m;
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "from_flat");
  free(flat);

  capnp_ptr_t r;
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root read");
  CHECK(r.kind == CAPNP_PK_STRUCT, "root is struct");
  CHECK(capnp_get_u32(&r, 0, 0) == 42, "value 42");

  const char *name = NULL;
  size_t nlen = 0;
  CHECK(capnp_get_text(&r, 0, &name, &nlen) == CAPNP_OK, "get text");
  CHECK(nlen == 5 && memcmp(name, "hello", 5) == 0, "name hello");

  capnp_ptr_t list;
  CHECK(capnp_getp(&r, 1, &list) == CAPNP_OK, "get list ptr");
  CHECK(list.kind == CAPNP_PK_LIST, "is list");
  CHECK(capnp_list_len(&list) == 3, "len 3");

  const char *e0, *e1, *e2;
  size_t l0, l1, l2;
  CHECK(capnp_list_get_text(&list, 0, &e0, &l0) == CAPNP_OK && l0 == 1 &&
            e0[0] == 'a',
        "item0");
  CHECK(capnp_list_get_text(&list, 1, &e1, &l1) == CAPNP_OK && l1 == 2 &&
            memcmp(e1, "bb", 2) == 0,
        "item1");
  CHECK(capnp_list_get_text(&list, 2, &e2, &l2) == CAPNP_OK && l2 == 3 &&
            memcmp(e2, "ccc", 3) == 0,
        "item2");

  capnp_message_free(&m);
}

static void test_view_flat(void) {
  capnp_builder_t b;
  capnp_builder_init(&b);
  capnp_bptr_t root, body;
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 1, 1, &body);
  capnp_builder_set_u32(&body, 0, 7);
  capnp_builder_set_text(&body, 1, 0, "x", 1);
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_builder_serialize(&b, &flat, &flat_len);
  capnp_builder_free(&b);

  capnp_message_t m;
  CHECK(capnp_message_view_flat(&m, flat, flat_len) == CAPNP_OK, "view");
  capnp_ptr_t r;
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_get_u32(&r, 0, 0) == 7, "7");
  capnp_message_free(&m);
  free(flat);
}

static void test_composite_list(void) {
  /*
   * Outer { probes @0 :List(Probe) }
   * Probe { exists @0 :Bool; name @0 :Text }  — 1 data word, 1 pointer
   */
  capnp_builder_t b;
  capnp_builder_init(&b);
  capnp_bptr_t root, body;
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "outer");
  capnp_bptr_t first;
  CHECK(capnp_builder_set_list_struct(&body, 0, 0, 2, 1, 1, &first) ==
            CAPNP_OK,
        "list struct");
  /* elem0: exists=1, name=aa */
  CHECK(capnp_builder_set_bool(&first, 0, 1) == CAPNP_OK, "bool0");
  CHECK(capnp_builder_set_text(&first, 1, 0, "aa", 2) == CAPNP_OK, "t0");
  capnp_bptr_t e1 = capnp_bptr_add(first, 2);
  CHECK(capnp_builder_set_bool(&e1, 0, 0) == CAPNP_OK, "bool1");
  CHECK(capnp_builder_set_text(&e1, 1, 0, "b", 1) == CAPNP_OK, "t1");

  uint8_t *flat = NULL;
  size_t flat_len = 0;
  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "ser");
  capnp_builder_free(&b);

  capnp_message_t m;
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "flat");
  free(flat);
  capnp_ptr_t r, list, e0, e1p;
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root r");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "get list");
  CHECK(list.kind == CAPNP_PK_LIST && list.esize == CAPNP_SZ_COMPOSITE, "comp");
  CHECK(capnp_list_len(&list) == 2, "len2");
  CHECK(capnp_list_getp(&list, 0, &e0) == CAPNP_OK, "e0");
  CHECK(capnp_get_bool(&e0, 0, 0) == 1, "exists0");
  const char *n0;
  size_t l0;
  CHECK(capnp_get_text(&e0, 0, &n0, &l0) == CAPNP_OK && l0 == 2, "name0");
  CHECK(capnp_list_getp(&list, 1, &e1p) == CAPNP_OK, "e1");
  CHECK(capnp_get_bool(&e1p, 0, 1) == 0, "exists1");
  capnp_message_free(&m);
}

static void test_schema_evolution_default(void) {
  /* Struct with 0 data words: get_u32 past end returns default. */
  capnp_builder_t b;
  capnp_builder_init(&b);
  capnp_bptr_t root, body;
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 0, &body);
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_builder_serialize(&b, &flat, &flat_len);
  capnp_builder_free(&b);

  CHECK(flat_len >= 16, "empty framed size");
  CHECK(flat[8] == 0xfc && flat[9] == 0xff && flat[10] == 0xff &&
            flat[11] == 0xff && flat[12] == 0 && flat[13] == 0 &&
            flat[14] == 0 && flat[15] == 0,
        "empty struct B=-1");

  capnp_message_t m;
  capnp_message_from_flat(&m, flat, flat_len);
  free(flat);
  capnp_ptr_t r;
  capnp_root(&m, &r);
  CHECK(capnp_get_u32(&r, 0, 99) == 99, "default past end");
  capnp_message_free(&m);
}

static void test_narrow_scalar_builders(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  capnp_message_t m;
  capnp_ptr_t r;
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  uint32_t f32_bits;
  float f32 = 1.5f;

  memcpy(&f32_bits, &f32, sizeof(f32_bits));
  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "narrow root");
  CHECK(capnp_builder_struct(&root, 1, 0, &body) == CAPNP_OK,
        "narrow struct");
  CHECK(capnp_builder_set_u8(&body, 0, UINT8_C(0xab)) == CAPNP_OK,
        "set u8");
  CHECK(capnp_builder_set_f32(&body, 4, f32) == CAPNP_OK, "set f32");
  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK,
        "narrow serialize");
  capnp_builder_free(&b);

  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK,
        "narrow parse");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "narrow read root");
  CHECK(capnp_get_u8(&r, 0, 0) == UINT8_C(0xab), "u8 round-trip");
  CHECK(capnp_get_u32(&r, 4, 0) == f32_bits, "f32 bits round-trip");
  capnp_message_free(&m);
}

int main(void) {
  test_roundtrip_demo();
  test_view_flat();
  test_composite_list();
  test_schema_evolution_default();
  test_narrow_scalar_builders();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
