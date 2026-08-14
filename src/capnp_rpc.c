/* Two-party Cap'n Proto RPC vat. See capnp-janet/capnp_rpc.h.
 *
 * Field offsets below are from rpc.capnp and rpc-twoparty.capnp; each is
 * named beside its schema field so the two can be checked against each
 * other by eye.
 */

#include <capnp-janet/capnp_rpc.h>

#include <stdlib.h>
#include <string.h>

/* rpc.capnp struct shapes: data words, pointer words. */
#define MESSAGE_DW 1
#define MESSAGE_PW 1
#define RETURN_DW 2
#define RETURN_PW 1
#define PAYLOAD_DW 0
#define PAYLOAD_PW 2
#define CAPDESC_DW 1
#define CAPDESC_PW 1
#define JOINRESULT_DW 1
#define JOINRESULT_PW 1

/* Message.union discriminant, byte 0. */
#define MESSAGE_TAG_OFF 0
/* Return: answerId @0 (u32), union tag at byte 6. */
#define RETURN_ANSWERID_OFF 0
#define RETURN_TAG_OFF 6
#define RETURN_TAG_RESULTS 0
/* MessageTarget: importedCap @0 (u32), union tag at byte 4. */
#define TARGET_IMPORTEDCAP_OFF 0
#define TARGET_TAG_OFF 4
#define TARGET_TAG_IMPORTEDCAP 0
/* CapDescriptor: union tag at byte 0, senderHosted @4 (u32). */
#define CAPDESC_TAG_OFF 0
#define CAPDESC_SENDERHOSTED_OFF 4
#define CAPDESC_TAG_SENDERHOSTED 1
/* Bootstrap / Join / Release: questionId or id @0 (u32). */
#define QUESTIONID_OFF 0
#define RELEASE_REFCOUNT_OFF 4
/* JoinKeyPart: joinId @0 (u32), partCount @4 (u16), partNum @6 (u16). */
#define JKP_JOINID_OFF 0
#define JKP_PARTCOUNT_OFF 4
#define JKP_PARTNUM_OFF 6
/* JoinResult: joinId @0 (u32), succeeded @32 (bit). */
#define JR_JOINID_OFF 0
#define JR_SUCCEEDED_BIT 32

void capnp_rpc_init(capnp_rpc_conn_t *c, capnp_rpc_send_fn send, void *send_ctx)
{
  memset(c, 0, sizeof(*c));
  c->send = send;
  c->send_ctx = send_ctx;
}

void capnp_rpc_set_bootstrap(capnp_rpc_conn_t *c, void *server,
                             capnp_rpc_dispatch_fn dispatch)
{
  c->bootstrap = server;
  c->bootstrap_dispatch = dispatch;
}

int capnp_rpc_export(capnp_rpc_conn_t *c, void *server,
                     capnp_rpc_dispatch_fn dispatch)
{
  int i;
  /* Reuse the export already naming this server: the peer must see one id
   * per object, or reference equality could not be answered. */
  for (i = 0; i < CAPNP_RPC_MAX_EXPORTS; i++) {
    if (c->exports[i].used && c->exports[i].server == server) {
      c->exports[i].refcount++;
      return i;
    }
  }
  for (i = 0; i < CAPNP_RPC_MAX_EXPORTS; i++) {
    if (!c->exports[i].used) {
      c->exports[i].used = 1;
      c->exports[i].refcount = 1;
      c->exports[i].server = server;
      c->exports[i].dispatch = dispatch;
      return i;
    }
  }
  return -1;
}

/* Resolve a MessageTarget to a local export index, or -1 when it names
 * nothing this vat hosts. Shared by Call and Join, which address
 * capabilities the same way. */
static int resolve_target(capnp_rpc_conn_t *c, const capnp_ptr_t *target)
{
  uint32_t id;
  if (target->kind != CAPNP_PK_STRUCT)
    return -1;
  if (capnp_get_u16(target, TARGET_TAG_OFF, 0) != TARGET_TAG_IMPORTEDCAP)
    return -1;
  id = capnp_get_u32(target, TARGET_IMPORTEDCAP_OFF, 0);
  if (id >= CAPNP_RPC_MAX_EXPORTS)
    return -1;
  return c->exports[id].used ? (int)id : -1;
}

/* Start a Return message: root Message, its Return, and its Payload. */
static int begin_return(capnp_builder_t *b, uint32_t answer_id,
                        capnp_bptr_t *ret_out, capnp_bptr_t *payload_out)
{
  capnp_bptr_t root, msg, ret, slot, payload;
  if (capnp_builder_root(b, &root))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u16(&msg, MESSAGE_TAG_OFF, CAPNP_RPC_MSG_RETURN))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_struct(&slot, RETURN_DW, RETURN_PW, &ret))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u32(&ret, RETURN_ANSWERID_OFF, answer_id))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u16(&ret, RETURN_TAG_OFF, RETURN_TAG_RESULTS))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_slot(&ret, RETURN_DW, 0, &slot))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_struct(&slot, PAYLOAD_DW, PAYLOAD_PW, &payload))
    return CAPNP_ERR_ALLOC;
  *ret_out = ret;
  *payload_out = payload;
  return CAPNP_OK;
}

/* One-entry capTable naming a senderHosted export, with the payload's
 * content pointer aimed at it. */
static int write_cap_table(const capnp_bptr_t *payload, int eid)
{
  capnp_bptr_t cd;
  if (capnp_builder_set_list_struct(payload, PAYLOAD_DW, 1, 1, CAPDESC_DW,
                                    CAPDESC_PW, &cd))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u16(&cd, CAPDESC_TAG_OFF, CAPDESC_TAG_SENDERHOSTED))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u32(&cd, CAPDESC_SENDERHOSTED_OFF, (uint32_t)eid))
    return CAPNP_ERR_ALLOC;
  return CAPNP_OK;
}

static int flush(capnp_rpc_conn_t *c, capnp_builder_t *b)
{
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc;
  if (capnp_builder_serialize(b, &flat, &len))
    return CAPNP_ERR_ALLOC;
  rc = c->send(c->send_ctx, flat, len);
  free(flat);
  return rc;
}

static int handle_bootstrap(capnp_rpc_conn_t *c, const capnp_ptr_t *boot)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload;
  uint32_t qid = capnp_get_u32(boot, QUESTIONID_OFF, 0);
  int eid, rc;

  if (c->bootstrap == NULL)
    return CAPNP_ERR_ARG;
  eid = capnp_rpc_export(c, c->bootstrap, c->bootstrap_dispatch);
  if (eid < 0)
    return CAPNP_ERR_ALLOC;

  capnp_builder_init(&b);
  rc = begin_return(&b, qid, &ret, &payload);
  if (rc == CAPNP_OK)
    rc = write_cap_table(&payload, eid);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_cap(&payload, PAYLOAD_DW, 0, 0);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

static int handle_call(capnp_rpc_conn_t *c, const capnp_ptr_t *call)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload, results;
  capnp_ptr_t target, params_payload, params;
  uint32_t qid = capnp_get_u32(call, QUESTIONID_OFF, 0);
  int eid, rc;

  if (capnp_getp(call, 0, &target) != CAPNP_OK)
    return CAPNP_ERR_KIND;
  eid = resolve_target(c, &target);
  if (eid < 0)
    return CAPNP_ERR_ARG;

  memset(&params, 0, sizeof params);
  if (capnp_getp(call, 1, &params_payload) == CAPNP_OK)
    (void)capnp_getp(&params_payload, 0, &params);

  capnp_builder_init(&b);
  rc = begin_return(&b, qid, &ret, &payload);
  if (rc == CAPNP_OK) {
    capnp_bptr_t slot;
    /* One data word and one pointer word covers the replies the bundled
     * servers make; a richer server allocates inside dispatch. */
    if (capnp_builder_slot(&payload, PAYLOAD_DW, 0, &slot) ||
        capnp_builder_struct(&slot, 1, 1, &results))
      rc = CAPNP_ERR_ALLOC;
    else
      rc = c->exports[eid].dispatch(c->exports[eid].server,
                                    capnp_get_u64(call, 8, 0),
                                    capnp_get_u16(call, 4, 0), &params,
                                    &results);
  }
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

static void handle_release(capnp_rpc_conn_t *c, const capnp_ptr_t *rel)
{
  uint32_t id = capnp_get_u32(rel, QUESTIONID_OFF, 0);
  uint32_t count = capnp_get_u32(rel, RELEASE_REFCOUNT_OFF, 0);
  if (id >= CAPNP_RPC_MAX_EXPORTS || !c->exports[id].used)
    return;
  c->exports[id].refcount -= (int)count;
  if (c->exports[id].refcount <= 0)
    memset(&c->exports[id], 0, sizeof c->exports[id]);
}

/* Answer one Join question with a JoinResult payload. */
static int send_join_result(capnp_rpc_conn_t *c, uint32_t qid, uint32_t join_id,
                            int succeeded, int with_cap, int eid)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload, slot, jr;
  int rc;

  capnp_builder_init(&b);
  rc = begin_return(&b, qid, &ret, &payload);
  if (rc == CAPNP_OK) {
    if (capnp_builder_slot(&payload, PAYLOAD_DW, 0, &slot) ||
        capnp_builder_struct(&slot, JOINRESULT_DW, JOINRESULT_PW, &jr))
      rc = CAPNP_ERR_ALLOC;
  }
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&jr, JR_JOINID_OFF, join_id);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_bool(&jr, JR_SUCCEEDED_BIT, succeeded);
  if (rc == CAPNP_OK && with_cap && eid >= 0) {
    /* The receiver gains a reference, so the refcount rises with it. */
    c->exports[eid].refcount++;
    rc = write_cap_table(&payload, eid);
    if (rc == CAPNP_OK)
      rc = capnp_builder_set_cap(&jr, JOINRESULT_DW, 0, 0);
  }
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

/* Find the slot holding joinId, or claim a free one. -1 when the table is
 * full or partCount disagrees with the parts already in. */
static int join_slot_find(capnp_rpc_conn_t *c, uint32_t jid, int pcount)
{
  int i;
  for (i = 0; i < CAPNP_RPC_MAX_JOINS; i++) {
    if (c->joins[i].used && c->joins[i].join_id == jid)
      return c->joins[i].part_count == pcount ? i : -1;
  }
  for (i = 0; i < CAPNP_RPC_MAX_JOINS; i++) {
    if (!c->joins[i].used) {
      memset(&c->joins[i], 0, sizeof c->joins[i]);
      c->joins[i].used = 1;
      c->joins[i].join_id = jid;
      c->joins[i].part_count = pcount;
      return i;
    }
  }
  return -1;
}

/* The set is complete: compare the targets and answer every part. */
static int join_complete(capnp_rpc_conn_t *c, int slot)
{
  capnp_rpc_join_t *j = &c->joins[slot];
  int i, first = j->eids[0], same, rc = 0;

  /* An unresolved part names nothing we host, so the set cannot be proven
   * equal -- parts that all name nothing agree, but not about an object. */
  same = first >= 0;
  for (i = 1; same && i < j->part_count; i++)
    if (j->eids[i] != first)
      same = 0;

  /* Exactly one result carries the joined capability, per JoinResult. */
  for (i = 0; i < j->part_count; i++) {
    int r = send_join_result(c, j->qids[i], j->join_id, same,
                             same && i == 0, first);
    if (r != 0)
      rc = r;
  }
  memset(j, 0, sizeof *j);
  return rc;
}

static int handle_join(capnp_rpc_conn_t *c, const capnp_ptr_t *join)
{
  capnp_ptr_t target, key;
  uint32_t qid = capnp_get_u32(join, QUESTIONID_OFF, 0);
  uint32_t jid;
  int pcount, pnum, slot, eid;

  if (capnp_getp(join, 1, &key) != CAPNP_OK || key.kind != CAPNP_PK_STRUCT)
    /* Without a JoinKeyPart there is no way to tell which set this
     * belongs to, so it can only fail on its own. */
    return send_join_result(c, qid, 0, 0, 0, -1);

  jid = capnp_get_u32(&key, JKP_JOINID_OFF, 0);
  pcount = (int)capnp_get_u16(&key, JKP_PARTCOUNT_OFF, 0);
  pnum = (int)capnp_get_u16(&key, JKP_PARTNUM_OFF, 0);
  if (pcount <= 0 || pcount > CAPNP_RPC_MAX_JOIN_PARTS || pnum >= pcount)
    return send_join_result(c, qid, jid, 0, 0, -1);

  slot = join_slot_find(c, jid, pcount);
  if (slot < 0 || c->joins[slot].seen[pnum])
    /* Table full, a disagreeing partCount, or a partNum reused before the
     * set completed: none of those leave the set answerable. */
    return send_join_result(c, qid, jid, 0, 0, -1);

  eid = -1;
  if (capnp_getp(join, 0, &target) == CAPNP_OK)
    eid = resolve_target(c, &target);
  c->joins[slot].seen[pnum] = 1;
  c->joins[slot].qids[pnum] = qid;
  c->joins[slot].eids[pnum] = eid;
  c->joins[slot].nseen++;

  if (c->joins[slot].nseen == c->joins[slot].part_count)
    return join_complete(c, slot);
  return CAPNP_OK;
}

/* Echo a message we did not understand, per the spec. */
static int send_unimplemented(capnp_rpc_conn_t *c, const capnp_ptr_t *orig)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot;
  int rc;

  capnp_builder_init(&b);
  rc = capnp_builder_root(&b, &root) ? CAPNP_ERR_ALLOC : CAPNP_OK;
  if (rc == CAPNP_OK && capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg))
    rc = CAPNP_ERR_ALLOC;
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&msg, MESSAGE_TAG_OFF,
                               CAPNP_RPC_MSG_UNIMPLEMENTED);
  if (rc == CAPNP_OK && capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot))
    rc = CAPNP_ERR_ALLOC;
  if (rc == CAPNP_OK)
    rc = capnp_builder_copy_ptr(&slot, orig);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

int capnp_rpc_handle(capnp_rpc_conn_t *c, const uint8_t *data, size_t len)
{
  capnp_message_t m;
  capnp_ptr_t root, body;
  int rc = CAPNP_OK;
  uint16_t which;

  if (capnp_message_from_flat(&m, data, len) != CAPNP_OK)
    return CAPNP_ERR_FRAMING;
  if (capnp_root(&m, &root) != CAPNP_OK) {
    capnp_message_free(&m);
    return CAPNP_ERR_FRAMING;
  }

  which = capnp_get_u16(&root, MESSAGE_TAG_OFF, 0);
  if (capnp_getp(&root, 0, &body) != CAPNP_OK)
    memset(&body, 0, sizeof body);

  switch (which) {
  case CAPNP_RPC_MSG_BOOTSTRAP:
    rc = handle_bootstrap(c, &body);
    break;
  case CAPNP_RPC_MSG_CALL:
    rc = handle_call(c, &body);
    break;
  case CAPNP_RPC_MSG_FINISH:
    /* Nothing is retained per answer yet, so a Finish only needs to be
     * accepted rather than acted on. */
    break;
  case CAPNP_RPC_MSG_RELEASE:
    handle_release(c, &body);
    break;
  case CAPNP_RPC_MSG_JOIN:
    rc = handle_join(c, &body);
    break;
  default:
    /* Provide and Accept name a third vat, which a two-party connection
     * cannot; the obsolete save/delete messages are gone from the
     * protocol. */
    rc = send_unimplemented(c, &root);
    break;
  }

  capnp_message_free(&m);
  return rc;
}
