#ifndef CAPNP_JANET_INTERNAL_H
#define CAPNP_JANET_INTERNAL_H

/*
 * Entry points that capnp-eval needs and that are not part of the library
 * ABI. They have hidden visibility, so the shared library does not export
 * them; capnp-eval links the library objects directly. Not installed.
 */

#include <capnp-janet/capnp_builder.h>

#include <stddef.h>
#include <stdint.h>

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32) &&          \
    !defined(__CYGWIN__)
#define CAPNP_JANET_HIDDEN __attribute__((visibility("hidden")))
#else
#define CAPNP_JANET_HIDDEN
#endif

/* MallocMessageBuilder's allocation, for byte-identical output. Every
 * segment keeps the capacity it was created with. An object goes in the
 * segment of the pointer naming it when it fits; otherwise it goes in the
 * newest segment, or a new one of max(need, next size), behind a landing
 * pad. FIXED_SIZE keeps the next size at first_words; GROW_HEURISTICALLY
 * adds each new segment's size to it. max_seg_words and force_single do
 * not apply. */
enum { CAPNP_ALLOC_FIXED_SIZE = 0, CAPNP_ALLOC_GROW_HEURISTICALLY = 1 };

CAPNP_JANET_HIDDEN void capnp_builder_init_malloc(capnp_builder_t *b,
                                                  size_t first_words,
                                                  int strategy);

/* A zeroed list of void, bit or 1/2/4/8-byte elements. `start` (may be
 * NULL) receives the first content word, to fill in little-endian. */
CAPNP_JANET_HIDDEN int capnp_builder_init_list(const capnp_bptr_t *body,
                                               uint16_t dwords,
                                               uint16_t ptr_index, int esize,
                                               uint32_t nitems,
                                               capnp_bptr_t *start);

/* Nesting allowed when a `capnp_dyn_t` encodes text or JSON, or parses
 * text, for every instance in the process. Defaults to
 * CAPNP_DEFAULT_DEPTH_LIMIT. JSON parsing keeps the limit of 64 that C++
 * JsonCodec has. */
CAPNP_JANET_HIDDEN void capnp_dyn_set_max_depth(int depth);

#endif
