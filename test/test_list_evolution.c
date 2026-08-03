/* SPDX-License-Identifier: MIT
 *
 * capnp-janet-mm9l: List(Bool) bit-list + List(Void) length.
 * capnp-janet-oymb: schema-evolution list upgrade/downgrade views
 * (shapes from capnp-fortran test_parity.f90 t_list_upgrade/downgrade_views).
 */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_list_bool_roundtrip(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t bits[10] = {1, 0, 1, 0, 1, 0, 0, 0, 0, 1};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list;
  uint32_t i;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_bool(&b, body.word, 0, 0, bits, 10) == CAPNP_OK,
        "set list bool");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  CHECK(list.kind == CAPNP_PK_LIST && list.esize == CAPNP_SZ_BIT, "esize bit");
  CHECK_EQ_U32(capnp_list_len(&list), 10, "len");
  for (i = 0; i < 10; i++) {
    int got = capnp_list_get_bool(&list, i, -1);
    CHECK(got == (bits[i] ? 1 : 0), "bit element");
  }
  CHECK(capnp_list_get_bool(&list, 9, 0) == 1, "last bit");
  CHECK(capnp_list_get_bool(&list, 10, 7) == 7, "oob default");
  capnp_message_free(&m);
}

static void test_list_void_length(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list;
  capnp_ptr_t el;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_void(&b, body.word, 0, 0, 42) == CAPNP_OK,
        "set void list");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  CHECK(list.kind == CAPNP_PK_LIST && list.esize == CAPNP_SZ_VOID, "esize void");
  CHECK_EQ_U32(capnp_list_len(&list), 42, "void len");
  CHECK(capnp_list_get_struct(&list, 0, &el) == CAPNP_ERR_KIND,
        "void cannot upgrade");
  CHECK_EQ_U32(capnp_list_get_u32(&list, 0, 99), 99, "void no u32");
  capnp_message_free(&m);
}

static void test_list_void_empty(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_void(&b, body.word, 0, 0, 0) == CAPNP_OK,
        "empty void");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  CHECK_EQ_U32(capnp_list_len(&list), 0, "empty len");
  capnp_message_free(&m);
}

static void test_list_upgrade_prim(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint32_t items[] = {100, 200};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list, el;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_u32(&b, body.word, 0, 0, items, 2) == CAPNP_OK,
        "set u32 list");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");

  CHECK(capnp_list_get_struct(&list, 1, &el) == CAPNP_OK, "upgrade view");
  CHECK(el.kind == CAPNP_PK_STRUCT, "upgrade is struct");
  CHECK_EQ_U32(capnp_get_u32(&el, 0, 0), 200, "field @0 value");
  CHECK_EQ_U64(capnp_get_u64(&el, 0, 5), 5, "oversize -> default");
  CHECK(capnp_list_get_struct(&list, 0, &el) == CAPNP_OK, "upgrade e0");
  CHECK_EQ_U32(capnp_get_u32(&el, 0, 0), 100, "e0 field @0");
  capnp_message_free(&m);
}

static void test_list_upgrade_ptr(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  const char *items[] = {"elem"};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list, el;
  const char *s;
  size_t n;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_text(&b, body.word, 0, 0, items, 1) == CAPNP_OK,
        "set List(Text)");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  CHECK(capnp_list_get_struct(&list, 0, &el) == CAPNP_OK, "ptr upgrade");
  CHECK(el.kind == CAPNP_PK_STRUCT && el.pwords == 1 && el.dwords == 0,
        "0-data 1-ptr view");
  CHECK(capnp_get_text(&el, 0, &s, &n) == CAPNP_OK, "get text @0");
  CHECK_STREQ(s, n, "elem", "ptr view field @0");
  capnp_message_free(&m);
}

static void test_list_upgrade_bit_refused(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t bits[] = {1, 0, 1};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list, el;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  capnp_builder_set_list_bool(&b, body.word, 0, 0, bits, 3);
  capnp_builder_serialize(&b, &flat, &flen);
  capnp_builder_free(&b);
  capnp_message_from_flat(&m, flat, flen);
  free(flat);
  capnp_root(&m, &r);
  capnp_getp(&r, 0, &list);
  CHECK(capnp_list_get_struct(&list, 0, &el) == CAPNP_ERR_KIND,
        "bit list no upgrade");
  capnp_message_free(&m);
}

static void test_list_downgrade_composite(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  size_t first = 0;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list, el, q;
  const char *s;
  size_t n;
  uint32_t i;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_struct(&b, body.word, 0, 0, 2, 1, 1, &first) ==
            CAPNP_OK,
        "composite list");
  for (i = 0; i < 2; i++) {
    size_t eword = first + (size_t)i * 2;
    capnp_builder_set_u32(&b, eword, 0, 1000 + i);
    CHECK(capnp_builder_set_text(&b, eword, 1, 0, "hello", 5) == CAPNP_OK,
          "set text");
  }
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  CHECK(list.esize == CAPNP_SZ_COMPOSITE, "composite");

  CHECK_EQ_U32(capnp_list_get_u32(&list, 1, 0), 1001, "downgrade u32");
  CHECK_EQ_U32(capnp_list_get_u32(&list, 0, 0), 1000, "downgrade u32 e0");

  CHECK(capnp_list_get_struct(&list, 0, &el) == CAPNP_OK, "struct elem");
  CHECK_EQ_U32(capnp_get_u32(&el, 0, 0), 1000, "elem data");

  CHECK(capnp_list_getp(&list, 0, &el) == CAPNP_OK, "list_getp elem");
  CHECK(capnp_getp(&el, 0, &q) == CAPNP_OK, "first pointer");
  CHECK(q.kind == CAPNP_PK_LIST && q.esize == CAPNP_SZ_BYTE, "text blob");

  CHECK(capnp_list_get_text(&list, 0, &s, &n) == CAPNP_OK, "list text");
  CHECK_STREQ(s, n, "hello", "downgrade List(Text)");
  CHECK(capnp_list_get_text(&list, 1, &s, &n) == CAPNP_OK, "list text e1");
  CHECK_STREQ(s, n, "hello", "e1 text");
  capnp_message_free(&m);
}

static void test_list_upgrade_u8(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t items[] = {1, 2, 3, 4, 5};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list, el;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  capnp_builder_set_list_u8(&b, body.word, 0, 0, items, 5);
  capnp_builder_serialize(&b, &flat, &flen);
  capnp_builder_free(&b);
  capnp_message_from_flat(&m, flat, flen);
  free(flat);
  capnp_root(&m, &r);
  capnp_getp(&r, 0, &list);
  CHECK(capnp_list_get_struct(&list, 3, &el) == CAPNP_OK, "u8 upgrade");
  CHECK_EQ_U32(capnp_get_u8(&el, 0, 0), 4, "u8 field @0");
  CHECK_EQ_U32(capnp_get_u16(&el, 0, 0xabcd), 0xabcd, "u8 oversize default");
  capnp_message_free(&m);
}

int main(void) {
  test_list_bool_roundtrip();
  test_list_void_length();
  test_list_void_empty();
  test_list_upgrade_prim();
  test_list_upgrade_ptr();
  test_list_upgrade_bit_refused();
  test_list_downgrade_composite();
  test_list_upgrade_u8();
  return harness_finish();
}
