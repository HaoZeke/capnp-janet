#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_copy.h>
#include <capnp-janet/capnp_pointer.h>

#include "capnp_internal.h"

#include <stdlib.h>
#include <string.h>

static size_t lim_words(const capnp_builder_t *b) {
  return b->max_seg_words ? b->max_seg_words : CAPNP_BUILDER_MAX_SEGMENT_WORDS;
}

static int raw_slot_checked(const capnp_bptr_t *slot) {
  if (!slot || !slot->b)
    return CAPNP_ERR_ARG;
  if (slot->seg >= slot->b->nsegs)
    return CAPNP_ERR_SEGMENT;
  if (slot->word >= slot->b->segs[slot->seg].words)
    return CAPNP_ERR_BOUNDS;
  return CAPNP_OK;
}

static int slot_word_checked(const capnp_bptr_t *body, uint16_t dwords,
                             uint16_t ptr_index, size_t *out) {
  size_t word;
  if (!body || !body->b || !out)
    return CAPNP_ERR_ARG;
  if (body->seg >= body->b->nsegs)
    return CAPNP_ERR_SEGMENT;
  if (body->word > SIZE_MAX - (size_t)dwords)
    return CAPNP_ERR_BOUNDS;
  word = body->word + (size_t)dwords;
  if (word > SIZE_MAX - (size_t)ptr_index)
    return CAPNP_ERR_BOUNDS;
  word += (size_t)ptr_index;
  if (word >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
  *out = word;
  return CAPNP_OK;
}

struct capnp_oref {
  uintptr_t owner_id;
  uint32_t seg;
  size_t word;
  size_t end; /* first word past the object; 0 for a zero-word object */
  int kind;
  uint16_t dwords;
  uint16_t pwords;
  int esize;
  uint32_t count;
};

/* Records of the live orphans sit in front of the segment table.
 * Callers allocate capnp_builder_t themselves, so these fields stay
 * outside the public struct. */
struct capnp_builder_priv {
  struct capnp_oref *orefs;
  uint32_t norefs;
  uint32_t orefs_cap;
  /* 0 for the growable arena, else CAPNP_ALLOC_* + 1. */
  int malloc_mode;
  uint32_t seg_with_space;
  size_t next_size;
  /* Landing pad alloc_for reserved in front of an object that did not
   * fit the segment of the pointer naming it. */
  int pad_pending;
  uint32_t pad_seg;
  size_t pad_word;
};

_Static_assert(sizeof(struct capnp_builder_priv) % _Alignof(capnp_bseg_t) == 0,
               "segment table stays aligned");

static char *segs_block(capnp_bseg_t *segs) {
  return (char *)segs - sizeof(struct capnp_builder_priv);
}

static struct capnp_builder_priv *builder_priv(capnp_builder_t *b) {
  if (!b || !b->segs)
    return NULL;
  return (struct capnp_builder_priv *)segs_block(b->segs);
}

static void builder_release_table(capnp_builder_t *b) {
  struct capnp_builder_priv *priv;
  if (!b || !b->segs)
    return;
  priv = builder_priv(b);
  free(priv->orefs);
  free(priv);
  b->segs = NULL;
}

static int ensure_segs_cap(capnp_builder_t *b, uint32_t need) {
  uint32_t ncap;
  capnp_bseg_t *p;
  char *block;
  size_t bytes;
  if (need <= b->segs_cap)
    return CAPNP_OK;
  ncap = b->segs_cap ? b->segs_cap : 4u;
  while (ncap < need)
    ncap = ncap > UINT32_MAX / 2u ? UINT32_MAX : ncap * 2u;
#if SIZE_MAX <= UINT32_MAX
  if ((size_t)ncap >
      (SIZE_MAX - sizeof(struct capnp_builder_priv)) / sizeof(capnp_bseg_t))
    return CAPNP_ERR_ALLOC;
#endif
  bytes = sizeof(struct capnp_builder_priv) + (size_t)ncap * sizeof(capnp_bseg_t);
  block = b->segs ? segs_block(b->segs) : NULL;
  block = (char *)realloc(block, bytes);
  if (!block)
    return CAPNP_ERR_ALLOC;
  if (!b->segs)
    memset(block, 0, sizeof(struct capnp_builder_priv));
  p = (capnp_bseg_t *)(block + sizeof(struct capnp_builder_priv));
  memset(p + b->segs_cap, 0, (size_t)(ncap - b->segs_cap) * sizeof(capnp_bseg_t));
  b->segs = p;
  b->segs_cap = ncap;
  return CAPNP_OK;
}

static int seg_ensure(capnp_builder_t *b, uint32_t seg, size_t need_words) {
  capnp_bseg_t *s;
  size_t ncap, lim = lim_words(b);
  uint8_t *p;
  if (seg >= b->nsegs)
    return CAPNP_ERR_SEGMENT;
  s = &b->segs[seg];
  if (need_words <= s->cap)
    return CAPNP_OK;
  if (need_words > lim)
    return CAPNP_ERR_ALLOC;
  ncap = s->cap ? s->cap * 2 : 16;
  while (ncap < need_words)
    ncap *= 2;
  if (ncap > lim)
    ncap = lim;
  if (ncap < need_words)
    return CAPNP_ERR_ALLOC;
  p = (uint8_t *)realloc(s->data, ncap * CAPNP_WORD_BYTES);
  if (!p)
    return CAPNP_ERR_ALLOC;
  if (ncap > s->cap)
    memset(p + s->cap * CAPNP_WORD_BYTES, 0,
           (ncap - s->cap) * CAPNP_WORD_BYTES);
  s->data = p;
  s->cap = ncap;
  return CAPNP_OK;
}

static int append_segment(capnp_builder_t *b, size_t cap_words) {
  capnp_bseg_t *s;
  size_t lim = lim_words(b);
  if (cap_words < 1)
    cap_words = 1;
  if (cap_words > lim)
    cap_words = lim;
  /* Far pointers name a segment with 32 bits. */
  if (b->nsegs == UINT32_MAX)
    return CAPNP_ERR_ALLOC;
  if (ensure_segs_cap(b, b->nsegs + 1))
    return CAPNP_ERR_ALLOC;
  s = &b->segs[b->nsegs];
  memset(s, 0, sizeof(*s));
  s->data = (uint8_t *)calloc(cap_words, CAPNP_WORD_BYTES);
  if (!s->data)
    return CAPNP_ERR_ALLOC;
  s->cap = cap_words;
  s->words = 0;
  b->nsegs++;
  return CAPNP_OK;
}

static int malloc_mode(capnp_builder_t *b) {
  struct capnp_builder_priv *priv = builder_priv(b);
  return priv ? priv->malloc_mode : 0;
}

/* BuilderArena::allocate: the segment with space, else a new segment of
 * max(n, nextSize) that MallocMessageBuilder::allocateSegment sizes. */
static int arena_alloc(capnp_builder_t *b, size_t n, uint32_t *out_seg,
                       size_t *out_word) {
  struct capnp_builder_priv *priv = builder_priv(b);
  capnp_bseg_t *s;
  size_t size;
  if (priv->seg_with_space < b->nsegs) {
    s = &b->segs[priv->seg_with_space];
    if (n <= s->cap - s->words) {
      *out_seg = priv->seg_with_space;
      *out_word = s->words;
      s->words += n;
      return CAPNP_OK;
    }
  }
  if (n > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_ALLOC;
  size = n > priv->next_size ? n : priv->next_size;
  if (append_segment(b, size))
    return CAPNP_ERR_ALLOC;
  priv = builder_priv(b);
  if (priv->malloc_mode == CAPNP_ALLOC_GROW_HEURISTICALLY + 1)
    priv->next_size = size <= CAPNP_BUILDER_MAX_SEGMENT_WORDS - priv->next_size
                          ? priv->next_size + size
                          : CAPNP_BUILDER_MAX_SEGMENT_WORDS;
  priv->seg_with_space = b->nsegs - 1;
  *out_seg = b->nsegs - 1;
  *out_word = 0;
  b->segs[*out_seg].words = n;
  return CAPNP_OK;
}

static int tail_slot(capnp_builder_t *b, uint32_t *seg, size_t *word) {
  if (!b || b->nsegs == 0 || !b->segs)
    return CAPNP_ERR_ALLOC;
  *seg = b->nsegs - 1;
  *word = b->segs[*seg].words;
  return CAPNP_OK;
}

static int alloc_words(capnp_builder_t *b, size_t n, uint32_t *out_seg,
                       size_t *out_word) {
  capnp_bseg_t *last;
  size_t lim = lim_words(b);
  size_t ncap;
  if (!b || !out_seg || !out_word || b->nsegs == 0)
    return CAPNP_ERR_ARG;
  if (malloc_mode(b))
    return arena_alloc(b, n, out_seg, out_word);
  if (n > lim)
    return CAPNP_ERR_ALLOC;
  last = &b->segs[b->nsegs - 1];
  if (n > SIZE_MAX - last->words)
    return CAPNP_ERR_ALLOC;
  if (last->words + n <= last->cap && last->words + n <= lim) {
    *out_seg = b->nsegs - 1;
    *out_word = last->words;
    last->words += n;
    return CAPNP_OK;
  }
  if (b->force_single) {
    if (seg_ensure(b, b->nsegs - 1, last->words + n))
      return CAPNP_ERR_ALLOC;
    last = &b->segs[b->nsegs - 1];
    *out_seg = b->nsegs - 1;
    *out_word = last->words;
    last->words += n;
    return CAPNP_OK;
  }
  ncap = last->cap ? last->cap * 2 : 16;
  if (ncap < n)
    ncap = n;
  if (ncap > lim)
    ncap = lim;
  if (ncap < n)
    return CAPNP_ERR_ALLOC;
  if (append_segment(b, ncap))
    return CAPNP_ERR_ALLOC;
  last = &b->segs[b->nsegs - 1];
  *out_seg = b->nsegs - 1;
  *out_word = 0;
  last->words = n;
  return CAPNP_OK;
}

static int alloc_in(capnp_builder_t *b, uint32_t seg, size_t n,
                    size_t *out_word) {
  capnp_bseg_t *s;
  if (!b || !out_word || seg >= b->nsegs)
    return CAPNP_ERR_ARG;
  s = &b->segs[seg];
  if (n > SIZE_MAX - s->words || s->words + n > lim_words(b))
    return CAPNP_ERR_ALLOC;
  if (malloc_mode(b) && s->words + n > s->cap)
    return CAPNP_ERR_ALLOC;
  if (seg_ensure(b, seg, s->words + n))
    return CAPNP_ERR_ALLOC;
  s = &b->segs[seg];
  *out_word = s->words;
  s->words += n;
  return CAPNP_OK;
}


/* Space for an object named by a pointer in slot_seg. WireHelpers::allocate
 * uses the pointer's own segment when the object fits, and otherwise takes
 * one more word from the arena for the landing pad in front of it. */
static int alloc_for(capnp_builder_t *b, uint32_t slot_seg, size_t n,
                     uint32_t *out_seg, size_t *out_word) {
  struct capnp_builder_priv *priv = builder_priv(b);
  capnp_bseg_t *s;
  uint32_t seg;
  size_t word;
  if (!priv || !priv->malloc_mode) {
    if (n == 0)
      return tail_slot(b, out_seg, out_word);
    return alloc_words(b, n, out_seg, out_word);
  }
  priv->pad_pending = 0;
  s = &b->segs[slot_seg];
  if (n <= s->cap - s->words) {
    *out_seg = slot_seg;
    *out_word = s->words;
    s->words += n;
    return CAPNP_OK;
  }
  if (n >= CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_ALLOC;
  if (arena_alloc(b, n + 1, &seg, &word))
    return CAPNP_ERR_ALLOC;
  priv = builder_priv(b);
  priv->pad_pending = 1;
  priv->pad_seg = seg;
  priv->pad_word = word;
  *out_seg = seg;
  *out_word = word + 1;
  return CAPNP_OK;
}

static int take_pad(capnp_builder_t *b, uint32_t seg, size_t body,
                    size_t *pad) {
  struct capnp_builder_priv *priv = builder_priv(b);
  if (!priv || !priv->pad_pending || priv->pad_seg != seg ||
      priv->pad_word + 1 != body)
    return 0;
  priv->pad_pending = 0;
  *pad = priv->pad_word;
  return 1;
}

/* Allocate n words for a far landing pad, never in avoid_seg. Always appends
 * a fresh segment so content segments at capacity stay untouched. */
static int alloc_pad_segment(capnp_builder_t *b, uint32_t avoid_seg, size_t n,
                             uint32_t *out_seg, size_t *out_word) {
  size_t saved = b->max_seg_words;
  size_t need = n < 1 ? 1 : n;
  (void)avoid_seg;
  if (malloc_mode(b))
    return arena_alloc(b, need, out_seg, out_word);
  if (saved && saved < need)
    b->max_seg_words = need;
  if (append_segment(b, need)) {
    b->max_seg_words = saved;
    return CAPNP_ERR_ALLOC;
  }
  b->max_seg_words = saved;
  *out_seg = b->nsegs - 1;
  *out_word = 0;
  b->segs[*out_seg].words = need;
  return CAPNP_OK;
}

static void store_w(capnp_builder_t *b, uint32_t seg, size_t word, uint64_t w) {
  capnp_store_le64(b->segs[seg].data + word * CAPNP_WORD_BYTES, w);
}

/* Release the object a pointer slot names, when that object is still
 * the tail of its segment, then zero the slot. */
static void drop_slot(capnp_builder_t *b, uint32_t seg, size_t word);

static uint8_t *wbytes(capnp_builder_t *b, uint32_t seg, size_t word) {
  return b->segs[seg].data + word * CAPNP_WORD_BYTES;
}

static int write_struct_ptr(capnp_builder_t *b, uint32_t slot_seg,
                            size_t slot_word, uint32_t body_seg,
                            size_t body_word, uint16_t dwords,
                            uint16_t pwords) {
  size_t pad;
  uint32_t pad_seg;
  size_t pad_word;

  if (slot_seg == body_seg) {
    int32_t off = (int32_t)((int64_t)body_word - (int64_t)slot_word - 1);
    store_w(b, slot_seg, slot_word, capnp_wp_make_struct(off, dwords, pwords));
    return CAPNP_OK;
  }
  if (take_pad(b, body_seg, body_word, &pad) ||
      alloc_in(b, body_seg, 1, &pad) == CAPNP_OK) {
    int32_t off = (int32_t)((int64_t)body_word - (int64_t)pad - 1);
    store_w(b, body_seg, pad, capnp_wp_make_struct(off, dwords, pwords));
    store_w(b, slot_seg, slot_word,
            capnp_wp_make_far(0, (uint32_t)pad, body_seg));
    return CAPNP_OK;
  }
  if (alloc_pad_segment(b, body_seg, 2, &pad_seg, &pad_word))
    return CAPNP_ERR_ALLOC;
  store_w(b, pad_seg, pad_word,
          capnp_wp_make_far(0, (uint32_t)body_word, body_seg));
  store_w(b, pad_seg, pad_word + 1, capnp_wp_make_struct(0, dwords, pwords));
  store_w(b, slot_seg, slot_word,
          capnp_wp_make_far(1, (uint32_t)pad_word, pad_seg));
  return CAPNP_OK;
}

static int write_list_ptr(capnp_builder_t *b, uint32_t slot_seg,
                          size_t slot_word, uint32_t content_seg,
                          size_t content_word, int esize, uint32_t count) {
  size_t pad;
  uint32_t pad_seg;
  size_t pad_word;

  if (count > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (slot_seg == content_seg) {
    int32_t off =
        (int32_t)((int64_t)content_word - (int64_t)slot_word - 1);
    store_w(b, slot_seg, slot_word, capnp_wp_make_list(off, esize, count));
    return CAPNP_OK;
  }
  if (take_pad(b, content_seg, content_word, &pad) ||
      alloc_in(b, content_seg, 1, &pad) == CAPNP_OK) {
    int32_t off = (int32_t)((int64_t)content_word - (int64_t)pad - 1);
    store_w(b, content_seg, pad, capnp_wp_make_list(off, esize, count));
    store_w(b, slot_seg, slot_word,
            capnp_wp_make_far(0, (uint32_t)pad, content_seg));
    return CAPNP_OK;
  }
  if (alloc_pad_segment(b, content_seg, 2, &pad_seg, &pad_word))
    return CAPNP_ERR_ALLOC;
  store_w(b, pad_seg, pad_word,
          capnp_wp_make_far(0, (uint32_t)content_word, content_seg));
  store_w(b, pad_seg, pad_word + 1, capnp_wp_make_list(0, esize, count));
  store_w(b, slot_seg, slot_word,
          capnp_wp_make_far(1, (uint32_t)pad_word, pad_seg));
  return CAPNP_OK;
}

void capnp_builder_init(capnp_builder_t *b) {
  capnp_builder_init_sized(b, CAPNP_BUILDER_DEFAULT_FIRST_WORDS);
}

void capnp_builder_init_sized(capnp_builder_t *b, size_t first_words) {
  memset(b, 0, sizeof(*b));
  if (first_words < 1)
    first_words = 1;
  if (first_words > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    first_words = CAPNP_BUILDER_MAX_SEGMENT_WORDS;
  if (append_segment(b, first_words) != CAPNP_OK) {
    uint32_t i;
    if (b->segs) {
      for (i = 0; i < b->nsegs; i++)
        free(b->segs[i].data);
    }
    builder_release_table(b);
    memset(b, 0, sizeof(*b));
  }
}

void capnp_builder_init_malloc(capnp_builder_t *b, size_t first_words,
                               int strategy) {
  struct capnp_builder_priv *priv;
  capnp_builder_init_sized(b, first_words);
  priv = builder_priv(b);
  if (!priv)
    return;
  priv->malloc_mode = strategy == CAPNP_ALLOC_FIXED_SIZE
                          ? CAPNP_ALLOC_FIXED_SIZE + 1
                          : CAPNP_ALLOC_GROW_HEURISTICALLY + 1;
  priv->next_size = b->segs[0].cap;
  priv->seg_with_space = 0;
}

void capnp_builder_set_max_seg_words(capnp_builder_t *b, size_t max_words) {
  if (b)
    b->max_seg_words = max_words;
}

void capnp_builder_free(capnp_builder_t *b) {
  uint32_t i;
  if (!b)
    return;
  for (i = 0; i < b->nsegs; i++)
    free(b->segs[i].data);
  builder_release_table(b);
  memset(b, 0, sizeof(*b));
}

uint32_t capnp_builder_nsegs(const capnp_builder_t *b) {
  return b ? b->nsegs : 0;
}

size_t capnp_builder_seg_words(const capnp_builder_t *b, uint32_t seg) {
  if (!b || seg >= b->nsegs)
    return 0;
  return b->segs[seg].words;
}

const uint8_t *capnp_builder_seg_data(const capnp_builder_t *b, uint32_t seg) {
  if (!b || seg >= b->nsegs)
    return NULL;
  return b->segs[seg].data;
}

int capnp_builder_root(capnp_builder_t *b, capnp_bptr_t *root) {
  uint32_t seg;
  size_t w;
  if (!b || !root || b->nsegs == 0 || b->segs[0].words != 0)
    return CAPNP_ERR_ARG;
  if (alloc_words(b, 1, &seg, &w) || seg != 0 || w != 0)
    return CAPNP_ERR_ALLOC;
  root->b = b;
  root->seg = 0;
  root->word = 0;
  return CAPNP_OK;
}

int capnp_builder_struct(const capnp_bptr_t *ptr, uint16_t dwords, uint16_t pwords,
                         capnp_bptr_t *body_out) {
  size_t n = (size_t)dwords + pwords;
  uint32_t body_seg;
  size_t body_word;
  int rc;
  if ((rc = raw_slot_checked(ptr)) != CAPNP_OK)
    return rc;
  drop_slot(ptr->b, ptr->seg, ptr->word);
  if (n == 0) {
    /* encoding.html: zero-size struct is A=0 B=-1 C=D=0 (0xFFFFFFFC),
     * not offset 0 (that is a null pointer). */
    store_w(ptr->b, ptr->seg, ptr->word, capnp_wp_make_struct(-1, 0, 0));
    if (body_out) {
      body_out->b = ptr->b;
      body_out->seg = ptr->seg;
      body_out->word = ptr->word;
    }
    return CAPNP_OK;
  }
  if (alloc_for(ptr->b, ptr->seg, n, &body_seg, &body_word))
    return CAPNP_ERR_ALLOC;
  rc = write_struct_ptr(ptr->b, ptr->seg, ptr->word, body_seg, body_word,
                        dwords, pwords);
  if (rc)
    return rc;
  if (body_out) {
    body_out->b = ptr->b;
    body_out->seg = body_seg;
    body_out->word = body_word;
  }
  return CAPNP_OK;
}

size_t capnp_builder_struct_body(const capnp_bptr_t *struct_ptr) {
  uint64_t w;
  int32_t off;
  if (!struct_ptr || !struct_ptr->b || struct_ptr->seg >= struct_ptr->b->nsegs)
    return 0;
  w = capnp_load_le64(struct_ptr->b->segs[struct_ptr->seg].data +
                      struct_ptr->word * CAPNP_WORD_BYTES);
  if (capnp_wp_kind(w) == CAPNP_WK_FAR)
    return 0;
  off = capnp_wp_offset(w);
  return (size_t)((int64_t)struct_ptr->word + 1 + off);
}

/* Locate `width` bytes at byte_offset inside the segment. A wrapped
 * offset must fail before any byte is stored. */
static int body_span(const capnp_bptr_t *body, uint32_t byte_offset,
                     size_t width, uint8_t **out) {
  size_t base, abs, seg_bytes;
  if (!body || !body->b || width == 0)
    return CAPNP_ERR_ARG;
  if (body->seg >= body->b->nsegs)
    return CAPNP_ERR_SEGMENT;
  if (body->word > SIZE_MAX / CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  base = body->word * CAPNP_WORD_BYTES;
  if ((size_t)byte_offset > SIZE_MAX - base)
    return CAPNP_ERR_BOUNDS;
  abs = base + (size_t)byte_offset;
  if (width > SIZE_MAX - abs)
    return CAPNP_ERR_BOUNDS;
  seg_bytes = body->b->segs[body->seg].words * CAPNP_WORD_BYTES;
  if (abs + width > seg_bytes)
    return CAPNP_ERR_BOUNDS;
  *out = body->b->segs[body->seg].data + abs;
  return CAPNP_OK;
}

int capnp_builder_set_u8(const capnp_bptr_t *body, uint32_t byte_offset,
                         uint8_t value) {
  uint8_t *p;
  int rc = body_span(body, byte_offset, 1, &p);
  if (rc)
    return rc;
  *p = value;
  return CAPNP_OK;
}

int capnp_builder_set_u16(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint16_t value) {
  uint8_t *p;
  int rc = body_span(body, byte_offset, 2, &p);
  if (rc)
    return rc;
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  return CAPNP_OK;
}

int capnp_builder_set_u32(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint32_t value) {
  uint8_t *p;
  int rc = body_span(body, byte_offset, 4, &p);
  if (rc)
    return rc;
  capnp_store_le32(p, value);
  return CAPNP_OK;
}

int capnp_builder_set_u64(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint64_t value) {
  uint8_t *p;
  int rc = body_span(body, byte_offset, 8, &p);
  if (rc)
    return rc;
  capnp_store_le64(p, value);
  return CAPNP_OK;
}

int capnp_builder_set_f32(const capnp_bptr_t *body, uint32_t byte_offset,
                          float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return capnp_builder_set_u32(body, byte_offset, bits);
}

int capnp_builder_set_f64(const capnp_bptr_t *body, uint32_t byte_offset,
                          double value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return capnp_builder_set_u64(body, byte_offset, bits);
}

int capnp_builder_slot(const capnp_bptr_t *body, uint16_t dwords,
                       uint16_t ptr_index, capnp_bptr_t *slot) {
  size_t pw;
  int rc;
  if (!slot)
    return CAPNP_ERR_ARG;
  if ((rc = slot_word_checked(body, dwords, ptr_index, &pw)) != CAPNP_OK)
    return rc;
  slot->b = body->b;
  slot->seg = body->seg;
  slot->word = pw;
  return CAPNP_OK;
}

int capnp_builder_clear_ptr(const capnp_bptr_t *body, uint16_t dwords,
                            uint16_t ptr_index) {
  capnp_bptr_t slot;
  int rc = capnp_builder_slot(body, dwords, ptr_index, &slot);
  if (rc != CAPNP_OK)
    return rc;
  drop_slot(slot.b, slot.seg, slot.word);
  return CAPNP_OK;
}

int capnp_builder_set_bool(const capnp_bptr_t *body, uint32_t bit_offset,
                           int value) {
  uint8_t bit, *p;
  int rc = body_span(body, bit_offset / 8u, 1, &p);
  if (rc)
    return rc;
  bit = (uint8_t)(1u << (bit_offset % 8u));
  if (value)
    *p |= bit;
  else
    *p &= (uint8_t)~bit;
  return CAPNP_OK;
}

static int write_text_at(capnp_builder_t *b, uint32_t slot_seg, size_t slot_word,
                         const char *text, size_t text_len) {
  size_t nbytes;
  size_t nwords;
  uint32_t start_seg;
  size_t start_word;
  /* The terminating NUL is a list element. C++ MAX_TEXT_SIZE is
   * (1<<29)-2 so the byte count, text_len + 1, fits in 29 bits.
   * Reject that length before allocating; write_list_ptr would refuse
   * the count only after the words were already reserved. */
  if (text_len >= (1u << 29) - 1u)
    return CAPNP_ERR_BOUNDS;
  nbytes = text_len + 1;
  nwords = (nbytes + 7) / 8;
  if (alloc_for(b, slot_seg, nwords, &start_seg, &start_word))
    return CAPNP_ERR_ALLOC;
  memset(wbytes(b, start_seg, start_word), 0, nwords * CAPNP_WORD_BYTES);
  memcpy(wbytes(b, start_seg, start_word), text, text_len);
  return write_list_ptr(b, slot_seg, slot_word, start_seg, start_word,
                        CAPNP_SZ_BYTE, (uint32_t)nbytes);
}

/* Write a capability pointer naming capTable entry `cap_index`. RPC
 * payloads carry capabilities this way: the pointer holds only the index
 * and the descriptor beside it says what the capability is. */
int capnp_builder_set_cap(const capnp_bptr_t *body, uint16_t dwords,
                          uint16_t ptr_index, uint32_t cap_index) {
  size_t ptr_word;
  int   rc = slot_word_checked(body, dwords, ptr_index, &ptr_word);
  if (rc)
    return rc;
  drop_slot(body->b, body->seg, ptr_word);
  store_w(body->b, body->seg, ptr_word, capnp_wp_make_cap(cap_index));
  return CAPNP_OK;
}

int capnp_builder_set_text(const capnp_bptr_t *body, uint16_t dwords,
                           uint16_t ptr_index, const char *text,
                           size_t text_len) {
  size_t ptr_word;
  int rc;
  if (!text)
    return CAPNP_ERR_ARG;
  rc = slot_word_checked(body, dwords, ptr_index, &ptr_word);
  if (rc)
    return rc;
  drop_slot(body->b, body->seg, ptr_word);
  return write_text_at(body->b, body->seg, ptr_word, text, text_len);
}

int capnp_builder_set_data(const capnp_bptr_t *body, uint16_t dwords,
                           uint16_t ptr_index, const uint8_t *data,
                           size_t data_len) {
  size_t nwords, ptr_word;
  uint32_t start_seg;
  size_t start_word;
  if (data_len > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (data_len && !data)
    return CAPNP_ERR_ARG;
  if (slot_word_checked(body, dwords, ptr_index, &ptr_word))
    return CAPNP_ERR_BOUNDS;
  drop_slot(body->b, body->seg, ptr_word);
  nwords = (data_len + 7u) / 8u;
  if (alloc_for(body->b, body->seg, nwords, &start_seg, &start_word))
    return CAPNP_ERR_ALLOC;
  if (nwords) {
    memset(wbytes(body->b, start_seg, start_word), 0,
           nwords * CAPNP_WORD_BYTES);
    if (data_len)
      memcpy(wbytes(body->b, start_seg, start_word), data, data_len);
  }
  return write_list_ptr(body->b, body->seg, ptr_word, start_seg, start_word,
                        CAPNP_SZ_BYTE, (uint32_t)data_len);
}

static int set_list_prim(const capnp_bptr_t *body, uint16_t dwords,
                         uint16_t ptr_index, int esize, uint32_t nitems,
                         size_t elem_bytes, const void *items,
                         capnp_bptr_t *start) {
  size_t ptr_word, nbytes, nwords;
  uint32_t start_seg;
  size_t start_word;
  if (nitems > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (slot_word_checked(body, dwords, ptr_index, &ptr_word))
    return CAPNP_ERR_BOUNDS;
  drop_slot(body->b, body->seg, ptr_word);
  if (elem_bytes && (size_t)nitems > SIZE_MAX / elem_bytes)
    return CAPNP_ERR_BOUNDS;
  nbytes = (size_t)nitems * elem_bytes;
  nwords = (nbytes + 7) / 8;
  if (alloc_for(body->b, body->seg, nwords, &start_seg, &start_word))
    return CAPNP_ERR_ALLOC;
  if (nwords) {
    memset(wbytes(body->b, start_seg, start_word), 0,
           nwords * CAPNP_WORD_BYTES);
    if (items && nbytes) {
      /* Element by element, little-endian: the wire format is
       * little-endian and the caller's array is in host order, so a bulk
       * memcpy is only right by accident on a little-endian host. */
      uint8_t *dst = wbytes(body->b, start_seg, start_word);
      const uint8_t *src = (const uint8_t *)items;
      uint32_t i;
      switch (elem_bytes) {
      case 1:
        memcpy(dst, items, nbytes);
        break;
      case 2: {
        for (i = 0; i < nitems; i++) {
          uint16_t value;
          memcpy(&value, src + (size_t)i * 2, sizeof(value));
          capnp_store_le16(dst + (size_t)i * 2, value);
        }
        break;
      }
      case 4: {
        for (i = 0; i < nitems; i++) {
          uint32_t value;
          memcpy(&value, src + (size_t)i * 4, sizeof(value));
          capnp_store_le32(dst + (size_t)i * 4, value);
        }
        break;
      }
      case 8: {
        for (i = 0; i < nitems; i++) {
          uint64_t value;
          memcpy(&value, src + (size_t)i * 8, sizeof(value));
          capnp_store_le64(dst + (size_t)i * 8, value);
        }
        break;
      }
      default:
        return CAPNP_ERR_ARG;
      }
    }
  }
  if (start) {
    start->b = body->b;
    start->seg = start_seg;
    start->word = start_word;
  }
  return write_list_ptr(body->b, body->seg, ptr_word, start_seg, start_word,
                        esize, nitems);
}

int capnp_builder_set_list_u8(const capnp_bptr_t *body, uint16_t dwords,
                              uint16_t ptr_index, const uint8_t *items,
                              uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_BYTE, nitems, 1, items,
                       NULL);
}
int capnp_builder_set_list_u16(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint16_t *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_TWO, nitems, 2, items,
                       NULL);
}
int capnp_builder_set_list_u32(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint32_t *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_FOUR, nitems, 4, items,
                       NULL);
}
int capnp_builder_set_list_u64(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint64_t *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_EIGHT, nitems, 8, items,
                       NULL);
}
int capnp_builder_set_list_f32(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const float *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_FOUR, nitems, 4, items,
                       NULL);
}
int capnp_builder_set_list_f64(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const double *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_EIGHT, nitems, 8, items,
                       NULL);
}

int capnp_builder_set_list_bool(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, const uint8_t *items,
                                uint32_t nitems) {
  size_t ptr_word, nbytes, nwords;
  uint32_t start_seg, i;
  size_t start_word;
  if (nitems > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (slot_word_checked(body, dwords, ptr_index, &ptr_word))
    return CAPNP_ERR_BOUNDS;
  drop_slot(body->b, body->seg, ptr_word);
  nbytes = ((size_t)nitems + 7u) / 8u;
  nwords = (nbytes + 7u) / 8u;
  if (alloc_for(body->b, body->seg, nwords, &start_seg, &start_word))
    return CAPNP_ERR_ALLOC;
  if (nwords) {
    memset(wbytes(body->b, start_seg, start_word), 0,
           nwords * CAPNP_WORD_BYTES);
    if (items) {
      for (i = 0; i < nitems; i++) {
        if (items[i]) {
          size_t byte = start_word * CAPNP_WORD_BYTES + (size_t)(i / 8u);
          body->b->segs[start_seg].data[byte] |= (uint8_t)(1u << (i % 8u));
        }
      }
    }
  }
  return write_list_ptr(body->b, body->seg, ptr_word, start_seg, start_word,
                        CAPNP_SZ_BIT, nitems);
}

int capnp_builder_set_list_void(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_VOID, nitems, 0, NULL,
                       NULL);
}

int capnp_builder_init_list(const capnp_bptr_t *body, uint16_t dwords,
                            uint16_t ptr_index, int esize, uint32_t nitems,
                            capnp_bptr_t *start) {
  static const size_t bytes[] = {0, 0, 1, 2, 4, 8};
  size_t ptr_word, nwords;
  uint32_t start_seg;
  size_t start_word;
  if (esize < CAPNP_SZ_VOID || esize > CAPNP_SZ_EIGHT)
    return CAPNP_ERR_ARG;
  if (esize != CAPNP_SZ_BIT)
    return set_list_prim(body, dwords, ptr_index, esize, nitems,
                         bytes[esize], NULL, start);
  if (nitems > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (slot_word_checked(body, dwords, ptr_index, &ptr_word))
    return CAPNP_ERR_BOUNDS;
  drop_slot(body->b, body->seg, ptr_word);
  nwords = ((size_t)nitems + 63u) / 64u;
  if (alloc_for(body->b, body->seg, nwords, &start_seg, &start_word))
    return CAPNP_ERR_ALLOC;
  if (nwords)
    memset(wbytes(body->b, start_seg, start_word), 0,
           nwords * CAPNP_WORD_BYTES);
  if (start) {
    start->b = body->b;
    start->seg = start_seg;
    start->word = start_word;
  }
  return write_list_ptr(body->b, body->seg, ptr_word, start_seg, start_word,
                        CAPNP_SZ_BIT, nitems);
}

int capnp_builder_set_list_text_n(const capnp_bptr_t *body, uint16_t dwords,
                                  uint16_t ptr_index,
                                  const char *const *items,
                                  const size_t *lengths, uint32_t nitems) {
  size_t list_ptr_word;
  uint32_t list_seg, i;
  size_t list_start;
  if (nitems > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (slot_word_checked(body, dwords, ptr_index, &list_ptr_word))
    return CAPNP_ERR_BOUNDS;
  drop_slot(body->b, body->seg, list_ptr_word);
  if (alloc_for(body->b, body->seg, nitems, &list_seg, &list_start))
    return CAPNP_ERR_ALLOC;
  if (write_list_ptr(body->b, body->seg, list_ptr_word, list_seg, list_start,
                     CAPNP_SZ_PTR, nitems))
    return CAPNP_ERR_ALLOC;
  for (i = 0; i < nitems; i++) {
    const char *t = items[i] ? items[i] : "";
    size_t len = lengths ? lengths[i] : strlen(t);
    if (write_text_at(body->b, list_seg, list_start + i, t, len))
      return CAPNP_ERR_ALLOC;
  }
  return CAPNP_OK;
}

int capnp_builder_set_list_text(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, const char *const *items,
                                uint32_t nitems) {
  return capnp_builder_set_list_text_n(body, dwords, ptr_index, items, NULL,
                                       nitems);
}

size_t capnp_builder_ptr_word(size_t body_word, uint16_t dwords,
                              uint16_t ptr_index) {
  return body_word + dwords + ptr_index;
}

int capnp_builder_init_ptr_list(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, uint32_t nitems,
                                capnp_bptr_t *first) {
  size_t list_ptr_word;
  uint32_t list_seg;
  size_t list_start;
  if (nitems > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (slot_word_checked(body, dwords, ptr_index, &list_ptr_word))
    return CAPNP_ERR_BOUNDS;
  drop_slot(body->b, body->seg, list_ptr_word);
  if (alloc_for(body->b, body->seg, nitems, &list_seg, &list_start))
    return CAPNP_ERR_ALLOC;
  if (nitems)
    memset(wbytes(body->b, list_seg, list_start), 0,
           (size_t)nitems * CAPNP_WORD_BYTES);
  if (write_list_ptr(body->b, body->seg, list_ptr_word, list_seg, list_start,
                     CAPNP_SZ_PTR, nitems))
    return CAPNP_ERR_ALLOC;
  if (first) {
    first->b = body->b;
    first->seg = list_seg;
    first->word = list_start;
  }
  return CAPNP_OK;
}

int capnp_builder_set_list_struct(const capnp_bptr_t *body, uint16_t dwords,
                                  uint16_t ptr_index, uint32_t nitems,
                                  uint16_t elem_dwords, uint16_t elem_pwords,
                                  capnp_bptr_t *first_elem) {
  size_t list_ptr_word, step, content_words, need;
  uint32_t tag_seg;
  size_t tag_word;
  if (nitems > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (slot_word_checked(body, dwords, ptr_index, &list_ptr_word))
    return CAPNP_ERR_BOUNDS;
  drop_slot(body->b, body->seg, list_ptr_word);
  step = (size_t)elem_dwords + elem_pwords;
  if (step && (size_t)nitems > SIZE_MAX / step)
    return CAPNP_ERR_BOUNDS;
  content_words = (size_t)nitems * step;
  if (content_words > CAPNP_BUILDER_MAX_SEGMENT_WORDS ||
      content_words == SIZE_MAX)
    return CAPNP_ERR_BOUNDS;
  need = 1 + content_words;
  if (alloc_for(body->b, body->seg, need, &tag_seg, &tag_word))
    return CAPNP_ERR_ALLOC;
  store_w(body->b, tag_seg, tag_word,
          capnp_wp_make_struct((int32_t)nitems, elem_dwords, elem_pwords));
  if (write_list_ptr(body->b, body->seg, list_ptr_word, tag_seg, tag_word,
                     CAPNP_SZ_COMPOSITE, (uint32_t)content_words))
    return CAPNP_ERR_ALLOC;
  if (first_elem) {
    first_elem->b = body->b;
    first_elem->seg = tag_seg;
    first_elem->word = tag_word + 1;
  }
  return CAPNP_OK;
}

int capnp_builder_copy_ptr(capnp_bptr_t *slot, const capnp_ptr_t *src) {
  int rc = raw_slot_checked(slot);
  if (rc)
    return rc;
  drop_slot(slot->b, slot->seg, slot->word);
  return capnp_copy_ptr_to_word(slot->b, slot->seg, slot->word, src, 0);
}

static uint64_t load_w(const capnp_builder_t *b, uint32_t seg, size_t word) {
  return capnp_load_le64(b->segs[seg].data + word * CAPNP_WORD_BYTES);
}

static int list_nwords(int esize, uint32_t count, size_t *out) {
  size_t n;
  switch (esize) {
  case CAPNP_SZ_VOID:
    n = 0;
    break;
  case CAPNP_SZ_BIT:
    n = (size_t)(count / 64u) + (count % 64u != 0);
    break;
  case CAPNP_SZ_BYTE:
    n = (size_t)(count / 8u) + (count % 8u != 0);
    break;
  case CAPNP_SZ_TWO:
    n = (size_t)(count / 4u) + (count % 4u != 0);
    break;
  case CAPNP_SZ_FOUR:
    n = (size_t)(count / 2u) + (count % 2u != 0);
    break;
  case CAPNP_SZ_EIGHT:
  case CAPNP_SZ_PTR:
    n = count;
    break;
  default:
    return CAPNP_ERR_KIND;
  }
  *out = n;
  return CAPNP_OK;
}

/* Words past s->words are allocated with calloc and reused as if they
 * were still zero. A reclaimed tail has to be cleared before the next
 * object is placed there. */
static void zero_words(capnp_builder_t *b, uint32_t seg, size_t start,
                       size_t n) {
  capnp_bseg_t *s;
  if (!b || n == 0 || seg >= b->nsegs)
    return;
  s = &b->segs[seg];
  if (start >= s->words)
    return;
  if (n > s->words - start)
    n = s->words - start;
  memset(s->data + start * CAPNP_WORD_BYTES, 0, n * CAPNP_WORD_BYTES);
}

/* A zero-word list may name the segment end, the way C++ allocate(0)
 * does. checkOffset treats that end as in bounds. Shrinking the segment
 * below the target makes the pointer past the end. */
static void note_zero_list(uint32_t content_seg, size_t content_word, int esize,
                           uint32_t count, uint32_t watch, size_t *pin) {
  size_t n = 0;
  if (esize == CAPNP_SZ_COMPOSITE)
    return;
  if (list_nwords(esize, count, &n) || n != 0)
    return;
  if (content_seg == watch && content_word > *pin)
    *pin = content_word;
}

static void walk_slot(capnp_builder_t *b, uint32_t seg, size_t word, int depth,
                      uint32_t watch, size_t *pin);

static void walk_struct_ptrs(capnp_builder_t *b, uint32_t seg, size_t body,
                             uint16_t dw, uint16_t pw, int depth,
                             uint32_t watch, size_t *pin) {
  uint16_t i;
  if (seg >= b->nsegs)
    return;
  for (i = 0; i < pw; i++) {
    size_t pwrd = body + (size_t)dw + i;
    if (pwrd < b->segs[seg].words)
      walk_slot(b, seg, pwrd, depth + 1, watch, pin);
  }
}

static void walk_list_body(capnp_builder_t *b, uint32_t seg, size_t start,
                           int esize, uint32_t count, int depth, uint32_t watch,
                           size_t *pin) {
  note_zero_list(seg, start, esize, count, watch, pin);
  if (seg >= b->nsegs)
    return;
  if (esize == CAPNP_SZ_PTR) {
    uint32_t i;
    for (i = 0; i < count; i++) {
      size_t pwrd = start + i;
      if (pwrd < b->segs[seg].words)
        walk_slot(b, seg, pwrd, depth + 1, watch, pin);
    }
    return;
  }
  if (esize != CAPNP_SZ_COMPOSITE || start >= b->segs[seg].words)
    return;
  {
    uint64_t tag = load_w(b, seg, start);
    uint16_t dw, pw;
    size_t step, i, elems;
    if (capnp_wp_kind(tag) != CAPNP_WK_STRUCT)
      return;
    dw = capnp_wp_struct_dwords(tag);
    pw = capnp_wp_struct_pwords(tag);
    step = (size_t)dw + pw;
    elems = ((uint32_t)tag >> 2) & 0x1fffffffu;
    if (elems == 0)
      elems = step ? (size_t)count / step : 0;
    for (i = 0; i < elems; i++) {
      size_t base = start + 1u + i * step;
      walk_struct_ptrs(b, seg, base, dw, pw, depth, watch, pin);
    }
  }
}

static void walk_word(capnp_builder_t *b, uint32_t seg, size_t ptr_word,
                      uint64_t w, int depth, uint32_t watch, size_t *pin) {
  int kind;
  if (!b || depth > 64 || seg >= b->nsegs)
    return;
  kind = capnp_wp_kind(w);
  if (kind == CAPNP_WK_STRUCT || kind == CAPNP_WK_LIST) {
    int64_t at = (int64_t)ptr_word + 1 + capnp_wp_offset(w);
    if (at < 0)
      return;
    if (kind == CAPNP_WK_STRUCT) {
      uint16_t dw = capnp_wp_struct_dwords(w);
      uint16_t pw = capnp_wp_struct_pwords(w);
      if ((size_t)dw + pw == 0)
        return;
      if ((size_t)at >= b->segs[seg].words)
        return;
      walk_struct_ptrs(b, seg, (size_t)at, dw, pw, depth, watch, pin);
      return;
    }
    walk_list_body(b, seg, (size_t)at, capnp_wp_list_esize(w),
                   capnp_wp_list_count(w), depth, watch, pin);
    return;
  }
  if (kind != CAPNP_WK_FAR)
    return;
  if (capnp_wp_far_two(w)) {
    uint32_t pseg = capnp_wp_far_seg(w);
    uint32_t poff = capnp_wp_far_off(w);
    uint64_t pad, tag;
    uint32_t cseg;
    size_t coff;
    if (poff > UINT32_MAX - 1u || pseg >= b->nsegs ||
        (size_t)poff + 1u >= b->segs[pseg].words)
      return;
    pad = load_w(b, pseg, poff);
    tag = load_w(b, pseg, (size_t)poff + 1u);
    if (capnp_wp_kind(pad) != CAPNP_WK_FAR || capnp_wp_far_two(pad) ||
        capnp_wp_offset(tag) != 0)
      return;
    cseg = capnp_wp_far_seg(pad);
    coff = capnp_wp_far_off(pad);
    if (cseg >= b->nsegs)
      return;
    if (capnp_wp_kind(tag) == CAPNP_WK_STRUCT) {
      uint16_t dw = capnp_wp_struct_dwords(tag);
      uint16_t pw = capnp_wp_struct_pwords(tag);
      if ((size_t)dw + pw == 0)
        return;
      if (coff >= b->segs[cseg].words)
        return;
      walk_struct_ptrs(b, cseg, coff, dw, pw, depth, watch, pin);
      return;
    }
    if (capnp_wp_kind(tag) == CAPNP_WK_LIST)
      walk_list_body(b, cseg, coff, capnp_wp_list_esize(tag),
                     capnp_wp_list_count(tag), depth, watch, pin);
    return;
  }
  {
    uint32_t tseg = capnp_wp_far_seg(w);
    size_t toff = capnp_wp_far_off(w);
    uint64_t landing;
    if (tseg >= b->nsegs || toff >= b->segs[tseg].words)
      return;
    landing = load_w(b, tseg, toff);
    walk_word(b, tseg, toff, landing, depth + 1, watch, pin);
  }
}

static void walk_slot(capnp_builder_t *b, uint32_t seg, size_t word, int depth,
                      uint32_t watch, size_t *pin) {
  if (!b || depth > 64 || seg >= b->nsegs || word >= b->segs[seg].words)
    return;
  walk_word(b, seg, word, load_w(b, seg, word), depth, watch, pin);
}

/* Highest word a live zero-word list names in `seg`. The segment must
 * stay at least this long so the target is not past the end. */
static size_t empty_list_pin(capnp_builder_t *b, uint32_t seg) {
  size_t pin = 0;
  if (!b || b->nsegs == 0 || b->segs[0].words == 0)
    return 0;
  walk_slot(b, 0, 0, 0, seg, &pin);
  return pin;
}

/* Highest word a live orphan occupies in `seg`, and whether any live
 * orphan names the segment. Retired orphans leave nothing behind, so a
 * disown/adopt loop does not stop later reclaims. */
static int orphan_seg_scan(capnp_builder_t *b, uint32_t seg, size_t *pin) {
  struct capnp_builder_priv *priv = builder_priv(b);
  uint32_t i;
  int live = 0;
  *pin = 0;
  if (!priv)
    return 0;
  for (i = 0; i < priv->norefs; i++) {
    const struct capnp_oref *rec = &priv->orefs[i];
    if (rec->seg != seg)
      continue;
    live = 1;
    if (rec->end > *pin)
      *pin = rec->end;
  }
  return live;
}

/* Segment 0 holds the root. An emptied later segment at the end is a
 * malloc the message no longer needs unless a live orphan still names
 * it. Nothing else can: a pointer into it would need a word there. */
static void drop_empty_tail(capnp_builder_t *b) {
  size_t opin;
  while (b->nsegs > 1) {
    capnp_bseg_t *s = &b->segs[b->nsegs - 1u];
    if (s->words != 0 || orphan_seg_scan(b, b->nsegs - 1u, &opin))
      return;
    free(s->data);
    memset(s, 0, sizeof(*s));
    b->nsegs--;
  }
}

static void reclaim_tail(capnp_builder_t *b, uint32_t seg, size_t start,
                         size_t n) {
  capnp_bseg_t *s;
  size_t pin, opin, keep;
  int has_live;
  if (!b || n == 0 || seg >= b->nsegs)
    return;
  s = &b->segs[seg];
  if (start > s->words || n > s->words - start || start + n != s->words)
    return;
  zero_words(b, seg, start, n);
  pin = empty_list_pin(b, seg);
  has_live = orphan_seg_scan(b, seg, &opin);
  if (opin > pin)
    pin = opin;
  /* A target equal to the word count is the segment end, which a
   * zero-word object may name. Do not move the end below that word. */
  keep = pin > start ? pin : start;
  if (keep > s->words)
    keep = s->words;
  s->words = keep;
  if (s->words == 0 && seg + 1u == b->nsegs && !has_live)
    drop_empty_tail(b);
}

static void drop_ptr(capnp_builder_t *b, uint32_t seg, size_t word, int depth);

/* Zero the struct or list `w` describes. `target` is that object's
 * first word (struct body, list content, or composite tag), so a
 * double-far landing pad can name word 0 of its segment. A zero-word
 * list may name the segment end. When `with_pad` is set and the object
 * ends on `pad_word`, that landing-pad word is released with it. */
static void release_target(capnp_builder_t *b, uint32_t seg, size_t target,
                           uint64_t w, int depth, int with_pad,
                           size_t pad_word) {
  int kind = capnp_wp_kind(w);
  if (seg >= b->nsegs)
    return;
  if (kind == CAPNP_WK_STRUCT) {
    uint16_t dw = capnp_wp_struct_dwords(w);
    uint16_t pw = capnp_wp_struct_pwords(w);
    size_t n = (size_t)dw + pw;
    uint16_t i;
    if (n == 0)
      return;
    if (target >= b->segs[seg].words)
      return;
    /* WireHelpers::zeroObject walks pointer words from the first to the
     * last, then zeroes the struct body (layout.c++). */
    for (i = 0; i < pw; i++) {
      size_t pwrd = target + dw + i;
      if (pwrd < b->segs[seg].words)
        drop_ptr(b, seg, pwrd, depth + 1);
    }
    zero_words(b, seg, target, n);
    if (with_pad && target + n == pad_word)
      reclaim_tail(b, seg, target, n + 1u);
    else
      reclaim_tail(b, seg, target, n);
    return;
  }
  if (kind != CAPNP_WK_LIST)
    return;
  if (capnp_wp_list_esize(w) == CAPNP_SZ_COMPOSITE) {
    uint64_t tag;
    uint16_t dw, pw;
    size_t step, words, i, count;
    if (target >= b->segs[seg].words)
      return;
    tag = load_w(b, seg, target);
    if (capnp_wp_kind(tag) != CAPNP_WK_STRUCT)
      return;
    dw = capnp_wp_struct_dwords(tag);
    pw = capnp_wp_struct_pwords(tag);
    step = (size_t)dw + pw;
    words = (size_t)capnp_wp_list_count(w);
    count = step ? words / step : 0;
    /* Same order as zeroObject on an inline composite: each element's
     * pointers from first to last, then the tag and element words. */
    for (i = 0; i < count; i++) {
      size_t base = target + 1u + i * step;
      uint16_t p;
      for (p = 0; p < pw; p++) {
        size_t pwrd = base + dw + p;
        if (pwrd < b->segs[seg].words)
          drop_ptr(b, seg, pwrd, depth + 1);
      }
    }
    zero_words(b, seg, target, words + 1u);
    if (with_pad && target + 1u + words == pad_word)
      reclaim_tail(b, seg, target, words + 2u);
    else
      reclaim_tail(b, seg, target, words + 1u);
    return;
  }
  {
    size_t n = 0;
    int esize = capnp_wp_list_esize(w);
    uint32_t count = capnp_wp_list_count(w);
    if (list_nwords(esize, count, &n))
      return;
    if (n > 0 && target >= b->segs[seg].words)
      return;
    if (esize == CAPNP_SZ_PTR) {
      uint32_t i;
      for (i = 0; i < count; i++) {
        size_t pwrd = target + i;
        if (pwrd < b->segs[seg].words)
          drop_ptr(b, seg, pwrd, depth + 1);
      }
    }
    if (n > 0)
      zero_words(b, seg, target, n);
    if (with_pad && n > 0 && target + n == pad_word)
      reclaim_tail(b, seg, target, n + 1u);
    else
      reclaim_tail(b, seg, target, n);
  }
}

/* Free the object named by a struct or list pointer word `w` that lives
 * at `ptr_word`. When `pad_after` is set, `ptr_word` is a landing pad
 * sitting just after that object and is released with it. */
static void release_near(capnp_builder_t *b, uint32_t seg, size_t ptr_word,
                         uint64_t w, int depth, int pad_after) {
  int kind = capnp_wp_kind(w);
  int64_t at;
  if (seg >= b->nsegs)
    return;
  if (kind != CAPNP_WK_STRUCT && kind != CAPNP_WK_LIST)
    return;
  at = (int64_t)ptr_word + 1 + capnp_wp_offset(w);
  if (at < 0)
    return;
  release_target(b, seg, (size_t)at, w, depth, pad_after, ptr_word);
}

static void drop_ptr(capnp_builder_t *b, uint32_t seg, size_t word, int depth) {
  uint64_t w;
  int kind;
  if (!b || depth > 64 || seg >= b->nsegs || word >= b->segs[seg].words)
    return;
  w = load_w(b, seg, word);
  if (w == 0)
    return;
  kind = capnp_wp_kind(w);
  store_w(b, seg, word, 0);
  if (kind == CAPNP_WK_STRUCT || kind == CAPNP_WK_LIST) {
    release_near(b, seg, word, w, depth, 0);
    return;
  }
  if (kind != CAPNP_WK_FAR)
    return;
  if (capnp_wp_far_two(w)) {
    uint32_t pseg = capnp_wp_far_seg(w);
    uint32_t poff = capnp_wp_far_off(w);
    uint64_t pad, tag;
    uint32_t cseg;
    size_t coff;
    if (poff > UINT32_MAX - 1u || pseg >= b->nsegs ||
        (size_t)poff + 1u >= b->segs[pseg].words)
      return;
    pad = load_w(b, pseg, poff);
    tag = load_w(b, pseg, (size_t)poff + 1u);
    store_w(b, pseg, poff, 0);
    store_w(b, pseg, (size_t)poff + 1u, 0);
    if (capnp_wp_kind(pad) != CAPNP_WK_FAR || capnp_wp_far_two(pad) ||
        capnp_wp_offset(tag) != 0)
      return;
    cseg = capnp_wp_far_seg(pad);
    coff = capnp_wp_far_off(pad);
    /* coff == words is the segment end, which a zero-word object may
     * name. A target past that is not an object. Word 0 is a real
     * object when the segment is non-empty, the same as any other
     * in-bounds offset. C++ zeroObject follows pad->farTarget with no
     * special case for the first word (layout.c++). */
    if (cseg >= b->nsegs || coff > b->segs[cseg].words)
      return;
    if (capnp_wp_kind(tag) == CAPNP_WK_STRUCT ||
        capnp_wp_kind(tag) == CAPNP_WK_LIST)
      release_target(b, cseg, coff, tag, depth, 0, 0);
    reclaim_tail(b, pseg, poff, 2);
    return;
  }
  {
    uint32_t tseg = capnp_wp_far_seg(w);
    size_t toff = capnp_wp_far_off(w);
    uint64_t landing;
    if (tseg >= b->nsegs || toff >= b->segs[tseg].words)
      return;
    landing = load_w(b, tseg, toff);
    store_w(b, tseg, toff, 0);
    if (capnp_wp_kind(landing) == CAPNP_WK_STRUCT ||
        capnp_wp_kind(landing) == CAPNP_WK_LIST)
      release_near(b, tseg, toff, landing, depth, 1);
  }
}

static void drop_slot(capnp_builder_t *b, uint32_t seg, size_t word) {
  drop_ptr(b, seg, word, 0);
}

static int orphan_grow(struct capnp_builder_priv *priv) {
  uint32_t cap;
  struct capnp_oref *grown;
  if (priv->norefs < priv->orefs_cap)
    return CAPNP_OK;
  cap = priv->orefs_cap ? priv->orefs_cap * 2u : 8u;
  if (priv->orefs_cap && cap < priv->orefs_cap)
    return CAPNP_ERR_ALLOC;
#if SIZE_MAX <= UINT32_MAX
  if ((size_t)cap > SIZE_MAX / sizeof(*grown))
    return CAPNP_ERR_ALLOC;
#endif
  grown = (struct capnp_oref *)realloc(priv->orefs, (size_t)cap * sizeof(*grown));
  if (!grown)
    return CAPNP_ERR_ALLOC;
  priv->orefs = grown;
  priv->orefs_cap = cap;
  return CAPNP_OK;
}

static int orphan_matches(const struct capnp_oref *rec,
                          const capnp_orphan_t *o) {
  return rec->seg == o->seg && rec->word == o->word &&
         rec->kind == o->kind && rec->dwords == o->dwords &&
         rec->pwords == o->pwords && rec->esize == o->esize &&
         rec->count == o->count;
}

/* Match by public orphan values in the arena side table so by-value and
 * designated-initializer copies work without relying on struct padding.
 * Prefer the registered variable address when multiple zero-word orphans
 * share a tail position. */
static int orphan_lookup_index(const capnp_orphan_t *o,
                               struct capnp_builder_priv **priv_out,
                               uint32_t *idx_out) {
  struct capnp_builder_priv *priv;
  uintptr_t addr;
  uint32_t i, fallback = 0;
  int found = 0;
  if (!o || !o->live || !o->b)
    return 0;
  priv = builder_priv(o->b);
  if (!priv)
    return 0;
  addr = (uintptr_t)o;
  for (i = priv->norefs; i > 0; i--) {
    const struct capnp_oref *rec = &priv->orefs[i - 1u];
    if (!orphan_matches(rec, o))
      continue;
    if (rec->owner_id == addr) {
      if (priv_out)
        *priv_out = priv;
      if (idx_out)
        *idx_out = i - 1u;
      return 1;
    }
    if (!found) {
      found = 1;
      fallback = i - 1u;
    }
  }
  if (!found)
    return 0;
  if (priv_out)
    *priv_out = priv;
  if (idx_out)
    *idx_out = fallback;
  return 1;
}

static int orphan_register(capnp_builder_t *b, capnp_orphan_t *o, size_t nwords) {
  struct capnp_builder_priv *priv = builder_priv(b);
  struct capnp_oref *rec;
  if (!priv)
    return CAPNP_ERR_ALLOC;
  if (nwords > 0 && nwords > SIZE_MAX - o->word)
    return CAPNP_ERR_BOUNDS;
  if (orphan_grow(priv))
    return CAPNP_ERR_ALLOC;
  rec = &priv->orefs[priv->norefs++];
  rec->owner_id = (uintptr_t)o;
  rec->seg = o->seg;
  rec->word = o->word;
  rec->end = nwords > 0 ? o->word + nwords : 0;
  rec->kind = o->kind;
  rec->dwords = o->dwords;
  rec->pwords = o->pwords;
  rec->esize = o->esize;
  rec->count = o->count;
  o->live = 1;
  return CAPNP_OK;
}

static int orphan_is_live(const capnp_orphan_t *o) {
  return orphan_lookup_index(o, NULL, NULL);
}

static void orphan_retire(capnp_orphan_t *o) {
  struct capnp_builder_priv *priv;
  uint32_t idx;
  if (!orphan_lookup_index(o, &priv, &idx))
    return;
  priv->norefs--;
  if (idx < priv->norefs)
    priv->orefs[idx] = priv->orefs[priv->norefs];
  o->live = 0;
  drop_empty_tail(o->b);
}

int capnp_builder_new_orphan_struct(capnp_builder_t *b, uint16_t dwords,
                                    uint16_t pwords, capnp_orphan_t *o,
                                    capnp_bptr_t *body) {
  size_t n = (size_t)dwords + pwords;
  uint32_t seg = 0;
  size_t word = 0;
  if (!b || !o)
    return CAPNP_ERR_ARG;
  memset(o, 0, sizeof(*o));
  if (n) {
    if (alloc_words(b, n, &seg, &word))
      return CAPNP_ERR_ALLOC;
    memset(wbytes(b, seg, word), 0, n * CAPNP_WORD_BYTES);
  } else if (tail_slot(b, &seg, &word)) {
    return CAPNP_ERR_ALLOC;
  }
  o->b = b;
  o->seg = seg;
  o->word = word;
  o->kind = CAPNP_PK_STRUCT;
  o->dwords = dwords;
  o->pwords = pwords;
  if (orphan_register(b, o, n)) {
    if (n)
      reclaim_tail(b, seg, word, n);
    memset(o, 0, sizeof(*o));
    return CAPNP_ERR_ALLOC;
  }
  if (body) {
    body->b = b;
    body->seg = seg;
    body->word = word;
  }
  return CAPNP_OK;
}

int capnp_builder_new_orphan_list(capnp_builder_t *b, int esize, uint32_t count,
                                  capnp_orphan_t *o, capnp_bptr_t *start) {
  size_t n = 0;
  uint32_t seg = 0;
  size_t word = 0;
  if (!b || !o)
    return CAPNP_ERR_ARG;
  if (list_nwords(esize, count, &n))
    return CAPNP_ERR_KIND;
  /* C++ 1.4.0 rejects an element count above 2^29-1 when the orphan is
   * created, for every element size, not when it is adopted. */
  if (count > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  memset(o, 0, sizeof(*o));
  if (n) {
    if (alloc_words(b, n, &seg, &word))
      return CAPNP_ERR_ALLOC;
    memset(wbytes(b, seg, word), 0, n * CAPNP_WORD_BYTES);
  } else if (tail_slot(b, &seg, &word)) {
    return CAPNP_ERR_ALLOC;
  }
  o->b = b;
  o->seg = seg;
  o->word = word;
  o->kind = CAPNP_PK_LIST;
  o->esize = esize;
  o->count = count;
  if (orphan_register(b, o, n)) {
    if (n)
      reclaim_tail(b, seg, word, n);
    memset(o, 0, sizeof(*o));
    return CAPNP_ERR_ALLOC;
  }
  if (start) {
    start->b = b;
    start->seg = seg;
    start->word = word;
  }
  return CAPNP_OK;
}

int capnp_builder_new_orphan_struct_list(capnp_builder_t *b, uint32_t count,
                                          uint16_t elem_dwords,
                                          uint16_t elem_pwords,
                                          capnp_orphan_t *o,
                                          capnp_bptr_t *first) {
  size_t step = (size_t)elem_dwords + elem_pwords;
  size_t content = (size_t)count * step;
  size_t need = 1 + content;
  uint32_t seg = 0;
  size_t word = 0;
  if (!b || !o)
    return CAPNP_ERR_ARG;
  /* The tag stores the element count in a 30-bit field, and
   * capnp_wp_make_struct keeps only those bits. C++ 1.4.0 rejects an
   * element count above 2^29-1 before writing the tag. A larger count
   * would be truncated (2^30 becomes zero) and still adopt. */
  if (count > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
    return CAPNP_ERR_BOUNDS;
  if (count && content / count != step)
    return CAPNP_ERR_BOUNDS;
  if (content == SIZE_MAX)
    return CAPNP_ERR_BOUNDS;
  memset(o, 0, sizeof(*o));
  if (alloc_words(b, need, &seg, &word))
    return CAPNP_ERR_ALLOC;
  memset(wbytes(b, seg, word), 0, need * CAPNP_WORD_BYTES);
  store_w(b, seg, word,
          capnp_wp_make_struct((int32_t)count, elem_dwords, elem_pwords));
  o->b = b;
  o->seg = seg;
  o->word = word;
  o->kind = CAPNP_PK_LIST;
  o->esize = CAPNP_SZ_COMPOSITE;
  o->count = (uint32_t)content;
  o->dwords = elem_dwords;
  o->pwords = elem_pwords;
  if (orphan_register(b, o, need)) {
    reclaim_tail(b, seg, word, need);
    memset(o, 0, sizeof(*o));
    return CAPNP_ERR_ALLOC;
  }
  if (first) {
    first->b = b;
    first->seg = seg;
    first->word = word + 1;
  }
  return CAPNP_OK;
}

/* Drop an orphan the caller will not adopt. Pointer children are
 * released first, then the object's own words are zeroed, matching
 * OrphanBuilder::euthanize (which does not rewind the segment tail). */
int capnp_builder_orphan_free(capnp_orphan_t *o) {
  capnp_builder_t *b;
  size_t n = 0;
  uint32_t seg;
  if (!orphan_is_live(o))
    return CAPNP_ERR_ARG;
  b = o->b;
  seg = o->seg;
  if (o->kind == CAPNP_PK_NULL) {
    orphan_retire(o);
    return CAPNP_OK;
  }
  if (seg >= b->nsegs) {
    orphan_retire(o);
    return CAPNP_ERR_SEGMENT;
  }
  if (o->kind == CAPNP_PK_STRUCT) {
    uint16_t i;
    n = (size_t)o->dwords + o->pwords;
    if (n && o->word <= b->segs[seg].words) {
      size_t rem = b->segs[seg].words - o->word;
      if (o->dwords <= rem) {
        rem -= o->dwords;
        for (i = 0; i < o->pwords && (size_t)i < rem; i++)
          drop_ptr(b, seg, o->word + o->dwords + i, 0);
      }
    }
  } else if (o->kind == CAPNP_PK_LIST && o->esize == CAPNP_SZ_COMPOSITE) {
    uint64_t tag;
    uint16_t dw, pw;
    size_t step, i, elems;
    if (o->word >= b->segs[seg].words ||
        (size_t)o->count > b->segs[seg].words - 1u - o->word) {
      orphan_retire(o);
      return CAPNP_ERR_BOUNDS;
    }
    n = (size_t)o->count + 1u;
    tag = load_w(b, seg, o->word);
    dw = capnp_wp_struct_dwords(tag);
    pw = capnp_wp_struct_pwords(tag);
    step = (size_t)dw + pw;
    elems = step ? (size_t)o->count / step : 0;
    for (i = 0; i < elems; i++) {
      uint16_t p;
      size_t base = o->word + 1u + i * step;
      for (p = 0; p < pw; p++) {
        size_t pwrd = base + dw + p;
        if (pwrd < b->segs[seg].words)
          drop_ptr(b, seg, pwrd, 0);
      }
    }
  } else if (o->kind == CAPNP_PK_LIST) {
    if (list_nwords(o->esize, o->count, &n)) {
      orphan_retire(o);
      return CAPNP_ERR_KIND;
    }
    if (o->esize == CAPNP_SZ_PTR && o->word <= b->segs[seg].words) {
      size_t rem = b->segs[seg].words - o->word;
      uint32_t i;
      for (i = 0; i < o->count && (size_t)i < rem; i++)
        drop_ptr(b, seg, o->word + i, 0);
    }
  } else {
    orphan_retire(o);
    return CAPNP_ERR_KIND;
  }
  if (n)
    zero_words(b, seg, o->word, n);
  orphan_retire(o);
  return CAPNP_OK;
}

int capnp_builder_adopt(const capnp_bptr_t *slot, capnp_orphan_t *o) {
  int rc;
  if (!slot || !slot->b || !orphan_is_live(o) || o->b != slot->b)
    return CAPNP_ERR_ARG;
  if ((rc = raw_slot_checked(slot)) != CAPNP_OK)
    return rc;
  if (o->kind != CAPNP_PK_NULL && o->kind != CAPNP_PK_STRUCT &&
      o->kind != CAPNP_PK_LIST)
    return CAPNP_ERR_KIND;
  /* WireHelpers::adopt zeroes the slot's old target before linking. */
  drop_slot(slot->b, slot->seg, slot->word);
  if (o->kind == CAPNP_PK_NULL) {
    store_w(slot->b, slot->seg, slot->word, 0);
  } else if (o->kind == CAPNP_PK_STRUCT) {
    if (o->dwords == 0 && o->pwords == 0)
      store_w(slot->b, slot->seg, slot->word, capnp_wp_make_struct(-1, 0, 0));
    else if ((rc = write_struct_ptr(slot->b, slot->seg, slot->word, o->seg,
                                    o->word, o->dwords, o->pwords)))
      return rc;
  } else if (o->kind == CAPNP_PK_LIST) {
    uint32_t lseg = o->seg;
    size_t lword = o->word;
    if (o->esize != CAPNP_SZ_COMPOSITE) {
      size_t nw = 0;
      if (list_nwords(o->esize, o->count, &nw))
        return CAPNP_ERR_KIND;
      if (nw == 0 && tail_slot(slot->b, &lseg, &lword))
        return CAPNP_ERR_ALLOC;
    }
    if ((rc = write_list_ptr(slot->b, slot->seg, slot->word, lseg, lword,
                             o->esize, o->count)))
      return rc;
  } else {
    return CAPNP_ERR_KIND;
  }
  orphan_retire(o);
  return CAPNP_OK;
}

static int orphan_struct_body(capnp_builder_t *b, uint32_t seg, size_t word,
                               uint16_t dwords, uint16_t pwords,
                               capnp_orphan_t *o) {
  size_t n = (size_t)dwords + pwords;
  if (seg >= b->nsegs)
    return CAPNP_ERR_SEGMENT;
  if (n == 0) {
    if (word > b->segs[seg].words)
      return CAPNP_ERR_BOUNDS;
  } else if (word >= b->segs[seg].words ||
             word > SIZE_MAX - (n - 1) ||
             word + (n - 1) >= b->segs[seg].words) {
    return CAPNP_ERR_BOUNDS;
  }
  o->kind = CAPNP_PK_STRUCT;
  o->dwords = dwords;
  o->pwords = pwords;
  o->seg = seg;
  o->word = word;
  return CAPNP_OK;
}

static int orphan_list_body(capnp_builder_t *b, uint32_t seg, size_t start,
                            int esize, uint32_t count, capnp_orphan_t *o) {
  if (seg >= b->nsegs)
    return CAPNP_ERR_SEGMENT;
  o->kind = CAPNP_PK_LIST;
  o->esize = esize;
  o->count = count;
  o->seg = seg;
  o->word = start;
  if (esize == CAPNP_SZ_COMPOSITE) {
    uint64_t tag;
    if (start >= b->segs[seg].words)
      return CAPNP_ERR_BOUNDS;
    tag = load_w(b, seg, start);
    if (capnp_wp_kind(tag) != CAPNP_WK_STRUCT)
      return CAPNP_ERR_KIND;
    o->dwords = capnp_wp_struct_dwords(tag);
    o->pwords = capnp_wp_struct_pwords(tag);
    return CAPNP_OK;
  }
  if (count == 0)
    return CAPNP_OK;
  /* A List(Void) occupies no words, so it may name the segment end. */
  if (esize == CAPNP_SZ_VOID ? start > b->segs[seg].words
                             : start >= b->segs[seg].words)
    return CAPNP_ERR_BOUNDS;
  return CAPNP_OK;
}

/* Follow a near, single-far, or double-far pointer to the object it names.
 * A double-far landing pad is left in place. Only the original slot is
 * cleared by the caller, matching C++ disown. */
static int orphan_from_word(capnp_builder_t *b, uint32_t seg, size_t word,
                            capnp_orphan_t *o) {
  uint64_t w;
  int kind;
  if (seg >= b->nsegs || word >= b->segs[seg].words)
    return CAPNP_ERR_BOUNDS;
  w = load_w(b, seg, word);
  /* WireHelpers::disown gives a null orphan for a null slot. */
  if (w == 0) {
    o->kind = CAPNP_PK_NULL;
    return CAPNP_OK;
  }
  kind = capnp_wp_kind(w);
  if (kind == CAPNP_WK_FAR && capnp_wp_far_two(w)) {
    uint32_t pseg = capnp_wp_far_seg(w);
    uint32_t poff = capnp_wp_far_off(w);
    uint64_t pad, tag;
    uint32_t cseg, coff;
    if (poff > UINT32_MAX - 1u)
      return CAPNP_ERR_BOUNDS;
    if (pseg >= b->nsegs || (size_t)poff + 1u >= b->segs[pseg].words)
      return CAPNP_ERR_BOUNDS;
    pad = load_w(b, pseg, poff);
    tag = load_w(b, pseg, (size_t)poff + 1u);
    if (capnp_wp_kind(pad) != CAPNP_WK_FAR || capnp_wp_far_two(pad))
      return CAPNP_ERR_KIND;
    if (capnp_wp_offset(tag) != 0)
      return CAPNP_ERR_KIND;
    cseg = capnp_wp_far_seg(pad);
    coff = capnp_wp_far_off(pad);
    if (capnp_wp_kind(tag) == CAPNP_WK_STRUCT)
      return orphan_struct_body(b, cseg, coff, capnp_wp_struct_dwords(tag),
                                capnp_wp_struct_pwords(tag), o);
    if (capnp_wp_kind(tag) == CAPNP_WK_LIST)
      return orphan_list_body(b, cseg, coff, capnp_wp_list_esize(tag),
                              capnp_wp_list_count(tag), o);
    return CAPNP_ERR_KIND;
  }
  if (kind == CAPNP_WK_FAR && !capnp_wp_far_two(w)) {
    seg = capnp_wp_far_seg(w);
    word = capnp_wp_far_off(w);
    if (seg >= b->nsegs || word >= b->segs[seg].words)
      return CAPNP_ERR_BOUNDS;
    w = load_w(b, seg, word);
    kind = capnp_wp_kind(w);
  }
  if (kind == CAPNP_WK_STRUCT) {
    size_t n = (size_t)capnp_wp_struct_dwords(w) + capnp_wp_struct_pwords(w);
    o->kind = CAPNP_PK_STRUCT;
    o->dwords = capnp_wp_struct_dwords(w);
    o->pwords = capnp_wp_struct_pwords(w);
    if (n == 0) {
      o->seg = seg;
      o->word = word;
    } else {
      int64_t body = (int64_t)word + 1 + capnp_wp_offset(w);
      if (body < 0 || (size_t)body >= b->segs[seg].words)
        return CAPNP_ERR_BOUNDS;
      o->seg = seg;
      o->word = (size_t)body;
    }
    return CAPNP_OK;
  }
  if (kind == CAPNP_WK_LIST) {
    int64_t start;
    if (capnp_wp_list_esize(w) == CAPNP_SZ_COMPOSITE) {
      uint64_t tag;
      start = (int64_t)word + 1 + capnp_wp_offset(w);
      if (start < 0 || (size_t)start >= b->segs[seg].words)
        return CAPNP_ERR_BOUNDS;
      tag = load_w(b, seg, (size_t)start);
      if (capnp_wp_kind(tag) != CAPNP_WK_STRUCT)
        return CAPNP_ERR_KIND;
      o->kind = CAPNP_PK_LIST;
      o->esize = CAPNP_SZ_COMPOSITE;
      o->count = capnp_wp_list_count(w);
      o->dwords = capnp_wp_struct_dwords(tag);
      o->pwords = capnp_wp_struct_pwords(tag);
      o->seg = seg;
      o->word = (size_t)start;
      return CAPNP_OK;
    }
    start = (int64_t)word + 1 + capnp_wp_offset(w);
    o->kind = CAPNP_PK_LIST;
    o->esize = capnp_wp_list_esize(w);
    o->count = capnp_wp_list_count(w);
    if (o->count == 0) {
      o->seg = seg;
      o->word = word;
      return CAPNP_OK;
    }
    if (start < 0 || (o->esize == CAPNP_SZ_VOID
                          ? (size_t)start > b->segs[seg].words
                          : (size_t)start >= b->segs[seg].words))
      return CAPNP_ERR_BOUNDS;
    o->seg = seg;
    o->word = (size_t)start;
    return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

int capnp_builder_disown(const capnp_bptr_t *slot, capnp_orphan_t *o) {
  int rc;
  size_t n = 0;
  if (!slot || !slot->b || !o)
    return CAPNP_ERR_ARG;
  if ((rc = raw_slot_checked(slot)) != CAPNP_OK)
    return rc;
  memset(o, 0, sizeof(*o));
  rc = orphan_from_word(slot->b, slot->seg, slot->word, o);
  if (rc)
    return rc;
  if (o->kind == CAPNP_PK_STRUCT) {
    n = (size_t)o->dwords + o->pwords;
  } else if (o->kind == CAPNP_PK_LIST && o->esize == CAPNP_SZ_COMPOSITE) {
#if SIZE_MAX <= UINT32_MAX
    if ((size_t)o->count > SIZE_MAX - 1u) {
      memset(o, 0, sizeof(*o));
      return CAPNP_ERR_BOUNDS;
    }
#endif
    n = (size_t)o->count + 1u;
  } else if (o->kind == CAPNP_PK_LIST) {
    if (list_nwords(o->esize, o->count, &n)) {
      memset(o, 0, sizeof(*o));
      return CAPNP_ERR_KIND;
    }
  }
  o->b = slot->b;
  if (orphan_register(slot->b, o, n)) {
    memset(o, 0, sizeof(*o));
    return CAPNP_ERR_ALLOC;
  }
  store_w(slot->b, slot->seg, slot->word, 0);
  return CAPNP_OK;
}

int capnp_builder_serialize(const capnp_builder_t *b, uint8_t **out,
                            size_t *out_len) {
  size_t table_bytes, body_words = 0, total, off;
  uint32_t i;
  uint8_t *buf;
  if (!b || !out || !out_len || b->nsegs == 0 || b->segs[0].words == 0)
    return CAPNP_ERR_ARG;
  for (i = 0; i < b->nsegs; i++) {
    if (b->segs[i].words > CAPNP_BUILDER_MAX_SEGMENT_WORDS ||
        b->segs[i].words > SIZE_MAX - body_words)
      return CAPNP_ERR_BOUNDS;
    body_words += b->segs[i].words;
  }
  table_bytes = 4u + 4u * (size_t)b->nsegs;
  if (table_bytes % 8u != 0)
    table_bytes += 4u;
  if (body_words > (SIZE_MAX - table_bytes) / CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  total = table_bytes + body_words * CAPNP_WORD_BYTES;
  buf = (uint8_t *)malloc(total);
  if (!buf)
    return CAPNP_ERR_ALLOC;
  capnp_store_le32(buf, b->nsegs - 1);
  for (i = 0; i < b->nsegs; i++)
    capnp_store_le32(buf + 4 + 4 * i, (uint32_t)b->segs[i].words);
  if (table_bytes > 4u + 4u * b->nsegs)
    memset(buf + 4 + 4 * b->nsegs, 0, table_bytes - (4 + 4 * b->nsegs));
  off = table_bytes;
  for (i = 0; i < b->nsegs; i++) {
    size_t nbytes = b->segs[i].words * CAPNP_WORD_BYTES;
    if (nbytes)
      memcpy(buf + off, b->segs[i].data, nbytes);
    off += nbytes;
  }
  *out = buf;
  *out_len = total;
  return CAPNP_OK;
}
