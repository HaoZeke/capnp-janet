/* SPDX-License-Identifier: MIT */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_packed.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_file(const char *path, uint8_t **out, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  long sz;
  uint8_t *buf;
  if (!f)
    return -1;
  if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return -1;
  }
  buf = malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  *out = buf;
  *out_len = (size_t)sz;
  return 0;
}

/* Spec example: encoding.html packing section. */
static void test_spec_example(void) {
  /* unpacked: 08 00 00 00 03 00 02 00  19 00 00 00 aa 01 00 00 */
  const uint8_t unp[] = {0x08, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00,
                         0x19, 0x00, 0x00, 0x00, 0xaa, 0x01, 0x00, 0x00};
  /* packed:   51 08 03 02  31 19 aa 01 */
  const uint8_t exp[] = {0x51, 0x08, 0x03, 0x02, 0x31, 0x19, 0xaa, 0x01};
  uint8_t *packed = NULL, *unpacked = NULL;
  size_t plen = 0, ulen = 0;

  CHECK(capnp_pack(unp, sizeof(unp), &packed, &plen) == CAPNP_OK, "pack");
  CHECK(plen == sizeof(exp) && memcmp(packed, exp, plen) == 0, "spec pack");
  CHECK(capnp_unpack(packed, plen, &unpacked, &ulen) == CAPNP_OK, "unpack");
  CHECK(ulen == sizeof(unp) && memcmp(unpacked, unp, ulen) == 0, "spec unpack");
  free(packed);
  free(unpacked);
}

/* 32 zero bytes → 00 03 (tag 0 + 3 additional zero words after first). */
static void test_zero_run(void) {
  uint8_t unp[32];
  uint8_t *packed = NULL, *unpacked = NULL;
  size_t plen = 0, ulen = 0;

  memset(unp, 0, sizeof(unp));
  CHECK(capnp_pack(unp, sizeof(unp), &packed, &plen) == CAPNP_OK, "pack");
  CHECK(plen == 2 && packed[0] == 0x00 && packed[1] == 0x03, "zero run pack");
  CHECK(capnp_unpack(packed, plen, &unpacked, &ulen) == CAPNP_OK, "unpack");
  CHECK(ulen == 32 && memcmp(unpacked, unp, 32) == 0, "zero run unpack");
  free(packed);
  free(unpacked);
}

/* All 0x8a words: 0xff path. */
static void test_dense_run(void) {
  uint8_t unp[32];
  uint8_t *packed = NULL, *unpacked = NULL;
  size_t plen = 0, ulen = 0;
  int i;

  memset(unp, 0x8a, sizeof(unp));
  CHECK(capnp_pack(unp, sizeof(unp), &packed, &plen) == CAPNP_OK, "pack");
  /* First word: tag 0xff + 8 bytes 0x8a + count 3 + 24 bytes 0x8a */
  CHECK(plen == 1 + 8 + 1 + 24, "dense size");
  CHECK(packed[0] == 0xff, "ff tag");
  for (i = 1; i <= 8; i++)
    CHECK(packed[i] == 0x8a, "payload");
  CHECK(packed[9] == 0x03, "count 3");
  CHECK(capnp_unpack(packed, plen, &unpacked, &ulen) == CAPNP_OK, "unpack");
  CHECK(ulen == 32 && memcmp(unpacked, unp, 32) == 0, "dense unpack");
  free(packed);
  free(unpacked);
}

/*
 * C++ PackedOutputStream heuristic: after 0xff, keep words with fewer than
 * two zero bytes in the verbatim run. One trailing zero stays uncompressed.
 * Input: "bob@exam" (8 nonzero) + "ple.com\0" (one zero) + sparse word.
 */
static void test_one_zero_verbatim(void) {
  uint8_t unp[24];
  /* Expected: ff + bob@exam + count 01 + ple.com\0 + tag 0x01 + 0x08 */
  const uint8_t exp[] = {
      0xff, 'b', 'o', 'b', '@', 'e', 'x', 'a', 'm', 0x01, 'p', 'l', 'e',
      '.',  'c', 'o', 'm', 0x00, 0x01, 0x08};
  uint8_t *packed = NULL, *unpacked = NULL;
  size_t plen = 0, ulen = 0;

  memcpy(unp, "bob@example.com", 15);
  unp[15] = 0;
  memset(unp + 16, 0, 8);
  unp[16] = 0x08;

  CHECK(capnp_pack(unp, sizeof(unp), &packed, &plen) == CAPNP_OK, "pack");
  CHECK(plen == sizeof(exp) && memcmp(packed, exp, plen) == 0,
        "one-zero verbatim pack");
  CHECK(capnp_unpack(packed, plen, &unpacked, &ulen) == CAPNP_OK, "unpack");
  CHECK(ulen == sizeof(unp) && memcmp(unpacked, unp, ulen) == 0, "roundtrip");
  free(packed);
  free(unpacked);
}

/*
 * Two zero bytes in a following word end the verbatim run: compress that
 * word with a normal tag instead.
 */
static void test_two_zeros_end_run(void) {
  uint8_t unp[16];
  /* Word0 all 0xff → tag ff + 8 bytes; word1 has two zeros → not in run. */
  const uint8_t exp[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                         0x00, /* count 0 */
                         0x3f, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  uint8_t *packed = NULL, *unpacked = NULL;
  size_t plen = 0, ulen = 0;

  memset(unp, 0xff, 8);
  unp[8] = 0x01;
  unp[9] = 0x02;
  unp[10] = 0x03;
  unp[11] = 0x04;
  unp[12] = 0x05;
  unp[13] = 0x06;
  unp[14] = 0x00;
  unp[15] = 0x00;

  CHECK(capnp_pack(unp, sizeof(unp), &packed, &plen) == CAPNP_OK, "pack");
  CHECK(plen == sizeof(exp) && memcmp(packed, exp, plen) == 0,
        "two-zero ends run");
  CHECK(capnp_unpack(packed, plen, &unpacked, &ulen) == CAPNP_OK, "unpack");
  CHECK(ulen == sizeof(unp) && memcmp(unpacked, unp, ulen) == 0, "roundtrip");
  free(packed);
  free(unpacked);
}

static void test_addressbook_roundtrip(void) {
  const char *src = getenv("CAPNP_JANET_SOURCE_ROOT");
  char path[1024];
  uint8_t *bin = NULL, *packed = NULL, *unpacked = NULL;
  size_t blen = 0, plen = 0, ulen = 0;
  capnp_message_t m;
  capnp_ptr_t book, people;

  if (!src || !src[0])
    src = ".";
  snprintf(path, sizeof(path), "%s/test/fixtures/addressbook_alice_bob.bin",
           src);
  if (load_file(path, &bin, &blen) != 0) {
    fprintf(stderr, "SKIP addressbook packed: no fixture\n");
    return;
  }
  CHECK(capnp_pack(bin, blen, &packed, &plen) == CAPNP_OK, "pack ab");
  CHECK(plen < blen, "packed smaller");
  CHECK(capnp_unpack(packed, plen, &unpacked, &ulen) == CAPNP_OK, "unpack ab");
  CHECK(ulen == blen && memcmp(unpacked, bin, blen) == 0, "ab bytes equal");

  /* Byte-identical to official `capnp convert binary:packed`. */
  {
    char gpath[1024];
    uint8_t *golden = NULL, *from_gold = NULL;
    size_t glen = 0, gulen = 0;
    snprintf(gpath, sizeof(gpath),
             "%s/test/fixtures/addressbook_alice_bob.packed", src);
    CHECK(load_file(gpath, &golden, &glen) == 0, "load official packed");
    CHECK(plen == glen && memcmp(packed, golden, glen) == 0,
          "pack == official packed golden");
    CHECK(capnp_unpack(golden, glen, &from_gold, &gulen) == CAPNP_OK,
          "unpack official packed");
    CHECK(gulen == blen && memcmp(from_gold, bin, blen) == 0,
          "official packed → original");
    free(from_gold);
    free(golden);
  }

  CHECK(capnp_message_from_flat(&m, unpacked, ulen) == CAPNP_OK, "from flat");
  CHECK(capnp_root(&m, &book) == CAPNP_OK, "root");
  CHECK(capnp_getp(&book, 0, &people) == CAPNP_OK, "people");
  CHECK_EQ_U32(capnp_list_len(&people), 2, "2 people after pack roundtrip");
  capnp_message_free(&m);
  free(bin);
  free(packed);
  free(unpacked);
}

static void test_builder_then_pack(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL, *packed = NULL, *unpacked = NULL;
  size_t flen = 0, plen = 0, ulen = 0;
  capnp_message_t m;
  capnp_ptr_t r;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 1, 1, &body);
  capnp_builder_set_u32(&body, 0, 42);
  capnp_builder_set_text(&body, 1, 0, "hi", 2);
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_pack(flat, flen, &packed, &plen) == CAPNP_OK, "pack");
  CHECK(capnp_unpack(packed, plen, &unpacked, &ulen) == CAPNP_OK, "unpack");
  CHECK(ulen == flen && memcmp(unpacked, flat, flen) == 0, "equal");
  CHECK(capnp_message_from_flat(&m, unpacked, ulen) == CAPNP_OK, "msg");
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK_EQ_U32(capnp_get_u32(&r, 0, 0), 42, "42");
  capnp_message_free(&m);
  free(flat);
  free(packed);
  free(unpacked);
}

int main(void) {
  test_spec_example();
  test_zero_run();
  test_dense_run();
  test_one_zero_verbatim();
  test_two_zeros_end_run();
  test_addressbook_roundtrip();
  test_builder_then_pack();
  return harness_finish();
}
