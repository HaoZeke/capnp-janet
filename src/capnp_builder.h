#ifndef CAPNP_JANET_BUILDER_H
#define CAPNP_JANET_BUILDER_H

#include "capnp_message.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Minimal single-segment builder for writing Cap'n messages from C/Janet.
 * Enough for PolicyDecision and similar small product structs.
 */

typedef struct capnp_builder {
  uint8_t *data;
  size_t words; /* used words */
  size_t cap;   /* capacity in words */
} capnp_builder_t;

typedef struct capnp_bptr {
  capnp_builder_t *b;
  size_t word; /* location of this pointer word in the segment */
} capnp_bptr_t;

void capnp_builder_init(capnp_builder_t *b);
void capnp_builder_free(capnp_builder_t *b);

/* Reserve root pointer word; returns pointer slot at word 0. */
int capnp_builder_root(capnp_builder_t *b, capnp_bptr_t *root);

/* Init struct at pointer location: allocates dwords+pwords body, writes ptr. */
int capnp_builder_struct(capnp_bptr_t *ptr, uint16_t dwords, uint16_t pwords,
                         capnp_bptr_t *body_out /* optional: body start as fake */);

/* Data writers on a struct body (word offset of body start). */
int capnp_builder_set_u32(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, uint32_t value);
int capnp_builder_set_bool(capnp_builder_t *b, size_t body_word,
                           uint32_t bit_offset, int value);

/* Set pointer slot of a struct to Text. */
int capnp_builder_set_text(capnp_builder_t *b, size_t body_word,
                           uint16_t dwords, uint16_t ptr_index, const char *text,
                           size_t text_len);

/* Set pointer slot to List(Text). */
int capnp_builder_set_list_text(capnp_builder_t *b, size_t body_word,
                                uint16_t dwords, uint16_t ptr_index,
                                const char *const *items, uint32_t nitems);

/*
 * Stream-frame the single segment into *out (malloc). Caller frees.
 * *out_len is total framed length.
 */
int capnp_builder_serialize(const capnp_builder_t *b, uint8_t **out,
                            size_t *out_len);

/* After struct init: body starts at body_word; pointer slots at body+dwords. */
size_t capnp_builder_struct_body(const capnp_bptr_t *struct_ptr);

#endif /* CAPNP_JANET_BUILDER_H */
