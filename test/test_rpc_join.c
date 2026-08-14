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
#define CAPDESC_DW 1
#define CAPDESC_PW 1
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


/* rpc.capnp shapes for the L1 messages. */
#define PROMISED_ANSWER_DW 1
#define PROMISED_ANSWER_PW 1
#define PA_OP_DW 1
#define PA_OP_PW 0
#define CALL_DW 3
#define CALL_PW 3
#define FINISH_DW 1
#define FINISH_PW 0
#define DIS_DW 1
#define DIS_PW 1

/* A call whose target is `promisedAnswer`: the answer to
 * `answer_qid`, optionally walked by getPointerField ops. */
static void send_pipelined_call(capnp_rpc_conn_t *c, uint32_t qid,
                                uint32_t answer_qid, const uint16_t *ops,
                                uint32_t nops)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, call, target, pa, op;
  uint8_t *flat = NULL;
  size_t len = 0;
  uint32_t i;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "msg");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_CALL) == CAPNP_OK, "call tag");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "msg slot");
  CHECK(capnp_builder_struct(&slot, CALL_DW, CALL_PW, &call) == CAPNP_OK, "call");
  CHECK(capnp_builder_set_u32(&call, 0, qid) == CAPNP_OK, "call qid");

  CHECK(capnp_builder_slot(&call, CALL_DW, 0, &slot) == CAPNP_OK, "target slot");
  CHECK(capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target) == CAPNP_OK, "target");
  CHECK(capnp_builder_set_u16(&target, 4, 1) == CAPNP_OK, "promisedAnswer tag");
  CHECK(capnp_builder_slot(&target, TARGET_DW, 0, &slot) == CAPNP_OK, "pa slot");
  CHECK(capnp_builder_struct(&slot, PROMISED_ANSWER_DW, PROMISED_ANSWER_PW, &pa) == CAPNP_OK, "pa");
  CHECK(capnp_builder_set_u32(&pa, 0, answer_qid) == CAPNP_OK, "pa qid");
  if (nops > 0) {
    CHECK(capnp_builder_set_list_struct(&pa, PROMISED_ANSWER_DW, 0, nops,
                                        PA_OP_DW, PA_OP_PW, &op) == CAPNP_OK, "ops");
    for (i = 0; i < nops; i++) {
      capnp_bptr_t e = op;
      e.word += (size_t)i * (PA_OP_DW + PA_OP_PW);
      CHECK(capnp_builder_set_u16(&e, 0, 1) == CAPNP_OK, "op tag");
      CHECK(capnp_builder_set_u16(&e, 2, ops[i]) == CAPNP_OK, "op field");
    }
  }

  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
  capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
}

/* Alice -> us: a Call whose params name a capability hosted by a third
 * vat, with a vine we can use in the meantime. This is the receiving
 * half of the introduction; the Provide/Accept cases are the hosting
 * half. */
static void send_call_with_third_party_cap(capnp_rpc_conn_t *c, uint32_t qid,
                                           uint32_t target_export,
                                           const char *host, uint16_t port,
                                           uint64_t nonce, uint32_t vine_id)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, call, target, payload, cd, content;
  uint8_t *flat = NULL;
  size_t len = 0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "msg");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_CALL) == CAPNP_OK, "call tag");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "msg slot");
  CHECK(capnp_builder_struct(&slot, CALL_DW, CALL_PW, &call) == CAPNP_OK, "call");
  CHECK(capnp_builder_set_u32(&call, 0, qid) == CAPNP_OK, "call qid");

  CHECK(capnp_builder_slot(&call, CALL_DW, 0, &slot) == CAPNP_OK, "target slot");
  CHECK(capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target) == CAPNP_OK, "target");
  CHECK(capnp_builder_set_u16(&target, 4, 0) == CAPNP_OK, "importedCap tag");
  CHECK(capnp_builder_set_u32(&target, 0, target_export) == CAPNP_OK, "export");

  CHECK(capnp_builder_slot(&call, CALL_DW, 1, &slot) == CAPNP_OK, "params slot");
  CHECK(capnp_builder_struct(&slot, PAYLOAD_DW, PAYLOAD_PW, &payload) == CAPNP_OK, "payload");
  CHECK(capnp_builder_slot(&payload, PAYLOAD_DW, 0, &slot) == CAPNP_OK, "content slot");
  CHECK(capnp_builder_struct(&slot, 1, 0, &content) == CAPNP_OK, "content");
  CHECK(capnp_builder_set_list_struct(&payload, PAYLOAD_DW, 1, 1, CAPDESC_DW,
                                      CAPDESC_PW, &cd) == CAPNP_OK, "capTable");
  CHECK(capnp_rpc_write_third_party_cap(c, &cd, host, port, nonce, vine_id) == CAPNP_OK,
        "third-party descriptor");

  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
  capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
}

/* The introducer can also hand us the descriptor in an answer, not only
 * in a call's params. */
static void send_return_with_third_party_cap(capnp_rpc_conn_t *c, uint32_t qid,
                                             const char *host, uint16_t port,
                                             uint64_t nonce, uint32_t vine_id)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, ret, payload, content, cd;
  uint8_t *flat = NULL;
  size_t len = 0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "msg");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RETURN) == CAPNP_OK, "return tag");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "msg slot");
  CHECK(capnp_builder_struct(&slot, RETURN_DW, RETURN_PW, &ret) == CAPNP_OK, "return");
  CHECK(capnp_builder_set_u32(&ret, 0, qid) == CAPNP_OK, "answerId");
  CHECK(capnp_builder_set_u16(&ret, 6, 0) == CAPNP_OK, "results tag");
  CHECK(capnp_builder_slot(&ret, RETURN_DW, 0, &slot) == CAPNP_OK, "results slot");
  CHECK(capnp_builder_struct(&slot, PAYLOAD_DW, PAYLOAD_PW, &payload) == CAPNP_OK, "payload");
  CHECK(capnp_builder_slot(&payload, PAYLOAD_DW, 0, &slot) == CAPNP_OK, "content slot");
  CHECK(capnp_builder_struct(&slot, 1, 0, &content) == CAPNP_OK, "content");
  CHECK(capnp_builder_set_list_struct(&payload, PAYLOAD_DW, 1, 1, CAPDESC_DW,
                                      CAPDESC_PW, &cd) == CAPNP_OK, "capTable");
  CHECK(capnp_rpc_write_third_party_cap(c, &cd, host, port, nonce, vine_id) == CAPNP_OK,
        "third-party descriptor");

  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
  capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
}

/* Whether a frame is a Release naming `id`. */
static int is_release_of(const uint8_t *data, size_t len, uint32_t id)
{
  capnp_message_t m;
  capnp_ptr_t root, rel;
  int match = 0;
  if (capnp_message_from_flat(&m, data, len) != CAPNP_OK)
    return 0;
  /* The root is the Message itself, as in read_join_reply above. */
  if (capnp_root(&m, &root) == CAPNP_OK &&
      capnp_get_u16(&root, 0, 0) == CAPNP_RPC_MSG_RELEASE &&
      capnp_getp(&root, 0, &rel) == CAPNP_OK) {
    match = capnp_get_u32(&rel, 0, 0) == id && capnp_get_u32(&rel, 4, 0) == 1;
  }
  capnp_message_free(&m);
  return match;
}

static void send_finish(capnp_rpc_conn_t *c, uint32_t qid)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, fin;
  uint8_t *flat = NULL;
  size_t len = 0;
  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "msg");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_FINISH) == CAPNP_OK, "finish tag");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "slot");
  CHECK(capnp_builder_struct(&slot, FINISH_DW, FINISH_PW, &fin) == CAPNP_OK, "finish");
  CHECK(capnp_builder_set_u32(&fin, 0, qid) == CAPNP_OK, "finish qid");
  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
  CHECK(capnp_rpc_handle(c, flat, len) == CAPNP_OK, "handle finish");
  free(flat);
  capnp_builder_free(&b);
}

static void send_disembargo(capnp_rpc_conn_t *c, uint16_t which, uint32_t id)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, dis, target;
  uint8_t *flat = NULL;
  size_t len = 0;
  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "msg");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_DISEMBARGO) == CAPNP_OK, "dis tag");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "slot");
  CHECK(capnp_builder_struct(&slot, DIS_DW, DIS_PW, &dis) == CAPNP_OK, "dis");
  CHECK(capnp_builder_slot(&dis, DIS_DW, 0, &slot) == CAPNP_OK, "dis target slot");
  CHECK(capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target) == CAPNP_OK, "dis target");
  CHECK(capnp_builder_set_u16(&target, 4, 0) == CAPNP_OK, "importedCap");
  CHECK(capnp_builder_set_u32(&target, 0, 0) == CAPNP_OK, "export 0");
  /* `context` is a group: it shares Disembargo's data section. */
  CHECK(capnp_builder_set_u16(&dis, 4, which) == CAPNP_OK, "ctx tag");
  CHECK(capnp_builder_set_u32(&dis, 0, id) == CAPNP_OK, "ctx id");
  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
  CHECK(capnp_rpc_handle(c, flat, len) == CAPNP_OK, "handle disembargo");
  free(flat);
  capnp_builder_free(&b);
}

/* Return union tag and, for results, the first u32 of the content. */
typedef struct {
  uint32_t answer_id;
  uint16_t which;
  uint32_t value;
} return_info_t;

static return_info_t read_return(const uint8_t *data, size_t len)
{
  capnp_message_t m;
  capnp_ptr_t root, ret, payload, content;
  return_info_t out;
  memset(&out, 0, sizeof out);
  CHECK(capnp_message_from_flat(&m, data, len) == CAPNP_OK, "from_flat");
  CHECK(capnp_root(&m, &root) == CAPNP_OK, "root");
  CHECK(capnp_get_u16(&root, 0, 0) == CAPNP_RPC_MSG_RETURN, "is return");
  CHECK(capnp_getp(&root, 0, &ret) == CAPNP_OK, "ret");
  out.answer_id = capnp_get_u32(&ret, 0, 0);
  out.which = capnp_get_u16(&ret, 6, 0);
  if (out.which == 0 && capnp_getp(&ret, 0, &payload) == CAPNP_OK &&
      capnp_getp(&payload, 0, &content) == CAPNP_OK &&
      content.kind == CAPNP_PK_STRUCT)
    out.value = capnp_get_u32(&content, 0, 0);
  capnp_message_free(&m);
  return out;
}


/* rpc.capnp / rpc-threeparty.capnp shapes for the level 3 messages. */
#define PROVIDE_DW 1
#define PROVIDE_PW 2
#define ACCEPT_DW 1
#define ACCEPT_PW 1
#define RECIPIENT_DW 1
#define RECIPIENT_PW 1
#define PROVISION_DW 1
#define PROVISION_PW 0
#define VATID_DW 1
#define VATID_PW 1

/* Alice -> us: hold export `export_id` for whoever presents `nonce`. */
static void send_provide(capnp_rpc_conn_t *c, uint32_t qid, uint32_t export_id,
                         uint64_t nonce)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, provide, target, recipient, vat;
  uint8_t *flat = NULL;
  size_t len = 0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "msg");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_PROVIDE) == CAPNP_OK, "tag");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "slot");
  CHECK(capnp_builder_struct(&slot, PROVIDE_DW, PROVIDE_PW, &provide) == CAPNP_OK, "provide");
  CHECK(capnp_builder_set_u32(&provide, 0, qid) == CAPNP_OK, "qid");

  CHECK(capnp_builder_slot(&provide, PROVIDE_DW, 0, &slot) == CAPNP_OK, "tslot");
  CHECK(capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target) == CAPNP_OK, "target");
  CHECK(capnp_builder_set_u16(&target, 4, 0) == CAPNP_OK, "importedCap");
  CHECK(capnp_builder_set_u32(&target, 0, export_id) == CAPNP_OK, "export");

  CHECK(capnp_builder_slot(&provide, PROVIDE_DW, 1, &slot) == CAPNP_OK, "rslot");
  CHECK(capnp_builder_struct(&slot, RECIPIENT_DW, RECIPIENT_PW, &recipient) == CAPNP_OK, "recipient");
  CHECK(capnp_builder_set_u64(&recipient, 0, nonce) == CAPNP_OK, "nonce");
  CHECK(capnp_builder_slot(&recipient, RECIPIENT_DW, 0, &slot) == CAPNP_OK, "vslot");
  CHECK(capnp_builder_struct(&slot, VATID_DW, VATID_PW, &vat) == CAPNP_OK, "vat");
  CHECK(capnp_builder_set_text(&vat, VATID_DW, 0, "127.0.0.1", 9) == CAPNP_OK, "host");
  CHECK(capnp_builder_set_u16(&vat, 0, 4000) == CAPNP_OK, "port");

  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
  capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
}

/* Read a checked-in golden frame. Missing means a broken tree. */
static uint8_t *load_frame(const char *name, size_t *len)
{
  const char *src = getenv("CAPNP_JANET_SOURCE_ROOT");
  char path[1024];
  FILE *f;
  long sz;
  uint8_t *buf;
  if (!src || !src[0])
    src = ".";
  snprintf(path, sizeof(path), "%s/test/fixtures/%s", src, name);
  f = fopen(path, "rb");
  CHECK(f != NULL, "golden frame opens");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    CHECK(0, "golden frame seeks");
    return NULL;
  }
  buf = malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf);
    fclose(f);
    CHECK(0, "golden frame reads");
    return NULL;
  }
  fclose(f);
  *len = (size_t)sz;
  return buf;
}

/* Carol -> us: claim the capability held under `nonce`. */
static void send_accept(capnp_rpc_conn_t *c, uint32_t qid, uint64_t nonce)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, accept, provision;
  uint8_t *flat = NULL;
  size_t len = 0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "msg");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_ACCEPT) == CAPNP_OK, "tag");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "slot");
  CHECK(capnp_builder_struct(&slot, ACCEPT_DW, ACCEPT_PW, &accept) == CAPNP_OK, "accept");
  CHECK(capnp_builder_set_u32(&accept, 0, qid) == CAPNP_OK, "qid");
  CHECK(capnp_builder_slot(&accept, ACCEPT_DW, 0, &slot) == CAPNP_OK, "pslot");
  CHECK(capnp_builder_struct(&slot, PROVISION_DW, PROVISION_PW, &provision) == CAPNP_OK, "provision");
  CHECK(capnp_builder_set_u64(&provision, 0, nonce) == CAPNP_OK, "nonce");
  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
  capnp_rpc_handle(c, flat, len);
  free(flat);
  capnp_builder_free(&b);
}

/* Return tag, and whether the results content is a capability. */
static void read_l3(const uint8_t *data, size_t len, uint32_t *ansid,
                    int *is_exception, int *has_cap)
{
  capnp_message_t m;
  capnp_ptr_t root, ret, payload, content;
  *ansid = 0;
  *is_exception = 1;
  *has_cap = 0;
  CHECK(capnp_message_from_flat(&m, data, len) == CAPNP_OK, "from_flat");
  CHECK(capnp_root(&m, &root) == CAPNP_OK, "root");
  CHECK(capnp_get_u16(&root, 0, 0) == CAPNP_RPC_MSG_RETURN, "is return");
  CHECK(capnp_getp(&root, 0, &ret) == CAPNP_OK, "ret");
  *ansid = capnp_get_u32(&ret, 0, 0);
  *is_exception = capnp_get_u16(&ret, 6, 0) != 0;
  if (!*is_exception && capnp_getp(&ret, 0, &payload) == CAPNP_OK &&
      capnp_getp(&payload, 0, &content) == CAPNP_OK)
    *has_cap = content.kind == CAPNP_PK_CAP;
  capnp_message_free(&m);
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


  /* Promise pipelining: a call addressed to an answer reaches the
   * capability inside it. */
  send_pipelined_call(&c, 2, 1, NULL, 0);
  CHECK(out.n == 1, "pipelined call answered");
  {
    return_info_t r = read_return(out.data[0], out.len[0]);
    CHECK(r.answer_id == 2, "pipelined answerId");
    CHECK(r.which == 0, "pipelined returned results");
    CHECK(r.value == 1, "pipelined call reached the server");
  }
  outbox_clear(&out);

  /* A transform op that walks past the capability does not resolve. */
  {
    uint16_t ops[1];
    ops[0] = 0;
    send_pipelined_call(&c, 3, 1, ops, 1);
    CHECK(out.n == 1, "walked-past call answered");
    {
      return_info_t r = read_return(out.data[0], out.len[0]);
      CHECK(r.which != 0, "walking past the cap does not resolve");
    }
    outbox_clear(&out);
  }

  /* Finish drops the answer, so later pipelining fails. */
  send_finish(&c, 1);
  send_pipelined_call(&c, 4, 1, NULL, 0);
  CHECK(out.n == 1, "post-finish call answered");
  {
    return_info_t r = read_return(out.data[0], out.len[0]);
    CHECK(r.which != 0, "the answer it named is gone");
  }
  outbox_clear(&out);

  /* Disembargo: senderLoopback is echoed as receiverLoopback. */
  send_disembargo(&c, 0, 12345);
  CHECK(out.n == 1, "disembargo echoed");
  {
    capnp_message_t m;
    capnp_ptr_t root, dis;
    CHECK(capnp_message_from_flat(&m, out.data[0], out.len[0]) == CAPNP_OK, "reply");
    CHECK(capnp_root(&m, &root) == CAPNP_OK, "reply root");
    CHECK(capnp_get_u16(&root, 0, 0) == CAPNP_RPC_MSG_DISEMBARGO, "is disembargo");
    CHECK(capnp_getp(&root, 0, &dis) == CAPNP_OK, "dis body");
    CHECK(capnp_get_u16(&dis, 4, 0) == 1, "receiverLoopback");
    CHECK(capnp_get_u32(&dis, 0, 0) == 12345, "same id echoed");
    capnp_message_free(&m);
  }
  outbox_clear(&out);

  /* receiverLoopback is absorbed: echoing it would bounce forever. */
  send_disembargo(&c, 1, 999);
  CHECK(out.n == 0, "receiverLoopback absorbed");


  /* Level 3: a provided capability is claimable exactly once. */
  {
    const uint64_t nonce = 0xfeedfaceULL;
    uint32_t ansid;
    int exc, cap;
    send_provide(&c, 10, 0, nonce);
    CHECK(out.n == 1, "provide answered");
    read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
    CHECK(!exc, "provide accepted");
    CHECK(capnp_rpc_pending_provisions(&c, NULL, 0) == 1, "one provision held");
    outbox_clear(&out);

    send_accept(&c, 11, nonce);
    CHECK(out.n == 1, "accept answered");
    read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
    CHECK(ansid == 11, "accept answerId");
    CHECK(!exc, "accept succeeded");
    /* The capability comes back as a capability, not a struct. */
    CHECK(cap, "accept returned the capability");
    CHECK(capnp_rpc_pending_provisions(&c, NULL, 0) == 0, "provision consumed");
    outbox_clear(&out);

    /* A nonce is single-use: leaving it claimable would let anyone who
     * learned it take the capability again. */
    send_accept(&c, 12, nonce);
    CHECK(out.n == 1, "replay answered");
    read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
    CHECK(exc, "a nonce cannot be claimed twice");
    outbox_clear(&out);
  }

  /* An Accept with an unknown nonce is refused, even while a different
   * arrangement is standing: matching is on the nonce, not on there
   * being something to hand over. */
  {
    uint32_t ansid;
    int exc, cap;
    send_provide(&c, 19, 0, 0xc0ffeeULL);
    outbox_clear(&out);
    send_accept(&c, 20, 0xdeadbeefULL);
    CHECK(out.n == 1, "unknown nonce answered");
    read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
    CHECK(exc, "unknown nonce refused");
    CHECK(capnp_rpc_pending_provisions(&c, NULL, 0) == 1,
          "the standing arrangement is untouched");
    outbox_clear(&out);
    send_accept(&c, 21, 0xc0ffeeULL);
    read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
    CHECK(!exc, "the arranged nonce is claimable");
    outbox_clear(&out);
  }

  /* Providing a capability we do not host is refused. */
  {
    uint32_t ansid;
    int exc, cap;
    send_provide(&c, 30, 40, 0x1234ULL);
    CHECK(out.n == 1, "bad provide answered");
    read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
    CHECK(exc, "cannot provide what we do not host");
    CHECK(capnp_rpc_pending_provisions(&c, NULL, 0) == 0, "nothing recorded");
    outbox_clear(&out);
  }

  /* Two provisions of the same capability are independent. */
  {
    uint32_t ansid;
    int exc, cap;
    send_provide(&c, 40, 0, 0xaaaULL);
    send_provide(&c, 41, 0, 0xbbbULL);
    CHECK(capnp_rpc_pending_provisions(&c, NULL, 0) == 2, "two provisions");
    outbox_clear(&out);
    send_accept(&c, 42, 0xaaaULL);
    CHECK(out.n == 1, "first claim answered");
    read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
    CHECK(!exc, "first claim succeeded");
    /* Claiming one leaves the other standing. */
    CHECK(capnp_rpc_pending_provisions(&c, NULL, 0) == 1, "the other stands");
    outbox_clear(&out);
  }

  /* Level 3, receiving half: a payload that names a third party's
   * capability is recorded as an introduction, and the vine survives
   * until the pickup is finished. */
  {
    capnp_rpc_introduction_t got[4];
    int i, released;

    CHECK(capnp_rpc_pending_introductions(&c, NULL, 0) == 0, "no introductions yet");
    send_call_with_third_party_cap(&c, 50, 0, "10.0.0.7", 5000, 0xabcdefULL, 77);
    CHECK(capnp_rpc_pending_introductions(&c, got, 4) == 1, "one introduction held");
    CHECK(got[0].nonce == 0xabcdefULL, "introduction nonce");
    CHECK(got[0].vine_id == 77, "introduction vine");
    CHECK(got[0].port == 5000, "introduction port");
    CHECK(strcmp(got[0].host, "10.0.0.7") == 0, "introduction host");

    /* Nothing is released while the pickup is outstanding: the vine is
     * the only way to reach the capability until then. */
    for (i = 0; i < out.n; i++)
      CHECK(!is_release_of(out.data[i], out.len[i], 77), "vine not released early");
    outbox_clear(&out);

    CHECK(capnp_rpc_introduction_done(&c, 0xabcdefULL) == 0, "pickup finished");
    released = 0;
    for (i = 0; i < out.n; i++)
      released = released || is_release_of(out.data[i], out.len[i], 77);
    CHECK(released, "vine released on pickup");
    CHECK(capnp_rpc_pending_introductions(&c, NULL, 0) == 0, "introduction cleared");
    outbox_clear(&out);

    /* Finishing an introduction nobody handed us is refused, even while
     * another is outstanding: the nonce picks the arrangement, not the
     * fact that there is one. */
    send_call_with_third_party_cap(&c, 51, 0, "10.0.0.8", 5001, 0x99ULL, 78);
    outbox_clear(&out);
    CHECK(capnp_rpc_introduction_done(&c, 0xabcdefULL) == -1, "unknown nonce refused");
    CHECK(capnp_rpc_pending_introductions(&c, NULL, 0) == 1, "the other stands");
    CHECK(capnp_rpc_introduction_done(&c, 0x99ULL) == 0, "the arranged nonce finishes");
    outbox_clear(&out);
  }

  /* The same descriptor in an answer is recorded too. */
  {
    capnp_rpc_introduction_t got[4];
    uint32_t qid = capnp_rpc_send_bootstrap(&c);
    outbox_clear(&out);
    send_return_with_third_party_cap(&c, qid, "10.0.0.9", 5002, 0x77ULL, 80);
    CHECK(capnp_rpc_pending_introductions(&c, got, 4) == 1, "answer introduction held");
    CHECK(got[0].nonce == 0x77ULL, "answer introduction nonce");
    CHECK(got[0].vine_id == 80, "answer introduction vine");
    CHECK(strcmp(got[0].host, "10.0.0.9") == 0, "answer introduction host");
    CHECK(capnp_rpc_introduction_done(&c, 0x77ULL) == 0, "answer pickup finished");
    outbox_clear(&out);
  }

  /* A host too long to store is refused rather than truncated: a
   * truncated address names a different vat. */
  {
    char host[CAPNP_RPC_MAX_HOST + 8];
    memset(host, 'h', sizeof host - 1);
    host[sizeof host - 1] = '\0';
    send_call_with_third_party_cap(&c, 52, 0, host, 5000, 0xfeedULL, 79);
    CHECK(capnp_rpc_pending_introductions(&c, NULL, 0) == 0, "overlong host refused");
    outbox_clear(&out);
  }

  /* Level 3 driven by frames the reference `capnp` CLI encoded.
   *
   * Everything above builds its own Provide and Accept, so it shows the
   * vat agrees with this library's builder and nothing more: a layout
   * both sides share but the wire format does not would pass all of it.
   * These bytes come from the reference implementation
   * (scripts/gen-rpc-frames.sh): hold export 0 for whoever presents
   * 0xfeedface (question 42), then claim it (question 43). */
  {
    uint8_t *frame;
    size_t len = 0;
    uint32_t ansid;
    int exc, cap;
    int before = capnp_rpc_pending_provisions(&c, NULL, 0);

    frame = load_frame("rpc-provide.bin", &len);
    if (frame) {
      CHECK(capnp_rpc_handle(&c, frame, len) == CAPNP_OK, "reference Provide handled");
      free(frame);
      CHECK(out.n == 1, "reference Provide answered");
      read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
      CHECK(ansid == 42, "reference Provide answerId");
      CHECK(!exc, "reference Provide accepted");
      /* The nonce the vat recorded is the one the CLI wrote. */
      CHECK(capnp_rpc_pending_provisions(&c, NULL, 0) == before + 1,
            "reference Provide recorded");
      outbox_clear(&out);
    }

    frame = load_frame("rpc-accept.bin", &len);
    if (frame) {
      CHECK(capnp_rpc_handle(&c, frame, len) == CAPNP_OK, "reference Accept handled");
      free(frame);
      CHECK(out.n == 1, "reference Accept answered");
      read_l3(out.data[0], out.len[0], &ansid, &exc, &cap);
      CHECK(ansid == 43, "reference Accept answerId");
      CHECK(!exc, "reference Accept succeeded");
      CHECK(cap, "reference Accept got the capability");
      CHECK(capnp_rpc_pending_provisions(&c, NULL, 0) == before,
            "reference Accept consumed the arrangement");
      outbox_clear(&out);
    }
  }

  if (g_failures != 0) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("ok test_rpc_join\n");
  return 0;
}
