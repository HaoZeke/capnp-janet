#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_copy.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdlib.h>
#include <string.h>

static size_t lim_words(const capnp_builder_t *b) {
  return b->max_seg_words ? b->max_seg_words : CAPNP_BUILDER_MAX_SEGMENT_WORDS;
}

static int ensure_segs_cap(capnp_builder_t *b, uint32_t need) {
  uint32_t ncap;
  capnp_bseg_t *p;
  if (need <= b->segs_cap)
    return CAPNP_OK;
  ncap = b->segs_cap ? b->segs_cap * 2u : 4u;
  while (ncap < need)
    ncap *= 2u;
  p = (capnp_bseg_t *)realloc(b->segs, (size_t)ncap * sizeof(capnp_bseg_t));
  if (!p)
    return CAPNP_ERR_ALLOC;
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

static int alloc_words(capnp_builder_t *b, size_t n, uint32_t *out_seg,
                       size_t *out_word) {
  capnp_bseg_t *last;
  size_t lim = lim_words(b);
  size_t ncap;
  if (!b || !out_seg || !out_word || b->nsegs == 0)
    return CAPNP_ERR_ARG;
  if (n > lim)
    return CAPNP_ERR_ALLOC;
  last = &b->segs[b->nsegs - 1];
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
  if (s->words + n > lim_words(b))
    return CAPNP_ERR_ALLOC;
  if (seg_ensure(b, seg, s->words + n))
    return CAPNP_ERR_ALLOC;
  s = &b->segs[seg];
  *out_word = s->words;
  s->words += n;
  return CAPNP_OK;
}


/* Allocate n words for a far landing pad, never in avoid_seg. Always appends
 * a fresh segment so content segments at capacity stay untouched. */
static int alloc_pad_segment(capnp_builder_t *b, uint32_t avoid_seg, size_t n,
                             uint32_t *out_seg, size_t *out_word) {
  size_t saved = b->max_seg_words;
  size_t need = n < 1 ? 1 : n;
  (void)avoid_seg;
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
  if (alloc_in(b, body_seg, 1, &pad) == CAPNP_OK) {
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

  if (slot_seg == content_seg) {
    int32_t off =
        (int32_t)((int64_t)content_word - (int64_t)slot_word - 1);
    store_w(b, slot_seg, slot_word, capnp_wp_make_list(off, esize, count));
    return CAPNP_OK;
  }
  if (alloc_in(b, content_seg, 1, &pad) == CAPNP_OK) {
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
    free(b->segs);
    memset(b, 0, sizeof(*b));
  }
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
  free(b->segs);
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

int capnp_builder_struct(capnp_bptr_t *ptr, uint16_t dwords, uint16_t pwords,
                         capnp_bptr_t *body_out) {
  size_t n = (size_t)dwords + pwords;
  uint32_t body_seg;
  size_t body_word;
  int rc;
  if (!ptr || !ptr->b)
    return CAPNP_ERR_ARG;
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
  if (alloc_words(ptr->b, n, &body_seg, &body_word))
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

static int body_ok(const capnp_bptr_t *body, size_t abs_end) {
  if (!body || !body->b || body->seg >= body->b->nsegs)
    return CAPNP_ERR_ARG;
  if (abs_end > body->b->segs[body->seg].words * CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  return CAPNP_OK;
}

int capnp_builder_set_u8(const capnp_bptr_t *body, uint32_t byte_offset,
                         uint8_t value) {
  size_t abs;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  abs = body->word * CAPNP_WORD_BYTES + byte_offset;
  if (body_ok(body, abs + 1))
    return CAPNP_ERR_BOUNDS;
  body->b->segs[body->seg].data[abs] = value;
  return CAPNP_OK;
}

int capnp_builder_set_u16(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint16_t value) {
  size_t abs;
  uint8_t *p;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  abs = body->word * CAPNP_WORD_BYTES + byte_offset;
  if (body_ok(body, abs + 2))
    return CAPNP_ERR_BOUNDS;
  p = body->b->segs[body->seg].data + abs;
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  return CAPNP_OK;
}

int capnp_builder_set_u32(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint32_t value) {
  size_t abs;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  abs = body->word * CAPNP_WORD_BYTES + byte_offset;
  if (body_ok(body, abs + 4))
    return CAPNP_ERR_BOUNDS;
  capnp_store_le32(body->b->segs[body->seg].data + abs, value);
  return CAPNP_OK;
}

int capnp_builder_set_u64(const capnp_bptr_t *body, uint32_t byte_offset,
                          uint64_t value) {
  size_t abs;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  abs = body->word * CAPNP_WORD_BYTES + byte_offset;
  if (body_ok(body, abs + 8))
    return CAPNP_ERR_BOUNDS;
  capnp_store_le64(body->b->segs[body->seg].data + abs, value);
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
  if (!body || !body->b || !slot)
    return CAPNP_ERR_ARG;
  pw = body->word + dwords + ptr_index;
  if (pw >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
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
  store_w(slot.b, slot.seg, slot.word, 0);
  return CAPNP_OK;
}

int capnp_builder_set_bool(const capnp_bptr_t *body, uint32_t bit_offset,
                           int value) {
  size_t abs;
  uint8_t bit, *p;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  abs = body->word * CAPNP_WORD_BYTES + bit_offset / 8;
  if (body_ok(body, abs + 1))
    return CAPNP_ERR_BOUNDS;
  p = body->b->segs[body->seg].data + abs;
  bit = (uint8_t)(1u << (bit_offset % 8));
  if (value)
    *p |= bit;
  else
    *p &= (uint8_t)~bit;
  return CAPNP_OK;
}

static int write_text_at(capnp_builder_t *b, uint32_t slot_seg, size_t slot_word,
                         const char *text, size_t text_len) {
  size_t nbytes = text_len + 1;
  size_t nwords = (nbytes + 7) / 8;
  uint32_t start_seg;
  size_t start_word;
  if (alloc_words(b, nwords, &start_seg, &start_word))
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
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  ptr_word = body->word + dwords + ptr_index;
  if (ptr_word >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
  store_w(body->b, body->seg, ptr_word, capnp_wp_make_cap(cap_index));
  return CAPNP_OK;
}

int capnp_builder_set_text(const capnp_bptr_t *body, uint16_t dwords,
                           uint16_t ptr_index, const char *text,
                           size_t text_len) {
  size_t ptr_word;
  if (!body || !body->b || !text)
    return CAPNP_ERR_ARG;
  ptr_word = body->word + dwords + ptr_index;
  if (ptr_word >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
  return write_text_at(body->b, body->seg, ptr_word, text, text_len);
}

int capnp_builder_set_data(const capnp_bptr_t *body, uint16_t dwords,
                           uint16_t ptr_index, const uint8_t *data,
                           size_t data_len) {
  size_t nwords, ptr_word;
  uint32_t start_seg;
  size_t start_word;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  if (data_len && !data)
    return CAPNP_ERR_ARG;
  ptr_word = body->word + dwords + ptr_index;
  if (ptr_word >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
  nwords = (data_len + 7) / 8;
  if (nwords) {
    if (alloc_words(body->b, nwords, &start_seg, &start_word))
      return CAPNP_ERR_ALLOC;
    memset(wbytes(body->b, start_seg, start_word), 0,
           nwords * CAPNP_WORD_BYTES);
    if (data_len)
      memcpy(wbytes(body->b, start_seg, start_word), data, data_len);
  } else {
    start_seg = body->seg;
    start_word = ptr_word + 1;
  }
  return write_list_ptr(body->b, body->seg, ptr_word, start_seg, start_word,
                        CAPNP_SZ_BYTE, (uint32_t)data_len);
}

static int set_list_prim(const capnp_bptr_t *body, uint16_t dwords,
                         uint16_t ptr_index, int esize, uint32_t nitems,
                         size_t elem_bytes, const void *items) {
  size_t ptr_word, nbytes, nwords;
  uint32_t start_seg;
  size_t start_word;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  ptr_word = body->word + dwords + ptr_index;
  if (ptr_word >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
  nbytes = (size_t)nitems * elem_bytes;
  nwords = (nbytes + 7) / 8;
  if (nwords) {
    if (alloc_words(body->b, nwords, &start_seg, &start_word))
      return CAPNP_ERR_ALLOC;
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
  } else {
    start_seg = body->seg;
    start_word = ptr_word + 1;
  }
  return write_list_ptr(body->b, body->seg, ptr_word, start_seg, start_word,
                        esize, nitems);
}

int capnp_builder_set_list_u8(const capnp_bptr_t *body, uint16_t dwords,
                              uint16_t ptr_index, const uint8_t *items,
                              uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_BYTE, nitems, 1, items);
}
int capnp_builder_set_list_u16(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint16_t *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_TWO, nitems, 2, items);
}
int capnp_builder_set_list_u32(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint32_t *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_FOUR, nitems, 4, items);
}
int capnp_builder_set_list_u64(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const uint64_t *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_EIGHT, nitems, 8, items);
}
int capnp_builder_set_list_f32(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const float *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_FOUR, nitems, 4, items);
}
int capnp_builder_set_list_f64(const capnp_bptr_t *body, uint16_t dwords,
                               uint16_t ptr_index, const double *items,
                               uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_EIGHT, nitems, 8, items);
}

int capnp_builder_set_list_bool(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, const uint8_t *items,
                                uint32_t nitems) {
  size_t ptr_word, nbytes, nwords;
  uint32_t start_seg, i;
  size_t start_word;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  ptr_word = body->word + dwords + ptr_index;
  if (ptr_word >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
  nbytes = ((size_t)nitems + 7u) / 8u;
  nwords = (nbytes + 7u) / 8u;
  if (nwords) {
    if (alloc_words(body->b, nwords, &start_seg, &start_word))
      return CAPNP_ERR_ALLOC;
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
  } else {
    start_seg = body->seg;
    start_word = ptr_word + 1;
  }
  return write_list_ptr(body->b, body->seg, ptr_word, start_seg, start_word,
                        CAPNP_SZ_BIT, nitems);
}

int capnp_builder_set_list_void(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, uint32_t nitems) {
  return set_list_prim(body, dwords, ptr_index, CAPNP_SZ_VOID, nitems, 0, NULL);
}

int capnp_builder_set_list_text(const capnp_bptr_t *body, uint16_t dwords,
                                uint16_t ptr_index, const char *const *items,
                                uint32_t nitems) {
  size_t list_ptr_word;
  uint32_t list_seg, i;
  size_t list_start;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  list_ptr_word = body->word + dwords + ptr_index;
  if (list_ptr_word >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
  if (nitems) {
    if (alloc_words(body->b, nitems, &list_seg, &list_start))
      return CAPNP_ERR_ALLOC;
  } else {
    list_seg = body->seg;
    list_start = list_ptr_word + 1;
  }
  if (write_list_ptr(body->b, body->seg, list_ptr_word, list_seg, list_start,
                     CAPNP_SZ_PTR, nitems))
    return CAPNP_ERR_ALLOC;
  for (i = 0; i < nitems; i++) {
    const char *t = items[i] ? items[i] : "";
    if (write_text_at(body->b, list_seg, list_start + i, t, strlen(t)))
      return CAPNP_ERR_ALLOC;
  }
  return CAPNP_OK;
}

size_t capnp_builder_ptr_word(size_t body_word, uint16_t dwords,
                              uint16_t ptr_index) {
  return body_word + dwords + ptr_index;
}

int capnp_builder_set_list_struct(const capnp_bptr_t *body, uint16_t dwords,
                                  uint16_t ptr_index, uint32_t nitems,
                                  uint16_t elem_dwords, uint16_t elem_pwords,
                                  capnp_bptr_t *first_elem) {
  size_t list_ptr_word, step, content_words, need;
  uint32_t tag_seg;
  size_t tag_word;
  if (!body || !body->b)
    return CAPNP_ERR_ARG;
  list_ptr_word = body->word + dwords + ptr_index;
  if (list_ptr_word >= body->b->segs[body->seg].words)
    return CAPNP_ERR_BOUNDS;
  step = (size_t)elem_dwords + elem_pwords;
  content_words = (size_t)nitems * step;
  need = 1 + content_words;
  if (alloc_words(body->b, need, &tag_seg, &tag_word))
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
  if (!slot || !slot->b)
    return CAPNP_ERR_ARG;
  return capnp_copy_ptr_to_word(slot->b, slot->seg, slot->word, src, 0);
}

int capnp_builder_serialize(const capnp_builder_t *b, uint8_t **out,
                            size_t *out_len) {
  size_t table_bytes, body_words = 0, total, off;
  uint32_t i;
  uint8_t *buf;
  if (!b || !out || !out_len || b->nsegs == 0 || b->segs[0].words == 0)
    return CAPNP_ERR_ARG;
  for (i = 0; i < b->nsegs; i++)
    body_words += b->segs[i].words;
  table_bytes = 4u + 4u * (size_t)b->nsegs;
  if (table_bytes % 8u != 0)
    table_bytes += 4u;
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
