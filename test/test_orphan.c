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

/* A copied orphan shares one object. Freeing or adopting one copy
 * must make the other copy a no-op, so a later object in those words
 * is left alone. */
static void test_copied_orphan_second_free(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, slot, obody;
  capnp_orphan_t orphan, copy;
  uint64_t got;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "c root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "c body");
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &orphan, &obody) == CAPNP_OK,
        "c orphan");
  copy = orphan;
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "c slot");
  check(capnp_builder_adopt(&slot, &orphan) == CAPNP_OK, "c adopt");
  check(orphan.live == 0, "c original retired");
  check(copy.live == 1, "c copy still marked live");
  check(capnp_builder_set_u64(&obody, 0, 0xabcdef) == CAPNP_OK, "c sentinel");
  check(capnp_builder_orphan_free(&copy) == CAPNP_ERR_ARG, "c second free");
  got = capnp_load_le64(b.segs[obody.seg].data + obody.word * 8);
  check(got == 0xabcdef, "c sentinel kept");
  check(capnp_builder_orphan_free(&orphan) == CAPNP_ERR_ARG, "c free original");
  got = capnp_load_le64(b.segs[obody.seg].data + obody.word * 8);
  check(got == 0xabcdef, "c sentinel kept again");
  capnp_builder_free(&b);
}

/* A caller may reconstruct an orphan with a designated initializer. It has
 * no access to padding, so this verifies that identity is external. */
static void test_orphan_copy_without_padding(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot, obody;
  capnp_orphan_t orphan = {0};
  capnp_orphan_t copy;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "p root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "p body");
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &orphan, &obody) == CAPNP_OK,
        "p orphan");
  copy = (capnp_orphan_t){
      .b = orphan.b,
      .seg = orphan.seg,
      .word = orphan.word,
      .kind = orphan.kind,
      .dwords = orphan.dwords,
      .pwords = orphan.pwords,
      .esize = orphan.esize,
      .count = orphan.count,
      .live = orphan.live,
  };
  check(capnp_builder_set_u64(&obody, 0, 7) == CAPNP_OK, "p value");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "p slot");
  check(capnp_builder_adopt(&slot, &copy) == CAPNP_OK, "p adopt copy");
  check(capnp_builder_orphan_free(&orphan) == CAPNP_ERR_ARG, "p original dead");
  capnp_builder_free(&b);
}

/* Freeing an orphan and allocating another must not let a stale copy of
 * the first orphan free or retire the second. */
static void test_stale_copy_after_free_and_realloc(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot, b1, b2;
  capnp_orphan_t o1 = {0}, copy = {0}, o2 = {0};
  uint64_t got;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "fr root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "fr body");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "fr slot");
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &o1, &b1) == CAPNP_OK,
        "fr o1");
  copy = o1;
  check(capnp_builder_orphan_free(&o1) == CAPNP_OK, "fr free o1");
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &o2, &b2) == CAPNP_OK,
        "fr o2");
  check(capnp_builder_set_u64(&b2, 0, 0xCAFEBABE12345678ULL) == CAPNP_OK,
        "fr set o2");
  check(capnp_builder_orphan_free(&copy) == CAPNP_ERR_ARG, "fr stale copy");
  got = capnp_load_le64(b.segs[b2.seg].data + b2.word * 8);
  check(got == 0xCAFEBABE12345678ULL, "fr o2 kept");
  check(capnp_builder_adopt(&slot, &o2) == CAPNP_OK, "fr adopt o2");
  capnp_builder_free(&b);
}

/* A by-value copy of an adopted orphan is not a handle C++ allows: its
 * Orphan is move-only. When the clear rewinds the tail and a later orphan
 * lands on the same words, the copy and the new orphan name one record. At
 * most one of them may succeed, and nothing outside that object is
 * touched. */
static void test_stale_copy_after_adopt_clear_realloc(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot, b1, b2, other;
  capnp_orphan_t o1 = {0}, copy = {0}, o2 = {0};
  int rc_copy, rc_o2;
  uint64_t got;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "ac root");
  check(capnp_builder_struct(&root, 0, 2, &body) == CAPNP_OK, "ac body");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "ac slot");
  check(capnp_builder_slot(&body, 0, 1, &other) == CAPNP_OK, "ac other");
  check(capnp_builder_set_text(&body, 0, 1, "keep", 4) == CAPNP_OK, "ac text");
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &o1, &b1) == CAPNP_OK,
        "ac o1");
  copy = o1;
  check(capnp_builder_adopt(&slot, &o1) == CAPNP_OK, "ac adopt o1");
  check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "ac clear");
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &o2, &b2) == CAPNP_OK,
        "ac o2");
  check(capnp_builder_set_u64(&b2, 0, 0x13572468ACE0BDF1ULL) == CAPNP_OK,
        "ac set o2");
  rc_copy = capnp_builder_orphan_free(&copy);
  got = capnp_load_le64(b.segs[b2.seg].data + b2.word * 8);
  if (rc_copy == CAPNP_OK)
    check(got == 0, "ac aliased free zeroes only the new orphan");
  else
    check(rc_copy == CAPNP_ERR_ARG && got == 0x13572468ACE0BDF1ULL,
          "ac inert copy leaves o2");
  rc_o2 = capnp_builder_orphan_free(&o2);
  check((rc_copy == CAPNP_OK) != (rc_o2 == CAPNP_OK), "ac exactly one frees");
  check(capnp_builder_slot(&body, 0, 1, &other) == CAPNP_OK, "ac other again");
  capnp_builder_free(&b);
}

/* Orphan records that are gone do not keep a reclaim pin. A loop that
 * disowns, adopts and clears the same slot must not grow the arena. */
static size_t total_words(capnp_builder_t *b) {
  size_t t = 0;
  uint32_t i;
  for (i = 0; i < capnp_builder_nsegs(b); i++)
    t += capnp_builder_seg_words(b, i);
  return t;
}

static void test_retired_orphan_keeps_no_pin(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot, child;
  capnp_orphan_t o = {0};
  size_t base;
  int i;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "pn root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "pn body");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "pn slot");
  check(capnp_builder_struct(&slot, 2, 2, &child) == CAPNP_OK, "pn child");
  check(capnp_builder_disown(&slot, &o) == CAPNP_OK, "pn warm disown");
  check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "pn warm adopt");
  check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "pn warm clear");
  base = total_words(&b);
  for (i = 0; i < 2000; i++) {
    check(capnp_builder_struct(&slot, 2, 2, &child) == CAPNP_OK, "pn init");
    check(capnp_builder_disown(&slot, &o) == CAPNP_OK, "pn disown");
    check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "pn adopt");
    check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "pn clear");
    check(capnp_builder_new_orphan_struct(&b, 2, 2, &o, NULL) == CAPNP_OK,
          "pn new");
    check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "pn new adopt");
    check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "pn new clear");
  }
  check(total_words(&b) <= base, "pn arena did not grow");
  check(capnp_builder_nsegs(&b) == 1, "pn one segment");
  capnp_builder_free(&b);
}

/* Counts that wrap size_t on ILP32 must fail instead of allocating zero
 * words for an object that claims billions of them. */
static void test_huge_counts_rejected(void) {
  capnp_builder_t b = {0};
  capnp_orphan_t o = {0}, fill = {0};
  capnp_bptr_t first;

  capnp_builder_init_sized(&b, 1);
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_EIGHT, 1, &fill, &first) ==
            CAPNP_OK,
        "hc fill");
  check(capnp_builder_new_orphan_struct_list(&b, 0xFFFFFFFFu, 1, 0, &o,
                                             &first) != CAPNP_OK,
        "hc struct list");
  check(o.live == 0, "hc struct list dead");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_BYTE, 0xFFFFFFFFu, &o,
                                      &first) != CAPNP_OK,
        "hc byte list");
  check(o.live == 0, "hc byte list dead");
  capnp_builder_free(&b);
}

/* The wire list count is 29 bits, including lists that need no content. */
static void test_wire_list_count_rejected(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot;
  capnp_orphan_t o = {0};

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "wc root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "wc body");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "wc slot");
  check(capnp_builder_new_orphan_list(
            &b, CAPNP_SZ_VOID, CAPNP_BUILDER_MAX_SEGMENT_WORDS + 1u, &o,
            NULL) == CAPNP_ERR_BOUNDS,
        "wc orphan");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_BIT, 1u << 29, &o, NULL) ==
            CAPNP_ERR_BOUNDS,
        "wc bit over");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_VOID, 0xFFFFFFFFu, &o,
                                      NULL) == CAPNP_ERR_BOUNDS,
        "wc void wide");
  check(capnp_builder_nsegs(&b) == 1, "wc list one seg");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_VOID,
                                      CAPNP_BUILDER_MAX_SEGMENT_WORDS, &o,
                                      NULL) == CAPNP_OK,
        "wc void max");
  check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "wc adopt");
  /* The composite tag holds the element count. 2^30 is masked to zero
   * by the 30-bit offset field, so creation itself has to fail. */
  check(capnp_builder_new_orphan_struct_list(
            &b, CAPNP_BUILDER_MAX_SEGMENT_WORDS + 1u, 0, 0, &o, NULL) ==
            CAPNP_ERR_BOUNDS,
        "wc struct over");
  check(o.live == 0, "wc struct over dead");
  check(capnp_builder_new_orphan_struct_list(&b, 1u << 30, 0, 0, &o, NULL) ==
            CAPNP_ERR_BOUNDS,
        "wc struct mask");
  check(o.live == 0, "wc struct mask dead");
  check(capnp_builder_new_orphan_struct_list(
            &b, CAPNP_BUILDER_MAX_SEGMENT_WORDS, 0, 0, &o, NULL) == CAPNP_OK,
        "wc struct max");
  check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "wc struct adopt");
  {
    const uint8_t *data = capnp_builder_seg_data(&b, slot.seg);
    uint64_t lw = capnp_load_le64(data + slot.word * CAPNP_WORD_BYTES);
    int64_t at = (int64_t)slot.word + 1 + capnp_wp_offset(lw);
    uint64_t tag;
    check(at >= 0 && (size_t)at < capnp_builder_seg_words(&b, slot.seg),
          "wc tag in seg");
    tag = capnp_load_le64(data + (size_t)at * CAPNP_WORD_BYTES);
    check(capnp_wp_offset(tag) == (int32_t)CAPNP_BUILDER_MAX_SEGMENT_WORDS,
          "wc tag count");
  }
  /* text_len + NUL is the list count. 2^29-1 bytes of text do not fit,
   * and the refusal must not open another segment. */
  check(capnp_builder_set_text(&body, 0, 0, "",
                               (size_t)CAPNP_BUILDER_MAX_SEGMENT_WORDS) ==
            CAPNP_ERR_BOUNDS,
        "wc text");
  check(capnp_builder_nsegs(&b) == 1, "wc text one seg");
  capnp_builder_free(&b);
}

/* A stale orphan referencing a freed builder must fail cleanly. */
static void test_stale_orphan_after_builder_free(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t b1;
  capnp_orphan_t o1 = {0};

  capnp_builder_init(&b);
  check(capnp_builder_new_orphan_struct(&b, 1, 0, &o1, &b1) == CAPNP_OK,
        "bf o1");
  capnp_builder_free(&b);
  check(capnp_builder_orphan_free(&o1) == CAPNP_ERR_ARG, "bf stale free");
}

/* Adopting a zero-word orphan list over an occupied tail slot must keep
 * the written list pointer within the segment bounds. */
static void test_orphan_adopt_zero_list_over_tail(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot;
  capnp_orphan_t o = {0};
  uint64_t lw;
  int64_t at;

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "zl root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "zl body");
  check(capnp_builder_set_text(&body, 0, 0, "hello world!!!!!", 16) == CAPNP_OK,
        "zl text");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_FOUR, 0, &o, NULL) ==
            CAPNP_OK,
        "zl orphan");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "zl slot");
  check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "zl adopt");
  lw = capnp_load_le64(capnp_builder_seg_data(&b, slot.seg) + slot.word * 8);
  at = (int64_t)slot.word + 1 + capnp_wp_offset(lw);
  check(at >= 0 && (size_t)at <= capnp_builder_seg_words(&b, slot.seg),
        "zl within segment");
  capnp_builder_free(&b);
}

/* Two concurrent zero-size orphans retire independently by owner variable. */
static void test_concurrent_zero_size_orphans(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot;
  capnp_orphan_t o1 = {0}, o2 = {0};

  capnp_builder_init(&b);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "cz root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "cz body");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "cz slot");
  check(capnp_builder_new_orphan_struct(&b, 0, 0, &o1, NULL) == CAPNP_OK,
        "cz o1");
  check(capnp_builder_new_orphan_struct(&b, 0, 0, &o2, NULL) == CAPNP_OK,
        "cz o2");
  check(capnp_builder_orphan_free(&o1) == CAPNP_OK, "cz free o1");
  check(capnp_builder_orphan_free(&o1) == CAPNP_ERR_ARG, "cz refree o1");
  check(capnp_builder_adopt(&slot, &o2) == CAPNP_OK, "cz adopt o2");
  check(capnp_builder_orphan_free(&o2) == CAPNP_ERR_ARG, "cz free o2 dead");
  capnp_builder_free(&b);
}

/* A live zero-size orphan anchored on a later segment must survive when
 * an earlier object in that segment is cleared. */
static void test_zero_orphan_on_second_segment(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot;
  capnp_orphan_t o = {0};

  capnp_builder_init_sized(&b, 2);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "zs root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "zs body");
  check(capnp_builder_set_text(&body, 0, 0, "spill_to_second_segment", 23) ==
            CAPNP_OK,
        "zs spill");
  check(capnp_builder_nsegs(&b) >= 2, "zs multi seg");
  check(capnp_builder_new_orphan_struct(&b, 0, 0, &o, NULL) == CAPNP_OK,
        "zs orphan");
  check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "zs clear");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "zs slot");
  check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "zs adopt");
  capnp_builder_free(&b);
}

/* The pin keeps an emptied later segment while a live orphan names it, so
 * free and adopt still find the segment after the clear. */
static void test_live_orphan_keeps_emptied_segment(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot;
  capnp_orphan_t o = {0}, l = {0};
  uint64_t lw;
  int64_t at;

  capnp_builder_init_sized(&b, 2);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "ks root");
  check(capnp_builder_struct(&root, 0, 2, &body) == CAPNP_OK, "ks body");
  check(capnp_builder_set_text(&body, 0, 0, "spill_to_second_segment", 23) ==
            CAPNP_OK,
        "ks spill");
  check(capnp_builder_nsegs(&b) >= 2, "ks multi seg");
  check(capnp_builder_new_orphan_struct(&b, 0, 0, &o, NULL) == CAPNP_OK,
        "ks struct orphan");
  check(capnp_builder_new_orphan_list(&b, CAPNP_SZ_FOUR, 0, &l, NULL) ==
            CAPNP_OK,
        "ks list orphan");
  check(capnp_builder_clear_ptr(&body, 0, 0) == CAPNP_OK, "ks clear");
  check(capnp_builder_orphan_free(&o) == CAPNP_OK, "ks free struct orphan");
  check(capnp_builder_slot(&body, 0, 1, &slot) == CAPNP_OK, "ks slot");
  check(capnp_builder_adopt(&slot, &l) == CAPNP_OK, "ks adopt list orphan");
  lw = capnp_load_le64(capnp_builder_seg_data(&b, slot.seg) + slot.word * 8);
  at = (int64_t)slot.word + 1 + capnp_wp_offset(lw);
  check(at >= 0 && (size_t)at <= capnp_builder_seg_words(&b, slot.seg),
        "ks list within segment");
  capnp_builder_free(&b);
}

/* Grow the side table past its initial capacity, churn orphans across
 * multiple segments, and free cleanly with zero leaks. */
static void test_orphan_side_table_churn(void) {
  capnp_builder_t b = {0};
  capnp_bptr_t root, body, slot, obody;
  capnp_orphan_t live[32];
  int i;

  capnp_builder_init_sized(&b, 4);
  check(capnp_builder_root(&b, &root) == CAPNP_OK, "ch root");
  check(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "ch body");
  check(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "ch slot");
  for (i = 0; i < 32; i++) {
    check(capnp_builder_new_orphan_struct(&b, 1, 0, &live[i], &obody) ==
              CAPNP_OK,
          "ch live");
    check(capnp_builder_set_u64(&obody, 0, (uint64_t)i) == CAPNP_OK, "ch set");
  }
  for (i = 31; i >= 0; i--) {
    check(capnp_builder_orphan_free(&live[i]) == CAPNP_OK, "ch free live");
    check(capnp_builder_orphan_free(&live[i]) == CAPNP_ERR_ARG, "ch dead");
  }
  for (i = 0; i < 200; i++) {
    capnp_orphan_t o = {0};
    check(capnp_builder_new_orphan_struct(&b, 1, 0, &o, &obody) == CAPNP_OK,
          "ch churn");
    if (i & 1)
      check(capnp_builder_adopt(&slot, &o) == CAPNP_OK, "ch adopt");
    else
      check(capnp_builder_orphan_free(&o) == CAPNP_OK, "ch free");
  }
  capnp_builder_free(&b);
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
  test_copied_orphan_second_free();
  test_orphan_copy_without_padding();
  test_stale_copy_after_free_and_realloc();
  test_stale_copy_after_adopt_clear_realloc();
  test_retired_orphan_keeps_no_pin();
  test_huge_counts_rejected();
  test_wire_list_count_rejected();
  test_stale_orphan_after_builder_free();
  test_orphan_adopt_zero_list_over_tail();
  test_concurrent_zero_size_orphans();
  test_zero_orphan_on_second_segment();
  test_live_orphan_keeps_emptied_segment();
  test_orphan_side_table_churn();
  if (g_fail) {
    fprintf(stderr, "%d orphan checks failed\n", g_fail);
    return 1;
  }
  printf("ok orphan\n");
  return 0;
}
