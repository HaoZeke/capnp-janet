/* Two-party Cap'n Proto RPC vat.
 *
 * Answers the level 1 messages a peer sends to a capability this vat
 * hosts, plus level 4 `Join`. Level 3 is absent by construction rather
 * than by omission: `Provide` and `Accept` introduce a capability to a
 * third vat, and a two-party connection has no way to name one --
 * rpc-twoparty.capnp declares `ThirdPartyCapId` and `RecipientId` empty,
 * "never used, because there is no third party".
 *
 * The vat owns no transport. A caller feeds it whole framed messages and
 * receives whole framed messages back through the send callback, so it
 * works the same over a socket or a harness in one process.
 *
 * rpc.capnp is read and written by field offset here rather than through
 * generated code: capnpc-janet emits Janet, and the handful of RPC
 * message shapes are small enough that the offsets stay checkable
 * against the schema comments beside them.
 */

#ifndef CAPNP_JANET_RPC_H
#define CAPNP_JANET_RPC_H

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPNP_RPC_MAX_EXPORTS 64
#define CAPNP_RPC_MAX_ANSWERS 64
#define CAPNP_RPC_MAX_ANSWER_BYTES 8192
#define CAPNP_RPC_MAX_QUESTIONS 64
#define CAPNP_RPC_MAX_PROVISIONS 32
#define CAPNP_RPC_MAX_INTRODUCTIONS 8
/* Long enough for a hostname; a longer one is refused, not truncated. */
#define CAPNP_RPC_MAX_HOST 64
#define CAPNP_RPC_MAX_JOINS 8
/* Outstanding unacknowledged calls a stream may hold. */
#define CAPNP_RPC_STREAM_MAX_WINDOW 64
#define CAPNP_RPC_MAX_JOIN_PARTS 16

/* rpc.capnp Message union tags. */
enum {
  CAPNP_RPC_MSG_UNIMPLEMENTED = 0,
  CAPNP_RPC_MSG_ABORT = 1,
  CAPNP_RPC_MSG_CALL = 2,
  CAPNP_RPC_MSG_RETURN = 3,
  CAPNP_RPC_MSG_FINISH = 4,
  CAPNP_RPC_MSG_RESOLVE = 5,
  CAPNP_RPC_MSG_RELEASE = 6,
  CAPNP_RPC_MSG_BOOTSTRAP = 8,
  CAPNP_RPC_MSG_PROVIDE = 10,
  CAPNP_RPC_MSG_ACCEPT = 11,
  CAPNP_RPC_MSG_JOIN = 12,
  CAPNP_RPC_MSG_DISEMBARGO = 13
};

struct capnp_rpc_conn;

/* Handle one call on an exported capability. Write the reply into
 * `results`; return 0 to answer with it, non-zero for an exception. */
typedef int (*capnp_rpc_dispatch_fn)(void *server, uint64_t interface_id,
                                     uint16_t method_id,
                                     const capnp_ptr_t *params,
                                     const capnp_bptr_t *results);

/* Send one framed message to the peer. Return 0 on success. */
typedef int (*capnp_rpc_send_fn)(void *ctx, const uint8_t *data, size_t len);

/* A Return already sent, kept until the peer sends `Finish`.
 *
 * Promise pipelining is the reason: a caller may address a capability
 * inside an answer before it has seen the answer, so the answer has to
 * still be here when the pipelined call arrives.
 */
typedef struct capnp_rpc_answer {
  int used;
  uint32_t question_id;
  uint8_t frame[CAPNP_RPC_MAX_ANSWER_BYTES];
  size_t len;
} capnp_rpc_answer_t;

typedef struct capnp_rpc_export {
  int used;
  int refcount;
  void *server;
  capnp_rpc_dispatch_fn dispatch;
} capnp_rpc_export_t;

/* A question this vat asked, from send until its Return arrives. */
typedef struct capnp_rpc_question {
  int used;
  uint32_t question_id;
  int answered;
  int failed;
  uint8_t reply[CAPNP_RPC_MAX_ANSWER_BYTES];
  size_t reply_len;
} capnp_rpc_question_t;

/* A capability promised to a third vat, awaiting its Accept.
 *
 * Level 3: the introducer told us to expect someone, and the nonce is
 * the whole of the arrangement. Matching on it alone is what lets the
 * recipient claim the capability without us having to trust her account
 * of who sent her. */
typedef struct capnp_rpc_provision {
  int used;
  uint64_t nonce;
  /* The capability itself, not this connection's id for it: the
   * recipient may well arrive on another connection, where that id means
   * nothing. */
  void *server;
  capnp_rpc_dispatch_fn dispatch;
  int export_id;
  /* The introducer's Provide question, which is how a later Disembargo
   * names this arrangement (rpc.capnp, Disembargo.context.provide). */
  uint32_t question_id;
  /* An embargoed Accept has claimed the capability but must not be
   * answered until the introducer lifts the embargo, so the slot
   * outlives the claim and carries the answer to send. */
  int embargoed;
  uint32_t accept_question_id;
} capnp_rpc_provision_t;

/* An introduction we have been handed but not yet picked up.
 *
 * Level 3: a payload can name a capability hosted by a third vat, as a
 * `thirdPartyHosted` CapDescriptor carrying where to go and a vine. The
 * vine is an ordinary import through the introducer, so calls work
 * before we ever reach the third party; that is the fallback the spec
 * gives level 1 and 2 receivers, and it is why the vine must not be
 * released until the pickup succeeds. Connecting is the network layer's
 * job, so the vat records the introduction and hands it over.
 */
typedef struct capnp_rpc_introduction {
  int used;
  uint64_t nonce;
  uint32_t vine_id;
  uint16_t port;
  char host[CAPNP_RPC_MAX_HOST];
} capnp_rpc_introduction_t;

/* Client-side flow control for `-> stream` methods: a bounded window of
 * unacknowledged stream calls. The wire carries ordinary Call/Return
 * pairs; the window is policy, as in capnp-C++. After any stream call
 * fails, later sends fail immediately and the failure surfaces at finish,
 * which is the streaming error-propagation rule. */
typedef struct capnp_rpc_stream {
  int window;
  int nout;
  uint32_t qids[CAPNP_RPC_STREAM_MAX_WINDOW];
  int failed;
  uint32_t first_failure;
  int have_failure;
} capnp_rpc_stream_t;

/* One in-flight Join, keyed by the sender's joinId.
 *
 * A Join asks whether several capabilities are the same object. Each part
 * is its own question, so no part is answerable when it arrives: the
 * answer depends on the whole set. Parts accumulate here and every
 * question is answered once the last one lands, which is the order
 * rpc-twoparty.capnp's JoinResult describes.
 */
typedef struct capnp_rpc_join {
  int used;
  uint32_t join_id;
  int part_count;
  int nseen;
  int seen[CAPNP_RPC_MAX_JOIN_PARTS];
  uint32_t qids[CAPNP_RPC_MAX_JOIN_PARTS];
  int eids[CAPNP_RPC_MAX_JOIN_PARTS];
} capnp_rpc_join_t;

/* What a vat knows across all its connections.
 *
 * A level 3 handoff is arranged on one connection and claimed on
 * another: the introducer sends `Provide` over its own, and the
 * recipient arrives on hers. Holding the arrangement on the connection
 * would make it claimable only by the introducer, which is no handoff at
 * all. Connections given no vat get one to themselves, which is what a
 * two-party deployment wants.
 */
typedef struct capnp_rpc_vat {
  capnp_rpc_provision_t provisions[CAPNP_RPC_MAX_PROVISIONS];
} capnp_rpc_vat_t;

typedef struct capnp_rpc_conn {
  capnp_rpc_export_t exports[CAPNP_RPC_MAX_EXPORTS];
  capnp_rpc_answer_t answers[CAPNP_RPC_MAX_ANSWERS];
  capnp_rpc_question_t questions[CAPNP_RPC_MAX_QUESTIONS];
  /* Shared with this vat's other connections when one is attached;
   * otherwise `own_vat` below. */
  capnp_rpc_vat_t *vat;
  capnp_rpc_vat_t own_vat;
  capnp_rpc_introduction_t introductions[CAPNP_RPC_MAX_INTRODUCTIONS];
  uint32_t next_question_id;
  capnp_rpc_join_t joins[CAPNP_RPC_MAX_JOINS];
  void *bootstrap;
  capnp_rpc_dispatch_fn bootstrap_dispatch;
  capnp_rpc_send_fn send;
  void *send_ctx;
} capnp_rpc_conn_t;

void capnp_rpc_init(capnp_rpc_conn_t *c, capnp_rpc_send_fn send,
                    void *send_ctx);
void capnp_rpc_set_bootstrap(capnp_rpc_conn_t *c, void *server,
                             capnp_rpc_dispatch_fn dispatch);

/* Export a capability, or return the id it already holds; -1 when full. */
int capnp_rpc_export(capnp_rpc_conn_t *c, void *server,
                     capnp_rpc_dispatch_fn dispatch);

/* Handle one framed message. 0 when handled, non-zero on a bad frame. */
int capnp_rpc_handle(capnp_rpc_conn_t *c, const uint8_t *data, size_t len);

/* --- client side -------------------------------------------------- */

/* Write a call's parameter struct. */
typedef void (*capnp_rpc_fill_fn)(void *ctx, const capnp_bptr_t *params);

/* Ask for the peer's bootstrap capability. Returns the questionId, or
 * (uint32_t)-1 when the question table is full. */
uint32_t capnp_rpc_send_bootstrap(capnp_rpc_conn_t *c);

/* Call a method on an imported capability. `fill` may be NULL.
 *
 * The caller gives the parameter struct's dimensions because only it
 * knows the method signature; a size guessed here silently drops any
 * field past the end. */
uint32_t capnp_rpc_send_call(capnp_rpc_conn_t *c, uint32_t imported_cap,
                             uint64_t interface_id, uint16_t method_id,
                             uint16_t params_dwords, uint16_t params_pwords,
                             capnp_rpc_fill_fn fill, void *fill_ctx);

/* Tell the peer we are done with an answer, and drop our copy. */
int capnp_rpc_send_finish(capnp_rpc_conn_t *c, uint32_t question_id);

/* Drop `count` references to an import. */
int capnp_rpc_send_release(capnp_rpc_conn_t *c, uint32_t import_id,
                           uint32_t count);

int capnp_rpc_is_answered(capnp_rpc_conn_t *c, uint32_t question_id);
int capnp_rpc_is_failed(capnp_rpc_conn_t *c, uint32_t question_id);

/* Results of an answered question. Returns CAPNP_OK and fills `msg_out`
 * / `out` on success. The caller frees `msg_out` with
 * capnp_message_free. */
int capnp_rpc_answer_content(capnp_rpc_conn_t *c, uint32_t question_id,
                             capnp_message_t *msg_out, capnp_ptr_t *out);

/* Nonces of capabilities held for a third vat, for tests and shutdown
 * accounting. Writes up to `cap` entries and returns how many exist. */
int capnp_rpc_pending_provisions(capnp_rpc_conn_t *c, uint64_t *out, int cap);

/* Introductions handed to us and not yet picked up. Copies up to `cap`
 * into `out` (may be NULL) and returns how many are held. */
/* Accepts claimed but still embargoed, awaiting Disembargo.provide. */
int capnp_rpc_embargoed_accepts(capnp_rpc_conn_t *c);

/* Share level 3 arrangements with this vat's other connections. Call
 * after capnp_rpc_init and before any Provide; `vat` must outlive the
 * connection. */
void capnp_rpc_set_vat(capnp_rpc_conn_t *c, capnp_rpc_vat_t *vat);

/* Ask the peer to hold `imported_cap` for a third vat, and return the
 * question id, which is also what a later Disembargo.provide names. The
 * nonce is the whole of the arrangement: the recipient presents it in an
 * Accept, and the host matches on it alone. */
uint32_t capnp_rpc_send_provide(capnp_rpc_conn_t *c, uint32_t imported_cap,
                                const char *recipient_host,
                                uint16_t recipient_port, uint64_t nonce);

/* Claim a capability a third vat provided for us. Returns the question
 * id; the answer carries the capability. */
uint32_t capnp_rpc_send_accept(capnp_rpc_conn_t *c, uint64_t nonce, int embargo);

/* Lift the embargo on the Accept this vat arranged with Provide. */
int capnp_rpc_send_disembargo_provide(capnp_rpc_conn_t *c, uint32_t provide_qid);

int capnp_rpc_pending_introductions(capnp_rpc_conn_t *c,
                                    capnp_rpc_introduction_t *out, int cap);

/* Finish the pickup for `nonce`: releases the vine, which the sender
 * treats as the signal to close its Provide. Call this only once the
 * third party has actually handed the capability over; releasing early
 * drops the fallback path with nothing in its place. Returns 0 on
 * success, -1 if no such introduction is held. */
int capnp_rpc_introduction_done(capnp_rpc_conn_t *c, uint64_t nonce);

/* Write a `thirdPartyHosted` descriptor into the builder `cd`: where the
 * recipient should go, which pending Provide to claim once there, and
 * the vine we export so calls work in the meantime. */
int capnp_rpc_write_third_party_cap(capnp_rpc_conn_t *c, capnp_bptr_t *cd,
                                    const char *host, uint16_t port,
                                    uint64_t nonce, uint32_t vine_id);

/* --- stream flow control ------------------------------------------- */

void capnp_rpc_stream_init(capnp_rpc_stream_t *s, int window);

/* Send one stream call, blocking only when the window is full. */
int capnp_rpc_stream_send(capnp_rpc_conn_t *c, capnp_rpc_stream_t *s,
                          uint32_t imported_cap, uint64_t interface_id,
                          uint16_t method_id, uint16_t params_dwords,
                          uint16_t params_pwords, capnp_rpc_fill_fn fill,
                          void *fill_ctx);

/* Wait for every outstanding call. CAPNP_OK when all succeeded. */
int capnp_rpc_stream_finish(capnp_rpc_conn_t *c, capnp_rpc_stream_t *s);

#ifdef __cplusplus
}
#endif

#endif /* CAPNP_JANET_RPC_H */
