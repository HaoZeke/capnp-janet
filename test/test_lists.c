/* SPDX-License-Identifier: MIT */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_list_u32(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint32_t items[] = {1, 2, 100};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_u32(&body, 0, 0, items, 3) == CAPNP_OK,
        "set list u32");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  CHECK_EQ_U32(capnp_list_len(&list), 3, "len");
  CHECK_EQ_U32(capnp_list_get_u32(&list, 0, 0), 1, "e0");
  CHECK_EQ_U32(capnp_list_get_u32(&list, 1, 0), 2, "e1");
  CHECK_EQ_U32(capnp_list_get_u32(&list, 2, 0), 100, "e2");
  capnp_message_free(&m);
}

/* A primitive list is little-endian on the wire whatever the host is.
 *
 * The round-trip tests above pass on a little-endian host even when the
 * builder writes host order, because the reader undoes exactly what the
 * writer did. Pinning the bytes is what catches that: on s390x the
 * builder wrote 00 00 00 01 where the format says 01 00 00 00.
 */
static void test_list_u32_wire_bytes(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint32_t items[] = {1, 2, 100};
  uint8_t *flat = NULL;
  size_t flen = 0;
  /* Segment table (8) + root pointer (8) + struct body (8) + the list's
   * three u32 elements padded to two words. The elements start at byte
   * 24: 01 00 00 00  02 00 00 00  64 00 00 00. */
  static const uint8_t want[] = {0x01, 0x00, 0x00, 0x00, 0x02, 0x00,
                                 0x00, 0x00, 0x64, 0x00, 0x00, 0x00};
  size_t i, off = 24;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_u32(&body, 0, 0, items, 3) == CAPNP_OK,
        "set list u32");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(flen >= off + sizeof want, "frame long enough");
  if (flen >= off + sizeof want) {
    for (i = 0; i < sizeof want; i++)
      CHECK(flat[off + i] == want[i], "list element byte");
  }
  free(flat);
}

static void test_data_blob(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  const uint8_t blob[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r;
  const uint8_t *out;
  size_t n;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_data(&body, 0, 0, blob, sizeof(blob)) ==
            CAPNP_OK,
        "set data");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_get_data(&r, 0, &out, &n) == CAPNP_OK, "get data");
  CHECK(n == sizeof(blob) && memcmp(out, blob, n) == 0, "blob bytes");
  capnp_message_free(&m);
}

static void test_list_f64(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  double items[] = {1.5, -2.0, 3.25};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_f64(&body, 0, 0, items, 3) == CAPNP_OK,
        "set f64");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  CHECK_NEAR(capnp_list_get_f64(&list, 0, 0), 1.5, 1e-12, "1.5");
  CHECK_NEAR(capnp_list_get_f64(&list, 1, 0), -2.0, 1e-12, "-2");
  CHECK_NEAR(capnp_list_get_f64(&list, 2, 0), 3.25, 1e-12, "3.25");
  capnp_message_free(&m);
}

static void test_copy_flat(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL, *copy = NULL;
  size_t flen = 0, clen = 0;
  capnp_message_t m, m2;
  capnp_ptr_t r;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 1, 0, &body);
  capnp_builder_set_u32(&body, 0, 99);
  capnp_builder_serialize(&b, &flat, &flen);
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "m");
  free(flat);
  CHECK(capnp_message_copy_flat(&m, &copy, &clen) == CAPNP_OK, "copy");
  capnp_message_free(&m);
  CHECK(capnp_message_from_flat(&m2, copy, clen) == CAPNP_OK, "m2");
  free(copy);
  CHECK(capnp_root(&m2, &r) == CAPNP_OK, "root");
  CHECK_EQ_U32(capnp_get_u32(&r, 0, 0), 99, "99");
  capnp_message_free(&m2);
}


static void test_struct_u64(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r;
  const uint64_t want = 0x1122334455667788ULL;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  /* 1 data word holds one u64 at byte offset 0. */
  capnp_builder_struct(&root, 1, 0, &body);
  CHECK(capnp_builder_set_u64(&body, 0, want) == CAPNP_OK, "set u64");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK_EQ_U64(capnp_get_u64(&r, 0, 0), want, "get u64");
  /* past-end default on empty struct */
  capnp_message_free(&m);

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 0, &body);
  capnp_builder_serialize(&b, &flat, &flen);
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat0");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root0");
  CHECK_EQ_U64(capnp_get_u64(&r, 0, 99), 99, "default past end");
  capnp_message_free(&m);
}

static void test_list_u64(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint64_t items[] = {1ULL, 0x100000001ULL, 0xdeadbeefcafebabeULL};
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  CHECK(capnp_builder_set_list_u64(&body, 0, 0, items, 3) == CAPNP_OK,
        "set list u64");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  CHECK_EQ_U32(capnp_list_len(&list), 3, "len");
  CHECK_EQ_U64(capnp_list_get_u64(&list, 0, 0), items[0], "e0");
  CHECK_EQ_U64(capnp_list_get_u64(&list, 1, 0), items[1], "e1");
  CHECK_EQ_U64(capnp_list_get_u64(&list, 2, 0), items[2], "e2");
  capnp_message_free(&m);
}

int main(void) {
  test_list_u32();
  test_list_u32_wire_bytes();
  test_struct_u64();
  test_list_u64();
  test_data_blob();
  test_list_f64();
  test_copy_flat();
  return harness_finish();
}
