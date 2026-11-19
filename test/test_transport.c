/* The vat dials and listens on TCP and Unix sockets itself.
 *
 * With no arguments, two vats on a Unix socket and two on TCP loopback
 * call an adder and check the sum. `dial <port>` calls the capnp-C++
 * Adder. `serve <port>` listens and answers one C++ client.
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>
#include <capnp-janet/capnp_transport.h>

#include <dirent.h>
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
    capnp_message_t m = {0};
    capnp_ptr_t content;
    check(capnp_rpc_answer_content(&link->conn, qcall, &m, &content) == CAPNP_OK,
          "content");
    check((int64_t)capnp_get_u64(&content, 0, 0) == 42, what);
    capnp_message_free(&m);
  }
  (void)i;
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

static void test_write_pipe(void) {
  int p[2];
  uint8_t byte = 7, got = 0;
  check(pipe(p) == 0, "pipe");
  check(capnp_write_all(p[1], &byte, 1) == CAPNP_OK, "write pipe");
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

int main(int argc, char **argv) {
  if (argc >= 3 && strcmp(argv[1], "dial") == 0)
    return dial_peer((uint16_t)atoi(argv[2]));
  if (argc >= 3 && strcmp(argv[1], "serve") == 0)
    return serve_peer((uint16_t)atoi(argv[2]));
  test_ipv4_literal();
  test_frame_little_endian();
  test_bootstrap_frame_little_endian();
  test_write_pipe();
  test_write_closed_socket();
  test_unix_replaces_only_socket();
  test_link_setup_closes_fd();
  loopback(1);
  loopback(0);
  if (g_fail) {
    fprintf(stderr, "%d transport checks failed\n", g_fail);
    return 1;
  }
  printf("ok transport\n");
  return 0;
}
