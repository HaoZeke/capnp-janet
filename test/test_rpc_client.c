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

static int doubler_calls;
static int doubler_fail_from;

static int doubler_dispatch(void *server, uint64_t iid, uint16_t mid,
                            const capnp_ptr_t *params,
                            const capnp_bptr_t *results)
{
  (void)server;
  (void)iid;
  (void)mid;
  doubler_calls++;
  if (doubler_calls >= doubler_fail_from)
    return CAPNP_ERR_ARG;
  {
    uint32_t in = params->kind == CAPNP_PK_STRUCT
                      ? capnp_get_u32(params, 0, 0)
                      : 0;
    return capnp_builder_set_u32(results, 0, in * 2);
  }
}

static void fill_u32(void *ctx, const capnp_bptr_t *params)
{
  CHECK(capnp_builder_set_u32(params, 0, *(uint32_t *)ctx) == CAPNP_OK, "fill");
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
    capnp_message_t m;
    capnp_ptr_t content;
    CHECK(capnp_rpc_answer_content(&client, q, &m, &content) == CAPNP_OK,
          "bootstrap content");
    /* The bootstrap answer's content is the capability itself. */
    CHECK(content.kind == CAPNP_PK_CAP, "bootstrap content is a capability");
    capnp_message_free(&m);
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
    capnp_message_t m;
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

  if (g_failures != 0) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("ok test_rpc_client\n");
  return 0;
}
