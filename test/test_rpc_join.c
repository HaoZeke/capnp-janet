/* Level 4 `Join`: does a set of capabilities name one object?
 *
 * The vat is driven with raw messages rather than through a client
 * facade, so each assertion is about the wire behaviour the spec
 * prescribes and not about a convenience layer on top of it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_rpc.h>

#include "harness.h"

/* rpc.capnp shapes, matching capnp_rpc.c. */
#define MESSAGE_DW 1
#define MESSAGE_PW 1
#define JOIN_DW 1
#define JOIN_PW 2
#define TARGET_DW 1
#define TARGET_PW 1
#define JKP_DW 1
#define JKP_PW 0
#define BOOTSTRAP_DW 1
#define BOOTSTRAP_PW 1
#define RETURN_DW 2
#define RETURN_PW 1
#define PAYLOAD_DW 0
#define PAYLOAD_PW 2

#define MAX_FRAMES 8

typedef struct {
  uint8_t *data[MAX_FRAMES];
  size_t len[MAX_FRAMES];
  int n;
} outbox_t;

static int collect(void *ctx, const uint8_t *data, size_t len)
{
  outbox_t *o = (outbox_t *)ctx;
  if (o->n >= MAX_FRAMES)
    return -1;
  o->data[o->n] = (uint8_t *)malloc(len);
  memcpy(o->data[o->n], data, len);
  o->len[o->n] = len;
  o->n++;
  return 0;
}

static void outbox_clear(outbox_t *o)
{
  int i;
  for (i = 0; i < o->n; i++)
    free(o->data[i]);
  o->n = 0;
}

static int counting_dispatch(void *server, uint64_t iid, uint16_t mid,
                             const capnp_ptr_t *params,
                             const capnp_bptr_t *results)
{
  int *calls = (int *)server;
  (void)iid;
  (void)mid;
  (void)params;
  (*calls)++;
  return capnp_builder_set_u32(results, 0, (uint32_t)*calls);
}

/* Send one Join part naming `export_id`. */
static void send_join_part(capnp_rpc_conn_t *c, uint32_t qid, uint32_t export_id,
                           uint32_t join_id, uint16_t pcount, uint16_t pnum)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, join, target, key;
  uint8_t *flat = NULL;
  size_t len = 0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "capnp_builder_root(&b, &root) == CAPNP_OK");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg)...");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_JOIN) == CAPNP_OK, "capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_JOIN) == CAP...");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK");
  CHECK(capnp_builder_struct(&slot, JOIN_DW, JOIN_PW, &join) == CAPNP_OK, "capnp_builder_struct(&slot, JOIN_DW, JOIN_PW, &join) == C...");
  CHECK(capnp_builder_set_u32(&join, 0, qid) == CAPNP_OK, "capnp_builder_set_u32(&join, 0, qid) == CAPNP_OK");

  CHECK(capnp_builder_slot(&join, JOIN_DW, 0, &slot) == CAPNP_OK, "capnp_builder_slot(&join, JOIN_DW, 0, &slot) == CAPNP_OK");
  CHECK(capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target) == CAPNP_OK, "capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target...");
  CHECK(capnp_builder_set_u16(&target, 4, 0) == CAPNP_OK, "capnp_builder_set_u16(&target, 4, 0) == CAPNP_OK"); /* importedCap */
  CHECK(capnp_builder_set_u32(&target, 0, export_id) == CAPNP_OK, "capnp_builder_set_u32(&target, 0, export_id) == CAPNP_OK");

  CHECK(capnp_builder_slot(&join, JOIN_DW, 1, &slot) == CAPNP_OK, "capnp_builder_slot(&join, JOIN_DW, 1, &slot) == CAPNP_OK");
  CHECK(capnp_builder_struct(&slot, JKP_DW, JKP_PW, &key) == CAPNP_OK, "capnp_builder_struct(&slot, JKP_DW, JKP_PW, &key) == CAPN...");
  CHECK(capnp_builder_set_u32(&key, 0, join_id) == CAPNP_OK, "capnp_builder_set_u32(&key, 0, join_id) == CAPNP_OK");
  CHECK(capnp_builder_set_u16(&key, 4, pcount) == CAPNP_OK, "capnp_builder_set_u16(&key, 4, pcount) == CAPNP_OK");
  CHECK(capnp_builder_set_u16(&key, 6, pnum) == CAPNP_OK, "capnp_builder_set_u16(&key, 6, pnum) == CAPNP_OK");

  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK");
  CHECK(capnp_rpc_handle(c, flat, len) == CAPNP_OK, "capnp_rpc_handle(c, flat, len) == CAPNP_OK");
  free(flat);
  capnp_builder_free(&b);
}

typedef struct {
  uint32_t answer_id;
  uint32_t join_id;
  int succeeded;
  int has_cap;
} join_reply_t;

static join_reply_t read_join_reply(const uint8_t *data, size_t len)
{
  capnp_message_t m;
  capnp_ptr_t root, ret, payload, jr;
  join_reply_t out;
  memset(&out, 0, sizeof out);

  CHECK(capnp_message_from_flat(&m, data, len) == CAPNP_OK, "capnp_message_from_flat(&m, data, len) == CAPNP_OK");
  CHECK(capnp_root(&m, &root) == CAPNP_OK, "capnp_root(&m, &root) == CAPNP_OK");
  CHECK(capnp_get_u16(&root, 0, 0) == CAPNP_RPC_MSG_RETURN, "capnp_get_u16(&root, 0, 0) == CAPNP_RPC_MSG_RETURN");
  CHECK(capnp_getp(&root, 0, &ret) == CAPNP_OK, "capnp_getp(&root, 0, &ret) == CAPNP_OK");
  out.answer_id = capnp_get_u32(&ret, 0, 0);
  CHECK(capnp_getp(&ret, 0, &payload) == CAPNP_OK, "capnp_getp(&ret, 0, &payload) == CAPNP_OK");
  CHECK(capnp_getp(&payload, 0, &jr) == CAPNP_OK, "capnp_getp(&payload, 0, &jr) == CAPNP_OK");
  out.join_id = capnp_get_u32(&jr, 0, 0);
  out.succeeded = capnp_get_bool(&jr, 32, 0);
  {
    capnp_ptr_t cap;
    out.has_cap = capnp_getp(&jr, 0, &cap) == CAPNP_OK &&
                  cap.kind == CAPNP_PK_CAP;
  }
  capnp_message_free(&m);
  return out;
}

/* Bootstrap once so the vat holds a live export (id 0). */
static void bootstrap_once(capnp_rpc_conn_t *c, outbox_t *o)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, boot;
  uint8_t *flat = NULL;
  size_t len = 0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "capnp_builder_root(&b, &root) == CAPNP_OK");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg)...");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_BOOTSTRAP) == CAPNP_OK, "capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_BOOTSTRAP) =...");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK");
  CHECK(capnp_builder_struct(&slot, BOOTSTRAP_DW, BOOTSTRAP_PW, &boot) == CAPNP_OK, "capnp_builder_struct(&slot, BOOTSTRAP_DW, BOOTSTRAP_PW, &...");
  CHECK(capnp_builder_set_u32(&boot, 0, 1) == CAPNP_OK, "capnp_builder_set_u32(&boot, 0, 1) == CAPNP_OK");
  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK");
  CHECK(capnp_rpc_handle(c, flat, len) == CAPNP_OK, "capnp_rpc_handle(c, flat, len) == CAPNP_OK");
  free(flat);
  capnp_builder_free(&b);
  outbox_clear(o); /* the Return itself is not what these tests read */
}

int main(void)
{
  outbox_t out;
  capnp_rpc_conn_t c;
  int calls = 0;

  /* Two parts naming one capability join, and one result carries the cap. */
  memset(&out, 0, sizeof out);
  capnp_rpc_init(&c, collect, &out);
  capnp_rpc_set_bootstrap(&c, &calls, counting_dispatch);
  bootstrap_once(&c, &out);

  send_join_part(&c, 700, 0, 9, 2, 0);
  /* An incomplete set is not answerable, so nothing comes back yet. */
  CHECK(out.n == 0, "out.n == 0");
  send_join_part(&c, 701, 0, 9, 2, 1);
  CHECK(out.n == 2, "out.n == 2");
  {
    join_reply_t a = read_join_reply(out.data[0], out.len[0]);
    join_reply_t b = read_join_reply(out.data[1], out.len[1]);
    CHECK(a.join_id == 9 && b.join_id == 9, "a.join_id == 9 && b.join_id == 9");
    CHECK(a.succeeded && b.succeeded, "a.succeeded && b.succeeded");
    CHECK((a.answer_id == 700 && b.answer_id == 701) ||
              (a.answer_id == 701 && b.answer_id == 700),
          "both join questions answered");
    /* JoinResult: exactly one of the set carries the joined capability. */
    CHECK((a.has_cap ? 1 : 0) + (b.has_cap ? 1 : 0) == 1, "(a.has_cap ? 1 : 0) + (b.has_cap ? 1 : 0) == 1");
  }
  outbox_clear(&out);

  /* A part naming nothing we host fails the whole set. */
  send_join_part(&c, 710, 0, 11, 2, 0);
  send_join_part(&c, 711, 40, 11, 2, 1);
  CHECK(out.n == 2, "out.n == 2");
  {
    join_reply_t a = read_join_reply(out.data[0], out.len[0]);
    join_reply_t b = read_join_reply(out.data[1], out.len[1]);
    CHECK(!a.succeeded && !b.succeeded, "!a.succeeded && !b.succeeded");
    CHECK(!a.has_cap && !b.has_cap, "!a.has_cap && !b.has_cap");
  }
  outbox_clear(&out);

  /* Every part unresolvable still fails: the parts agree, but they agree
   * on naming nothing. */
  send_join_part(&c, 740, 40, 17, 2, 0);
  send_join_part(&c, 741, 40, 17, 2, 1);
  CHECK(out.n == 2, "out.n == 2");
  {
    join_reply_t a = read_join_reply(out.data[0], out.len[0]);
    join_reply_t b = read_join_reply(out.data[1], out.len[1]);
    CHECK(!a.succeeded && !b.succeeded, "!a.succeeded && !b.succeeded");
    CHECK(!a.has_cap && !b.has_cap, "!a.has_cap && !b.has_cap");
  }
  outbox_clear(&out);

  /* An incomplete set is never answered. */
  send_join_part(&c, 720, 0, 13, 3, 0);
  send_join_part(&c, 721, 0, 13, 3, 1);
  CHECK(out.n == 0, "out.n == 0");

  /* A partNum outside the set is rejected on its own. */
  send_join_part(&c, 730, 0, 15, 2, 7);
  CHECK(out.n == 1, "out.n == 1");
  {
    join_reply_t a = read_join_reply(out.data[0], out.len[0]);
    CHECK(a.answer_id == 730, "a.answer_id == 730");
    CHECK(!a.succeeded, "!a.succeeded");
  }
  outbox_clear(&out);

  if (g_failures != 0) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("ok test_rpc_join\n");
  return 0;
}
