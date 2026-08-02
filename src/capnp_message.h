#ifndef CAPNP_JANET_MESSAGE_H
#define CAPNP_JANET_MESSAGE_H

#include "capnp_kinds.h"
#include <stddef.h>
#include <stdint.h>

#define CAPNP_MAX_SEGMENTS 16

typedef struct capnp_segment {
  const uint8_t *data; /* words, little-endian */
  size_t words;        /* length in 8-byte words */
} capnp_segment_t;

typedef struct capnp_message {
  capnp_segment_t segs[CAPNP_MAX_SEGMENTS];
  uint32_t nsegs;
  /* When non-NULL, owns a copy of the framed bytes (deserialize path). */
  uint8_t *owned;
  size_t owned_len;
  uint64_t traversal_left;
  int depth_limit;
} capnp_message_t;

/* Resolved object handle (struct or list). Never a far landing pad. */
typedef struct capnp_ptr {
  capnp_message_t *msg;
  uint32_t seg;
  size_t word; /* word offset of content (struct body / list start / tag) */
  int kind;    /* CAPNP_PK_* */
  /* struct */
  uint16_t dwords;
  uint16_t pwords;
  /* list */
  int esize;
  uint32_t count; /* elements; for composite, element count after tag */
  size_t step_words; /* for composite lists: words per element */
} capnp_ptr_t;

/* Zero a message. Free with capnp_message_free. */
void capnp_message_init(capnp_message_t *m);
void capnp_message_free(capnp_message_t *m);

/*
 * Zero-copy view of already-separated segments. Segments must outlive m.
 * Does not take ownership.
 */
int capnp_message_from_segments(capnp_message_t *m, const capnp_segment_t *segs,
                               uint32_t nsegs);

/*
 * Deserialize a stream-framed Cap'n message (segment table + segments).
 * Copies bytes into m->owned so the input buffer may be freed after return.
 */
int capnp_message_from_flat(capnp_message_t *m, const uint8_t *data, size_t len);

/*
 * Zero-copy stream-framed view: segments alias into data. data must outlive m.
 */
int capnp_message_view_flat(capnp_message_t *m, const uint8_t *data, size_t len);

/* Root object of segment 0 (pointer at word 0). */
int capnp_root(capnp_message_t *m, capnp_ptr_t *out);

/* Read pointer slot @ptr_index from a struct. */
int capnp_getp(const capnp_ptr_t *s, uint16_t ptr_index, capnp_ptr_t *out);

/* Data-section readers (schema-evolution: past end -> default). XOR not applied
 * here; callers pass the schema default (already the wire XOR base). */
uint8_t capnp_get_u8(const capnp_ptr_t *s, uint32_t byte_offset, uint8_t dflt);
uint16_t capnp_get_u16(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint16_t dflt);
uint32_t capnp_get_u32(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint32_t dflt);
uint64_t capnp_get_u64(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint64_t dflt);
int capnp_get_bool(const capnp_ptr_t *s, uint32_t bit_offset, int dflt);

/*
 * Text: List(UInt8) with trailing NUL included in count. out points into the
 * segment; len is byte length excluding the NUL (like strlen).
 */
int capnp_get_text(const capnp_ptr_t *s, uint16_t ptr_index, const char **out,
                   size_t *len);

/* List element accessors. */
uint32_t capnp_list_len(const capnp_ptr_t *list);
int capnp_list_getp(const capnp_ptr_t *list, uint32_t index, capnp_ptr_t *out);
int capnp_list_get_text(const capnp_ptr_t *list, uint32_t index,
                        const char **out, size_t *len);

#endif /* CAPNP_JANET_MESSAGE_H */
