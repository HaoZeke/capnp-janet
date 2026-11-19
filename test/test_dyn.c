#include "harness.h"

#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_dyn.h>
#include <capnp-janet/capnp_message.h>

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

static int write_file(const char *path, const void *data, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f || fwrite(data, 1, n, f) != n) {
    if (f)
      fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

static int open_root(const char *req_path, const char *msg_path, capnp_dyn_t **d,
                     capnp_message_t *m, capnp_ptr_t *root) {
  uint8_t *req = NULL, *msg = NULL;
  size_t rn = 0, mn = 0;
  if (load_file(req_path, &req, &rn) || load_file(msg_path, &msg, &mn))
    return 1;
  if (capnp_dyn_init(d, req, rn) != CAPNP_OK) {
    free(req);
    free(msg);
    return 1;
  }
  free(req);
  if (capnp_message_from_flat(m, msg, mn) != CAPNP_OK ||
      capnp_root(m, root) != CAPNP_OK) {
    free(msg);
    capnp_dyn_free(*d);
    return 1;
  }
  free(msg);
  return 0;
}

static int cmd_read(const char *req, const char *msg) {
  capnp_dyn_t *d = NULL;
  capnp_message_t m;
  capnp_ptr_t root, inner, matrix, row;
  int flag = 1;
  int64_t tiny = 0, wide = 0, n = 0;
  const char *text = NULL;
  size_t len = 0;
  if (open_root(req, msg, &d, &m, &root))
    return 1;
  CHECK(capnp_dyn_get_bool(d, &root, "Conform", "flag", &flag) == CAPNP_OK,
        "flag");
  CHECK(flag == 0, "flag false");
  CHECK(capnp_dyn_get_i64(d, &root, "Conform", "tiny", &tiny) == CAPNP_OK, "tiny");
  CHECK(tiny == 3, "tiny 3");
  CHECK(capnp_dyn_get_i64(d, &root, "Conform", "wide", &wide) == CAPNP_OK, "wide");
  CHECK(wide == 99, "wide 99");
  CHECK(capnp_dyn_get_text(d, &root, "Conform", "title", &text, &len) == CAPNP_OK,
        "title");
  CHECK_STREQ(text, len, "hi", "title hi");
  CHECK(capnp_dyn_get_ptr(d, &root, "Conform", "inner", &inner) == CAPNP_OK,
        "inner");
  CHECK(capnp_dyn_get_i64(d, &inner, "Inner", "n", &n) == CAPNP_OK, "n");
  CHECK(n == 8, "inner n");
  CHECK(capnp_dyn_get_ptr(d, &root, "Conform", "matrix", &matrix) == CAPNP_OK,
        "matrix");
  CHECK(capnp_list_getp(&matrix, 0, &row) == CAPNP_OK, "row");
  CHECK(capnp_list_get_text(&row, 0, &text, &len) == CAPNP_OK, "cell");
  CHECK_STREQ(text, len, "x", "matrix x");
  capnp_message_free(&m);
  capnp_dyn_free(d);
  return harness_finish();
}

static int cmd_encode(const char *req, const char *msg, const char *out, int json) {
  capnp_dyn_t *d = NULL;
  capnp_message_t m;
  capnp_ptr_t root;
  char *text = NULL;
  size_t n = 0;
  int rc;
  if (open_root(req, msg, &d, &m, &root))
    return 1;
  rc = json ? capnp_dyn_encode_json(d, "Conform", &root, &text, &n)
            : capnp_dyn_encode_text(d, "Conform", &root, &text, &n);
  if (rc || write_file(out, text, n)) {
    free(text);
    capnp_message_free(&m);
    capnp_dyn_free(d);
    return 1;
  }
  free(text);
  capnp_message_free(&m);
  capnp_dyn_free(d);
  return 0;
}

static int cmd_decode(const char *req, const char *text_path, const char *out,
                      int json) {
  capnp_dyn_t *d = NULL;
  uint8_t *reqb = NULL, *textb = NULL, *msg = NULL;
  size_t rn = 0, tn = 0, mn = 0;
  int rc;
  if (load_file(req, &reqb, &rn) || load_file(text_path, &textb, &tn))
    return 1;
  if (capnp_dyn_init(&d, reqb, rn) != CAPNP_OK) {
    free(reqb);
    free(textb);
    return 1;
  }
  free(reqb);
  rc = json ? capnp_dyn_decode_json(d, "Conform", (char *)textb, tn, &msg, &mn)
            : capnp_dyn_decode_text(d, "Conform", (char *)textb, tn, &msg, &mn);
  free(textb);
  capnp_dyn_free(d);
  if (rc || write_file(out, msg, mn)) {
    free(msg);
    return 1;
  }
  free(msg);
  return 0;
}

static int cmd_canon(const char *msg_path, const char *out) {
  uint8_t *msg = NULL, *can = NULL;
  size_t mn = 0, cn = 0;
  capnp_message_t m;
  if (load_file(msg_path, &msg, &mn))
    return 1;
  if (capnp_message_from_flat(&m, msg, mn) != CAPNP_OK) {
    free(msg);
    return 1;
  }
  free(msg);
  if (capnp_canonicalize(&m, &can, &cn) != CAPNP_OK) {
    capnp_message_free(&m);
    return 1;
  }
  capnp_message_free(&m);
  if (write_file(out, can, cn)) {
    free(can);
    return 1;
  }
  free(can);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "read") == 0)
    return cmd_read(argv[2], argv[3]);
  if (argc == 5 && strcmp(argv[1], "text-encode") == 0)
    return cmd_encode(argv[2], argv[3], argv[4], 0);
  if (argc == 5 && strcmp(argv[1], "json-encode") == 0)
    return cmd_encode(argv[2], argv[3], argv[4], 1);
  if (argc == 5 && strcmp(argv[1], "text-decode") == 0)
    return cmd_decode(argv[2], argv[3], argv[4], 0);
  if (argc == 5 && strcmp(argv[1], "json-decode") == 0)
    return cmd_decode(argv[2], argv[3], argv[4], 1);
  if (argc == 4 && strcmp(argv[1], "canon") == 0)
    return cmd_canon(argv[2], argv[3]);
  fprintf(stderr, "usage: test_dyn <mode> ...\n");
  return 2;
}
