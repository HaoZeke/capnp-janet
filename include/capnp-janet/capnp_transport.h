/* SPDX-License-Identifier: MIT */
#ifndef CAPNP_JANET_TRANSPORT_H
#define CAPNP_JANET_TRANSPORT_H

#include <capnp-janet/capnp_rpc.h>
#include <stddef.h>
#include <stdint.h>

/* A vat bound to one connected socket. `conn` is large; allocate the
 * link on the heap. Outgoing frames are written to the socket as the
 * vat produces them. capnp_link_recv reads one frame and handles it. */
typedef struct capnp_link {
  capnp_rpc_conn_t conn;
  int fd;
} capnp_link_t;

CAPNP_JANET_EXPORT int capnp_listen_tcp(const char *host, uint16_t port, int *fd,
                                        uint16_t *bound_port);
CAPNP_JANET_EXPORT int capnp_dial_tcp(const char *host, uint16_t port, int *fd);
CAPNP_JANET_EXPORT int capnp_listen_unix(const char *path, int *fd);
CAPNP_JANET_EXPORT int capnp_dial_unix(const char *path, int *fd);
CAPNP_JANET_EXPORT int capnp_accept_fd(int listen_fd, int *fd);

CAPNP_JANET_EXPORT int capnp_write_all(int fd, const uint8_t *data, size_t len);
CAPNP_JANET_EXPORT int capnp_read_frame(int fd, uint8_t *buf, size_t cap, size_t *out_len);

CAPNP_JANET_EXPORT int capnp_link_from_fd(capnp_link_t *link, int fd);
CAPNP_JANET_EXPORT int capnp_link_dial_tcp(capnp_link_t *link, const char *host,
                                           uint16_t port);
CAPNP_JANET_EXPORT int capnp_link_dial_unix(capnp_link_t *link, const char *path);
CAPNP_JANET_EXPORT int capnp_link_accept(int listen_fd, capnp_link_t *link);
CAPNP_JANET_EXPORT int capnp_link_recv(capnp_link_t *link);
CAPNP_JANET_EXPORT void capnp_link_close(capnp_link_t *link);

#endif
