/* Cap'n Proto wire constants and error codes for the Janet runtime. */
#ifndef CAPNP_JANET_KINDS_H
#define CAPNP_JANET_KINDS_H

#include <stdint.h>

enum {
  CAPNP_OK = 0,
  CAPNP_ERR_BOUNDS = 1,
  CAPNP_ERR_KIND = 2,
  CAPNP_ERR_DEPTH = 3,
  CAPNP_ERR_TRAVERSAL = 4,
  CAPNP_ERR_ALLOC = 5,
  CAPNP_ERR_FRAMING = 6,
  CAPNP_ERR_PACKED = 7,
  CAPNP_ERR_ARG = 8,
  CAPNP_ERR_SEGMENT = 9,
  CAPNP_ERR_IO = 10
};

/* Wire pointer kinds (bits 0-1). */
enum {
  CAPNP_WK_STRUCT = 0,
  CAPNP_WK_LIST = 1,
  CAPNP_WK_FAR = 2,
  CAPNP_WK_CAP = 3
};

/* Resolved pointer kinds. */
enum {
  CAPNP_PK_NULL = 0,
  CAPNP_PK_STRUCT = 1,
  CAPNP_PK_LIST = 2,
  CAPNP_PK_CAP = 3
};

/* List element size codes (bits 32-34 of a list pointer). */
enum {
  CAPNP_SZ_VOID = 0,
  CAPNP_SZ_BIT = 1,
  CAPNP_SZ_BYTE = 2,
  CAPNP_SZ_TWO = 3,
  CAPNP_SZ_FOUR = 4,
  CAPNP_SZ_EIGHT = 5,
  CAPNP_SZ_PTR = 6,
  CAPNP_SZ_COMPOSITE = 7
};

#define CAPNP_WORD_BYTES 8u
#define CAPNP_DEFAULT_TRAVERSAL_WORDS 8388608ull
#define CAPNP_DEFAULT_DEPTH_LIMIT 64

#endif /* CAPNP_JANET_KINDS_H */
