#ifndef CAPNP_JANET_INTERNAL_H
#define CAPNP_JANET_INTERNAL_H

/*
 * Entry points that capnp-eval needs and that are not part of the library
 * ABI. They have hidden visibility, so the shared library does not export
 * them; capnp-eval links the library objects directly. Not installed.
 */

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_dyn.h>
#include <capnp-janet/capnp_message.h>

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

/* Text (@json 0) or JSON encoding that reads a corrupt pointer as its
 * default and goes on, as `capnp convert` does. *wire_err receives the
 * first reader error, or CAPNP_OK. Fails with CAPNP_ERR_IO when the walk
 * enters a segment whose data is NULL. */
CAPNP_JANET_HIDDEN int capnp_dyn_encode_recover(capnp_dyn_t *d,
                                                const char *type_name,
                                                const capnp_ptr_t *s,
                                                int json, int pretty,
                                                char **out, size_t *len,
                                                int *wire_err);

/* capnp_dyn_decode_json, going on after the errors JsonCodec recovers
 * from: an integer out of range or a bad numeric string takes the value
 * kj substitutes, and a list field that is not an array is empty.
 * *soft is set when one occurred; `capnp convert` then fails the run. */
CAPNP_JANET_HIDDEN int capnp_dyn_decode_json_recover(capnp_dyn_t *d,
                                                     const char *type_name,
                                                     const char *text,
                                                     size_t len, uint8_t **out,
                                                     size_t *out_len,
                                                     int *soft);
/* capnp_dyn_decode_text, going on after the errors ValueTranslator
 * reports: a value of the wrong type or an unknown field is dropped, and
 * an integer out of range is clamped. *soft is set as above. */
CAPNP_JANET_HIDDEN int capnp_dyn_decode_text_recover(capnp_dyn_t *d,
                                                     const char *type_name,
                                                     const char *text,
                                                     size_t len, uint8_t **out,
                                                     size_t *out_len,
                                                     int *soft);
/* Canonical copy of @root, a struct or null, the way C++ `canonicalize`
 * copies: a corrupt pointer is written as null and sets *soft, and a
 * section is truncated by its raw words. The result may not be canonical
 * then; callers check. Fails with CAPNP_ERR_IO as above. */
CAPNP_JANET_HIDDEN int capnp_canonicalize_recover(const capnp_ptr_t *root,
                                                  uint8_t **out,
                                                  size_t *out_len, int *soft);

/* Bytes read ahead of the frame being handled, as C++
 * BufferedMessageStream buffers them: `data[begin..end)` is unread. */
#define CAPNP_RPC_INBUF_WORDS 8192u
typedef struct {
  uint8_t *data;
  size_t begin, end;
} capnp_rpc_inbuf_t;

struct capnp_rpc_conn;
/* The read-ahead buffer of a connection, NULL when out of memory. */
CAPNP_JANET_HIDDEN capnp_rpc_inbuf_t *
capnp_rpc_inbuf(const struct capnp_rpc_conn *c);
/* Send an Abort of Exception.type `type`, as rpc.c++ disconnect does,
 * unless this vat has sent one already, and fail every open question. */
CAPNP_JANET_HIDDEN int capnp_rpc_abort_typed(struct capnp_rpc_conn *c,
                                             uint16_t type,
                                             const char *reason);

#endif
