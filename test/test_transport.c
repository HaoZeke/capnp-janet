/* The vat dials and listens on TCP and Unix sockets itself.
 *
 * With no arguments, two vats on a Unix socket and two on TCP loopback
 * call an adder and check the sum. `dial <port>` calls the capnp-C++
 * Adder. `serve <port>` listens and answers one C++ client.
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_transport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ADDER_IFACE 0xea01e10cbc414411ULL

static int g_fail;
static int g_server_obj;

static void check(int cond, const char *what) {
  if (!cond) {
    fprintf(stderr, "FAIL %s\n", what);
    g_fail++;
  }
}

static int adder_dispatch(void *server, uint64_t interface_id, uint16_t method_id,
                          const capnp_ptr_t *params, const capnp_bptr_t *results) {
  capnp_bptr_t body;
  int64_t a, b;
  (void)server;
  (void)interface_id;
  (void)method_id;
  a = (int64_t)capnp_get_u64(params, 0, 0);
  b = (int64_t)capnp_get_u64(params, 8, 0);
  if (capnp_builder_struct(results, 1, 0, &body))
    return CAPNP_ERR_ALLOC;
  return capnp_builder_set_u64(&body, 0, (uint64_t)(a + b));
}

static void fill_add(void *ctx, const capnp_bptr_t *params) {
  (void)ctx;
  capnp_builder_set_u64(params, 0, 20);
  capnp_builder_set_u64(params, 8, 22);
}

static void child_serve(int lfd) {
  capnp_link_t *link = (capnp_link_t *)calloc(1, sizeof(*link));
  int i;
  if (!link || capnp_link_accept(lfd, link))
    _exit(1);
  capnp_rpc_set_bootstrap(&link->conn, &g_server_obj, adder_dispatch);
  for (i = 0; i < 8; i++) {
    if (capnp_link_recv(link))
      break;
  }
  capnp_link_close(link);
  free(link);
  _exit(0);
}

static void client_add(capnp_link_t *link, const char *what) {
  uint32_t qboot, qcall;
  int i;
  qboot = capnp_rpc_send_bootstrap(&link->conn);
  check(qboot != (uint32_t)-1, "bootstrap");
  check(capnp_link_recv(link) == CAPNP_OK, "bootstrap reply");
  check(capnp_rpc_is_answered(&link->conn, qboot), "answered bootstrap");
  {
    int cap = capnp_rpc_answer_cap_id(&link->conn, qboot);
    check(cap >= 0, "bootstrap cap");
    qcall = capnp_rpc_send_call(&link->conn, (uint32_t)cap, ADDER_IFACE, 0, 2, 0,
                                fill_add, NULL);
  }
  check(qcall != (uint32_t)-1, "call");
  check(capnp_link_recv(link) == CAPNP_OK, "call reply");
  check(capnp_rpc_is_answered(&link->conn, qcall) &&
            !capnp_rpc_is_failed(&link->conn, qcall),
        "call ok");
  {
    capnp_message_t m;
    capnp_ptr_t content;
    check(capnp_rpc_answer_content(&link->conn, qcall, &m, &content) == CAPNP_OK,
          "content");
    check((int64_t)capnp_get_u64(&content, 0, 0) == 42, what);
    capnp_message_free(&m);
  }
  (void)i;
}

static void loopback(int unix_sock) {
  int lfd = -1;
  uint16_t port = 0;
  pid_t pid;
  capnp_link_t *client = (capnp_link_t *)calloc(1, sizeof(*client));
  char path[] = "/tmp/capnp-janet-vat.sock";
  if (unix_sock) {
    unlink(path);
    check(capnp_listen_unix(path, &lfd) == CAPNP_OK, "listen unix");
  } else {
    check(capnp_listen_tcp("127.0.0.1", 0, &lfd, &port) == CAPNP_OK, "listen tcp");
  }
  pid = fork();
  if (pid == 0)
    child_serve(lfd);
  if (unix_sock)
    check(capnp_link_dial_unix(client, path) == CAPNP_OK, "dial unix");
  else
    check(capnp_link_dial_tcp(client, "127.0.0.1", port) == CAPNP_OK, "dial tcp");
  client_add(client, unix_sock ? "unix sum" : "tcp sum");
  capnp_link_close(client);
  waitpid(pid, NULL, 0);
  close(lfd);
  if (unix_sock)
    unlink(path);
  free(client);
}

static int dial_peer(uint16_t port) {
  capnp_link_t *client = (capnp_link_t *)calloc(1, sizeof(*client));
  int tries;
  for (tries = 0; tries < 50; tries++) {
    if (capnp_link_dial_tcp(client, "127.0.0.1", port) == CAPNP_OK)
      break;
    {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 50000000L;
      nanosleep(&ts, NULL);
    }
  }
  if (client->fd <= 0 && tries >= 50) {
    fprintf(stderr, "could not dial 127.0.0.1:%u\n", port);
    free(client);
    return 1;
  }
  client_add(client, "cpp sum");
  capnp_link_close(client);
  free(client);
  return g_fail ? 1 : 0;
}

static int serve_peer(uint16_t port) {
  int lfd = -1;
  uint16_t bound = 0;
  capnp_link_t *link = (capnp_link_t *)calloc(1, sizeof(*link));
  int i;
  if (capnp_listen_tcp("127.0.0.1", port, &lfd, &bound) != CAPNP_OK)
    return 1;
  printf("%u\n", bound);
  fflush(stdout);
  if (capnp_link_accept(lfd, link) != CAPNP_OK)
    return 1;
  capnp_rpc_set_bootstrap(&link->conn, &g_server_obj, adder_dispatch);
  for (i = 0; i < 8; i++) {
    if (capnp_link_recv(link))
      break;
  }
  capnp_link_close(link);
  close(lfd);
  free(link);
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "dial") == 0)
    return dial_peer((uint16_t)atoi(argv[2]));
  if (argc >= 3 && strcmp(argv[1], "serve") == 0)
    return serve_peer((uint16_t)atoi(argv[2]));
  loopback(1);
  loopback(0);
  if (g_fail) {
    fprintf(stderr, "%d transport checks failed\n", g_fail);
    return 1;
  }
  printf("ok transport\n");
  return 0;
}
