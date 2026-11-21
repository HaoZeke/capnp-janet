#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdlib.h>
#include <string.h>

#define CAPNP_WIRE_MAX_SEGMENT_WORDS ((size_t)((1u << 29) - 1u))
#define CAPNP_MSG_READY 0xC4A7019E11D0ULL

/* m must already be zeroed or initialized. ready is only meaningful
 * then: the magic value owns segs and owned, and any other value does
 * not. */
static void release_if_live(capnp_message_t *m) {
  if (!m || m->ready != CAPNP_MSG_READY)
    return;
  free(m->owned);
  free(m->segs);
  m->owned = NULL;
  m->owned_len = 0;
  m->segs = NULL;
  m->nsegs = 0;
  m->ready = 0;
}

void capnp_message_init(capnp_message_t *m) {
  memset(m, 0, sizeof(*m));
  m->traversal_left = CAPNP_DEFAULT_TRAVERSAL_WORDS;
  m->depth_limit = CAPNP_DEFAULT_DEPTH_LIMIT;
  m->ready = CAPNP_MSG_READY;
}

void capnp_message_free(capnp_message_t *m) {
  if (!m || m->ready != CAPNP_MSG_READY)
    return;
  free(m->owned);
  free(m->segs);
  memset(m, 0, sizeof(*m));
}

static int attach_segments(capnp_message_t *m, const capnp_segment_t *segs,
                           uint32_t nsegs) {
  capnp_segment_t *copy;
  uint32_t i;
  if (nsegs == 0 || nsegs > CAPNP_MAX_SEGMENTS)
    return CAPNP_ERR_FRAMING;
  for (i = 0; i < nsegs; i++) {
    if (!segs || !segs[i].data || (segs[i].words == 0 && i == 0) ||
        segs[i].words > CAPNP_WIRE_MAX_SEGMENT_WORDS)
      return CAPNP_ERR_FRAMING;
  }
  copy = (capnp_segment_t *)malloc((size_t)nsegs * sizeof(*copy));
  if (!copy)
    return CAPNP_ERR_ALLOC;
  memcpy(copy, segs, (size_t)nsegs * sizeof(*copy));
  free(m->segs);
  m->segs = copy;
  m->nsegs = nsegs;
  return CAPNP_OK;
}

int capnp_message_from_segments(capnp_message_t *m, const capnp_segment_t *segs,
                               uint32_t nsegs) {
  release_if_live(m);
  capnp_message_init(m);
  return attach_segments(m, segs, nsegs);
}

/*
 * Stream framing:
 *   u32 segmentCountMinusOne
 *   u32 sizes[segmentCount]   (in words)
 *   pad to 8-byte boundary
 *   segment0 bytes ...
 */
static int parse_flat(capnp_message_t *m, const uint8_t *data, size_t len,
                      int copy) {
  uint32_t nsegs, i;
  size_t table_bytes, total_words, body, off;
  uint32_t *sizes;
  const uint8_t *base;
  capnp_segment_t *table;

  if (len < 8)
    return CAPNP_ERR_FRAMING;
  nsegs = capnp_load_le32(data) + 1u;
  if (nsegs == 0 || nsegs > CAPNP_MAX_SEGMENTS)
    return CAPNP_ERR_FRAMING;
  /* A 32-bit size_t cannot hold a segment table past SIZE_MAX/4.
   * On a wider size_t the comparison is out of range for nsegs. */
#if UINT32_MAX > SIZE_MAX / 4u
  if ((size_t)nsegs > (SIZE_MAX / 4u) - 1u)
    return CAPNP_ERR_FRAMING;
#endif
  table_bytes = 4u + 4u * (size_t)nsegs;
  if (table_bytes % 8u != 0)
    table_bytes += 4u; /* pad */
  if (len < table_bytes)
    return CAPNP_ERR_FRAMING;

  sizes = (uint32_t *)malloc((size_t)nsegs * sizeof(uint32_t));
  if (!sizes)
    return CAPNP_ERR_ALLOC;
  total_words = 0;
  for (i = 0; i < nsegs; i++) {
    sizes[i] = capnp_load_le32(data + 4 + 4 * i);
    if (sizes[i] > CAPNP_WIRE_MAX_SEGMENT_WORDS) {
      free(sizes);
      return CAPNP_ERR_FRAMING;
    }
    if ((size_t)sizes[i] > SIZE_MAX - total_words) {
      free(sizes);
      return CAPNP_ERR_FRAMING;
    }
    total_words += sizes[i];
  }
  if (total_words > (SIZE_MAX - table_bytes) / CAPNP_WORD_BYTES) {
    free(sizes);
    return CAPNP_ERR_FRAMING;
  }
  body = table_bytes + total_words * CAPNP_WORD_BYTES;
  if (body > len) {
    free(sizes);
    return CAPNP_ERR_FRAMING;
  }

  if (copy) {
    m->owned = (uint8_t *)malloc(body);
    if (!m->owned) {
      free(sizes);
      return CAPNP_ERR_ALLOC;
    }
    memcpy(m->owned, data, body);
    m->owned_len = body;
    base = m->owned;
  } else {
    base = data;
  }

  table = (capnp_segment_t *)malloc((size_t)nsegs * sizeof(*table));
  if (!table) {
    free(m->owned);
    m->owned = NULL;
    m->owned_len = 0;
    free(sizes);
    return CAPNP_ERR_ALLOC;
  }
  off = table_bytes;
  for (i = 0; i < nsegs; i++) {
    table[i].data = base + off;
    table[i].words = sizes[i];
    off += (size_t)sizes[i] * CAPNP_WORD_BYTES;
  }
  free(sizes);
  free(m->segs);
  m->segs = table;
  m->nsegs = nsegs;
  return CAPNP_OK;
}

int capnp_message_from_flat(capnp_message_t *m, const uint8_t *data,
                            size_t len) {
  release_if_live(m);
  capnp_message_init(m);
  return parse_flat(m, data, len, 1);
}

int capnp_message_view_flat(capnp_message_t *m, const uint8_t *data,
                            size_t len) {
  release_if_live(m);
  capnp_message_init(m);
  return parse_flat(m, data, len, 0);
}

static int bounds_word(const capnp_message_t *m, uint32_t seg, size_t word) {
  if (seg >= m->nsegs)
    return CAPNP_ERR_SEGMENT;
  if (word >= m->segs[seg].words)
    return CAPNP_ERR_BOUNDS;
  return CAPNP_OK;
}

static uint64_t read_word(const capnp_message_t *m, uint32_t seg, size_t word) {
  return capnp_load_le64(m->segs[seg].data + word * CAPNP_WORD_BYTES);
}

static int charge(capnp_message_t *m, uint64_t words) {
  if (words > m->traversal_left)
    return CAPNP_ERR_TRAVERSAL;
  m->traversal_left -= words;
  return CAPNP_OK;
}

static int resolve_ptr(capnp_message_t *m, uint32_t seg, size_t word,
                       int depth, capnp_ptr_t *out);

/* `depth` is the number of further pointer follows allowed from this object. */
static void fill_obj(capnp_ptr_t *out, capnp_message_t *m, uint32_t seg,
                     size_t word, int kind, int depth) {
  out->msg = m;
  out->seg = seg;
  out->word = word;
  out->kind = kind;
  out->dwords = 0;
  out->pwords = 0;
  out->esize = 0;
  out->count = 0;
  out->step_words = 0;
  out->body_byte = 0;
  out->data_bits = 0;
  out->depth_left = depth;
}

static int ptr_target(size_t word, int32_t off, size_t *target) {
  int64_t base;
  if (word > (size_t)INT64_MAX)
    return CAPNP_ERR_BOUNDS;
  base = (int64_t)word + 1;
  if (off > 0 && base > INT64_MAX - (int64_t)off)
    return CAPNP_ERR_BOUNDS;
  base += (int64_t)off;
  if (base < 0)
    return CAPNP_ERR_BOUNDS;
  *target = (size_t)base;
  return CAPNP_OK;
}

/* Word count of a non-composite list body. Composite lists carry a tag
 * and are never a zero-word object. */
static int list_body_words(int esize, uint32_t count, size_t *out) {
  size_t n;
  switch (esize) {
  case CAPNP_SZ_VOID:
    n = 0;
    break;
  case CAPNP_SZ_BIT:
    n = ((size_t)count + 63u) / 64u;
    break;
  case CAPNP_SZ_BYTE:
    n = ((size_t)count + 7u) / 8u;
    break;
  case CAPNP_SZ_TWO:
    n = ((size_t)count + 3u) / 4u;
    break;
  case CAPNP_SZ_FOUR:
    n = ((size_t)count + 1u) / 2u;
    break;
  case CAPNP_SZ_EIGHT:
  case CAPNP_SZ_PTR:
    n = (size_t)count;
    break;
  default:
    return -1;
  }
  *out = n;
  return 0;
}

static int list_zero_words(int esize, uint32_t count) {
  size_t n = 0;
  if (esize == CAPNP_SZ_COMPOSITE)
    return 0;
  if (list_body_words(esize, count, &n))
    return 0;
  return n == 0;
}

static int land_struct(capnp_message_t *m, uint32_t seg, size_t body,
                       uint16_t dwords, uint16_t pwords, int depth,
                       capnp_ptr_t *out) {
  size_t nwords = (size_t)dwords + (size_t)pwords;
  /* `depth` is what the struct may still follow: C++ readStructPointer
   * needs the reader's nestingLimit above zero. */
  if (depth < 0)
    return CAPNP_ERR_DEPTH;
  if (seg >= m->nsegs)
    return CAPNP_ERR_SEGMENT;
  /* checkOffset clamps an out-of-range target to the segment end.
   * checkObject then accepts a zero-word object and rejects any other. */
  if (body > m->segs[seg].words) {
    if (nwords != 0)
      return CAPNP_ERR_BOUNDS;
    body = m->segs[seg].words;
  }
  fill_obj(out, m, seg, body, CAPNP_PK_STRUCT, depth);
  out->dwords = dwords;
  out->pwords = pwords;
  if (nwords == 0)
    return charge(m, 0);
  if (body > SIZE_MAX - nwords)
    return CAPNP_ERR_BOUNDS;
  if (bounds_word(m, seg, body) || bounds_word(m, seg, body + nwords - 1))
    return CAPNP_ERR_BOUNDS;
  return charge(m, nwords);
}

static int land_list(capnp_message_t *m, uint32_t seg, size_t start, int esize,
                     uint32_t count, int depth, capnp_ptr_t *out) {
  if (depth < 0)
    return CAPNP_ERR_DEPTH;
  if (seg >= m->nsegs)
    return CAPNP_ERR_SEGMENT;
  /* Same clamp as SegmentReader::checkOffset. A zero-word list whose
   * target is past the segment end is an empty list at that end. */
  if (start > m->segs[seg].words) {
    if (!list_zero_words(esize, count))
      return CAPNP_ERR_BOUNDS;
    start = m->segs[seg].words;
  }
  fill_obj(out, m, seg, start, CAPNP_PK_LIST, depth);
  out->esize = esize;
  out->count = count;
  if (esize == CAPNP_SZ_COMPOSITE) {
    uint64_t tag, need;
    uint32_t elems;
    uint16_t dw, pw;
    uint32_t step;
    if (bounds_word(m, seg, start))
      return CAPNP_ERR_BOUNDS;
    if (charge(m, 1))
      return CAPNP_ERR_TRAVERSAL;
    tag = read_word(m, seg, start);
    if (capnp_wp_kind(tag) != CAPNP_WK_STRUCT)
      return CAPNP_ERR_KIND;
    elems = ((uint32_t)tag >> 2) & 0x1fffffffu;
    dw = capnp_wp_struct_dwords(tag);
    pw = capnp_wp_struct_pwords(tag);
    step = (uint32_t)dw + (uint32_t)pw;
    if ((uint64_t)elems * (uint64_t)step > (uint64_t)count)
      return CAPNP_ERR_BOUNDS;
    out->count = elems;
    out->dwords = dw;
    out->pwords = pw;
    out->step_words = step;
    out->word = start + 1;
    need = count;
    if (need) {
      if (out->word > SIZE_MAX - (size_t)(need - 1))
        return CAPNP_ERR_BOUNDS;
      if (bounds_word(m, seg, out->word + (size_t)need - 1))
        return CAPNP_ERR_BOUNDS;
    }
    if (charge(m, need))
      return CAPNP_ERR_TRAVERSAL;
    return step == 0 ? charge(m, (uint64_t)elems) : CAPNP_OK;
  }

  {
    uint64_t bits = 0, words;
    switch (esize) {
    case CAPNP_SZ_VOID:
      bits = 0;
      break;
    case CAPNP_SZ_BIT:
      bits = (uint64_t)count;
      break;
    case CAPNP_SZ_BYTE:
      bits = (uint64_t)count * 8u;
      break;
    case CAPNP_SZ_TWO:
      bits = (uint64_t)count * 16u;
      break;
    case CAPNP_SZ_FOUR:
      bits = (uint64_t)count * 32u;
      break;
    case CAPNP_SZ_EIGHT:
    case CAPNP_SZ_PTR:
      bits = (uint64_t)count * 64u;
      break;
    default:
      return CAPNP_ERR_KIND;
    }
    words = (bits + 63u) / 64u;
    if (words) {
      if (start > SIZE_MAX - (size_t)(words - 1))
        return CAPNP_ERR_BOUNDS;
      if (bounds_word(m, seg, start + (size_t)words - 1))
        return CAPNP_ERR_BOUNDS;
    }
    if (esize == CAPNP_SZ_VOID)
      return charge(m, count);
    return charge(m, words);
  }
}

/* A negative or overflowing offset is out of range. checkOffset turns
 * that into the segment end, and a zero-word object still lands. */
static int land_struct_off(capnp_message_t *m, uint32_t seg, size_t ptr_word,
                           int32_t off, uint16_t dw, uint16_t pw, int depth,
                           capnp_ptr_t *out) {
  size_t body = 0;
  int rc = ptr_target(ptr_word, off, &body);
  if (rc) {
    if (dw == 0 && pw == 0 && seg < m->nsegs)
      return land_struct(m, seg, m->segs[seg].words, 0, 0, depth, out);
    return rc;
  }
  return land_struct(m, seg, body, dw, pw, depth, out);
}

static int land_list_off(capnp_message_t *m, uint32_t seg, size_t ptr_word,
                         int32_t off, int esize, uint32_t count, int depth,
                         capnp_ptr_t *out) {
  size_t start = 0;
  int rc = ptr_target(ptr_word, off, &start);
  if (rc) {
    if (list_zero_words(esize, count) && seg < m->nsegs)
      return land_list(m, seg, m->segs[seg].words, esize, count, depth, out);
    return rc;
  }
  return land_list(m, seg, start, esize, count, depth, out);
}

static int resolve_word(capnp_message_t *m, uint32_t seg, size_t word,
                        uint64_t w, int depth, capnp_ptr_t *out) {
  int kind;
  if (w == 0) {
    fill_obj(out, m, seg, word, CAPNP_PK_NULL, depth);
    return CAPNP_OK;
  }

  kind = capnp_wp_kind(w);
  if (kind == CAPNP_WK_FAR) {
    uint32_t tseg = capnp_wp_far_seg(w);
    uint32_t toff = capnp_wp_far_off(w);
    if (capnp_wp_far_two(w)) {
      uint64_t pad, tag;
      uint32_t cseg, coff;
      /* Landing pad is two words. The object's body is the far target,
       * which has to fall inside that segment. */
      if (toff > UINT32_MAX - 1u)
        return CAPNP_ERR_BOUNDS;
      if (bounds_word(m, tseg, toff) || bounds_word(m, tseg, (size_t)toff + 1u))
        return CAPNP_ERR_BOUNDS;
      if (charge(m, 2))
        return CAPNP_ERR_TRAVERSAL;
      pad = read_word(m, tseg, toff);
      tag = read_word(m, tseg, (size_t)toff + 1u);
      /* C++ ignores the pad's own double-far bit and the tag's offset. */
      if (capnp_wp_kind(pad) != CAPNP_WK_FAR)
        return CAPNP_ERR_KIND;
      cseg = capnp_wp_far_seg(pad);
      coff = capnp_wp_far_off(pad);
      if (capnp_wp_kind(tag) == CAPNP_WK_STRUCT)
        return land_struct(m, cseg, coff, capnp_wp_struct_dwords(tag),
                           capnp_wp_struct_pwords(tag), depth, out);
      if (capnp_wp_kind(tag) == CAPNP_WK_LIST)
        return land_list(m, cseg, coff, capnp_wp_list_esize(tag),
                         capnp_wp_list_count(tag), depth, out);
      return CAPNP_ERR_KIND;
    }
    if (bounds_word(m, tseg, toff))
      return CAPNP_ERR_BOUNDS;
    if (charge(m, 1))
      return CAPNP_ERR_TRAVERSAL;
    w = read_word(m, tseg, toff);
    kind = capnp_wp_kind(w);
    if (kind == CAPNP_WK_FAR)
      return CAPNP_ERR_KIND;
    if (kind == CAPNP_WK_STRUCT)
      return land_struct_off(m, tseg, toff, capnp_wp_offset(w),
                             capnp_wp_struct_dwords(w), capnp_wp_struct_pwords(w),
                             depth, out);
    if (kind == CAPNP_WK_LIST)
      return land_list_off(m, tseg, toff, capnp_wp_offset(w),
                           capnp_wp_list_esize(w), capnp_wp_list_count(w), depth,
                           out);
    if ((uint32_t)w == CAPNP_WK_CAP) {
      fill_obj(out, m, tseg, toff, CAPNP_PK_CAP, depth);
      out->count = (uint32_t)(w >> 32);
      return CAPNP_OK;
    }
    return CAPNP_ERR_KIND;
  }

  if (kind == CAPNP_WK_STRUCT)
    return land_struct_off(m, seg, word, capnp_wp_offset(w),
                           capnp_wp_struct_dwords(w), capnp_wp_struct_pwords(w),
                           depth, out);

  if (kind == CAPNP_WK_LIST)
    return land_list_off(m, seg, word, capnp_wp_offset(w), capnp_wp_list_esize(w),
                         capnp_wp_list_count(w), depth, out);

  if (kind == CAPNP_WK_CAP) {
    if ((uint32_t)w != CAPNP_WK_CAP)
      return CAPNP_ERR_KIND;
    fill_obj(out, m, seg, word, CAPNP_PK_CAP, depth);
    out->count = (uint32_t)((w >> 32) & 0xffffffffull);
    return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

static int resolve_ptr(capnp_message_t *m, uint32_t seg, size_t word, int depth,
                       capnp_ptr_t *out) {
  uint64_t w;
  if (bounds_word(m, seg, word))
    return CAPNP_ERR_BOUNDS;
  if (charge(m, 1))
    return CAPNP_ERR_TRAVERSAL;
  w = read_word(m, seg, word);
  return resolve_word(m, seg, word, w, depth - 1, out);
}

int capnp_root(capnp_message_t *m, capnp_ptr_t *out) {
  if (!m || !out || m->nsegs == 0)
    return CAPNP_ERR_ARG;
  return resolve_ptr(m, 0, 0, m->depth_limit, out);
}

int capnp_getp(const capnp_ptr_t *s, uint16_t ptr_index, capnp_ptr_t *out) {
  if (!s || !out || s->kind != CAPNP_PK_STRUCT)
    return CAPNP_ERR_KIND;
  if (ptr_index >= s->pwords) {
    fill_obj(out, s->msg, s->seg, 0, CAPNP_PK_NULL, s->depth_left);
    return CAPNP_OK;
  }
  size_t word = s->word + s->dwords + ptr_index;
  return resolve_ptr(s->msg, s->seg, word, s->depth_left, out);
}

/* Data section size in bits. Upgrade views set data_bits to the element
 * width so oversize field reads return defaults instead of neighbour bytes. */
static uint32_t data_bit_count(const capnp_ptr_t *s) {
  if (s->data_bits != 0)
    return s->data_bits;
  return (uint32_t)s->dwords * 64u;
}

static const uint8_t *data_bytes(const capnp_ptr_t *s) {
  return s->msg->segs[s->seg].data + s->word * CAPNP_WORD_BYTES + s->body_byte;
}

static int data_has_bytes(const capnp_ptr_t *s, uint32_t byte_offset,
                          uint32_t width) {
  size_t logical, abs, seg_bytes;
  if (!s || !s->msg || s->kind != CAPNP_PK_STRUCT || s->seg >= s->msg->nsegs)
    return 0;
  if (width == 0)
    return 0;
  logical = (size_t)data_bit_count(s) / 8u;
  if ((size_t)byte_offset > logical || (size_t)width > logical - byte_offset)
    return 0;
  if (s->word > SIZE_MAX / CAPNP_WORD_BYTES)
    return 0;
  abs = s->word * CAPNP_WORD_BYTES + (size_t)s->body_byte;
  if (abs > SIZE_MAX - byte_offset)
    return 0;
  abs += byte_offset;
  if (abs > SIZE_MAX - width)
    return 0;
  seg_bytes = s->msg->segs[s->seg].words * CAPNP_WORD_BYTES;
  return abs + width <= seg_bytes;
}

uint8_t capnp_get_u8(const capnp_ptr_t *s, uint32_t byte_offset, uint8_t dflt) {
  if (!data_has_bytes(s, byte_offset, 1))
    return dflt;
  return data_bytes(s)[byte_offset];
}

uint16_t capnp_get_u16(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint16_t dflt) {
  if (!data_has_bytes(s, byte_offset, 2))
    return dflt;
  const uint8_t *p = data_bytes(s) + byte_offset;
  return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t capnp_get_u32(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint32_t dflt) {
  if (!data_has_bytes(s, byte_offset, 4))
    return dflt;
  return capnp_load_le32(data_bytes(s) + byte_offset);
}

uint64_t capnp_get_u64(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint64_t dflt) {
  if (!data_has_bytes(s, byte_offset, 8))
    return dflt;
  return capnp_load_le64(data_bytes(s) + byte_offset);
}

float capnp_get_f32(const capnp_ptr_t *s, uint32_t byte_offset, float dflt) {
  uint32_t bits;
  float value;
  if (!data_has_bytes(s, byte_offset, 4))
    return dflt;
  bits = capnp_load_le32(data_bytes(s) + byte_offset);
  memcpy(&value, &bits, sizeof(value));
  return value;
}

double capnp_get_f64(const capnp_ptr_t *s, uint32_t byte_offset, double dflt) {
  if (!data_has_bytes(s, byte_offset, 8))
    return dflt;
  uint64_t bits = capnp_load_le64(data_bytes(s) + byte_offset);
  double v;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

int capnp_get_bool(const capnp_ptr_t *s, uint32_t bit_offset, int dflt) {
  uint32_t byte_offset;
  uint8_t bit;
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return dflt;
  if (bit_offset >= data_bit_count(s))
    return dflt;
  byte_offset = bit_offset / 8u;
  if (!data_has_bytes(s, byte_offset, 1))
    return dflt;
  bit = (uint8_t)(1u << (bit_offset % 8u));
  return (data_bytes(s)[byte_offset] & bit) != 0;
}

/* Text is a byte list whose last element is NUL. A null pointer is the
 * empty default; a list that does not end in NUL is not text. */
static int text_from_bytes(const capnp_ptr_t *list, const char **out,
                           size_t *len) {
  const char *p;
  size_t n;
  if (!list || list->kind != CAPNP_PK_LIST || list->esize != CAPNP_SZ_BYTE)
    return CAPNP_ERR_KIND;
  if (list->count == 0)
    return CAPNP_ERR_KIND;
  p = (const char *)(list->msg->segs[list->seg].data +
                     list->word * CAPNP_WORD_BYTES);
  n = list->count;
  if (p[n - 1] != '\0')
    return CAPNP_ERR_KIND;
  if (out)
    *out = p;
  if (len)
    *len = n - 1;
  return CAPNP_OK;
}

/* C++ reads Text and Data with no nesting check, so a blob stays readable
 * from a struct that may follow no further pointers. */
static int getp_blob(const capnp_ptr_t *s, uint16_t ptr_index,
                     capnp_ptr_t *out) {
  capnp_ptr_t holder;
  if (!s)
    return CAPNP_ERR_KIND;
  holder = *s;
  if (holder.depth_left < 1)
    holder.depth_left = 1;
  return capnp_getp(&holder, ptr_index, out);
}

int capnp_get_text(const capnp_ptr_t *s, uint16_t ptr_index, const char **out,
                   size_t *len) {
  capnp_ptr_t list;
  int rc = getp_blob(s, ptr_index, &list);
  if (rc)
    return rc;
  if (list.kind == CAPNP_PK_NULL) {
    if (out)
      *out = "";
    if (len)
      *len = 0;
    return CAPNP_OK;
  }
  return text_from_bytes(&list, out, len);
}

uint32_t capnp_list_len(const capnp_ptr_t *list) {
  if (!list || list->kind != CAPNP_PK_LIST)
    return 0;
  return list->count;
}

int capnp_list_getp(const capnp_ptr_t *list, uint32_t index, capnp_ptr_t *out) {
  if (!list || !out || list->kind != CAPNP_PK_LIST)
    return CAPNP_ERR_KIND;
  if (index >= list->count)
    return CAPNP_ERR_BOUNDS;
  if (list->esize == CAPNP_SZ_PTR) {
    size_t word;
    if (index > SIZE_MAX - list->word)
      return CAPNP_ERR_BOUNDS;
    word = list->word + index;
    return resolve_ptr(list->msg, list->seg, word, list->depth_left, out);
  }
  if (list->esize == CAPNP_SZ_COMPOSITE) {
    size_t delta;
    if (list->step_words != 0 &&
        (size_t)index > SIZE_MAX / list->step_words)
      return CAPNP_ERR_BOUNDS;
    delta = (size_t)index * list->step_words;
    if (list->word > SIZE_MAX - delta)
      return CAPNP_ERR_BOUNDS;
    fill_obj(out, list->msg, list->seg, list->word + delta, CAPNP_PK_STRUCT,
             list->depth_left);
    out->dwords = list->dwords;
    out->pwords = list->pwords;
    return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

/*
 * List(Text) per https://capnproto.org/encoding.html:
 *   - Written as a pointer list (element size C=6); each element is a pointer
 *     to Text (List(UInt8) with trailing NUL counted on the wire).
 *   - A list of pointer values may also be *decoded* as a composite list of
 *     one-pointer, zero-data structs (schema evolution upgrade). Accept both
 *     when reading (same rule as Cap'n C++ / capnp-fortran).
 */
int capnp_list_get_text(const capnp_ptr_t *list, uint32_t index,
                        const char **out, size_t *len) {
  capnp_ptr_t elem;
  int rc;

  if (!list || list->kind != CAPNP_PK_LIST)
    return CAPNP_ERR_KIND;

  /* C=6 pointer list: resolve element pointer to the Text blob. */
  if (list->esize == CAPNP_SZ_PTR) {
    capnp_ptr_t holder = *list;
    if (holder.depth_left < 1)
      holder.depth_left = 1;
    rc = capnp_list_getp(&holder, index, &elem);
    if (rc)
      return rc;
    if (elem.kind == CAPNP_PK_NULL) {
      if (out)
        *out = "";
      if (len)
        *len = 0;
      return CAPNP_OK;
    }
    if (elem.kind == CAPNP_PK_LIST && elem.esize == CAPNP_SZ_BYTE)
      return text_from_bytes(&elem, out, len);
    return CAPNP_ERR_KIND;
  }

  /* C=7 composite upgrade view: element is a struct; Text at pointer 0. */
  if (list->esize == CAPNP_SZ_COMPOSITE) {
    rc = capnp_list_getp(list, index, &elem);
    if (rc)
      return rc;
    if (elem.kind != CAPNP_PK_STRUCT)
      return CAPNP_ERR_KIND;
    if (elem.pwords == 0)
      return CAPNP_ERR_KIND;
    return capnp_get_text(&elem, 0, out, len);
  }

  return CAPNP_ERR_KIND;
}

int capnp_get_data(const capnp_ptr_t *s, uint16_t ptr_index, const uint8_t **out,
                   size_t *len) {
  capnp_ptr_t list;
  int rc = getp_blob(s, ptr_index, &list);
  if (rc)
    return rc;
  if (list.kind == CAPNP_PK_NULL) {
    if (out)
      *out = NULL;
    if (len)
      *len = 0;
    return CAPNP_OK;
  }
  if (list.kind != CAPNP_PK_LIST || list.esize != CAPNP_SZ_BYTE)
    return CAPNP_ERR_KIND;
  if (out)
    *out = list.msg->segs[list.seg].data + list.word * CAPNP_WORD_BYTES;
  if (len)
    *len = list.count;
  return CAPNP_OK;
}

static const uint8_t *list_elem_bytes(const capnp_ptr_t *list, uint32_t index,
                                      size_t elem_bytes) {
  return list->msg->segs[list->seg].data + list->word * CAPNP_WORD_BYTES +
         (size_t)index * elem_bytes;
}

/* Composite list element base (field @0 / first data word). */
static const uint8_t *composite_elem_data(const capnp_ptr_t *list,
                                          uint32_t index) {
  return list->msg->segs[list->seg].data +
         (list->word + (size_t)index * list->step_words) * CAPNP_WORD_BYTES;
}

/* Element `index` of a data list whose elements are at least `want`
 * bytes wide, or NULL. A wider element is read from its low bytes, as
 * C++ `readListPointer` allows. */
static const uint8_t *data_elem(const capnp_ptr_t *list, uint32_t index,
                                size_t want) {
  static const size_t width[] = {0, 0, 1, 2, 4, 8};
  if (!list || list->kind != CAPNP_PK_LIST || index >= list->count)
    return NULL;
  if (list->esize == CAPNP_SZ_COMPOSITE)
    return list->dwords >= 1 ? composite_elem_data(list, index) : NULL;
  if (list->esize < CAPNP_SZ_BYTE || list->esize > CAPNP_SZ_EIGHT ||
      width[list->esize] < want)
    return NULL;
  return list_elem_bytes(list, index, width[list->esize]);
}

uint8_t capnp_list_get_u8(const capnp_ptr_t *list, uint32_t index,
                          uint8_t dflt) {
  const uint8_t *p = data_elem(list, index, 1);
  return p ? *p : dflt;
}

uint16_t capnp_list_get_u16(const capnp_ptr_t *list, uint32_t index,
                            uint16_t dflt) {
  const uint8_t *p = data_elem(list, index, 2);
  return p ? (uint16_t)(p[0] | (p[1] << 8)) : dflt;
}

uint32_t capnp_list_get_u32(const capnp_ptr_t *list, uint32_t index,
                            uint32_t dflt) {
  const uint8_t *p = data_elem(list, index, 4);
  return p ? capnp_load_le32(p) : dflt;
}

uint64_t capnp_list_get_u64(const capnp_ptr_t *list, uint32_t index,
                            uint64_t dflt) {
  const uint8_t *p = data_elem(list, index, 8);
  return p ? capnp_load_le64(p) : dflt;
}

float capnp_list_get_f32(const capnp_ptr_t *list, uint32_t index, float dflt) {
  const uint8_t *p = data_elem(list, index, 4);
  uint32_t bits;
  float value;
  if (!p)
    return dflt;
  bits = capnp_load_le32(p);
  memcpy(&value, &bits, sizeof(value));
  return value;
}

double capnp_list_get_f64(const capnp_ptr_t *list, uint32_t index, double dflt) {
  const uint8_t *p = data_elem(list, index, 8);
  uint64_t bits;
  double v;
  if (!p)
    return dflt;
  bits = capnp_load_le64(p);
  memcpy(&v, &bits, sizeof(v));
  return v;
}

/* C++ reads bit `index` from the start of any non-pointer data list,
 * whatever the element width. */
int capnp_list_get_bool(const capnp_ptr_t *list, uint32_t index, int dflt) {
  const uint8_t *base;
  uint8_t bit;
  if (!list || list->kind != CAPNP_PK_LIST || list->esize < CAPNP_SZ_BIT ||
      list->esize > CAPNP_SZ_EIGHT || index >= list->count)
    return dflt;
  base = list->msg->segs[list->seg].data + list->word * CAPNP_WORD_BYTES;
  bit = (uint8_t)(1u << (index % 8u));
  return (base[index / 8u] & bit) != 0;
}

int capnp_list_get_struct(const capnp_ptr_t *list, uint32_t index,
                          capnp_ptr_t *out) {
  size_t elem_bytes, abs_byte;

  if (!list || !out || list->kind != CAPNP_PK_LIST)
    return CAPNP_ERR_KIND;
  if (index >= list->count)
    return CAPNP_ERR_BOUNDS;

  switch (list->esize) {
  case CAPNP_SZ_COMPOSITE: {
    size_t delta;
    if (list->step_words != 0 &&
        (size_t)index > SIZE_MAX / list->step_words)
      return CAPNP_ERR_BOUNDS;
    delta = (size_t)index * list->step_words;
    if (list->word > SIZE_MAX - delta)
      return CAPNP_ERR_BOUNDS;
    fill_obj(out, list->msg, list->seg, list->word + delta, CAPNP_PK_STRUCT,
             list->depth_left);
    out->dwords = list->dwords;
    out->pwords = list->pwords;
    return CAPNP_OK;
  }

  case CAPNP_SZ_BYTE:
    elem_bytes = 1;
    break;
  case CAPNP_SZ_TWO:
    elem_bytes = 2;
    break;
  case CAPNP_SZ_FOUR:
    elem_bytes = 4;
    break;
  case CAPNP_SZ_EIGHT:
    elem_bytes = 8;
    break;

  case CAPNP_SZ_PTR:
    /* Upgrade: pointer list element is a 0-data / 1-pointer struct. */
    if (index > SIZE_MAX - list->word)
      return CAPNP_ERR_BOUNDS;
    fill_obj(out, list->msg, list->seg, list->word + index, CAPNP_PK_STRUCT,
             list->depth_left);
    out->pwords = 1;
    return CAPNP_OK;

  case CAPNP_SZ_VOID:
    fill_obj(out, list->msg, list->seg, list->word, CAPNP_PK_STRUCT,
             list->depth_left);
    return CAPNP_OK;

  default:
    /* BIT cannot upgrade (encoding.html list-upgrade rules). */
    return CAPNP_ERR_KIND;
  }

  /* Primitive data list -> synthetic struct with field @0 = the element. */
  if (list->word > SIZE_MAX / CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  if (index > SIZE_MAX / elem_bytes)
    return CAPNP_ERR_BOUNDS;
  if (list->word * CAPNP_WORD_BYTES >
      SIZE_MAX - (size_t)index * elem_bytes)
    return CAPNP_ERR_BOUNDS;
  abs_byte = list->word * CAPNP_WORD_BYTES + (size_t)index * elem_bytes;
  fill_obj(out, list->msg, list->seg, abs_byte / CAPNP_WORD_BYTES,
           CAPNP_PK_STRUCT, list->depth_left);
  out->body_byte = (uint8_t)(abs_byte % CAPNP_WORD_BYTES);
  out->dwords = 1;
  out->data_bits = (uint16_t)(elem_bytes * 8u);
  return CAPNP_OK;
}

int capnp_message_copy_flat(const capnp_message_t *m, uint8_t **out,
                            size_t *out_len) {
  size_t table_bytes, body_words, total, i, off;
  uint8_t *buf;

  if (!m || !out || !out_len || m->nsegs == 0)
    return CAPNP_ERR_ARG;
  body_words = 0;
  for (i = 0; i < m->nsegs; i++) {
    if (m->segs[i].words > CAPNP_WIRE_MAX_SEGMENT_WORDS ||
        m->segs[i].words > SIZE_MAX - body_words)
      return CAPNP_ERR_BOUNDS;
    body_words += m->segs[i].words;
  }
  table_bytes = 4u + 4u * (size_t)m->nsegs;
  if (table_bytes % 8u != 0)
    table_bytes += 4u;
  if (body_words > (SIZE_MAX - table_bytes) / CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  total = table_bytes + body_words * CAPNP_WORD_BYTES;
  buf = (uint8_t *)malloc(total);
  if (!buf)
    return CAPNP_ERR_ALLOC;
  capnp_store_le32(buf, m->nsegs - 1);
  for (i = 0; i < m->nsegs; i++)
    capnp_store_le32(buf + 4 + 4 * i, (uint32_t)m->segs[i].words);
  if (table_bytes > 4u + 4u * m->nsegs)
    memset(buf + 4 + 4 * m->nsegs, 0, table_bytes - (4 + 4 * m->nsegs));
  off = table_bytes;
  for (i = 0; i < m->nsegs; i++) {
    size_t nbytes = m->segs[i].words * CAPNP_WORD_BYTES;
    memcpy(buf + off, m->segs[i].data, nbytes);
    off += nbytes;
  }
  *out = buf;
  *out_len = total;
  return CAPNP_OK;
}
