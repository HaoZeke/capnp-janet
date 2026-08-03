/*
 * Calculator Expression sample (serialization subset of C++/pycapnp calculator).
 * Full Calculator is Cap'n RPC; here we round-trip Expression trees and evaluate
 * them in-process, matching the sample expression shapes.
 *
 * Expression layout (capnp compile -ocapnp): 16 data bytes, 1 ptr
 *   union tag bits [64, 80) = u16 at byte 8
 *   tag 0 literal: f64 @byte0
 *   tag 1 parameter: u32 @byte0
 *   tag 2 call: op enum u16 @byte0, params List(Expression) @ptr0
 */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  EXPR_LITERAL = 0,
  EXPR_PARAMETER = 1,
  EXPR_CALL = 2
};

enum {
  OP_ADD = 0,
  OP_SUB = 1,
  OP_MUL = 2,
  OP_DIV = 3
};

/* Expression: 2 data words, 1 pointer. */
enum { EXPR_D = 2, EXPR_P = 1 };

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

static int eval_expr(const capnp_ptr_t *e, double *out) {
  uint16_t tag = capnp_get_u16(e, 8, 0xffff);
  if (tag == EXPR_LITERAL) {
    *out = capnp_get_f64(e, 0, 0.0);
    return 0;
  }
  if (tag == EXPR_PARAMETER) {
    /* parameters only valid inside function bodies; reject in free eval */
    return -1;
  }
  if (tag == EXPR_CALL) {
    uint16_t op = capnp_get_u16(e, 0, 0xffff);
    capnp_ptr_t params;
    uint32_t n, i;
    double acc = 0.0;
    if (capnp_getp(e, 0, &params) != CAPNP_OK ||
        params.kind != CAPNP_PK_LIST)
      return -1;
    n = capnp_list_len(&params);
    if (n == 0)
      return -1;
    for (i = 0; i < n; i++) {
      capnp_ptr_t child;
      double v;
      if (capnp_list_getp(&params, i, &child) != CAPNP_OK)
        return -1;
      if (eval_expr(&child, &v) != 0)
        return -1;
      if (i == 0) {
        acc = v;
        continue;
      }
      switch (op) {
      case OP_ADD:
        acc += v;
        break;
      case OP_SUB:
        acc -= v;
        break;
      case OP_MUL:
        acc *= v;
        break;
      case OP_DIV:
        if (v == 0.0)
          return -1;
        acc /= v;
        break;
      default:
        return -1;
      }
    }
    *out = acc;
    return 0;
  }
  return -1;
}

/* Write Expression.literal at body. */
static int write_literal(const capnp_bptr_t *body, double v) {
  if (capnp_builder_set_f64(body, 0, v))
    return -1;
  if (capnp_builder_set_u16(body, 8, EXPR_LITERAL))
    return -1;
  return 0;
}

/* Write Expression.call at body; returns params list first-elem body. */
static int write_call(const capnp_bptr_t *body, uint16_t op, uint32_t nparams,
                      capnp_bptr_t *params0) {
  if (capnp_builder_set_u16(body, 0, op))
    return -1;
  if (capnp_builder_set_u16(body, 8, EXPR_CALL))
    return -1;
  if (capnp_builder_set_list_struct(body, EXPR_D, 0, nparams, EXPR_D, EXPR_P,
                                    params0))
    return -1;
  return 0;
}

/* Build EvaluateRequest for (2 + 3). */
static void test_builder_add_2_3(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, expr_slot, expr_body;
  capnp_bptr_t params0;
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t req, expr;
  double v = 0.0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  /* EvaluateRequest: 0 data, 1 ptr */
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "req");
  CHECK(capnp_builder_slot(&body, 0, 0, &expr_slot) == CAPNP_OK,
        "slot");
  CHECK(capnp_builder_struct(&expr_slot, EXPR_D, EXPR_P, &expr_body) ==
            CAPNP_OK,
        "expr");
  CHECK(write_call(&expr_body, OP_ADD, 2, &params0) == 0, "call");
  CHECK(write_literal(&params0, 2.0) == 0, "lit2");
  {
    capnp_bptr_t p1 = capnp_bptr_add(params0, EXPR_D + EXPR_P);
    CHECK(write_literal(&p1, 3.0) == 0, "lit3");
  }

  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "ser");
  capnp_builder_free(&b);

  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &req) == CAPNP_OK, "root");
  CHECK(capnp_getp(&req, 0, &expr) == CAPNP_OK, "expr");
  CHECK_EQ_U32(capnp_get_u16(&expr, 8, 0xffff), EXPR_CALL, "call tag");
  CHECK_EQ_U32(capnp_get_u16(&expr, 0, 0xffff), OP_ADD, "op add");
  CHECK(eval_expr(&expr, &v) == 0, "eval");
  CHECK_NEAR(v, 5.0, 1e-12, "2+3=5");
  capnp_message_free(&m);
}

/* Build (2 + 3) * 4 */
static void test_builder_mul_add(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, expr_slot, expr_body;
  capnp_bptr_t outer_params, inner_params;
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t req, expr;
  double v = 0.0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "req");
  CHECK(capnp_builder_slot(&body, 0, 0, &expr_slot) == CAPNP_OK, "sl");
  CHECK(capnp_builder_struct(&expr_slot, EXPR_D, EXPR_P, &expr_body) ==
            CAPNP_OK,
        "expr");
  /* outer call multiply with 2 params */
  CHECK(write_call(&expr_body, OP_MUL, 2, &outer_params) == 0, "mul");
  /* outer_params[0] = call add [2,3] */
  CHECK(write_call(&outer_params, OP_ADD, 2, &inner_params) == 0, "add");
  CHECK(write_literal(&inner_params, 2.0) == 0, "2");
  {
    capnp_bptr_t t = capnp_bptr_add(inner_params, EXPR_D + EXPR_P);
    CHECK(write_literal(&t, 3.0) == 0, "3");
  }
  /* outer_params[1] = 4 */
  {
    capnp_bptr_t t = capnp_bptr_add(outer_params, EXPR_D + EXPR_P);
    CHECK(write_literal(&t, 4.0) == 0, "4");
  }

  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &req) == CAPNP_OK, "root");
  CHECK(capnp_getp(&req, 0, &expr) == CAPNP_OK, "expr");
  CHECK(eval_expr(&expr, &v) == 0, "eval");
  CHECK_NEAR(v, 20.0, 1e-12, "(2+3)*4=20");
  capnp_message_free(&m);
}

static void test_response_value(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t resp;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  /* EvaluateResponse: 1 data word, 0 ptrs */
  CHECK(capnp_builder_struct(&root, 1, 0, &body) == CAPNP_OK, "resp");
  CHECK(capnp_builder_set_f64(&body, 0, 5.0) == CAPNP_OK, "5");
  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &resp) == CAPNP_OK, "root");
  CHECK_NEAR(capnp_get_f64(&resp, 0, 0.0), 5.0, 1e-12, "value 5");
  capnp_message_free(&m);
}

static void test_golden(const char *rel, double expect) {
  const char *src = getenv("CAPNP_JANET_SOURCE_ROOT");
  char path[1024];
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t req, expr;
  double v = 0.0;

  if (!src || !src[0])
    src = ".";
  snprintf(path, sizeof(path), "%s/%s", src, rel);
  if (load_file(path, &flat, &flat_len) != 0) {
    fprintf(stderr, "SKIP golden: cannot open %s\n", path);
    return;
  }
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "golden flat");
  free(flat);
  CHECK(capnp_root(&m, &req) == CAPNP_OK, "golden root");
  CHECK(capnp_getp(&req, 0, &expr) == CAPNP_OK, "expr");
  CHECK(eval_expr(&expr, &v) == 0, "eval golden");
  CHECK_NEAR(v, expect, 1e-12, "golden value");
  capnp_message_free(&m);
}

static void test_golden_response(void) {
  const char *src = getenv("CAPNP_JANET_SOURCE_ROOT");
  char path[1024];
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t resp;

  if (!src || !src[0])
    src = ".";
  snprintf(path, sizeof(path), "%s/test/fixtures/calculator_value_5.bin", src);
  if (load_file(path, &flat, &flat_len) != 0) {
    fprintf(stderr, "SKIP golden response: cannot open %s\n", path);
    return;
  }
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &resp) == CAPNP_OK, "root");
  CHECK_NEAR(capnp_get_f64(&resp, 0, 0.0), 5.0, 1e-12, "golden 5");
  capnp_message_free(&m);
}

static void test_literal_only_request(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, expr_slot, expr_body;
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t req, expr;
  double v = 0.0;

  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 0, 1, &body);
  capnp_builder_slot(&body, 0, 0, &expr_slot);
  capnp_builder_struct(&expr_slot, EXPR_D, EXPR_P, &expr_body);
  CHECK(write_literal(&expr_body, 42.5) == 0, "lit");
  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &req) == CAPNP_OK, "root");
  CHECK(capnp_getp(&req, 0, &expr) == CAPNP_OK, "expr");
  CHECK_EQ_U32(capnp_get_u16(&expr, 8, 0xffff), EXPR_LITERAL, "lit tag");
  CHECK(eval_expr(&expr, &v) == 0, "eval");
  CHECK_NEAR(v, 42.5, 1e-12, "42.5");
  capnp_message_free(&m);
}

int main(void) {
  test_builder_add_2_3();
  test_builder_mul_add();
  test_response_value();
  test_literal_only_request();
  test_golden("test/fixtures/calculator_add_2_3.bin", 5.0);
  test_golden("test/fixtures/calculator_mul_add.bin", 20.0);
  test_golden_response();
  return harness_finish();
}
