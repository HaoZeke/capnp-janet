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
  if (mid == 7) {
    static uint8_t big[10000];
    memset(big, 0xab, sizeof big);
    if (capnp_builder_struct(results, 0, 1, &body))
      return CAPNP_ERR_ALLOC;
    return capnp_builder_set_data(&body, 0, 0, big, sizeof big);
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

/* The one frame queued is an Abort whose reason is `reason`. */
static int sent_abort(queue_t *q, const char *reason)
{
  capnp_message_t m = {0};
  capnp_ptr_t r, exc;
  const char *text = NULL;
  size_t n = 0;
  int ok = q->tail == 1 &&
           capnp_message_from_flat(&m, q->data[0], q->len[0]) == CAPNP_OK &&
           capnp_root(&m, &r) == CAPNP_OK &&
           capnp_get_u16(&r, 0, 0) == CAPNP_RPC_MSG_ABORT &&
           capnp_getp(&r, 0, &exc) == CAPNP_OK &&
           capnp_get_text(&exc, 0, &text, &n) == CAPNP_OK &&
           n == strlen(reason) && memcmp(text, reason, n) == 0;
  capnp_message_free(&m);
  q_drop(q);
  return ok;
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

  /* A call the vat cannot route comes back failed, not silent. */
  q = capnp_rpc_send_call(&client, 99, 0, 0, 1, 1, NULL, NULL);
  pump(&vat, &g_to_vat);
  pump(&client, &g_to_client);
  CHECK(capnp_rpc_is_answered(&client, q), "unroutable call answered");
  CHECK(capnp_rpc_is_failed(&client, q), "unroutable call failed");

  /* A method that fails is answered with an exception. */
  doubler_fail_from = doubler_calls + 1;
  q = capnp_rpc_send_call(&client, 0, 0x1234, 1, 1, 1, fill_u32, &arg);
  pump(&vat, &g_to_vat);
  pump(&client, &g_to_client);
  CHECK(capnp_rpc_is_answered(&client, q), "failed method answered");
  CHECK(capnp_rpc_is_failed(&client, q), "failed method is an exception");
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

  /* A Return for a question we never asked is ignored. */
  {
    capnp_builder_t b;
    capnp_bptr_t root, msg, slot, ret;
    uint8_t *flat = NULL;
    size_t len = 0;
    capnp_builder_init(&b);
    CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
    CHECK(capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK, "msg");
    CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RETURN) == CAPNP_OK, "tag");
    CHECK(capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK, "slot");
    CHECK(capnp_builder_struct(&slot, 2, 1, &ret) == CAPNP_OK, "ret");
    CHECK(capnp_builder_set_u32(&ret, 0, 77) == CAPNP_OK, "answerId 77");
    CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
    capnp_rpc_handle(&client, flat, len);
    free(flat);
    capnp_builder_free(&b);
    /* Recording it would let a peer plant answers to questions we never
     * asked, which later pipelining would then trust. */
    CHECK(!capnp_rpc_is_answered(&client, 77), "stray Return ignored");
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

  /* A call with sendResultsTo.yourself is answered resultsSentElsewhere,
   * and the vat keeps its results for a takeFromOtherQuestion. */
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
    pump(&client, &g_to_client);
    CHECK(capnp_rpc_is_answered(&client, qy) &&
              !capnp_rpc_is_failed(&client, qy),
          "resultsSentElsewhere answers a tail call");
    vq = capnp_rpc_send_bootstrap(&vat);
    q_drop(&g_to_client);
    CHECK(feed_return(&vat, vq, 4, qy) == CAPNP_OK &&
              capnp_rpc_answer_content(&vat, vq, &m, &content) == CAPNP_OK &&
              capnp_get_u32(&content, 0, 0) == 16,
          "takeFromOtherQuestion takes the kept results");
    capnp_message_free(&m);
    CHECK(g_to_client.tail == 0, "takeFromOtherQuestion sends nothing");
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
    qn = capnp_rpc_send_call(&client, 0, 0x1234, 1, 1, 1, fill_u32, &arg);
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

  /* A peer controls Release.referenceCount. A value that does not fit the
   * local signed counter must release the export rather than overflowing. */
  {
    capnp_builder_t b;
    capnp_bptr_t root, msg, slot, rel;
    uint8_t *flat = NULL;
    size_t len = 0;
    capnp_builder_init(&b);
    CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "release root");
    CHECK(capnp_builder_struct(&root, 1, 1, &msg) == CAPNP_OK, "release msg");
    CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RELEASE) == CAPNP_OK,
          "release tag");
    CHECK(capnp_builder_slot(&msg, 1, 0, &slot) == CAPNP_OK, "release slot");
    CHECK(capnp_builder_struct(&slot, 1, 0, &rel) == CAPNP_OK, "release body");
    CHECK(capnp_builder_set_u32(&rel, 0, 0) == CAPNP_OK, "release id");
    CHECK(capnp_builder_set_u32(&rel, 4, UINT32_MAX) == CAPNP_OK,
          "release count");
    CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK,
          "release serialize");
    CHECK(capnp_rpc_handle(&vat, flat, len) == CAPNP_OK, "release handle");
    CHECK(!vat.exports[0].used, "large release drops export");
    free(flat);
    capnp_builder_free(&b);
  }

  /* Abort ends the connection: unanswered questions fail and nothing
   * goes back, or two vats would trade replies forever. */
  {
    capnp_builder_t b;
    capnp_bptr_t root, msg;
    uint8_t *flat = NULL;
    size_t len = 0;
    uint32_t pending = capnp_rpc_send_bootstrap(&client);
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
    CHECK(g_to_vat.tail == 0, "abort not answered");
    CHECK(capnp_rpc_is_failed(&client, pending), "pending question failed");
    free(flat);
    capnp_builder_free(&b);
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
