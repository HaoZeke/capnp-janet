/*
 * Stub for capnpc-janet: a capnp compile -o plugin.
 *
 * Real plugin will read CodeGeneratorRequest (like capnpc-c / capnpc-fortran)
 * and emit Janet modules and/or C accessor tables.
 *
 * Build later: meson option + install as capnpc-janet on PATH.
 */
#include <stdio.h>

int main(void) {
  fprintf(stderr,
          "capnpc-janet: not implemented yet (see README roadmap v0.3)\n");
  return 1;
}
