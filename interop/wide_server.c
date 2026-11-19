/* Serve Wide.make with a result bigger than one data word and one
 * pointer word, and print the bound port. A capnp-C++ client calls it. */
#define _POSIX_C_SOURCE 200809L
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_transport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_server;

static int wide_dispatch(void *server, uint64_t interface_id, uint16_t method_id,
                         const capnp_ptr_t *params, const capnp_bptr_t *results) {
  capnp_bptr_t body;
  int64_t n;
  (void)server;
  (void)interface_id;
  (void)method_id;
  n = (int64_t)capnp_get_u64(params, 0, 0);
  /* lo, hi: two words. flag lives in the next word. name is pointer 0. */
  if (capnp_builder_struct(results, 3, 1, &body))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u64(&body, 0, (uint64_t)(n - 1)))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_u64(&body, 8, (uint64_t)(n + 1)))
    return CAPNP_ERR_ALLOC;
  if (capnp_builder_set_bool(&body, 128, 1))
    return CAPNP_ERR_ALLOC;
  return capnp_builder_set_text(&body, 3, 0, "wide", 4);
}

int main(int argc, char **argv) {
  int lfd = -1;
  uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 0;
  uint16_t bound = 0;
  capnp_link_t *link = (capnp_link_t *)calloc(1, sizeof(*link));
  int i;
  if (!link || capnp_listen_tcp("127.0.0.1", port, &lfd, &bound) != CAPNP_OK)
    return 1;
  printf("%u\n", bound);
  fflush(stdout);
  if (capnp_link_accept(lfd, link) != CAPNP_OK)
    return 1;
  capnp_rpc_set_bootstrap(&link->conn, &g_server, wide_dispatch);
  for (i = 0; i < 8; i++) {
    if (capnp_link_recv(link))
      break;
  }
  capnp_link_close(link);
  free(link);
  close(lfd);
  return 0;
}
