/* Two-party Cap'n Proto RPC vat. See capnp-janet/capnp_rpc.h.
 *
 * Field offsets below are from rpc.capnp and rpc-twoparty.capnp; each is
 * named beside its schema field so the two can be checked against each
 * other by eye.
 */

#include <capnp-janet/capnp_rpc.h>

#include <stdlib.h>
#include <string.h>

/* Defined below, beside the rest of the answer table. */
static capnp_rpc_answer_t *answer_find(capnp_rpc_conn_t *c, uint32_t qid);

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
#define RETURN_TAG_EXCEPTION 1
/* Exception: reason @ptr0, type @0 (u16 enum). */
#define EXCEPTION_DW 1
#define EXCEPTION_PW 2
#define EXCEPTION_TYPE_OFF 0
#define EXCEPTION_TYPE_FAILED 0
/* MessageTarget: importedCap @0 (u32), union tag at byte 4. */
#define TARGET_IMPORTEDCAP_OFF 0
#define TARGET_TAG_OFF 4
#define TARGET_TAG_IMPORTEDCAP 0
/* CapDescriptor: union tag at byte 0, senderHosted @4 (u32). */
#define CAPDESC_TAG_OFF 0
#define CAPDESC_SENDERHOSTED_OFF 4
#define CAPDESC_TAG_SENDERHOSTED 1
#define CAPDESC_TAG_THIRDPARTYHOSTED 5
/* ThirdPartyCapDescriptor: id @ptr0, vineId @0 (u32).
 * ThirdPartyCapId (rpc-threeparty.capnp): vat @ptr0, nonce @0 (u64).
 * VatId: host @ptr0, port @0 (u16). */
#define TPCD_DW 1
#define TPCD_PW 1
#define TPCD_VINEID_OFF 0
#define TPCID_DW 1
#define TPCID_PW 1
#define TPCID_NONCE_OFF 0
#define VATID_DW 1
#define VATID_PW 1
#define VATID_PORT_OFF 0
/* Call: questionId @0, methodId @4 (u16), interfaceId @8 (u64);
 * target @ptr0, params @ptr1. */
#define CALL_DW 3
#define CALL_PW 3
#define CALL_QUESTIONID_OFF 0
#define CALL_METHODID_OFF 4
#define CALL_INTERFACEID_OFF 8
#define BOOTSTRAP_DW 1
#define BOOTSTRAP_PW 1
#define FINISH_DW 1
#define FINISH_PW 0
#define RELEASE_DW 1
#define RELEASE_PW 0
#define TARGET_DW 1
#define TARGET_PW 1
/* Bootstrap / Join / Release: questionId or id @0 (u32). */
#define QUESTIONID_OFF 0
#define RELEASE_REFCOUNT_OFF 4
/* Provide: questionId @0 (u32), target @ptr0, recipient @ptr1.
 * Accept: questionId @0 (u32), provision @ptr0, embargo @32 (bit).
 * RecipientId (rpc-threeparty.capnp): vat @ptr0, nonce @0 (u64).
 * ProvisionId: nonce @0 (u64). */
#define PROVIDE_DW 1
#define PROVIDE_PW 2
#define ACCEPT_DW 1
#define ACCEPT_PW 1
#define RECIPIENT_NONCE_OFF 0
#define PROVISION_NONCE_OFF 0
/* JoinKeyPart: joinId @0 (u32), partCount @4 (u16), partNum @6 (u16). */
#define JKP_JOINID_OFF 0
#define JKP_PARTCOUNT_OFF 4
#define JKP_PARTNUM_OFF 6
/* PromisedAnswer: questionId @0 (u32), transform @ptr0.
 * PromisedAnswer.Op: union tag @0 (u16), getPointerField @2 (u16). */
#define PA_QUESTIONID_OFF 0
#define PA_OP_TAG_OFF 0
#define PA_OP_GETPTRFIELD_OFF 2
#define PA_OP_TAG_GETPTRFIELD 1
/* Disembargo: target @ptr0; context is a group sharing the data section,
 * union tag @4 (u16), the loopback id @0 (u32). */
#define DIS_CTX_TAG_OFF 4
#define DIS_CTX_VALUE_OFF 0
#define DIS_CTX_SENDER_LOOPBACK 0
#define DIS_CTX_RECEIVER_LOOPBACK 1
#define DISEMBARGO_DW 1
#define DISEMBARGO_PW 1
/* MessageTarget union tags. */
#define TARGET_TAG_PROMISEDANSWER 1
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
static int resolve_promised_answer(capnp_rpc_conn_t *c,
                                   const capnp_ptr_t *promised);

static int resolve_target(capnp_rpc_conn_t *c, const capnp_ptr_t *target)
{
  uint32_t id;
  uint16_t which;
  if (target->kind != CAPNP_PK_STRUCT)
    return -1;
  which = capnp_get_u16(target, TARGET_TAG_OFF, 0);
  if (which == TARGET_TAG_PROMISEDANSWER) {
    capnp_ptr_t pa;
    if (capnp_getp(target, 0, &pa) != CAPNP_OK)
      return -1;
    return resolve_promised_answer(c, &pa);
  }
  if (which != TARGET_TAG_IMPORTEDCAP)
    return -1;
  id = capnp_get_u32(target, TARGET_IMPORTEDCAP_OFF, 0);
  if (id >= CAPNP_RPC_MAX_EXPORTS)
    return -1;
  return c->exports[id].used ? (int)id : -1;
}

/* Promise pipelining: the caller addressed a capability inside an answer,
 * identified by walking the transform ops into that answer's results and
 * reading the capTable entry the resulting pointer names. */
static int resolve_promised_answer(capnp_rpc_conn_t *c,
                                   const capnp_ptr_t *promised)
{
  capnp_rpc_answer_t *a;
  capnp_message_t stored;
  capnp_ptr_t root, ret, payload, cursor, table, desc, ops;
  uint32_t qid;
  int i, n, eid = -1;

  if (promised->kind != CAPNP_PK_STRUCT)
    return -1;
  qid = capnp_get_u32(promised, PA_QUESTIONID_OFF, 0);
  a = answer_find(c, qid);
  if (a == NULL)
    return -1;
  if (capnp_message_from_flat(&stored, a->frame, a->len) != CAPNP_OK)
    return -1;

  if (capnp_root(&stored, &root) != CAPNP_OK)
    goto out;
  if (capnp_get_u16(&root, MESSAGE_TAG_OFF, 0) != CAPNP_RPC_MSG_RETURN)
    goto out;
  if (capnp_getp(&root, 0, &ret) != CAPNP_OK)
    goto out;
  if (capnp_get_u16(&ret, RETURN_TAG_OFF, 0) != RETURN_TAG_RESULTS)
    goto out;
  if (capnp_getp(&ret, 0, &payload) != CAPNP_OK)
    goto out;
  if (capnp_getp(&payload, 0, &cursor) != CAPNP_OK)
    goto out;

  if (capnp_getp(promised, 0, &ops) == CAPNP_OK) {
    n = (int)capnp_list_len(&ops);
    for (i = 0; i < n; i++) {
      capnp_ptr_t op, next;
      if (capnp_list_get_struct(&ops, (uint32_t)i, &op) != CAPNP_OK)
        goto out;
      if (capnp_get_u16(&op, PA_OP_TAG_OFF, 0) != PA_OP_TAG_GETPTRFIELD)
        continue;
      /* The peer chooses the transform, so a step into something with no
       * pointer section is an unresolvable target, not a fault. */
      if (cursor.kind != CAPNP_PK_STRUCT)
        goto out;
      if (capnp_getp(&cursor, capnp_get_u16(&op, PA_OP_GETPTRFIELD_OFF, 0),
                     &next) != CAPNP_OK)
        goto out;
      cursor = next;
    }
  }

  if (cursor.kind != CAPNP_PK_CAP)
    goto out;
  /* The pointer holds a capTable index; the descriptor beside it says
   * which export the caller is naming. */
  if (capnp_getp(&payload, 1, &table) != CAPNP_OK)
    goto out;
  if (cursor.count >= capnp_list_len(&table))
    goto out;
  if (capnp_list_get_struct(&table, cursor.count, &desc) != CAPNP_OK)
    goto out;
  if (capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0) != CAPDESC_TAG_SENDERHOSTED)
    goto out;
  {
    uint32_t id = capnp_get_u32(&desc, CAPDESC_SENDERHOSTED_OFF, 0);
    if (id < CAPNP_RPC_MAX_EXPORTS && c->exports[id].used)
      eid = (int)id;
  }

out:
  capnp_message_free(&stored);
  return eid;
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

static capnp_rpc_answer_t *answer_find(capnp_rpc_conn_t *c, uint32_t qid)
{
  int i;
  for (i = 0; i < CAPNP_RPC_MAX_ANSWERS; i++)
    if (c->answers[i].used && c->answers[i].question_id == qid)
      return &c->answers[i];
  return NULL;
}

static void answer_drop(capnp_rpc_conn_t *c, uint32_t qid)
{
  capnp_rpc_answer_t *a = answer_find(c, qid);
  if (a)
    a->used = 0;
}

/* Send a Return and keep it until `Finish`, so a call pipelined against
 * this answer can still find the capability it names. */
static int flush_answer(capnp_rpc_conn_t *c, capnp_builder_t *b, uint32_t qid)
{
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc, i;
  if (capnp_builder_serialize(b, &flat, &len))
    return CAPNP_ERR_ALLOC;
  if (len <= CAPNP_RPC_MAX_ANSWER_BYTES) {
    for (i = 0; i < CAPNP_RPC_MAX_ANSWERS; i++) {
      if (!c->answers[i].used) {
        c->answers[i].used = 1;
        c->answers[i].question_id = qid;
        memcpy(c->answers[i].frame, flat, len);
        c->answers[i].len = len;
        break;
      }
    }
  }
  rc = c->send(c->send_ctx, flat, len);
  free(flat);
  return rc;
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

/* Answer a question with an exception. A call the vat cannot route still
 * has to be answered, or the caller waits forever. */
static int send_return_exception(capnp_rpc_conn_t *c, uint32_t qid,
                                 const char *reason)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, ret, slot, exc;
  int rc;

  capnp_builder_init(&b);
  rc = capnp_builder_root(&b, &root);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&msg, MESSAGE_TAG_OFF, CAPNP_RPC_MSG_RETURN);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, RETURN_DW, RETURN_PW, &ret);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&ret, RETURN_ANSWERID_OFF, qid);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&ret, RETURN_TAG_OFF, RETURN_TAG_EXCEPTION);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&ret, RETURN_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, EXCEPTION_DW, EXCEPTION_PW, &exc);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&exc, EXCEPTION_TYPE_OFF, EXCEPTION_TYPE_FAILED);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_text(&exc, EXCEPTION_DW, 0, reason, strlen(reason));
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

static int handle_bootstrap(capnp_rpc_conn_t *c, const capnp_ptr_t *boot)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload;
  uint32_t qid = capnp_get_u32(boot, QUESTIONID_OFF, 0);
  int eid, rc;

  if (c->bootstrap == NULL)
    return send_return_exception(c, qid, "no bootstrap capability");
  eid = capnp_rpc_export(c, c->bootstrap, c->bootstrap_dispatch);
  if (eid < 0)
    return send_return_exception(c, qid, "export table full");

  capnp_builder_init(&b);
  rc = begin_return(&b, qid, &ret, &payload);
  if (rc == CAPNP_OK)
    rc = write_cap_table(&payload, eid);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_cap(&payload, PAYLOAD_DW, 0, 0);
  if (rc == CAPNP_OK)
    rc = flush_answer(c, &b, qid);
  capnp_builder_free(&b);
  return rc;
}

/* --- level 3: introductions handed to us ---------------------------- */

/* Record every `thirdPartyHosted` entry in an incoming cap table.
 *
 * The descriptor says where the capability really lives and hands us a
 * vine, an ordinary import through the introducer. Calls on the vine
 * work right away, which is the fallback the spec gives receivers that
 * cannot reach a third party; the vine must therefore outlive the
 * pickup. Dialling the third vat belongs to the network layer, so the
 * arrangement is recorded and handed over rather than acted on here.
 */
static void note_introductions(capnp_rpc_conn_t *c, const capnp_ptr_t *payload)
{
  capnp_ptr_t table;
  uint32_t n, i;
  int j;

  if (payload->kind != CAPNP_PK_STRUCT)
    return;
  if (capnp_getp(payload, 1, &table) != CAPNP_OK)
    return;
  n = capnp_list_len(&table);
  for (i = 0; i < n; i++) {
    capnp_ptr_t desc, tpcd, id, vat;
    const char *host = NULL;
    size_t host_len = 0;

    if (capnp_list_get_struct(&table, i, &desc) != CAPNP_OK)
      continue;
    if (capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0) != CAPDESC_TAG_THIRDPARTYHOSTED)
      continue;
    if (capnp_getp(&desc, 0, &tpcd) != CAPNP_OK || tpcd.kind != CAPNP_PK_STRUCT)
      continue;
    if (capnp_getp(&tpcd, 0, &id) != CAPNP_OK || id.kind != CAPNP_PK_STRUCT)
      continue;
    if (capnp_getp(&id, 0, &vat) != CAPNP_OK || vat.kind != CAPNP_PK_STRUCT)
      continue;
    if (capnp_get_text(&vat, 0, &host, &host_len) != CAPNP_OK || host == NULL)
      continue;
    /* A host that will not fit is refused rather than truncated: a
     * truncated address names a different vat. */
    if (host_len >= CAPNP_RPC_MAX_HOST)
      continue;

    for (j = 0; j < CAPNP_RPC_MAX_INTRODUCTIONS; j++) {
      if (c->introductions[j].used)
        continue;
      c->introductions[j].used = 1;
      c->introductions[j].nonce = capnp_get_u64(&id, TPCID_NONCE_OFF, 0);
      c->introductions[j].vine_id = capnp_get_u32(&tpcd, TPCD_VINEID_OFF, 0);
      c->introductions[j].port = (uint16_t)capnp_get_u16(&vat, VATID_PORT_OFF, 0);
      memcpy(c->introductions[j].host, host, host_len);
      c->introductions[j].host[host_len] = '\0';
      break;
    }
  }
}

int capnp_rpc_pending_introductions(capnp_rpc_conn_t *c,
                                    capnp_rpc_introduction_t *out, int cap)
{
  int i, n = 0;
  for (i = 0; i < CAPNP_RPC_MAX_INTRODUCTIONS; i++) {
    if (!c->introductions[i].used)
      continue;
    if (out != NULL && n < cap)
      out[n] = c->introductions[i];
    n++;
  }
  return n;
}

int capnp_rpc_introduction_done(capnp_rpc_conn_t *c, uint64_t nonce)
{
  int i;
  for (i = 0; i < CAPNP_RPC_MAX_INTRODUCTIONS; i++) {
    if (!c->introductions[i].used || c->introductions[i].nonce != nonce)
      continue;
    /* Releasing the vine is what tells the introducer it may close the
     * Provide it opened on our behalf. */
    capnp_rpc_send_release(c, c->introductions[i].vine_id, 1);
    memset(&c->introductions[i], 0, sizeof c->introductions[i]);
    return 0;
  }
  return -1;
}

int capnp_rpc_write_third_party_cap(capnp_rpc_conn_t *c, capnp_bptr_t *cd,
                                    const char *host, uint16_t port,
                                    uint64_t nonce, uint32_t vine_id)
{
  capnp_bptr_t slot, tpcd, id, vat;

  (void)c;
  if (cd == NULL || host == NULL)
    return CAPNP_ERR_ARG;
  if (capnp_builder_set_u16(cd, CAPDESC_TAG_OFF, CAPDESC_TAG_THIRDPARTYHOSTED))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_slot(cd, CAPDESC_DW, 0, &slot))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_struct(&slot, TPCD_DW, TPCD_PW, &tpcd))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u32(&tpcd, TPCD_VINEID_OFF, vine_id))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_slot(&tpcd, TPCD_DW, 0, &slot))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_struct(&slot, TPCID_DW, TPCID_PW, &id))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u64(&id, TPCID_NONCE_OFF, nonce))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_slot(&id, TPCID_DW, 0, &slot))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_struct(&slot, VATID_DW, VATID_PW, &vat))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_text(&vat, VATID_DW, 0, host, strlen(host)))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u16(&vat, VATID_PORT_OFF, port))
    return CAPNP_ERR_ALLOC;
  return CAPNP_OK;
}

static int handle_call(capnp_rpc_conn_t *c, const capnp_ptr_t *call)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload, results;
  capnp_ptr_t target, params_payload, params;
  uint32_t qid = capnp_get_u32(call, QUESTIONID_OFF, 0);
  int eid, rc;

  if (capnp_getp(call, 0, &target) != CAPNP_OK)
    return send_return_exception(c, qid, "call has no target");
  eid = resolve_target(c, &target);
  if (eid < 0)
    return send_return_exception(c, qid, "no such export");

  memset(&params, 0, sizeof params);
  if (capnp_getp(call, 1, &params_payload) == CAPNP_OK) {
    (void)capnp_getp(&params_payload, 0, &params);
    note_introductions(c, &params_payload);
  }

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
    rc = flush_answer(c, &b, qid);
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

/* A Disembargo with `senderLoopback` is echoed back as `receiverLoopback`
 * carrying the same id. That reflection is what lets the sender know every
 * call it had already sent through a promise has arrived, so it can stop
 * routing new ones the long way round. */
static int handle_disembargo(capnp_rpc_conn_t *c, const capnp_ptr_t *dis)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, out;
  capnp_ptr_t target;
  uint32_t id;
  int rc;

  if (dis->kind != CAPNP_PK_STRUCT)
    return CAPNP_OK;
  /* receiverLoopback is the reply to an embargo we raised, and this vat
   * raises none; accept it without echoing to avoid a loop. */
  if (capnp_get_u16(dis, DIS_CTX_TAG_OFF, 0) != DIS_CTX_SENDER_LOOPBACK)
    return CAPNP_OK;
  id = capnp_get_u32(dis, DIS_CTX_VALUE_OFF, 0);

  capnp_builder_init(&b);
  rc = capnp_builder_root(&b, &root);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&msg, MESSAGE_TAG_OFF, CAPNP_RPC_MSG_DISEMBARGO);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, DISEMBARGO_DW, DISEMBARGO_PW, &out);
  /* Echo the target back untouched: the sender matches on it. */
  if (rc == CAPNP_OK && capnp_getp(dis, 0, &target) == CAPNP_OK &&
      target.kind != CAPNP_PK_NULL) {
    capnp_bptr_t tslot;
    if (capnp_builder_slot(&out, DISEMBARGO_DW, 0, &tslot) == CAPNP_OK)
      (void)capnp_builder_copy_ptr(&tslot, &target);
  }
  /* `context` is a group, so it shares Disembargo's own data section
   * rather than living behind a pointer. */
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&out, DIS_CTX_TAG_OFF, DIS_CTX_RECEIVER_LOOPBACK);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&out, DIS_CTX_VALUE_OFF, id);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
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


/* --- client side -------------------------------------------------- */

static capnp_rpc_question_t *question_claim(capnp_rpc_conn_t *c,
                                            uint32_t *qid_out)
{
  int i;
  for (i = 0; i < CAPNP_RPC_MAX_QUESTIONS; i++) {
    if (!c->questions[i].used) {
      memset(&c->questions[i], 0, sizeof c->questions[i]);
      c->questions[i].used = 1;
      c->questions[i].question_id = c->next_question_id++;
      *qid_out = c->questions[i].question_id;
      return &c->questions[i];
    }
  }
  return NULL;
}

static capnp_rpc_question_t *question_find(capnp_rpc_conn_t *c, uint32_t qid)
{
  int i;
  for (i = 0; i < CAPNP_RPC_MAX_QUESTIONS; i++)
    if (c->questions[i].used && c->questions[i].question_id == qid)
      return &c->questions[i];
  return NULL;
}

/* Build a Message with one struct in its union slot. */
static int begin_message(capnp_builder_t *b, uint16_t tag, uint16_t dw,
                         uint16_t pw, capnp_bptr_t *body_out)
{
  capnp_bptr_t root, msg, slot;
  if (capnp_builder_root(b, &root))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u16(&msg, MESSAGE_TAG_OFF, tag))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_struct(&slot, dw, pw, body_out))
    return CAPNP_ERR_ALLOC;
  return CAPNP_OK;
}

uint32_t capnp_rpc_send_bootstrap(capnp_rpc_conn_t *c)
{
  capnp_builder_t b;
  capnp_bptr_t boot;
  capnp_rpc_question_t *q;
  uint32_t qid = 0;
  int rc;

  q = question_claim(c, &qid);
  if (q == NULL)
    return (uint32_t)-1;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_BOOTSTRAP, BOOTSTRAP_DW, BOOTSTRAP_PW,
                     &boot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&boot, QUESTIONID_OFF, qid);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  if (rc != CAPNP_OK) {
    q->used = 0;
    return (uint32_t)-1;
  }
  return qid;
}

uint32_t capnp_rpc_send_call(capnp_rpc_conn_t *c, uint32_t imported_cap,
                             uint64_t interface_id, uint16_t method_id,
                             uint16_t params_dwords, uint16_t params_pwords,
                             capnp_rpc_fill_fn fill, void *fill_ctx)
{
  capnp_builder_t b;
  capnp_bptr_t call, slot, target, payload, params;
  capnp_rpc_question_t *q;
  uint32_t qid = 0;
  int rc;

  q = question_claim(c, &qid);
  if (q == NULL)
    return (uint32_t)-1;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_CALL, CALL_DW, CALL_PW, &call);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&call, CALL_QUESTIONID_OFF, qid);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&call, CALL_METHODID_OFF, method_id);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u64(&call, CALL_INTERFACEID_OFF, interface_id);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&call, CALL_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&target, TARGET_TAG_OFF, TARGET_TAG_IMPORTEDCAP);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&target, TARGET_IMPORTEDCAP_OFF, imported_cap);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&call, CALL_DW, 1, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, PAYLOAD_DW, PAYLOAD_PW, &payload);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&payload, PAYLOAD_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, params_dwords, params_pwords, &params);
  if (rc == CAPNP_OK && fill)
    fill(fill_ctx, &params);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  if (rc != CAPNP_OK) {
    q->used = 0;
    return (uint32_t)-1;
  }
  return qid;
}

int capnp_rpc_send_finish(capnp_rpc_conn_t *c, uint32_t question_id)
{
  capnp_builder_t b;
  capnp_bptr_t fin;
  capnp_rpc_question_t *q = question_find(c, question_id);
  int rc;
  if (q)
    q->used = 0;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_FINISH, FINISH_DW, FINISH_PW, &fin);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&fin, QUESTIONID_OFF, question_id);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

int capnp_rpc_send_release(capnp_rpc_conn_t *c, uint32_t import_id,
                           uint32_t count)
{
  capnp_builder_t b;
  capnp_bptr_t rel;
  int rc;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_RELEASE, RELEASE_DW, RELEASE_PW, &rel);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&rel, QUESTIONID_OFF, import_id);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&rel, RELEASE_REFCOUNT_OFF, count);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

int capnp_rpc_is_answered(capnp_rpc_conn_t *c, uint32_t question_id)
{
  capnp_rpc_question_t *q = question_find(c, question_id);
  return q && q->answered;
}

int capnp_rpc_is_failed(capnp_rpc_conn_t *c, uint32_t question_id)
{
  capnp_rpc_question_t *q = question_find(c, question_id);
  return q && q->answered && q->failed;
}

int capnp_rpc_answer_content(capnp_rpc_conn_t *c, uint32_t question_id,
                             capnp_message_t *msg_out, capnp_ptr_t *out)
{
  capnp_rpc_question_t *q = question_find(c, question_id);
  capnp_ptr_t root, ret, payload;

  if (q == NULL || !q->answered || q->failed)
    return CAPNP_ERR_ARG;
  if (capnp_message_from_flat(msg_out, q->reply, q->reply_len) != CAPNP_OK)
    return CAPNP_ERR_FRAMING;
  if (capnp_root(msg_out, &root) != CAPNP_OK ||
      capnp_get_u16(&root, MESSAGE_TAG_OFF, 0) != CAPNP_RPC_MSG_RETURN ||
      capnp_getp(&root, 0, &ret) != CAPNP_OK ||
      capnp_get_u16(&ret, RETURN_TAG_OFF, 0) != RETURN_TAG_RESULTS ||
      capnp_getp(&ret, 0, &payload) != CAPNP_OK ||
      capnp_getp(&payload, 0, out) != CAPNP_OK) {
    capnp_message_free(msg_out);
    return CAPNP_ERR_KIND;
  }
  return CAPNP_OK;
}

/* Record a Return against the question that asked it. A Return naming a
 * question this vat never asked is dropped: recording it would let a peer
 * plant answers that later pipelining would trust. */
static void handle_return(capnp_rpc_conn_t *c, const capnp_ptr_t *ret,
                          const uint8_t *frame, size_t len)
{
  capnp_rpc_question_t *q;
  if (ret->kind != CAPNP_PK_STRUCT)
    return;
  q = question_find(c, capnp_get_u32(ret, RETURN_ANSWERID_OFF, 0));
  if (q == NULL || len > CAPNP_RPC_MAX_ANSWER_BYTES)
    return;
  q->answered = 1;
  q->failed = capnp_get_u16(ret, RETURN_TAG_OFF, 0) != RETURN_TAG_RESULTS;
  if (!q->failed) {
    capnp_ptr_t results;
    if (capnp_getp(ret, 0, &results) == CAPNP_OK)
      note_introductions(c, &results);
  }
  memcpy(q->reply, frame, len);
  q->reply_len = len;
}

/* --- stream flow control ------------------------------------------- */

void capnp_rpc_stream_init(capnp_rpc_stream_t *s, int window)
{
  memset(s, 0, sizeof *s);
  if (window < 1)
    window = 1;
  if (window > CAPNP_RPC_STREAM_MAX_WINDOW)
    window = CAPNP_RPC_STREAM_MAX_WINDOW;
  s->window = window;
}

/* Retire the oldest outstanding call. A call that never answered, or
 * answered with an exception, marks the stream failed. */
static void stream_retire_oldest(capnp_rpc_conn_t *c, capnp_rpc_stream_t *s)
{
  uint32_t qid;
  int i;
  if (s->nout == 0)
    return;
  qid = s->qids[0];
  for (i = 1; i < s->nout; i++)
    s->qids[i - 1] = s->qids[i];
  s->nout--;
  if (!capnp_rpc_is_answered(c, qid) || capnp_rpc_is_failed(c, qid)) {
    s->failed = 1;
    if (!s->have_failure) {
      s->have_failure = 1;
      s->first_failure = qid;
    }
  }
  capnp_rpc_send_finish(c, qid);
}

int capnp_rpc_stream_send(capnp_rpc_conn_t *c, capnp_rpc_stream_t *s,
                          uint32_t imported_cap, uint64_t interface_id,
                          uint16_t method_id, uint16_t params_dwords,
                          uint16_t params_pwords, capnp_rpc_fill_fn fill,
                          void *fill_ctx)
{
  uint32_t qid;
  if (s->failed)
    return CAPNP_ERR_ARG;
  if (s->nout >= s->window) {
    stream_retire_oldest(c, s);
    if (s->failed)
      return CAPNP_ERR_ARG;
  }
  qid = capnp_rpc_send_call(c, imported_cap, interface_id, method_id,
                            params_dwords, params_pwords, fill, fill_ctx);
  if (qid == (uint32_t)-1)
    return CAPNP_ERR_ALLOC;
  s->qids[s->nout++] = qid;
  return CAPNP_OK;
}

int capnp_rpc_stream_finish(capnp_rpc_conn_t *c, capnp_rpc_stream_t *s)
{
  while (s->nout > 0)
    stream_retire_oldest(c, s);
  return s->have_failure ? CAPNP_ERR_ARG : CAPNP_OK;
}


/* --- level 3: three-party handoff ---------------------------------- */

/* Answer a question with empty results: the introducer is not waiting for
 * a value, only for confirmation that the arrangement is recorded. */
static int send_empty_return(capnp_rpc_conn_t *c, uint32_t qid)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload;
  int rc;
  capnp_builder_init(&b);
  rc = begin_return(&b, qid, &ret, &payload);
  if (rc == CAPNP_OK)
    rc = flush_answer(c, &b, qid);
  capnp_builder_free(&b);
  return rc;
}

/* `Provide`: hold the target for whoever presents this nonce. */
static int handle_provide(capnp_rpc_conn_t *c, const capnp_ptr_t *provide)
{
  capnp_ptr_t target, recipient;
  uint32_t qid;
  uint64_t nonce;
  int eid, i;

  if (provide->kind != CAPNP_PK_STRUCT)
    return CAPNP_OK;
  qid = capnp_get_u32(provide, QUESTIONID_OFF, 0);
  if (capnp_getp(provide, 0, &target) != CAPNP_OK)
    return send_return_exception(c, qid, "provide: no target");
  eid = resolve_target(c, &target);
  if (eid < 0)
    return send_return_exception(c, qid, "provide: no such capability");
  if (capnp_getp(provide, 1, &recipient) != CAPNP_OK ||
      recipient.kind != CAPNP_PK_STRUCT)
    return send_return_exception(c, qid, "provide: no recipient");
  nonce = capnp_get_u64(&recipient, RECIPIENT_NONCE_OFF, 0);

  for (i = 0; i < CAPNP_RPC_MAX_PROVISIONS; i++) {
    if (!c->provisions[i].used) {
      c->provisions[i].used = 1;
      c->provisions[i].nonce = nonce;
      c->provisions[i].export_id = eid;
      /* The recipient holds a reference once it accepts. */
      c->exports[eid].refcount++;
      return send_empty_return(c, qid);
    }
  }
  return send_return_exception(c, qid, "provide: table full");
}

/* `Accept`: claim a capability a third vat provided for us.
 *
 * A nonce is single-use. Leaving it claimable would let anyone who learns
 * it take the capability again later. */
static int handle_accept(capnp_rpc_conn_t *c, const capnp_ptr_t *accept)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload;
  capnp_ptr_t provision;
  uint32_t qid;
  uint64_t nonce;
  int i, eid = -1, rc;

  if (accept->kind != CAPNP_PK_STRUCT)
    return CAPNP_OK;
  qid = capnp_get_u32(accept, QUESTIONID_OFF, 0);
  if (capnp_getp(accept, 0, &provision) != CAPNP_OK ||
      provision.kind != CAPNP_PK_STRUCT)
    return send_return_exception(c, qid, "accept: no provision");
  nonce = capnp_get_u64(&provision, PROVISION_NONCE_OFF, 0);

  for (i = 0; i < CAPNP_RPC_MAX_PROVISIONS; i++) {
    if (c->provisions[i].used && c->provisions[i].nonce == nonce) {
      eid = c->provisions[i].export_id;
      c->provisions[i].used = 0;
      break;
    }
  }
  if (eid < 0)
    return send_return_exception(c, qid, "accept: no such provision");

  capnp_builder_init(&b);
  rc = begin_return(&b, qid, &ret, &payload);
  if (rc == CAPNP_OK)
    rc = write_cap_table(&payload, eid);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_cap(&payload, PAYLOAD_DW, 0, 0);
  if (rc == CAPNP_OK)
    rc = flush_answer(c, &b, qid);
  capnp_builder_free(&b);
  return rc;
}

int capnp_rpc_pending_provisions(capnp_rpc_conn_t *c, uint64_t *out, int cap)
{
  int i, n = 0;
  for (i = 0; i < CAPNP_RPC_MAX_PROVISIONS; i++) {
    if (c->provisions[i].used) {
      if (out && n < cap)
        out[n] = c->provisions[i].nonce;
      n++;
    }
  }
  return n;
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
    /* The caller is done with the answer, so the results it might have
     * pipelined against can go. */
    answer_drop(c, capnp_get_u32(&body, QUESTIONID_OFF, 0));
    break;
  case CAPNP_RPC_MSG_RELEASE:
    handle_release(c, &body);
    break;
  case CAPNP_RPC_MSG_JOIN:
    rc = handle_join(c, &body);
    break;
  case CAPNP_RPC_MSG_DISEMBARGO:
    rc = handle_disembargo(c, &body);
    break;
  case CAPNP_RPC_MSG_RETURN:
    handle_return(c, &body, data, len);
    break;
  case CAPNP_RPC_MSG_PROVIDE:
    rc = handle_provide(c, &body);
    break;
  case CAPNP_RPC_MSG_ACCEPT:
    rc = handle_accept(c, &body);
    break;
  case CAPNP_RPC_MSG_RESOLVE:
    /* Promise resolution. Replying unimplemented is the spec-defined
     * signal that this vat does not adopt resolutions: the sender keeps
     * forwarding calls addressed to the promise, which it does until
     * Release. */
    rc = send_unimplemented(c, &root);
    break;
  default:
    /* The obsolete save/delete messages. */
    rc = send_unimplemented(c, &root);
    break;
  }

  capnp_message_free(&m);
  return rc;
}
