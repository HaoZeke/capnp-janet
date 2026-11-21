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

static int send_fd(void *ctx, const uint8_t *data, size_t len)
{
	size_t off = 0;
	(void)ctx;
	g_sends++;
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

	/* An Unimplemented reply ends the connection for capnp-C++, and
	 * echoing it back would make the peer abort. */
	{
		ssize_t n;
		int sends, rc;
		int64_t sum = 0;
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
		rc = capnp_rpc_handle(&conn, buf, (size_t)n);
		if (rc != CAPNP_ERR_KIND || g_sends != sends) {
			fprintf(stderr, "Unimplemented: rc %d, %d message(s) sent\n",
			        rc, g_sends - sends);
			return 1;
		}
		if (call_add(&conn, buf, sizeof buf, 1, 2, &sum) || sum != 3) {
			fprintf(stderr, "capnp-C++ dropped the connection\n");
			return 1;
		}
	}

	capnp_rpc_free(&conn);
	close(g_fd);
	printf("All rpc interop assertions passed.\n");
	return 0;
}
