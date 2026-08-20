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

/* Mirrors the id in schema/adder.capnp. */
#define ADDER_IFACE 0xea01e10cbc414411ULL

static int g_fd = -1;

static int send_fd(void *ctx, const uint8_t *data, size_t len)
{
	size_t off = 0;
	(void)ctx;
	while (off < len) {
		ssize_t n = write(g_fd, data + off, len - off);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static ssize_t read_all(int fd, void *p, size_t count)
{
	size_t off = 0;
	while (off < count) {
		ssize_t n = read(fd, (char *)p + off, count - off);
		if (n <= 0)
			return n == 0 && off > 0 ? (ssize_t)off : n;
		off += (size_t)n;
	}
	return (ssize_t)off;
}

/* Read one stream-framed message off the socket into `buf`. The segment
 * table says how much body follows, so the frame boundary is recoverable
 * from the stream without a length prefix of our own. */
static ssize_t read_frame(int fd, uint8_t *buf, size_t cap)
{
	uint32_t nsegs, i, table_bytes, total_words = 0;
	if (read_all(fd, buf, 4) != 4)
		return -1;
	memcpy(&nsegs, buf, 4);
	nsegs += 1;
	if (nsegs > 64)
		return -1;
	table_bytes = 4 + nsegs * 4;
	if (table_bytes % 8)
		table_bytes += 4;
	if (table_bytes > cap)
		return -1;
	if (read_all(fd, buf + 4, table_bytes - 4) != (ssize_t)(table_bytes - 4))
		return -1;
	for (i = 0; i < nsegs; i++) {
		uint32_t w;
		memcpy(&w, buf + 4 + i * 4, 4);
		total_words += w;
	}
	if (table_bytes + total_words * 8 > cap)
		return -1;
	if (read_all(fd, buf + table_bytes, total_words * 8) !=
	    (ssize_t)(total_words * 8))
		return -1;
	return (ssize_t)(table_bytes + total_words * 8);
}

static void fill_add(void *ctx, const capnp_bptr_t *params)
{
	const int64_t *ab = (const int64_t *)ctx;
	capnp_builder_set_u64(params, 0, (uint64_t)ab[0]);
	capnp_builder_set_u64(params, 8, (uint64_t)ab[1]);
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
		capnp_message_t m;
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

	close(g_fd);
	printf("All rpc interop assertions passed.\n");
	return 0;
}
