#include "capnp_builder.h"
#include "capnp_message.h"

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
  CHECK(capnp_builder_set_u32(&b, body.word, 0, 42) == CAPNP_OK, "u32");
  CHECK(capnp_builder_set_text(&b, body.word, 1, 0, "hello", 5) == CAPNP_OK,
        "text");
  const char *items[] = {"a", "bb", "ccc"};
  CHECK(capnp_builder_set_list_text(&b, body.word, 1, 1, items, 3) == CAPNP_OK,
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
  capnp_builder_set_u32(&b, body.word, 0, 7);
  capnp_builder_set_text(&b, body.word, 1, 0, "x", 1);
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

  capnp_message_t m;
  capnp_message_from_flat(&m, flat, flat_len);
  free(flat);
  capnp_ptr_t r;
  capnp_root(&m, &r);
  CHECK(capnp_get_u32(&r, 0, 99) == 99, "default past end");
  capnp_message_free(&m);
}

int main(void) {
  test_roundtrip_demo();
  test_view_flat();
  test_schema_evolution_default();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
