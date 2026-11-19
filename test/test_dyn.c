#define _GNU_SOURCE
#include "harness.h"

#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_dyn.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__SANITIZE_ADDRESS__)
#include <dlfcn.h>
#endif

#if !defined(__SANITIZE_ADDRESS__)
/* The text and JSON encoders grow their piece lists with malloc.
 * Fail the second 128-byte malloc, which is the size array of the
 * 8-to-16 growth, and count every block handed out while armed. */
static void *(*real_malloc)(size_t);
static void (*real_free)(void *);
static void *(*real_realloc)(void *, size_t);
static int hook_ready;
static int grow_arm;
/* malloc is a compiler builtin. A plain int is not a visible side
 * effect of the call, so an optimizing build deletes the store that
 * tells malloc this realloc is a fresh buffer. */
static volatile int in_realloc;
static int seen128;
static int double_free;
static int track_fail;
static void **livep;
static size_t nlive, clive;
static void **deadp;
static size_t ndead, cdead;

__attribute__((constructor)) static void init_alloc_hook(void) {
  real_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
  real_free = (void (*)(void *))dlsym(RTLD_NEXT, "free");
  real_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
  hook_ready = 1;
}

static int push_ptr(void ***arr, size_t *n, size_t *c, void *p) {
  if (*n == *c) {
    size_t nc = *c ? *c * 2 : 32;
    void **ni = (void **)real_realloc(*arr, nc * sizeof(void *));
    if (!ni) {
      track_fail = 1;
      return -1;
    }
    *arr = ni;
    *c = nc;
  }
  (*arr)[(*n)++] = p;
  return 0;
}

static int take_live(void *p) {
  size_t i;
  for (i = 0; i < nlive; i++) {
    if (livep[i] == p) {
      livep[i] = livep[--nlive];
      return 1;
    }
  }
  return 0;
}

static int was_dead(void *p) {
  size_t i;
  for (i = 0; i < ndead; i++) {
    if (deadp[i] == p)
      return 1;
  }
  return 0;
}

static void grow_reset(void) {
  nlive = 0;
  ndead = 0;
  double_free = 0;
  track_fail = 0;
  seen128 = 0;
}

void *malloc(size_t n) {
  void *r;
  if (!hook_ready || !real_malloc)
    return NULL;
  /* realloc(NULL, n) is a fresh buffer, not a piece-list growth. */
  if (grow_arm && !in_realloc && n == 128) {
    seen128++;
    if (seen128 == 2)
      return NULL;
  }
  r = real_malloc(n);
  if (grow_arm && r)
    push_ptr(&livep, &nlive, &clive, r);
  return r;
}

void free(void *p) {
  if (!p)
    return;
  if (!hook_ready || !real_free)
    return;
  if (grow_arm) {
    if (take_live(p))
      push_ptr(&deadp, &ndead, &cdead, p);
    else if (was_dead(p))
      double_free = 1;
  }
  real_free(p);
}

void *realloc(void *p, size_t n) {
  void *r;
  int was;
  if (!p) {
    void *fresh;
    in_realloc = 1;
    fresh = malloc(n);
    in_realloc = 0;
    return fresh;
  }
  if (!hook_ready || !real_realloc)
    return NULL;
  was = grow_arm && take_live(p);
  r = real_realloc(p, n);
  if (!r) {
    if (was)
      push_ptr(&livep, &nlive, &clive, p);
    return NULL;
  }
  if (grow_arm)
    push_ptr(&livep, &nlive, &clive, r);
  return r;
}
#endif

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
  capnp_message_t m = {0};
  capnp_ptr_t root, inner, matrix, row;
  int flag = 1;
  int64_t tiny = 0, wide = 0, n = 0;
  uint64_t wide_u = 0;
  double ratio = 0.0;
  const char *text = NULL, *wname = NULL;
  size_t len = 0;
  uint8_t *empty_msg = NULL;
  size_t empty_len = 0;
  capnp_message_t em = {0};
  capnp_ptr_t eroot;

  if (open_root(req, msg, &d, &m, &root))
    return 1;
  CHECK(capnp_dyn_get_bool(d, &root, "Conform", "flag", &flag) == CAPNP_OK,
        "flag");
  CHECK(flag == 0, "flag false");
  CHECK(capnp_dyn_get_i64(d, &root, "Conform", "tiny", &tiny) == CAPNP_OK,
        "tiny");
  CHECK(tiny == 3, "tiny 3");
  CHECK(capnp_dyn_get_i64(d, &root, "Conform", "wide", &wide) == CAPNP_OK,
        "wide");
  CHECK(wide == 99, "wide 99");
  CHECK(capnp_dyn_get_u64(d, &root, "Conform", "wide", &wide_u) == CAPNP_OK,
        "wide u64");
  CHECK(wide_u == 99u, "wide u64 99");
  CHECK(capnp_dyn_get_f64(d, &root, "Conform", "ratio", &ratio) == CAPNP_OK,
        "ratio f64");
  CHECK_NEAR(ratio, 2.5, 1e-12, "ratio 2.5");
  CHECK(capnp_dyn_which(d, &root, "Conform", &wname) == CAPNP_OK, "which");
  CHECK(wname && strcmp(wname, "note") == 0, "which note");
  CHECK(capnp_dyn_get_text(d, &root, "Conform", "title", &text, &len) ==
            CAPNP_OK,
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

  /* Check defaults and union active checks on an empty () message */
  CHECK(capnp_dyn_decode_text(d, "Conform", "()", 2, &empty_msg, &empty_len) ==
            CAPNP_OK,
        "decode empty");
  CHECK(capnp_message_from_flat(&em, empty_msg, empty_len) == CAPNP_OK &&
            capnp_root(&em, &eroot) == CAPNP_OK,
        "open empty");
  free(empty_msg);
  CHECK(capnp_dyn_get_text(d, &eroot, "Conform", "title", &text, &len) ==
            CAPNP_OK,
        "default title");
  CHECK_STREQ(text, len, "untitled", "default title untitled");
  CHECK(capnp_dyn_get_f64(d, &eroot, "Conform", "ratio", &ratio) == CAPNP_OK,
        "default ratio");
  CHECK_NEAR(ratio, 1.5, 1e-12, "default ratio 1.5");
  CHECK(capnp_dyn_which(d, &eroot, "Conform", &wname) == CAPNP_OK &&
            strcmp(wname, "quiet") == 0,
        "default which quiet");
  CHECK(capnp_dyn_get_text(d, &eroot, "Conform", "note", &text, &len) ==
            CAPNP_ERR_KIND,
        "inactive union arm rejected");
  capnp_message_free(&em);

  capnp_message_free(&m);
  capnp_dyn_free(d);
  return harness_finish();
}

static int cmd_edges(const char *conform_req, const char *parity_req) {
  capnp_dyn_t *dc = NULL, *dp = NULL;
  uint8_t *reqb = NULL, *msg = NULL;
  size_t rn = 0, mn = 0;
  capnp_message_t m = {0};
  capnp_ptr_t root, buckets, b0, scores, pts, pt, raw_ptr;
  uint64_t u64v = 0;
  double f64v = 0.0;
  const char *txt = NULL;
  size_t tlen = 0;
  char *big_list = NULL;
  size_t pos = 0;
  int i;

  if (load_file(conform_req, &reqb, &rn) ||
      capnp_dyn_init(&dc, reqb, rn) != CAPNP_OK) {
    free(reqb);
    return 1;
  }
  free(reqb);
  if (load_file(parity_req, &reqb, &rn) ||
      capnp_dyn_init(&dp, reqb, rn) != CAPNP_OK) {
    free(reqb);
    capnp_dyn_free(dc);
    return 1;
  }
  free(reqb);

  /* 1. Empty primitive list must encode wire ElementSize FOUR_BYTES (4), not
   * POINTER (6) */
  {
    const char *b_in = "(name = \"a\", scores = [])";
    CHECK(capnp_dyn_decode_text(dc, "Bucket", b_in, strlen(b_in), &msg, &mn) ==
              CAPNP_OK,
          "decode empty scores");
  }
  CHECK(capnp_message_from_flat(&m, msg, mn) == CAPNP_OK &&
            capnp_root(&m, &b0) == CAPNP_OK,
        "open bucket");
  free(msg);
  CHECK(capnp_getp(&b0, 1, &scores) == CAPNP_OK, "scores ptr");
  CHECK(scores.kind == CAPNP_PK_LIST && scores.esize == CAPNP_SZ_FOUR &&
            scores.count == 0,
        "empty scores has CAPNP_SZ_FOUR");
  capnp_message_free(&m);

  /* 2. >64 elements in List(Struct) (70 elements) */
  big_list = (char *)malloc(8192);
  CHECK(big_list != NULL, "alloc big_list");
  pos += (size_t)snprintf(big_list + pos, 8192 - pos, "(buckets = [");
  for (i = 0; i < 70; i++) {
    pos += (size_t)snprintf(big_list + pos, 8192 - pos,
                            "%s(name = \"b%d\", scores = [%d])",
                            i ? ", " : "", i, i);
  }
  pos += (size_t)snprintf(big_list + pos, 8192 - pos, "])");
  CHECK(capnp_dyn_decode_text(dc, "Conform", big_list, pos, &msg, &mn) ==
            CAPNP_OK,
        "decode 70 buckets");
  free(big_list);
  CHECK(capnp_message_from_flat(&m, msg, mn) == CAPNP_OK &&
            capnp_root(&m, &root) == CAPNP_OK,
        "open 70 buckets");
  free(msg);
  CHECK(capnp_dyn_get_ptr(dc, &root, "Conform", "buckets", &buckets) ==
            CAPNP_OK,
        "get buckets");
  CHECK(capnp_list_len(&buckets) == 70, "70 buckets len");
  CHECK(capnp_list_getp(&buckets, 69, &b0) == CAPNP_OK, "bucket 69");
  CHECK(capnp_dyn_get_text(dc, &b0, "Bucket", "name", &txt, &tlen) == CAPNP_OK,
        "bucket 69 name");
  CHECK_STREQ(txt, tlen, "b69", "bucket 69 name b69");
  capnp_message_free(&m);

  /* 3. Pointer defaults honoured on Parity () */
  CHECK(capnp_dyn_decode_text(dp, "Parity", "()", 2, &msg, &mn) == CAPNP_OK,
        "decode Parity ()");
  CHECK(capnp_message_from_flat(&m, msg, mn) == CAPNP_OK &&
            capnp_root(&m, &root) == CAPNP_OK,
        "open Parity ()");
  free(msg);
  CHECK(capnp_dyn_get_u64(dp, &root, "Parity", "vU64", &u64v) == CAPNP_OK &&
            u64v == UINT64_MAX,
        "Parity vU64 default UINT64_MAX");
  CHECK(capnp_dyn_get_f64(dp, &root, "Parity", "vF64", &f64v) == CAPNP_OK &&
            fabs(f64v - (-2.25)) < 1e-12,
        "Parity vF64 default -2.25");
  CHECK(capnp_dyn_get_text(dp, &root, "Parity", "vText", &txt, &tlen) ==
            CAPNP_OK,
        "Parity vText default");
  CHECK_STREQ(txt, tlen, "default-text", "Parity vText default-text");
  CHECK(capnp_dyn_get_ptr(dp, &root, "Parity", "vStruct", &pt) == CAPNP_OK &&
            pt.kind == CAPNP_PK_STRUCT,
        "Parity vStruct pointer default");
  {
    int64_t px = 0, py = 0;
    CHECK(capnp_dyn_get_i64(dp, &pt, "Point", "x", &px) == CAPNP_OK && px == 3,
          "vStruct default x == 3");
    CHECK(capnp_dyn_get_i64(dp, &pt, "Point", "y", &py) == CAPNP_OK && py == 4,
          "vStruct default y == 4");
  }
  CHECK(capnp_dyn_get_ptr(dp, &root, "Parity", "lI32", &pts) == CAPNP_OK &&
            pts.kind == CAPNP_PK_LIST && capnp_list_len(&pts) == 2 &&
            capnp_list_get_u32(&pts, 0, 0) == 9,
        "Parity lI32 list default [9, 8]");
  capnp_message_free(&m);

  /* 4. Unknown enum ordinal in JSON and text */
  {
    const char *e_in = "{\"vEnum\": 99, \"lEnum\": [\"emerald\", 77]}";
    CHECK(capnp_dyn_decode_json(dp, "Parity", e_in, strlen(e_in), &msg, &mn) ==
              CAPNP_OK,
          "decode unknown enum ordinal JSON");
  }
  CHECK(capnp_message_from_flat(&m, msg, mn) == CAPNP_OK &&
            capnp_root(&m, &root) == CAPNP_OK,
        "open unknown enum msg");
  free(msg);
  {
    char *jout = NULL, *tout = NULL;
    size_t jlen = 0, tlen2 = 0;
    CHECK(capnp_dyn_encode_json_short(dp, "Parity", &root, &jout, &jlen) ==
              CAPNP_OK,
          "encode unknown enum json");
    CHECK(strstr(jout, "\"vEnum\":99") != NULL &&
              strstr(jout, "\"lEnum\":[\"emerald\",77]") != NULL,
          "json preserves unknown enum ordinal");
    free(jout);
    CHECK(capnp_dyn_encode_text_short(dp, "Parity", &root, &tout, &tlen2) ==
              CAPNP_OK,
          "encode unknown enum text");
    CHECK(strstr(tout, "vEnum = (99)") != NULL &&
              strstr(tout, "lEnum = [green, (77)]") != NULL,
          "text formats unknown enum ordinal as (N)");
    free(tout);
  }
  capnp_message_free(&m);

  /* 5. Strict JSON grammar and trailing-token rejection + error-path leak
   * check (under ASan/LSan) */
  {
    const char *bad_json[] = {
        "{} trailing",
        "{,\"vBool\":true}",
        "{\"vBool\":true,}",
        "{\"vBool\":true \"vI8\":1}",
        "{\"vBool\":true,,\"vI8\":1}",
        "{\"lI8\":[1,,2]}",
        "{\"lI8\":[1,]}",
        "{\"lI8\":[,1]}",
        "{\"vText\":\"\\x41\"}",
        "{\"vText\":\"\\u12zz\"}",
        "{\"vI8\":128}",
        "{\"vI8\":-129}",
        "{\"vU8\":256}",
        "{\"vU8\":-1}",
        "{\"vU64\":\"18446744073709551616\"}",
        "{\"vI64\":\"9223372036854775808\"}",
        "{\"vI64\":\"177a\"}",
        "{\"b64Data\":\"not!base64!\"}",
        "{\"hexData\":\"123\"}",
        "{\"hexData\":\"12zz\"}",
        "{\"vText\":\"unterminated",
    };
    const char *bad_text[] = {
        "() extra",
        "(vBool = true) (vBool = false)",
        "(,)",
        "(vBool = true,, vI8 = 1)",
        "(lI8 = [1,, 2])",
        "(lI8 = [, 1])",
        "(unknownField = 1)",
        "(vI8 = 200)",
        "(vU8 = -1)",
        "(vText = \"\\x4g\")",
        "(vText = \"unterminated",
    };
    size_t k;
    for (k = 0; k < sizeof(bad_json) / sizeof(bad_json[0]); k++) {
      uint8_t *out = NULL;
      size_t olen = 0;
      CHECK(capnp_dyn_decode_json(dp, "Parity", bad_json[k],
                                  strlen(bad_json[k]), &out, &olen) != CAPNP_OK,
            bad_json[k]);
      free(out);
    }
    for (k = 0; k < sizeof(bad_text) / sizeof(bad_text[0]); k++) {
      uint8_t *out = NULL;
      size_t olen = 0;
      CHECK(capnp_dyn_decode_text(dp, "Parity", bad_text[k],
                                  strlen(bad_text[k]), &out, &olen) != CAPNP_OK,
            bad_text[k]);
      free(out);
    }
    /* Non-NUL-terminated slice boundary check */
    {
      const char raw_slice[] = "(vText = \"\\x41\"GARBAGE";
      uint8_t *out = NULL;
      size_t olen = 0;
      CHECK(capnp_dyn_decode_text(dp, "Parity", raw_slice, 11, &out, &olen) !=
                CAPNP_OK,
            "truncated hex escape in non-NUL slice");
      free(out);
    }
  }
  (void)raw_ptr;
  capnp_dyn_free(dc);
  capnp_dyn_free(dp);
  return harness_finish();
}

static int cmd_encode(const char *req, const char *msg, const char *out,
                      int json) {
  capnp_dyn_t *d = NULL;
  capnp_message_t m = {0};
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
  capnp_message_t m = {0};
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

static int cmd_disc(const char *req, const char *mode) {
  uint8_t *reqb = NULL, *flat = NULL, *msg = NULL;
  size_t rn = 0, fl = 0, mn = 0;
  capnp_message_t m = {0};
  capnp_ptr_t root, nodes, node;
  capnp_dyn_t *d = NULL;
  uint32_t i, n;
  int oob = strcmp(mode, "oob") == 0;
  int huge = strcmp(mode, "huge") == 0;
  int rc;
  const char *json = "{\"kind\":\"b\",\"payload\":5}";
  if (load_file(req, &reqb, &rn) || capnp_message_from_flat(&m, reqb, rn))
    return 1;
  free(reqb);
  if (capnp_root(&m, &root) || capnp_getp(&root, 0, &nodes)) {
    capnp_message_free(&m);
    return 1;
  }
  n = capnp_list_len(&nodes);
  for (i = 0; i < n; i++) {
    uint8_t *p;
    if (capnp_list_getp(&nodes, i, &node))
      continue;
    if (capnp_get_u16(&node, 12, 0xffff) != 1 || capnp_get_u16(&node, 30, 0) == 0)
      continue;
    if (!oob && !huge)
      continue;
    p = (uint8_t *)node.msg->segs[node.seg].data + node.word * 8 +
        node.body_byte;
    capnp_store_le32(p + 32, huge ? 0x80000000u : 4u);
  }
  if (capnp_message_copy_flat(&m, &flat, &fl)) {
    capnp_message_free(&m);
    return 1;
  }
  capnp_message_free(&m);
  rc = capnp_dyn_init(&d, flat, fl);
  if (rc != CAPNP_OK) {
    free(flat);
    /* A discriminant past the data section, including one whose byte
     * offset wraps a 32-bit multiply, is a malformed schema. */
    if (oob || huge)
      return rc == CAPNP_ERR_SCHEMA ? 0 : 1;
    return 1;
  }
  free(flat);
  rc = capnp_dyn_decode_json(d, "U", json, strlen(json), &msg, &mn);
  free(msg);
  capnp_dyn_free(d);
  if (oob || huge)
    return rc == CAPNP_OK;
  return rc != CAPNP_OK;
}

static int cmd_reject(const char *req, const char *type, const char *which,
                       const char *text, const char *needle) {
  capnp_dyn_t *d = NULL;
  uint8_t *reqb = NULL, *msg = NULL;
  size_t rn = 0, mn = 0;
  int json = strcmp(which, "json") == 0;
  int rc;
  const char *err;
  if (load_file(req, &reqb, &rn))
    return 1;
  if (capnp_dyn_init(&d, reqb, rn) != CAPNP_OK) {
    free(reqb);
    return 1;
  }
  free(reqb);
  rc = json ? capnp_dyn_decode_json(d, type, text, strlen(text), &msg, &mn)
            : capnp_dyn_decode_text(d, type, text, strlen(text), &msg, &mn);
  err = capnp_dyn_error(d);
  free(msg);
  if (rc == CAPNP_OK || !err || !strstr(err, needle)) {
    fprintf(stderr, "reject %s: rc=%d err=%s\n", which, rc, err ? err : "");
    capnp_dyn_free(d);
    return 1;
  }
  capnp_dyn_free(d);
  return 0;
}

static int cmd_grow(const char *req, const char *msg_path) {
  capnp_dyn_t *d = NULL;
  capnp_message_t m = {0};
  capnp_ptr_t root;
  char *text = NULL;
  char *json = NULL;
  size_t n = 0, jn = 0;
  int rc;
  if (open_root(req, msg_path, &d, &m, &root))
    return 1;
  rc = capnp_dyn_encode_text(d, "Conform", &root, &text, &n);
  if (rc != CAPNP_OK || !text || !strstr(text, "scores")) {
    free(text);
    capnp_message_free(&m);
    capnp_dyn_free(d);
    return 1;
  }
  free(text);
  text = NULL;
  rc = capnp_dyn_encode_json(d, "Conform", &root, &json, &jn);
  if (rc != CAPNP_OK || !json || !strstr(json, "scores")) {
    free(json);
    capnp_message_free(&m);
    capnp_dyn_free(d);
    return 1;
  }
  free(json);
  json = NULL;
#if !defined(__SANITIZE_ADDRESS__)
  grow_reset();
  grow_arm = 1;
  rc = capnp_dyn_encode_text(d, "Conform", &root, &text, &n);
  grow_arm = 0;
  free(text);
  text = NULL;
  if (rc != CAPNP_ERR_ALLOC || nlive || double_free || track_fail) {
    capnp_message_free(&m);
    capnp_dyn_free(d);
    return 1;
  }
  grow_reset();
  grow_arm = 1;
  rc = capnp_dyn_encode_json(d, "Conform", &root, &json, &jn);
  grow_arm = 0;
  free(json);
  if (rc != CAPNP_ERR_ALLOC || nlive || double_free || track_fail) {
    capnp_message_free(&m);
    capnp_dyn_free(d);
    return 1;
  }
#endif
  capnp_message_free(&m);
  capnp_dyn_free(d);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "read") == 0)
    return cmd_read(argv[2], argv[3]);
  if (argc == 4 && strcmp(argv[1], "edges") == 0)
    return cmd_edges(argv[2], argv[3]);
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
  if (argc == 4 && strcmp(argv[1], "grow") == 0)
    return cmd_grow(argv[2], argv[3]);
  if (argc == 7 && strcmp(argv[1], "reject") == 0)
    return cmd_reject(argv[2], argv[3], argv[4], argv[5], argv[6]);
  if (argc == 4 && strcmp(argv[1], "disc") == 0)
    return cmd_disc(argv[2], argv[3]);
  fprintf(stderr, "usage: test_dyn <mode> ...\n");
  return 2;
}
