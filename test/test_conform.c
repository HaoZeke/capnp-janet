/* Read an official `capnp encode` of schema/conform.capnp and check that
 * canonical bytes do not depend on field-set order or far pointers. */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_packed.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CONFORM_D = 3, CONFORM_P = 7, INNER_D = 1, INNER_P = 1 };

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
  buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
  if (!buf || (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz)) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  *out = buf;
  *out_len = (size_t)sz;
  return 0;
}

static uint64_t f64_bits(double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

static void check_full(const capnp_ptr_t *root) {
  capnp_ptr_t inner, buckets, b0, b1, scores, matrix, row, blob;
  const char *s;
  size_t n;
  const uint8_t *data;
  uint32_t wide_wire, n_wire;
  uint64_t ratio_wire;

  CHECK(root->kind == CAPNP_PK_STRUCT, "struct");
  CHECK(root->dwords == CONFORM_D && root->pwords == CONFORM_P, "shape");
  CHECK(capnp_get_bool(root, 0, 0) == 1, "flag wire is default xor false");
  CHECK((int8_t)(capnp_get_u8(root, 1, 0) ^ (uint8_t)-5) == 3, "tiny");
  CHECK(capnp_get_u16(root, 2, 0xffff) == 1, "union note");
  wide_wire = capnp_get_u32(root, 4, 0);
  CHECK((wide_wire ^ 7u) == 99u, "wide");
  ratio_wire = capnp_get_u64(root, 8, 0);
  CHECK(ratio_wire == (f64_bits(2.5) ^ f64_bits(1.5)), "ratio");
  CHECK((capnp_get_u16(root, 16, 0) ^ 4u) == 9u, "rev");

  CHECK(capnp_get_text(root, 0, &s, &n) == CAPNP_OK, "title");
  CHECK_STREQ(s, n, "hi", "title text");
  CHECK(capnp_get_data(root, 1, &data, &n) == CAPNP_OK, "blob");
  CHECK(n == 3 && data[0] == 'a' && data[1] == 'b' && data[2] == 'c', "abc");
  CHECK(capnp_get_text(root, 5, &s, &n) == CAPNP_OK, "note");
  CHECK_STREQ(s, n, "hey", "note text");
  CHECK(capnp_get_text(root, 6, &s, &n) == CAPNP_OK, "comment");
  CHECK_STREQ(s, n, "c", "comment text");

  CHECK(capnp_getp(root, 2, &inner) == CAPNP_OK, "inner");
  n_wire = capnp_get_u32(&inner, 0, 0);
  CHECK((n_wire ^ (uint32_t)-3) == 8u, "inner n");
  CHECK(capnp_get_text(&inner, 0, &s, &n) == CAPNP_OK, "label");
  CHECK_STREQ(s, n, "in", "label text");

  CHECK(capnp_getp(root, 3, &buckets) == CAPNP_OK, "buckets");
  CHECK(capnp_list_len(&buckets) == 2, "two buckets");
  CHECK(capnp_list_getp(&buckets, 0, &b0) == CAPNP_OK, "b0");
  CHECK(capnp_get_text(&b0, 0, &s, &n) == CAPNP_OK, "b0 name");
  CHECK_STREQ(s, n, "a", "name a");
  CHECK(capnp_getp(&b0, 1, &scores) == CAPNP_OK, "scores");
  CHECK(capnp_list_len(&scores) == 2, "two scores");
  CHECK(capnp_list_get_u32(&scores, 0, 0) == 1, "score 1");
  CHECK(capnp_list_get_u32(&scores, 1, 0) == 2, "score 2");
  CHECK(capnp_list_getp(&buckets, 1, &b1) == CAPNP_OK, "b1");
  CHECK(capnp_get_text(&b1, 0, &s, &n) == CAPNP_OK, "b1 name");
  CHECK_STREQ(s, n, "b", "name b");
  CHECK(capnp_getp(&b1, 1, &scores) == CAPNP_OK, "b1 scores");
  CHECK(capnp_list_len(&scores) == 1 && capnp_list_get_u32(&scores, 0, 0) == 3,
        "score 3");

  CHECK(capnp_getp(root, 4, &matrix) == CAPNP_OK, "matrix");
  CHECK(matrix.kind == CAPNP_PK_LIST && matrix.esize == CAPNP_SZ_PTR, "rows");
  CHECK(capnp_list_len(&matrix) == 2, "two rows");
  CHECK(capnp_list_getp(&matrix, 0, &row) == CAPNP_OK, "row0");
  CHECK(capnp_list_len(&row) == 2, "row0 len");
  CHECK(capnp_list_get_text(&row, 0, &s, &n) == CAPNP_OK, "x");
  CHECK_STREQ(s, n, "x", "cell x");
  CHECK(capnp_list_get_text(&row, 1, &s, &n) == CAPNP_OK, "yy");
  CHECK_STREQ(s, n, "yy", "cell yy");
  CHECK(capnp_list_getp(&matrix, 1, &row) == CAPNP_OK, "row1");
  CHECK(capnp_list_len(&row) == 1, "row1 len");
  CHECK(capnp_list_get_text(&row, 0, &s, &n) == CAPNP_OK, "z");
  CHECK_STREQ(s, n, "z", "cell z");
  (void)blob;
}

static void check_empty(const capnp_ptr_t *root) {
  const char *s;
  size_t n;
  CHECK(capnp_get_bool(root, 0, 1) == 0, "flag wire zero");
  CHECK((int8_t)capnp_get_u8(root, 1, 0) == 0, "tiny wire zero");
  CHECK((capnp_get_u8(root, 1, 0) ^ (uint8_t)-5) == (uint8_t)-5, "tiny default");
  CHECK(capnp_get_u16(root, 2, 99) == 0, "quiet union");
  CHECK((capnp_get_u32(root, 4, 0) ^ 7u) == 7u, "wide default");
  CHECK(capnp_get_u64(root, 8, 0) == 0, "ratio wire zero");
  CHECK((capnp_get_u16(root, 16, 0) ^ 4u) == 4u, "rev default");
  CHECK(capnp_get_text(root, 0, &s, &n) == CAPNP_OK && n == 0, "null title");
}

static int read_mode(const char *path, int empty) {
  uint8_t *buf = NULL;
  size_t len = 0;
  capnp_message_t m = {0};
  capnp_ptr_t root;
  if (load_file(path, &buf, &len)) {
    fprintf(stderr, "cannot read %s\n", path);
    return 1;
  }
  if (capnp_message_from_flat(&m, buf, len) != CAPNP_OK) {
    free(buf);
    fprintf(stderr, "framing\n");
    return 1;
  }
  free(buf);
  if (capnp_root(&m, &root) != CAPNP_OK) {
    capnp_message_free(&m);
    return 1;
  }
  if (empty)
    check_empty(&root);
  else
    check_full(&root);
  capnp_message_free(&m);
  return harness_finish();
}

static void set_scalars(const capnp_bptr_t *body) {
  capnp_builder_set_bool(body, 0, 1);
  capnp_builder_set_u8(body, 1, (uint8_t)3 ^ (uint8_t)-5);
  capnp_builder_set_u16(body, 2, 1);
  capnp_builder_set_u32(body, 4, 99u ^ 7u);
  capnp_builder_set_u64(body, 8, f64_bits(2.5) ^ f64_bits(1.5));
  capnp_builder_set_u16(body, 16, (uint16_t)(9u ^ 4u));
}

static int set_title(const capnp_bptr_t *body) {
  return capnp_builder_set_text(body, CONFORM_D, 0, "hi", 2);
}

static int set_blob(const capnp_bptr_t *body) {
  uint8_t abc[] = {'a', 'b', 'c'};
  return capnp_builder_set_data(body, CONFORM_D, 1, abc, 3);
}

static int set_inner(const capnp_bptr_t *body) {
  capnp_bptr_t slot, inner;
  if (capnp_builder_slot(body, CONFORM_D, 2, &slot) ||
      capnp_builder_struct(&slot, INNER_D, INNER_P, &inner))
    return -1;
  if (capnp_builder_set_u32(&inner, 0, 8u ^ (uint32_t)-3))
    return -1;
  return capnp_builder_set_text(&inner, INNER_D, 0, "in", 2);
}

static int set_buckets(const capnp_bptr_t *body) {
  capnp_bptr_t buckets, b0, b1;
  int32_t s0[] = {1, 2};
  int32_t s1[] = {3};
  if (capnp_builder_set_list_struct(body, CONFORM_D, 3, 2, 0, 2, &buckets))
    return -1;
  b0 = buckets;
  b1 = capnp_bptr_add(buckets, 2);
  if (capnp_builder_set_text(&b0, 0, 0, "a", 1) ||
      capnp_builder_set_list_u32(&b0, 0, 1, (uint32_t *)s0, 2) ||
      capnp_builder_set_text(&b1, 0, 0, "b", 1) ||
      capnp_builder_set_list_u32(&b1, 0, 1, (uint32_t *)s1, 1))
    return -1;
  return 0;
}

static int set_matrix(const capnp_bptr_t *body) {
  capnp_bptr_t rows, row;
  const char *row0[] = {"x", "yy"};
  const char *row1[] = {"z"};
  if (capnp_builder_init_ptr_list(body, CONFORM_D, 4, 2, &rows))
    return -1;
  row = rows;
  if (capnp_builder_set_list_text(&row, 0, 0, row0, 2))
    return -1;
  row = capnp_bptr_add(rows, 1);
  return capnp_builder_set_list_text(&row, 0, 0, row1, 1);
}

static int set_note(const capnp_bptr_t *body) {
  return capnp_builder_set_text(body, CONFORM_D, 5, "hey", 3);
}

static int set_comment(const capnp_bptr_t *body) {
  return capnp_builder_set_text(body, CONFORM_D, 6, "c", 1);
}

static int fill_conform(capnp_builder_t *b, int reverse) {
  capnp_bptr_t root, body;
  if (capnp_builder_root(b, &root) ||
      capnp_builder_struct(&root, CONFORM_D, CONFORM_P, &body))
    return -1;
  if (!reverse)
    set_scalars(&body);
  if (!reverse) {
    if (set_title(&body) || set_blob(&body) || set_inner(&body) ||
        set_buckets(&body) || set_matrix(&body) || set_note(&body) ||
        set_comment(&body))
      return -1;
  } else if (set_comment(&body) || set_note(&body) || set_matrix(&body) ||
             set_buckets(&body) || set_inner(&body) || set_blob(&body) ||
             set_title(&body)) {
    return -1;
  }
  if (reverse)
    set_scalars(&body);
  return 0;
}

static int write_canon(const char *path, int mode) {
  capnp_builder_t b;
  uint8_t *flat = NULL, *can = NULL;
  size_t flen = 0, clen = 0;
  capnp_message_t m = {0};
  FILE *f;
  int rc;

  if (mode == 2) {
    /* One-word first segment forces the root struct into the next segment. */
    capnp_builder_init_sized(&b, 1);
    capnp_builder_set_max_seg_words(&b, 16);
  } else {
    capnp_builder_init(&b);
  }
  if (fill_conform(&b, mode == 1)) {
    capnp_builder_free(&b);
    return 1;
  }
  if (mode == 2 && capnp_builder_nsegs(&b) < 2) {
    capnp_builder_free(&b);
    return 1;
  }
  rc = capnp_builder_serialize(&b, &flat, &flen);
  capnp_builder_free(&b);
  if (rc || capnp_message_from_flat(&m, flat, flen)) {
    free(flat);
    return 1;
  }
  free(flat);
  rc = capnp_canonicalize(&m, &can, &clen);
  capnp_message_free(&m);
  if (rc)
    return 1;
  f = fopen(path, "wb");
  if (!f || fwrite(can, 1, clen, f) != clen) {
    if (f)
      fclose(f);
    free(can);
    return 1;
  }
  fclose(f);
  free(can);
  return 0;
}

static int pack_mode(const char *in_path, const char *out_path) {
  uint8_t *buf = NULL, *packed = NULL;
  size_t len = 0, plen = 0;
  FILE *f;
  if (load_file(in_path, &buf, &len))
    return 1;
  if (len % 8u != 0 || capnp_pack(buf, len, &packed, &plen) != CAPNP_OK) {
    free(buf);
    return 1;
  }
  free(buf);
  f = fopen(out_path, "wb");
  if (!f || fwrite(packed, 1, plen, f) != plen) {
    if (f)
      fclose(f);
    free(packed);
    return 1;
  }
  fclose(f);
  free(packed);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "read") == 0)
    return read_mode(argv[2], 0);
  if (argc == 3 && strcmp(argv[1], "read-empty") == 0)
    return read_mode(argv[2], 1);
  if (argc == 3 && strcmp(argv[1], "canon-schema") == 0)
    return write_canon(argv[2], 0);
  if (argc == 3 && strcmp(argv[1], "canon-reverse") == 0)
    return write_canon(argv[2], 1);
  if (argc == 3 && strcmp(argv[1], "canon-far") == 0)
    return write_canon(argv[2], 2);
  if (argc == 4 && strcmp(argv[1], "pack") == 0)
    return pack_mode(argv[2], argv[3]);
  fprintf(stderr, "usage: test_conform <mode> <path>\n");
  return 2;
}
