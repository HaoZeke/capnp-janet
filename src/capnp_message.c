#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdlib.h>
#include <string.h>

void capnp_message_init(capnp_message_t *m) {
  memset(m, 0, sizeof(*m));
  m->traversal_left = CAPNP_DEFAULT_TRAVERSAL_WORDS;
  m->depth_limit = CAPNP_DEFAULT_DEPTH_LIMIT;
}

void capnp_message_free(capnp_message_t *m) {
  if (!m)
    return;
  free(m->owned);
  m->owned = NULL;
  m->owned_len = 0;
  m->nsegs = 0;
}

static int attach_segments(capnp_message_t *m, const capnp_segment_t *segs,
                           uint32_t nsegs) {
  if (nsegs == 0 || nsegs > CAPNP_MAX_SEGMENTS)
    return CAPNP_ERR_FRAMING;
  for (uint32_t i = 0; i < nsegs; i++) {
    if (!segs[i].data || (segs[i].words == 0 && i == 0))
      return CAPNP_ERR_FRAMING;
    m->segs[i] = segs[i];
  }
  m->nsegs = nsegs;
  return CAPNP_OK;
}

int capnp_message_from_segments(capnp_message_t *m, const capnp_segment_t *segs,
                               uint32_t nsegs) {
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
  if (len < 8)
    return CAPNP_ERR_FRAMING;
  uint32_t nsegs = capnp_load_le32(data) + 1u;
  if (nsegs == 0 || nsegs > CAPNP_MAX_SEGMENTS)
    return CAPNP_ERR_FRAMING;
  size_t table_bytes = 4u + 4u * (size_t)nsegs;
  if (table_bytes % 8u != 0)
    table_bytes += 4u; /* pad */
  if (len < table_bytes)
    return CAPNP_ERR_FRAMING;

  size_t total_words = 0;
  uint32_t sizes[CAPNP_MAX_SEGMENTS];
  for (uint32_t i = 0; i < nsegs; i++) {
    sizes[i] = capnp_load_le32(data + 4 + 4 * i);
    total_words += sizes[i];
  }
  size_t body = table_bytes + total_words * CAPNP_WORD_BYTES;
  if (body > len)
    return CAPNP_ERR_FRAMING;

  const uint8_t *base;
  if (copy) {
    m->owned = (uint8_t *)malloc(body);
    if (!m->owned)
      return CAPNP_ERR_ALLOC;
    memcpy(m->owned, data, body);
    m->owned_len = body;
    base = m->owned;
  } else {
    base = data;
  }

  size_t off = table_bytes;
  for (uint32_t i = 0; i < nsegs; i++) {
    m->segs[i].data = base + off;
    m->segs[i].words = sizes[i];
    off += (size_t)sizes[i] * CAPNP_WORD_BYTES;
  }
  m->nsegs = nsegs;
  return CAPNP_OK;
}

int capnp_message_from_flat(capnp_message_t *m, const uint8_t *data,
                            size_t len) {
  capnp_message_init(m);
  return parse_flat(m, data, len, 1);
}

int capnp_message_view_flat(capnp_message_t *m, const uint8_t *data,
                            size_t len) {
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

static int resolve_word(capnp_message_t *m, uint32_t seg, size_t word,
                        uint64_t w, int depth, capnp_ptr_t *out) {
  if (depth <= 0)
    return CAPNP_ERR_DEPTH;
  if (w == 0) {
    out->msg = m;
    out->seg = seg;
    out->word = word;
    out->kind = CAPNP_PK_NULL;
    out->dwords = 0;
    out->pwords = 0;
    out->esize = 0;
    out->count = 0;
    out->step_words = 0;
    return CAPNP_OK;
  }

  int kind = capnp_wp_kind(w);
  if (kind == CAPNP_WK_FAR) {
    uint32_t tseg = capnp_wp_far_seg(w);
    uint32_t toff = capnp_wp_far_off(w);
    if (capnp_wp_far_two(w)) {
      /* double-far: landing pad is two words in tseg */
      if (bounds_word(m, tseg, toff) || bounds_word(m, tseg, toff + 1))
        return CAPNP_ERR_BOUNDS;
      if (charge(m, 2))
        return CAPNP_ERR_TRAVERSAL;
      uint64_t pad = read_word(m, tseg, toff);
      uint64_t tag = read_word(m, tseg, toff + 1);
      if (capnp_wp_kind(pad) != CAPNP_WK_FAR || capnp_wp_far_two(pad))
        return CAPNP_ERR_KIND;
      uint32_t cseg = capnp_wp_far_seg(pad);
      uint32_t coff = capnp_wp_far_off(pad);
      /* tag is the real pointer with offset=0 relative to content */
      if (capnp_wp_kind(tag) == CAPNP_WK_STRUCT) {
        out->msg = m;
        out->seg = cseg;
        out->word = coff;
        out->kind = CAPNP_PK_STRUCT;
        out->dwords = capnp_wp_struct_dwords(tag);
        out->pwords = capnp_wp_struct_pwords(tag);
        out->esize = 0;
        out->count = 0;
        out->step_words = 0;
        return charge(m, (uint64_t)out->dwords + out->pwords);
      }
      if (capnp_wp_kind(tag) == CAPNP_WK_LIST) {
        out->msg = m;
        out->seg = cseg;
        out->word = coff;
        out->kind = CAPNP_PK_LIST;
        out->esize = capnp_wp_list_esize(tag);
        out->count = capnp_wp_list_count(tag);
        out->dwords = 0;
        out->pwords = 0;
        out->step_words = 0;
        if (out->esize == CAPNP_SZ_COMPOSITE) {
          if (bounds_word(m, cseg, coff))
            return CAPNP_ERR_BOUNDS;
          uint64_t t = read_word(m, cseg, coff);
          /* tag word: element count in offset field; sizes in upper */
          out->count = (uint32_t)capnp_wp_offset(t);
          out->dwords = capnp_wp_struct_dwords(t);
          out->pwords = capnp_wp_struct_pwords(t);
          out->step_words = (size_t)out->dwords + out->pwords;
          out->word = coff + 1;
          return charge(m, 1 + (uint64_t)out->count * out->step_words);
        }
        return CAPNP_OK;
      }
      return CAPNP_ERR_KIND;
    }
    /* single far: one word landing pad is the real pointer */
    if (bounds_word(m, tseg, toff))
      return CAPNP_ERR_BOUNDS;
    if (charge(m, 1))
      return CAPNP_ERR_TRAVERSAL;
    uint64_t land = read_word(m, tseg, toff);
    return resolve_word(m, tseg, toff, land, depth - 1, out);
  }

  if (kind == CAPNP_WK_STRUCT) {
    int32_t off = capnp_wp_offset(w);
    /* content starts at word+1+off */
    size_t body = (size_t)((int64_t)word + 1 + off);
    out->msg = m;
    out->seg = seg;
    out->word = body;
    out->kind = CAPNP_PK_STRUCT;
    out->dwords = capnp_wp_struct_dwords(w);
    out->pwords = capnp_wp_struct_pwords(w);
    out->esize = 0;
    out->count = 0;
    out->step_words = 0;
    if (out->dwords || out->pwords) {
      size_t end = body + out->dwords + out->pwords;
      if (bounds_word(m, seg, body) ||
          (end > 0 && bounds_word(m, seg, end - 1)))
        return CAPNP_ERR_BOUNDS;
    }
    return charge(m, (uint64_t)out->dwords + out->pwords);
  }

  if (kind == CAPNP_WK_LIST) {
    int32_t off = capnp_wp_offset(w);
    size_t start = (size_t)((int64_t)word + 1 + off);
    out->msg = m;
    out->seg = seg;
    out->word = start;
    out->kind = CAPNP_PK_LIST;
    out->esize = capnp_wp_list_esize(w);
    out->count = capnp_wp_list_count(w);
    out->dwords = 0;
    out->pwords = 0;
    out->step_words = 0;
    if (out->esize == CAPNP_SZ_COMPOSITE) {
      if (bounds_word(m, seg, start))
        return CAPNP_ERR_BOUNDS;
      if (charge(m, 1))
        return CAPNP_ERR_TRAVERSAL;
      uint64_t tag = read_word(m, seg, start);
      /* list count field was words of content excl. tag; tag carries elem count */
      out->count = (uint32_t)capnp_wp_offset(tag);
      out->dwords = capnp_wp_struct_dwords(tag);
      out->pwords = capnp_wp_struct_pwords(tag);
      out->step_words = (size_t)out->dwords + out->pwords;
      out->word = start + 1;
      uint64_t need = (uint64_t)out->count * out->step_words;
      if (need && bounds_word(m, seg, out->word + need - 1))
        return CAPNP_ERR_BOUNDS;
      return charge(m, need);
    }
    /* primitive / pointer list: charge by byte-ish size */
    uint64_t bits = 0;
    switch (out->esize) {
    case CAPNP_SZ_VOID:
      bits = 0;
      break;
    case CAPNP_SZ_BIT:
      bits = (uint64_t)out->count;
      break;
    case CAPNP_SZ_BYTE:
      bits = (uint64_t)out->count * 8;
      break;
    case CAPNP_SZ_TWO:
      bits = (uint64_t)out->count * 16;
      break;
    case CAPNP_SZ_FOUR:
      bits = (uint64_t)out->count * 32;
      break;
    case CAPNP_SZ_EIGHT:
    case CAPNP_SZ_PTR:
      bits = (uint64_t)out->count * 64;
      break;
    default:
      return CAPNP_ERR_KIND;
    }
    uint64_t words = (bits + 63) / 64;
    if (words && bounds_word(m, seg, start + words - 1))
      return CAPNP_ERR_BOUNDS;
    return charge(m, words);
  }

  if (kind == CAPNP_WK_CAP) {
    out->msg = m;
    out->seg = seg;
    out->word = word;
    out->kind = CAPNP_PK_CAP;
    out->dwords = 0;
    out->pwords = 0;
    out->esize = 0;
    out->count = (uint32_t)((w >> 32) & 0xffffffffull);
    out->step_words = 0;
    return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

static int resolve_ptr(capnp_message_t *m, uint32_t seg, size_t word, int depth,
                       capnp_ptr_t *out) {
  if (bounds_word(m, seg, word))
    return CAPNP_ERR_BOUNDS;
  if (charge(m, 1))
    return CAPNP_ERR_TRAVERSAL;
  uint64_t w = read_word(m, seg, word);
  return resolve_word(m, seg, word, w, depth, out);
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
    out->msg = s->msg;
    out->seg = s->seg;
    out->word = 0;
    out->kind = CAPNP_PK_NULL;
    out->dwords = out->pwords = 0;
    out->esize = 0;
    out->count = 0;
    out->step_words = 0;
    return CAPNP_OK;
  }
  size_t word = s->word + s->dwords + ptr_index;
  return resolve_ptr(s->msg, s->seg, word, s->msg->depth_limit, out);
}

static const uint8_t *data_bytes(const capnp_ptr_t *s) {
  return s->msg->segs[s->seg].data + s->word * CAPNP_WORD_BYTES;
}

uint8_t capnp_get_u8(const capnp_ptr_t *s, uint32_t byte_offset, uint8_t dflt) {
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return dflt;
  if (byte_offset >= (uint32_t)s->dwords * CAPNP_WORD_BYTES)
    return dflt;
  return data_bytes(s)[byte_offset];
}

uint16_t capnp_get_u16(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint16_t dflt) {
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return dflt;
  if (byte_offset + 2 > (uint32_t)s->dwords * CAPNP_WORD_BYTES)
    return dflt;
  const uint8_t *p = data_bytes(s) + byte_offset;
  return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t capnp_get_u32(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint32_t dflt) {
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return dflt;
  if (byte_offset + 4 > (uint32_t)s->dwords * CAPNP_WORD_BYTES)
    return dflt;
  return capnp_load_le32(data_bytes(s) + byte_offset);
}

uint64_t capnp_get_u64(const capnp_ptr_t *s, uint32_t byte_offset,
                       uint64_t dflt) {
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return dflt;
  if (byte_offset + 8 > (uint32_t)s->dwords * CAPNP_WORD_BYTES)
    return dflt;
  return capnp_load_le64(data_bytes(s) + byte_offset);
}

int capnp_get_bool(const capnp_ptr_t *s, uint32_t bit_offset, int dflt) {
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return dflt;
  uint32_t byte_offset = bit_offset / 8;
  if (byte_offset >= (uint32_t)s->dwords * CAPNP_WORD_BYTES)
    return dflt;
  uint8_t bit = (uint8_t)(1u << (bit_offset % 8));
  return (data_bytes(s)[byte_offset] & bit) != 0;
}

int capnp_get_text(const capnp_ptr_t *s, uint16_t ptr_index, const char **out,
                   size_t *len) {
  capnp_ptr_t list;
  int rc = capnp_getp(s, ptr_index, &list);
  if (rc)
    return rc;
  if (list.kind == CAPNP_PK_NULL) {
    if (out)
      *out = "";
    if (len)
      *len = 0;
    return CAPNP_OK;
  }
  if (list.kind != CAPNP_PK_LIST || list.esize != CAPNP_SZ_BYTE)
    return CAPNP_ERR_KIND;
  if (list.count == 0) {
    if (out)
      *out = "";
    if (len)
      *len = 0;
    return CAPNP_OK;
  }
  const char *p =
      (const char *)(list.msg->segs[list.seg].data + list.word * CAPNP_WORD_BYTES);
  /* count includes trailing NUL */
  size_t n = list.count;
  if (n > 0 && p[n - 1] == '\0')
    n -= 1;
  if (out)
    *out = p;
  if (len)
    *len = n;
  return CAPNP_OK;
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
    size_t word = list->word + index;
    return resolve_ptr(list->msg, list->seg, word, list->msg->depth_limit, out);
  }
  if (list->esize == CAPNP_SZ_COMPOSITE) {
    out->msg = list->msg;
    out->seg = list->seg;
    out->word = list->word + (size_t)index * list->step_words;
    out->kind = CAPNP_PK_STRUCT;
    out->dwords = list->dwords;
    out->pwords = list->pwords;
    out->esize = 0;
    out->count = 0;
    out->step_words = 0;
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
    rc = capnp_list_getp(list, index, &elem);
    if (rc)
      return rc;
    if (elem.kind == CAPNP_PK_NULL) {
      if (out)
        *out = "";
      if (len)
        *len = 0;
      return CAPNP_OK;
    }
    if (elem.kind == CAPNP_PK_LIST && elem.esize == CAPNP_SZ_BYTE) {
      const char *p = (const char *)(elem.msg->segs[elem.seg].data +
                                     elem.word * CAPNP_WORD_BYTES);
      size_t n = elem.count;
      if (n > 0 && p[n - 1] == '\0')
        n -= 1;
      if (out)
        *out = p;
      if (len)
        *len = n;
      return CAPNP_OK;
    }
    return CAPNP_ERR_KIND;
  }

  /* C=7 composite upgrade view: element is a struct; Text at pointer 0. */
  if (list->esize == CAPNP_SZ_COMPOSITE) {
    rc = capnp_list_getp(list, index, &elem);
    if (rc)
      return rc;
    if (elem.kind != CAPNP_PK_STRUCT)
      return CAPNP_ERR_KIND;
    return capnp_get_text(&elem, 0, out, len);
  }

  return CAPNP_ERR_KIND;
}
