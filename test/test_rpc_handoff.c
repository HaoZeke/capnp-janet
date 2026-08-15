/* A whole level 3 handoff, with three vats.
 *
 * Alice holds a capability that Bob hosts and wants Carol to have it.
 * The cases in test_rpc_join drive one side at a time with hand-built
 * frames; this one runs all three vats and lets them speak to each
 * other, which is the only way to see that the halves agree:
 *
 *   Alice -> Bob    Provide{target, recipient = RecipientId{carol, nonce}}
 *   Alice -> Carol  a payload carrying ThirdPartyCapId{bob, nonce}
 *   Carol -> Bob    Accept{ProvisionId{nonce}}
 *   Bob   -> Carol  Return carrying the capability
 *
 * The nonce is the only thing the three messages share, which is what
 * lets Bob hand the capability over without taking Carol's word for who
 * sent her.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_rpc.h>

#include "harness.h"

#define MESSAGE_DW 1
#define MESSAGE_PW 1
#define CALL_DW 3
#define CALL_PW 3
#define TARGET_DW 1
#define TARGET_PW 1
#define PAYLOAD_DW 0
#define PAYLOAD_PW 2
#define CAPDESC_DW 1
#define CAPDESC_PW 1

/* A pipe between two vats: whatever one writes, the other handles. */
typedef struct wire {
  capnp_rpc_conn_t *peer;
  uint8_t *data[64];
  size_t len[64];
  int n;
} wire_t;

static int queue_frame(void *ctx, const uint8_t *data, size_t len)
{
  wire_t *w = (wire_t *)ctx;
  if (w->n >= 64)
    return CAPNP_ERR_ALLOC;
  w->data[w->n] = (uint8_t *)malloc(len);
  if (w->data[w->n] == NULL)
    return CAPNP_ERR_ALLOC;
  memcpy(w->data[w->n], data, len);
  w->len[w->n] = len;
  w->n++;
  return CAPNP_OK;
}

/* Deliver everything one side has written to the other. */
static void flush_wire(wire_t *w)
{
  int i, n = w->n;
  uint8_t *frames[64];
  size_t lens[64];
  for (i = 0; i < n; i++) {
    frames[i] = w->data[i];
    lens[i] = w->len[i];
  }
  w->n = 0;
  for (i = 0; i < n; i++) {
    capnp_rpc_handle(w->peer, frames[i], lens[i]);
    free(frames[i]);
  }
}

/* Answers with its own mark, so a call shows which object it reached. */
typedef struct marked {
  int calls;
  uint32_t mark;
} marked_t;

static int marked_dispatch(void *server, uint64_t iface, uint16_t method,
                           const capnp_ptr_t *params,
                           const capnp_bptr_t *results)
{
  marked_t *m = (marked_t *)server;
  (void)iface;
  (void)method;
  (void)params;
  m->calls++;
  return capnp_builder_set_u32(results, 0, m->mark);
}

/* Alice -> Carol: a call whose params name the capability Bob hosts.
 * Alice is the introducer, so she writes the descriptor. */
static void tell_where_to_go(capnp_rpc_conn_t *writer, capnp_rpc_conn_t *reader,
                             uint32_t qid, const char *host, uint16_t port,
                             uint64_t nonce, uint32_t vine_id)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, call, target, payload, content, cd;
  uint8_t *flat = NULL;
  size_t len = 0;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK, "msg");
  CHECK(capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_CALL) == CAPNP_OK, "call tag");
  CHECK(capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) == CAPNP_OK, "msg slot");
  CHECK(capnp_builder_struct(&slot, CALL_DW, CALL_PW, &call) == CAPNP_OK, "call");
  CHECK(capnp_builder_set_u32(&call, 0, qid) == CAPNP_OK, "qid");
  CHECK(capnp_builder_slot(&call, CALL_DW, 0, &slot) == CAPNP_OK, "target slot");
  CHECK(capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target) == CAPNP_OK, "target");
  CHECK(capnp_builder_set_u16(&target, 4, 0) == CAPNP_OK, "importedCap");
  CHECK(capnp_builder_set_u32(&target, 0, 0) == CAPNP_OK, "export 0");
  CHECK(capnp_builder_slot(&call, CALL_DW, 1, &slot) == CAPNP_OK, "params slot");
  CHECK(capnp_builder_struct(&slot, PAYLOAD_DW, PAYLOAD_PW, &payload) == CAPNP_OK, "payload");
  CHECK(capnp_builder_slot(&payload, PAYLOAD_DW, 0, &slot) == CAPNP_OK, "content slot");
  CHECK(capnp_builder_struct(&slot, 1, 0, &content) == CAPNP_OK, "content");
  CHECK(capnp_builder_set_list_struct(&payload, PAYLOAD_DW, 1, 1, CAPDESC_DW,
                                      CAPDESC_PW, &cd) == CAPNP_OK, "capTable");
  CHECK(capnp_rpc_write_third_party_cap(writer, &cd, host, port, nonce, vine_id) == CAPNP_OK,
        "third-party descriptor");

  CHECK(capnp_builder_serialize(&b, &flat, &len) == CAPNP_OK, "serialize");
  capnp_rpc_handle(reader, flat, len);
  free(flat);
  capnp_builder_free(&b);
}

/* Whether the answer to `qid` arrived carrying a capability. */
static int answered_with_cap(capnp_rpc_conn_t *c, uint32_t qid)
{
  capnp_message_t held;
  capnp_ptr_t content;
  int ok;
  if (!capnp_rpc_is_answered(c, qid) || capnp_rpc_is_failed(c, qid))
    return 0;
  if (capnp_rpc_answer_content(c, qid, &held, &content) != CAPNP_OK)
    return 0;
  ok = content.kind == CAPNP_PK_CAP;
  capnp_message_free(&held);
  return ok;
}

int main(void)
{
  const uint64_t nonce = 0x5eedULL;
  int before, claimed_id;
  marked_t hosted = {0, 42};
  /* Bob answers Carol's connection with a different object, so the two
   * connections do not agree on export ids by accident: the capability
   * Alice hands over must arrive under an id of Carol's connection, not
   * the one Alice used. */
  marked_t sidecar = {0, 1000};
  marked_t carols = {0, 1};
  uint32_t boot, provide, claim, replay, q, side;
  capnp_rpc_vat_t bob_vat;
  capnp_rpc_conn_t alice_to_bob, bob_to_alice, carol_to_bob, bob_to_carol;
  capnp_rpc_conn_t alice_to_carol, carol_to_alice;
  capnp_rpc_introduction_t learned[4];
  wire_t a2b, b2a, c2b, b2c, a2c, c2a;

  memset(&bob_vat, 0, sizeof bob_vat);
  memset(&a2b, 0, sizeof a2b);
  memset(&b2a, 0, sizeof b2a);
  memset(&c2b, 0, sizeof c2b);
  memset(&b2c, 0, sizeof b2c);
  memset(&a2c, 0, sizeof a2c);
  memset(&c2a, 0, sizeof c2a);

  capnp_rpc_init(&alice_to_bob, queue_frame, &a2b);
  capnp_rpc_init(&bob_to_alice, queue_frame, &b2a);
  capnp_rpc_init(&carol_to_bob, queue_frame, &c2b);
  capnp_rpc_init(&bob_to_carol, queue_frame, &b2c);
  capnp_rpc_init(&alice_to_carol, queue_frame, &a2c);
  capnp_rpc_init(&carol_to_alice, queue_frame, &c2a);

  /* Bob is one vat with two connections, so the arrangement Alice makes
   * on hers is claimable on Carol's. */
  capnp_rpc_set_vat(&bob_to_alice, &bob_vat);
  capnp_rpc_set_vat(&bob_to_carol, &bob_vat);
  capnp_rpc_set_bootstrap(&bob_to_alice, &hosted, marked_dispatch);
  capnp_rpc_set_bootstrap(&bob_to_carol, &sidecar, marked_dispatch);
  capnp_rpc_set_bootstrap(&carol_to_alice, &carols, marked_dispatch);

  a2b.peer = &bob_to_alice;
  b2a.peer = &alice_to_bob;
  c2b.peer = &bob_to_carol;
  b2c.peer = &carol_to_bob;
  a2c.peer = &carol_to_alice;
  c2a.peer = &alice_to_carol;

  /* Alice bootstraps, so Bob exports the capability to her. */
  boot = capnp_rpc_send_bootstrap(&alice_to_bob);
  flush_wire(&a2b);
  flush_wire(&b2a);
  CHECK(capnp_rpc_is_answered(&alice_to_bob, boot), "bootstrap answered");

  /* 1. Alice tells Bob to expect Carol. */
  provide = capnp_rpc_send_provide(&alice_to_bob, 0, "10.0.0.2", 5001, nonce);
  CHECK(provide != (uint32_t)-1, "provide sent");
  flush_wire(&a2b);
  flush_wire(&b2a);
  CHECK(capnp_rpc_pending_provisions(&bob_to_alice, NULL, 0) == 1, "arrangement recorded");
  /* The same vat, seen through its other connection. */
  CHECK(capnp_rpc_pending_provisions(&bob_to_carol, NULL, 0) == 1,
        "the vat's other connection sees it");

  /* 2. Alice tells Carol where to go. Carol records the introduction
   *    rather than dialling: reaching Bob is the network's job, and here
   *    the connection already exists. */
  tell_where_to_go(&alice_to_carol, &carol_to_alice, 90, "10.0.0.1", 5000, nonce, 7);
  CHECK(capnp_rpc_pending_introductions(&carol_to_alice, learned, 4) == 1,
        "introduction recorded");
  CHECK(learned[0].nonce == nonce, "introduction nonce");
  CHECK(strcmp(learned[0].host, "10.0.0.1") == 0, "introduction host");
  CHECK(learned[0].port == 5000, "introduction port");

  /* Carol bootstraps Bob first, so her connection's export 0 is the
   * sidecar and the handed-over capability cannot land on 0 too. */
  side = capnp_rpc_send_bootstrap(&carol_to_bob);
  flush_wire(&c2b);
  flush_wire(&b2c);
  CHECK(capnp_rpc_is_answered(&carol_to_bob, side), "sidecar bootstrapped");

  /* 3. Carol presents the nonce to Bob, over her own connection. She was
   *    never told which export id Alice used, and it would mean nothing
   *    here: the arrangement is keyed by the nonce alone. */
  claim = capnp_rpc_send_accept(&carol_to_bob, nonce, 0);
  CHECK(claim != (uint32_t)-1, "accept sent");
  flush_wire(&c2b);
  flush_wire(&b2c);
  CHECK(answered_with_cap(&carol_to_bob, claim), "Carol got the capability");
  CHECK(capnp_rpc_pending_provisions(&bob_to_alice, NULL, 0) == 0, "arrangement consumed");

  /* Claimable exactly once, on any connection. */
  replay = capnp_rpc_send_accept(&carol_to_bob, nonce, 0);
  flush_wire(&c2b);
  flush_wire(&b2c);
  CHECK(capnp_rpc_is_failed(&carol_to_bob, replay), "a nonce cannot be claimed twice");

  /* 4. Carol drops the vine now that the pickup is done. */
  CHECK(capnp_rpc_introduction_done(&carol_to_alice, nonce) == 0, "pickup finished");
  CHECK(capnp_rpc_pending_introductions(&carol_to_alice, NULL, 0) == 0, "vine dropped");

  /* Carol now holds the capability Bob hosts. Calling it reaches the
   * object Alice was talking to, not whatever else sits at that id on
   * Carol's connection: this one answers 42, the sidecar 1000. */
  claimed_id = capnp_rpc_answer_cap_id(&carol_to_bob, claim);
  /* Not the id Alice used: her connection called it 0, and 0 here is the
   * sidecar. */
  CHECK(claimed_id > 0, "the claim named an id of Carol's connection");
  before = hosted.calls;
  q = capnp_rpc_send_call(&carol_to_bob, (uint32_t)claimed_id, 0x1234, 0, 1, 0,
                          NULL, NULL);
  flush_wire(&c2b);
  flush_wire(&b2c);
  CHECK(hosted.calls == before + 1, "the call reached the object Alice provided");
  CHECK(sidecar.calls == 0, "and not the sidecar");
  CHECK(capnp_rpc_is_answered(&carol_to_bob, q), "the call was answered");

  if (g_failures != 0) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("ok test_rpc_handoff\n");
  return 0;
}
