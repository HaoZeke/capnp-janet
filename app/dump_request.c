/* Plugin that writes the CodeGeneratorRequest from stdin to request.bin. */
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  size_t cap = 4096, n = 0;
  char *buf = (char *)malloc(cap);
  FILE *out;
  if (!buf)
    return 1;
  for (;;) {
    size_t got;
    if (n + 4096 > cap) {
      char *nb;
      cap *= 2;
      nb = (char *)realloc(buf, cap);
      if (!nb) {
        free(buf);
        return 1;
      }
      buf = nb;
    }
    got = fread(buf + n, 1, 4096, stdin);
    n += got;
    if (got < 4096)
      break;
  }
  out = fopen("request.bin", "wb");
  if (!out || fwrite(buf, 1, n, out) != n) {
    if (out)
      fclose(out);
    free(buf);
    return 1;
  }
  fclose(out);
  free(buf);
  return 0;
}
