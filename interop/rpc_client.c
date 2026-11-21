/* Live-peer RPC interop: this vat against capnp-C++.
 *
 * Bootstraps the Adder served by interop/rpc_peer_server.c++ over
 * rpc-twoparty on 127.0.0.1:<argv[1]> and calls add(), which proves
 * protocol-level compatibility with the reference implementation rather
 * than only wire-format byte equality against our own encoders.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_rpc.h>
#include <capnp-janet/capnp_transport.h>

/* Mirrors the id in schema/adder.capnp. */
#define ADDER_IFACE 0xea01e10cbc414411ULL

static int g_fd = -1;
static int g_sends;
/* With `g_keep_last` set, a copy of the last frame sent, for the caller
 * to free. */
static int g_keep_last;
static uint8_t *g_last;
static size_t g_last_len;

static int send_fd(void *ctx, const uint8_t *data, size_t len)
{
	size_t off = 0;
	(void)ctx;
	g_sends++;
	if (g_keep_last) {
		free(g_last);
		g_last = (uint8_t *)malloc(len);
		if (g_last != NULL)
			memcpy(g_last, data, len);
		g_last_len = len;
	}
	while (off < len) {
		ssize_t n = write(g_fd, data + off, len - off);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

/* Read one stream-framed message. Segment counts are little-endian on
 * the wire; a host-endian load of those bytes rejects a real reply on
 * a big-endian machine. */
static ssize_t read_frame(int fd, uint8_t *buf, size_t cap)
{
	size_t n = 0;
	if (capnp_read_frame(fd, buf, cap, &n) != CAPNP_OK)
		return -1;
	return (ssize_t)n;
}

static int fill_add(void *ctx, const capnp_bptr_t *params)
{
	const int64_t *ab = (const int64_t *)ctx;
	capnp_builder_set_u64(params, 0, (uint64_t)ab[0]);
	capnp_builder_set_u64(params, 8, (uint64_t)ab[1]);
	return 0;
}

/* Message.which 7 is the obsolete save, which capnp-C++ answers with
 * Unimplemented. */
static int send_save(void)
{
	capnp_builder_t b;
	capnp_bptr_t root, msg;
	uint8_t *out = NULL;
	size_t len = 0;
	int rc;
	capnp_builder_init(&b);
	rc = capnp_builder_root(&b, &root) ||
	     capnp_builder_struct(&root, 1, 1, &msg) ||
	     capnp_builder_set_u16(&msg, 0, 7) ||
	     capnp_builder_serialize(&b, &out, &len) || send_fd(NULL, out, len);
	free(out);
	capnp_builder_free(&b);
	return rc;
}

static int fill_u32(void *ctx, const capnp_bptr_t *params)
{
	return capnp_builder_set_u32(params, 0, *(const uint32_t *)ctx);
}

/* Handle frames until `q` is answered; a Resolve may come first. */
static int wait_answer(capnp_rpc_conn_t *conn, uint32_t q, uint8_t *buf,
		       size_t cap)
{
	while (!capnp_rpc_is_answered(conn, q)) {
		ssize_t n = read_frame(g_fd, buf, cap);
		if (n <= 0 || capnp_rpc_handle(conn, buf, (size_t)n) != 0)
			return -1;
	}
	return capnp_rpc_is_failed(conn, q) ? -1 : 0;
}

static int call_add(capnp_rpc_conn_t *conn, uint8_t *buf, size_t cap,
		    int64_t a, int64_t b, int64_t *sum)
{
	int64_t ab[2];
	uint32_t q;
	capnp_message_t m = {0};
	capnp_ptr_t content;
	ssize_t n;
	ab[0] = a;
	ab[1] = b;
	q = capnp_rpc_send_call(conn, 0, ADDER_IFACE, 0, 2, 0, fill_add, ab);
	if (q == (uint32_t)-1)
		return -1;
	n = read_frame(g_fd, buf, cap);
	if (n <= 0 || capnp_rpc_handle(conn, buf, (size_t)n) != 0 ||
	    capnp_rpc_answer_content(conn, q, &m, &content) != CAPNP_OK)
		return -1;
	*sum = (int64_t)capnp_get_u64(&content, 0, 0);
	capnp_message_free(&m);
	return 0;
}

/* self() returns the Adder itself. The add() is addressed to that
 * result before the self() Return has arrived, so capnp-C++ resolves
 * the promised answer by walking pointer 0 of the results. */
static int check_pipeline(capnp_rpc_conn_t *conn, uint8_t *buf, size_t cap)
{
	int64_t ab[2] = {30, 12};
	uint16_t ops[1] = {0};
	capnp_rpc_call_t call;
	capnp_message_t m = {0};
	capnp_ptr_t content;
	uint32_t qself, qadd;
	int64_t sum;
	qself = capnp_rpc_send_call(conn, 0, ADDER_IFACE, 2, 0, 0, NULL, NULL);
	memset(&call, 0, sizeof call);
	call.promised = 1;
	call.target = qself;
	call.transform = ops;
	call.transform_len = 1;
	call.interface_id = ADDER_IFACE;
	call.method_id = 0;
	call.params_dwords = 2;
	call.fill = fill_add;
	call.fill_ctx = ab;
	qadd = capnp_rpc_send_call_ex(conn, &call);
	if (qself == (uint32_t)-1 || qadd == (uint32_t)-1) {
		fprintf(stderr, "pipelined call not sent\n");
		return 1;
	}
	if (wait_answer(conn, qadd, buf, cap) ||
	    capnp_rpc_answer_content(conn, qadd, &m, &content) != CAPNP_OK) {
		fprintf(stderr, "pipelined add() failed\n");
		return 1;
	}
	sum = (int64_t)capnp_get_u64(&content, 0, 0);
	capnp_message_free(&m);
	if (sum != 42) {
		fprintf(stderr, "pipelined add(30, 12) returned %lld\n",
		        (long long)sum);
		return 1;
	}
	{
		capnp_ptr_t adder;
		int got = 0;
		if (wait_answer(conn, qself, buf, cap) == 0 &&
		    capnp_rpc_answer_content(conn, qself, &m, &content) == CAPNP_OK) {
			got = capnp_getp(&content, 0, &adder) == CAPNP_OK &&
			      adder.kind == CAPNP_PK_CAP;
			capnp_message_free(&m);
		}
		if (!got) {
			fprintf(stderr, "self() returned no capability\n");
			return 1;
		}
	}
	capnp_rpc_send_finish(conn, qadd);
	capnp_rpc_send_finish(conn, qself);
	return 0;
}

/* This vat's own Adder adds 1000, so a sum shows whose add() ran. */
static int local_add(void *server, uint64_t iid, uint16_t mid,
		     const capnp_ptr_t *params, const capnp_bptr_t *results)
{
	capnp_bptr_t body;
	(void)server;
	if (iid != ADDER_IFACE || (mid != 0 && mid != 4) ||
	    capnp_builder_struct(results, 1, 0, &body))
		return 1;
	return capnp_builder_set_u64(&body, 0,
	                             capnp_get_u64(params, 0, 0) +
	                                 capnp_get_u64(params, 8, 0) + 1000);
}

/* self() answers with this export, or with the peer's own bootstrap
 * when the server is `&g_hand_back`. */
static int g_self_eid = -1, g_hand_back;

static int local_self(void *server, uint64_t iid, uint16_t mid,
		      const capnp_ptr_t *params, const capnp_bptr_t *results)
{
	capnp_rpc_cap_ref_t ref;
	capnp_bptr_t body;
	int idx;
	if (iid != ADDER_IFACE || mid != 2)
		return local_add(server, iid, mid, params, results);
	ref.kind = server == &g_hand_back ? CAPNP_RPC_CAP_IMPORT
	                                  : CAPNP_RPC_CAP_EXPORT;
	ref.id = server == &g_hand_back ? 0 : (uint32_t)g_self_eid;
	idx = capnp_rpc_result_cap(&ref);
	if (idx < 0 || capnp_builder_struct(results, 0, 1, &body))
		return 1;
	return capnp_builder_set_cap(&body, 0, 0, (uint32_t)idx);
}

static int fill_cb(void *ctx, const capnp_bptr_t *params)
{
	(void)ctx;
	return capnp_builder_set_cap(params, 0, 0, 0);
}

/* callBack(cb) calls cb.add(20, 22). Passed one of our exports, the
 * peer calls back into this vat; passed its own bootstrap as
 * receiverHosted, it calls its own Adder. */
static int check_cap_params(capnp_rpc_conn_t *conn, uint8_t *buf, size_t cap)
{
	static int self;
	capnp_rpc_cap_ref_t ref;
	capnp_rpc_call_t call;
	int pass, eid = capnp_rpc_export(conn, &self, local_add);
	if (eid < 0)
		return 1;
	for (pass = 0; pass < 2; pass++) {
		int64_t want = pass == 0 ? 1042 : 42, sum;
		capnp_message_t m = {0};
		capnp_ptr_t content;
		uint32_t q;
		ref.kind = pass == 0 ? CAPNP_RPC_CAP_EXPORT : CAPNP_RPC_CAP_IMPORT;
		ref.id = pass == 0 ? (uint32_t)eid : 0;
		memset(&call, 0, sizeof call);
		call.interface_id = ADDER_IFACE;
		call.method_id = 3;
		call.params_pwords = 1;
		call.fill = fill_cb;
		call.caps = &ref;
		call.ncaps = 1;
		q = capnp_rpc_send_call_ex(conn, &call);
		if (q == (uint32_t)-1 || wait_answer(conn, q, buf, cap) ||
		    capnp_rpc_answer_content(conn, q, &m, &content) != CAPNP_OK) {
			fprintf(stderr, "callBack() pass %d failed\n", pass);
			return 1;
		}
		sum = (int64_t)capnp_get_u64(&content, 0, 0);
		capnp_message_free(&m);
		if (sum != want) {
			fprintf(stderr, "callBack() pass %d returned %lld, want %lld\n",
			        pass, (long long)sum, (long long)want);
			return 1;
		}
		capnp_rpc_send_finish(conn, q);
	}
	return 0;
}

/* Whether the question table still has `q`. */
static int question_held(const capnp_rpc_conn_t *conn, uint32_t q)
{
	int i;
	for (i = 0; i < CAPNP_RPC_MAX_QUESTIONS; i++)
		if (conn->questions[i].used && conn->questions[i].question_id == q)
			return 1;
	return 0;
}

/* Handle one frame, noting the union tag of a Return in `tags`. */
static int handle_noting(capnp_rpc_conn_t *conn, uint8_t *buf, size_t cap,
			 unsigned *tags)
{
	capnp_message_t m = {0};
	capnp_ptr_t root, ret;
	ssize_t n = read_frame(g_fd, buf, cap);
	if (n <= 0)
		return -1;
	if (capnp_message_from_flat(&m, buf, (size_t)n) == CAPNP_OK &&
	    capnp_root(&m, &root) == CAPNP_OK &&
	    capnp_get_u16(&root, 0, 0) == CAPNP_RPC_MSG_RETURN &&
	    capnp_getp(&root, 0, &ret) == CAPNP_OK)
		*tags |= 1u << capnp_get_u16(&ret, 6, 0);
	capnp_message_free(&m);
	return capnp_rpc_handle(conn, buf, (size_t)n) != 0 ? -1 : 0;
}

/* Like wait_answer, also noting the union tag of each Return seen. */
static int wait_noting(capnp_rpc_conn_t *conn, uint32_t q, uint8_t *buf,
		       size_t cap, unsigned *tags)
{
	while (!capnp_rpc_is_answered(conn, q))
		if (handle_noting(conn, buf, cap, tags))
			return -1;
	return capnp_rpc_is_failed(conn, q) ? -1 : 0;
}

static int fill_relay(void *ctx, const capnp_bptr_t *params)
{
	const int64_t *ab = (const int64_t *)ctx;
	return capnp_builder_set_u64(params, 0, (uint64_t)ab[0]) ||
	       capnp_builder_set_u64(params, 8, (uint64_t)ab[1]) ||
	       capnp_builder_set_cap(params, 2, 0, 0);
}

/* Tail calls. An add() asking for sendResultsTo.yourself comes back as
 * resultsSentElsewhere, which the vat takes quietly. relay(target) tail-calls target.relay(), and
 * with this vat's export as target capnp-C++ sends that call here with
 * sendResultsTo.yourself, then answers ours with takeFromOtherQuestion. */
/* callSelf(cb) calls cb.self().add(1, 2). Our self() returns its own
 * export, which capnp-C++ calls back; or the peer's bootstrap handed
 * back, which it calls locally. */
static int check_result_caps(capnp_rpc_conn_t *conn, uint8_t *buf,
                             size_t cap)
{
	static int self;
	capnp_rpc_cap_ref_t ref;
	capnp_rpc_call_t call;
	int pass;
	g_self_eid = capnp_rpc_export(conn, &self, local_self);
	ref.kind = CAPNP_RPC_CAP_EXPORT;
	for (pass = 0; pass < 2; pass++) {
		int64_t want = pass == 0 ? 1003 : 3, sum;
		capnp_message_t m = {0};
		capnp_ptr_t content;
		int eid = pass == 0 ? g_self_eid
		                    : capnp_rpc_export(conn, &g_hand_back, local_self);
		uint32_t q;
		if (eid < 0)
			return 1;
		ref.id = (uint32_t)eid;
		memset(&call, 0, sizeof call);
		call.interface_id = ADDER_IFACE;
		call.method_id = 6;
		call.params_pwords = 1;
		call.fill = fill_cb;
		call.caps = &ref;
		call.ncaps = 1;
		q = capnp_rpc_send_call_ex(conn, &call);
		if (q == (uint32_t)-1 || wait_answer(conn, q, buf, cap) ||
		    capnp_rpc_answer_content(conn, q, &m, &content) != CAPNP_OK) {
			fprintf(stderr, "callSelf() pass %d failed\n", pass);
			return 1;
		}
		sum = (int64_t)capnp_get_u64(&content, 0, 0);
		capnp_message_free(&m);
		if (sum != want) {
			fprintf(stderr, "callSelf() pass %d returned %lld, want %lld\n",
			        pass, (long long)sum, (long long)want);
			return 1;
		}
		capnp_rpc_send_finish(conn, q);
	}
	return 0;
}

/* relaySelf(target) tail-calls target.self() back into this vat, and
 * capnp-C++ answers with takeFromOtherQuestion. The capability taken
 * must name the side it lives on: passing it to callBack() reaches our
 * add() for our export, and the peer's own add() for its bootstrap. */
static int check_take_caps(capnp_rpc_conn_t *conn, uint8_t *buf, size_t cap)
{
	capnp_rpc_cap_ref_t ref, got;
	capnp_rpc_call_t call;
	int pass;
	for (pass = 0; pass < 2; pass++) {
		int64_t want = pass == 0 ? 1042 : 42, sum;
		capnp_message_t m = {0};
		capnp_ptr_t content, adder;
		int eid = pass == 0 ? g_self_eid
		                    : capnp_rpc_export(conn, &g_hand_back, local_self);
		unsigned tags = 0;
		uint32_t q, cq;
		if (eid < 0)
			return 1;
		ref.kind = CAPNP_RPC_CAP_EXPORT;
		ref.id = (uint32_t)eid;
		memset(&call, 0, sizeof call);
		call.interface_id = ADDER_IFACE;
		call.method_id = 7;
		call.params_pwords = 1;
		call.fill = fill_cb;
		call.caps = &ref;
		call.ncaps = 1;
		q = capnp_rpc_send_call_ex(conn, &call);
		if (q == (uint32_t)-1 || wait_noting(conn, q, buf, cap, &tags) ||
		    !(tags & (1u << 4)) ||
		    capnp_rpc_answer_content(conn, q, &m, &content) != CAPNP_OK ||
		    capnp_getp(&content, 0, &adder) != CAPNP_OK ||
		    adder.kind != CAPNP_PK_CAP ||
		    capnp_rpc_answer_cap(conn, q, adder.count, &got) != 0) {
			capnp_message_free(&m);
			fprintf(stderr, "relaySelf() pass %d: tags %#x\n", pass, tags);
			return 1;
		}
		capnp_message_free(&m);
		if (pass == 0 ? got.kind != CAPNP_RPC_CAP_EXPORT ||
		                    got.id != (uint32_t)eid
		              : got.kind != CAPNP_RPC_CAP_IMPORT || got.id != 0) {
			fprintf(stderr, "relaySelf() pass %d took kind %d id %u\n",
			        pass, got.kind, got.id);
			return 1;
		}
		memset(&call, 0, sizeof call);
		call.interface_id = ADDER_IFACE;
		call.method_id = 3;
		call.params_pwords = 1;
		call.fill = fill_cb;
		call.caps = &got;
		call.ncaps = 1;
		cq = capnp_rpc_send_call_ex(conn, &call);
		if (cq == (uint32_t)-1 || wait_answer(conn, cq, buf, cap) ||
		    capnp_rpc_answer_content(conn, cq, &m, &content) != CAPNP_OK) {
			fprintf(stderr, "callBack(taken) pass %d failed\n", pass);
			return 1;
		}
		sum = (int64_t)capnp_get_u64(&content, 0, 0);
		capnp_message_free(&m);
		if (sum != want) {
			fprintf(stderr, "callBack(taken) pass %d returned %lld\n", pass,
			        (long long)sum);
			return 1;
		}
		capnp_rpc_send_finish(conn, cq);
		capnp_rpc_send_finish(conn, q);
	}
	return 0;
}

static int check_tail(capnp_rpc_conn_t *conn, uint8_t *buf, size_t cap)
{
	static int self;
	int64_t ab[2] = {5, 6};
	capnp_rpc_cap_ref_t ref;
	capnp_rpc_call_t call;
	capnp_message_t m = {0};
	capnp_ptr_t content;
	unsigned tags = 0;
	uint32_t q, tq;
	int64_t sum;
	int eid = capnp_rpc_export(conn, &self, local_add);

	memset(&call, 0, sizeof call);
	call.interface_id = ADDER_IFACE;
	call.params_dwords = 2;
	call.fill = fill_add;
	call.fill_ctx = ab;
	call.results_to_yourself = 1;
	tq = capnp_rpc_send_call_ex(conn, &call);
	/* capnp-C++ sends resultsSentElsewhere once the caller has sent
	 * Finish, so a bootstrap call afterwards waits behind it. Until then
	 * the finished question stays in the table. */
	if (tq == (uint32_t)-1 || capnp_rpc_send_finish(conn, tq) ||
	    !question_held(conn, tq)) {
		fprintf(stderr, "add() to yourself not sent\n");
		return 1;
	}
	q = capnp_rpc_send_call(conn, 0, ADDER_IFACE, 0, 2, 0, fill_add, ab);
	if (q == (uint32_t)-1 || wait_noting(conn, q, buf, cap, &tags) ||
	    !(tags & (1u << 3)) || question_held(conn, tq)) {
		fprintf(stderr, "add() to yourself: tags %#x\n", tags);
		return 1;
	}
	capnp_rpc_send_finish(conn, q);

	if (eid < 0)
		return 1;
	ref.kind = CAPNP_RPC_CAP_EXPORT;
	ref.id = (uint32_t)eid;
	memset(&call, 0, sizeof call);
	call.interface_id = ADDER_IFACE;
	call.method_id = 4;
	call.params_dwords = 2;
	call.params_pwords = 1;
	call.fill = fill_relay;
	call.fill_ctx = ab;
	call.caps = &ref;
	call.ncaps = 1;
	tags = 0;
	q = capnp_rpc_send_call_ex(conn, &call);
	if (q == (uint32_t)-1 || wait_noting(conn, q, buf, cap, &tags) ||
	    !(tags & (1u << 4)) ||
	    capnp_rpc_answer_content(conn, q, &m, &content) != CAPNP_OK) {
		fprintf(stderr, "relay() failed: tags %#x\n", tags);
		return 1;
	}
	sum = (int64_t)capnp_get_u64(&content, 0, 0);
	capnp_message_free(&m);
	if (sum != 1011) {
		fprintf(stderr, "relay(5, 6) returned %lld\n", (long long)sum);
		return 1;
	}
	capnp_rpc_send_finish(conn, q);
	/* The Finish and Release for the forwarded call follow; a round
	 * trip takes them. */
	q = capnp_rpc_send_call(conn, 0, ADDER_IFACE, 0, 2, 0, fill_add, ab);
	if (q == (uint32_t)-1 || wait_answer(conn, q, buf, cap)) {
		fprintf(stderr, "add() after relay() failed\n");
		return 1;
	}
	capnp_rpc_send_finish(conn, q);
	return 0;
}

/* hang() never returns. After Finish capnp-C++ cancels it and answers
 * canceled. The vat keeps the finished question until that Return, as
 * rpc.c++ does, and the Return then closes it. That Return may come
 * after the answer to a later call. */
static int check_cancel(capnp_rpc_conn_t *conn, uint8_t *buf, size_t cap)
{
	int64_t ab[2] = {1, 2};
	unsigned tags = 0;
	uint32_t hq = capnp_rpc_send_call(conn, 0, ADDER_IFACE, 5, 0, 0, NULL,
	                                  NULL), q;
	if (hq == (uint32_t)-1 || capnp_rpc_send_finish(conn, hq)) {
		fprintf(stderr, "hang() not sent\n");
		return 1;
	}
	if (!question_held(conn, hq)) {
		fprintf(stderr, "hang() dropped before its Return\n");
		return 1;
	}
	q = capnp_rpc_send_call(conn, 0, ADDER_IFACE, 0, 2, 0, fill_add, ab);
	if (q == (uint32_t)-1 || wait_noting(conn, q, buf, cap, &tags)) {
		fprintf(stderr, "add() after hang() failed\n");
		return 1;
	}
	capnp_rpc_send_finish(conn, q);
	while (!(tags & (1u << 2)))
		if (handle_noting(conn, buf, cap, &tags)) {
			fprintf(stderr, "hang() after Finish: tags %#x\n", tags);
			return 1;
		}
	if (question_held(conn, hq)) {
		fprintf(stderr, "hang() kept after its Return\n");
		return 1;
	}
	return 0;
}

/* The reason of an Abort frame, copied into `reason`; else -1. */
static int abort_reason(const uint8_t *frame, size_t n, char *reason,
			size_t cap)
{
	capnp_message_t m = {0};
	capnp_ptr_t root, exc;
	const char *text = NULL;
	size_t tn = 0;
	int rc = -1;
	if (capnp_message_from_flat(&m, frame, n) == CAPNP_OK &&
	    capnp_root(&m, &root) == CAPNP_OK &&
	    capnp_get_u16(&root, 0, 0) == CAPNP_RPC_MSG_ABORT &&
	    capnp_getp(&root, 0, &exc) == CAPNP_OK &&
	    capnp_get_text(&exc, 0, &text, &tn) == CAPNP_OK && tn < cap) {
		memcpy(reason, text, tn);
		reason[tn] = 0;
		rc = 0;
	}
	capnp_message_free(&m);
	return rc;
}

/* A Return with no content naming `answer`, with union tag `tag`. */
static int bare_return(uint32_t answer, uint16_t tag, uint8_t **out,
		       size_t *len)
{
	capnp_builder_t b;
	capnp_bptr_t root, msg, slot, ret;
	int rc;
	capnp_builder_init(&b);
	rc = capnp_builder_root(&b, &root) ||
	     capnp_builder_struct(&root, 1, 1, &msg) ||
	     capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RETURN) ||
	     capnp_builder_slot(&msg, 1, 0, &slot) ||
	     capnp_builder_struct(&slot, 2, 1, &ret) ||
	     capnp_builder_set_u32(&ret, 0, answer) ||
	     capnp_builder_set_u16(&ret, 6, tag) ||
	     capnp_builder_serialize(&b, out, len);
	capnp_builder_free(&b);
	return rc;
}

static char g_local_abort[256];

static int keep_abort(void *ctx, const uint8_t *data, size_t len)
{
	(void)ctx;
	if (abort_reason(data, len, g_local_abort, sizeof g_local_abort))
		g_local_abort[0] = 0;
	return 0;
}

/* relay() makes capnp-C++ send this vat a call with
 * sendResultsTo.yourself. Answering it with `tag` (results or an
 * exception) breaks the tail call, and the Abort capnp-C++ sends must
 * give the reason this vat gives for the same Return. */
static int check_tail_refusal(const struct sockaddr_in *addr, uint16_t tag)
{
	static capnp_rpc_conn_t c, local;
	static int self;
	int64_t ab[2] = {5, 6};
	char cpp[256] = "";
	capnp_rpc_cap_ref_t ref;
	capnp_rpc_call_t call;
	uint8_t *out = NULL;
	size_t len = 0;
	uint8_t buf[16384];
	uint32_t q;
	int saved = g_fd, rc = 1;

	g_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_fd < 0 ||
	    connect(g_fd, (const struct sockaddr *)addr, sizeof *addr) != 0)
		goto done;
	capnp_rpc_init(&c, send_fd, NULL);
	q = capnp_rpc_send_bootstrap(&c);
	if (q == (uint32_t)-1 || wait_answer(&c, q, buf, sizeof buf))
		goto done;
	ref.kind = CAPNP_RPC_CAP_EXPORT;
	ref.id = (uint32_t)capnp_rpc_export(&c, &self, local_add);
	memset(&call, 0, sizeof call);
	call.interface_id = ADDER_IFACE;
	call.method_id = 4;
	call.params_dwords = 2;
	call.params_pwords = 1;
	call.fill = fill_relay;
	call.fill_ctx = ab;
	call.caps = &ref;
	call.ncaps = 1;
	if (capnp_rpc_send_call_ex(&c, &call) == (uint32_t)-1)
		goto done;
	for (;;) {
		capnp_message_t m = {0};
		capnp_ptr_t root, body;
		int mtag = -1, yourself = 0;
		uint32_t qid = 0;
		ssize_t n = read_frame(g_fd, buf, sizeof buf);
		if (n <= 0)
			goto done;
		if (capnp_message_from_flat(&m, buf, (size_t)n) == CAPNP_OK &&
		    capnp_root(&m, &root) == CAPNP_OK &&
		    capnp_getp(&root, 0, &body) == CAPNP_OK) {
			mtag = capnp_get_u16(&root, 0, 0);
			yourself = mtag == CAPNP_RPC_MSG_CALL &&
			           capnp_get_u16(&body, 6, 0) == 1;
			qid = capnp_get_u32(&body, 0, 0);
		}
		capnp_message_free(&m);
		if (mtag == CAPNP_RPC_MSG_ABORT) {
			abort_reason(buf, (size_t)n, cpp, sizeof cpp);
			break;
		}
		if (yourself) {
			int bad = bare_return(qid, tag, &out, &len) ||
			          send_fd(NULL, out, len);
			free(out);
			out = NULL;
			if (bad)
				goto done;
		} else {
			capnp_rpc_handle(&c, buf, (size_t)n);
		}
	}

	capnp_rpc_init(&local, keep_abort, NULL);
	memset(&call, 0, sizeof call);
	call.interface_id = ADDER_IFACE;
	call.results_to_yourself = 1;
	q = capnp_rpc_send_call_ex(&local, &call);
	g_local_abort[0] = 0;
	if (q == (uint32_t)-1 || bare_return(q, tag, &out, &len))
		goto done;
	capnp_rpc_handle(&local, out, len);
	if (cpp[0] == 0 || strcmp(cpp, g_local_abort) != 0) {
		fprintf(stderr, "tail Return %u: capnp-C++ \"%s\", this vat \"%s\"\n",
		        tag, cpp, g_local_abort);
		goto done;
	}
	printf("same tail Return %u: %s\n", tag, cpp);
	rc = 0;
done:
	free(out);
	capnp_rpc_free(&c);
	capnp_rpc_free(&local);
	if (g_fd >= 0)
		close(g_fd);
	g_fd = saved;
	return rc;
}

/* The bootstrap of `rpc_peer_server PORT promise` resolves later, so
 * capnp-C++ describes it as senderPromise. Its export id is still the
 * one to call; the peer queues the call until the promise resolves. */
static int check_promise(capnp_rpc_conn_t *conn, uint32_t qboot,
			 uint8_t *buf, size_t cap)
{
	capnp_message_t m = {0};
	capnp_ptr_t content, root, ret, payload, table, desc;
	int64_t args[2] = {7, 8};
	uint32_t q;
	int id, tag = -1;
	if (capnp_rpc_answer_content(conn, qboot, &m, &content) != CAPNP_OK)
		return 1;
	if (content.kind == CAPNP_PK_CAP && capnp_root(&m, &root) == CAPNP_OK &&
	    capnp_getp(&root, 0, &ret) == CAPNP_OK &&
	    capnp_getp(&ret, 0, &payload) == CAPNP_OK &&
	    capnp_getp(&payload, 1, &table) == CAPNP_OK &&
	    capnp_list_get_struct(&table, content.count, &desc) == CAPNP_OK)
		tag = capnp_get_u16(&desc, 0, 0);
	capnp_message_free(&m);
	if (tag != 2) {
		fprintf(stderr, "bootstrap descriptor tag %d, expected 2\n", tag);
		return 1;
	}
	id = capnp_rpc_answer_cap_id(conn, qboot);
	if (id < 0) {
		fprintf(stderr, "senderPromise bootstrap has no id\n");
		return 1;
	}
	q = capnp_rpc_send_call(conn, (uint32_t)id, ADDER_IFACE, 0, 2, 0,
	                        fill_add, args);
	if (q == (uint32_t)-1 || wait_answer(conn, q, buf, cap) ||
	    capnp_rpc_answer_content(conn, q, &m, &content) != CAPNP_OK) {
		fprintf(stderr, "add() on the promised bootstrap failed\n");
		return 1;
	}
	if (capnp_get_u64(&content, 0, 0) != 15) {
		fprintf(stderr, "add(7, 8) on the promise returned %llu\n",
		        (unsigned long long)capnp_get_u64(&content, 0, 0));
		capnp_message_free(&m);
		return 1;
	}
	capnp_message_free(&m);
	capnp_rpc_free(conn);
	close(g_fd);
	printf("All rpc promise interop assertions passed.\n");
	return 0;
}

int main(int argc, char **argv)
{
	struct sockaddr_in addr;
	capnp_rpc_conn_t conn;
	uint8_t buf[16384];
	uint32_t qboot, qcall;
	int port = argc > 1 ? atoi(argv[1]) : 43117;
	int tries;
	int64_t ab[2];

	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	/* The peer may still be binding when this starts. */
	for (tries = 0; tries < 100; tries++) {
		g_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (g_fd >= 0 &&
		    connect(g_fd, (struct sockaddr *)&addr, sizeof addr) == 0)
			break;
		if (g_fd >= 0)
			close(g_fd);
		g_fd = -1;
		{
			/* usleep left POSIX in 2008; nanosleep is the portable
			 * spelling of "wait a moment and retry". */
			struct timespec ts;
			ts.tv_sec = 0;
			ts.tv_nsec = 50000000L;
			nanosleep(&ts, NULL);
		}
	}
	if (g_fd < 0) {
		fprintf(stderr, "could not connect to 127.0.0.1:%d\n", port);
		return 1;
	}

	capnp_rpc_init(&conn, send_fd, NULL);

	qboot = capnp_rpc_send_bootstrap(&conn);
	if (qboot == (uint32_t)-1) {
		fprintf(stderr, "bootstrap not sent\n");
		return 1;
	}
	{
		ssize_t n = read_frame(g_fd, buf, sizeof buf);
		if (n <= 0 || capnp_rpc_handle(&conn, buf, (size_t)n) != 0) {
			fprintf(stderr, "bootstrap reply not handled\n");
			return 1;
		}
	}
	if (!capnp_rpc_is_answered(&conn, qboot) ||
	    capnp_rpc_is_failed(&conn, qboot)) {
		fprintf(stderr, "capnp-C++ did not answer the bootstrap\n");
		return 1;
	}
	if (argc > 2 && strcmp(argv[2], "promise") == 0)
		return check_promise(&conn, qboot, buf, sizeof buf);

	/* The bootstrap capability lands in the peer's export table; a
	 * two-party server hands out id 0 for it. */
	ab[0] = 20;
	ab[1] = 22;
	qcall = capnp_rpc_send_call(&conn, 0, ADDER_IFACE, 0, 2, 0, fill_add, ab);
	if (qcall == (uint32_t)-1) {
		fprintf(stderr, "call not sent\n");
		return 1;
	}
	{
		ssize_t n = read_frame(g_fd, buf, sizeof buf);
		if (n <= 0 || capnp_rpc_handle(&conn, buf, (size_t)n) != 0) {
			fprintf(stderr, "call reply not handled\n");
			return 1;
		}
	}
	if (!capnp_rpc_is_answered(&conn, qcall)) {
		fprintf(stderr, "capnp-C++ did not answer the call\n");
		return 1;
	}
	if (capnp_rpc_is_failed(&conn, qcall)) {
		fprintf(stderr, "capnp-C++ returned an exception for add()\n");
		return 1;
	}
	{
		capnp_message_t m = {0};
		capnp_ptr_t content;
		int64_t sum;
		if (capnp_rpc_answer_content(&conn, qcall, &m, &content) != CAPNP_OK) {
			fprintf(stderr, "no results in the add() answer\n");
			return 1;
		}
		sum = (int64_t)capnp_get_u64(&content, 0, 0);
		capnp_message_free(&m);
		if (sum != 42) {
			fprintf(stderr, "add(20, 22) returned %lld, expected 42\n",
			        (long long)sum);
			return 1;
		}
	}

	/* Finishing an answered bootstrap keeps its capability: the import
	 * is ours now, and only a Release may drop it. */
	{
		int64_t sum = 0;
		if (capnp_rpc_send_finish(&conn, qboot)) {
			fprintf(stderr, "finish not sent\n");
			return 1;
		}
		if (call_add(&conn, buf, sizeof buf, 5, 6, &sum) || sum != 11) {
			fprintf(stderr, "bootstrap capability lost after Finish\n");
			return 1;
		}
	}

	/* A Return longer than CAPNP_RPC_MAX_ANSWER_BYTES is recorded like
	 * any other. */
	{
		uint32_t len = 10000, q;
		capnp_message_t m = {0};
		capnp_ptr_t content;
		const uint8_t *data = NULL;
		size_t n = 0;
		q = capnp_rpc_send_call(&conn, 0, ADDER_IFACE, 1, 1, 0, fill_u32,
		                        &len);
		if (q == (uint32_t)-1 || wait_answer(&conn, q, buf, sizeof buf) ||
		    capnp_rpc_answer_content(&conn, q, &m, &content) != CAPNP_OK) {
			fprintf(stderr, "echo(10000) not answered\n");
			return 1;
		}
		if (capnp_get_data(&content, 0, &data, &n) != CAPNP_OK ||
		    n != len || data[0] != 0xab || data[n - 1] != 0xab) {
			fprintf(stderr, "echo(10000) returned %zu bytes\n", n);
			return 1;
		}
		capnp_message_free(&m);
		capnp_rpc_send_finish(&conn, q);
	}

	if (check_pipeline(&conn, buf, sizeof buf))
		return 1;
	if (check_cap_params(&conn, buf, sizeof buf))
		return 1;
	if (check_result_caps(&conn, buf, sizeof buf))
		return 1;
	if (check_take_caps(&conn, buf, sizeof buf))
		return 1;
	if (check_tail(&conn, buf, sizeof buf))
		return 1;
	if (check_cancel(&conn, buf, sizeof buf))
		return 1;

	/* An Unimplemented reply to anything but Resolve ends the
	 * connection: the vat aborts with the reason rpc.c++ gives, and
	 * capnp-C++ closes. */
	{
		static const char want[] =
		    "Peer did not implement required RPC message type.; "
		    "(uint)message.which() = 7";
		char reason[256] = "";
		ssize_t n;
		int sends, rc;
		uint8_t *sent = NULL;
		size_t sent_len = 0;
		if (send_save()) {
			fprintf(stderr, "save not sent\n");
			return 1;
		}
		n = read_frame(g_fd, buf, sizeof buf);
		if (n <= 0) {
			fprintf(stderr, "no reply to save\n");
			return 1;
		}
		sends = g_sends;
		g_keep_last = 1;
		rc = capnp_rpc_handle(&conn, buf, (size_t)n);
		g_keep_last = 0;
		sent = g_last;
		sent_len = g_last_len;
		g_last = NULL;
		if (rc != CAPNP_ERR_KIND || g_sends != sends + 1 || sent == NULL ||
		    abort_reason(sent, sent_len, reason, sizeof reason) ||
		    strcmp(reason, want) != 0) {
			fprintf(stderr, "Unimplemented: rc %d, %d message(s) sent, "
			        "reason \"%s\"\n", rc, g_sends - sends, reason);
			free(sent);
			return 1;
		}
		free(sent);
		/* capnp-C++ answers an Abort with its own, then closes. */
		{
			char back[256] = "";
			n = read_frame(g_fd, buf, sizeof buf);
			if (n <= 0 ||
			    abort_reason(buf, (size_t)n, back, sizeof back) ||
			    strncmp(back, "remote exception: ", 18) != 0 ||
			    strcmp(back + 18, want) != 0 ||
			    read_frame(g_fd, buf, sizeof buf) > 0) {
				fprintf(stderr, "after Abort: \"%s\"\n", back);
				return 1;
			}
			printf("capnp-C++ answers: %s\n", back);
		}
		printf("Unimplemented aborts: %s\n", reason);
	}

	if (check_tail_refusal(&addr, 0) || check_tail_refusal(&addr, 1))
		return 1;

	capnp_rpc_free(&conn);
	close(g_fd);
	printf("All rpc interop assertions passed.\n");
	return 0;
}
