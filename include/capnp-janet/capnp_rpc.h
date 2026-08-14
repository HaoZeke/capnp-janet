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

typedef struct capnp_rpc_conn {
  capnp_rpc_export_t exports[CAPNP_RPC_MAX_EXPORTS];
  capnp_rpc_answer_t answers[CAPNP_RPC_MAX_ANSWERS];
  capnp_rpc_question_t questions[CAPNP_RPC_MAX_QUESTIONS];
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
