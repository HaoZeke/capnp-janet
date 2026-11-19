#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static int contains_bytes(const uint8_t *buf, size_t n, const char *s);

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
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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

/* 1 when the slot is a double-far pointer. *coff_out is the target word. */
static int double_far_coff(const capnp_builder_t *b, uint32_t slot_seg,
                           size_t slot_word, size_t *coff_out) {
  uint64_t w, pad;
  uint32_t pseg;
  size_t poff;
  const uint8_t *data;
  w = capnp_load_le64(capnp_builder_seg_data(b, slot_seg) + slot_word * 8);
  if (capnp_wp_kind(w) != CAPNP_WK_FAR || capnp_wp_far_two(w) != 1)
    return 0;
  pseg = capnp_wp_far_seg(w);
  poff = capnp_wp_far_off(w);
  data = capnp_builder_seg_data(b, pseg);
  if (!data || poff + 1u >= capnp_builder_seg_words(b, pseg))
    return 0;
  pad = capnp_load_le64(data + poff * 8);
  if (capnp_wp_kind(pad) != CAPNP_WK_FAR || capnp_wp_far_two(pad))
    return 0;
  *coff_out = capnp_wp_far_off(pad);
  return 1;
}

/* A text that fills a fresh segment starts at word 0 and is double-far.
 * Clearing that slot must drop the bytes. */
static void test_dfar_clear_at_zero(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot;
  uint8_t *flat = NULL;
  size_t n = 0, coff = 1;

  capnp_builder_init_sized(&b, 2);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "dc root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "dc body");
  capnp_builder_set_max_seg_words(&b, 3);
  check(capnp_builder_set_text(&body, 0, 0, "SECRET_DFAR_TEXT", 16) == CAPNP_OK,
        "dc text");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "dc slot");
  check(double_far_coff(&b, slot.seg, slot.word, &coff) && coff == 0,
        "dc coff 0");
  check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "dc clear");
  check(capnp_builder_serialize(&b, &flat, &n) == CAPNP_OK, "dc ser");
  capnp_builder_free(&b);
  check(!contains_bytes(flat, n, "SECRET_DFAR_TEXT"), "dc secret gone");
  free(flat);
}

/* Adopting over that same slot must drop the text before linking. */
static void test_dfar_adopt_at_zero(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, obody;
  capnp_orphan_t orphan;
  uint8_t *flat = NULL;
  size_t n = 0, coff = 1;
  capnp_message_t m = {0};
  capnp_ptr_t decoded, child;

  memset(&m, 0, sizeof(m));
  capnp_builder_init_sized(&b, 2);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "da root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "da body");
  capnp_builder_set_max_seg_words(&b, 3);
  check(capnp_builder_set_text(&body, 0, 0, "SECRET_DFAR_ADOPT", 17) ==
            CAPNP_OK,
        "da text");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "da slot");
  check(double_far_coff(&b, slot.seg, slot.word, &coff) && coff == 0,
        "da coff 0");
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &orphan, &obody) == CAPNP_OK,
        "da orphan");
  check(capnp_builder_set_u64(&obody, 0, 9) == CAPNP_OK, "da set");
  check(capnp_builder_adopt(&slot, &orphan) == CAPNP_OK, "da adopt");
  check(capnp_builder_serialize(&b, &flat, &n) == CAPNP_OK, "da ser");
  capnp_builder_free(&b);
  check(!contains_bytes(flat, n, "SECRET_DFAR_ADOPT"), "da secret gone");
  check(capnp_message_from_flat(&m, flat, n) == CAPNP_OK, "da parse");
  free(flat);
  check(capnp_root(&m, &decoded) == CAPNP_OK, "da read");
  check(capnp_getp(&decoded, 0, &child) == CAPNP_OK, "da child");
  check(child.kind == CAPNP_PK_STRUCT && capnp_get_u64(&child, 0, 0) == 9,
        "da value");
  capnp_message_free(&m);
}

/* A struct at word 0 of a fresh segment, holding a text, must lose
 * both itself and the child when the slot is cleared. */
static void test_dfar_struct_child_at_zero(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, child;
  uint8_t *flat = NULL;
  size_t n = 0, coff = 1;

  capnp_builder_init_sized(&b, 2);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "dk root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "dk body");
  capnp_builder_set_max_seg_words(&b, 1);
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "dk slot");
  check(capnp_builder_struct(&slot, 0, 1, &child) == CAPNP_OK, "dk struct");
  check(double_far_coff(&b, slot.seg, slot.word, &coff) && coff == 0,
        "dk coff 0");
  capnp_builder_set_max_seg_words(&b, 4);
  check(capnp_builder_set_text(&child, 0, 0, "SECRET_DFAR_KID", 15) == CAPNP_OK,
        "dk text");
  check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "dk clear");
  check(capnp_builder_serialize(&b, &flat, &n) == CAPNP_OK, "dk ser");
  capnp_builder_free(&b);
  check(!contains_bytes(flat, n, "SECRET_DFAR_KID"), "dk secret gone");
  free(flat);
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

static int contains_bytes(const uint8_t *buf, size_t n, const char *s) {
  size_t len = strlen(s);
  size_t i;
  if (len == 0 || n < len)
    return 0;
  for (i = 0; i + len <= n; i++) {
    if (memcmp(buf + i, s, len) == 0)
      return 1;
  }
  return 0;
}

/* Clearing a tail text and then allocating a struct must not leave the
 * text bytes in the new struct's pointer word. */
static void test_reclaim_zeroes_tail(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, child;
  uint8_t *flat = NULL;
  size_t n = 0;
  capnp_message_t m = {0};
  capnp_ptr_t decoded, inner, leaf;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "rz root");
  check(capnp_builder_struct(&root, 0, 2, &body) == CAPNP_OK, "rz body");
  check(capnp_builder_set_text(&body, 0, 0, "ABCD1234", 8) == CAPNP_OK,
        "rz text");
  check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "rz clear");
  check(capnp_builder_slot(&body, 0, 1, &slot) == CAPNP_OK, "rz slot");
  check(capnp_builder_struct(&slot, 0, 1, &child) == CAPNP_OK, "rz child");
  check(capnp_builder_serialize(&b, &flat, &n) == CAPNP_OK, "rz ser");
  check(!contains_bytes(flat, n, "ABCD1234"), "rz text gone");
  check(capnp_message_from_flat(&m, flat, n) == CAPNP_OK, "rz parse");
  free(flat);
  check(capnp_root(&m, &decoded) == CAPNP_OK, "rz root read");
  check(capnp_getp(&decoded, 1, &inner) == CAPNP_OK, "rz child read");
  check(inner.kind == CAPNP_PK_STRUCT, "rz child struct");
  check(capnp_getp(&inner, 0, &leaf) == CAPNP_OK, "rz leaf");
  check(leaf.kind == CAPNP_PK_NULL, "rz leaf null");
  capnp_message_free(&m);
  capnp_builder_free(&b);
}

/* A pointer that is not the segment tail is still zeroed when replaced. */
static void test_overwrite_zeroes_nontail(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL;
  size_t n = 0;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "nt root");
  check(capnp_builder_struct(&root, 0, 2, &body) == CAPNP_OK, "nt body");
  check(capnp_builder_set_text(&body, 0, 0, "SECRET_BYTES_123", 16) == CAPNP_OK,
        "nt secret");
  check(capnp_builder_set_text(&body, 0, 1, "abc", 3) == CAPNP_OK, "nt later");
  check(capnp_builder_set_text(&body, 0, 0, "ok", 2) == CAPNP_OK, "nt replace");
  check(capnp_builder_serialize(&b, &flat, &n) == CAPNP_OK, "nt ser");
  check(!contains_bytes(flat, n, "SECRET_BYTES_123"), "nt secret gone");
  check(contains_bytes(flat, n, "ok"), "nt new text");
  free(flat);
  capnp_builder_free(&b);
}

/* A text, an empty list aimed at the segment end, then a shorter text.
 * Reclaiming the first text must not leave the list past the segment. */
static void test_empty_list_after_reclaim(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL;
  size_t n = 0;
  capnp_message_t m = {0};
  capnp_ptr_t decoded, list;
  const char *text = NULL;
  size_t tlen = 0;

  memset(&m, 0, sizeof(m));
  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "el root");
  check(capnp_builder_struct(&root, 0, 2, &body) == CAPNP_OK, "el body");
  check(capnp_builder_set_text(&body, 0, 0, "hello world!!!!!", 16) == CAPNP_OK,
        "el first");
  check(capnp_builder_set_list_u32(&body, 0, 1, NULL, 0) == CAPNP_OK, "el list");
  check(capnp_builder_set_text(&body, 0, 0, "a", 1) == CAPNP_OK, "el second");
  {
    uint64_t lw;
    int64_t at;
    size_t list_word = body.word + 1u;
    const uint8_t *seg = capnp_builder_seg_data(&b, body.seg);
    check(seg != NULL, "el seg");
    lw = capnp_load_le64(seg + list_word * 8);
    check(capnp_wp_kind(lw) == CAPNP_WK_LIST, "el wire list");
    at = (int64_t)list_word + 1 + capnp_wp_offset(lw);
    /* A zero-word list may name the segment end. Past that end the
     * pointer is out of bounds even though a clamping reader still
     * reports an empty list. */
    check(at >= 0 && (size_t)at <= capnp_builder_seg_words(&b, body.seg),
          "el list within segment");
  }
  check(capnp_builder_serialize(&b, &flat, &n) == CAPNP_OK, "el ser");
  capnp_builder_free(&b);
  check(!contains_bytes(flat, n, "hello world!!!!!"), "el old text cleared");
  check(capnp_message_from_flat(&m, flat, n) == CAPNP_OK, "el parse");
  free(flat);
  check(capnp_root(&m, &decoded) == CAPNP_OK, "el read");
  check(capnp_get_text(&decoded, 0, &text, &tlen) == CAPNP_OK, "el text");
  check(tlen == 1 && text && text[0] == 'a', "el text is a");
  check(capnp_getp(&decoded, 1, &list) == CAPNP_OK, "el get list");
  check(list.kind == CAPNP_PK_LIST && list.count == 0, "el empty list");
  capnp_message_free(&m);
}

/* checkOffset clamps a target past the segment to the end. A zero-word
 * list there is empty; a list that occupies words is not. */
static void test_zero_list_past_end(void) {
  uint8_t flat[16];
  capnp_message_t m = {0};
  capnp_ptr_t root;
  uint64_t past, real;

  memset(&m, 0, sizeof(m));
  memset(flat, 0, sizeof(flat));
  capnp_store_le32(flat, 0);
  capnp_store_le32(flat + 4, 1);
  past = capnp_wp_make_list(8, CAPNP_SZ_FOUR, 0);
  capnp_store_le64(flat + 8, past);
  check(capnp_message_from_flat(&m, flat, sizeof(flat)) == CAPNP_OK,
        "past parse");
  check(capnp_root(&m, &root) == CAPNP_OK, "past root");
  check(root.kind == CAPNP_PK_LIST && root.count == 0, "past empty");
  capnp_message_free(&m);

  real = capnp_wp_make_list(8, CAPNP_SZ_FOUR, 1);
  capnp_store_le64(flat + 8, real);
  check(capnp_message_from_flat(&m, flat, sizeof(flat)) == CAPNP_OK,
        "past parse again");
  check(capnp_root(&m, &root) == CAPNP_ERR_BOUNDS, "nonzero past end");
  capnp_message_free(&m);
}

/* adopt over an occupied slot zeroes the object the slot used to name. */
static void test_adopt_zeroes_occupied(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, obody;
  capnp_orphan_t orphan;
  uint8_t *flat = NULL;
  size_t n = 0;
  capnp_message_t m = {0};
  capnp_ptr_t decoded, child;

  memset(&m, 0, sizeof(m));
  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "az root");
  check(capnp_builder_struct(&root, 0, 2, &body) == CAPNP_OK, "az body");
  check(capnp_builder_set_text(&body, 0, 0, "SECRET_ADOPT_OVERWRITE", 22) ==
            CAPNP_OK,
        "az secret");
  check(capnp_builder_set_text(&body, 0, 1, "later", 5) == CAPNP_OK, "az later");
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &orphan, &obody) == CAPNP_OK,
        "az orphan");
  check(capnp_builder_set_u64(&obody, 0, 1) == CAPNP_OK, "az set");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "az slot");
  check(capnp_builder_adopt(&slot, &orphan) == CAPNP_OK, "az adopt");
  check(capnp_builder_serialize(&b, &flat, &n) == CAPNP_OK, "az ser");
  capnp_builder_free(&b);
  check(!contains_bytes(flat, n, "SECRET_ADOPT_OVERWRITE"), "az secret gone");
  check(contains_bytes(flat, n, "later"), "az later kept");
  check(capnp_message_from_flat(&m, flat, n) == CAPNP_OK, "az parse");
  free(flat);
  check(capnp_root(&m, &decoded) == CAPNP_OK, "az read");
  check(capnp_getp(&decoded, 0, &child) == CAPNP_OK, "az child");
  check(child.kind == CAPNP_PK_STRUCT, "az struct");
  check(capnp_get_u64(&child, 0, 0) == 1, "az value");
  capnp_message_free(&m);
}

/* An orphan that is freed rather than adopted must not keep its text. */
static void test_orphan_free_zeroes(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, obody;
  capnp_orphan_t orphan;
  uint8_t *flat = NULL;
  size_t n = 0;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "of root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "of body");
  check(capnp_builder_set_text(&body, 0, 0, "keep", 4) == CAPNP_OK, "of keep");
  check(capnp_builder_new_orphan_struct(&b, 2, 1, &orphan, &obody) == CAPNP_OK,
        "of orphan");
  memcpy(b.segs[obody.seg].data + obody.word * 8, "DATAWORD_SECRET!", 16);
  check(capnp_builder_set_text(&obody, 2, 0, "SECRET_ORPHAN", 13) == CAPNP_OK,
        "of secret");
  check(capnp_builder_orphan_free(&orphan) == CAPNP_OK, "of free");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "of slot");
  check(capnp_builder_adopt(&slot, &orphan) == CAPNP_ERR_ARG, "of dead");
  check(capnp_builder_serialize(&b, &flat, &n) == CAPNP_OK, "of ser");
  capnp_builder_free(&b);
  check(!contains_bytes(flat, n, "DATAWORD_SECRET!"), "of data gone");
  check(!contains_bytes(flat, n, "SECRET_ORPHAN"), "of secret gone");
  check(contains_bytes(flat, n, "keep"), "of keep stays");
  free(flat);
}

/* A composite list of zero-size structs is a tag word and no element
 * body. C++ 1.4.0 encodes that, and set_list_struct already does too. */
static void test_zero_size_struct_list_orphan(void) {
  capnp_builder_t b, direct;
  capnp_bptr_t root, body, slot, first, droot, dbody, dfirst;
  capnp_orphan_t orphan;
  uint8_t *flat = NULL, *dflat = NULL;
  size_t flen = 0, dlen = 0;
  capnp_message_t m = {0};
  capnp_ptr_t r, list;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "z root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "z body");
  check(capnp_builder_new_orphan_struct_list(&b, 3, 0, 0, &orphan, &first) ==
            CAPNP_OK,
        "z orphan");
  check(orphan.live == 1, "z live");
  check(orphan.esize == CAPNP_SZ_COMPOSITE, "z esize");
  check(orphan.count == 0, "z content words");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "z slot");
  check(capnp_builder_adopt(&slot, &orphan) == CAPNP_OK, "z adopt");
  check(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "z ser");
  capnp_builder_free(&b);

  capnp_builder_init(&direct);
  check(capnp_builder_root(&direct, &droot) == CAPNP_OK, "z droot");
  check(capnp_builder_struct(&droot, 0, 1, &dbody) == CAPNP_OK, "z dbody");
  check(capnp_builder_set_list_struct(&dbody, 0, 0, 3, 0, 0, &dfirst) ==
            CAPNP_OK,
        "z direct");
  check(capnp_builder_serialize(&direct, &dflat, &dlen) == CAPNP_OK, "z dser");
  capnp_builder_free(&direct);
  check(flen == dlen && flat && dflat && memcmp(flat, dflat, flen) == 0,
        "z matches set_list_struct");

  check(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "z parse");
  free(flat);
  free(dflat);
  check(capnp_root(&m, &r) == CAPNP_OK, "z read");
  check(capnp_getp(&r, 0, &list) == CAPNP_OK, "z list");
  check(list.kind == CAPNP_PK_LIST, "z kind");
  check(capnp_list_len(&list) == 3, "z len");
  capnp_message_free(&m);
}

int main(void) {
  test_adopt_matches_direct();
  test_list_orphan_roundtrip();
  test_struct_list_orphan();
  test_zero_size_struct_list_orphan();
  test_empty_builder_orphan();
  test_disown_double_far();
  test_dfar_clear_at_zero();
  test_dfar_adopt_at_zero();
  test_dfar_struct_child_at_zero();
  test_reclaim_zeroes_tail();
  test_overwrite_zeroes_nontail();
  test_empty_list_after_reclaim();
  test_zero_list_past_end();
  test_adopt_zeroes_occupied();
  test_orphan_free_zeroes();
  if (g_fail) {
    fprintf(stderr, "%d orphan checks failed\n", g_fail);
    return 1;
  }
  printf("ok orphan\n");
  return 0;
}
