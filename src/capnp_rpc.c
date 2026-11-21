/* Two-party Cap'n Proto RPC vat. See capnp-janet/capnp_rpc.h.
 *
 * Field offsets below are from rpc.capnp and rpc-twoparty.capnp; each is
 * named beside its schema field so the two can be checked against each
 * other by eye.
 */

#include <capnp-janet/capnp_rpc.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined below, beside the rest of the answer and question tables. */
static capnp_rpc_answer_t *answer_find(capnp_rpc_conn_t *c, uint32_t qid);
static void questions_fail_all(capnp_rpc_conn_t *c);

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

/* `used` value of an answer in a tail call; see capnp_rpc.h. */
#define SLOT_MARKED 2
/* A redirected answer whose results a takeFromOtherQuestion collected. */
#define SLOT_TAKEN 3
/* A call with onlyPromisePipeline, kept for pipelining and never
 * returned. */
#define SLOT_QUIET 4

/* How keep_answer treats a Return: sent, kept for sendResultsTo.yourself,
 * or kept without a word for onlyPromisePipeline. */
enum { KEEP_SEND, KEEP_REDIRECT, KEEP_QUIET };
/* Bits of a question's `used`: asked, sent with sendResultsTo.yourself,
 * and finished by us before its Return arrived. */
#define Q_USED 1
#define Q_TAIL 2
#define Q_FINISHED 4
/* Its Return said `noFinishNeeded`. */
#define Q_NO_FINISH 8

/* Message.union discriminant, byte 0. */
#define MESSAGE_TAG_OFF 0
/* Return: answerId @0 (u32), union tag at byte 6. */
#define RETURN_ANSWERID_OFF 0
#define RETURN_TAG_OFF 6
#define RETURN_TAG_RESULTS 0
#define RETURN_TAG_EXCEPTION 1
#define RETURN_TAG_CANCELED 2
#define RETURN_TAG_RESULTS_SENT_ELSEWHERE 3
#define RETURN_TAG_TAKE_FROM_OTHER_QUESTION 4
/* Return.takeFromOtherQuestion @6 (u32). */
#define RETURN_TAKEFROM_OFF 8
/* Exception: reason @ptr0, type @3 (u16 enum) at bits [32, 48). */
#define EXCEPTION_DW 1
#define EXCEPTION_PW 2
#define EXCEPTION_TYPE_OFF 4
#define EXCEPTION_TYPE_FAILED 0
/* MessageTarget: importedCap @0 (u32), union tag at byte 4. */
#define TARGET_IMPORTEDCAP_OFF 0
#define TARGET_TAG_OFF 4
#define TARGET_TAG_IMPORTEDCAP 0
/* CapDescriptor: union tag at byte 0, senderHosted @4 (u32). */
#define CAPDESC_TAG_OFF 0
#define CAPDESC_SENDERHOSTED_OFF 4
#define CAPDESC_TAG_SENDERHOSTED 1
#define CAPDESC_TAG_SENDERPROMISE 2
#define CAPDESC_TAG_RECEIVERHOSTED 3
#define CAPDESC_TAG_RECEIVERANSWER 4
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
/* Call.sendResultsTo: union tag at byte 6, caller 0, yourself 1. */
#define CALL_SENDRESULTSTO_OFF 6
#define CALL_SENDRESULTSTO_YOURSELF 1
/* Call.onlyPromisePipeline: the caller wants no Return. */
#define CALL_ONLY_PIPELINE_BIT 130
#define BOOTSTRAP_DW 1
#define BOOTSTRAP_PW 1
#define FINISH_DW 1
#define FINISH_PW 0
/* Finish.releaseResultCaps, stored XOR its default of true. */
#define FINISH_RELEASE_CAPS_BIT 32
/* Return.releaseParamCaps, stored inverted: its default is true. */
#define RETURN_RELEASE_PARAMS_BIT 32
#define RETURN_NO_FINISH_BIT 33
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
/* RecipientId: vat @ptr0, nonce @0 (u64). ProvisionId: nonce @0. */
#define RECIPIENT_DW 1
#define RECIPIENT_PW 1
#define PROVISION_DW 1
#define PROVISION_PW 0
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
#define PA_DW 1
#define PA_PW 1
#define PA_OP_DW 1
#define PA_OP_PW 0
/* Resolve: promiseId @0 (u32), union tag at byte 4: cap @ptr0 is 0. */
#define RESOLVE_DW 1
#define RESOLVE_PW 1
#define RESOLVE_PROMISEID_OFF 0
#define RESOLVE_TAG_OFF 4
#define RESOLVE_TAG_CAP 0
#define RESOLVE_TAG_EXCEPTION 1
/* Disembargo: target @ptr0; context is a group sharing the data section,
 * union tag @4 (u16), the loopback id @0 (u32). */
#define DIS_CTX_TAG_OFF 4
#define DIS_CTX_VALUE_OFF 0
#define DIS_CTX_SENDER_LOOPBACK 0
#define DIS_CTX_RECEIVER_LOOPBACK 1
#define DIS_CTX_ACCEPT 2
#define DIS_CTX_PROVIDE 3
/* Accept.embargo is a bit, at bit offset 32 of the data section. */
#define ACCEPT_EMBARGO_BIT 32
#define DISEMBARGO_DW 1
#define DISEMBARGO_PW 1
/* MessageTarget union tags. */
#define TARGET_TAG_PROMISEDANSWER 1
/* JoinResult: joinId @0 (u32), succeeded @32 (bit). */
#define JR_JOINID_OFF 0
#define JR_SUCCEEDED_BIT 32

/* A frame longer than the inline buffer lives on the heap with its
 * address kept at the start of the buffer, so the public structs keep
 * their layout. */
static const uint8_t *frame_bytes(const uint8_t *buf, size_t len)
{
  const uint8_t *p;
  if (len <= CAPNP_RPC_MAX_ANSWER_BYTES)
    return buf;
  memcpy(&p, buf, sizeof p);
  return p;
}

static void frame_clear(uint8_t *buf, size_t *len)
{
  if (*len > CAPNP_RPC_MAX_ANSWER_BYTES) {
    uint8_t *p;
    memcpy(&p, buf, sizeof p);
    free(p);
  }
  *len = 0;
}

static int frame_store(uint8_t *buf, size_t *len_out, const uint8_t *data,
                       size_t len)
{
  frame_clear(buf, len_out);
  if (len <= CAPNP_RPC_MAX_ANSWER_BYTES) {
    memcpy(buf, data, len);
  } else {
    uint8_t *p = (uint8_t *)malloc(len);
    if (p == NULL)
      return CAPNP_ERR_ALLOC;
    memcpy(p, data, len);
    memcpy(buf, &p, sizeof p);
  }
  *len_out = len;
  return CAPNP_OK;
}

static void question_drop(capnp_rpc_question_t *q)
{
  frame_clear(q->reply, &q->reply_len);
  q->used = 0;
}

/* State a connection needs beyond capnp_rpc_conn_t, whose layout is part
 * of the ABI. It lives in a list keyed by the connection's address, made
 * on first use and freed by capnp_rpc_free or the next capnp_rpc_init at
 * that address. A connection is used by one thread at a time; the lock
 * covers only the list. */
/* An import rpc.c++ would keep in its import table. */
typedef struct {
  /* Unique for the connection: a call's holds name the entry by key, as
   * its id may be reused once released. */
  uint64_t key;
  uint32_t id;
  /* References from Returns, which the application releases itself. */
  uint32_t app;
  /* References from call params, released together once no call holds
   * the import. */
  uint32_t refs;
  uint32_t holds;
  /* The first descriptor naming it was senderPromise, which rpc.c++
   * gives a fulfiller that a Resolve may settle. */
  int promise;
  /* A Resolve settled this promise: it is off the table under `id`, and
   * its holders now hold the import keyed `to`, or nothing when 0. */
  int settled;
  uint64_t to;
} rpc_import_t;

/* A call that keeps the import keyed `key` until it is finished. */
typedef struct {
  uint32_t qid;
  uint64_t key;
} rpc_hold_t;

typedef struct rpc_side {
  const capnp_rpc_conn_t *conn;
  struct rpc_side *next;
  /* This vat has sent Abort. rpc.c++ reads nothing after that, so the
   * peer's Abort in reply goes unanswered. */
  int aborted;
  rpc_import_t *imports;
  size_t nimports, cimports;
  uint64_t next_key;
  rpc_hold_t *holds;
  size_t nholds, cholds;
} rpc_side_t;

static pthread_mutex_t side_lock = PTHREAD_MUTEX_INITIALIZER;
static rpc_side_t *side_list;

static rpc_side_t *side_get(const capnp_rpc_conn_t *c, int create)
{
  rpc_side_t *s;
  pthread_mutex_lock(&side_lock);
  for (s = side_list; s != NULL && s->conn != c; s = s->next)
    ;
  if (s == NULL && create) {
    s = (rpc_side_t *)calloc(1, sizeof *s);
    if (s != NULL) {
      s->conn = c;
      s->next = side_list;
      side_list = s;
    }
  }
  pthread_mutex_unlock(&side_lock);
  return s;
}

static void side_drop(const capnp_rpc_conn_t *c)
{
  rpc_side_t **p, *s = NULL;
  pthread_mutex_lock(&side_lock);
  for (p = &side_list; *p != NULL; p = &(*p)->next) {
    if ((*p)->conn == c) {
      s = *p;
      *p = s->next;
      break;
    }
  }
  pthread_mutex_unlock(&side_lock);
  if (s != NULL) {
    free(s->imports);
    free(s->holds);
  }
  free(s);
}

static rpc_import_t *import_find(rpc_side_t *s, uint32_t id)
{
  size_t i;
  for (i = 0; s != NULL && i < s->nimports; i++)
    if (s->imports[i].id == id && !s->imports[i].settled)
      return &s->imports[i];
  return NULL;
}

static rpc_import_t *import_by_key(rpc_side_t *s, uint64_t key)
{
  size_t i;
  for (i = 0; s != NULL && i < s->nimports; i++)
    if (s->imports[i].key == key)
      return &s->imports[i];
  return NULL;
}

/* The entry for import `id`, made if new. A new entry can move the
 * others. */
static rpc_import_t *import_get(rpc_side_t *s, uint32_t id, int promise)
{
  rpc_import_t *im = import_find(s, id);
  if (s == NULL || im != NULL)
    return im;
  if (s->nimports == s->cimports) {
    size_t n = s->cimports ? s->cimports * 2 : 8;
    rpc_import_t *grown =
        (rpc_import_t *)realloc(s->imports, n * sizeof *grown);
    if (grown == NULL)
      return NULL;
    s->imports = grown;
    s->cimports = n;
  }
  im = &s->imports[s->nimports++];
  memset(im, 0, sizeof *im);
  im->key = ++s->next_key;
  im->id = id;
  im->promise = promise;
  return im;
}

static void import_remove(rpc_side_t *s, rpc_import_t *im)
{
  *im = s->imports[--s->nimports];
}

/* One more reference a Return handed the application, as rpc.c++ counts
 * each descriptor that names an import. */
static void import_add(capnp_rpc_conn_t *c, uint32_t id, int promise)
{
  rpc_import_t *im = import_get(side_get(c, 1), id, promise);
  if (im != NULL)
    im->app++;
}

/* The application drops `count` references; the last one also gives
 * back those a Resolve added, which it returns. */
static uint32_t import_drop(capnp_rpc_conn_t *c, uint32_t id, uint32_t count)
{
  rpc_side_t *s = side_get(c, 0);
  rpc_import_t *im = import_find(s, id);
  uint32_t refs;
  if (im == NULL)
    return 0;
  im->app = count < im->app ? im->app - count : 0;
  if (im->app > 0 || im->holds > 0)
    return 0;
  refs = im->refs;
  import_remove(s, im);
  return refs;
}

/* A reference a call's params hold; the key names the hold, 0 when none
 * could be recorded. */
static uint64_t import_hold(capnp_rpc_conn_t *c, uint32_t id, int promise)
{
  rpc_import_t *im = import_get(side_get(c, 1), id, promise);
  if (im == NULL)
    return 0;
  im->refs++;
  im->holds++;
  return im->key;
}

static int send_release(capnp_rpc_conn_t *c, uint32_t import_id,
                        uint32_t count);

/* Drop one hold. The last one releases every reference params took, in
 * one Release, as rpc.c++ drops an ImportClient. */
static int hold_drop(capnp_rpc_conn_t *c, uint64_t key)
{
  rpc_side_t *s = side_get(c, 0);
  rpc_import_t *im = import_by_key(s, key);
  uint32_t id, refs;
  uint64_t to;
  if (im == NULL || im->holds == 0 || --im->holds > 0)
    return CAPNP_OK;
  if (im->settled) {
    to = im->to;
    import_remove(s, im);
    return to ? hold_drop(c, to) : CAPNP_OK;
  }
  id = im->id;
  refs = im->refs;
  im->refs = 0;
  if (im->app == 0)
    import_remove(s, im);
  return refs ? send_release(c, id, refs) : CAPNP_OK;
}

/* rpc.c++ keeps a call's capabilities in a kj::Array, which destroys its
 * elements last to first. */
static int holds_drop(capnp_rpc_conn_t *c, const uint64_t *keys, size_t n)
{
  size_t i;
  int rc = CAPNP_OK;
  for (i = n; i-- > 0;) {
    int r = hold_drop(c, keys[i]);
    if (rc == CAPNP_OK)
      rc = r;
  }
  return rc;
}

/* Keep a call's holds until its answer is finished. */
static void holds_keep(capnp_rpc_conn_t *c, uint32_t qid, const uint64_t *keys,
                       size_t n)
{
  rpc_side_t *s = side_get(c, 1);
  size_t i;
  if (s == NULL || n == 0)
    return;
  if (s->nholds + n > s->cholds) {
    size_t cap = (s->nholds + n) * 2;
    rpc_hold_t *grown = (rpc_hold_t *)realloc(s->holds, cap * sizeof *grown);
    if (grown == NULL)
      return;
    s->holds = grown;
    s->cholds = cap;
  }
  for (i = 0; i < n; i++) {
    s->holds[s->nholds].qid = qid;
    s->holds[s->nholds].key = keys[i];
    s->nholds++;
  }
}

/* The answer to question `qid` is finished: the call's params go. */
static int holds_finish(capnp_rpc_conn_t *c, uint32_t qid)
{
  rpc_side_t *s = side_get(c, 0);
  uint64_t *keys;
  size_t i, j = 0, n = 0;
  int rc;
  if (s == NULL || s->nholds == 0)
    return CAPNP_OK;
  keys = (uint64_t *)malloc(s->nholds * sizeof *keys);
  if (keys == NULL)
    return CAPNP_ERR_ALLOC;
  for (i = 0; i < s->nholds; i++) {
    if (s->holds[i].qid == qid)
      keys[n++] = s->holds[i].key;
    else
      s->holds[j++] = s->holds[i];
  }
  s->nholds = j;
  rc = holds_drop(c, keys, n);
  free(keys);
  return rc;
}

void capnp_rpc_init(capnp_rpc_conn_t *c, capnp_rpc_send_fn send, void *send_ctx)
{
  side_drop(c);
  memset(c, 0, sizeof(*c));
  c->send = send;
  c->send_ctx = send_ctx;
  c->vat = &c->own_vat;
}

void capnp_rpc_free(capnp_rpc_conn_t *c)
{
  int i;
  if (c == NULL)
    return;
  side_drop(c);
  for (i = 0; i < CAPNP_RPC_MAX_ANSWERS; i++)
    frame_clear(c->answers[i].frame, &c->answers[i].len);
  for (i = 0; i < CAPNP_RPC_MAX_QUESTIONS; i++)
    frame_clear(c->questions[i].reply, &c->questions[i].reply_len);
}

void capnp_rpc_set_vat(capnp_rpc_conn_t *c, capnp_rpc_vat_t *vat)
{
  c->vat = vat != NULL ? vat : &c->own_vat;
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
  capnp_message_t stored = {0};
  capnp_ptr_t root, ret, payload, cursor, table, desc, ops;
  uint32_t qid;
  int i, n, eid = -1;

  /* A null promisedAnswer reads as its default: question 0, no ops. */
  if (promised->kind != CAPNP_PK_STRUCT && promised->kind != CAPNP_PK_NULL)
    return -1;
  qid = capnp_get_u32(promised, PA_QUESTIONID_OFF, 0);
  a = answer_find(c, qid);
  if (a == NULL)
    return -1;
  if (capnp_message_from_flat(&stored, frame_bytes(a->frame, a->len), a->len) != CAPNP_OK)
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
  if (capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0) != CAPDESC_TAG_SENDERHOSTED &&
      capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0) != CAPDESC_TAG_SENDERPROMISE)
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

#define NO_PIPELINE_REASON                                                     \
  "Pipeline call on a request that returned no capabilities or was "          \
  "already closed."

/* Why a call pipelined on `promised` cannot be routed, as rpc.c++ words
 * it. A missing answer, or results with no capabilities, leave no
 * pipeline; a failed answer breaks every call made on it with its own
 * exception; and a bootstrap answer is a single capability that no
 * transform leads into. Copies into `buf`. */
static const char *promised_failure(capnp_rpc_conn_t *c,
                                    const capnp_ptr_t *promised, char *buf,
                                    size_t cap, uint16_t *type)
{
  capnp_rpc_answer_t *a;
  capnp_message_t stored = {0};
  capnp_ptr_t root, ret, exc, payload, content, table, ops;
  const char *text = NULL;
  size_t n = 0;
  const char *reason = "no such export";
  uint32_t nops = 0;

  *type = EXCEPTION_TYPE_FAILED;
  if (promised->kind != CAPNP_PK_STRUCT && promised->kind != CAPNP_PK_NULL)
    return reason;
  a = answer_find(c, capnp_get_u32(promised, PA_QUESTIONID_OFF, 0));
  if (a == NULL)
    return NO_PIPELINE_REASON;
  if (capnp_getp(promised, 0, &ops) == CAPNP_OK)
    nops = capnp_list_len(&ops);
  if (capnp_message_from_flat(&stored, frame_bytes(a->frame, a->len),
                              a->len) != CAPNP_OK ||
      capnp_root(&stored, &root) != CAPNP_OK ||
      capnp_getp(&root, 0, &ret) != CAPNP_OK)
    goto out;
  switch (capnp_get_u16(&ret, RETURN_TAG_OFF, 0)) {
  case RETURN_TAG_EXCEPTION:
    if (capnp_getp(&ret, 0, &exc) == CAPNP_OK &&
        capnp_get_text(&exc, 0, &text, &n) == CAPNP_OK && text != NULL &&
        n < cap) {
      memcpy(buf, text, n);
      buf[n] = '\0';
      reason = buf;
      *type = capnp_get_u16(&exc, EXCEPTION_TYPE_OFF, 0);
    }
    break;
  case RETURN_TAG_RESULTS:
    if (capnp_getp(&ret, 0, &payload) != CAPNP_OK)
      break;
    if (capnp_getp(&payload, 0, &content) == CAPNP_OK &&
        content.kind == CAPNP_PK_CAP && nops > 0)
      reason = "Invalid pipeline transform.";
    else if (capnp_getp(&payload, 1, &table) != CAPNP_OK ||
             capnp_list_len(&table) == 0)
      reason = NO_PIPELINE_REASON;
    break;
  default:
    break;
  }
out:
  capnp_message_free(&stored);
  return reason;
}

/* Payload.capTable naming `caps`: our exports as senderHosted, and the
 * peer's own capabilities handed back as receiverHosted. */
static int write_cap_refs(const capnp_bptr_t *payload,
                          const capnp_rpc_cap_ref_t *caps, uint32_t ncaps)
{
  capnp_bptr_t cd;
  uint32_t i;
  if (ncaps == 0)
    return CAPNP_OK;
  if (capnp_builder_set_list_struct(payload, PAYLOAD_DW, 1, ncaps,
                                    CAPDESC_DW, CAPDESC_PW, &cd))
    return CAPNP_ERR_ALLOC;
  for (i = 0; i < ncaps; i++) {
    capnp_bptr_t e = cd;
    uint16_t tag = caps[i].kind == CAPNP_RPC_CAP_EXPORT
                       ? CAPDESC_TAG_SENDERHOSTED
                       : CAPDESC_TAG_RECEIVERHOSTED;
    e.word += (size_t)i * (CAPDESC_DW + CAPDESC_PW);
    if (capnp_builder_set_u16(&e, CAPDESC_TAG_OFF, tag) ||
        capnp_builder_set_u32(&e, CAPDESC_SENDERHOSTED_OFF, caps[i].id))
      return CAPNP_ERR_ALLOC;
  }
  return CAPNP_OK;
}

/* The capabilities the method `handle_call` is running has put in its
 * results so far. */
typedef struct result_caps {
  capnp_rpc_conn_t *c;
  capnp_rpc_cap_ref_t caps[CAPNP_RPC_MAX_RESULT_CAPS];
  uint32_t n;
  /* The exception capnp_rpc_fail asked for, if the method fails. */
  char *fail_reason;
  uint16_t fail_type;
} result_caps_t;

static _Thread_local result_caps_t *current_results;

int capnp_rpc_fail(int type, const char *reason)
{
  result_caps_t *r = current_results;
  char *copy;
  if (r == NULL || reason == NULL || type < 0 || type > 0xffff)
    return -1;
  copy = (char *)malloc(strlen(reason) + 1);
  if (copy == NULL)
    return -1;
  strcpy(copy, reason);
  free(r->fail_reason);
  r->fail_reason = copy;
  r->fail_type = (uint16_t)type;
  return 0;
}

int capnp_rpc_unimplemented(const char *interface_name, uint64_t interface_id,
                            uint16_t method_id)
{
  char reason[512];
  if (interface_name == NULL)
    return -1;
  snprintf(reason, sizeof reason,
           "Method not implemented.; interfaceName = %s; typeId = %llu; "
           "methodId = %u",
           interface_name, (unsigned long long)interface_id,
           (unsigned)method_id);
  return capnp_rpc_fail(CAPNP_RPC_EXCEPTION_UNIMPLEMENTED, reason);
}

int capnp_rpc_result_cap(const capnp_rpc_cap_ref_t *cap)
{
  result_caps_t *r = current_results;
  if (r == NULL || cap == NULL || r->n >= CAPNP_RPC_MAX_RESULT_CAPS)
    return -1;
  if (cap->kind == CAPNP_RPC_CAP_EXPORT
          ? cap->id >= CAPNP_RPC_MAX_EXPORTS || !r->c->exports[cap->id].used
          : cap->kind != CAPNP_RPC_CAP_IMPORT)
    return -1;
  r->caps[r->n] = *cap;
  return (int)r->n++;
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
static int write_cap_table_tag(const capnp_bptr_t *payload, int eid,
                               uint16_t tag)
{
  capnp_bptr_t cd;
  if (capnp_builder_set_list_struct(payload, PAYLOAD_DW, 1, 1, CAPDESC_DW,
                                    CAPDESC_PW, &cd))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u16(&cd, CAPDESC_TAG_OFF, tag))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u32(&cd, CAPDESC_SENDERHOSTED_OFF, (uint32_t)eid))
    return CAPNP_ERR_ALLOC;
  return CAPNP_OK;
}

static int write_cap_table(const capnp_bptr_t *payload, int eid)
{
  return write_cap_table_tag(payload, eid, CAPDESC_TAG_SENDERHOSTED);
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
  if (a) {
    frame_clear(a->frame, &a->len);
    a->used = 0;
  }
}

static int protocol_abort(capnp_rpc_conn_t *c, const char *reason);

/* rpc.c++ releaseExport: naming no export, or more references than the
 * export has, aborts. */
static int export_release(capnp_rpc_conn_t *c, uint32_t id, uint32_t count)
{
  if (id >= CAPNP_RPC_MAX_EXPORTS || !c->exports[id].used)
    return protocol_abort(c, "Tried to release invalid export ID.");
  /* Release.referenceCount is an untrusted UInt32 while the local count
   * is signed. Do not narrow a large count before comparing it. */
  if (count > (uint32_t)c->exports[id].refcount) {
    char reason[128];
    snprintf(reason, sizeof reason,
             "expected refcount <= exp->refcount [%u <= %u]; Tried to drop "
             "export's refcount below zero.",
             (unsigned)count, (unsigned)c->exports[id].refcount);
    return protocol_abort(c, reason);
  }
  if (count == (uint32_t)c->exports[id].refcount)
    memset(&c->exports[id], 0, sizeof c->exports[id]);
  else
    c->exports[id].refcount -= (int)count;
  return CAPNP_OK;
}

/* Finish with releaseResultCaps: drop the reference each capability in
 * the stored Return took, as C++ releases an answer's resultExports. */
static int answer_release_caps(capnp_rpc_conn_t *c, uint32_t qid)
{
  capnp_rpc_answer_t *a = answer_find(c, qid);
  capnp_message_t stored = {0};
  capnp_ptr_t root, ret, payload, table;
  uint32_t i, n;
  int rc = CAPNP_OK;
  /* Redirected results never reached the peer, so their capabilities
   * took no references. */
  if (a == NULL || a->used != 1 ||
      capnp_message_from_flat(&stored, frame_bytes(a->frame, a->len), a->len) != CAPNP_OK)
    return CAPNP_OK;
  if (capnp_root(&stored, &root) == CAPNP_OK &&
      capnp_get_u16(&root, MESSAGE_TAG_OFF, 0) == CAPNP_RPC_MSG_RETURN &&
      capnp_getp(&root, 0, &ret) == CAPNP_OK &&
      capnp_get_u16(&ret, RETURN_TAG_OFF, 0) == RETURN_TAG_RESULTS &&
      capnp_getp(&ret, 0, &payload) == CAPNP_OK &&
      capnp_getp(&payload, 1, &table) == CAPNP_OK) {
    n = capnp_list_len(&table);
    for (i = 0; i < n && rc == CAPNP_OK; i++) {
      capnp_ptr_t desc;
      uint16_t tag;
      if (capnp_list_get_struct(&table, i, &desc) != CAPNP_OK)
        continue;
      tag = capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0);
      if (tag == CAPDESC_TAG_SENDERHOSTED || tag == CAPDESC_TAG_SENDERPROMISE)
        rc = export_release(c, capnp_get_u32(&desc, CAPDESC_SENDERHOSTED_OFF, 0), 1);
    }
  }
  capnp_message_free(&stored);
  return rc;
}

static int send_results_sent_elsewhere(capnp_rpc_conn_t *c, uint32_t qid);

/* Keep a Return until `Finish`, so a call pipelined against this answer
 * can still find the capability it names. A call that asked for
 * sendResultsTo.yourself sends nothing yet: the kept Return waits for a
 * takeFromOtherQuestion, and resultsSentElsewhere goes out then or at
 * Finish, as rpc.c++ sends it. One with onlyPromisePipeline is never
 * sent at all. */
static int keep_answer(capnp_rpc_conn_t *c, capnp_builder_t *b, uint32_t qid,
                       int mode)
{
  uint8_t *flat = NULL;
  size_t len = 0;
  int rc, i;
  if (capnp_builder_serialize(b, &flat, &len))
    return CAPNP_ERR_ALLOC;
  for (i = 0; i < CAPNP_RPC_MAX_ANSWERS; i++) {
    if (!c->answers[i].used) {
      if (frame_store(c->answers[i].frame, &c->answers[i].len, flat, len) ==
          CAPNP_OK) {
        c->answers[i].used = mode == KEEP_REDIRECT ? SLOT_MARKED
                             : mode == KEEP_QUIET  ? SLOT_QUIET
                                                   : 1;
        c->answers[i].question_id = qid;
      }
      break;
    }
  }
  rc = mode != KEEP_SEND ? CAPNP_OK : c->send(c->send_ctx, flat, len);
  free(flat);
  return rc;
}

static int flush_answer(capnp_rpc_conn_t *c, capnp_builder_t *b, uint32_t qid)
{
  return keep_answer(c, b, qid, KEEP_SEND);
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

/* A Return message with only the answer id and union tag set. */
static int build_return_tag(capnp_builder_t *b, uint32_t qid, uint16_t tag,
                            capnp_bptr_t *ret)
{
  capnp_bptr_t root, msg, slot;
  if (capnp_builder_root(b, &root) ||
      capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) ||
      capnp_builder_set_u16(&msg, MESSAGE_TAG_OFF, CAPNP_RPC_MSG_RETURN) ||
      capnp_builder_slot(&msg, MESSAGE_DW, 0, &slot) ||
      capnp_builder_struct(&slot, RETURN_DW, RETURN_PW, ret) ||
      capnp_builder_set_u32(ret, RETURN_ANSWERID_OFF, qid) ||
      capnp_builder_set_u16(ret, RETURN_TAG_OFF, tag))
    return CAPNP_ERR_ALLOC;
  return CAPNP_OK;
}

static int send_results_sent_elsewhere(capnp_rpc_conn_t *c, uint32_t qid)
{
  capnp_builder_t b;
  capnp_bptr_t ret;
  int rc;
  capnp_builder_init(&b);
  rc = build_return_tag(&b, qid, RETURN_TAG_RESULTS_SENT_ELSEWHERE, &ret);
  if (rc == CAPNP_OK &&
      capnp_builder_set_bool(&ret, RETURN_RELEASE_PARAMS_BIT, 1) != CAPNP_OK)
    rc = CAPNP_ERR_ALLOC;
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

/* An Exception struct of `type` in slot `ptr` of `body`. */
static int write_exception_typed(const capnp_bptr_t *body, uint16_t dwords,
                                 uint16_t ptr, const char *reason,
                                 size_t reason_len, uint16_t type)
{
  capnp_bptr_t slot, exc;
  if (capnp_builder_slot(body, dwords, ptr, &slot) ||
      capnp_builder_struct(&slot, EXCEPTION_DW, EXCEPTION_PW, &exc) ||
      capnp_builder_set_u16(&exc, EXCEPTION_TYPE_OFF, type) ||
      capnp_builder_set_text(&exc, EXCEPTION_DW, 0, reason, reason_len))
    return CAPNP_ERR_ALLOC;
  return CAPNP_OK;
}

static int write_exception(const capnp_bptr_t *body, uint16_t dwords,
                           uint16_t ptr, const char *reason)
{
  return write_exception_typed(body, dwords, ptr, reason, strlen(reason),
                               EXCEPTION_TYPE_FAILED);
}

/* Answer a call or bootstrap with an exception, kept until Finish as
 * rpc.c++ keeps the answer: its question id stays in use, and a call
 * pipelined on it fails with the same reason. A call that asked for
 * sendResultsTo.yourself keeps it for takeFromOtherQuestion. */
static int answer_exception_typed(capnp_rpc_conn_t *c, uint32_t qid,
                                  const char *reason, size_t len,
                                  uint16_t type, int mode, int call)
{
  capnp_builder_t b;
  capnp_bptr_t ret;
  int rc;
  capnp_builder_init(&b);
  rc = build_return_tag(&b, qid, RETURN_TAG_EXCEPTION, &ret);
  if (rc == CAPNP_OK && call &&
      capnp_builder_set_bool(&ret, RETURN_RELEASE_PARAMS_BIT, 1) != CAPNP_OK)
    rc = CAPNP_ERR_ALLOC;
  if (rc == CAPNP_OK)
    rc = write_exception_typed(&ret, RETURN_DW, 0, reason, len, type);
  if (rc == CAPNP_OK)
    rc = keep_answer(c, &b, qid, mode);
  capnp_builder_free(&b);
  return rc;
}

static int answer_exception(capnp_rpc_conn_t *c, uint32_t qid,
                            const char *reason, int mode)
{
  return answer_exception_typed(c, qid, reason, strlen(reason),
                                EXCEPTION_TYPE_FAILED, mode, 0);
}

/* Abort with rpc.c++'s reason for refusing a message: the connection is
 * over, so every unanswered question fails. */
static int protocol_abort(capnp_rpc_conn_t *c, const char *reason)
{
  capnp_builder_t b;
  capnp_bptr_t root, msg;
  rpc_side_t *s = side_get(c, 1);
  if (s != NULL)
    s->aborted = 1;
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) == CAPNP_OK &&
      capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK &&
      capnp_builder_set_u16(&msg, MESSAGE_TAG_OFF, CAPNP_RPC_MSG_ABORT) ==
          CAPNP_OK &&
      write_exception(&msg, MESSAGE_DW, 0, reason) == CAPNP_OK)
    (void)flush(c, &b);
  capnp_builder_free(&b);
  questions_fail_all(c);
  return CAPNP_ERR_KIND;
}

#define REMOTE_PREFIX "remote exception: "

/* rpc.c++ turns an Abort into a remote exception of the same type and
 * disconnects, which sends that exception back as its own Abort. A
 * reason that already carries the prefix keeps it, so the echo is the
 * reason unchanged. A vat that has sent its own Abort is already
 * disconnected and answers nothing. */
static int handle_abort(capnp_rpc_conn_t *c, const capnp_ptr_t *exc)
{
  const char *text = NULL;
  size_t n = 0, plen = strlen(REMOTE_PREFIX), skip;
  uint16_t type = EXCEPTION_TYPE_FAILED;
  capnp_builder_t b;
  capnp_bptr_t root, msg;
  char *reason;
  rpc_side_t *s = side_get(c, 0);
  questions_fail_all(c);
  if (s != NULL && s->aborted)
    return CAPNP_ERR_IO;
  if (s == NULL)
    s = side_get(c, 1);
  if (s != NULL)
    s->aborted = 1;
  if (exc->kind == CAPNP_PK_STRUCT) {
    type = capnp_get_u16(exc, EXCEPTION_TYPE_OFF, 0);
    if (capnp_get_text(exc, 0, &text, &n) != CAPNP_OK || text == NULL)
      n = 0;
  }
  skip = n >= plen && memcmp(text, REMOTE_PREFIX, plen) == 0 ? plen : 0;
  reason = (char *)malloc(plen + n + 1);
  if (reason == NULL)
    return CAPNP_ERR_IO;
  memcpy(reason, REMOTE_PREFIX, plen);
  if (n > skip)
    memcpy(reason + plen, text + skip, n - skip);
  reason[plen + n - skip] = '\0';
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) == CAPNP_OK &&
      capnp_builder_struct(&root, MESSAGE_DW, MESSAGE_PW, &msg) == CAPNP_OK &&
      capnp_builder_set_u16(&msg, MESSAGE_TAG_OFF, CAPNP_RPC_MSG_ABORT) ==
          CAPNP_OK &&
      write_exception_typed(&msg, MESSAGE_DW, 0, reason, plen + n - skip,
                            type) == CAPNP_OK)
    (void)flush(c, &b);
  capnp_builder_free(&b);
  free(reason);
  return CAPNP_ERR_IO;
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

static int begin_message(capnp_builder_t *b, uint16_t tag, uint16_t dw,
                         uint16_t pw, capnp_bptr_t *body_out);

#define NO_BOOTSTRAP_REASON \
  "This vat does not expose any public/bootstrap interfaces."

/* The capability rpc.c++ hands out when it has no bootstrap: a promise
 * already broken, so every call on it fails with that reason. */
static char no_bootstrap_server;

static int no_bootstrap_dispatch(void *server, uint64_t interface_id,
                                 uint16_t method_id, const capnp_ptr_t *params,
                                 const capnp_bptr_t *results)
{
  (void)server;
  (void)interface_id;
  (void)method_id;
  (void)params;
  (void)results;
  (void)capnp_rpc_fail(EXCEPTION_TYPE_FAILED, NO_BOOTSTRAP_REASON);
  return -1;
}

/* rpc.c++ BootstrapFactory makes a new broken promise for each Bootstrap,
 * exports it as senderPromise, and then sends the Resolve that breaks
 * it. */
static int answer_no_bootstrap(capnp_rpc_conn_t *c, uint32_t qid)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload, res;
  int eid, rc;
  for (eid = 0; eid < CAPNP_RPC_MAX_EXPORTS && c->exports[eid].used; eid++)
    ;
  if (eid == CAPNP_RPC_MAX_EXPORTS)
    return answer_exception(c, qid, "export table full", KEEP_SEND);
  c->exports[eid].used = 1;
  c->exports[eid].refcount = 1;
  c->exports[eid].server = &no_bootstrap_server;
  c->exports[eid].dispatch = no_bootstrap_dispatch;

  capnp_builder_init(&b);
  rc = begin_return(&b, qid, &ret, &payload);
  if (rc == CAPNP_OK)
    rc = write_cap_table_tag(&payload, eid, CAPDESC_TAG_SENDERPROMISE);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_cap(&payload, PAYLOAD_DW, 0, 0);
  if (rc == CAPNP_OK)
    rc = flush_answer(c, &b, qid);
  capnp_builder_free(&b);
  if (rc != CAPNP_OK)
    return rc;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_RESOLVE, RESOLVE_DW, RESOLVE_PW, &res);
  if (rc == CAPNP_OK &&
      (capnp_builder_set_u32(&res, RESOLVE_PROMISEID_OFF, (uint32_t)eid) ||
       capnp_builder_set_u16(&res, RESOLVE_TAG_OFF, RESOLVE_TAG_EXCEPTION)))
    rc = CAPNP_ERR_ALLOC;
  if (rc == CAPNP_OK)
    rc = write_exception(&res, RESOLVE_DW, 0, NO_BOOTSTRAP_REASON);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

static int handle_bootstrap(capnp_rpc_conn_t *c, const capnp_ptr_t *boot)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload;
  capnp_ptr_t object_id;
  uint32_t qid = capnp_get_u32(boot, QUESTIONID_OFF, 0);
  int eid, rc;

  if (answer_find(c, qid) != NULL) {
    char reason[96];
    snprintf(reason, sizeof reason,
             "expected !answer.active; questionId is already in use; "
             "answerId = %u",
             (unsigned)qid);
    return protocol_abort(c, reason);
  }
  /* rpc.c++ answers a 0.4-style named export with this exception when,
   * like this vat, it has no restorer. */
  if (capnp_getp(boot, 0, &object_id) == CAPNP_OK &&
      object_id.kind != CAPNP_PK_NULL)
    return answer_exception(c, qid,
                            "This vat only supports a bootstrap interface, "
                            "not the old Cap'n-Proto-0.4-style named "
                            "exports.",
                            0);
  if (c->bootstrap == NULL)
    return answer_no_bootstrap(c, qid);
  eid = capnp_rpc_export(c, c->bootstrap, c->bootstrap_dispatch);
  if (eid < 0)
    return answer_exception(c, qid, "export table full", KEEP_SEND);

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
/* Each senderHosted or senderPromise entry of a Return's capTable is an
 * import rpc.c++ would hold until released. */
static void note_imports(capnp_rpc_conn_t *c, const capnp_ptr_t *payload)
{
  capnp_ptr_t table, desc;
  uint32_t n, i;
  uint16_t tag;
  if (payload->kind != CAPNP_PK_STRUCT ||
      capnp_getp(payload, 1, &table) != CAPNP_OK)
    return;
  n = capnp_list_len(&table);
  for (i = 0; i < n; i++) {
    if (capnp_list_get_struct(&table, i, &desc) != CAPNP_OK)
      continue;
    tag = capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0);
    if (tag == CAPDESC_TAG_SENDERHOSTED || tag == CAPDESC_TAG_SENDERPROMISE)
      import_add(c, capnp_get_u32(&desc, CAPDESC_SENDERHOSTED_OFF, 0),
                 tag == CAPDESC_TAG_SENDERPROMISE);
  }
}

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

/* The target in pointer 0 of a Call or Disembargo, resolved as rpc.c++
 * getMessageTarget does: an export of ours in `*eid`, or in `*failure`
 * why the promised answer it names holds no capability. A target rpc.c++
 * refuses aborts. A null target reads as the default, importedCap 0. */
static int message_target(capnp_rpc_conn_t *c, const capnp_ptr_t *body,
                          int *eid, const char **failure, char *buf,
                          size_t cap, uint16_t *type)
{
  capnp_ptr_t target;
  uint16_t which = TARGET_TAG_IMPORTEDCAP;
  *eid = -1;
  *failure = NULL;
  if (capnp_getp(body, 0, &target) != CAPNP_OK)
    memset(&target, 0, sizeof target);
  if (target.kind == CAPNP_PK_STRUCT)
    which = capnp_get_u16(&target, TARGET_TAG_OFF, 0);
  if (which == TARGET_TAG_IMPORTEDCAP) {
    uint32_t id = target.kind == CAPNP_PK_STRUCT
                      ? capnp_get_u32(&target, TARGET_IMPORTEDCAP_OFF, 0)
                      : 0;
    if (id >= CAPNP_RPC_MAX_EXPORTS || !c->exports[id].used)
      return protocol_abort(c, "Message target is not a current export ID.");
    *eid = (int)id;
  } else if (which == TARGET_TAG_PROMISEDANSWER) {
    capnp_ptr_t pa;
    if (capnp_getp(&target, 0, &pa) != CAPNP_OK)
      memset(&pa, 0, sizeof pa);
    *eid = resolve_promised_answer(c, &pa);
    if (*eid < 0)
      *failure = promised_failure(c, &pa, buf, cap, type);
  } else {
    /* rpc.c++ prints the target, and a MessageTarget with an unknown arm
     * prints as "()". */
    return protocol_abort(c, "Unknown message target type.; target = ()");
  }
  return CAPNP_OK;
}

/* Each senderHosted or senderPromise entry of a call's capTable is an
 * import the call holds, as rpc.c++ receives every capability of the
 * params before it runs the call. */
static uint64_t *param_holds(capnp_rpc_conn_t *c, const capnp_ptr_t *payload,
                             size_t *n)
{
  capnp_ptr_t table, desc;
  uint32_t i, len;
  uint64_t *keys;
  uint16_t tag;
  *n = 0;
  if (payload->kind != CAPNP_PK_STRUCT ||
      capnp_getp(payload, 1, &table) != CAPNP_OK ||
      (len = capnp_list_len(&table)) == 0)
    return NULL;
  keys = (uint64_t *)malloc(len * sizeof *keys);
  if (keys == NULL)
    return NULL;
  for (i = 0; i < len; i++) {
    if (capnp_list_get_struct(&table, i, &desc) != CAPNP_OK)
      continue;
    tag = capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0);
    if (tag == CAPDESC_TAG_SENDERHOSTED || tag == CAPDESC_TAG_SENDERPROMISE) {
      uint64_t key = import_hold(c, capnp_get_u32(&desc, CAPDESC_SENDERHOSTED_OFF, 0),
                                 tag == CAPDESC_TAG_SENDERPROMISE);
      if (key != 0)
        keys[(*n)++] = key;
    }
  }
  return keys;
}

static int call_answer(capnp_rpc_conn_t *c, const capnp_ptr_t *call,
                       uint32_t qid, int eid, const char *failure,
                       uint16_t ftype, int mode, const capnp_ptr_t *params,
                       int *kept);

/* rpc.c++ releases a call's params when its context goes: right after a
 * Return that leaves nothing to pipeline on, or that is an exception,
 * and otherwise at Finish. */
static int handle_call(capnp_rpc_conn_t *c, const capnp_ptr_t *call)
{
  capnp_ptr_t params_payload, params;
  uint32_t qid = capnp_get_u32(call, QUESTIONID_OFF, 0);
  uint16_t send_to, ftype = EXCEPTION_TYPE_FAILED;
  const char *failure = NULL;
  char reason[1024];
  int eid = -1, rc, yourself, mode, kept = 0;
  uint64_t *keys = NULL;
  size_t nkeys = 0;

  /* The checks and their order are rpc.c++ handleCall's: the target,
   * then sendResultsTo, then the question id. */
  memset(&params, 0, sizeof params);
  rc = message_target(c, call, &eid, &failure, reason, sizeof reason,
                      &ftype);
  if (rc != CAPNP_OK)
    return rc;

  send_to = capnp_get_u16(call, CALL_SENDRESULTSTO_OFF, 0);
  if (send_to > CALL_SENDRESULTSTO_YOURSELF)
    return protocol_abort(c, "Unsupported `Call.sendResultsTo`.");
  yourself = send_to == CALL_SENDRESULTSTO_YOURSELF;
  /* rpc.c++ ignores onlyPromisePipeline on a call it redirects. */
  mode = yourself ? KEEP_REDIRECT
         : capnp_get_bool(call, CALL_ONLY_PIPELINE_BIT, 0) ? KEEP_QUIET
                                                           : KEEP_SEND;

  /* The cap table describes the message, not the dispatch: a call this
   * vat cannot route still told us where a third party's capability
   * lives. */
  if (capnp_getp(call, 1, &params_payload) == CAPNP_OK) {
    (void)capnp_getp(&params_payload, 0, &params);
    note_introductions(c, &params_payload);
    keys = param_holds(c, &params_payload, &nkeys);
  }

  if (answer_find(c, qid) != NULL) {
    /* rpc.c++ has already made the call's context by then, and that
     * context answers as it unwinds, before the Abort: canceled, or
     * resultsSentElsewhere for results sent to the callee, and nothing
     * for an onlyPromisePipeline call. */
    capnp_builder_t b;
    capnp_bptr_t ret;
    capnp_builder_init(&b);
    if (mode != KEEP_QUIET &&
        build_return_tag(&b, qid,
                         yourself ? RETURN_TAG_RESULTS_SENT_ELSEWHERE
                                  : RETURN_TAG_CANCELED,
                         &ret) == CAPNP_OK &&
        capnp_builder_set_bool(&ret, RETURN_RELEASE_PARAMS_BIT, 1) ==
            CAPNP_OK)
      (void)flush(c, &b);
    capnp_builder_free(&b);
    (void)holds_drop(c, keys, nkeys);
    free(keys);
    return protocol_abort(c, "expected !answer.active; questionId is "
                             "already in use");
  }
  rc = call_answer(c, call, qid, eid, failure, ftype, mode, &params, &kept);
  if (kept && answer_find(c, qid) != NULL) {
    holds_keep(c, qid, keys, nkeys);
  } else {
    int r = holds_drop(c, keys, nkeys);
    if (rc == CAPNP_OK)
      rc = r;
  }
  free(keys);
  return rc;
}

/* Answer a call that passed rpc.c++'s checks. `*kept` says whether its
 * params stay until Finish. */
static int call_answer(capnp_rpc_conn_t *c, const capnp_ptr_t *call,
                       uint32_t qid, int eid, const char *failure,
                       uint16_t ftype, int mode, const capnp_ptr_t *params,
                       int *kept)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload, results;
  int rc;
  *kept = mode != KEEP_SEND;
  if (failure != NULL)
    return answer_exception_typed(c, qid, failure, strlen(failure), ftype,
                                  mode, 1);

  capnp_builder_init(&b);
  rc = begin_return(&b, qid, &ret, &payload);
  /* rpc.c++ releases a call's params itself; only a bootstrap's Return
   * leaves releaseParamCaps true. */
  if (rc == CAPNP_OK &&
      capnp_builder_set_bool(&ret, RETURN_RELEASE_PARAMS_BIT, 1) != CAPNP_OK)
    rc = CAPNP_ERR_ALLOC;
  if (rc == CAPNP_OK) {
    /* The server allocates the result struct. A fixed 1-word body would
     * drop every field past that, which C++ does not do. */
    result_caps_t caps;
    result_caps_t *outer = current_results;
    uint32_t i;
    caps.c = c;
    caps.n = 0;
    caps.fail_reason = NULL;
    caps.fail_type = EXCEPTION_TYPE_FAILED;
    if (capnp_builder_slot(&payload, PAYLOAD_DW, 0, &results)) {
      rc = CAPNP_ERR_ALLOC;
    } else {
      int failed;
      current_results = &caps;
      failed = c->exports[eid].dispatch(c->exports[eid].server,
                                        capnp_get_u64(call, 8, 0),
                                        capnp_get_u16(call, 4, 0), params,
                                        &results) != 0;
      current_results = outer;
      if (failed) {
        /* A method that fails is answered with an exception, as rpc.c++
         * does when one throws; otherwise the caller waits forever. */
        const char *why =
            caps.fail_reason != NULL ? caps.fail_reason : "method failed";
        capnp_builder_free(&b);
        rc = answer_exception_typed(c, qid, why, strlen(why), caps.fail_type,
                                    mode, 1);
        free(caps.fail_reason);
        return rc;
      }
      free(caps.fail_reason);
      rc = write_cap_refs(&payload, caps.caps, caps.n);
    }
    /* Each export sent takes a reference the peer gives back with
     * Release. Redirected results stay here, as in rpc.c++, where they
     * are never written out and export nothing. */
    for (i = 0; rc == CAPNP_OK && mode == KEEP_SEND && i < caps.n; i++)
      if (caps.caps[i].kind == CAPNP_RPC_CAP_EXPORT)
        c->exports[caps.caps[i].id].refcount++;
    /* With no capability in the results nothing can be pipelined on
     * them, so rpc.c++ says no Finish is needed and forgets the answer,
     * which frees its question id. */
    *kept = mode != KEEP_SEND || caps.n > 0;
    if (rc == CAPNP_OK && mode == KEEP_SEND && caps.n == 0) {
      if (capnp_builder_set_bool(&ret, RETURN_NO_FINISH_BIT, 1))
        rc = CAPNP_ERR_ALLOC;
      else
        rc = flush(c, &b);
      capnp_builder_free(&b);
      return rc;
    }
  }
  if (rc == CAPNP_OK)
    rc = keep_answer(c, &b, qid, mode);
  capnp_builder_free(&b);
  return rc;
}

static int handle_release(capnp_rpc_conn_t *c, const capnp_ptr_t *rel)
{
  return export_release(c, capnp_get_u32(rel, QUESTIONID_OFF, 0),
                        capnp_get_u32(rel, RELEASE_REFCOUNT_OFF, 0));
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

static int release_provide_embargo(capnp_rpc_conn_t *c, uint32_t provide_qid);

static int provision_known(capnp_rpc_conn_t *c, uint32_t provide_qid)
{
  int i;
  for (i = 0; i < CAPNP_RPC_MAX_PROVISIONS; i++)
    if (c->vat->provisions[i].used &&
        c->vat->provisions[i].question_id == provide_qid)
      return 1;
  return 0;
}

/* Append to `*buf`, growing it; a failed allocation drops the rest. */
static void text_append(char **buf, size_t *len, size_t *cap,
                        const char *fmt, ...)
{
  va_list ap;
  int n;
  if (*buf == NULL)
    return;
  for (;;) {
    va_start(ap, fmt);
    n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0)
      return;
    if ((size_t)n < *cap - *len) {
      *len += (size_t)n;
      return;
    }
    {
      char *grown = (char *)realloc(*buf, *cap * 2 + (size_t)n);
      if (grown == NULL)
        return;
      *buf = grown;
      *cap = *cap * 2 + (size_t)n;
    }
  }
}

/* The reason rpc.c++ gives for a Disembargo context it does not know:
 * the message printed by kj, where a null pointer field is left out and
 * an unknown union arm prints as "()". */
static int abort_unimplemented_disembargo(capnp_rpc_conn_t *c,
                                          const capnp_ptr_t *dis)
{
  size_t len = 0, cap = 128;
  char *buf = (char *)malloc(cap);
  capnp_ptr_t target, pa, ops, op;
  const char *sep = "";
  uint16_t ctx = capnp_get_u16(dis, DIS_CTX_TAG_OFF, 0);
  int rc;
  if (buf == NULL)
    return protocol_abort(c, "Unimplemented Disembargo type.");
  buf[0] = '\0';
  text_append(&buf, &len, &cap, "Unimplemented Disembargo type.; disembargo = (");
  if (capnp_getp(dis, 0, &target) == CAPNP_OK &&
      target.kind == CAPNP_PK_STRUCT) {
    uint16_t arm = capnp_get_u16(&target, TARGET_TAG_OFF, 0);
    text_append(&buf, &len, &cap, "target = (");
    if (arm == TARGET_TAG_IMPORTEDCAP) {
      text_append(&buf, &len, &cap, "importedCap = %u",
                  (unsigned)capnp_get_u32(&target, TARGET_IMPORTEDCAP_OFF, 0));
    } else if (arm == TARGET_TAG_PROMISEDANSWER) {
      if (capnp_getp(&target, 0, &pa) != CAPNP_OK)
        memset(&pa, 0, sizeof pa);
      text_append(&buf, &len, &cap, "promisedAnswer = (questionId = %u",
                  (unsigned)capnp_get_u32(&pa, 0, 0));
      if (pa.kind == CAPNP_PK_STRUCT && capnp_getp(&pa, 0, &ops) == CAPNP_OK &&
          ops.kind == CAPNP_PK_LIST) {
        uint32_t i, n = capnp_list_len(&ops);
        text_append(&buf, &len, &cap, ", transform = [");
        for (i = 0; i < n; i++) {
          uint16_t tag;
          if (capnp_list_get_struct(&ops, i, &op) != CAPNP_OK)
            memset(&op, 0, sizeof op);
          tag = capnp_get_u16(&op, 0, 0);
          if (tag == 0)
            text_append(&buf, &len, &cap, "%s(noop = void)", sep);
          else if (tag == 1)
            text_append(&buf, &len, &cap, "%s(getPointerField = %u)", sep,
                        (unsigned)capnp_get_u16(&op, 2, 0));
          else
            text_append(&buf, &len, &cap, "%s()", sep);
          sep = ", ";
        }
        text_append(&buf, &len, &cap, "]");
      }
      text_append(&buf, &len, &cap, ")");
    }
    text_append(&buf, &len, &cap, "), ");
  }
  if (ctx == DIS_CTX_ACCEPT)
    text_append(&buf, &len, &cap, "context = (accept = void))");
  else if (ctx == DIS_CTX_PROVIDE)
    text_append(&buf, &len, &cap, "context = (provide = %u))",
                (unsigned)capnp_get_u32(dis, DIS_CTX_VALUE_OFF, 0));
  else
    text_append(&buf, &len, &cap, "context = ())");
  rc = protocol_abort(c, buf);
  free(buf);
  return rc;
}

/* A Disembargo as rpc.c++ answers it on a vat that exports no promises
 * and raises no embargoes. A senderLoopback must name a capability that
 * resolved back to the sender, and every export here is local, so it
 * aborts once its target resolves; rpc.c++ prints both objects' brands
 * in the reason. A receiverLoopback names no embargo of ours. rpc.c++
 * refuses every other context. This vat serves level 3, so a provide
 * naming a Provide it holds lifts that Accept's embargo; the introducer
 * may send it before the recipient's Accept arrives on its own
 * connection. */
static int handle_disembargo(capnp_rpc_conn_t *c, const capnp_ptr_t *dis)
{
  const char *failure = NULL;
  char reason[320];
  int eid = -1, rc;
  uint16_t ctx, ftype;

  if (dis->kind != CAPNP_PK_STRUCT)
    return CAPNP_OK;
  ctx = capnp_get_u16(dis, DIS_CTX_TAG_OFF, 0);
  if (ctx == DIS_CTX_PROVIDE &&
      provision_known(c, capnp_get_u32(dis, DIS_CTX_VALUE_OFF, 0)))
    return release_provide_embargo(c, capnp_get_u32(dis, DIS_CTX_VALUE_OFF, 0));
  if (ctx == DIS_CTX_RECEIVER_LOOPBACK)
    return protocol_abort(c, "Invalid embargo ID in "
                             "'Disembargo.context.receiverLoopback'.");
  if (ctx != DIS_CTX_SENDER_LOOPBACK)
    return abort_unimplemented_disembargo(c, dis);
  rc = message_target(c, dis, &eid, &failure, reason, sizeof reason,
                      &ftype);
  if (rc != CAPNP_OK)
    return rc;
  if (failure != NULL)
    return protocol_abort(c, failure);
  /* rpc.c++ waits for the target to resolve before its brand check, and
   * a missing bootstrap resolves to its exception. */
  if (c->exports[eid].server == &no_bootstrap_server)
    return protocol_abort(c, NO_BOOTSTRAP_REASON);
  snprintf(reason, sizeof reason,
           "expected target->getBrand() == this [%lx == %lx]; 'Disembargo' "
           "of type 'senderLoopback' sent to an object that does not point "
           "back to the sender.",
           (unsigned long)(uintptr_t)c->exports[eid].server,
           (unsigned long)(uintptr_t)c);
  return protocol_abort(c, reason);
}

/* Take the capability a Resolve names. With `keep` null it is dropped,
 * as rpc.c++ does when nothing on this side waits for it: one the peer
 * hosts is released at once, unless it names an import still on the
 * table, whose last release then counts it. Otherwise an import it names
 * is held, and `*keep` is the hold's key, or 0 for a capability of ours.
 * `hosted` gets the id of a senderHosted capability or vine, or -1. */
static int resolve_take_cap(capnp_rpc_conn_t *c, const capnp_ptr_t *res,
                            uint64_t *keep, int64_t *hosted)
{
  capnp_ptr_t desc, tpcd;
  rpc_import_t *im;
  uint16_t tag;
  uint32_t id;
  *hosted = -1;
  if (capnp_getp(res, 0, &desc) != CAPNP_OK)
    memset(&desc, 0, sizeof desc);
  tag = desc.kind == CAPNP_PK_STRUCT ? capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0)
                                     : 0;
  switch (tag) {
  case 0:
    return protocol_abort(c, "'Resolve' contained 'CapDescriptor.none'.");
  case CAPDESC_TAG_SENDERHOSTED:
  case CAPDESC_TAG_SENDERPROMISE:
    id = capnp_get_u32(&desc, CAPDESC_SENDERHOSTED_OFF, 0);
    break;
  case CAPDESC_TAG_RECEIVERHOSTED:
  case CAPDESC_TAG_RECEIVERANSWER:
    return CAPNP_OK;
  case CAPDESC_TAG_THIRDPARTYHOSTED:
    /* rpc.c++ imports the vine in place of the third party. */
    if (capnp_getp(&desc, 0, &tpcd) != CAPNP_OK)
      memset(&tpcd, 0, sizeof tpcd);
    id = tpcd.kind == CAPNP_PK_STRUCT ? capnp_get_u32(&tpcd, TPCD_VINEID_OFF, 0)
                                      : 0;
    tag = CAPDESC_TAG_SENDERHOSTED;
    break;
  default:
    return protocol_abort(c, "unknown CapDescriptor type");
  }
  if (tag == CAPDESC_TAG_SENDERHOSTED)
    *hosted = id;
  if (keep == NULL) {
    im = import_find(side_get(c, 0), id);
    if (im == NULL)
      return send_release(c, id, 1);
    im->refs++;
    return CAPNP_OK;
  }
  *keep = import_hold(c, id, tag == CAPDESC_TAG_SENDERPROMISE);
  return CAPNP_OK;
}

/* rpc.c++ fulfils a promise that only calls hold: the replacement takes
 * its place for as long as they hold it, and the promise import is
 * released at once. */
static int resolve_settle(capnp_rpc_conn_t *c, const capnp_ptr_t *res,
                          uint16_t tag, uint64_t key)
{
  rpc_import_t *im;
  uint64_t to = 0;
  int64_t hosted;
  uint32_t id, refs;
  int rc = CAPNP_OK;
  if (tag == RESOLVE_TAG_CAP)
    rc = resolve_take_cap(c, res, &to, &hosted);
  if (rc != CAPNP_OK)
    return rc;
  im = import_by_key(side_get(c, 0), key);
  if (im == NULL)
    return CAPNP_OK;
  if (to == key) {
    /* Resolved to itself: the one import stays, with one more reference
     * to release. */
    im->holds--;
    return CAPNP_OK;
  }
  id = im->id;
  refs = im->refs;
  im->refs = 0;
  im->settled = 1;
  im->to = to;
  return refs ? send_release(c, id, refs) : CAPNP_OK;
}

/* A Resolve as rpc.c++ handleResolve treats it. A promise import only
 * calls hold is settled. Otherwise the new capability is taken and
 * dropped, and then a held import that was not a promise ends the
 * connection, as does a new one the peer hosts under the promise's own
 * id. A promise import the application holds stays until the
 * application releases it, so its id keeps naming one capability;
 * rpc.c++ releases it here and forwards to the replacement. */
static int handle_resolve(capnp_rpc_conn_t *c, const capnp_ptr_t *res)
{
  rpc_import_t *im;
  uint16_t tag;
  uint32_t pid;
  int64_t hosted = -1;
  int rc = CAPNP_OK;
  if (res->kind != CAPNP_PK_STRUCT)
    return CAPNP_OK;
  tag = capnp_get_u16(res, RESOLVE_TAG_OFF, 0);
  if (tag != RESOLVE_TAG_CAP && tag != RESOLVE_TAG_EXCEPTION)
    return protocol_abort(c, "Unknown 'Resolve' type.");
  pid = capnp_get_u32(res, RESOLVE_PROMISEID_OFF, 0);
  im = import_find(side_get(c, 0), pid);
  if (im != NULL && im->promise && im->app == 0)
    return resolve_settle(c, res, tag, im->key);
  if (tag == RESOLVE_TAG_CAP)
    rc = resolve_take_cap(c, res, NULL, &hosted);
  if (rc != CAPNP_OK)
    return rc;
  im = import_find(side_get(c, 0), pid);
  if (im != NULL ? !im->promise : hosted == (int64_t)pid)
    return protocol_abort(c, "Got 'Resolve' for a non-promise import.");
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


/* --- client side -------------------------------------------------- */

static capnp_rpc_question_t *question_claim(capnp_rpc_conn_t *c,
                                            uint32_t *qid_out)
{
  int i;
  for (i = 0; i < CAPNP_RPC_MAX_QUESTIONS; i++) {
    if (!c->questions[i].used) {
      memset(&c->questions[i], 0, sizeof c->questions[i]);
      c->questions[i].used = Q_USED;
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

/* A question the caller still has: asked and not yet finished. */
static capnp_rpc_question_t *question_live(capnp_rpc_conn_t *c, uint32_t qid)
{
  capnp_rpc_question_t *q = question_find(c, qid);
  return q && !(q->used & Q_FINISHED) ? q : NULL;
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

uint32_t capnp_rpc_send_provide(capnp_rpc_conn_t *c, uint32_t imported_cap,
                                const char *recipient_host,
                                uint16_t recipient_port, uint64_t nonce)
{
  capnp_builder_t b;
  capnp_bptr_t provide, slot, target, recipient, vat;
  capnp_rpc_question_t *q;
  uint32_t qid = 0;
  int rc;

  if (recipient_host == NULL)
    return (uint32_t)-1;
  q = question_claim(c, &qid);
  if (q == NULL)
    return (uint32_t)-1;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_PROVIDE, PROVIDE_DW, PROVIDE_PW, &provide);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&provide, QUESTIONID_OFF, qid);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&provide, PROVIDE_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&target, TARGET_TAG_OFF, TARGET_TAG_IMPORTEDCAP);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&target, TARGET_IMPORTEDCAP_OFF, imported_cap);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&provide, PROVIDE_DW, 1, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, RECIPIENT_DW, RECIPIENT_PW, &recipient);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u64(&recipient, RECIPIENT_NONCE_OFF, nonce);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&recipient, RECIPIENT_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, VATID_DW, VATID_PW, &vat);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_text(&vat, VATID_DW, 0, recipient_host,
                                strlen(recipient_host));
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&vat, VATID_PORT_OFF, recipient_port);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  if (rc != CAPNP_OK) {
    question_drop(q);
    return (uint32_t)-1;
  }
  return qid;
}

uint32_t capnp_rpc_send_accept(capnp_rpc_conn_t *c, uint64_t nonce, int embargo)
{
  capnp_builder_t b;
  capnp_bptr_t accept, slot, provision;
  capnp_rpc_question_t *q;
  uint32_t qid = 0;
  int rc;

  q = question_claim(c, &qid);
  if (q == NULL)
    return (uint32_t)-1;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_ACCEPT, ACCEPT_DW, ACCEPT_PW, &accept);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&accept, QUESTIONID_OFF, qid);
  if (rc == CAPNP_OK && embargo)
    rc = capnp_builder_set_bool(&accept, ACCEPT_EMBARGO_BIT, 1);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&accept, ACCEPT_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, PROVISION_DW, PROVISION_PW, &provision);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u64(&provision, PROVISION_NONCE_OFF, nonce);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  if (rc != CAPNP_OK) {
    question_drop(q);
    return (uint32_t)-1;
  }
  return qid;
}

int capnp_rpc_send_disembargo_provide(capnp_rpc_conn_t *c, uint32_t provide_qid)
{
  capnp_builder_t b;
  capnp_bptr_t dis;
  int rc;

  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_DISEMBARGO, DISEMBARGO_DW, DISEMBARGO_PW,
                     &dis);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&dis, DIS_CTX_TAG_OFF, DIS_CTX_PROVIDE);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&dis, DIS_CTX_VALUE_OFF, provide_qid);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
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
    question_drop(q);
    return (uint32_t)-1;
  }
  return qid;
}

/* MessageTarget.promisedAnswer: the question and the getPointerField
 * steps into its results. */
static int write_promised_target(const capnp_bptr_t *target,
                                 const capnp_rpc_call_t *call)
{
  capnp_bptr_t slot, pa, op;
  uint32_t i;
  if (capnp_builder_set_u16(target, TARGET_TAG_OFF, TARGET_TAG_PROMISEDANSWER) ||
      capnp_builder_slot(target, TARGET_DW, 0, &slot) ||
      capnp_builder_struct(&slot, PA_DW, PA_PW, &pa) ||
      capnp_builder_set_u32(&pa, PA_QUESTIONID_OFF, call->target))
    return CAPNP_ERR_ALLOC;
  if (call->transform_len == 0)
    return CAPNP_OK;
  if (capnp_builder_set_list_struct(&pa, PA_DW, 0, call->transform_len,
                                    PA_OP_DW, PA_OP_PW, &op))
    return CAPNP_ERR_ALLOC;
  for (i = 0; i < call->transform_len; i++) {
    capnp_bptr_t e = op;
    e.word += (size_t)i * (PA_OP_DW + PA_OP_PW);
    if (capnp_builder_set_u16(&e, PA_OP_TAG_OFF, PA_OP_TAG_GETPTRFIELD) ||
        capnp_builder_set_u16(&e, PA_OP_GETPTRFIELD_OFF, call->transform[i]))
      return CAPNP_ERR_ALLOC;
  }
  return CAPNP_OK;
}


/* Until its Return arrives a question has no reply, so its buffer keeps
 * the exports the call's params took a reference to: a count, then the
 * ids. rpc.c++ keeps them as the question's paramExports. */
#define PARAM_EXPORTS_MAX (CAPNP_RPC_MAX_ANSWER_BYTES / 4 - 1)

static void param_exports_note(capnp_rpc_question_t *q,
                               const capnp_rpc_call_t *call)
{
  uint32_t i, n = 0;
  for (i = 0; i < call->ncaps && n < PARAM_EXPORTS_MAX; i++) {
    if (call->caps[i].kind == CAPNP_RPC_CAP_EXPORT) {
      memcpy(q->reply + 4 + 4 * n, &call->caps[i].id, 4);
      n++;
    }
  }
  memcpy(q->reply, &n, 4);
}

/* A Return with releaseParamCaps gives those references back. */
static int param_exports_release(capnp_rpc_conn_t *c, capnp_rpc_question_t *q,
                                 int release)
{
  uint32_t i, n, id;
  int rc = CAPNP_OK;
  if (q->reply_len != 0)
    return CAPNP_OK;
  memcpy(&n, q->reply, 4);
  memset(q->reply, 0, 4);
  for (i = 0; release && i < n && rc == CAPNP_OK; i++) {
    memcpy(&id, q->reply + 4 + 4 * i, 4);
    rc = export_release(c, id, 1);
  }
  return rc;
}

uint32_t capnp_rpc_send_call_ex(capnp_rpc_conn_t *c, const capnp_rpc_call_t *call)
{
  capnp_builder_t b;
  capnp_bptr_t msg, slot, target, payload, params;
  capnp_rpc_question_t *q;
  uint32_t qid = 0, i;
  int rc;

  if (call == NULL || (call->transform_len && call->transform == NULL) ||
      (call->ncaps && call->caps == NULL))
    return (uint32_t)-1;
  for (i = 0; i < call->ncaps; i++) {
    const capnp_rpc_cap_ref_t *r = &call->caps[i];
    if (r->kind == CAPNP_RPC_CAP_EXPORT
            ? r->id >= CAPNP_RPC_MAX_EXPORTS || !c->exports[r->id].used
            : r->kind != CAPNP_RPC_CAP_IMPORT)
      return (uint32_t)-1;
  }
  /* rpc.c++ refuses to pipeline on a request it no longer holds. */
  if (call->promised && question_live(c, call->target) == NULL)
    return (uint32_t)-1;
  q = question_claim(c, &qid);
  if (q == NULL)
    return (uint32_t)-1;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_CALL, CALL_DW, CALL_PW, &msg);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&msg, CALL_QUESTIONID_OFF, qid);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u16(&msg, CALL_METHODID_OFF, call->method_id);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u64(&msg, CALL_INTERFACEID_OFF, call->interface_id);
  if (rc == CAPNP_OK && call->results_to_yourself)
    rc = capnp_builder_set_u16(&msg, CALL_SENDRESULTSTO_OFF,
                               CALL_SENDRESULTSTO_YOURSELF);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&msg, CALL_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, TARGET_DW, TARGET_PW, &target);
  if (rc == CAPNP_OK && call->promised) {
    rc = write_promised_target(&target, call);
  } else if (rc == CAPNP_OK) {
    rc = capnp_builder_set_u16(&target, TARGET_TAG_OFF, TARGET_TAG_IMPORTEDCAP);
    if (rc == CAPNP_OK)
      rc = capnp_builder_set_u32(&target, TARGET_IMPORTEDCAP_OFF, call->target);
  }
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&msg, CALL_DW, 1, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, PAYLOAD_DW, PAYLOAD_PW, &payload);
  if (rc == CAPNP_OK)
    rc = capnp_builder_slot(&payload, PAYLOAD_DW, 0, &slot);
  if (rc == CAPNP_OK)
    rc = capnp_builder_struct(&slot, call->params_dwords, call->params_pwords,
                              &params);
  if (rc == CAPNP_OK && call->fill && call->fill(call->fill_ctx, &params) != 0)
    rc = CAPNP_ERR_KIND;
  if (rc == CAPNP_OK)
    rc = write_cap_refs(&payload, call->caps, call->ncaps);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  if (rc != CAPNP_OK) {
    question_drop(q);
    return (uint32_t)-1;
  }
  for (i = 0; i < call->ncaps; i++)
    if (call->caps[i].kind == CAPNP_RPC_CAP_EXPORT)
      c->exports[call->caps[i].id].refcount++;
  param_exports_note(q, call);
  if (call->results_to_yourself)
    q->used |= Q_TAIL;
  return qid;
}

uint32_t capnp_rpc_send_call(capnp_rpc_conn_t *c, uint32_t imported_cap,
                             uint64_t interface_id, uint16_t method_id,
                             uint16_t params_dwords, uint16_t params_pwords,
                             capnp_rpc_fill_fn fill, void *fill_ctx)
{
  capnp_rpc_call_t call;
  memset(&call, 0, sizeof call);
  call.target = imported_cap;
  call.interface_id = interface_id;
  call.method_id = method_id;
  call.params_dwords = params_dwords;
  call.params_pwords = params_pwords;
  call.fill = fill;
  call.fill_ctx = fill_ctx;
  return capnp_rpc_send_call_ex(c, &call);
}

int capnp_rpc_send_finish(capnp_rpc_conn_t *c, uint32_t question_id)
{
  capnp_builder_t b;
  capnp_bptr_t fin;
  capnp_rpc_question_t *q = question_live(c, question_id);
  int keep_caps = q && q->answered;
  int rc;
  /* rpc.c++ skips the Finish a Return said is not needed. */
  if (q && q->answered && (q->used & Q_NO_FINISH)) {
    question_drop(q);
    return CAPNP_OK;
  }
  /* As in rpc.c++, a question whose Return is still to come stays in the
   * table until that Return, which then only closes it. */
  if (q && q->answered)
    question_drop(q);
  else if (q)
    q->used |= Q_FINISHED;
  capnp_builder_init(&b);
  rc = begin_message(&b, CAPNP_RPC_MSG_FINISH, FINISH_DW, FINISH_PW, &fin);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_u32(&fin, QUESTIONID_OFF, question_id);
  /* As in C++: once the Return is in, its capabilities are our imports,
   * dropped by Release; only a cancelled question asks the peer to drop
   * them. */
  if (rc == CAPNP_OK && keep_caps)
    rc = capnp_builder_set_bool(&fin, FINISH_RELEASE_CAPS_BIT, 1);
  if (rc == CAPNP_OK)
    rc = flush(c, &b);
  capnp_builder_free(&b);
  return rc;
}

int capnp_rpc_send_release(capnp_rpc_conn_t *c, uint32_t import_id,
                           uint32_t count)
{
  return send_release(c, import_id, count + import_drop(c, import_id, count));
}

static int send_release(capnp_rpc_conn_t *c, uint32_t import_id,
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
  capnp_rpc_question_t *q = question_live(c, question_id);
  return q && q->answered;
}

int capnp_rpc_is_failed(capnp_rpc_conn_t *c, uint32_t question_id)
{
  capnp_rpc_question_t *q = question_live(c, question_id);
  return q && q->answered && q->failed;
}

/* The descriptor for capTable entry `index` of an answered question's
 * results, or for the entry its content points at when `index` is
 * negative. */
static int answer_desc(capnp_rpc_conn_t *c, uint32_t question_id,
                       int64_t index, capnp_rpc_cap_ref_t *out)
{
  capnp_rpc_question_t *q = question_live(c, question_id);
  capnp_message_t m = {0};
  capnp_ptr_t root, ret, payload, content, table, desc;
  int rc = -1;

  if (q == NULL || out == NULL || !q->answered || q->failed)
    return -1;
  if (capnp_message_from_flat(&m, frame_bytes(q->reply, q->reply_len), q->reply_len) != CAPNP_OK)
    return -1;
  if (capnp_root(&m, &root) != CAPNP_OK ||
      capnp_get_u16(&root, MESSAGE_TAG_OFF, 0) != CAPNP_RPC_MSG_RETURN ||
      capnp_getp(&root, 0, &ret) != CAPNP_OK ||
      capnp_get_u16(&ret, RETURN_TAG_OFF, 0) != RETURN_TAG_RESULTS ||
      capnp_getp(&ret, 0, &payload) != CAPNP_OK)
    goto out;
  if (index < 0) {
    if (capnp_getp(&payload, 0, &content) != CAPNP_OK ||
        content.kind != CAPNP_PK_CAP)
      goto out;
    index = content.count;
  }
  if (capnp_getp(&payload, 1, &table) == CAPNP_OK &&
      (uint64_t)index < capnp_list_len(&table) &&
      capnp_list_get_struct(&table, (uint32_t)index, &desc) == CAPNP_OK) {
    /* senderPromise names an export like senderHosted does; the peer
     * queues calls on it until the promise resolves. */
    uint16_t tag = capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0);
    out->id = capnp_get_u32(&desc, CAPDESC_SENDERHOSTED_OFF, 0);
    if (tag == CAPDESC_TAG_SENDERHOSTED || tag == CAPDESC_TAG_SENDERPROMISE) {
      out->kind = CAPNP_RPC_CAP_IMPORT;
      rc = 0;
    } else if (tag == CAPDESC_TAG_RECEIVERHOSTED) {
      out->kind = CAPNP_RPC_CAP_EXPORT;
      rc = 0;
    }
  }
out:
  capnp_message_free(&m);
  return rc;
}

int capnp_rpc_answer_cap(capnp_rpc_conn_t *c, uint32_t question_id,
                         uint32_t index, capnp_rpc_cap_ref_t *out)
{
  return answer_desc(c, question_id, index, out);
}

int capnp_rpc_answer_cap_id(capnp_rpc_conn_t *c, uint32_t question_id)
{
  capnp_rpc_cap_ref_t ref;
  if (answer_desc(c, question_id, -1, &ref) != 0 ||
      ref.kind != CAPNP_RPC_CAP_IMPORT)
    return -1;
  return (int)ref.id;
}

int capnp_rpc_answer_content(capnp_rpc_conn_t *c, uint32_t question_id,
                             capnp_message_t *msg_out, capnp_ptr_t *out)
{
  capnp_rpc_question_t *q = question_live(c, question_id);
  capnp_ptr_t root, ret, payload;

  if (q == NULL || !q->answered || q->failed)
    return CAPNP_ERR_ARG;
  if (capnp_message_from_flat(msg_out, frame_bytes(q->reply, q->reply_len),
                              q->reply_len) != CAPNP_OK)
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

/* Results a takeFromOtherQuestion collected were written for the peer,
 * so each descriptor names its capability from the far side. Turn them
 * round, as rpc.c++ hands the question the very capabilities it
 * returned: our exports become receiverHosted, and the peer's own
 * become senderHosted imports. */
static void flip_taken_caps(capnp_rpc_question_t *q)
{
  uint8_t *flat = (uint8_t *)frame_bytes(q->reply, q->reply_len);
  capnp_message_t m = {0};
  capnp_ptr_t root, ret, payload, table, desc;
  uint32_t i, n;
  if (capnp_message_view_flat(&m, flat, q->reply_len) != CAPNP_OK)
    return;
  if (capnp_root(&m, &root) == CAPNP_OK &&
      capnp_getp(&root, 0, &ret) == CAPNP_OK &&
      capnp_getp(&ret, 0, &payload) == CAPNP_OK &&
      capnp_getp(&payload, 1, &table) == CAPNP_OK) {
    n = capnp_list_len(&table);
    for (i = 0; i < n; i++) {
      uint8_t *tag;
      uint16_t was, now;
      if (capnp_list_get_struct(&table, i, &desc) != CAPNP_OK ||
          desc.dwords == 0)
        continue;
      was = capnp_get_u16(&desc, CAPDESC_TAG_OFF, 0);
      if (was == CAPDESC_TAG_SENDERHOSTED || was == CAPDESC_TAG_SENDERPROMISE)
        now = CAPDESC_TAG_RECEIVERHOSTED;
      else if (was == CAPDESC_TAG_RECEIVERHOSTED)
        now = CAPDESC_TAG_SENDERHOSTED;
      else
        continue;
      tag = (uint8_t *)m.segs[desc.seg].data + desc.word * 8 + CAPDESC_TAG_OFF;
      tag[0] = (uint8_t)now;
      tag[1] = (uint8_t)(now >> 8);
    }
  }
  capnp_message_free(&m);
}

/* Record a Return against the question that asked it. The checks are the
 * ones rpc.c++ handleReturn makes, and the reasons are the Abort text
 * capnp-C++ sends, condition included. */
static int handle_return(capnp_rpc_conn_t *c, const capnp_ptr_t *ret,
                         const uint8_t *frame, size_t len)
{
  capnp_rpc_question_t *q;
  uint16_t tag;
  if (ret->kind != CAPNP_PK_STRUCT)
    return CAPNP_OK;
  /* rpc.c++ keeps ids with the high bit for calls that want no Return,
   * and ignores a Return naming one. This vat asks no such question. */
  if (capnp_get_u32(ret, RETURN_ANSWERID_OFF, 0) & 0x80000000u)
    return CAPNP_OK;
  q = question_find(c, capnp_get_u32(ret, RETURN_ANSWERID_OFF, 0));
  if (q == NULL)
    return protocol_abort(c, "Invalid question ID in Return message.");
  if (q->answered)
    return protocol_abort(c, "expected question->isAwaitingReturn; "
                             "Duplicate Return.");
  {
    int rc = param_exports_release(
        c, q, !capnp_get_bool(ret, RETURN_RELEASE_PARAMS_BIT, 0));
    if (rc != CAPNP_OK)
      return rc;
  }
  if (capnp_get_bool(ret, RETURN_NO_FINISH_BIT, 0))
    q->used |= Q_NO_FINISH;
  tag = capnp_get_u16(ret, RETURN_TAG_OFF, 0);
  if (q->used & Q_FINISHED) {
    /* We finished it first, so nobody wants the results. A tail call back
     * to us still frees the answer it would have taken. */
    if (tag == RETURN_TAG_TAKE_FROM_OTHER_QUESTION) {
      capnp_rpc_answer_t *a =
          answer_find(c, capnp_get_u32(ret, RETURN_TAKEFROM_OFF, 0));
      question_drop(q);
      if (a != NULL && a->used == SLOT_MARKED) {
        a->used = SLOT_TAKEN;
        return send_results_sent_elsewhere(c, a->question_id);
      }
      return CAPNP_OK;
    }
    question_drop(q);
    return CAPNP_OK;
  }
  if ((q->used & Q_TAIL) && tag == RETURN_TAG_RESULTS)
    return protocol_abort(c, "expected !question->isTailCall; Tail call "
                             "`Return` must set `resultsSentElsewhere`, not "
                             "`results`.");
  if ((q->used & Q_TAIL) && tag == RETURN_TAG_EXCEPTION)
    return protocol_abort(c, "expected !question->isTailCall; Tail call "
                             "`Return` must set `resultsSentElsewhere`, not "
                             "`exception`.");
  if (tag == RETURN_TAG_CANCELED)
    return protocol_abort(c, "Return message falsely claims call was "
                             "canceled.");
  if (tag > RETURN_TAG_TAKE_FROM_OTHER_QUESTION)
    return protocol_abort(c, "Unknown 'Return' type.");
  if (tag == RETURN_TAG_RESULTS_SENT_ELSEWHERE) {
    if (!(q->used & Q_TAIL))
      return protocol_abort(c, "expected question->isTailCall; `Return` had "
                               "`resultsSentElsewhere` but this was not a "
                               "tail call.");
    q->answered = 1;
    q->failed = 0;
    if (frame_store(q->reply, &q->reply_len, frame, len) != CAPNP_OK)
      q->failed = 1;
    return CAPNP_OK;
  }
  if (tag == RETURN_TAG_TAKE_FROM_OTHER_QUESTION) {
    capnp_rpc_answer_t *a =
        answer_find(c, capnp_get_u32(ret, RETURN_TAKEFROM_OFF, 0));
    capnp_message_t kept = {0};
    capnp_ptr_t root, kret;
    if (a == NULL)
      return protocol_abort(c, "`Return.takeFromOtherQuestion` had invalid "
                               "answer ID.");
    if (a->used != SLOT_MARKED)
      return protocol_abort(c, "`Return.takeFromOtherQuestion` referenced a "
                               "call that did not use `sendResultsTo.yourself`.");
    /* The results go to this question, once; the answer itself stays
     * until Finish for anything pipelined on it. */
    a->used = SLOT_TAKEN;
    q->answered = 1;
    q->failed = 1;
    if (capnp_message_from_flat(&kept, frame_bytes(a->frame, a->len), a->len) ==
            CAPNP_OK &&
        capnp_root(&kept, &root) == CAPNP_OK &&
        capnp_getp(&root, 0, &kret) == CAPNP_OK)
      q->failed = capnp_get_u16(&kret, RETURN_TAG_OFF, 0) != RETURN_TAG_RESULTS;
    capnp_message_free(&kept);
    if (frame_store(q->reply, &q->reply_len, frame_bytes(a->frame, a->len),
                    a->len) != CAPNP_OK)
      q->failed = 1;
    if (!q->failed)
      flip_taken_caps(q);
    return send_results_sent_elsewhere(c, a->question_id);
  }
  q->answered = 1;
  q->failed = tag != RETURN_TAG_RESULTS;
  if (!q->failed) {
    capnp_ptr_t results;
    if (capnp_getp(ret, 0, &results) == CAPNP_OK) {
      note_introductions(c, &results);
      note_imports(c, &results);
    }
  }
  if (frame_store(q->reply, &q->reply_len, frame, len) != CAPNP_OK)
    q->failed = 1;
  return CAPNP_OK;
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
    if (!c->vat->provisions[i].used) {
      c->vat->provisions[i].used = 1;
      c->vat->provisions[i].nonce = nonce;
      c->vat->provisions[i].server = c->exports[eid].server;
      c->vat->provisions[i].dispatch = c->exports[eid].dispatch;
      c->vat->provisions[i].export_id = eid;
      c->vat->provisions[i].question_id = qid;
      c->vat->provisions[i].embargoed = 0;
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
  int i, eid = -1, rc, embargo;

  if (accept->kind != CAPNP_PK_STRUCT)
    return CAPNP_OK;
  qid = capnp_get_u32(accept, QUESTIONID_OFF, 0);
  if (capnp_getp(accept, 0, &provision) != CAPNP_OK ||
      provision.kind != CAPNP_PK_STRUCT)
    return send_return_exception(c, qid, "accept: no provision");
  nonce = capnp_get_u64(&provision, PROVISION_NONCE_OFF, 0);

  embargo = capnp_get_bool(accept, ACCEPT_EMBARGO_BIT, 0);
  for (i = 0; i < CAPNP_RPC_MAX_PROVISIONS; i++) {
    if (c->vat->provisions[i].used && !c->vat->provisions[i].embargoed &&
        c->vat->provisions[i].nonce == nonce) {
      /* Export it here: the id the introducer used belongs to its own
       * connection and means nothing on this one. */
      eid = capnp_rpc_export(c, c->vat->provisions[i].server,
                             c->vat->provisions[i].dispatch);
      if (eid < 0)
        return send_return_exception(c, qid, "accept: export table full");
      c->vat->provisions[i].export_id = eid;
      if (embargo) {
        /* Claimed, but the Return waits: the recipient has pipelined
         * calls in flight through the introducer, and answering now
         * would let a later call overtake them. The introducer lifts it
         * with Disembargo.provide. */
        c->vat->provisions[i].embargoed = 1;
        c->vat->provisions[i].accept_question_id = qid;
      } else {
        c->vat->provisions[i].used = 0;
      }
      break;
    }
  }
  if (eid < 0)
    return send_return_exception(c, qid, "accept: no such provision");
  if (embargo)
    return CAPNP_OK;

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

/* Disembargo.provide: the introducer lifts the embargo on the Accept it
 * arranged, naming its own Provide question. */
static int release_provide_embargo(capnp_rpc_conn_t *c, uint32_t provide_qid)
{
  capnp_builder_t b;
  capnp_bptr_t ret, payload;
  int i, rc;

  for (i = 0; i < CAPNP_RPC_MAX_PROVISIONS; i++) {
    uint32_t qid;
    int eid;
    if (!c->vat->provisions[i].used || !c->vat->provisions[i].embargoed)
      continue;
    if (c->vat->provisions[i].question_id != provide_qid)
      continue;
    qid = c->vat->provisions[i].accept_question_id;
    eid = c->vat->provisions[i].export_id;
    memset(&c->vat->provisions[i], 0, sizeof c->vat->provisions[i]);

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
  /* A Disembargo naming nothing we hold is the sender's problem, not a
   * reason to disturb this connection. */
  return CAPNP_OK;
}

int capnp_rpc_embargoed_accepts(capnp_rpc_conn_t *c)
{
  int i, n = 0;
  for (i = 0; i < CAPNP_RPC_MAX_PROVISIONS; i++)
    if (c->vat->provisions[i].used && c->vat->provisions[i].embargoed)
      n++;
  return n;
}

int capnp_rpc_pending_provisions(capnp_rpc_conn_t *c, uint64_t *out, int cap)
{
  int i, n = 0;
  for (i = 0; i < CAPNP_RPC_MAX_PROVISIONS; i++) {
    if (c->vat->provisions[i].used) {
      if (out && n < cap)
        out[n] = c->vat->provisions[i].nonce;
      n++;
    }
  }
  return n;
}

/* The connection is lost: nothing still waiting will be answered. */
static void questions_fail_all(capnp_rpc_conn_t *c)
{
  int i;
  for (i = 0; i < CAPNP_RPC_MAX_QUESTIONS; i++) {
    capnp_rpc_question_t *q = &c->questions[i];
    if (q->used & Q_FINISHED) {
      question_drop(q);
    } else if (q->used && !q->answered) {
      q->answered = 1;
      q->failed = 1;
      frame_clear(q->reply, &q->reply_len);
    }
  }
}

/* rpc.c++ handleUnimplemented: a Resolve may go unadopted, which gives
 * back the reference its descriptor carried, and any other message the
 * peer refused is fatal to the connection. */
static int handle_unimplemented(capnp_rpc_conn_t *c, const capnp_ptr_t *orig)
{
  char reason[96];
  uint16_t which = orig->kind == CAPNP_PK_STRUCT
                       ? capnp_get_u16(orig, MESSAGE_TAG_OFF, 0)
                       : CAPNP_RPC_MSG_UNIMPLEMENTED;
  if (which == CAPNP_RPC_MSG_RESOLVE) {
    capnp_ptr_t res, cap, tpcd;
    uint16_t tag;
    if (capnp_getp(orig, 0, &res) != CAPNP_OK ||
        res.kind != CAPNP_PK_STRUCT ||
        capnp_get_u16(&res, RESOLVE_TAG_OFF, 0) != RESOLVE_TAG_CAP ||
        capnp_getp(&res, 0, &cap) != CAPNP_OK || cap.kind != CAPNP_PK_STRUCT)
      return CAPNP_OK;
    tag = capnp_get_u16(&cap, CAPDESC_TAG_OFF, 0);
    if (tag == CAPDESC_TAG_SENDERHOSTED || tag == CAPDESC_TAG_SENDERPROMISE)
      return export_release(c, capnp_get_u32(&cap, CAPDESC_SENDERHOSTED_OFF, 0),
                            1);
    if (tag == CAPDESC_TAG_THIRDPARTYHOSTED) {
      if (capnp_getp(&cap, 0, &tpcd) != CAPNP_OK)
        memset(&tpcd, 0, sizeof tpcd);
      return export_release(c, capnp_get_u32(&tpcd, TPCD_VINEID_OFF, 0), 1);
    }
    return CAPNP_OK;
  }
  snprintf(reason, sizeof reason,
           "Peer did not implement required RPC message type.; "
           "(uint)message.which() = %u",
           (unsigned)which);
  return protocol_abort(c, reason);
}

int capnp_rpc_handle(capnp_rpc_conn_t *c, const uint8_t *data, size_t len)
{
  capnp_message_t m = {0};
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
  case CAPNP_RPC_MSG_UNIMPLEMENTED:
    rc = handle_unimplemented(c, &body);
    break;
  case CAPNP_RPC_MSG_ABORT:
    rc = handle_abort(c, &body);
    break;
  case CAPNP_RPC_MSG_BOOTSTRAP:
    rc = handle_bootstrap(c, &body);
    break;
  case CAPNP_RPC_MSG_CALL:
    rc = handle_call(c, &body);
    break;
  case CAPNP_RPC_MSG_FINISH:
    /* The caller is done with the answer, so the results it might have
     * pipelined against can go. */
    {
      uint32_t qid = capnp_get_u32(&body, QUESTIONID_OFF, 0);
      capnp_rpc_answer_t *a = answer_find(c, qid);
      /* Redirected results nobody took: rpc.c++ answers the call as it
       * tears it down. The call's params go with it, and only then are
       * the results' exports released, which aborts on one that is not
       * there. */
      if (a != NULL && a->used == SLOT_MARKED)
        rc = send_results_sent_elsewhere(c, qid);
      if (rc == CAPNP_OK)
        rc = holds_finish(c, qid);
      if (rc == CAPNP_OK && !capnp_get_bool(&body, FINISH_RELEASE_CAPS_BIT, 0))
        rc = answer_release_caps(c, qid);
      answer_drop(c, qid);
    }
    break;
  case CAPNP_RPC_MSG_RELEASE:
    rc = handle_release(c, &body);
    break;
  case CAPNP_RPC_MSG_JOIN:
    rc = handle_join(c, &body);
    break;
  case CAPNP_RPC_MSG_DISEMBARGO:
    rc = handle_disembargo(c, &body);
    break;
  case CAPNP_RPC_MSG_RETURN:
    rc = handle_return(c, &body, data, len);
    break;
  case CAPNP_RPC_MSG_PROVIDE:
    rc = handle_provide(c, &body);
    break;
  case CAPNP_RPC_MSG_ACCEPT:
    rc = handle_accept(c, &body);
    break;
  case CAPNP_RPC_MSG_RESOLVE:
    rc = handle_resolve(c, &body);
    break;
  default:
    /* The obsolete save/delete messages. */
    rc = send_unimplemented(c, &root);
    break;
  }

  capnp_message_free(&m);
  return rc;
}
