/* The vat dials and listens on TCP and Unix sockets itself.
 *
 * With no arguments, two vats on a Unix socket and two on TCP loopback
 * call an adder and check the sum. `dial <port>` calls the capnp-C++
 * Adder. `serve <port>` listens and answers one C++ client.
 * `bad-return <port>` answers the bootstrap of each of seven C++ clients
 * with a Return rpc.c++ refuses, and checks that the Abort it sends
 * back gives the reason this vat gives for the same Return.
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>
#include <capnp-janet/capnp_transport.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#if !defined(CAPNP_HAS_MALLINFO2) && defined(__GLIBC__) &&                   \
    (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 33)
#define CAPNP_HAS_MALLINFO2 1
#endif
#ifdef CAPNP_HAS_MALLINFO2
#include <malloc.h>
#endif
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

static int fill_add(void *ctx, const capnp_bptr_t *params) {
  (void)ctx;
  capnp_builder_set_u64(params, 0, 20);
  capnp_builder_set_u64(params, 8, 22);
  return 0;
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

static int client_add(capnp_link_t *link, const char *what) {
  uint32_t qboot, qcall;
  int i, cap;
  qboot = capnp_rpc_send_bootstrap(&link->conn);
  check(qboot != (uint32_t)-1, "bootstrap");
  check(capnp_link_recv(link) == CAPNP_OK, "bootstrap reply");
  check(capnp_rpc_is_answered(&link->conn, qboot), "answered bootstrap");
  cap = capnp_rpc_answer_cap_id(&link->conn, qboot);
  check(cap >= 0, "bootstrap cap");
  qcall = capnp_rpc_send_call(&link->conn, (uint32_t)cap, ADDER_IFACE, 0, 2, 0,
                              fill_add, NULL);
  check(qcall != (uint32_t)-1, "call");
  check(capnp_link_recv(link) == CAPNP_OK, "call reply");
  check(capnp_rpc_is_answered(&link->conn, qcall) &&
            !capnp_rpc_is_failed(&link->conn, qcall),
        "call ok");
  {
    capnp_message_t m = {0};
    capnp_ptr_t content;
    check(capnp_rpc_answer_content(&link->conn, qcall, &m, &content) == CAPNP_OK,
          "content");
    check((int64_t)capnp_get_u64(&content, 0, 0) == 42, what);
    capnp_message_free(&m);
  }
  (void)i;
  return cap;
}

static int big_dispatch(void *server, uint64_t interface_id, uint16_t method_id,
                        const capnp_ptr_t *params, const capnp_bptr_t *results) {
  capnp_bptr_t body;
  uint32_t n = capnp_get_u32(params, 0, 0);
  uint8_t *big = (uint8_t *)malloc(n ? n : 1);
  int rc;
  (void)server;
  (void)interface_id;
  (void)method_id;
  if (!big)
    return CAPNP_ERR_ALLOC;
  memset(big, 0xab, n);
  rc = capnp_builder_struct(results, 0, 1, &body);
  if (rc == CAPNP_OK)
    rc = capnp_builder_set_data(&body, 0, 0, big, n);
  free(big);
  return rc;
}

static int fill_len(void *ctx, const capnp_bptr_t *params) {
  return capnp_builder_set_u32(params, 0, *(uint32_t *)ctx);
}

static size_t heap_in_use(void) {
#ifdef CAPNP_HAS_MALLINFO2
  return mallinfo2().uordblks;
#else
  return 0;
#endif
}

/* A call whose result is @n bytes of Data, over two links on a socket
 * pair. Neither side sends Finish, so closing each link has to free the
 * answer and the return it keeps. */
static void test_link_large_result(uint32_t n, const char *what) {
  capnp_link_t *srv = (capnp_link_t *)calloc(1, sizeof(*srv));
  capnp_link_t *cli = (capnp_link_t *)calloc(1, sizeof(*cli));
  int sv[2], bufsz = 1 << 21, cap, i;
  uint32_t qb, qc;
  size_t before = heap_in_use(), after;
  check(srv && cli && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0,
        "large link pair");
  for (i = 0; i < 2; i++) {
    setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
  }
  check(capnp_link_from_fd(srv, sv[0]) == CAPNP_OK &&
            capnp_link_from_fd(cli, sv[1]) == CAPNP_OK,
        "large link setup");
  capnp_rpc_set_bootstrap(&srv->conn, &g_server_obj, big_dispatch);
  qb = capnp_rpc_send_bootstrap(&cli->conn);
  check(capnp_link_recv(srv) == CAPNP_OK, "large bootstrap served");
  check(capnp_link_recv(cli) == CAPNP_OK, "large bootstrap reply");
  cap = capnp_rpc_answer_cap_id(&cli->conn, qb);
  qc = capnp_rpc_send_call(&cli->conn, (uint32_t)cap, ADDER_IFACE, 0, 1, 0,
                           fill_len, &n);
  check(capnp_link_recv(srv) == CAPNP_OK, "large call served");
  check(capnp_link_recv(cli) == CAPNP_OK, what);
  {
    capnp_message_t m = {0};
    capnp_ptr_t content;
    const uint8_t *data = NULL;
    size_t len = 0;
    check(capnp_rpc_answer_content(&cli->conn, qc, &m, &content) == CAPNP_OK &&
              capnp_get_data(&content, 0, &data, &len) == CAPNP_OK &&
              len == n && data[n - 1] == 0xab,
          "large link result content");
    capnp_message_free(&m);
  }
  capnp_link_close(srv);
  capnp_link_close(cli);
  free(srv);
  free(cli);
  after = heap_in_use();
  check(after <= before + 4096, "closed links free their large frames");
}

static int count_fds(void) {
  DIR *d = opendir("/proc/self/fd");
  struct dirent *ent;
  int n = 0;
  if (!d)
    return -1;
  while ((ent = readdir(d)) != NULL) {
    if (ent->d_name[0] != '.')
      n++;
  }
  closedir(d);
  return n;
}

static void test_link_setup_closes_fd(void) {
  int lfd = -1;
  uint16_t port = 0;
  int before, client = -1;
  char dir[] = "/tmp/capnp-janet-XXXXXX";
  char path[sizeof(dir) + 16];
  check(capnp_listen_tcp("127.0.0.1", 0, &lfd, &port) == CAPNP_OK, "leak listen");
  before = count_fds();
  check(capnp_link_dial_tcp(NULL, "127.0.0.1", port) != CAPNP_OK, "null tcp link");
  check(count_fds() == before, "tcp dial closes fd");
  check(capnp_dial_tcp("127.0.0.1", port, &client) == CAPNP_OK, "tcp connect");
  before = count_fds();
  check(capnp_link_accept(lfd, NULL) != CAPNP_OK, "null accept link");
  check(count_fds() == before, "accept closes fd");
  if (client >= 0)
    close(client);
  close(lfd);
  lfd = -1;
  check(mkdtemp(dir) != NULL, "leak dir");
  snprintf(path, sizeof(path), "%s/vat.sock", dir);
  check(capnp_listen_unix(path, &lfd) == CAPNP_OK, "leak unix");
  before = count_fds();
  check(capnp_link_dial_unix(NULL, path) != CAPNP_OK, "null unix link");
  check(count_fds() == before, "unix dial closes fd");
  client = -1;
  check(capnp_dial_unix(path, &client) == CAPNP_OK, "unix connect");
  before = count_fds();
  check(capnp_link_accept(lfd, NULL) != CAPNP_OK, "null unix accept");
  check(count_fds() == before, "unix accept closes fd");
  if (client >= 0)
    close(client);
  if (lfd >= 0)
    close(lfd);
  unlink(path);
  rmdir(dir);
}

static void test_unix_replaces_only_socket(void) {
  char dir[] = "/tmp/capnp-janet-XXXXXX";
  char path[sizeof(dir) + 16];
  char linkpath[sizeof(dir) + 16];
  struct stat st;
  int lfd = -1;
  int s;
  struct sockaddr_un addr;
  FILE *f;
  check(mkdtemp(dir) != NULL, "mkdtemp");
  snprintf(path, sizeof(path), "%s/vat.sock", dir);
  f = fopen(path, "w");
  check(f != NULL, "create file");
  if (f) {
    fputs("keep", f);
    fclose(f);
  }
  check(capnp_listen_unix(path, &lfd) != CAPNP_OK, "refuse regular file");
  check(lfd < 0, "no fd for file");
  f = fopen(path, "r");
  check(f != NULL, "file still there");
  if (f) {
    char buf[8] = {0};
    check(fread(buf, 1, 4, f) == 4 && strcmp(buf, "keep") == 0, "file bytes");
    fclose(f);
  }
  unlink(path);
  s = socket(AF_UNIX, SOCK_STREAM, 0);
  check(s >= 0, "stale socket");
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, strlen(path) + 1);
  check(bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind stale");
  close(s);
  check(capnp_listen_unix(path, &lfd) == CAPNP_OK, "replace stale socket");
  check(lstat(path, &st) == 0 && S_ISSOCK(st.st_mode), "path is a socket");
  if (lfd >= 0)
    close(lfd);
  lfd = -1;
  unlink(path);
  snprintf(linkpath, sizeof(linkpath), "%s/other", dir);
  f = fopen(linkpath, "w");
  if (f)
    fclose(f);
  check(symlink(linkpath, path) == 0, "symlink");
  check(capnp_listen_unix(path, &lfd) != CAPNP_OK, "refuse symlink");
  check(lstat(path, &st) == 0 && S_ISLNK(st.st_mode), "symlink remains");
  unlink(path);
  unlink(linkpath);
  rmdir(dir);
}

static void test_unix_keeps_live_socket(void) {
  char dir[] = "/tmp/capnp-janet-XXXXXX";
  char path[sizeof(dir) + 16];
  int live = -1, again = -1, client = -1, accepted = -1;
  check(mkdtemp(dir) != NULL, "live dir");
  snprintf(path, sizeof(path), "%s/vat.sock", dir);
  check(capnp_listen_unix(path, &live) == CAPNP_OK, "first listen");
  check(capnp_listen_unix(path, &again) != CAPNP_OK, "refuse live socket");
  check(again < 0, "no second fd");
  if (again >= 0)
    close(again);
  check(capnp_dial_unix(path, &client) == CAPNP_OK, "still connectable");
  if (live >= 0)
    fcntl(live, F_SETFL, O_NONBLOCK);
  if (live >= 0)
    accepted = accept(live, NULL, NULL);
  check(accepted >= 0, "original accepts");
  if (accepted >= 0)
    close(accepted);
  if (client >= 0)
    close(client);
  if (live >= 0)
    close(live);
  unlink(path);
  rmdir(dir);
}

static void loopback(int unix_sock) {
  int lfd = -1;
  uint16_t port = 0;
  pid_t pid;
  capnp_link_t *client = (capnp_link_t *)calloc(1, sizeof(*client));
  char dir[] = "/tmp/capnp-janet-XXXXXX";
  char path[sizeof(dir) + 16];
  int have_dir = 0;
  path[0] = 0;
  if (unix_sock) {
    have_dir = mkdtemp(dir) != NULL;
    check(have_dir, "private socket dir");
    if (have_dir) {
      snprintf(path, sizeof(path), "%s/vat.sock", dir);
      check(strstr(path, dir) == path, "socket stays in the private dir");
      check(capnp_listen_unix(path, &lfd) == CAPNP_OK, "listen unix");
    }
  } else {
    check(capnp_listen_tcp("127.0.0.1", 0, &lfd, &port) == CAPNP_OK, "listen tcp");
  }
  pid = fork();
  if (pid == 0)
    child_serve(lfd);
  if (unix_sock)
    check(have_dir && capnp_link_dial_unix(client, path) == CAPNP_OK, "dial unix");
  else
    check(capnp_link_dial_tcp(client, "127.0.0.1", port) == CAPNP_OK, "dial tcp");
  client_add(client, unix_sock ? "unix sum" : "tcp sum");
  capnp_link_close(client);
  waitpid(pid, NULL, 0);
  if (lfd >= 0)
    close(lfd);
  if (unix_sock && have_dir) {
    unlink(path);
    rmdir(dir);
  }
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
  {
    int cap = client_add(client, "cpp sum");
    uint32_t n = 70000, q;
    capnp_message_t m = {0};
    capnp_ptr_t content;
    const uint8_t *data = NULL;
    size_t len = 0;
    q = capnp_rpc_send_call(&client->conn, (uint32_t)cap, ADDER_IFACE, 1, 1, 0,
                            fill_len, &n);
    check(capnp_link_recv(client) == CAPNP_OK, "cpp echo over 64 KiB");
    check(capnp_rpc_answer_content(&client->conn, q, &m, &content) == CAPNP_OK &&
              capnp_get_data(&content, 0, &data, &len) == CAPNP_OK &&
              len == n && data[n - 1] == 0xab,
          "cpp echo content");
    capnp_message_free(&m);
  }
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

/* `skew` moves the answer id off the bootstrap question. `twice` instead
 * answers the bootstrap and the add call properly, then sends the add
 * Return again while the client still holds the response. */
static const struct {
  uint16_t tag;
  uint32_t take_from;
  uint32_t skew;
  int twice;
  const char *name;
} bad_returns[] = {
    {2, 0, 0, 0, "canceled"},
    {3, 0, 0, 0, "resultsSentElsewhere"},
    {4, 99, 0, 0, "takeFromOtherQuestion"},
    {5, 0, 0, 0, "acceptFromThirdParty"},
    {9, 0, 0, 0, "unknown arm"},
    {0, 0, 77, 0, "unknown question"},
    {0, 0, 0, 1, "duplicate"},
};
#define N_BAD_RETURNS (sizeof bad_returns / sizeof bad_returns[0])

static int build_return(uint32_t answer, uint16_t tag, uint32_t take_from,
                        uint8_t **out, size_t *len) {
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
       capnp_builder_set_u32(&ret, 8, take_from) ||
       capnp_builder_serialize(&b, out, len);
  capnp_builder_free(&b);
  return rc;
}

/* The reason text of an Abort frame, copied into `reason`; else -1. */
static int abort_reason(const uint8_t *frame, size_t n, char *reason,
                        size_t cap) {
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

static char g_local_abort[256];

static int keep_abort(void *ctx, const uint8_t *data, size_t len) {
  (void)ctx;
  if (abort_reason(data, len, g_local_abort, sizeof g_local_abort))
    g_local_abort[0] = 0;
  return 0;
}

/* The Abort this vat sends for the same Return to its own bootstrap. */
static int local_abort(size_t i) {
  static capnp_rpc_conn_t c;
  uint8_t *out = NULL;
  size_t len = 0;
  uint32_t q;
  int rc;
  capnp_rpc_init(&c, keep_abort, NULL);
  q = capnp_rpc_send_bootstrap(&c);
  g_local_abort[0] = 0;
  rc = build_return(q + bad_returns[i].skew, bad_returns[i].tag,
                    bad_returns[i].take_from, &out, &len) ||
       (bad_returns[i].twice && capnp_rpc_handle(&c, out, len) != CAPNP_OK) ||
       capnp_rpc_handle(&c, out, len) != CAPNP_ERR_KIND;
  free(out);
  capnp_rpc_free(&c);
  return rc;
}

static int g_dup_fd = -1;
static uint8_t *g_dup_last;
static size_t g_dup_len;

static int send_dup(void *ctx, const uint8_t *data, size_t len) {
  (void)ctx;
  free(g_dup_last);
  g_dup_last = (uint8_t *)malloc(len);
  if (g_dup_last == NULL)
    return -1;
  memcpy(g_dup_last, data, len);
  g_dup_len = len;
  return capnp_write_all(g_dup_fd, data, len) == CAPNP_OK ? 0 : -1;
}

static int serve_bad_returns(uint16_t port) {
  static capnp_rpc_conn_t vat;
  int lfd = -1, failed = 0;
  uint16_t bound = 0;
  size_t i;
  if (capnp_listen_tcp("127.0.0.1", port, &lfd, &bound) != CAPNP_OK)
    return 1;
  printf("%u\n", bound);
  fflush(stdout);
  for (i = 0; i < N_BAD_RETURNS; i++) {
    char cpp[256] = "";
    int fd = accept(lfd, NULL, NULL), sent = 0;
    if (fd < 0)
      return 1;
    g_dup_fd = fd;
    capnp_rpc_init(&vat, send_dup, NULL);
    capnp_rpc_set_bootstrap(&vat, &g_server_obj, adder_dispatch);
    for (;;) {
      capnp_message_t m = {0};
      capnp_ptr_t root, body;
      uint8_t *frame = NULL;
      size_t n = 0;
      int tag = -1, done = 0;
      if (capnp_read_frame_alloc(fd, &frame, &n) != CAPNP_OK)
        break;
      if (capnp_message_from_flat(&m, frame, n) == CAPNP_OK &&
          capnp_root(&m, &root) == CAPNP_OK)
        tag = capnp_get_u16(&root, 0, 0);
      if (bad_returns[i].twice && tag != CAPNP_RPC_MSG_ABORT) {
        (void)capnp_rpc_handle(&vat, frame, n);
        if (tag == CAPNP_RPC_MSG_CALL && g_dup_last != NULL)
          (void)capnp_write_all(fd, g_dup_last, g_dup_len);
      } else if (tag == CAPNP_RPC_MSG_BOOTSTRAP && !sent &&
                 capnp_getp(&root, 0, &body) == CAPNP_OK) {
        uint8_t *out = NULL;
        size_t len = 0;
        sent = build_return(capnp_get_u32(&body, 0, 0) + bad_returns[i].skew,
                            bad_returns[i].tag, bad_returns[i].take_from, &out,
                            &len) == 0 &&
               capnp_write_all(fd, out, len) == CAPNP_OK;
        free(out);
      }
      if (tag == CAPNP_RPC_MSG_ABORT) {
        abort_reason(frame, n, cpp, sizeof cpp);
        done = 1;
      }
      capnp_message_free(&m);
      free(frame);
      if (done)
        break;
    }
    close(fd);
    capnp_rpc_free(&vat);
    free(g_dup_last);
    g_dup_last = NULL;
    if (local_abort(i) ||
        cpp[0] == 0 || strcmp(cpp, g_local_abort) != 0) {
      fprintf(stderr, "FAIL Return %s: capnp-C++ \"%s\", this vat \"%s\"\n",
              bad_returns[i].name, cpp, g_local_abort);
      failed++;
    } else {
      printf("same Return %s: %s\n", bad_returns[i].name, cpp);
    }
  }
  close(lfd);
  return failed != 0;
}

/* Segment table is little-endian on the wire. The sizes below are
 * asymmetric (1 word, then 2) so a big-endian load of the same bytes
 * is a different count and misses the payload word. */
static void test_ipv4_literal(void) {
  int fd = -1;
  uint16_t port = 0;
  check(capnp_listen_tcp("127.1", 0, &fd, &port) != CAPNP_OK, "reject 127.1");
  check(fd < 0, "no fd for abbreviated address");
  check(capnp_listen_tcp("not-an-ip", 0, &fd, &port) != CAPNP_OK, "reject hostname");
  check(capnp_dial_tcp("127.1", 9, &fd) != CAPNP_OK, "dial abbreviated address");
  check(capnp_listen_tcp("127.0.0.1", 0, &fd, &port) == CAPNP_OK, "loopback literal");
  if (fd >= 0)
    close(fd);
  fd = -1;
  check(capnp_listen_tcp("", 0, &fd, &port) == CAPNP_OK, "empty host is loopback");
  if (fd >= 0)
    close(fd);
  fd = -1;
  check(capnp_listen_tcp(NULL, 0, &fd, &port) == CAPNP_OK, "null host is loopback");
  if (fd >= 0)
    close(fd);
}

static void test_frame_little_endian(void) {
  int sv[2];
  uint8_t frame[40];
  uint8_t buf[64];
  size_t n = 0;
  memset(frame, 0, sizeof(frame));
  capnp_store_le32(frame, 1);
  capnp_store_le32(frame + 4, 1);
  capnp_store_le32(frame + 8, 2);
  capnp_store_le64(frame + 16, 0x1122334455667788ULL);
  capnp_store_le64(frame + 24, 0x0102030405060708ULL);
  capnp_store_le64(frame + 32, 0xaabbccddeeff0011ULL);
  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
  check(capnp_write_all(sv[0], frame, sizeof(frame)) == CAPNP_OK, "write frame");
  check(capnp_read_frame(sv[1], buf, sizeof(buf), &n) == CAPNP_OK, "read frame");
  check(n == sizeof(frame), "frame length");
  check(capnp_load_le32(buf) == 1, "segment count minus one");
  check(buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0,
        "first size is little-endian");
  check(buf[8] == 2 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0,
        "second size is little-endian");
  check(capnp_load_le32(buf + 4) == 1, "first segment words");
  check(capnp_load_le32(buf + 8) == 2, "second segment words");
  check(capnp_load_le64(buf + 16) == 0x1122334455667788ULL, "seg0");
  check(capnp_load_le64(buf + 24) == 0x0102030405060708ULL, "seg1 word0");
  check(capnp_load_le64(buf + 32) == 0xaabbccddeeff0011ULL, "seg1 word1");
  close(sv[0]);
  close(sv[1]);
}

/* C++ stream readers take at most 511 segments. */
static void test_frame_segment_limit(void) {
  static uint8_t frame[8 + 4 * 512 + 8 * 512], buf[sizeof(frame)];
  uint32_t nsegs, i;
  for (nsegs = 511; nsegs <= 512; nsegs++) {
    int sv[2];
    size_t n = 0, table = 4 + 4 * (size_t)nsegs;
    int rc;
    table += table % 8;
    memset(frame, 0, sizeof(frame));
    capnp_store_le32(frame, nsegs - 1);
    for (i = 0; i < nsegs; i++)
      capnp_store_le32(frame + 4 + 4 * i, 1);
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    check(capnp_write_all(sv[0], frame, table + 8 * nsegs) == CAPNP_OK,
          "write frame");
    rc = capnp_read_frame(sv[1], buf, sizeof(buf), &n);
    if (nsegs == 511)
      check(rc == CAPNP_OK && n == table + 8 * nsegs, "511 segments read");
    else
      check(rc == CAPNP_ERR_FRAMING, "512 segments refused");
    close(sv[0]);
    close(sv[1]);
  }
}

/* C++ RPC refuses a frame of more than 8388608 words, counting the
 * segment table, before it reads the body. A one-segment frame of
 * 8388607 words is in range, so the reader waits for the body and here
 * meets end of file. */
static void test_frame_alloc_limit(void) {
  uint32_t words;
  for (words = 8388607; words <= 8388608; words++) {
    uint8_t head[8], *buf = (uint8_t *)1;
    size_t n = 0;
    int sv[2], rc;
    capnp_store_le32(head, 0);
    capnp_store_le32(head + 4, words);
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "limit socketpair");
    check(capnp_write_all(sv[0], head, 8) == CAPNP_OK, "limit header");
    close(sv[0]);
    rc = capnp_read_frame_alloc(sv[1], &buf, &n);
    if (words == 8388607)
      check(rc == CAPNP_ERR_FRAMING && buf == NULL, "frame at the limit read");
    else
      check(rc == CAPNP_ERR_BOUNDS && buf == NULL, "frame over the limit refused");
    close(sv[1]);
  }
}

static uint8_t g_boot[256];
static size_t g_boot_len;

static int capture_send(void *ctx, const uint8_t *data, size_t len) {
  (void)ctx;
  if (len > sizeof(g_boot))
    return -1;
  memcpy(g_boot, data, len);
  g_boot_len = len;
  return 0;
}

/* A bootstrap is one small segment. The word count's low byte is first
 * and the three high bytes are zero. A big-endian store of that count
 * puts the nonzero byte at the other end. */
static void test_bootstrap_frame_little_endian(void) {
  capnp_rpc_conn_t conn;
  uint32_t words;
  memset(&conn, 0, sizeof(conn));
  capnp_rpc_init(&conn, capture_send, NULL);
  g_boot_len = 0;
  check(capnp_rpc_send_bootstrap(&conn) != (uint32_t)-1, "send bootstrap");
  check(g_boot_len >= 12, "bootstrap frame length");
  check(g_boot[0] == 0 && g_boot[1] == 0 && g_boot[2] == 0 && g_boot[3] == 0,
        "one segment");
  words = capnp_load_le32(g_boot + 4);
  check(words > 0 && words < 64, "bootstrap word count");
  check(g_boot[4] == (uint8_t)words, "word count low byte first");
  check(g_boot[5] == 0 && g_boot[6] == 0 && g_boot[7] == 0,
        "word count high bytes clear");
}

static void test_size_to_i32(void) {
  int32_t n = -1;
  check(capnp_size_to_i32(4, &n) == CAPNP_OK && n == 4, "i32 4");
  check(capnp_size_to_i32(2147483647u, &n) == CAPNP_OK && n == 2147483647,
        "i32 max");
#if SIZE_MAX > 4294967295u
  check(capnp_size_to_i32((size_t)2147483648u, &n) != CAPNP_OK, "i32 overflow");
#endif
  check(capnp_size_to_i32(1, NULL) != CAPNP_OK, "i32 null");
}

static void test_write_pipe(void) {
  int p[2];
  uint8_t byte = 7, got = 0;
  check(pipe(p) == 0, "pipe");
  errno = EAGAIN;
  check(capnp_write_all(p[1], &byte, 1) == CAPNP_OK, "write pipe");
  check(errno == EAGAIN, "pipe write preserves errno");
  check(read(p[0], &got, 1) == 1 && got == 7, "pipe byte");
  close(p[0]);
  close(p[1]);
}

/* A closed peer must return an error. With the default SIGPIPE
 * disposition, write() on that socket kills the process. */
static void test_write_closed_socket(void) {
  int sv[2];
  uint8_t byte = 1;
  struct sigaction sa, old;
  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "closed pair");
  close(sv[1]);
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  check(sigaction(SIGPIPE, &sa, &old) == 0, "sigpipe default");
  check(capnp_write_all(sv[0], &byte, 1) != CAPNP_OK, "closed write");
  sigaction(SIGPIPE, &old, NULL);
  close(sv[0]);
}

/* The pipe fallback uses write(). A closed read end must return an
 * error on Linux too, where SO_NOSIGPIPE is absent. */
static void test_write_closed_pipe(void) {
  int p[2];
  uint8_t byte = 9;
  struct sigaction sa, old;
  check(pipe(p) == 0, "closed pipe");
  close(p[0]);
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  check(sigaction(SIGPIPE, &sa, &old) == 0, "sigpipe default pipe");
  errno = 0;
  check(capnp_write_all(p[1], &byte, 1) != CAPNP_OK && errno == EPIPE,
        "closed pipe returns EPIPE");
  sigaction(SIGPIPE, &old, NULL);
  close(p[1]);
}

static void test_write_pending_sigpipe(void) {
  int p[2], signo;
  uint8_t byte = 3;
  sigset_t block, old, pending;
  struct sigaction sa, old_sa;
  int pending_after;
  check(pipe(p) == 0, "pending pipe");
  close(p[0]);
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  check(pthread_sigmask(SIG_BLOCK, &block, &old) == 0, "block sigpipe");
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  check(sigaction(SIGPIPE, &sa, &old_sa) == 0, "pending sigpipe default");
  check(pthread_kill(pthread_self(), SIGPIPE) == 0, "queue sigpipe");
  check(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1,
        "sigpipe pending before write");
  errno = 0;
  check(capnp_write_all(p[1], &byte, 1) != CAPNP_OK && errno == EPIPE,
        "pending closed pipe returns EPIPE");
  pending_after = sigpending(&pending) == 0 &&
                  sigismember(&pending, SIGPIPE) == 1;
  check(pending_after, "sigpipe remains pending after write");
  if (pending_after)
    check(sigwait(&block, &signo) == 0 && signo == SIGPIPE, "consume sigpipe");
  sigaction(SIGPIPE, &old_sa, NULL);
  check(pthread_sigmask(SIG_SETMASK, &old, NULL) == 0, "restore sigpipe");
  close(p[1]);
}

#if !defined(__APPLE__) && !defined(__OpenBSD__)
/* sigpending() is only the union, so a process-directed SIGPIPE and the
 * write's thread-directed one both look like one pending signal. Count
 * them by dequeuing. */
static int closed_write_fd(void) {
  int p[2];
  if (pipe(p) != 0)
    return -1;
  close(p[0]);
  return p[1];
}

static int drain_sigpipe(void) {
  sigset_t set;
  struct timespec ts;
  int n = 0;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  ts.tv_sec = 0;
  ts.tv_nsec = 0;
  while (sigtimedwait(&set, NULL, &ts) == SIGPIPE) {
    n++;
    if (n > 8)
      break;
  }
  return n;
}

static void block_sigpipe(sigset_t *block, sigset_t *old) {
  sigemptyset(block);
  sigaddset(block, SIGPIPE);
  check(pthread_sigmask(SIG_BLOCK, block, old) == 0, "block sigpipe set");
}

static void test_write_process_sigpipe(void) {
  sigset_t block, old, pending;
  struct sigaction sa, old_sa;
  uint8_t byte = 3;
  int fd, n;
  block_sigpipe(&block, &old);
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  check(sigaction(SIGPIPE, &sa, &old_sa) == 0, "process sigpipe default");

  fd = closed_write_fd();
  check(fd >= 0, "process pipe");
  check(pthread_kill(pthread_self(), SIGPIPE) == 0, "queue thread sigpipe");
  errno = 0;
  check(capnp_write_all(fd, &byte, 1) != CAPNP_OK && errno == EPIPE,
        "thread sigpipe write returns EPIPE");
  close(fd);
  n = drain_sigpipe();
  check(n == 1, "thread sigpipe still exactly one after write");

  fd = closed_write_fd();
  check(fd >= 0, "process pipe 2");
  check(kill(getpid(), SIGPIPE) == 0, "queue process sigpipe");
  check(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1,
        "process sigpipe pending before write");
  errno = 0;
  check(capnp_write_all(fd, &byte, 1) != CAPNP_OK && errno == EPIPE,
        "process sigpipe write returns EPIPE");
  close(fd);
  n = drain_sigpipe();
  check(n == 1, "process sigpipe still exactly one after write");

  sa.sa_handler = SIG_IGN;
  check(sigaction(SIGPIPE, &sa, NULL) == 0, "sigpipe ignore");
  fd = closed_write_fd();
  check(fd >= 0, "ignored process pipe");
  check(kill(getpid(), SIGPIPE) == 0, "queue ignored process sigpipe");
  errno = 0;
  check(capnp_write_all(fd, &byte, 1) != CAPNP_OK && errno == EPIPE,
        "ignored process sigpipe write returns EPIPE");
  close(fd);
  n = drain_sigpipe();
  check(n == 1, "ignored process sigpipe still exactly one after write");

  sigaction(SIGPIPE, &old_sa, NULL);
  check(pthread_sigmask(SIG_SETMASK, &old, NULL) == 0,
        "restore process sigpipe mask");
}

static volatile sig_atomic_t g_waiter_stop;
static volatile sig_atomic_t g_writer_stop;
static volatile int g_waiter_hits;
static volatile long g_writer_writes;
static sem_t g_waiter_got;
static sem_t g_waiter_ready;
static sigset_t g_sigpipe_set;

static void *sigpipe_waiter(void *arg) {
  (void)arg;
  sem_post(&g_waiter_ready);
  while (!g_waiter_stop) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50 * 1000 * 1000;
    if (sigtimedwait(&g_sigpipe_set, NULL, &ts) == SIGPIPE) {
      g_waiter_hits++;
      sem_post(&g_waiter_got);
    }
  }
  return NULL;
}

static void *sigpipe_writer(void *arg) {
  uint8_t byte = 1;
  (void)arg;
  while (!g_writer_stop) {
    int fd = closed_write_fd();
    if (fd < 0)
      break;
    capnp_write_all(fd, &byte, 1);
    close(fd);
    g_writer_writes++;
  }
  return NULL;
}

static void test_write_sigpipe_two_threads(void) {
  sigset_t block, old;
  struct sigaction sa, old_sa;
  pthread_t waiter, writer;
  struct timespec pause, deadline;
  uint8_t byte = 1;
  int i, lost, nsend;
  block_sigpipe(&block, &old);
  g_sigpipe_set = block;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  check(sigaction(SIGPIPE, &sa, &old_sa) == 0, "two thread sigpipe default");
  check(sem_init(&g_waiter_got, 0, 0) == 0, "waiter sem");
  check(sem_init(&g_waiter_ready, 0, 0) == 0, "ready sem");
  g_waiter_stop = 0;
  g_writer_stop = 0;
  g_waiter_hits = 0;
  g_writer_writes = 0;
  check(pthread_create(&waiter, NULL, sigpipe_waiter, NULL) == 0,
        "create waiter");
  check(sem_wait(&g_waiter_ready) == 0, "waiter ready");
  pause.tv_sec = 0;
  pause.tv_nsec = 100 * 1000 * 1000;
  nanosleep(&pause, NULL);
  for (i = 0; i < 100; i++) {
    int fd = closed_write_fd();
    check(fd >= 0, "two thread pipe");
    capnp_write_all(fd, &byte, 1);
    close(fd);
  }
  pause.tv_nsec = 200 * 1000 * 1000;
  nanosleep(&pause, NULL);
  check(g_waiter_hits == 0,
        "waiter saw no SIGPIPE from closed-pipe writes");
  check(kill(getpid(), SIGPIPE) == 0, "explicit process sigpipe");
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 2;
  check(sem_timedwait(&g_waiter_got, &deadline) == 0 && g_waiter_hits == 1,
        "waiter received the explicit process SIGPIPE");

  g_waiter_hits = 0;
  check(pthread_create(&writer, NULL, sigpipe_writer, NULL) == 0,
        "create writer");
  pause.tv_nsec = 1000 * 1000;
  for (i = 0; i < 5000 && g_writer_writes == 0; i++)
    nanosleep(&pause, NULL);
  check(g_writer_writes > 0, "writer running before the signals start");
  lost = 0;
  nsend = 40;
  for (i = 0; i < nsend && !lost; i++) {
    struct timespec step;
    check(kill(getpid(), SIGPIPE) == 0, "stress process sigpipe");
    clock_gettime(CLOCK_REALTIME, &step);
    step.tv_sec += 2;
    if (sem_timedwait(&g_waiter_got, &step) != 0)
      lost++;
  }
  g_writer_stop = 1;
  pthread_join(writer, NULL);
  g_waiter_stop = 1;
  pthread_join(waiter, NULL);
  check(lost == 0 && g_waiter_hits == nsend,
        "process SIGPIPE not swallowed beside closed-pipe writes");
  sem_destroy(&g_waiter_got);
  sem_destroy(&g_waiter_ready);
  sigaction(SIGPIPE, &old_sa, NULL);
  drain_sigpipe();
  check(pthread_sigmask(SIG_SETMASK, &old, NULL) == 0,
        "restore two thread sigpipe mask");
}
#endif

/* --- vat-diff: the same frames to capnp-C++ and to this vat ---------- */

enum {
  VF_BOOT,
  VF_CALL,
  VF_RELEASE,
  VF_UNIMPL,
  VF_UNIMPL_RESOLVE,
  VF_FINISH,
  VF_RETURN,
  VF_ABORT
};
enum { VT_IMPORT, VT_PROMISED, VT_UNKNOWN, VT_NULL };
#define NULL_ANSWER 0xffffffffu

typedef struct {
  int kind;
  /* Question id, or the export a Release names. */
  uint32_t qid;
  int target;
  /* The export or question a call targets. */
  uint32_t tval;
  uint16_t send_to;
  uint16_t method;
  /* Release count; for a Bootstrap, nonzero sends a deprecatedObjectId;
   * for a promised target, how many getPointerField 0 steps follow. For
   * an Unimplemented Resolve, `method` is the descriptor arm naming
   * export `tval`, and `send_to` nonzero sends an exception. */
  uint32_t count;
} vframe_t;

/* After a Bootstrap on question 0 has been answered, `frames` go to each
 * vat, and the replies up to an Abort or the Return for `until` are
 * compared. `opaque` compares only which exception reasons repeat, for
 * a method whose own error text is the server's business. `known` marks
 * a difference listed in the notes, printed without failing. */
typedef struct {
  const char *name;
  vframe_t frames[3];
  int nframes;
  uint32_t until;
  int opaque;
  int known;
} vscenario_t;

static const vscenario_t vat_diffs[] = {
    {"call to an unknown export",
     {{VF_CALL, 1, VT_IMPORT, 99, 0, 0, 0}}, 1, 1, 0, 0},
    {"call pipelined on a missing answer",
     {{VF_CALL, 1, VT_PROMISED, 50, 0, 0, 0}}, 1, 1, 0, 0},
    {"call pipelined on the bootstrap answer",
     {{VF_CALL, 1, VT_PROMISED, 0, 0, 0, 0}}, 1, 1, 0, 0},
    {"call pipelined on a finished answer",
     {{VF_CALL, 1, VT_IMPORT, 0, 0, 0, 0},
      {VF_FINISH, 1, 0, 0, 0, 0, 0},
      {VF_CALL, 2, VT_PROMISED, 1, 0, 0, 0}}, 3, 2, 0, 0},
    {"call pipelined on a failed answer",
     {{VF_CALL, 1, VT_IMPORT, 0, 0, 9, 0},
      {VF_CALL, 2, VT_PROMISED, 1, 0, 0, 0}}, 2, 2, 1, 0},
    {"call with a null target",
     {{VF_CALL, 1, VT_NULL, 0, 0, 0, 0}}, 1, 1, 0, 0},
    {"call with an unknown target arm",
     {{VF_CALL, 1, VT_UNKNOWN, 0, 0, 0, 0}}, 1, 1, 0, 0},
    {"sendResultsTo.thirdParty",
     {{VF_CALL, 1, VT_IMPORT, 0, 2, 0, 0}}, 1, 1, 0, 0},
    {"sendResultsTo unknown arm",
     {{VF_CALL, 1, VT_IMPORT, 0, 7, 0, 0}}, 1, 1, 0, 0},
    {"call reusing an active question",
     {{VF_CALL, 0, VT_IMPORT, 0, 0, 0, 0}}, 1, 99, 0, 0},
    {"call with results sent to yourself reusing an active question",
     {{VF_CALL, 0, VT_IMPORT, 0, 1, 0, 0}}, 1, 99, 0, 0},
    {"call on a null promisedAnswer",
     {{VF_CALL, 1, VT_PROMISED, NULL_ANSWER, 0, 0, 0}}, 1, 1, 0, 0},
    {"bootstrap reusing an active question",
     {{VF_BOOT, 0, 0, 0, 0, 0, 0}}, 1, 0, 0, 0},
    {"bootstrap with deprecatedObjectId",
     {{VF_BOOT, 1, 0, 0, 0, 0, 1}}, 1, 1, 0, 0},
    {"Finish for an unknown question",
     {{VF_FINISH, 33, 0, 0, 0, 0, 0},
      {VF_CALL, 1, VT_IMPORT, 0, 0, 0, 0}}, 2, 1, 0, 0},
    {"Return for an unknown question",
     {{VF_RETURN, 5, 0, 0, 0, 0, 0}}, 1, 5, 0, 0},
    {"Unimplemented for a Call",
     {{VF_UNIMPL, 1, VT_IMPORT, 0, 0, 0, 0}}, 1, 99, 0, 0},
    {"Unimplemented for a Resolve of the bootstrap",
     {{VF_UNIMPL_RESOLVE, 0, 0, 0, 0, 1, 0},
      {VF_CALL, 1, VT_IMPORT, 0, 0, 0, 0}}, 2, 1, 0, 0},
    {"Unimplemented for a Resolve of an unknown export",
     {{VF_UNIMPL_RESOLVE, 0, 0, 77, 0, 2, 0}}, 1, 99, 0, 0},
    {"Unimplemented for a Resolve to an exception",
     {{VF_UNIMPL_RESOLVE, 0, 0, 0, 1, 0, 0},
      {VF_CALL, 1, VT_IMPORT, 0, 0, 0, 0}}, 2, 1, 0, 0},
    {"call pipelined through the bootstrap capability",
     {{VF_CALL, 1, VT_PROMISED, 0, 0, 0, 1}}, 1, 1, 0, 0},
    {"call pipelined through a field with no capability",
     {{VF_CALL, 1, VT_IMPORT, 0, 0, 0, 0},
      {VF_CALL, 2, VT_PROMISED, 1, 0, 0, 1}}, 2, 2, 0, 0},
    {"call pipelined on results that are no capability",
     {{VF_CALL, 1, VT_IMPORT, 0, 0, 0, 0},
      {VF_CALL, 2, VT_PROMISED, 1, 0, 0, 0}}, 2, 2, 0, 0},
    {"Abort",
     {{VF_ABORT, 0, 0, 0, 0, 0, 0}}, 1, 99, 0, 0},
    {"Abort carrying a remote exception",
     {{VF_ABORT, 0, 0, 0, 1, 0, 0}}, 1, 99, 0, 1},
    {"Release of an unknown export",
     {{VF_RELEASE, 99, 0, 0, 0, 0, 1}}, 1, 99, 0, 0},
    {"Release beyond the reference count",
     {{VF_RELEASE, 0, 0, 0, 0, 0, 5}}, 1, 99, 0, 0},
    {"Release of the last reference",
     {{VF_RELEASE, 0, 0, 0, 0, 0, 1},
      {VF_CALL, 1, VT_IMPORT, 0, 0, 0, 0}}, 2, 1, 0, 0},
    {"Finish releasing a released result",
     {{VF_RELEASE, 0, 0, 0, 0, 0, 1},
      {VF_FINISH, 0, 0, 0, 0, 0, 0}}, 2, 99, 0, 0},
};
#define N_VAT_DIFFS (sizeof vat_diffs / sizeof vat_diffs[0])

/* Method 0 adds; any other method fails, as an unknown one does in C++. */
static int vd_dispatch(void *server, uint64_t interface_id, uint16_t method_id,
                       const capnp_ptr_t *params, const capnp_bptr_t *results) {
  if (method_id != 0)
    return CAPNP_ERR_ARG;
  return adder_dispatch(server, interface_id, method_id, params, results);
}

static int build_target(const capnp_bptr_t *call, const vframe_t *f) {
  capnp_bptr_t slot, target, pa;
  if (f->target == VT_NULL)
    return 0;
  if (capnp_builder_slot(call, 3, 0, &slot) ||
      capnp_builder_struct(&slot, 1, 1, &target))
    return -1;
  switch (f->target) {
  case VT_IMPORT:
    return capnp_builder_set_u32(&target, 0, f->tval);
  case VT_PROMISED: {
    capnp_bptr_t ops;
    uint32_t i;
    /* A `tval` of NULL_ANSWER leaves the promisedAnswer null. */
    if (f->tval == NULL_ANSWER)
      return capnp_builder_set_u16(&target, 4, 1);
    if (capnp_builder_set_u16(&target, 4, 1) ||
        capnp_builder_slot(&target, 1, 0, &slot) ||
        capnp_builder_struct(&slot, 1, 1, &pa) ||
        capnp_builder_set_u32(&pa, 0, f->tval))
      return -1;
    if (f->count == 0)
      return 0;
    if (capnp_builder_set_list_struct(&pa, 1, 0, f->count, 1, 0, &ops))
      return -1;
    for (i = 0; i < f->count; i++) {
      capnp_bptr_t e = ops;
      e.word += i;
      if (capnp_builder_set_u16(&e, 0, 1))
        return -1;
    }
    return 0;
  }
  default:
    return capnp_builder_set_u16(&target, 4, 2);
  }
}

static int build_vcall(const capnp_bptr_t *slot0, const vframe_t *f) {
  capnp_bptr_t call, slot, payload, content;
  return capnp_builder_struct(slot0, 3, 3, &call) ||
         capnp_builder_set_u32(&call, 0, f->qid) ||
         capnp_builder_set_u16(&call, 4, f->method) ||
         capnp_builder_set_u16(&call, 6, f->send_to) ||
         capnp_builder_set_u64(&call, 8, ADDER_IFACE) ||
         build_target(&call, f) ||
         capnp_builder_slot(&call, 3, 1, &slot) ||
         capnp_builder_struct(&slot, 0, 2, &payload) ||
         capnp_builder_slot(&payload, 0, 0, &slot) ||
         capnp_builder_struct(&slot, 2, 0, &content) ||
         capnp_builder_set_u64(&content, 0, 20) ||
         capnp_builder_set_u64(&content, 8, 22);
}

static int build_vframe(const vframe_t *f, uint8_t **out, size_t *len) {
  capnp_builder_t b;
  capnp_bptr_t root, msg, slot, body, inner;
  int rc;
  if (f->kind == VF_RETURN)
    return build_return(f->qid, 0, 0, out, len);
  if (f->kind == VF_ABORT) {
    /* `send_to` nonzero sends what rpc.c++ answers an Abort with. */
    const char *why = f->send_to ? "remote exception: gone" : "gone";
    capnp_builder_init(&b);
    rc = capnp_builder_root(&b, &root) ||
         capnp_builder_struct(&root, 1, 1, &msg) ||
         capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_ABORT) ||
         capnp_builder_slot(&msg, 1, 0, &slot) ||
         capnp_builder_struct(&slot, 1, 2, &body) ||
         capnp_builder_set_text(&body, 1, 0, why, strlen(why)) ||
         capnp_builder_serialize(&b, out, len);
    capnp_builder_free(&b);
    return rc;
  }
  capnp_builder_init(&b);
  rc = capnp_builder_root(&b, &root) ||
       capnp_builder_struct(&root, 1, 1, &msg) ||
       capnp_builder_slot(&msg, 1, 0, &slot);
  if (rc == 0) {
    switch (f->kind) {
    case VF_BOOT:
      rc = capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_BOOTSTRAP) ||
           capnp_builder_struct(&slot, 1, 1, &body) ||
           capnp_builder_set_u32(&body, 0, f->qid);
      if (rc == 0 && f->count)
        rc = capnp_builder_slot(&body, 1, 0, &slot) ||
             capnp_builder_struct(&slot, 1, 0, &inner) ||
             capnp_builder_set_u32(&inner, 0, f->count);
      break;
    case VF_CALL:
      rc = capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_CALL) ||
           build_vcall(&slot, f);
      break;
    case VF_RELEASE:
      rc = capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_RELEASE) ||
           capnp_builder_struct(&slot, 1, 0, &body) ||
           capnp_builder_set_u32(&body, 0, f->qid) ||
           capnp_builder_set_u32(&body, 4, f->count);
      break;
    case VF_UNIMPL:
      /* Unimplemented carries the refused message: a Call here. */
      rc = capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_UNIMPLEMENTED) ||
           capnp_builder_struct(&slot, 1, 1, &inner) ||
           capnp_builder_set_u16(&inner, 0, CAPNP_RPC_MSG_CALL) ||
           capnp_builder_slot(&inner, 1, 0, &slot) || build_vcall(&slot, f);
      break;
    case VF_UNIMPL_RESOLVE: {
      capnp_bptr_t res, cap;
      rc = capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_UNIMPLEMENTED) ||
           capnp_builder_struct(&slot, 1, 1, &inner) ||
           capnp_builder_set_u16(&inner, 0, CAPNP_RPC_MSG_RESOLVE) ||
           capnp_builder_slot(&inner, 1, 0, &slot) ||
           capnp_builder_struct(&slot, 1, 1, &res) ||
           capnp_builder_set_u16(&res, 4, f->send_to ? 1 : 0) ||
           capnp_builder_slot(&res, 1, 0, &slot);
      if (rc == 0 && f->send_to)
        rc = capnp_builder_struct(&slot, 1, 2, &cap) ||
             capnp_builder_set_text(&cap, 1, 0, "gone", 4);
      else if (rc == 0)
        rc = capnp_builder_struct(&slot, 1, 1, &cap) ||
             capnp_builder_set_u16(&cap, 0, f->method) ||
             capnp_builder_set_u32(&cap, 4, f->tval);
      break;
    }
    default:
      rc = capnp_builder_set_u16(&msg, 0, CAPNP_RPC_MSG_FINISH) ||
           capnp_builder_struct(&slot, 1, 0, &body) ||
           capnp_builder_set_u32(&body, 0, f->qid);
      break;
    }
  }
  if (rc == 0)
    rc = capnp_builder_serialize(&b, out, len);
  capnp_builder_free(&b);
  return rc;
}

/* Append one reply to `out`; 1 when it ends the exchange. */
static int describe_reply(const uint8_t *frame, size_t n, uint32_t until,
                          char *out, size_t cap) {
  capnp_message_t m = {0};
  capnp_ptr_t root, body, p, content;
  char ev[320];
  const char *text = NULL;
  size_t tn = 0;
  int end = 0;
  uint16_t tag;
  snprintf(ev, sizeof ev, "unreadable");
  if (capnp_message_from_flat(&m, frame, n) == CAPNP_OK &&
      capnp_root(&m, &root) == CAPNP_OK) {
    tag = capnp_get_u16(&root, 0, 0);
    if (capnp_getp(&root, 0, &body) != CAPNP_OK)
      memset(&body, 0, sizeof body);
    if (tag == CAPNP_RPC_MSG_ABORT) {
      if (capnp_get_text(&body, 0, &text, &tn) != CAPNP_OK || text == NULL)
        tn = 0;
      snprintf(ev, sizeof ev, "abort: %.*s", (int)tn, tn ? text : "");
      end = 1;
    } else if (tag == CAPNP_RPC_MSG_RETURN) {
      uint32_t aid = capnp_get_u32(&body, 0, 0);
      uint16_t arm = capnp_get_u16(&body, 6, 0);
      if (arm == 0) {
        long long sum = -1;
        if (capnp_getp(&body, 0, &p) == CAPNP_OK &&
            capnp_getp(&p, 0, &content) == CAPNP_OK &&
            content.kind == CAPNP_PK_STRUCT)
          sum = (long long)capnp_get_u64(&content, 0, 0);
        snprintf(ev, sizeof ev, "return %u results %lld", (unsigned)aid,
                 sum);
      } else if (arm == 1) {
        if (capnp_getp(&body, 0, &p) != CAPNP_OK ||
            capnp_get_text(&p, 0, &text, &tn) != CAPNP_OK || text == NULL)
          tn = 0;
        snprintf(ev, sizeof ev, "return %u exception: %.*s", (unsigned)aid,
                 (int)tn, tn ? text : "");
      } else {
        snprintf(ev, sizeof ev, "return %u arm %u", (unsigned)aid,
                 (unsigned)arm);
      }
      end = aid == until;
    } else {
      snprintf(ev, sizeof ev, "message %u", (unsigned)tag);
    }
  }
  capnp_message_free(&m);
  if (out[0] && strlen(out) + 2 < cap)
    strcat(out, " | ");
  strncat(out, ev, cap - strlen(out) - 1);
  return end;
}

/* Replace each distinct exception reason with R1, R2, ... in order. */
static void opaque_reasons(char *s) {
  char seen[4][256];
  int nseen = 0;
  char out[1024] = "";
  char *p = s;
  for (;;) {
    char *e = strstr(p, "exception: "), *stop;
    size_t len;
    int k;
    if (e == NULL) {
      strncat(out, p, sizeof out - strlen(out) - 1);
      break;
    }
    e += strlen("exception: ");
    strncat(out, p, (size_t)(e - p) < sizeof out - strlen(out) - 1
                        ? (size_t)(e - p)
                        : sizeof out - strlen(out) - 1);
    stop = strstr(e, " | ");
    len = stop ? (size_t)(stop - e) : strlen(e);
    if (len > 255)
      len = 255;
    for (k = 0; k < nseen; k++)
      if (strlen(seen[k]) == len && memcmp(seen[k], e, len) == 0)
        break;
    if (k == nseen && nseen < 4) {
      memcpy(seen[nseen], e, len);
      seen[nseen][len] = 0;
      nseen++;
    }
    {
      char tok[16];
      snprintf(tok, sizeof tok, "R%d", k + 1);
      strncat(out, tok, sizeof out - strlen(out) - 1);
    }
    p = e + (stop ? (size_t)(stop - e) : strlen(e));
  }
  strcpy(s, out);
}

#define VD_MAX_SENT 16
static uint8_t *g_vd_sent[VD_MAX_SENT];
static size_t g_vd_len[VD_MAX_SENT];
static int g_vd_n;

static int vd_capture(void *ctx, const uint8_t *data, size_t len) {
  (void)ctx;
  if (g_vd_n >= VD_MAX_SENT)
    return 0;
  g_vd_sent[g_vd_n] = (uint8_t *)malloc(len);
  if (g_vd_sent[g_vd_n] == NULL)
    return -1;
  memcpy(g_vd_sent[g_vd_n], data, len);
  g_vd_len[g_vd_n++] = len;
  return 0;
}

static void vd_clear(void) {
  while (g_vd_n > 0)
    free(g_vd_sent[--g_vd_n]);
}

static void vd_local(const vscenario_t *s, char *out, size_t cap) {
  static capnp_rpc_conn_t c;
  uint8_t *f = NULL;
  size_t n = 0;
  int i, end = 0;
  vframe_t boot = {VF_BOOT, 0, 0, 0, 0, 0, 0};
  out[0] = 0;
  capnp_rpc_init(&c, vd_capture, NULL);
  capnp_rpc_set_bootstrap(&c, &g_server_obj, vd_dispatch);
  if (build_vframe(&boot, &f, &n) == 0)
    (void)capnp_rpc_handle(&c, f, n);
  free(f);
  vd_clear();
  for (i = 0; i < s->nframes; i++) {
    f = NULL;
    if (build_vframe(&s->frames[i], &f, &n) == 0)
      (void)capnp_rpc_handle(&c, f, n);
    free(f);
  }
  for (i = 0; i < g_vd_n && !end; i++)
    end = describe_reply(g_vd_sent[i], g_vd_len[i], s->until, out, cap);
  if (!end)
    strncat(out, out[0] ? " | no end" : "no end", cap - strlen(out) - 1);
  vd_clear();
  capnp_rpc_free(&c);
}

static int vd_read(int fd, uint8_t **frame, size_t *n) {
  struct pollfd p;
  p.fd = fd;
  p.events = POLLIN;
  if (poll(&p, 1, 1500) <= 0)
    return -1;
  return capnp_read_frame_alloc(fd, frame, n) == CAPNP_OK ? 0 : -2;
}

static void vd_remote(uint16_t port, const vscenario_t *s, char *out,
                      size_t cap) {
  struct sockaddr_in addr;
  uint8_t *f = NULL;
  size_t n = 0;
  int fd, i, end = 0, got_boot = 0;
  vframe_t boot = {VF_BOOT, 0, 0, 0, 0, 0, 0};
  out[0] = 0;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(0x7f000001);
  if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    snprintf(out, cap, "connect failed");
    if (fd >= 0)
      close(fd);
    return;
  }
  if (build_vframe(&boot, &f, &n) == 0)
    (void)capnp_write_all(fd, f, n);
  free(f);
  while (!got_boot) {
    char tmp[320] = "";
    f = NULL;
    if (vd_read(fd, &f, &n) != 0) {
      snprintf(out, cap, "no bootstrap Return");
      close(fd);
      return;
    }
    got_boot = describe_reply(f, n, 0, tmp, sizeof tmp);
    free(f);
  }
  for (i = 0; i < s->nframes; i++) {
    f = NULL;
    if (build_vframe(&s->frames[i], &f, &n) == 0)
      (void)capnp_write_all(fd, f, n);
    free(f);
  }
  while (!end) {
    int r;
    f = NULL;
    r = vd_read(fd, &f, &n);
    if (r != 0) {
      const char *why = r == -1 ? "no end" : "closed";
      if (out[0])
        strncat(out, " | ", cap - strlen(out) - 1);
      strncat(out, why, cap - strlen(out) - 1);
      break;
    }
    end = describe_reply(f, n, s->until, out, cap);
    free(f);
  }
  close(fd);
}

/* `vat-diff <port>`: each scenario against `rpc_peer_server <port>` and
 * against this vat, printing both outcomes. */
static int vat_diff(uint16_t port) {
  size_t i;
  int failed = 0;
  for (i = 0; i < N_VAT_DIFFS; i++) {
    char cpp[1024], ours[1024];
    vd_remote(port, &vat_diffs[i], cpp, sizeof cpp);
    vd_local(&vat_diffs[i], ours, sizeof ours);
    if (vat_diffs[i].opaque) {
      opaque_reasons(cpp);
      opaque_reasons(ours);
    }
    if (strcmp(cpp, ours) == 0) {
      printf("same %s: %s\n", vat_diffs[i].name, cpp);
    } else if (vat_diffs[i].known) {
      printf("known difference %s: capnp-C++ \"%s\", this vat \"%s\"\n",
             vat_diffs[i].name, cpp, ours);
    } else {
      fprintf(stderr, "FAIL %s: capnp-C++ \"%s\", this vat \"%s\"\n",
              vat_diffs[i].name, cpp, ours);
      failed++;
    }
  }
  return failed != 0;
}

/* What capnp-C++ 1.4.0 answered in vat-diff, so this vat is checked
 * against it with no C++ peer running. */
static const struct {
  const char *name;
  const char *cpp;
} vat_recorded[] = {
    {"Unimplemented for a Call",
     "abort: Peer did not implement required RPC message type.; "
     "(uint)message.which() = 2"},
    {"Unimplemented for a Resolve of the bootstrap",
     "abort: Message target is not a current export ID."},
    {"Unimplemented for a Resolve of an unknown export",
     "abort: Tried to release invalid export ID."},
    {"Unimplemented for a Resolve to an exception", "return 1 results 42"},
    {"call pipelined through the bootstrap capability",
     "return 1 exception: Invalid pipeline transform."},
    {"call pipelined through a field with no capability",
     "return 1 results 42 | return 2 exception: Pipeline call on a request "
     "that returned no capabilities or was already closed."},
    {"call pipelined on results that are no capability",
     "return 1 results 42 | return 2 exception: Pipeline call on a request "
     "that returned no capabilities or was already closed."},
};

static void test_vat_recorded(void) {
  size_t i, j;
  for (i = 0; i < sizeof vat_recorded / sizeof vat_recorded[0]; i++) {
    char ours[1024] = "";
    for (j = 0; j < N_VAT_DIFFS; j++)
      if (strcmp(vat_diffs[j].name, vat_recorded[i].name) == 0)
        break;
    if (j < N_VAT_DIFFS) {
      vd_local(&vat_diffs[j], ours, sizeof ours);
      if (vat_diffs[j].opaque)
        opaque_reasons(ours);
    }
    if (strcmp(ours, vat_recorded[i].cpp) != 0) {
      fprintf(stderr, "FAIL recorded %s: capnp-C++ \"%s\", this vat \"%s\"\n",
              vat_recorded[i].name, vat_recorded[i].cpp, ours);
      g_fail++;
    }
  }
}

int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "dial") == 0)
    return dial_peer((uint16_t)atoi(argv[2]));
  if (argc >= 3 && strcmp(argv[1], "serve") == 0)
    return serve_peer((uint16_t)atoi(argv[2]));
  if (argc >= 3 && strcmp(argv[1], "bad-return") == 0)
    return serve_bad_returns((uint16_t)atoi(argv[2]));
  if (argc >= 3 && strcmp(argv[1], "vat-diff") == 0)
    return vat_diff((uint16_t)atoi(argv[2]));
  test_ipv4_literal();
  test_frame_little_endian();
  test_frame_segment_limit();
  test_frame_alloc_limit();
  test_bootstrap_frame_little_endian();
  test_size_to_i32();
  test_write_pipe();
  test_write_closed_socket();
  test_write_closed_pipe();
  test_write_pending_sigpipe();
#if !defined(__APPLE__) && !defined(__OpenBSD__)
  test_write_process_sigpipe();
  test_write_sigpipe_two_threads();
#endif
  test_unix_replaces_only_socket();
  test_unix_keeps_live_socket();
  test_link_setup_closes_fd();
  test_link_large_result(10000, "large link result");
  test_link_large_result(70000, "link result over 64 KiB");
  loopback(1);
  loopback(0);
  test_vat_recorded();
  if (g_fail) {
    fprintf(stderr, "%d transport checks failed\n", g_fail);
    return 1;
  }
  printf("ok transport\n");
  return 0;
}
