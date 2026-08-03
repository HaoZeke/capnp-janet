/* SPDX-License-Identifier: MIT */
/* Multi-segment arena, far + double-far builders, multi-seg copy_flat. */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_far_pointer_write_read(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, kid_slot, kid;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, k;
  uint64_t slot_w;

  /* root(1)+outer(1) fill 2 of 3; kid(2) spills to segment 1. */
  capnp_builder_init_sized(&b, 3);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "outer");
  CHECK(capnp_builder_slot(&body, 0, 0, &kid_slot) == CAPNP_OK, "slot");
  CHECK(capnp_builder_struct(&kid_slot, 2, 0, &kid) == CAPNP_OK, "kid");
  CHECK(capnp_builder_nsegs(&b) >= 2, "multi seg");
  CHECK(body.seg != kid.seg, "cross seg");
  CHECK(capnp_builder_set_u64(&kid, 0, 111) == CAPNP_OK, "111");
  CHECK(capnp_builder_set_u64(&kid, 8, 222) == CAPNP_OK, "222");

  slot_w = capnp_load_le64(capnp_builder_seg_data(&b, kid_slot.seg) +
                           kid_slot.word * CAPNP_WORD_BYTES);
  CHECK(capnp_wp_kind(slot_w) == CAPNP_WK_FAR, "slot far");
  CHECK(capnp_wp_far_two(slot_w) == 0, "single far");

  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);

  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "from_flat");
  free(flat);
  CHECK(m.nsegs >= 2, "reader multi");
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root r");
  CHECK(capnp_getp(&r, 0, &k) == CAPNP_OK, "getp");
  CHECK(k.kind == CAPNP_PK_STRUCT, "struct");
  CHECK(capnp_get_u64(&k, 0, 0) == 111, "111");
  CHECK(capnp_get_u64(&k, 8, 0) == 222, "222");

  {
    uint8_t *copy = NULL;
    size_t clen = 0;
    capnp_message_t m2;
    capnp_ptr_t r2, k2;
    CHECK(capnp_message_copy_flat(&m, &copy, &clen) == CAPNP_OK, "copy_flat");
    CHECK(capnp_message_from_flat(&m2, copy, clen) == CAPNP_OK, "reparse");
    free(copy);
    CHECK(m2.nsegs >= 2, "copy multi");
    CHECK(capnp_root(&m2, &r2) == CAPNP_OK, "r2");
    CHECK(capnp_getp(&r2, 0, &k2) == CAPNP_OK, "k2");
    CHECK(capnp_get_u64(&k2, 0, 0) == 111 && capnp_get_u64(&k2, 8, 0) == 222,
          "copy fields");
    capnp_message_free(&m2);
  }
  capnp_message_free(&m);
}

static void test_far_text(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r;
  const char *s;
  size_t n;
  char longtxt[200];

  memset(longtxt, 'x', sizeof(longtxt) - 1);
  longtxt[sizeof(longtxt) - 1] = '\0';

  capnp_builder_init_sized(&b, 4);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "st");
  CHECK(capnp_builder_set_text(&body, 0, 0, longtxt, strlen(longtxt)) ==
            CAPNP_OK,
        "text");
  CHECK(capnp_builder_nsegs(&b) >= 2, "text multi");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_get_text(&r, 0, &s, &n) == CAPNP_OK, "get");
  CHECK(n == strlen(longtxt) && s[0] == 'x' && s[n - 1] == 'x', "bytes");
  capnp_message_free(&m);
}

/* Hand-built multi-seg double-far fixture (read path). */
static void test_double_far_fixture_read(void) {
  uint8_t seg0[8], seg1[16], seg2[8];
  capnp_segment_t segs[3];
  capnp_message_t m;
  capnp_ptr_t r;
  uint8_t *framed = NULL;
  size_t framed_len = 0;

  capnp_store_le64(seg0, capnp_wp_make_far(1, 0, 1)); /* double-far -> seg1 */
  capnp_store_le64(seg1, capnp_wp_make_far(0, 0, 2)); /* content seg2 w0 */
  capnp_store_le64(seg1 + 8, capnp_wp_make_struct(0, 1, 0));
  capnp_store_le64(seg2, 4242);

  segs[0].data = seg0;
  segs[0].words = 1;
  segs[1].data = seg1;
  segs[1].words = 2;
  segs[2].data = seg2;
  segs[2].words = 1;

  CHECK(capnp_message_from_segments(&m, segs, 3) == CAPNP_OK, "from_segs");
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(r.kind == CAPNP_PK_STRUCT, "struct");
  CHECK(capnp_get_u64(&r, 0, 0) == 4242, "4242");
  CHECK(capnp_message_copy_flat(&m, &framed, &framed_len) == CAPNP_OK, "frame");
  capnp_message_free(&m);
  CHECK(capnp_message_from_flat(&m, framed, framed_len) == CAPNP_OK, "reparse");
  free(framed);
  CHECK(m.nsegs == 3, "3 segs");
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root2");
  CHECK(capnp_get_u64(&r, 0, 0) == 4242, "4242b");
  capnp_message_free(&m);
}

/* Builder-emitted double-far: max_seg_words=1 so pad cannot join object seg. */
static void test_double_far_builder_emit(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, obj;
  uint64_t w;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, k;

  capnp_builder_init_sized(&b, 3);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "outer");
  CHECK(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "slot");

  /* First kid spills (fills awareness); then freeze + emit double-far object. */
  CHECK(capnp_builder_struct(&slot, 2, 0, &obj) == CAPNP_OK, "spill");
  CHECK(obj.seg != slot.seg, "spilled");

  memset((void *)(uintptr_t)(capnp_builder_seg_data(&b, slot.seg) +
                             slot.word * CAPNP_WORD_BYTES),
         0, 8);
  /* New segments capped at 1 word: object fills entire new seg; pad cannot
   * alloc_in there -> double-far. */
  capnp_builder_set_max_seg_words(&b, 1);
  CHECK(capnp_builder_struct(&slot, 1, 0, &obj) == CAPNP_OK, "obj");
  CHECK(obj.seg != slot.seg, "obj other");
  CHECK(capnp_builder_set_u64(&obj, 0, 4242) == CAPNP_OK, "set");

  w = capnp_load_le64(capnp_builder_seg_data(&b, slot.seg) +
                      slot.word * CAPNP_WORD_BYTES);
  CHECK(capnp_wp_kind(w) == CAPNP_WK_FAR, "far");
  CHECK(capnp_wp_far_two(w) == 1, "double-far flag");

  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "parse");
  free(flat);
  CHECK(capnp_root(&m, &r) == CAPNP_OK, "root");
  CHECK(capnp_getp(&r, 0, &k) == CAPNP_OK, "getp");
  CHECK(k.kind == CAPNP_PK_STRUCT, "struct");
  CHECK(capnp_get_u64(&k, 0, 0) == 4242, "4242");
  capnp_message_free(&m);
}

int main(void) {
  test_far_pointer_write_read();
  test_far_text();
  test_double_far_fixture_read();
  test_double_far_builder_emit();
  return harness_finish();
}
