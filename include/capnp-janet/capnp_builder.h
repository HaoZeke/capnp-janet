#ifndef CAPNP_JANET_BUILDER_H
#define CAPNP_JANET_BUILDER_H

#include <capnp-janet/capnp_message.h>
#include <stddef.h>
#include <capnp-janet/capnp_kinds.h>
#include <stdint.h>

/*
 * Multi-segment growable arena builder.
 *
 * Segment size policy:
 *   - capnp_builder_init: first segment capacity =
 *     CAPNP_BUILDER_DEFAULT_FIRST_WORDS (1024 words = 8 KiB).
 *   - capnp_builder_init_sized(b, first_words): first segment capacity
 *     max(1, first_words). Small values force multi-segment messages (and far
 *     pointers) early — used by tests.
 *   - Normal allocation tries the last segment. If the run does not fit its
 *     remaining capacity, a new segment is appended with capacity
 *     max(need, 2 * previous segment capacity), unless force_single is set
 *     (canonicalize path): then the last segment grows in place.
 *   - Far landing pads allocate inside the target object's segment
 *     (grow-in-place). If that segment is already at max_seg_words, a
 *     double-far landing pad is placed in another segment.
 *   - Hard ceiling per segment: CAPNP_BUILDER_MAX_SEGMENT_WORDS ((1<<29)-1).
 *   - Wire segment IDs are 0-based.
 */

#define CAPNP_BUILDER_DEFAULT_FIRST_WORDS 1024u
#define CAPNP_BUILDER_MAX_SEGMENT_WORDS ((1u << 29) - 1u)

typedef struct capnp_bseg {
  uint8_t *data;
  size_t words; /* used words */
  size_t cap;   /* capacity in words */
} capnp_bseg_t;

typedef struct capnp_builder {
  capnp_bseg_t *segs;
  uint32_t nsegs;
  uint32_t segs_cap;
  size_t max_seg_words; /* 0 => CAPNP_BUILDER_MAX_SEGMENT_WORDS */
  int force_single;     /* grow last seg instead of appending */
} capnp_builder_t;

typedef struct capnp_bptr {
  capnp_builder_t *b;
  uint32_t seg;
  size_t word;
} capnp_bptr_t;

CAPNP_JANET_EXPORT void capnp_builder_init(capnp_builder_t *b);
CAPNP_JANET_EXPORT void capnp_builder_init_sized(capnp_builder_t *b, size_t first_words);
CAPNP_JANET_EXPORT void capnp_builder_free(capnp_builder_t *b);
CAPNP_JANET_EXPORT void capnp_builder_set_max_seg_words(capnp_builder_t *b, size_t max_words);

CAPNP_JANET_EXPORT uint32_t capnp_builder_nsegs(const capnp_builder_t *b);
CAPNP_JANET_EXPORT size_t capnp_builder_seg_words(const capnp_builder_t *b, uint32_t seg);
CAPNP_JANET_EXPORT const uint8_t *capnp_builder_seg_data(const capnp_builder_t *b,
                                                         uint32_t seg);

static inline capnp_bptr_t capnp_bptr_add(capnp_bptr_t base, size_t word_delta) {
  base.word += word_delta;
  return base;
}

CAPNP_JANET_EXPORT int capnp_builder_root(capnp_builder_t *b, capnp_bptr_t *root);
CAPNP_JANET_EXPORT int capnp_builder_struct(const capnp_bptr_t *ptr, uint16_t dwords,
                         uint16_t pwords, capnp_bptr_t *body_out);

CAPNP_JANET_EXPORT int capnp_builder_set_u8(const capnp_bptr_t *body, uint32_t byte_offset,
                         uint8_t value);
CAPNP_JANET_EXPORT int capnp_builder_set_u16(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint16_t value);
CAPNP_JANET_EXPORT int capnp_builder_set_u32(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint32_t value);
CAPNP_JANET_EXPORT int capnp_builder_set_u64(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint64_t value);
CAPNP_JANET_EXPORT int capnp_builder_set_f32(const capnp_bptr_t *body, uint32_t byte_offset,
                          float value);
CAPNP_JANET_EXPORT int capnp_builder_set_f64(const capnp_bptr_t *body, uint32_t byte_offset,
                          double value);
CAPNP_JANET_EXPORT int capnp_builder_set_bool(const capnp_bptr_t *body, uint32_t bit_offset,
                           int value);

CAPNP_JANET_EXPORT int capnp_builder_slot(const capnp_bptr_t *body, uint16_t dwords,
                       uint16_t ptr_index, capnp_bptr_t *slot);
CAPNP_JANET_EXPORT int capnp_builder_clear_ptr(const capnp_bptr_t *body,
                            uint16_t dwords, uint16_t ptr_index);

CAPNP_JANET_EXPORT int capnp_builder_set_text(const capnp_bptr_t *body, uint16_t dwords,
                           uint16_t ptr_index, const char *text,
                           size_t text_len);
/* Capability pointer naming capTable entry `cap_index`. */
CAPNP_JANET_EXPORT int capnp_builder_set_cap(const capnp_bptr_t *body, uint16_t dwords,
                          uint16_t ptr_index, uint32_t cap_index);
CAPNP_JANET_EXPORT int capnp_builder_set_list_text(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, const char *const *items,
                                uint32_t nitems);
CAPNP_JANET_EXPORT int capnp_builder_set_list_text_n(
    const capnp_bptr_t *body, uint16_t dwords, uint16_t ptr_index,
    const char *const *items, const size_t *lengths, uint32_t nitems);
CAPNP_JANET_EXPORT int capnp_builder_set_data(const capnp_bptr_t *body, uint16_t dwords,
                           uint16_t ptr_index, const uint8_t *data,
                           size_t data_len);

CAPNP_JANET_EXPORT int capnp_builder_set_list_u8(const capnp_bptr_t *body, uint16_t dwords,
                              uint16_t ptr_index, const uint8_t *items,
                              uint32_t nitems);
CAPNP_JANET_EXPORT int capnp_builder_set_list_u16(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint16_t *items,
                               uint32_t nitems);
CAPNP_JANET_EXPORT int capnp_builder_set_list_u32(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint32_t *items,
                               uint32_t nitems);
CAPNP_JANET_EXPORT int capnp_builder_set_list_u64(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint64_t *items,
                               uint32_t nitems);
CAPNP_JANET_EXPORT int capnp_builder_set_list_f32(const capnp_bptr_t *body,
                               uint16_t dwords, uint16_t ptr_index,
                               const float *items, uint32_t nitems);
CAPNP_JANET_EXPORT int capnp_builder_set_list_f64(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const double *items,
                               uint32_t nitems);

/* List(Bool): items[i] non-zero = true. Bits packed LSB-first per byte. */
CAPNP_JANET_EXPORT int capnp_builder_set_list_bool(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, const uint8_t *items,
                                uint32_t nitems);
/* List(Void): length only; no element payload. */
CAPNP_JANET_EXPORT int capnp_builder_set_list_void(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, uint32_t nitems);

CAPNP_JANET_EXPORT int capnp_builder_set_list_struct(const capnp_bptr_t *body, uint16_t dwords,
                                  uint16_t ptr_index, uint32_t nitems,
                                  uint16_t elem_dwords, uint16_t elem_pwords,
                                  capnp_bptr_t *first_elem);

/* Zeroed pointer list. `first` is element 0; element i is `i` words later. */
CAPNP_JANET_EXPORT int capnp_builder_init_ptr_list(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, uint32_t nitems,
                                capnp_bptr_t *first);

CAPNP_JANET_EXPORT size_t capnp_builder_ptr_word(size_t body_word, uint16_t dwords,
                              uint16_t ptr_index);

CAPNP_JANET_EXPORT int capnp_builder_copy_ptr(capnp_bptr_t *slot, const capnp_ptr_t *src);

/* An object allocated in the arena and not yet attached to a pointer
 * slot, the same relationship as a C++ Orphan. adopt writes the pointer
 * and clears `live`. disown nulls the slot and returns the object.
 *
 * Identity lives in a table the arena owns, keyed by the public fields
 * below. C++ Orphan is move-only, so a by-value copy is not a handle
 * C++ allows. A copy made before adopt or free is rejected afterwards
 * unless a later orphan occupies the same words with the same shape.
 * Then the copy and that orphan share one record and only one of them
 * succeeds. Keep one handle per orphan. */
typedef struct capnp_orphan {
  capnp_builder_t *b;
  uint32_t seg;
  size_t word;
  int kind; /* CAPNP_PK_STRUCT or CAPNP_PK_LIST */
  uint16_t dwords;
  uint16_t pwords;
  int esize;
  uint32_t count;
  int live;
} capnp_orphan_t;

CAPNP_JANET_EXPORT int capnp_builder_new_orphan_struct(capnp_builder_t *b,
                                                       uint16_t dwords,
                                                       uint16_t pwords,
                                                       capnp_orphan_t *o,
                                                       capnp_bptr_t *body);
CAPNP_JANET_EXPORT int capnp_builder_new_orphan_list(capnp_builder_t *b, int esize,
                                                     uint32_t count,
                                                     capnp_orphan_t *o,
                                                     capnp_bptr_t *start);
/* A composite list. `first`, when set, is the first element body. For a
 * composite orphan, `count` on the orphan is the wire word count of the
 * elements (not the element count), which is what the list pointer stores. */
CAPNP_JANET_EXPORT int capnp_builder_new_orphan_struct_list(
    capnp_builder_t *b, uint32_t count, uint16_t elem_dwords,
    uint16_t elem_pwords, capnp_orphan_t *o, capnp_bptr_t *first);
CAPNP_JANET_EXPORT int capnp_builder_adopt(const capnp_bptr_t *slot,
                                           capnp_orphan_t *o);
/* Zero the orphan's words and drop its pointer children. The orphan
 * is not live afterwards, and adopt of it fails. Like C++
 * OrphanBuilder::euthanize this leaves the segment tail where it is. */
CAPNP_JANET_EXPORT int capnp_builder_orphan_free(capnp_orphan_t *o);
CAPNP_JANET_EXPORT int capnp_builder_disown(const capnp_bptr_t *slot,
                                            capnp_orphan_t *o);

CAPNP_JANET_EXPORT int capnp_builder_serialize(const capnp_builder_t *b, uint8_t **out,
                            size_t *out_len);

CAPNP_JANET_EXPORT size_t capnp_builder_struct_body(const capnp_bptr_t *struct_ptr);

#endif /* CAPNP_JANET_BUILDER_H */
