/* List(Text) wire shape from encoding.html: C=6 pointer list. */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(c, m)                                                            \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "FAIL: %s\n", m);                                        \
      failures++;                                                              \
    }                                                                          \
  } while (0)

int main(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  const char *items[] = {"python3", "script.py"};
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t r, lp;
  const char *s0, *s1;
  size_t n0, n1;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "struct");
  CHECK(capnp_builder_set_list_text(&b, body.word, 0, 0, items, 2) == CAPNP_OK,
        "list text");
  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "ser");
  capnp_builder_free(&b);

  /* Pointer word of list at root body ptr 0: element size field must be C=6. */
  {
    /* Stream: 8-byte header + segment. Root pointer at first word of body. */
    const uint8_t *seg = flat + 8;
    uint64_t root_ptr = (uint64_t)seg[0] | ((uint64_t)seg[1] << 8) |
                        ((uint64_t)seg[2] << 16) | ((uint64_t)seg[3] << 24) |
                        ((uint64_t)seg[4] << 32) | ((uint64_t)seg[5] << 40) |
                        ((uint64_t)seg[6] << 48) | ((uint64_t)seg[7] << 56);
    /* Follow to list pointer at body: root is struct off 0, 0 data, 1 ptr →
     * body starts at word 1; list ptr is at word 1. */
    uint64_t list_ptr = (uint64_t)seg[8] | ((uint64_t)seg[9] << 8) |
                        ((uint64_t)seg[10] << 16) | ((uint64_t)seg[11] << 24) |
                        ((uint64_t)seg[12] << 32) | ((uint64_t)seg[13] << 40) |
                        ((uint64_t)seg[14] << 48) | ((uint64_t)seg[15] << 56);
    int esize = (int)((list_ptr >> 32) & 7);
    CHECK((root_ptr & 3) == 0, "root is struct");
    CHECK((list_ptr & 3) == 1, "list kind");
    CHECK(esize == 6, "List(Text) element size C=6 (pointer)");
    (void)root_ptr;
  }

  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "from_flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root read");
  CHECK(capnp_getp(&r, 0, &lp) == CAPNP_OK, "get list");
  CHECK(lp.kind == CAPNP_PK_LIST && lp.esize == CAPNP_SZ_PTR, "esize ptr");
  CHECK(capnp_list_len(&lp) == 2, "len 2");
  CHECK(capnp_list_get_text(&lp, 0, &s0, &n0) == CAPNP_OK, "t0");
  CHECK(n0 == 7 && memcmp(s0, "python3", 7) == 0, "python3");
  CHECK(capnp_list_get_text(&lp, 1, &s1, &n1) == CAPNP_OK, "t1");
  CHECK(n1 == 9 && memcmp(s1, "script.py", 9) == 0, "script.py");
  capnp_message_free(&m);

  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}
