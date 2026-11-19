#define _POSIX_C_SOURCE 200809L
/* capnp eval equivalent: print a const or a field default as text.
 *
 * The schema is parsed by `capnp compile -o-`, which is the compiler's
 * CodeGeneratorRequest. This program does not reimplement the schema language.
 */
#include <capnp-janet/capnp_dyn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(FILE *f, size_t *len) {
  char *buf = NULL;
  size_t n = 0, cap = 0;
  for (;;) {
    if (n == cap) {
      size_t ncap = cap ? cap * 2 : 4096;
      char *p = (char *)realloc(buf, ncap);
      if (!p) {
        free(buf);
        return NULL;
      }
      buf = p;
      cap = ncap;
    }
    size_t got = fread(buf + n, 1, cap - n, f);
    n += got;
    if (got == 0)
      break;
  }
  *len = n;
  return buf;
}

int main(int argc, char **argv) {
  char cmd[1024];
  FILE *pipe;
  char *req = NULL;
  size_t req_len = 0;
  capnp_dyn_t *dyn = NULL;
  char *text = NULL;
  size_t text_len = 0;
  int i;
  if (argc != 3) {
    fprintf(stderr, "usage: capnp-eval schema.capnp name\n");
    return 2;
  }
  for (i = 0; argv[1][i]; i++) {
    if (argv[1][i] == '\'' || argv[1][i] == '"' || argv[1][i] == '\n') {
      fprintf(stderr, "capnp-eval: schema path has a quote\n");
      return 2;
    }
  }
  snprintf(cmd, sizeof(cmd), "capnp compile -o- '%s'", argv[1]);
  pipe = popen(cmd, "r");
  if (!pipe) {
    fprintf(stderr, "capnp-eval: could not run capnp compile\n");
    return 1;
  }
  req = read_all(pipe, &req_len);
  if (pclose(pipe) != 0 || !req) {
    fprintf(stderr, "capnp-eval: capnp compile failed\n");
    free(req);
    return 1;
  }
  if (capnp_dyn_init(&dyn, (const uint8_t *)req, req_len) != 0) {
    fprintf(stderr, "capnp-eval: could not read the request\n");
    free(req);
    return 1;
  }
  free(req);
  if (capnp_dyn_eval(dyn, argv[2], &text, &text_len) != 0) {
    fprintf(stderr, "capnp-eval: could not evaluate %s\n", argv[2]);
    capnp_dyn_free(dyn);
    return 1;
  }
  fwrite(text, 1, text_len, stdout);
  fputc('\n', stdout);
  free(text);
  capnp_dyn_free(dyn);
  return 0;
}
