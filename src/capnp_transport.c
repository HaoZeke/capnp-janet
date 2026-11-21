#define _POSIX_C_SOURCE 200809L
#include <capnp-janet/capnp_pointer.h>
#include <capnp-janet/capnp_transport.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* A pipe needs write(), which has no MSG_NOSIGNAL equivalent.
 *
 * kj 1.4.0 does not mask SIGPIPE around a write.
 * UnixEventPort::ignoreSigpipe() installs signal(SIGPIPE, SIG_IGN)
 * once (src/kj/async-unix.c++:218-231). The write paths then call
 * ::writev or ::sendmsg with a zero flags word (src/kj/io.c++:450,
 * src/kj/async-io-unix.c++:778, src/kj/async-io-unix.c++:815). An
 * ignored signal that is not blocked is discarded, so those writes
 * do not leave SIGPIPE pending. Copying that into this library would
 * either change the host disposition or, while the caller already
 * blocks SIGPIPE, still queue the write: a blocked signal is pending
 * even when its disposition is SIG_IGN. There is no portable way to
 * tell the write's signal from one already pending for the process.
 * sigpending() is the union of the two sets, and on Linux both
 * kill() and write() report si_code SI_USER.
 *
 * Where sigtimedwait() exists, it dequeues a thread-pending signal
 * before a process-pending one. The write's SIGPIPE is thread
 * directed. After EPIPE we dequeue one. If the pending set is then
 * empty, the write coalesced with a signal this thread already had
 * and we put that signal back. If a signal is still pending, the one
 * removed was the write's and the process-directed signal stays.
 * POSIX does not require that order. kj does not rely on it; its
 * epoll poll uses sigtimedwait only to take signals the event loop
 * itself is waiting for (src/kj/async-unix.c++:695). With both a
 * thread and a process signal already pending, one is dropped. If
 * another thread takes the process-directed signal while the write
 * runs, one extra is raised.
 *
 * macOS and OpenBSD have no sigtimedwait (src/kj/async-unix.c++:43-46).
 * A blocking sigwait can hang if another thread takes the
 * process-directed signal first, the race kj describes at
 * src/kj/async-unix.c++:959-963. On those platforms a SIGPIPE that
 * was already pending is left untouched, so a process-directed
 * signal can still be joined by the write's thread-directed one.
 * pthread_sigmask, not sigprocmask, is what kj uses to change one
 * thread's mask (src/kj/async-unix.c++:192).
 */
#if !defined(__APPLE__) && !defined(__OpenBSD__)
static int sigpipe_pending(void) {
  sigset_t pending;
  if (sigpending(&pending) != 0)
    return 0;
  return sigismember(&pending, SIGPIPE) == 1;
}
#endif

static void release_write_sigpipe(const sigset_t *block, int had) {
#if !defined(__APPLE__) && !defined(__OpenBSD__)
  struct timespec ts;
  int signo;
  ts.tv_sec = 0;
  ts.tv_nsec = 0;
  do {
    signo = sigtimedwait(block, NULL, &ts);
  } while (signo < 0 && errno == EINTR);
  if (signo != SIGPIPE)
    return;
  if (had && !sigpipe_pending())
    pthread_kill(pthread_self(), SIGPIPE);
#else
  int signo;
  sigset_t pending;
  if (had)
    return;
  if (sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) != 1)
    return;
  while (sigwait(block, &signo) == EINTR)
    ;
#endif
}

static ssize_t write_nosigpipe(int fd, const void *buf, size_t count) {
  sigset_t block, old, pending;
  ssize_t n;
  int saved, had_sigpipe;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &block, &old) != 0)
    return write(fd, buf, count);
  had_sigpipe = sigpending(&pending) != 0 ||
                sigismember(&pending, SIGPIPE) == 1;
  n = write(fd, buf, count);
  if (n < 0) {
    saved = errno;
    if (saved == EPIPE)
      release_write_sigpipe(&block, had_sigpipe);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = saved;
    return n;
  }
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  return n;
}

int capnp_write_all(int fd, const uint8_t *data, size_t len) {
  size_t off = 0;
  int flags = 0;
  int saved_errno = errno;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  while (off < len) {
    int saved = errno;
    ssize_t n = send(fd, data + off, len - off, flags);
    if (n < 0 && errno == ENOTSOCK) {
      n = write_nosigpipe(fd, data + off, len - off);
      if (n >= 0)
        errno = saved;
    }
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return CAPNP_ERR_ALLOC;
    }
    if (n == 0)
      return CAPNP_ERR_ALLOC;
    off += (size_t)n;
  }
  errno = saved_errno;
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
  if (nsegs == 0 || nsegs > CAPNP_MAX_STREAM_SEGMENTS)
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

/* A bound socket with nobody listening answers ECONNREFUSED. A live
 * listener accepts the connect, and anything else is left in place. */
static int unix_socket_is_stale(const char *path) {
  int s, rc;
  struct sockaddr_un addr;
  s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    return 0;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, strlen(path) + 1);
  rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
  if (rc == 0) {
    close(s);
    return 0;
  }
  rc = errno;
  close(s);
  return rc == ECONNREFUSED;
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
      if (!unix_socket_is_stale(path) || unlink(path) != 0)
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
