#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdlib.h>
#include <string.h>

void capnp_builder_init(capnp_builder_t *b) {
  memset(b, 0, sizeof(*b));
}

void capnp_builder_free(capnp_builder_t *b) {
  if (!b)
    return;
  free(b->data);
  b->data = NULL;
  b->words = b->cap = 0;
}

static int ensure(capnp_builder_t *b, size_t need_words) {
  if (need_words <= b->cap)
    return CAPNP_OK;
  size_t ncap = b->cap ? b->cap * 2 : 16;
  while (ncap < need_words)
    ncap *= 2;
  uint8_t *p = (uint8_t *)realloc(b->data, ncap * CAPNP_WORD_BYTES);
  if (!p)
    return CAPNP_ERR_ALLOC;
  /* zero new capacity */
  if (ncap > b->cap)
    memset(p + b->cap * CAPNP_WORD_BYTES, 0,
           (ncap - b->cap) * CAPNP_WORD_BYTES);
  b->data = p;
  b->cap = ncap;
  return CAPNP_OK;
}

static int alloc_words(capnp_builder_t *b, size_t n, size_t *out_word) {
  if (ensure(b, b->words + n))
    return CAPNP_ERR_ALLOC;
  *out_word = b->words;
  b->words += n;
  return CAPNP_OK;
}

int capnp_builder_root(capnp_builder_t *b, capnp_bptr_t *root) {
  if (!b || !root)
    return CAPNP_ERR_ARG;
  if (b->words != 0)
    return CAPNP_ERR_ARG;
  size_t w;
  if (alloc_words(b, 1, &w))
    return CAPNP_ERR_ALLOC;
  root->b = b;
  root->word = 0;
  return CAPNP_OK;
}

int capnp_builder_struct(capnp_bptr_t *ptr, uint16_t dwords, uint16_t pwords,
                         capnp_bptr_t *body_out) {
  if (!ptr || !ptr->b)
    return CAPNP_ERR_ARG;
  size_t body;
  size_t n = (size_t)dwords + pwords;
  if (alloc_words(ptr->b, n ? n : 0, &body) && n)
    return CAPNP_ERR_ALLOC;
  if (n == 0)
    body = ptr->word + 1; /* empty struct still needs a legal offset */
  /* offset from end of pointer word to body */
  int32_t off = (int32_t)((int64_t)body - (int64_t)ptr->word - 1);
  uint64_t w = capnp_wp_make_struct(off, dwords, pwords);
  capnp_store_le64(ptr->b->data + ptr->word * CAPNP_WORD_BYTES, w);
  if (body_out) {
    body_out->b = ptr->b;
    body_out->word = body;
  }
  return CAPNP_OK;
}

size_t capnp_builder_struct_body(const capnp_bptr_t *struct_ptr) {
  /* After capnp_builder_struct, re-read offset. */
  if (!struct_ptr || !struct_ptr->b)
    return 0;
  uint64_t w =
      capnp_load_le64(struct_ptr->b->data + struct_ptr->word * CAPNP_WORD_BYTES);
  int32_t off = capnp_wp_offset(w);
  return (size_t)((int64_t)struct_ptr->word + 1 + off);
}

int capnp_builder_set_u16(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, uint16_t value) {
  if (!b)
    return CAPNP_ERR_ARG;
  size_t abs = body_word * CAPNP_WORD_BYTES + byte_offset;
  if (abs + 2 > b->words * CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  b->data[abs] = (uint8_t)(value);
  b->data[abs + 1] = (uint8_t)(value >> 8);
  return CAPNP_OK;
}

int capnp_builder_set_u32(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, uint32_t value) {
  if (!b)
    return CAPNP_ERR_ARG;
  size_t abs = body_word * CAPNP_WORD_BYTES + byte_offset;
  if (abs + 4 > b->words * CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  capnp_store_le32(b->data + abs, value);
  return CAPNP_OK;
}

int capnp_builder_set_u64(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, uint64_t value) {
  if (!b)
    return CAPNP_ERR_ARG;
  size_t abs = body_word * CAPNP_WORD_BYTES + byte_offset;
  if (abs + 8 > b->words * CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  capnp_store_le64(b->data + abs, value);
  return CAPNP_OK;
}

int capnp_builder_set_f64(capnp_builder_t *b, size_t body_word,
                          uint32_t byte_offset, double value) {
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return capnp_builder_set_u64(b, body_word, byte_offset, bits);
}

int capnp_builder_slot(capnp_builder_t *b, size_t body_word, uint16_t dwords,
                       uint16_t ptr_index, capnp_bptr_t *slot) {
  if (!b || !slot)
    return CAPNP_ERR_ARG;
  size_t pw = body_word + dwords + ptr_index;
  if (pw >= b->words)
    return CAPNP_ERR_BOUNDS;
  slot->b = b;
  slot->word = pw;
  return CAPNP_OK;
}

int capnp_builder_set_bool(capnp_builder_t *b, size_t body_word,
                           uint32_t bit_offset, int value) {
  if (!b)
    return CAPNP_ERR_ARG;
  uint32_t byte_offset = bit_offset / 8;
  size_t abs = body_word * CAPNP_WORD_BYTES + byte_offset;
  if (abs >= b->words * CAPNP_WORD_BYTES)
    return CAPNP_ERR_BOUNDS;
  uint8_t bit = (uint8_t)(1u << (bit_offset % 8));
  if (value)
    b->data[abs] |= bit;
  else
    b->data[abs] &= (uint8_t)~bit;
  return CAPNP_OK;
}

static int write_text_at(capnp_builder_t *b, size_t ptr_word, const char *text,
                         size_t text_len) {
  size_t nbytes = text_len + 1; /* NUL */
  size_t nwords = (nbytes + 7) / 8;
  size_t start;
  if (alloc_words(b, nwords, &start))
    return CAPNP_ERR_ALLOC;
  memset(b->data + start * CAPNP_WORD_BYTES, 0, nwords * CAPNP_WORD_BYTES);
  memcpy(b->data + start * CAPNP_WORD_BYTES, text, text_len);
  int32_t off = (int32_t)((int64_t)start - (int64_t)ptr_word - 1);
  uint64_t w = capnp_wp_make_list(off, CAPNP_SZ_BYTE, (uint32_t)nbytes);
  capnp_store_le64(b->data + ptr_word * CAPNP_WORD_BYTES, w);
  return CAPNP_OK;
}

int capnp_builder_set_text(capnp_builder_t *b, size_t body_word,
                           uint16_t dwords, uint16_t ptr_index, const char *text,
                           size_t text_len) {
  if (!b || !text)
    return CAPNP_ERR_ARG;
  size_t ptr_word = body_word + dwords + ptr_index;
  if (ptr_word >= b->words)
    return CAPNP_ERR_BOUNDS;
  return write_text_at(b, ptr_word, text, text_len);
}

int capnp_builder_set_data(capnp_builder_t *b, size_t body_word,
                           uint16_t dwords, uint16_t ptr_index,
                           const uint8_t *data, size_t data_len) {
  size_t nwords, start, ptr_word;
  int32_t off;
  uint64_t w;

  if (!b)
    return CAPNP_ERR_ARG;
  if (data_len && !data)
    return CAPNP_ERR_ARG;
  ptr_word = body_word + dwords + ptr_index;
  if (ptr_word >= b->words)
    return CAPNP_ERR_BOUNDS;
  nwords = (data_len + 7) / 8;
  if (nwords) {
    if (alloc_words(b, nwords, &start))
      return CAPNP_ERR_ALLOC;
    memset(b->data + start * CAPNP_WORD_BYTES, 0, nwords * CAPNP_WORD_BYTES);
    if (data_len)
      memcpy(b->data + start * CAPNP_WORD_BYTES, data, data_len);
  } else {
    start = ptr_word + 1;
  }
  off = (int32_t)((int64_t)start - (int64_t)ptr_word - 1);
  w = capnp_wp_make_list(off, CAPNP_SZ_BYTE, (uint32_t)data_len);
  capnp_store_le64(b->data + ptr_word * CAPNP_WORD_BYTES, w);
  return CAPNP_OK;
}

static int set_list_prim(capnp_builder_t *b, size_t body_word, uint16_t dwords,
                         uint16_t ptr_index, int esize, uint32_t nitems,
                         size_t elem_bytes, const void *items) {
  size_t ptr_word, nbytes, nwords, start;
  int32_t off;
  uint64_t w;

  if (!b)
    return CAPNP_ERR_ARG;
  ptr_word = body_word + dwords + ptr_index;
  if (ptr_word >= b->words)
    return CAPNP_ERR_BOUNDS;
  nbytes = (size_t)nitems * elem_bytes;
  nwords = (nbytes + 7) / 8;
  if (nwords) {
    if (alloc_words(b, nwords, &start))
      return CAPNP_ERR_ALLOC;
    memset(b->data + start * CAPNP_WORD_BYTES, 0, nwords * CAPNP_WORD_BYTES);
    if (items && nbytes)
      memcpy(b->data + start * CAPNP_WORD_BYTES, items, nbytes);
  } else {
    start = ptr_word + 1;
  }
  off = (int32_t)((int64_t)start - (int64_t)ptr_word - 1);
  w = capnp_wp_make_list(off, esize, nitems);
  capnp_store_le64(b->data + ptr_word * CAPNP_WORD_BYTES, w);
  return CAPNP_OK;
}

int capnp_builder_set_list_u8(capnp_builder_t *b, size_t body_word,
                              uint16_t dwords, uint16_t ptr_index,
                              const uint8_t *items, uint32_t nitems) {
  return set_list_prim(b, body_word, dwords, ptr_index, CAPNP_SZ_BYTE, nitems,
                       1, items);
}

int capnp_builder_set_list_u16(capnp_builder_t *b, size_t body_word,
                               uint16_t dwords, uint16_t ptr_index,
                               const uint16_t *items, uint32_t nitems) {
  return set_list_prim(b, body_word, dwords, ptr_index, CAPNP_SZ_TWO, nitems, 2,
                       items);
}

int capnp_builder_set_list_u32(capnp_builder_t *b, size_t body_word,
                               uint16_t dwords, uint16_t ptr_index,
                               const uint32_t *items, uint32_t nitems) {
  return set_list_prim(b, body_word, dwords, ptr_index, CAPNP_SZ_FOUR, nitems,
                       4, items);
}

int capnp_builder_set_list_u64(capnp_builder_t *b, size_t body_word,
                               uint16_t dwords, uint16_t ptr_index,
                               const uint64_t *items, uint32_t nitems) {
  return set_list_prim(b, body_word, dwords, ptr_index, CAPNP_SZ_EIGHT, nitems,
                       8, items);
}

int capnp_builder_set_list_f64(capnp_builder_t *b, size_t body_word,
                               uint16_t dwords, uint16_t ptr_index,
                               const double *items, uint32_t nitems) {
  return set_list_prim(b, body_word, dwords, ptr_index, CAPNP_SZ_EIGHT, nitems,
                       8, items);
}

int capnp_builder_set_list_text(capnp_builder_t *b, size_t body_word,
                                uint16_t dwords, uint16_t ptr_index,
                                const char *const *items, uint32_t nitems) {
  if (!b)
    return CAPNP_ERR_ARG;
  size_t list_ptr_word = body_word + dwords + ptr_index;
  if (list_ptr_word >= b->words)
    return CAPNP_ERR_BOUNDS;

  size_t list_start;
  if (alloc_words(b, nitems ? nitems : 0, &list_start) && nitems)
    return CAPNP_ERR_ALLOC;
  if (nitems == 0)
    list_start = list_ptr_word + 1;

  /* Normative List(Text): pointer list, element size C=6 (encoding.html). */
  int32_t off = (int32_t)((int64_t)list_start - (int64_t)list_ptr_word - 1);
  uint64_t lw = capnp_wp_make_list(off, CAPNP_SZ_PTR, nitems);
  capnp_store_le64(b->data + list_ptr_word * CAPNP_WORD_BYTES, lw);

  for (uint32_t i = 0; i < nitems; i++) {
    const char *t = items[i] ? items[i] : "";
    size_t tlen = strlen(t);
    if (write_text_at(b, list_start + i, t, tlen))
      return CAPNP_ERR_ALLOC;
  }
  return CAPNP_OK;
}

size_t capnp_builder_ptr_word(size_t body_word, uint16_t dwords,
                              uint16_t ptr_index) {
  return body_word + dwords + ptr_index;
}

int capnp_builder_set_list_struct(capnp_builder_t *b, size_t body_word,
                                  uint16_t dwords, uint16_t ptr_index,
                                  uint32_t nitems, uint16_t elem_dwords,
                                  uint16_t elem_pwords,
                                  size_t *first_elem_body) {
  if (!b)
    return CAPNP_ERR_ARG;
  size_t list_ptr_word = body_word + dwords + ptr_index;
  if (list_ptr_word >= b->words)
    return CAPNP_ERR_BOUNDS;

  size_t step = (size_t)elem_dwords + elem_pwords;
  size_t content_words = (size_t)nitems * step;
  /* tag + elements */
  size_t need = 1 + content_words;
  size_t tag_word;
  if (alloc_words(b, need, &tag_word))
    return CAPNP_ERR_ALLOC;

  /* Tag: struct pointer layout with offset = element count. */
  uint64_t tag =
      capnp_wp_make_struct((int32_t)nitems, elem_dwords, elem_pwords);
  capnp_store_le64(b->data + tag_word * CAPNP_WORD_BYTES, tag);

  int32_t off = (int32_t)((int64_t)tag_word - (int64_t)list_ptr_word - 1);
  /* list count field for composite = words of content excluding tag */
  uint64_t lw =
      capnp_wp_make_list(off, CAPNP_SZ_COMPOSITE, (uint32_t)content_words);
  capnp_store_le64(b->data + list_ptr_word * CAPNP_WORD_BYTES, lw);

  if (first_elem_body)
    *first_elem_body = tag_word + 1;
  return CAPNP_OK;
}

int capnp_builder_serialize(const capnp_builder_t *b, uint8_t **out,
                            size_t *out_len) {
  if (!b || !out || !out_len || !b->data || b->words == 0)
    return CAPNP_ERR_ARG;
  /* 1 segment: count-1=0, size, no pad needed (8 bytes header) */
  size_t header = 8;
  size_t body = b->words * CAPNP_WORD_BYTES;
  size_t total = header + body;
  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf)
    return CAPNP_ERR_ALLOC;
  capnp_store_le32(buf, 0); /* nsegs - 1 */
  capnp_store_le32(buf + 4, (uint32_t)b->words);
  memcpy(buf + header, b->data, body);
  *out = buf;
  *out_len = total;
  return CAPNP_OK;
}
