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
#define CAPNP_RPC_MAX_JOINS 8
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

typedef struct capnp_rpc_export {
  int used;
  int refcount;
  void *server;
  capnp_rpc_dispatch_fn dispatch;
} capnp_rpc_export_t;

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

#ifdef __cplusplus
}
#endif

#endif /* CAPNP_JANET_RPC_H */
