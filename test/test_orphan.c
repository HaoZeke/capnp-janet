#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void check(int cond, const char *what) {
  if (!cond) {
    fprintf(stderr, "FAIL %s\n", what);
    g_fail++;
  }
}

/* Build the same two-pointer struct twice: once attached immediately, once
 * via an orphan that is disowned and adopted into the other slot. Canonical
 * form must match, which is what capnp encode produces for this shape. */
static void test_adopt_matches_direct(void) {
  capnp_builder_t direct, via;
  capnp_bptr_t root, body, slot, child, orphan_body, slot0;
  capnp_orphan_t orphan;
  uint8_t *fa = NULL, *fb = NULL, *ca = NULL, *cb = NULL;
  size_t la = 0, lb = 0, lca = 0, lcb = 0;
  capnp_message_t ma = {0}, mb = {0};

  capnp_builder_init(&direct);
  check(capnp_builder_root(&direct, &root) == CAPNP_OK, "root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "body");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "slot");
  check(capnp_builder_struct(&slot, 1, 0, &child) == CAPNP_OK, "child");
  check(capnp_builder_set_u64(&child, 0, 7) == CAPNP_OK, "set");
  check(capnp_builder_serialize(&direct, &fa, &la) == CAPNP_OK, "ser a");

  capnp_builder_init(&via);
  check(capnp_builder_root(&via, &root) == CAPNP_OK, "root b");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "body b");
  check(capnp_builder_new_orphan_struct(&via, 1, 0, &orphan, &orphan_body) ==
            CAPNP_OK,
        "new orphan");
  check(capnp_builder_set_u64(&orphan_body, 0, 7) == CAPNP_OK, "orphan set");
  check(capnp_builder_slot(&body, 0, 0, &slot0) == CAPNP_OK, "slot0");
  check(capnp_builder_adopt(&slot0, &orphan) == CAPNP_OK, "adopt");
  check(orphan.live == 0, "adopted");
  check(capnp_builder_disown(&slot0, &orphan) == CAPNP_OK, "disown");
  check(capnp_builder_adopt(&slot0, &orphan) == CAPNP_OK, "readopt");
  check(capnp_builder_serialize(&via, &fb, &lb) == CAPNP_OK, "ser b");

  check(capnp_message_from_flat(&ma, fa, la) == CAPNP_OK, "parse a");
  check(capnp_message_from_flat(&mb, fb, lb) == CAPNP_OK, "parse b");
  check(capnp_canonicalize(&ma, &ca, &lca) == CAPNP_OK, "canon a");
  check(capnp_canonicalize(&mb, &cb, &lcb) == CAPNP_OK, "canon b");
  /* Both trees are one struct pointer to a 1-word struct holding 7. The
   * extra null pointer in the second message is truncated by canonical form. */
  check(lca == lcb && memcmp(ca, cb, lca) == 0, "canonical match");

  free(fa);
  free(fb);
  free(ca);
  free(cb);
  capnp_message_free(&ma);
  capnp_message_free(&mb);
  capnp_builder_free(&direct);
  capnp_builder_free(&via);
}

static void test_list_orphan_roundtrip(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, start;
  capnp_orphan_t orphan;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list;
  const char *text = NULL;
  size_t n = 0;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "body");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_BYTE, 6, &orphan, &start) ==
            CAPNP_OK,
        "list orphan");
  memcpy(b.segs[start.seg].data + start.word * 8, "hello", 6);
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "slot");
  check(capnp_builder_adopt(&slot, &orphan) == CAPNP_OK, "adopt list");
  check(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  check(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "parse");
  free(flat);
  check(capnp_root(&m, &r) == CAPNP_OK, "root read");
  check(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  check(capnp_get_text(&r, 0, &text, &n) == CAPNP_OK, "text");
  check(n == 5 && text && memcmp(text, "hello", 5) == 0, "hello");
  capnp_message_free(&m);
}

static void test_struct_list_orphan(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, first;
  capnp_orphan_t orphan;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, list, el;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "body");
  check(capnp_builder_new_orphan_struct_list(&b, 2, 1, 0, &orphan, &first) ==
            CAPNP_OK,
        "struct list");
  check(capnp_builder_set_u64(&first, 0, 3) == CAPNP_OK, "elem0");
  first.word += 1;
  check(capnp_builder_set_u64(&first, 0, 9) == CAPNP_OK, "elem1");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "slot");
  check(capnp_builder_adopt(&slot, &orphan) == CAPNP_OK, "adopt composite");
  check(capnp_builder_disown(&slot, &orphan) == CAPNP_OK, "disown composite");
  check(orphan.esize == CAPNP_SZ_COMPOSITE, "esize");
  check(capnp_builder_adopt(&slot, &orphan) == CAPNP_OK, "readopt composite");
  check(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  check(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "parse");
  free(flat);
  check(capnp_root(&m, &r) == CAPNP_OK, "root read");
  check(capnp_getp(&r, 0, &list) == CAPNP_OK, "list");
  check(capnp_list_len(&list) == 2, "len");
  check(capnp_list_getp(&list, 0, &el) == CAPNP_OK, "el0");
  check(capnp_get_u64(&el, 0, 0) == 3, "3");
  check(capnp_list_getp(&list, 1, &el) == CAPNP_OK, "el1");
  check(capnp_get_u64(&el, 0, 0) == 9, "9");
  capnp_message_free(&m);
}

static void test_disown_double_far(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, obj;
  capnp_orphan_t o;
  uint64_t w, pad0;
  uint32_t pseg;
  size_t poff;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m;
  capnp_ptr_t r, k;

  capnp_builder_init_sized(&b, 3);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "df root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "df outer");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "df slot");
  check(capnp_builder_struct(&slot, 2, 0, &obj) == CAPNP_OK, "df spill");
  memset((void *)(uintptr_t)(capnp_builder_seg_data(&b, slot.seg) +
                             slot.word * 8),
         0, 8);
  capnp_builder_set_max_seg_words(&b, 1);
  check(capnp_builder_struct(&slot, 1, 0, &obj) == CAPNP_OK, "df obj");
  check(capnp_builder_set_u64(&obj, 0, 4242) == CAPNP_OK, "df set");
  w = capnp_load_le64(capnp_builder_seg_data(&b, slot.seg) + slot.word * 8);
  check(capnp_wp_kind(w) == CAPNP_WK_FAR && capnp_wp_far_two(w) == 1, "df far");
  pseg = capnp_wp_far_seg(w);
  poff = capnp_wp_far_off(w);
  pad0 = capnp_load_le64(capnp_builder_seg_data(&b, pseg) + poff * 8);
  check(capnp_builder_disown(&slot, &o) == CAPNP_OK, "df disown");
  w = capnp_load_le64(capnp_builder_seg_data(&b, slot.seg) + slot.word * 8);
  check(w == 0, "df slot cleared");
  check(capnp_load_le64(capnp_builder_seg_data(&b, pseg) + poff * 8) == pad0,
        "df pad kept");
  check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "df adopt");
  check(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "df ser");
  capnp_builder_free(&b);
  check(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "df parse");
  free(flat);
  check(capnp_root(&m, &r) == CAPNP_OK && capnp_getp(&r, 0, &k) == CAPNP_OK,
        "df read");
  check(k.kind == CAPNP_PK_STRUCT && capnp_get_u64(&k, 0, 0) == 4242, "df 4242");
  capnp_message_free(&m);
}

static void test_empty_builder_orphan(void) {
  capnp_builder_t b;
  capnp_orphan_t o;
  memset(&b, 0, sizeof(b));
  check(capnp_builder_new_orphan_struct(&b, 0, 0, &o, NULL) == CAPNP_ERR_ALLOC,
        "zero struct on empty builder");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_VOID, 0, &o, NULL) ==
            CAPNP_ERR_ALLOC,
        "zero list on empty builder");
  capnp_builder_init(&b);
  check(capnp_builder_new_orphan_struct(&b, 0, 0, &o, NULL) == CAPNP_OK,
        "zero struct on live builder");
  check(o.live == 1, "struct orphan live");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_VOID, 0, &o, NULL) ==
            CAPNP_OK,
        "zero list on live builder");
  capnp_builder_free(&b);
}

int main(void) {
  test_adopt_matches_direct();
  test_list_orphan_roundtrip();
  test_struct_list_orphan();
  test_empty_builder_orphan();
  test_disown_double_far();
  if (g_fail) {
    fprintf(stderr, "%d orphan checks failed\n", g_fail);
    return 1;
  }
  printf("ok orphan\n");
  return 0;
}
