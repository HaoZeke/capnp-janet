/* Malformed-message and nesting-limit regressions. */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_truncated_framing(void) {
  capnp_message_t m;
  uint8_t tiny[4] = {0, 0, 0, 0};
  uint8_t short_table[8] = {1, 0, 0, 0, 1, 0, 0, 0};

  CHECK(capnp_message_from_flat(&m, tiny, sizeof(tiny)) == CAPNP_ERR_FRAMING,
        "four bytes is not a frame");
  capnp_message_free(&m);
  /* segmentCountMinusOne = 1 claims two segments but the size table is short. */
  CHECK(capnp_message_from_flat(&m, short_table, sizeof(short_table)) ==
            CAPNP_ERR_FRAMING,
        "truncated segment table");
  capnp_message_free(&m);
}

static void test_inline_struct_out_of_bounds(void) {
  uint8_t seg[8];
  capnp_segment_t s;
  capnp_message_t m;
  capnp_ptr_t root;

  /* Offset 0 puts a 4-word body past this one-word segment. */
  capnp_store_le64(seg, capnp_wp_make_struct(0, 4, 0));
  s.data = seg;
  s.words = 1;
  CHECK(capnp_message_from_segments(&m, &s, 1) == CAPNP_OK, "view");
  CHECK(capnp_root(&m, &root) == CAPNP_ERR_BOUNDS, "struct body past end");
  capnp_message_free(&m);
}

static void test_double_far_struct_out_of_bounds(void) {
  uint8_t seg0[8], seg1[16], seg2[8];
  capnp_segment_t segs[3];
  capnp_message_t m;
  capnp_ptr_t root;

  capnp_store_le64(seg0, capnp_wp_make_far(1, 0, 1));
  capnp_store_le64(seg1, capnp_wp_make_far(0, 5, 2));
  capnp_store_le64(seg1 + 8, capnp_wp_make_struct(0, 1, 0));
  capnp_store_le64(seg2, 1);
  segs[0].data = seg0;
  segs[0].words = 1;
  segs[1].data = seg1;
  segs[1].words = 2;
  segs[2].data = seg2;
  segs[2].words = 1;
  CHECK(capnp_message_from_segments(&m, segs, 3) == CAPNP_OK, "view");
  CHECK(capnp_root(&m, &root) == CAPNP_ERR_BOUNDS,
        "double-far body past the target segment");
  capnp_message_free(&m);
}

static void test_double_far_list_out_of_bounds(void) {
  uint8_t seg0[8], seg1[16], seg2[8];
  capnp_segment_t segs[3];
  capnp_message_t m;
  capnp_ptr_t root;

  capnp_store_le64(seg0, capnp_wp_make_far(1, 0, 1));
  capnp_store_le64(seg1, capnp_wp_make_far(0, 4, 2));
  capnp_store_le64(seg1 + 8, capnp_wp_make_list(0, CAPNP_SZ_BYTE, 32));
  memset(seg2, 0, sizeof(seg2));
  segs[0].data = seg0;
  segs[0].words = 1;
  segs[1].data = seg1;
  segs[1].words = 2;
  segs[2].data = seg2;
  segs[2].words = 1;
  CHECK(capnp_message_from_segments(&m, segs, 3) == CAPNP_OK, "view");
  CHECK(capnp_root(&m, &root) == CAPNP_ERR_BOUNDS,
        "double-far list past the target segment");
  capnp_message_free(&m);
}

static void test_negative_composite_count(void) {
  uint8_t seg[16];
  capnp_segment_t s;
  capnp_message_t m;
  capnp_ptr_t root;

  /* List of 1 word whose composite tag claims a negative element count. */
  capnp_store_le64(seg, capnp_wp_make_list(0, CAPNP_SZ_COMPOSITE, 1));
  capnp_store_le64(seg + 8, capnp_wp_make_struct(-1, 1, 0));
  s.data = seg;
  s.words = 2;
  CHECK(capnp_message_from_segments(&m, &s, 1) == CAPNP_OK, "view");
  CHECK(capnp_root(&m, &root) == CAPNP_ERR_BOUNDS, "negative element count");
  capnp_message_free(&m);
}

static void test_depth_limit_and_cycle(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, cur;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t p;
  int i, rc;
  uint8_t cycle[16];
  capnp_segment_t s;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "top");
  cur = body;
  for (i = 0; i < 6; i++) {
    capnp_bptr_t next;
    CHECK(capnp_builder_slot(&cur, 0, 0, &slot) == CAPNP_OK, "slot");
    CHECK(capnp_builder_struct(&slot, 0, 1, &next) == CAPNP_OK, "next");
    cur = next;
  }
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "parse");
  free(flat);
  m.depth_limit = 4;
  CHECK(capnp_root(&m, &p) == CAPNP_OK, "root read");
  for (i = 0; i < 3; i++) {
    rc = capnp_getp(&p, 0, &p);
    CHECK(rc == CAPNP_OK, "follow within limit");
  }
  CHECK(capnp_getp(&p, 0, &p) == CAPNP_ERR_DEPTH, "follow past limit");
  capnp_message_free(&m);

  /* A struct pointer whose body is its own word. Walking it must stop. */
  capnp_store_le64(cycle, capnp_wp_make_struct(0, 0, 1));
  capnp_store_le64(cycle + 8, capnp_wp_make_struct(-1, 0, 1));
  s.data = cycle;
  s.words = 2;
  CHECK(capnp_message_from_segments(&m, &s, 1) == CAPNP_OK, "cycle view");
  m.depth_limit = 8;
  CHECK(capnp_root(&m, &p) == CAPNP_OK, "cycle root");
  for (i = 0; i < 16; i++) {
    rc = capnp_getp(&p, 0, &p);
    if (rc == CAPNP_ERR_DEPTH)
      break;
  }
  CHECK(rc == CAPNP_ERR_DEPTH, "cycle hits the depth limit");
  capnp_message_free(&m);
}

static void test_builder_offset_overflow(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r;

  capnp_builder_init_sized(&b, 2);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 1, 0, &body) == CAPNP_OK, "struct");
  CHECK(capnp_builder_set_u8(&body, UINT32_MAX, 0x11) == CAPNP_ERR_BOUNDS,
        "u8 offset wraps");
  CHECK(capnp_builder_set_u32(&body, UINT32_MAX - 2u, 1) == CAPNP_ERR_BOUNDS,
        "u32 offset wraps");
  CHECK(capnp_builder_set_u64(&body, UINT32_MAX, 1) == CAPNP_ERR_BOUNDS,
        "u64 offset wraps");
  CHECK(capnp_builder_set_bool(&body, UINT32_MAX, 1) == CAPNP_ERR_BOUNDS,
        "bool offset wraps");
  CHECK(capnp_builder_set_u32(&body, 0, 0x11223344u) == CAPNP_OK, "in range");
  CHECK(capnp_builder_set_u8(&body, 7, 0xab) == CAPNP_OK, "last byte");
  CHECK(capnp_builder_set_u8(&body, 8, 1) == CAPNP_ERR_BOUNDS, "past segment");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "parse");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "read");
  CHECK(capnp_get_u32(&r, 0, 0) == 0x11223344u, "low word kept");
  CHECK(capnp_get_u8(&r, 7, 0) == 0xab, "last byte kept");
  capnp_message_free(&m);
}

static void test_many_segments_roundtrip(void) {
  capnp_builder_t b;
  capnp_bptr_t root, cur, slot, next;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t p;
  uint32_t nsegs;
  int i;
  uint8_t huge[16];

  capnp_builder_init_sized(&b, 1);
  capnp_builder_set_max_seg_words(&b, 1);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &cur) == CAPNP_OK, "first");
  for (i = 0; i < 20; i++) {
    CHECK(capnp_builder_slot(&cur, 0, 0, &slot) == CAPNP_OK, "slot");
    CHECK(capnp_builder_struct(&slot, 0, 1, &next) == CAPNP_OK, "next");
    cur = next;
  }
  nsegs = capnp_builder_nsegs(&b);
  CHECK(nsegs > 16, "builder passed the old 16 segment cap");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK,
        "reader accepts the segments");
  free(flat);
  CHECK(m.nsegs == nsegs, "segment count preserved");
  CHECK(capnp_root(&m, &p) == CAPNP_OK, "root");
  CHECK(p.kind == CAPNP_PK_STRUCT, "root struct");
  capnp_message_free(&m);

  memset(huge, 0, sizeof(huge));
  capnp_store_le32(huge, 1);
  capnp_store_le32(huge + 4, 0xffffffffu);
  capnp_store_le32(huge + 8, 0xffffffffu);
  CHECK(capnp_message_from_flat(&m, huge, sizeof(huge)) == CAPNP_ERR_FRAMING,
        "segment sizes do not wrap into a short body");
  capnp_message_free(&m);
}

static void test_text_requires_nul(void) {
  uint8_t seg[24];
  capnp_segment_t s;
  capnp_message_t m;
  capnp_ptr_t root;
  const char *text = NULL;
  size_t n = 99;

  memset(seg, 0, sizeof(seg));
  /* Root struct: 0 data, 1 pointer. Pointer targets a 4-byte list "abcd" with no NUL. */
  capnp_store_le64(seg, capnp_wp_make_struct(0, 0, 1));
  capnp_store_le64(seg + 8, capnp_wp_make_list(0, CAPNP_SZ_BYTE, 4));
  memcpy(seg + 16, "abcd", 4);
  s.data = seg;
  s.words = 3;
  CHECK(capnp_message_from_segments(&m, &s, 1) == CAPNP_OK, "view");
  CHECK(capnp_root(&m, &root) == CAPNP_OK, "root");
  CHECK(capnp_get_text(&root, 0, &text, &n) == CAPNP_ERR_KIND,
        "text without NUL");
  capnp_message_free(&m);

  memset(seg, 0, sizeof(seg));
  capnp_store_le64(seg, capnp_wp_make_struct(0, 0, 1));
  capnp_store_le64(seg + 8, capnp_wp_make_list(0, CAPNP_SZ_BYTE, 5));
  memcpy(seg + 16, "abcd", 5); /* fourth payload byte plus NUL in the word */
  seg[20] = 0;
  s.words = 3;
  CHECK(capnp_message_from_segments(&m, &s, 1) == CAPNP_OK, "view nul");
  CHECK(capnp_root(&m, &root) == CAPNP_OK, "root nul");
  CHECK(capnp_get_text(&root, 0, &text, &n) == CAPNP_OK, "terminated text");
  CHECK(n == 4 && text && memcmp(text, "abcd", 4) == 0, "abcd");
  capnp_message_free(&m);
}

int main(void) {
  test_truncated_framing();
  test_inline_struct_out_of_bounds();
  test_double_far_struct_out_of_bounds();
  test_double_far_list_out_of_bounds();
  test_negative_composite_count();
  test_depth_limit_and_cycle();
  test_text_requires_nul();
  test_builder_offset_overflow();
  test_many_segments_roundtrip();
  return harness_finish();
}
