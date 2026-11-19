/* Malformed-message and nesting-limit regressions. */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SANITIZE_ADDRESS__)
size_t __sanitizer_get_current_allocated_bytes(void);
#else
#include <malloc.h>
#endif

static void test_truncated_framing(void) {
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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
  uint8_t seg[32];
  capnp_segment_t s;
  capnp_message_t m = {0};
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

  /* Empty byte lists are valid Data but never valid Text. */
  memset(seg, 0, sizeof(seg));
  capnp_store_le64(seg, capnp_wp_make_struct(0, 0, 1));
  capnp_store_le64(seg + 8, capnp_wp_make_list(0, CAPNP_SZ_BYTE, 0));
  s.words = 2;
  CHECK(capnp_message_from_segments(&m, &s, 1) == CAPNP_OK, "empty bytes view");
  CHECK(capnp_root(&m, &root) == CAPNP_OK, "empty bytes root");
  CHECK(capnp_get_text(&root, 0, &text, &n) == CAPNP_ERR_KIND,
        "empty bytes are not text");
  capnp_message_free(&m);

  /* A composite text upgrade needs its field-zero pointer section. */
  memset(seg, 0, sizeof(seg));
  capnp_store_le64(seg, capnp_wp_make_list(0, CAPNP_SZ_COMPOSITE, 1));
  capnp_store_le64(seg + 8, capnp_wp_make_struct(1, 1, 0));
  capnp_store_le64(seg + 16, 0);
  s.words = 3;
  CHECK(capnp_message_from_segments(&m, &s, 1) == CAPNP_OK, "data-only list");
  CHECK(capnp_root(&m, &root) == CAPNP_OK, "data-only root");
  CHECK(capnp_list_get_text(&root, 0, &text, &n) == CAPNP_ERR_KIND,
        "data-only composite is not text");
  capnp_message_free(&m);
}

/* C++ ReaderOptions.traversalLimitInWords: each pointer and its body is
 * charged, and a read past the budget fails instead of walking the rest. */
static void test_traversal_budget(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, child;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m = {0};
  capnp_ptr_t r, k;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 4, 1, &body) == CAPNP_OK, "body");
  CHECK(capnp_builder_set_u64(&body, 0, 1) == CAPNP_OK, "word");
  CHECK(capnp_builder_slot(&body, 4, 0, &slot) == CAPNP_OK, "slot");
  CHECK(capnp_builder_struct(&slot, 2, 0, &child) == CAPNP_OK, "child");
  CHECK(capnp_builder_set_u64(&child, 0, 9) == CAPNP_OK, "child word");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);

  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "parse");
  /* One pointer word plus a 5-word body (4 data + 1 pointer) is more than 3. */
  m.traversal_left = 3;
  CHECK(capnp_root(&m, &r) == CAPNP_ERR_TRAVERSAL, "root over budget");
  capnp_message_free(&m);

  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "parse again");
  free(flat);
  m.traversal_left = 8;
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root within budget");
  CHECK(capnp_getp(&r, 0, &k) == CAPNP_ERR_TRAVERSAL, "child over budget");
  capnp_message_free(&m);
}

static void test_zero_size_and_far_validation(void) {
  uint8_t seg0[8], seg1[16], seg2[32];
  capnp_segment_t segs[3];
  capnp_message_t m = {0};
  capnp_ptr_t root;

  /* A zero-byte list still has to name an existing segment. */
  capnp_store_le64(seg0, capnp_wp_make_far(1, 0, 1));
  capnp_store_le64(seg1, capnp_wp_make_far(0, 0, 2));
  capnp_store_le64(seg1 + 8, capnp_wp_make_list(0, CAPNP_SZ_BYTE, 0));
  segs[0] = (capnp_segment_t){seg0, 1};
  segs[1] = (capnp_segment_t){seg1, 2};
  CHECK(capnp_message_from_segments(&m, segs, 2) == CAPNP_OK, "zero list view");
  CHECK(capnp_root(&m, &root) != CAPNP_OK, "zero list validates far segment");
  capnp_message_free(&m);

  /* A single-far landing pad is an object pointer, never another far. */
  capnp_store_le64(seg0, capnp_wp_make_far(0, 0, 1));
  capnp_store_le64(seg1, capnp_wp_make_far(0, 0, 0));
  segs[0] = (capnp_segment_t){seg0, 1};
  segs[1] = (capnp_segment_t){seg1, 1};
  CHECK(capnp_message_from_segments(&m, segs, 2) == CAPNP_OK, "far chain view");
  CHECK(capnp_root(&m, &root) == CAPNP_ERR_KIND, "far chain rejected");
  capnp_message_free(&m);

  /* A zero pad is the canonical empty struct, not a null pointer. */
  capnp_store_le64(seg1, 0);
  CHECK(capnp_message_from_segments(&m, segs, 2) == CAPNP_OK, "zero pad view");
  CHECK(capnp_root(&m, &root) == CAPNP_OK && root.kind == CAPNP_PK_STRUCT,
        "zero far pad is empty struct");
  capnp_message_free(&m);

  /* Reserved OTHER encodings are not capability pointers. */
  capnp_store_le64(seg0, 7);
  segs[0] = (capnp_segment_t){seg0, 1};
  CHECK(capnp_message_from_segments(&m, segs, 1) == CAPNP_OK, "other view");
  CHECK(capnp_root(&m, &root) == CAPNP_ERR_KIND, "reserved other rejected");
  capnp_message_free(&m);

  /* Over-allocated composite payload words are valid non-canonical input. */
  capnp_store_le64(seg2, capnp_wp_make_list(0, CAPNP_SZ_COMPOSITE, 2));
  capnp_store_le64(seg2 + 8, capnp_wp_make_struct(1, 1, 0));
  capnp_store_le64(seg2 + 16, 0);
  segs[0] = (capnp_segment_t){seg2, 4};
  CHECK(capnp_message_from_segments(&m, segs, 1) == CAPNP_OK, "composite view");
  CHECK(capnp_root(&m, &root) == CAPNP_OK && capnp_list_len(&root) == 1,
        "over-allocated composite accepted");
  capnp_message_free(&m);
}

static void test_virtual_traversal_charge(void) {
  uint8_t seg[8];
  capnp_segment_t s = {seg, 1};
  capnp_message_t m = {0};
  capnp_ptr_t root;
  capnp_store_le64(seg, capnp_wp_make_list(0, CAPNP_SZ_VOID, 0x1fffffffu));
  CHECK(capnp_message_from_segments(&m, &s, 1) == CAPNP_OK, "void list view");
  CHECK(capnp_root(&m, &root) == CAPNP_ERR_TRAVERSAL,
        "void list consumes traversal budget");
  capnp_message_free(&m);
}

static size_t heap_now(void) {
#if defined(__SANITIZE_ADDRESS__)
  return __sanitizer_get_current_allocated_bytes();
#else
  struct mallinfo2 info = mallinfo2();
  return (size_t)info.uordblks;
#endif
}

static void test_reparse_releases_storage(void) {
  uint8_t flat[16];
  uint8_t body[8];
  capnp_segment_t seg;
  capnp_message_t m = {0};
  size_t before, after;
  int i;
  memset(&m, 0, sizeof(m));
  memset(flat, 0, sizeof(flat));
  capnp_store_le32(flat, 0);
  capnp_store_le32(flat + 4, 1);
  capnp_store_le64(flat + 8, capnp_wp_make_struct(-1, 0, 0));
  memset(body, 0, sizeof(body));
  capnp_store_le64(body, capnp_wp_make_struct(-1, 0, 0));
  seg.data = body;
  seg.words = 1;
  before = heap_now();
  for (i = 0; i < 200; i++) {
    CHECK(capnp_message_from_flat(&m, flat, sizeof(flat)) == CAPNP_OK,
          "reparse flat");
    CHECK(capnp_message_from_flat(&m, flat, sizeof(flat)) == CAPNP_OK,
          "reparse flat again");
    CHECK(capnp_message_view_flat(&m, flat, sizeof(flat)) == CAPNP_OK,
          "reparse view");
    CHECK(capnp_message_from_segments(&m, &seg, 1) == CAPNP_OK, "reparse segs");
    capnp_message_free(&m);
    capnp_message_free(&m);
  }
  after = heap_now();
  CHECK(after <= before + 4096, "reparse does not leak");
}

/* The documented contract: a zeroed message is not live, so free is a
 * no-op and the first view does not release garbage. */
static void test_zero_init_message(void) {
  uint8_t flat[16];
  capnp_message_t m = {0};
  capnp_ptr_t root;
  memset(flat, 0, sizeof(flat));
  capnp_store_le32(flat, 0);
  capnp_store_le32(flat + 4, 1);
  capnp_store_le64(flat + 8, capnp_wp_make_struct(-1, 0, 0));
  capnp_message_free(&m);
  CHECK(capnp_message_view_flat(&m, flat, sizeof(flat)) == CAPNP_OK,
        "zeroed view");
  CHECK(capnp_root(&m, &root) == CAPNP_OK, "zeroed root");
  capnp_message_free(&m);
  capnp_message_free(&m);
}

static void test_u32_grow(void) {
  uint32_t ncap = 1;
  size_t nbytes = 1;
  CHECK(capnp_u32_grow(0x80000000u, sizeof(void *), &ncap, &nbytes) ==
            CAPNP_ERR_BOUNDS,
        "cap cannot double");
  CHECK(ncap == 1 && nbytes == 1, "outputs unchanged on failure");
  CHECK(capnp_u32_grow(4, sizeof(void *), &ncap, &nbytes) == CAPNP_OK,
        "small cap doubles");
  CHECK(ncap == 8 && nbytes == 8u * sizeof(void *), "8 slots");
  CHECK(capnp_u32_grow(0, sizeof(void *), &ncap, &nbytes) == CAPNP_OK,
        "empty starts at 4");
  CHECK(ncap == 4, "first cap");
  CHECK(capnp_u32_grow(4, SIZE_MAX, &ncap, &nbytes) == CAPNP_ERR_BOUNDS,
        "byte size overflow");
}

int main(void) {
  test_truncated_framing();
  test_reparse_releases_storage();
  test_zero_init_message();
  test_u32_grow();
  test_inline_struct_out_of_bounds();
  test_double_far_struct_out_of_bounds();
  test_double_far_list_out_of_bounds();
  test_negative_composite_count();
  test_depth_limit_and_cycle();
  test_text_requires_nul();
  test_builder_offset_overflow();
  test_many_segments_roundtrip();
  test_traversal_budget();
  test_zero_size_and_far_validation();
  test_virtual_traversal_charge();
  return harness_finish();
}
