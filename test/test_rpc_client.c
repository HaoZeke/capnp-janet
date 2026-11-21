/* The client half of level 1: asking questions rather than only
 * answering them, and the `-> stream` flow-control window.
 *
 * Two vats share a pair of frame queues, so every message is a real frame
 * and the two are pumped in lockstep.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_rpc.h>

#include "harness.h"

#define MAX_Q 32

typedef struct {
  uint8_t *data[MAX_Q];
  size_t len[MAX_Q];
  int head, tail;
} queue_t;

static void q_push(queue_t *q, const uint8_t *d, size_t n)
{
  if (q->tail >= MAX_Q)
    return;
  q->data[q->tail] = (uint8_t *)malloc(n);
  memcpy(q->data[q->tail], d, n);
  q->len[q->tail] = n;
  q->tail++;
}

static queue_t g_to_vat, g_to_client;

static int send_to_vat(void *ctx, const uint8_t *d, size_t n)
{
  (void)ctx;
  q_push(&g_to_vat, d, n);
  return 0;
}

static int send_to_client(void *ctx, const uint8_t *d, size_t n)
{
  (void)ctx;
  q_push(&g_to_client, d, n);
  return 0;
}

static int pump(capnp_rpc_conn_t *c, queue_t *q)
{
  int n = 0;
  while (q->head < q->tail) {
    capnp_rpc_handle(c, q->data[q->head], q->len[q->head]);
    free(q->data[q->head]);
    q->head++;
    n++;
  }
  q->head = q->tail = 0;
  return n;
}

static void q_drop(queue_t *q)
{
  while (q->head < q->tail)
    free(q->data[q->head++]);
  q->head = q->tail = 0;
}

static int doubler_calls;
static int doubler_fail_from;

static int doubler_dispatch(void *server, uint64_t iid, uint16_t mid,
                            const capnp_ptr_t *params,
                            const capnp_bptr_t *results)
{
  (void)server;
  (void)iid;
  (void)mid;
  capnp_bptr_t body;
  doubler_calls++;
  if (doubler_calls >= doubler_fail_from)
    return CAPNP_ERR_ARG;
  if (mid == 11) {
    CHECK(capnp_rpc_fail(CAPNP_RPC_EXCEPTION_OVERLOADED, "busy") == 0,
          "fail inside a dispatch");
    return CAPNP_ERR_ARG;
  }
  if (mid == 12) {
    CHECK(capnp_rpc_unimplemented("x.capnp:X", iid, mid) == 0,
          "unimplemented inside a dispatch");
    return CAPNP_ERR_ARG;
  }
  if (mid == 13)
    CHECK(capnp_rpc_fail(CAPNP_RPC_EXCEPTION_FAILED, "unused") == 0,
          "fail before succeeding");
  if (mid == 7) {
    /* The capability keeps the answer for pipelining. */
    static uint8_t big[10000];
    capnp_rpc_cap_ref_t self = {CAPNP_RPC_CAP_EXPORT, 0};
    int idx = capnp_rpc_result_cap(&self);
    memset(big, 0xab, sizeof big);
    if (idx < 0 || capnp_builder_struct(results, 0, 2, &body) ||
        capnp_builder_set_cap(&body, 0, 1, (uint32_t)idx))
      return CAPNP_ERR_ALLOC;
    return capnp_builder_set_data(&body, 0, 0, big, sizeof big);
  }
  if (mid == 9) {
    /* Returns export `n`, or import `n - 100` from 100 on. */
    uint32_t n = capnp_get_u32(params, 0, 0);
    capnp_rpc_cap_ref_t ref;
    int idx;
    ref.kind = n < 100 ? CAPNP_RPC_CAP_EXPORT : CAPNP_RPC_CAP_IMPORT;
    ref.id = n < 100 ? n : n - 100;
    idx = capnp_rpc_result_cap(&ref);
    if (idx < 0 || capnp_builder_struct(results, 0, 1, &body))
      return CAPNP_ERR_ARG;
    return capnp_builder_set_cap(&body, 0, 0, (uint32_t)idx);
  }
  if (capnp_builder_struct(results, 1, 0, &body))
    return CAPNP_ERR_ALLOC;
  {
    uint32_t in = params->kind == CAPNP_PK_STRUCT
                      ? capnp_get_u32(params, 0, 0)
                      : 0;
    return capnp_builder_set_u32(&body, 0, in * 2);
  }
}

static int fill_u32(void *ctx, const capnp_bptr_t *params)
{
  CHECK(capnp_builder_set_u32(params, 0, *(uint32_t *)ctx) == CAPNP_OK, "fill");
  return 0;
}

/* A Return naming `answer`, with union tag `tag` and `u32` at byte 8,
 * which takeFromOtherQuestion reads. */
static int feed_return(capnp_rpc_conn_t *c, uint32_t answer, uint16_t tag,
                       uint32_t u32)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, ret;
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc = CAPNP_ERR_ALLOC;
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) == CAPNP_OK &&
      capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK &&
      capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RETURN) == CAPNP_OK &&
      capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 2, 1, &ret) == CAPNP_OK &&
      capnp_builder_set_u32(&ret, 0, answer) == CAPNP_OK &&
      capnp_builder_set_u16(&ret, 6, tag) == CAPNP_OK &&
      capnp_builder_set_u32(&ret, 8, u32) == CAPNP_OK &&
      capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK)
    rc = capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
  return rc;
}

/* A Release of `count` references to export `id`. */
static int feed_release(capnp_rpc_conn_t *c, uint32_t id, uint32_t count)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, rel;
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc = CAPNP_ERR_ALLOC;
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) == CAPNP_OK &&
      capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK &&
      capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RELEASE) == CAPNP_OK &&
      capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 1, 0, &rel) == CAPNP_OK &&
      capnp_builder_set_u32(&rel, 0, id) == CAPNP_OK &&
      capnp_builder_set_u32(&rel, 4, count) == CAPNP_OK &&
      capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK)
    rc = capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
  return rc;
}

/* A Call to import 0 with onlyPromisePipeline set, method `mid` and
 * `arg` as its u32 param. */
static int feed_quiet_call(capnp_rpc_conn_t *c, uint32_t qid, uint16_t mid,
                           uint32_t arg)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, call, target, payload, params;
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc = CAPNP_ERR_ALLOC;
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) == CAPNP_OK &&
      capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK &&
      capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_CALL) == CAPNP_OK &&
      capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 3, 3, &call) == CAPNP_OK &&
      capnp_builder_set_u32(&call, 0, qid) == CAPNP_OK &&
      capnp_builder_set_u16(&call, 4, mid) == CAPNP_OK &&
      capnp_builder_set_u64(&call, 8, 0x1234) == CAPNP_OK &&
      capnp_builder_set_bool(&call, 130, 1) == CAPNP_OK &&
      capnp_builder_slot(&call, 3, 0, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 1, 1, &target) == CAPNP_OK &&
      capnp_builder_slot(&call, 3, 1, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 0, 2, &payload) == CAPNP_OK &&
      capnp_builder_slot(&payload, 0, 0, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 1, 0, &params) == CAPNP_OK &&
      capnp_builder_set_u32(&params, 0, arg) == CAPNP_OK &&
      capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK)
    rc = capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
  return rc;
}

/* Resolve arm `arm` that feed_resolve sends as Resolve.exception. */
#define RESOLVE_EXCEPTION 0xffff

/* A Resolve of promise `pid` to descriptor arm `arm` naming `id`. */
static int feed_resolve(capnp_rpc_conn_t *c, uint32_t pid, uint16_t arm,
                        uint32_t id)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, res, desc;
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc = CAPNP_ERR_ALLOC, ok;
  capnp_builder_init(&b);
  ok = capnp_builder_root(&b, &root) == CAPNP_OK &&
       capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK &&
       capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RESOLVE) == CAPNP_OK &&
       capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK &&
       capnp_builder_struct(&slot, 1, 1, &res) == CAPNP_OK &&
       capnp_builder_set_u32(&res, 0, pid) == CAPNP_OK &&
       capnp_builder_slot(&res, 1, 0, &slot) == CAPNP_OK;
  if (ok && arm == RESOLVE_EXCEPTION)
    ok = capnp_builder_set_u16(&res, 4, 1) == CAPNP_OK &&
         capnp_builder_struct(&slot, 1, 2, &desc) == CAPNP_OK &&
         capnp_builder_set_text(&desc, 1, 0, "broken", 6) == CAPNP_OK;
  else if (ok)
    ok = capnp_builder_struct(&slot, 1, 1, &desc) == CAPNP_OK &&
         capnp_builder_set_u16(&desc, 0, arm) == CAPNP_OK &&
         capnp_builder_set_u32(&desc, 4, id) == CAPNP_OK;
  if (ok && capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK)
    rc = capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
  return rc;
}

/* A Return of results for `answer` whose content is the capability
 * capTable entry 0 describes: arm `arm` naming `id`. */
static int feed_cap_return(capnp_rpc_conn_t *c, uint32_t answer, uint16_t arm,
                           uint32_t id)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, ret, payload, table;
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc = CAPNP_ERR_ALLOC;
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) == CAPNP_OK &&
      capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK &&
      capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RETURN) == CAPNP_OK &&
      capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 2, 1, &ret) == CAPNP_OK &&
      capnp_builder_set_u32(&ret, 0, answer) == CAPNP_OK &&
      capnp_builder_slot(&ret, 2, 0, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 0, 2, &payload) == CAPNP_OK &&
      capnp_builder_set_cap(&payload, 0, 0, 0) == CAPNP_OK &&
      capnp_builder_set_list_struct(&payload, 0, 1, 1, 1, 1, &table) ==
          CAPNP_OK &&
      capnp_builder_set_u16(&table, 0, arm) == CAPNP_OK &&
      capnp_builder_set_u32(&table, 4, id) == CAPNP_OK &&
      capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK)
    rc = capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
  return rc;
}

/* Frame `i` of `q` is a Release of `count` references to `id`. */
static int frame_release(queue_t *q, int i, uint32_t id, uint32_t count)
{
  capnp_message_t m = {0};
  capnp_ptr_t r, rel;
  int ok = i < q->tail &&
           capnp_message_from_flat(&m, q->data[i], q->len[i]) == CAPNP_OK &&
           capnp_root(&m, &r) == CAPNP_OK &&
           capnp_get_u16(&r, 0, 0) == CAPNP_RPC_MSG_RELEASE &&
           capnp_getp(&r, 0, &rel) == CAPNP_OK &&
           capnp_get_u32(&rel, 0, 0) == id && capnp_get_u32(&rel, 4, 0) == count;
  capnp_message_free(&m);
  return ok;
}

/* Frame `i` of `q` is a Return of an exception with `reason` and
 * Exception.type `type`. */
static int frame_exception(queue_t *q, int i, const char *reason,
                           uint16_t type)
{
  capnp_message_t m = {0};
  capnp_ptr_t r, ret, exc;
  const char *text = NULL;
  size_t n = 0;
  int ok = i < q->tail &&
           capnp_message_from_flat(&m, q->data[i], q->len[i]) == CAPNP_OK &&
           capnp_root(&m, &r) == CAPNP_OK &&
           capnp_get_u16(&r, 0, 0) == CAPNP_RPC_MSG_RETURN &&
           capnp_getp(&r, 0, &ret) == CAPNP_OK &&
           capnp_get_u16(&ret, 6, 0) == 1 &&
           capnp_getp(&ret, 0, &exc) == CAPNP_OK &&
           capnp_get_text(&exc, 0, &text, &n) == CAPNP_OK &&
           n == strlen(reason) && memcmp(text, reason, n) == 0 &&
           capnp_get_u16(&exc, 4, 0) == type;
  capnp_message_free(&m);
  return ok;
}

/* The one frame queued is a Release of `count` references to `id`. */
static int sent_release(queue_t *q, uint32_t id, uint32_t count)
{
  int ok = q->tail == 1 && frame_release(q, 0, id, count);
  q_drop(q);
  return ok;
}

/* Frame `i` of `q` is an Abort whose reason is `reason` and whose
 * Exception.type, at byte 4, is `type`. */
static int frame_abort(queue_t *q, int i, const char *reason, uint16_t type)
{
  capnp_message_t m = {0};
  capnp_ptr_t r, exc;
  const char *text = NULL;
  size_t n = 0;
  int ok = i < q->tail &&
           capnp_message_from_flat(&m, q->data[i], q->len[i]) == CAPNP_OK &&
           capnp_root(&m, &r) == CAPNP_OK &&
           capnp_get_u16(&r, 0, 0) == CAPNP_RPC_MSG_ABORT &&
           capnp_getp(&r, 0, &exc) == CAPNP_OK &&
           capnp_get_text(&exc, 0, &text, &n) == CAPNP_OK &&
           n == strlen(reason) && memcmp(text, reason, n) == 0 &&
           capnp_get_u16(&exc, 4, 0) == type;
  capnp_message_free(&m);
  return ok;
}

/* The one frame queued is such an Abort. */
static int sent_abort_typed(queue_t *q, const char *reason, uint16_t type)
{
  int ok = q->tail == 1 && frame_abort(q, 0, reason, type);
  q_drop(q);
  return ok;
}

static int sent_abort(queue_t *q, const char *reason)
{
  return sent_abort_typed(q, reason, 0);
}

/* An Abort whose reason is `reason` and whose Exception.type is `type`. */
static int feed_abort(capnp_rpc_conn_t *c, const char *reason, uint16_t type)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, exc;
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc = CAPNP_ERR_ALLOC;
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) == CAPNP_OK &&
      capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK &&
      capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_ABORT) == CAPNP_OK &&
      capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK &&
      capnp_builder_struct(&slot, 1, 2, &exc) == CAPNP_OK &&
      capnp_builder_set_text(&exc, 1, 0, reason, strlen(reason)) ==
          CAPNP_OK &&
      capnp_builder_set_u16(&exc, 4, type) == CAPNP_OK &&
      capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK)
    rc = capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
  return rc;
}

int main(void)
{
  /* static, not automatic: see test_rpc_join.c. */
  static capnp_rpc_conn_t client, vat;
  uint32_t q, arg;

  memset(&g_to_vat, 0, sizeof g_to_vat);
  memset(&g_to_client, 0, sizeof g_to_client);
  doubler_calls = 0;
  doubler_fail_from = 1 << 30;
  capnp_rpc_init(&client, send_to_vat, NULL);
  capnp_rpc_init(&vat, send_to_client, NULL);
  capnp_rpc_set_bootstrap(&vat, &doubler_calls, doubler_dispatch);

  /* Bootstrap resolves to the peer's capability. */
  q = capnp_rpc_send_bootstrap(&client);
  CHECK(q != (uint32_t)-1, "bootstrap sent");
  CHECK(!capnp_rpc_is_answered(&client, q), "not answered yet");
  pump(&vat, &g_to_vat);
  pump(&client, &g_to_client);
  CHECK(capnp_rpc_is_answered(&client, q), "bootstrap answered");
  CHECK(!capnp_rpc_is_failed(&client, q), "bootstrap succeeded");
  {
    capnp_message_t m = {0};
    capnp_ptr_t content;
    CHECK(capnp_rpc_answer_content(&client, q, &m, &content) == CAPNP_OK,
          "bootstrap content");
    /* The bootstrap answer's content is the capability itself. */
    CHECK(content.kind == CAPNP_PK_CAP, "bootstrap content is a capability");
    capnp_message_free(&m);
  }

  /* A call pipelined on the bootstrap answer reaches the capability it
   * returned. A question we never asked cannot be pipelined on. */
  {
    capnp_rpc_call_t call;
    uint32_t qp, boot = q;
    capnp_message_t m = {0};
    capnp_ptr_t content;
    memset(&call, 0, sizeof call);
    call.promised = 1;
    call.target = 999;
    call.interface_id = 0x1234;
    call.params_dwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    arg = 5;
    CHECK(capnp_rpc_send_call_ex(&client, &call) == (uint32_t)-1,
          "unknown question not pipelined");
    call.target = boot;
    qp = capnp_rpc_send_call_ex(&client, &call);
    CHECK(qp != (uint32_t)-1, "pipelined call sent");
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_answer_content(&client, qp, &m, &content) == CAPNP_OK &&
              capnp_get_u32(&content, 0, 0) == 10,
          "pipelined call answered");
    capnp_message_free(&m);
    capnp_rpc_send_finish(&client, qp);
    pump(&vat, &g_to_vat);
    doubler_calls = 0;
  }

  /* An exported cap in the params gains a reference for the peer to
   * release. An export we do not hold is refused before sending. */
  {
    static int local;
    capnp_rpc_call_t call;
    capnp_rpc_cap_ref_t ref;
    int eid = capnp_rpc_export(&client, &local, doubler_dispatch);
    uint32_t qc;
    memset(&call, 0, sizeof call);
    call.interface_id = 0x1234;
    call.params_dwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    call.caps = &ref;
    call.ncaps = 1;
    ref.kind = CAPNP_RPC_CAP_EXPORT;
    ref.id = (uint32_t)eid + 1;
    CHECK(capnp_rpc_send_call_ex(&client, &call) == (uint32_t)-1,
          "unknown export refused");
    ref.id = (uint32_t)eid;
    qc = capnp_rpc_send_call_ex(&client, &call);
    CHECK(qc != (uint32_t)-1 && client.exports[eid].refcount == 2,
          "exported cap referenced");
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_is_answered(&client, qc), "cap call answered");
    capnp_rpc_send_finish(&client, qc);
    pump(&vat, &g_to_vat);
    memset(&client.exports[eid], 0, sizeof client.exports[eid]);
    doubler_calls = 0;
  }

  /* A call returns the server's results to the caller. */
  arg = 21;
  q = capnp_rpc_send_call(&client, 0, 0x1234, 1, 1, 1, fill_u32, &arg);
  CHECK(q != (uint32_t)-1, "call sent");
  pump(&vat, &g_to_vat);
  pump(&client, &g_to_client);
  CHECK(capnp_rpc_is_answered(&client, q), "call answered");
  CHECK(!capnp_rpc_is_failed(&client, q), "call succeeded");
  {
    capnp_message_t m = {0};
    capnp_ptr_t content;
    CHECK(capnp_rpc_answer_content(&client, q, &m, &content) == CAPNP_OK,
          "call content");
    CHECK(capnp_get_u32(&content, 0, 0) == 42, "server doubled the argument");
    capnp_message_free(&m);
  }
  CHECK(doubler_calls == 1, "one dispatch");

  /* A call to an export the vat does not have aborts, as rpc.c++ does,
   * and so does a sendResultsTo arm other than caller and yourself. */
  q = capnp_rpc_send_call(&client, 99, 0, 0, 1, 1, NULL, NULL);
  pump(&vat, &g_to_vat);
  CHECK(sent_abort(&g_to_client, "Message target is not a current export ID."),
        "unroutable call aborts");
  {
    capnp_builder_t b;
    capnp_bptr_t root, msg, slot, call, target;
    uint8_t *flat = NULL;
    size_t len = 0;
    capnp_builder_init(&b);
    CHECK(capnp_builder_root(&b, &root) == CAPNP_OK &&
              capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK &&
              capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_CALL) == CAPNP_OK &&
              capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK &&
              capnp_builder_struct(&slot, 3, 3, &call) == CAPNP_OK &&
              capnp_builder_set_u32(&call, 0, 500) == CAPNP_OK &&
              capnp_builder_set_u16(&call, 6, 2) == CAPNP_OK &&
              capnp_builder_slot(&call, 3, 0, &slot) == CAPNP_OK &&
              capnp_builder_struct(&slot, 1, 1, &target) == CAPNP_OK &&
              capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK,
          "thirdParty call built");
    CHECK(capnp_rpc_handle(&vat, flat, len) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_client, "Unsupported `Call.sendResultsTo`."),
          "sendResultsTo.thirdParty aborts");
    free(flat);
    capnp_builder_free(&b);
  }
  q = capnp_rpc_send_call(&client, 0, 0x1234, 1, 1, 1, fill_u32, &arg);
  pump(&vat, &g_to_vat);
  pump(&client, &g_to_client);
  CHECK(capnp_rpc_is_answered(&client, q) && !capnp_rpc_is_failed(&client, q),
        "vat still answers after refusing");

  /* A method that fails is answered with an exception. */
  doubler_fail_from = doubler_calls + 1;
  q = capnp_rpc_send_call(&client, 0, 0x1234, 1, 1, 1, fill_u32, &arg);
  pump(&vat, &g_to_vat);
  pump(&client, &g_to_client);
  CHECK(capnp_rpc_is_answered(&client, q), "failed method answered");
  CHECK(capnp_rpc_is_failed(&client, q), "failed method is an exception");
  doubler_fail_from = 1 << 30;

  /* A dispatch chooses the exception it fails with; rpc.c++ words an
   * unimplemented method with the interface, type id and method. */
  CHECK(capnp_rpc_fail(CAPNP_RPC_EXCEPTION_FAILED, "x") == -1,
        "fail outside a dispatch");
  q = capnp_rpc_send_call(&client, 0, 0x1234, 11, 1, 1, fill_u32, &arg);
  pump(&vat, &g_to_vat);
  CHECK(frame_exception(&g_to_client, 0, "busy",
                        CAPNP_RPC_EXCEPTION_OVERLOADED),
        "the reason and type a dispatch chose");
  pump(&client, &g_to_client);
  q = capnp_rpc_send_call(&client, 0, 0x1234, 12, 1, 1, fill_u32, &arg);
  pump(&vat, &g_to_vat);
  CHECK(frame_exception(&g_to_client, 0,
                        "Method not implemented.; interfaceName = x.capnp:X; "
                        "typeId = 4660; methodId = 12",
                        CAPNP_RPC_EXCEPTION_UNIMPLEMENTED),
        "an unimplemented method worded as rpc.c++ words it");
  pump(&client, &g_to_client);
  q = capnp_rpc_send_call(&client, 0, 0x1234, 13, 1, 1, fill_u32, &arg);
  pump(&vat, &g_to_vat);
  pump(&client, &g_to_client);
  CHECK(capnp_rpc_is_answered(&client, q) && !capnp_rpc_is_failed(&client, q),
        "a dispatch that succeeds ignores capnp_rpc_fail");
  q = capnp_rpc_send_call(&client, 0, 0x1234, 1, 1, 1, fill_u32, &arg);
  doubler_fail_from = doubler_calls + 1;
  pump(&vat, &g_to_vat);
  CHECK(frame_exception(&g_to_client, 0, "method failed", 0),
        "the next failure is method failed again");
  pump(&client, &g_to_client);
  doubler_fail_from = 1 << 30;

  /* A Return of any size is recorded on the caller and kept by the vat
   * until Finish, as C++ has no frame limit. */
  {
    capnp_message_t m = {0};
    capnp_ptr_t content;
    const uint8_t *data = NULL;
    size_t n = 0;
    int i, kept = 0;
    q = capnp_rpc_send_call(&client, 0, 0x1234, 7, 1, 1, fill_u32, &arg);
    CHECK(q != (uint32_t)-1, "large call sent");
    pump(&vat, &g_to_vat);
    for (i = 0; i < CAPNP_RPC_MAX_ANSWERS; i++)
      kept |= vat.answers[i].used && vat.answers[i].question_id == q &&
              vat.answers[i].len > CAPNP_RPC_MAX_ANSWER_BYTES;
    CHECK(kept, "large answer kept for pipelining");
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_is_answered(&client, q), "large return answered");
    CHECK(capnp_rpc_answer_content(&client, q, &m, &content) == CAPNP_OK &&
              capnp_get_data(&content, 0, &data, &n) == CAPNP_OK &&
              n == 10000 && data[9999] == 0xab,
          "large return content");
    capnp_message_free(&m);
    CHECK(capnp_rpc_send_finish(&client, q) == CAPNP_OK, "large finish");
    pump(&vat, &g_to_vat);
    for (i = 0; i < CAPNP_RPC_MAX_ANSWERS; i++)
      CHECK(!(vat.answers[i].used && vat.answers[i].question_id == q),
            "large answer dropped at finish");
  }

  /* A Return for a question we never asked, or a second Return for one,
   * aborts as rpc.c++ does. Recording it would let a peer plant answers
   * that later pipelining would trust. */
  {
    uint32_t qd;
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, 77, 0, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "Invalid question ID in Return message."),
          "stray Return aborts");
    CHECK(!capnp_rpc_is_answered(&client, 77), "stray Return not recorded");
    qd = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qd, 0, 0) == CAPNP_OK &&
              capnp_rpc_is_answered(&client, qd),
          "first Return recorded");
    CHECK(feed_return(&client, qd, 0, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "expected question->isAwaitingReturn; "
                                    "Duplicate Return."),
          "duplicate Return aborts");
    capnp_rpc_send_finish(&client, qd);
    q_drop(&g_to_vat);
  }

  /* A question finished before its Return stays until that Return, which
   * only closes it. */
  {
    uint32_t qf;
    int i, kept = 0;
    qf = capnp_rpc_send_bootstrap(&client);
    CHECK(capnp_rpc_send_finish(&client, qf) == CAPNP_OK, "early finish");
    q_drop(&g_to_vat);
    for (i = 0; i < CAPNP_RPC_MAX_QUESTIONS; i++)
      kept |= client.questions[i].used && client.questions[i].question_id == qf;
    CHECK(kept && !capnp_rpc_is_answered(&client, qf),
          "finished question kept for its Return");
    CHECK(feed_return(&client, 0x80000000u | qf, 0, 0) == CAPNP_OK &&
              g_to_vat.tail == 0 && capnp_rpc_is_answered(&client, qf) == 0,
          "a Return for a high question id is ignored");
    CHECK(feed_return(&client, qf, 0, 0) == CAPNP_OK && g_to_vat.tail == 0,
          "late Return closes the question quietly");
    for (i = 0; i < CAPNP_RPC_MAX_QUESTIONS; i++)
      CHECK(!(client.questions[i].used && client.questions[i].question_id == qf),
            "late Return drops the question");
    CHECK(feed_return(&client, qf, 0, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "Invalid question ID in Return message."),
          "a Return after that one aborts");
  }

  /* A senderPromise descriptor names an export as senderHosted does. */
  {
    capnp_builder_t b;
    capnp_bptr_t root, msg, slot, ret, payload, desc;
    uint8_t *flat = NULL;
    size_t len = 0;
    uint32_t pq = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    capnp_builder_init(&b);
    CHECK(capnp_builder_root(&b, &root) == CAPNP_OK &&
              capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK &&
              capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RETURN) ==
                  CAPNP_OK &&
              capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK &&
              capnp_builder_struct(&slot, 2, 1, &ret) == CAPNP_OK &&
              capnp_builder_set_u32(&ret, 0, pq) == CAPNP_OK &&
              capnp_builder_slot(&ret, 2, 0, &slot) == CAPNP_OK &&
              capnp_builder_struct(&slot, 0, 2, &payload) == CAPNP_OK &&
              capnp_builder_set_cap(&payload, 0, 0, 0) == CAPNP_OK &&
              capnp_builder_set_list_struct(&payload, 0, 1, 1, 1, 1, &desc) ==
                  CAPNP_OK &&
              capnp_builder_set_u16(&desc, 0, 2) == CAPNP_OK &&
              capnp_builder_set_u32(&desc, 4, 7) == CAPNP_OK &&
              capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK,
          "senderPromise Return built");
    capnp_rpc_handle(&client, flat, len);
    free(flat);
    capnp_builder_free(&b);
    CHECK(capnp_rpc_answer_cap_id(&client, pq) == 7,
          "senderPromise names export 7");
  }

  /* A call with sendResultsTo.yourself is answered resultsSentElsewhere
   * once a takeFromOtherQuestion collects the results the vat kept. */
  {
    capnp_rpc_call_t call;
    capnp_message_t m = {0};
    capnp_ptr_t content;
    uint32_t qy, vq;
    memset(&call, 0, sizeof call);
    call.interface_id = 0x1234;
    call.params_dwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    call.results_to_yourself = 1;
    arg = 8;
    qy = capnp_rpc_send_call_ex(&client, &call);
    pump(&vat, &g_to_vat);
    CHECK(g_to_client.tail == 0, "kept results send nothing yet");
    vq = capnp_rpc_send_bootstrap(&vat);
    q_drop(&g_to_client);
    CHECK(feed_return(&vat, vq, 4, qy) == CAPNP_OK &&
              capnp_rpc_answer_content(&vat, vq, &m, &content) == CAPNP_OK &&
              capnp_get_u32(&content, 0, 0) == 16,
          "takeFromOtherQuestion takes the kept results");
    capnp_message_free(&m);
    CHECK(g_to_client.tail == 1, "the take sends one Return");
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_is_answered(&client, qy) &&
              !capnp_rpc_is_failed(&client, qy),
          "resultsSentElsewhere answers a tail call");
    vq = capnp_rpc_send_bootstrap(&vat);
    q_drop(&g_to_client);
    CHECK(feed_return(&vat, vq, 4, qy) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_client,
                         "`Return.takeFromOtherQuestion` referenced a call "
                         "that did not use `sendResultsTo.yourself`."),
          "kept results are taken once");
    capnp_rpc_send_finish(&client, qy);
    pump(&vat, &g_to_vat);
  }

  /* A method returns capabilities: its own export as senderHosted, and
   * one of the caller's handed back as receiverHosted. */
  {
    capnp_rpc_call_t call;
    capnp_rpc_cap_ref_t ref, out;
    uint32_t qc;
    int before = vat.exports[0].refcount;
    memset(&call, 0, sizeof call);
    call.interface_id = 0x1234;
    call.method_id = 9;
    call.params_dwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    ref.kind = CAPNP_RPC_CAP_EXPORT;
    ref.id = 0;
    CHECK(capnp_rpc_result_cap(&ref) == -1,
          "no result capability outside a method");
    arg = 0;
    qc = capnp_rpc_send_call_ex(&client, &call);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_answer_cap(&client, qc, 0, &out) == 0 &&
              out.kind == CAPNP_RPC_CAP_IMPORT && out.id == 0 &&
              capnp_rpc_answer_cap(&client, qc, 1, &out) == -1,
          "a returned export arrives as an import");
    CHECK(vat.exports[0].refcount == before + 1,
          "a returned export takes a reference");
    capnp_rpc_send_finish(&client, qc);
    pump(&vat, &g_to_vat);
    arg = 105;
    qc = capnp_rpc_send_call_ex(&client, &call);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_answer_cap(&client, qc, 0, &out) == 0 &&
              out.kind == CAPNP_RPC_CAP_EXPORT && out.id == 5,
          "a capability handed back arrives as our export");
    capnp_rpc_send_finish(&client, qc);
    pump(&vat, &g_to_vat);
    arg = 50;
    qc = capnp_rpc_send_call_ex(&client, &call);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_is_failed(&client, qc),
          "an export the vat lacks is no result capability");
    capnp_rpc_send_finish(&client, qc);
    pump(&vat, &g_to_vat);
    arg = 0;
    call.results_to_yourself = 1;
    qc = capnp_rpc_send_call_ex(&client, &call);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(vat.exports[0].refcount == before + 1,
          "redirected results take no reference");
    capnp_rpc_send_finish(&client, qc);
    pump(&vat, &g_to_vat);
    CHECK(vat.exports[0].refcount == before + 1,
          "finishing redirected results releases nothing");
    CHECK(feed_release(&vat, 0, 1) == CAPNP_OK &&
              vat.exports[0].refcount == before,
          "the returned reference comes back with Release");
  }

  /* Results a takeFromOtherQuestion collects name their capabilities
   * from the side that asked: the vat's own export comes back as an
   * export, and one the caller handed back as an import. */
  {
    capnp_rpc_call_t call;
    capnp_rpc_cap_ref_t out;
    uint32_t qy, vq;
    int pass;
    memset(&call, 0, sizeof call);
    call.interface_id = 0x1234;
    call.method_id = 9;
    call.params_dwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    call.results_to_yourself = 1;
    for (pass = 0; pass < 2; pass++) {
      arg = pass == 0 ? 0 : 105;
      qy = capnp_rpc_send_call_ex(&client, &call);
      pump(&vat, &g_to_vat);
      pump(&client, &g_to_client);
      vq = capnp_rpc_send_bootstrap(&vat);
      q_drop(&g_to_client);
      CHECK(feed_return(&vat, vq, 4, qy) == CAPNP_OK &&
                capnp_rpc_answer_cap(&vat, vq, 0, &out) == 0,
            "taken results carry their capability");
      if (pass == 0)
        CHECK(out.kind == CAPNP_RPC_CAP_EXPORT && out.id == 0 &&
                  capnp_rpc_answer_cap_id(&vat, vq) == -1,
              "a taken export of ours stays ours");
      else
        CHECK(out.kind == CAPNP_RPC_CAP_IMPORT && out.id == 5,
              "a taken capability of the caller's is an import");
      capnp_rpc_send_finish(&vat, vq);
      capnp_rpc_send_finish(&client, qy);
      pump(&vat, &g_to_vat);
      q_drop(&g_to_client);
    }
  }

  /* A Return with releaseParamCaps, the default the vat sends, gives back
   * the reference each export in the params took, even once finished. */
  {
    static int param_server;
    capnp_rpc_call_t call;
    capnp_rpc_cap_ref_t ref;
    uint32_t qp;
    int ce = capnp_rpc_export(&client, &param_server, doubler_dispatch);
    int before = client.exports[ce].refcount;
    ref.kind = CAPNP_RPC_CAP_EXPORT;
    ref.id = (uint32_t)ce;
    memset(&call, 0, sizeof call);
    call.interface_id = 0x1234;
    call.params_dwords = 1;
    call.params_pwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    call.caps = &ref;
    call.ncaps = 1;
    qp = capnp_rpc_send_call_ex(&client, &call);
    CHECK(client.exports[ce].refcount == before + 1, "params take a reference");
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(client.exports[ce].refcount == before,
          "releaseParamCaps gives it back");
    capnp_rpc_send_finish(&client, qp);
    pump(&vat, &g_to_vat);
    qp = capnp_rpc_send_call_ex(&client, &call);
    capnp_rpc_send_finish(&client, qp);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(client.exports[ce].refcount == before,
          "a late Return gives it back too");
  }

  /* A Return with noFinishNeeded is finished without a message, as
   * rpc.c++ skips it; results with a capability still need one. */
  {
    uint32_t qf;
    arg = 3;
    qf = capnp_rpc_send_call(&client, 0, 0x1234, 0, 1, 0, fill_u32, &arg);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_send_finish(&client, qf) == CAPNP_OK &&
              g_to_vat.tail == 0 && !capnp_rpc_is_answered(&client, qf),
          "noFinishNeeded skips the Finish");
    arg = 0;
    qf = capnp_rpc_send_call(&client, 0, 0x1234, 9, 1, 0, fill_u32, &arg);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_send_finish(&client, qf) == CAPNP_OK &&
              g_to_vat.tail == 1,
          "results with a capability are finished");
    pump(&vat, &g_to_vat);
  }

  /* A call with onlyPromisePipeline is never returned, as rpc.c++ sends
   * nothing for it; its answer stays for pipelining until Finish, and a
   * capability in it takes no reference. */
  {
    int i, kept = 0, before = vat.exports[0].refcount;
    q_drop(&g_to_client);
    CHECK(feed_quiet_call(&vat, 900, 0, 4) == CAPNP_OK &&
              g_to_client.tail == 0,
          "onlyPromisePipeline gets no Return");
    for (i = 0; i < CAPNP_RPC_MAX_ANSWERS; i++)
      kept |= vat.answers[i].used && vat.answers[i].question_id == 900;
    CHECK(kept, "its answer stays for pipelining");
    CHECK(feed_quiet_call(&vat, 901, 9, 0) == CAPNP_OK &&
              g_to_client.tail == 0 && vat.exports[0].refcount == before,
          "a capability in it takes no reference");
    capnp_rpc_send_finish(&client, 900);
    capnp_rpc_send_finish(&client, 901);
    pump(&vat, &g_to_vat);
    CHECK(g_to_client.tail == 0 && vat.exports[0].refcount == before,
          "Finish sends and releases nothing");
    for (i = 0, kept = 0; i < CAPNP_RPC_MAX_ANSWERS; i++)
      kept |= vat.answers[i].used && (vat.answers[i].question_id == 900 ||
                                      vat.answers[i].question_id == 901);
    CHECK(!kept, "Finish drops the answers");
  }

  /* A Resolve names a promise this vat never imported, so rpc.c++ drops
   * the new capability: one the peer hosts is released at once, one of
   * ours needs nothing. */
  q_drop(&g_to_client);
  CHECK(feed_resolve(&vat, 7, 1, 3) == CAPNP_OK &&
            sent_release(&g_to_client, 3, 1),
        "a resolution the peer hosts is released");
  CHECK(feed_resolve(&vat, 7, 2, 4) == CAPNP_OK &&
            sent_release(&g_to_client, 4, 1),
        "a resolution to a peer promise is released");
  CHECK(feed_resolve(&vat, 7, 3, 0) == CAPNP_OK && g_to_client.tail == 0,
        "a resolution to our own export is quiet");

  /* A vat with no bootstrap capability answers as rpc.c++ does: a fresh
   * promise, broken by a Resolve right after, so calls on it fail. */
  {
    static capnp_rpc_conn_t nb, cl;
    uint32_t bq, cq, v = 1;
    capnp_rpc_init(&nb, send_to_client, NULL);
    capnp_rpc_init(&cl, send_to_vat, NULL);
    q_drop(&g_to_vat);
    q_drop(&g_to_client);
    bq = capnp_rpc_send_bootstrap(&cl);
    pump(&nb, &g_to_vat);
    CHECK(g_to_client.tail == 2, "no bootstrap: a Return and a Resolve");
    pump(&cl, &g_to_client);
    CHECK(capnp_rpc_is_answered(&cl, bq) && !capnp_rpc_is_failed(&cl, bq),
          "no bootstrap: the question succeeds");
    {
      capnp_rpc_cap_ref_t ref;
      CHECK(capnp_rpc_answer_cap(&cl, bq, 0, &ref) == 0 &&
                ref.kind == CAPNP_RPC_CAP_IMPORT && ref.id == 0,
            "no bootstrap: a promise import");
    }
    cq = capnp_rpc_send_call(&cl, 0, 0x1234, 0, 1, 0, fill_u32, &v);
    pump(&nb, &g_to_vat);
    CHECK(frame_exception(&g_to_client, 0,
                          "This vat does not expose any public/bootstrap "
                          "interfaces.",
                          0),
          "no bootstrap: a call fails with the rpc.c++ reason");
    pump(&cl, &g_to_client);
    CHECK(capnp_rpc_is_failed(&cl, cq), "no bootstrap: the call failed");
    capnp_rpc_free(&nb);
    capnp_rpc_free(&cl);
  }

  /* An import a Return handed us is on the table until released. A
   * Resolve naming it when it was not a promise ends the connection, as
   * in rpc.c++ handleResolve, after the new capability is dropped. */
  {
    static capnp_rpc_conn_t h;
    uint32_t bq;
    capnp_rpc_init(&h, send_to_vat, NULL);
    bq = capnp_rpc_send_bootstrap(&h);
    q_drop(&g_to_vat);
    CHECK(feed_cap_return(&h, bq, 1, 6) == CAPNP_OK && g_to_vat.tail == 0,
          "import: hosted bootstrap");
    CHECK(feed_resolve(&h, 9, 1, 3) == CAPNP_OK &&
              sent_release(&g_to_vat, 3, 1),
          "import: an unknown promise drops the resolution");
    CHECK(feed_resolve(&h, 6, 1, 3) == CAPNP_ERR_KIND &&
              g_to_vat.tail == 2 && frame_release(&g_to_vat, 0, 3, 1) &&
              frame_abort(&g_to_vat, 1,
                          "Got 'Resolve' for a non-promise import.", 0),
          "import: a held import drops the resolution, then aborts");
    q_drop(&g_to_vat);

    capnp_rpc_init(&h, send_to_vat, NULL);
    bq = capnp_rpc_send_bootstrap(&h);
    q_drop(&g_to_vat);
    CHECK(feed_cap_return(&h, bq, 1, 6) == CAPNP_OK, "import: hosted again");
    CHECK(feed_resolve(&h, 6, RESOLVE_EXCEPTION, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "Got 'Resolve' for a non-promise import."),
          "import: a broken resolution of a held import aborts");

    capnp_rpc_init(&h, send_to_vat, NULL);
    bq = capnp_rpc_send_bootstrap(&h);
    q_drop(&g_to_vat);
    CHECK(feed_cap_return(&h, bq, 1, 6) == CAPNP_OK, "import: hosted twice");
    CHECK(capnp_rpc_send_release(&h, 6, 1) == CAPNP_OK &&
              sent_release(&g_to_vat, 6, 1),
          "import: released");
    CHECK(feed_resolve(&h, 6, 1, 3) == CAPNP_OK &&
              sent_release(&g_to_vat, 3, 1),
          "import: a released import is unknown again");

    capnp_rpc_init(&h, send_to_vat, NULL);
    bq = capnp_rpc_send_bootstrap(&h);
    q_drop(&g_to_vat);
    CHECK(feed_cap_return(&h, bq, 2, 6) == CAPNP_OK, "import: promise");
    CHECK(feed_resolve(&h, 6, 1, 3) == CAPNP_OK &&
              sent_release(&g_to_vat, 3, 1),
          "import: a promise import takes its resolution");
    CHECK(feed_resolve(&h, 6, RESOLVE_EXCEPTION, 0) == CAPNP_OK &&
              g_to_vat.tail == 0,
          "import: a promise import takes a broken resolution");

    capnp_rpc_init(&h, send_to_vat, NULL);
    bq = capnp_rpc_send_bootstrap(&h);
    q_drop(&g_to_vat);
    CHECK(feed_cap_return(&h, bq, 1, 6) == CAPNP_OK, "import: hosted once more");
    CHECK(feed_resolve(&h, 9, 1, 6) == CAPNP_OK && g_to_vat.tail == 0,
          "import: a resolution naming a held import adds a reference");
    CHECK(capnp_rpc_send_release(&h, 6, 1) == CAPNP_OK &&
              sent_release(&g_to_vat, 6, 2),
          "import: the last release gives that reference back");
    capnp_rpc_free(&h);
  }

  /* Kept results nobody takes are answered resultsSentElsewhere when the
   * caller finishes them. */
  {
    capnp_rpc_call_t call;
    uint32_t qy;
    memset(&call, 0, sizeof call);
    call.interface_id = 0x1234;
    call.params_dwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    call.results_to_yourself = 1;
    qy = capnp_rpc_send_call_ex(&client, &call);
    pump(&vat, &g_to_vat);
    q_drop(&g_to_client);
    capnp_rpc_send_finish(&client, qy);
    pump(&vat, &g_to_vat);
    CHECK(g_to_client.tail == 1, "Finish of untaken results sends a Return");
    pump(&client, &g_to_client);
    CHECK(!capnp_rpc_is_answered(&client, qy), "and that Return ends it");
  }

  /* A tail call back to us whose question we already finished frees the
   * kept results, as rpc.c++ drops the answer's task. */
  {
    capnp_rpc_call_t call;
    uint32_t qy, vq;
    int i, takeable = 0, kept = 0;
    memset(&call, 0, sizeof call);
    call.interface_id = 0x1234;
    call.params_dwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    call.results_to_yourself = 1;
    qy = capnp_rpc_send_call_ex(&client, &call);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    vq = capnp_rpc_send_bootstrap(&vat);
    capnp_rpc_send_finish(&vat, vq);
    q_drop(&g_to_client);
    CHECK(feed_return(&vat, vq, 4, qy) == CAPNP_OK && g_to_client.tail == 1,
          "late takeFromOtherQuestion answers the redirected call");
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_is_answered(&client, qy) &&
              !capnp_rpc_is_failed(&client, qy),
          "that answer is resultsSentElsewhere");
    for (i = 0; i < CAPNP_RPC_MAX_ANSWERS; i++) {
      if (vat.answers[i].used && vat.answers[i].question_id == qy) {
        kept = 1;
        takeable |= vat.answers[i].used == 2;
      }
    }
    CHECK(kept && !takeable, "late takeFromOtherQuestion frees the results");
    capnp_rpc_send_finish(&client, qy);
    pump(&vat, &g_to_vat);
    q_drop(&g_to_client);
  }

  /* The other Return arms abort the connection as rpc.c++ does, and
   * every question still waiting fails. */
  {
    capnp_rpc_call_t call;
    uint32_t qt, qn, vq;
    memset(&call, 0, sizeof call);
    call.interface_id = 0x1234;
    call.params_dwords = 1;
    call.fill = fill_u32;
    call.fill_ctx = &arg;
    call.results_to_yourself = 1;
    qt = capnp_rpc_send_call_ex(&client, &call);
    qn = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qt, 0, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "expected !question->isTailCall; Tail "
                                    "call `Return` must set "
                                    "`resultsSentElsewhere`, not `results`."),
          "tail call refuses results");
    CHECK(capnp_rpc_is_failed(&client, qt) && capnp_rpc_is_failed(&client, qn),
          "abort fails waiting questions");
    qt = capnp_rpc_send_call_ex(&client, &call);
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qt, 1, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "expected !question->isTailCall; Tail "
                                    "call `Return` must set "
                                    "`resultsSentElsewhere`, not "
                                    "`exception`."),
          "tail call refuses an exception");
    qn = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qn, 3, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "expected question->isTailCall; `Return` "
                                    "had `resultsSentElsewhere` but this "
                                    "was not a tail call."),
          "resultsSentElsewhere needs a tail call");
    qn = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qn, 4, 4242) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "`Return.takeFromOtherQuestion` had "
                                    "invalid answer ID."),
          "takeFromOtherQuestion needs a known answer");
    qn = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qn, 2, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "Return message falsely claims call "
                                    "was canceled."),
          "canceled needs a Finish first");
    qn = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qn, 5, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "Unknown 'Return' type."),
          "acceptFromThirdParty is refused");
    qn = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qn, 9, 0) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_vat, "Unknown 'Return' type."),
          "an unknown Return arm is refused");
    qn = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(capnp_rpc_send_finish(&client, qn) == CAPNP_OK, "finish first");
    q_drop(&g_to_vat);
    CHECK(feed_return(&client, qn, 2, 0) == CAPNP_OK && g_to_vat.tail == 0,
          "canceled after Finish is quiet");
    qn = capnp_rpc_send_call(&client, 0, 0x1234, 1, 1, 1, fill_u32, &arg);
    pump(&vat, &g_to_vat);
    q_drop(&g_to_client);
    vq = capnp_rpc_send_bootstrap(&vat);
    q_drop(&g_to_client);
    CHECK(feed_return(&vat, vq, 4, qn) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_client,
                         "`Return.takeFromOtherQuestion` had invalid answer "
                         "ID."),
          "results without capabilities are forgotten at once");
    arg = 0;
    qn = capnp_rpc_send_call(&client, 0, 0x1234, 9, 1, 1, fill_u32, &arg);
    pump(&vat, &g_to_vat);
    q_drop(&g_to_client);
    vq = capnp_rpc_send_bootstrap(&vat);
    q_drop(&g_to_client);
    CHECK(feed_return(&vat, vq, 4, qn) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_client,
                         "`Return.takeFromOtherQuestion` referenced a call "
                         "that did not use `sendResultsTo.yourself`."),
          "takeFromOtherQuestion needs sendResultsTo.yourself");
    capnp_rpc_send_finish(&client, qn);
    pump(&vat, &g_to_vat);
    q_drop(&g_to_client);
  }

  /* The stream window bounds how many calls are outstanding at once. */
  {
    capnp_rpc_stream_t s;
    uint32_t v = 1;
    capnp_rpc_stream_init(&s, 2);
    CHECK(capnp_rpc_stream_send(&client, &s, 0, 0, 0, 1, 1, fill_u32, &v) == CAPNP_OK, "s1");
    CHECK(capnp_rpc_stream_send(&client, &s, 0, 0, 0, 1, 1, fill_u32, &v) == CAPNP_OK, "s2");
    CHECK(s.nout == 2, "window holds two");
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_stream_send(&client, &s, 0, 0, 0, 1, 1, fill_u32, &v) == CAPNP_OK, "s3");
    CHECK(s.nout <= 2, "window still bounded");
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_stream_finish(&client, &s) == CAPNP_OK, "stream finished clean");
    CHECK(s.nout == 0, "window drained");
  }

  /* finish reports a call-level failure after draining the window. */
  {
    capnp_rpc_stream_t s;
    uint32_t v = 1;
    doubler_fail_from = doubler_calls + 2;
    capnp_rpc_stream_init(&s, 4);
    capnp_rpc_stream_send(&client, &s, 0, 0, 0, 1, 1, fill_u32, &v);
    capnp_rpc_stream_send(&client, &s, 0, 0, 0, 1, 1, fill_u32, &v);
    capnp_rpc_stream_send(&client, &s, 0, 0, 0, 1, 1, fill_u32, &v);
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_stream_finish(&client, &s) != CAPNP_OK, "finish reports failure");
    CHECK(s.nout == 0, "window still drained");
    CHECK(s.failed, "stream marked failed");
    /* The window is empty, so this refusal can only come from the stream
     * remembering it failed. */
    CHECK(capnp_rpc_stream_send(&client, &s, 0, 0, 0, 1, 1, fill_u32, &v) != CAPNP_OK,
          "failed stream refuses further sends");
  }

  /* Finish asks the peer to release result capabilities only while the
   * Return is still outstanding; afterwards they are our imports. */
  {
    uint32_t done = capnp_rpc_send_bootstrap(&client);
    uint32_t pending;
    int k;
    pump(&vat, &g_to_vat);
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_is_answered(&client, done), "finish: answered");
    pending = capnp_rpc_send_bootstrap(&client);
    q_drop(&g_to_vat);
    CHECK(capnp_rpc_send_finish(&client, done) == CAPNP_OK, "finish answered");
    CHECK(capnp_rpc_send_finish(&client, pending) == CAPNP_OK,
          "finish pending");
    CHECK(g_to_vat.tail == 2, "two finishes");
    for (k = 0; k < 2 && k < g_to_vat.tail; k++) {
      capnp_message_t m = {0};
      capnp_ptr_t r, fin;
      CHECK(capnp_message_from_flat(&m, g_to_vat.data[k], g_to_vat.len[k]) ==
                    CAPNP_OK &&
                capnp_root(&m, &r) == CAPNP_OK &&
                capnp_get_u16(&r, 0, 0) == CAPNP_RPC_MSG_FINISH &&
                capnp_getp(&r, 0, &fin) == CAPNP_OK,
            "finish frame");
      /* releaseResultCaps is stored XOR its default of true. */
      CHECK(capnp_get_bool(&fin, 32, 0) == (k == 0),
            k == 0 ? "answered finish keeps caps" : "pending finish drops caps");
      capnp_message_free(&m);
    }
    pump(&vat, &g_to_vat);
  }

  /* A Finish that crosses the Return releases the capabilities in it. */
  {
    uint32_t cancelled = capnp_rpc_send_bootstrap(&client);
    int before;
    pump(&vat, &g_to_vat);
    before = vat.exports[0].refcount;
    CHECK(vat.exports[0].used && before > 0, "cancel: exported");
    CHECK(capnp_rpc_send_finish(&client, cancelled) == CAPNP_OK,
          "cancel: finish");
    pump(&vat, &g_to_vat);
    CHECK(vat.exports[0].used ? vat.exports[0].refcount == before - 1
                              : before == 1,
          "cancelled finish releases the result capability");
    pump(&client, &g_to_client);
  }

  /* A peer controls Release.referenceCount. A count above the export's,
   * even one the local signed counter cannot hold, aborts as rpc.c++
   * does, and so does a Release naming no export. */
  {
    char want[128];
    snprintf(want, sizeof want,
             "expected refcount <= exp->refcount [%u <= %d]; Tried to drop "
             "export's refcount below zero.",
             (unsigned)UINT32_MAX, vat.exports[0].refcount);
    q_drop(&g_to_client);
    CHECK(feed_release(&vat, 0, UINT32_MAX) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_client, want),
          "large release aborts");
    CHECK(vat.exports[0].used, "large release keeps the export");
    CHECK(feed_release(&vat, 63, 1) == CAPNP_ERR_KIND &&
              sent_abort(&g_to_client, "Tried to release invalid export ID."),
          "release of an unknown export aborts");
    CHECK(feed_release(&vat, 0, (uint32_t)vat.exports[0].refcount) ==
                  CAPNP_OK &&
              !vat.exports[0].used && g_to_client.tail == 0,
          "releasing every reference drops the export");
  }

  /* Abort ends the connection: unanswered questions fail, and the
   * Abort is answered once with a remote exception, as rpc.c++ does when
   * it disconnects. A vat that has sent its own Abort reads nothing more
   * in rpc.c++, so it leaves the answer to that Abort unanswered. */
  {
    capnp_builder_t b;
    capnp_bptr_t root, msg;
    uint8_t *flat = NULL;
    size_t len = 0;
    uint32_t pending;
    capnp_rpc_free(&client);
    capnp_rpc_init(&client, send_to_vat, NULL);
    pending = capnp_rpc_send_bootstrap(&client);
    CHECK(pending != (uint32_t)-1, "pending bootstrap");
    q_drop(&g_to_vat);
    capnp_builder_init(&b);
    CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "abort root");
    CHECK(capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK, "abort msg");
    CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_ABORT) == CAPNP_OK,
          "abort tag");
    CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK,
          "abort serialize");
    CHECK(capnp_rpc_handle(&client, flat, len) == CAPNP_ERR_IO,
          "abort reported");
    CHECK(sent_abort(&g_to_vat, "remote exception: "),
          "abort answered with a remote exception");
    CHECK(capnp_rpc_is_failed(&client, pending), "pending question failed");
    free(flat);
    capnp_builder_free(&b);

    /* The vat refuses a Release; a client that has not yet seen an Abort
     * answers it, and the vat leaves the answer unanswered. */
    capnp_rpc_free(&client);
    capnp_rpc_init(&client, send_to_vat, NULL);
    CHECK(feed_release(&vat, 63, 1) == CAPNP_ERR_KIND, "vat aborts");
    CHECK(g_to_client.tail == 1, "one Abort to the client");
    pump(&client, &g_to_client);
    CHECK(g_to_vat.tail == 1, "client answers once");
    pump(&vat, &g_to_vat);
    CHECK(g_to_client.tail == 0, "vat does not answer the answer");
  }

  /* rpc.c++ keeps the type of the Abort it echoes, and a reason that
   * already carries the remote prefix is echoed unchanged. */
  {
    static capnp_rpc_conn_t fresh;
    capnp_rpc_init(&fresh, send_to_client, NULL);
    q_drop(&g_to_client);
    CHECK(feed_abort(&fresh, "remote exception: gone", 2) == CAPNP_ERR_IO &&
              sent_abort_typed(&g_to_client, "remote exception: gone", 2),
          "prefixed abort echoed once with its type");
    CHECK(feed_abort(&fresh, "remote exception: gone", 2) == CAPNP_ERR_IO &&
              g_to_client.tail == 0,
          "second abort unanswered");
    capnp_rpc_init(&fresh, send_to_client, NULL);
    CHECK(feed_abort(&fresh, "gone", 1) == CAPNP_ERR_IO &&
              sent_abort_typed(&g_to_client, "remote exception: gone", 1),
          "abort echoed with the remote prefix and its type");
    capnp_rpc_free(&fresh);
  }

  capnp_rpc_free(&client);
  capnp_rpc_free(&vat);
  if (g_failures != 0) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("ok test_rpc_client\n");
  return 0;
}
