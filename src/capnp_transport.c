#define _POSIX_C_SOURCE 200809L
#include <capnp-janet/capnp_pointer.h>
#include <capnp-janet/capnp_transport.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int capnp_write_all(int fd, const uint8_t *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return CAPNP_ERR_ALLOC;
    }
    if (n == 0)
      return CAPNP_ERR_ALLOC;
    off += (size_t)n;
  }
  return CAPNP_OK;
}

static int read_full(int fd, uint8_t *p, size_t count) {
  size_t off = 0;
  while (off < count) {
    ssize_t n = read(fd, p + off, count - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1;
    off += (size_t)n;
  }
  return 0;
}

int capnp_read_frame(int fd, uint8_t *buf, size_t cap, size_t *out_len) {
  uint32_t nsegs, i, table_bytes, total_words = 0;
  if (!buf || cap < 8 || !out_len)
    return CAPNP_ERR_ARG;
  if (read_full(fd, buf, 4))
    return CAPNP_ERR_FRAMING;
  nsegs = capnp_load_le32(buf);
  nsegs += 1;
  if (nsegs == 0 || nsegs > 512)
    return CAPNP_ERR_FRAMING;
  table_bytes = 4 + nsegs * 4;
  if (table_bytes % 8)
    table_bytes += 4;
  if (table_bytes > cap)
    return CAPNP_ERR_BOUNDS;
  if (read_full(fd, buf + 4, table_bytes - 4))
    return CAPNP_ERR_FRAMING;
  for (i = 0; i < nsegs; i++) {
    uint32_t w = capnp_load_le32(buf + 4 + (size_t)i * 4);
    if (total_words > UINT32_MAX - w)
      return CAPNP_ERR_BOUNDS;
    total_words += w;
  }
  if ((size_t)table_bytes + (size_t)total_words * 8 > cap)
    return CAPNP_ERR_BOUNDS;
  if (total_words &&
      read_full(fd, buf + table_bytes, (size_t)total_words * 8))
    return CAPNP_ERR_FRAMING;
  *out_len = table_bytes + (size_t)total_words * 8;
  return CAPNP_OK;
}

static int send_link(void *ctx, const uint8_t *data, size_t len) {
  capnp_link_t *link = (capnp_link_t *)ctx;
  return capnp_write_all(link->fd, data, len);
}

int capnp_link_from_fd(capnp_link_t *link, int fd) {
  if (!link || fd < 0)
    return CAPNP_ERR_ARG;
  memset(link, 0, sizeof(*link));
  link->fd = fd;
  capnp_rpc_init(&link->conn, send_link, link);
  return CAPNP_OK;
}

static int parse_v4(const char *host, struct in_addr *out) {
  const char *h = (host && host[0]) ? host : "127.0.0.1";
  return inet_pton(AF_INET, h, out) == 1 ? 0 : -1;
}

int capnp_listen_tcp(const char *host, uint16_t port, int *fd, uint16_t *bound_port) {
  int s, one = 1;
  struct sockaddr_in addr;
  socklen_t alen = sizeof(addr);
  if (!fd)
    return CAPNP_ERR_ARG;
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    return CAPNP_ERR_ALLOC;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (parse_v4(host, &addr.sin_addr)) {
    close(s);
    return CAPNP_ERR_ARG;
  }
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) || listen(s, 4)) {
    close(s);
    return CAPNP_ERR_ALLOC;
  }
  if (getsockname(s, (struct sockaddr *)&addr, &alen) == 0 && bound_port)
    *bound_port = ntohs(addr.sin_port);
  *fd = s;
  return CAPNP_OK;
}

int capnp_dial_tcp(const char *host, uint16_t port, int *fd) {
  int s;
  struct sockaddr_in addr;
  if (!fd)
    return CAPNP_ERR_ARG;
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    return CAPNP_ERR_ALLOC;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (parse_v4(host, &addr.sin_addr)) {
    close(s);
    return CAPNP_ERR_ARG;
  }
  if (connect(s, (struct sockaddr *)&addr, sizeof(addr))) {
    close(s);
    return CAPNP_ERR_ALLOC;
  }
  *fd = s;
  return CAPNP_OK;
}

int capnp_listen_unix(const char *path, int *fd) {
  int s;
  struct sockaddr_un addr;
  if (!path || !fd)
    return CAPNP_ERR_ARG;
  if (strlen(path) >= sizeof(addr.sun_path))
    return CAPNP_ERR_BOUNDS;
  {
    struct stat st;
    if (lstat(path, &st) == 0) {
      if (!S_ISSOCK(st.st_mode))
        return CAPNP_ERR_IO;
      if (unlink(path) != 0)
        return CAPNP_ERR_IO;
    } else if (errno != ENOENT) {
      return CAPNP_ERR_IO;
    }
  }
  s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    return CAPNP_ERR_ALLOC;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, strlen(path) + 1);
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) || listen(s, 4)) {
    close(s);
    return CAPNP_ERR_ALLOC;
  }
  *fd = s;
  return CAPNP_OK;
}

int capnp_dial_unix(const char *path, int *fd) {
  int s;
  struct sockaddr_un addr;
  if (!path || !fd)
    return CAPNP_ERR_ARG;
  if (strlen(path) >= sizeof(addr.sun_path))
    return CAPNP_ERR_BOUNDS;
  s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    return CAPNP_ERR_ALLOC;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, strlen(path) + 1);
  if (connect(s, (struct sockaddr *)&addr, sizeof(addr))) {
    close(s);
    return CAPNP_ERR_ALLOC;
  }
  *fd = s;
  return CAPNP_OK;
}

int capnp_accept_fd(int listen_fd, int *fd) {
  int s;
  if (!fd)
    return CAPNP_ERR_ARG;
  do {
    s = accept(listen_fd, NULL, NULL);
  } while (s < 0 && errno == EINTR);
  if (s < 0)
    return CAPNP_ERR_ALLOC;
  *fd = s;
  return CAPNP_OK;
}

static int link_from_fd_or_close(capnp_link_t *link, int fd) {
  int rc = capnp_link_from_fd(link, fd);
  if (rc && fd >= 0)
    close(fd);
  return rc;
}

int capnp_link_dial_tcp(capnp_link_t *link, const char *host, uint16_t port) {
  int fd, rc;
  rc = capnp_dial_tcp(host, port, &fd);
  if (rc)
    return rc;
  return link_from_fd_or_close(link, fd);
}

int capnp_link_dial_unix(capnp_link_t *link, const char *path) {
  int fd, rc;
  rc = capnp_dial_unix(path, &fd);
  if (rc)
    return rc;
  return link_from_fd_or_close(link, fd);
}

int capnp_link_accept(int listen_fd, capnp_link_t *link) {
  int fd, rc;
  rc = capnp_accept_fd(listen_fd, &fd);
  if (rc)
    return rc;
  return link_from_fd_or_close(link, fd);
}

int capnp_link_recv(capnp_link_t *link) {
  uint8_t buf[65536];
  size_t n = 0;
  int rc;
  if (!link)
    return CAPNP_ERR_ARG;
  rc = capnp_read_frame(link->fd, buf, sizeof(buf), &n);
  if (rc)
    return rc;
  if (capnp_rpc_handle(&link->conn, buf, n))
    return CAPNP_ERR_KIND;
  return CAPNP_OK;
}

void capnp_link_close(capnp_link_t *link) {
  if (!link)
    return;
  if (link->fd >= 0)
    close(link->fd);
  link->fd = -1;
}
