#ifndef CAPNP_JANET_BUILDER_H
#define CAPNP_JANET_BUILDER_H

#include <capnp-janet/capnp_message.h>
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
int capnp_builder_set_u16(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, uint16_t value);
int capnp_builder_set_u32(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, uint32_t value);
int capnp_builder_set_u64(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, uint64_t value);
int capnp_builder_set_f64(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, double value);
int capnp_builder_set_bool(capnp_builder_t *b, size_t body_word,
                           uint32_t bit_offset, int value);

/*
 * Point @slot at an existing pointer word in a struct body so callers can
 * nest with capnp_builder_struct (AddressBook people[i], Expression trees).
 */
int capnp_builder_slot(capnp_builder_t *b, size_t body_word, uint16_t dwords,
                       uint16_t ptr_index, capnp_bptr_t *slot);

/* Set pointer slot of a struct to Text. */
int capnp_builder_set_text(capnp_builder_t *b, size_t body_word,
                           uint16_t dwords, uint16_t ptr_index, const char *text,
                           size_t text_len);

/* Set pointer slot to List(Text). */
int capnp_builder_set_list_text(capnp_builder_t *b, size_t body_word,
                                uint16_t dwords, uint16_t ptr_index,
                                const char *const *items, uint32_t nitems);

/* Data = List(UInt8) without requiring trailing NUL (Text always has NUL). */
int capnp_builder_set_data(capnp_builder_t *b, size_t body_word,
                           uint16_t dwords, uint16_t ptr_index,
                           const uint8_t *data, size_t data_len);

/* Primitive lists (element sizes 2/3/4/5). */
int capnp_builder_set_list_u8(capnp_builder_t *b, size_t body_word,
                              uint16_t dwords, uint16_t ptr_index,
                              const uint8_t *items, uint32_t nitems);
int capnp_builder_set_list_u16(capnp_builder_t *b, size_t body_word,
                               uint16_t dwords, uint16_t ptr_index,
                               const uint16_t *items, uint32_t nitems);
int capnp_builder_set_list_u32(capnp_builder_t *b, size_t body_word,
                               uint16_t dwords, uint16_t ptr_index,
                               const uint32_t *items, uint32_t nitems);
int capnp_builder_set_list_u64(capnp_builder_t *b, size_t body_word,
                               uint16_t dwords, uint16_t ptr_index,
                               const uint64_t *items, uint32_t nitems);
int capnp_builder_set_list_f64(capnp_builder_t *b, size_t body_word,
                               uint16_t dwords, uint16_t ptr_index,
                               const double *items, uint32_t nitems);

/* List(Bool): items[i] non-zero = true. Bits packed LSB-first within each byte. */
int capnp_builder_set_list_bool(capnp_builder_t *b, size_t body_word,
                                uint16_t dwords, uint16_t ptr_index,
                                const uint8_t *items, uint32_t nitems);

/* List(Void): length only; no element payload on the wire. */
int capnp_builder_set_list_void(capnp_builder_t *b, size_t body_word,
                                uint16_t dwords, uint16_t ptr_index,
                                uint32_t nitems);

/*
 * Set pointer slot to List(Struct) composite list.
 * Each element has elem_dwords data words and elem_pwords pointer words.
 * On success, *first_elem_body is the body word of element 0 (then stride
 * elem_dwords+elem_pwords).
 */
int capnp_builder_set_list_struct(capnp_builder_t *b, size_t body_word,
                                  uint16_t dwords, uint16_t ptr_index,
                                  uint32_t nitems, uint16_t elem_dwords,
                                  uint16_t elem_pwords,
                                  size_t *first_elem_body);

/* Pointer-slot word of a struct body (for nested set_text etc.). */
size_t capnp_builder_ptr_word(size_t body_word, uint16_t dwords,
                              uint16_t ptr_index);

/*
 * Deep-copy a resolved pointer (struct/list/text/null) from any message into
 * @slot (like C++ set() / capnp-fortran cross-message setp). Preorder layout.
 */
int capnp_builder_copy_ptr(capnp_bptr_t *slot, const capnp_ptr_t *src);

/*
 * Stream-frame the single segment into *out (malloc). Caller frees.
 * *out_len is total framed length.
 */
int capnp_builder_serialize(const capnp_builder_t *b, uint8_t **out,
                            size_t *out_len);

/* After struct init: body starts at body_word; pointer slots at body+dwords. */
size_t capnp_builder_struct_body(const capnp_bptr_t *struct_ptr);

#endif /* CAPNP_JANET_BUILDER_H */
