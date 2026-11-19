/* Cap'n Proto wire constants and error codes for the Janet runtime. */
#ifndef CAPNP_JANET_KINDS_H
#define CAPNP_JANET_KINDS_H

#include <stddef.h>
#include <stdint.h>

/* Shared-library export. MSVC only writes an import library
 * (capnp_janet.lib) when at least one symbol is __declspec(dllexport),
 * so a Windows consumer cannot link a shared build without this. Static
 * builds (CAPNP_JANET_STATIC) and non-Windows leave it empty. Define
 * CAPNP_JANET_BUILDING when compiling the library objects.
 */
#if defined(CAPNP_JANET_STATIC)
#define CAPNP_JANET_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#ifdef CAPNP_JANET_BUILDING
#define CAPNP_JANET_EXPORT __declspec(dllexport)
#else
#define CAPNP_JANET_EXPORT __declspec(dllimport)
#endif
#else
#define CAPNP_JANET_EXPORT
#endif

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
  CAPNP_ERR_IO = 10,
  /* A schema value cannot describe a wire object, for example a
   * discriminant that lands past the struct's data section. */
  CAPNP_ERR_SCHEMA = 11
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
/* Same defaults as C++ ReaderOptions: 8 Mi words, nesting depth 64.
 * capnp_message_init stores them on the message. A read that would pass
 * either limit returns CAPNP_ERR_TRAVERSAL or CAPNP_ERR_DEPTH. */
#define CAPNP_DEFAULT_TRAVERSAL_WORDS 8388608ull
#define CAPNP_DEFAULT_DEPTH_LIMIT 64

/* Double a table capacity. Rejects a cap that cannot double in uint32_t
 * and a byte size that cannot be represented in size_t. */
static inline int capnp_u32_grow(uint32_t cap, size_t elem, uint32_t *out_cap,
                                 size_t *out_bytes) {
  uint32_t ncap;
  if (!out_cap || !out_bytes || elem == 0 || cap > UINT32_MAX / 2u)
    return CAPNP_ERR_BOUNDS;
  ncap = cap ? cap * 2u : 4u;
  if ((size_t)ncap > SIZE_MAX / elem)
    return CAPNP_ERR_BOUNDS;
  *out_cap = ncap;
  *out_bytes = (size_t)ncap * elem;
  return CAPNP_OK;
}

#endif /* CAPNP_JANET_KINDS_H */
