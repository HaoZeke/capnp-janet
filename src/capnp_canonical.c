/* SPDX-License-Identifier: MIT */
/*
 * Canonical rewrite. See encoding.html#canonicalization.
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_copy.h>
#include <capnp-janet/capnp_pointer.h>

#include <stdlib.h>
#include <string.h>

static int ensure_b(capnp_builder_t *b, size_t need_words) {
  if (need_words <= b->cap)
    return CAPNP_OK;
  {
    size_t ncap = b->cap ? b->cap * 2 : 16;
    uint8_t *p;
    while (ncap < need_words)
      ncap *= 2;
    p = (uint8_t *)realloc(b->data, ncap * CAPNP_WORD_BYTES);
    if (!p)
      return CAPNP_ERR_ALLOC;
    if (ncap > b->cap)
      memset(p + b->cap * CAPNP_WORD_BYTES, 0,
             (ncap - b->cap) * CAPNP_WORD_BYTES);
    b->data = p;
    b->cap = ncap;
  }
  return CAPNP_OK;
}

static int alloc_words_local(capnp_builder_t *b, size_t n, size_t *out_word) {
  if (n == 0) {
    *out_word = b->words;
    return CAPNP_OK;
  }
  if (ensure_b(b, b->words + n))
    return CAPNP_ERR_ALLOC;
  *out_word = b->words;
  b->words += n;
  return CAPNP_OK;
}

static int trimmed_dwords(const capnp_ptr_t *p) {
  int nd = p->dwords;
  while (nd > 0) {
    uint64_t w = capnp_get_u64(p, (uint32_t)(nd - 1) * 8, 0);
    if (w != 0)
      break;
    nd--;
  }
  return nd;
}

static int trimmed_pwords(const capnp_ptr_t *p) {
  int np = p->pwords;
  while (np > 0) {
    capnp_ptr_t q;
    if (capnp_getp(p, (uint16_t)(np - 1), &q) != CAPNP_OK)
      break;
    if (q.kind != CAPNP_PK_NULL)
      break;
    np--;
  }
  return np;
}

static void composite_trim(const capnp_ptr_t *list, int *nd, int *np) {
  uint32_t i;
  *nd = 0;
  *np = 0;
  for (i = 0; i < list->count; i++) {
    capnp_ptr_t el;
    int td, tp;
    if (capnp_list_getp(list, i, &el) != CAPNP_OK)
      continue;
    td = trimmed_dwords(&el);
    tp = trimmed_pwords(&el);
    if (td > *nd)
      *nd = td;
    if (tp > *np)
      *np = tp;
  }
}

int capnp_copy_ptr_to_word(capnp_builder_t *b, size_t slot_word,
                            const capnp_ptr_t *src, int depth);

static int write_struct_body_data(capnp_builder_t *b, size_t body, int nd,
                                  const capnp_ptr_t *src) {
  int i;
  for (i = 0; i < nd; i++) {
    uint64_t w = 0;
    if (i < src->dwords)
      w = capnp_get_u64(src, (uint32_t)i * 8, 0);
    capnp_store_le64(b->data + (body + (size_t)i) * CAPNP_WORD_BYTES, w);
  }
  return CAPNP_OK;
}

static int write_list_to_slot(capnp_builder_t *b, size_t slot_word,
                              const capnp_ptr_t *list, int depth) {
  uint32_t n = list->count;
  int esize = list->esize;

  if (esize == CAPNP_SZ_COMPOSITE) {
    int nd, np;
    size_t first, tag_word, step, content;
    uint32_t i;
    int32_t off;
    uint64_t lw, tag;

    composite_trim(list, &nd, &np);
    step = (size_t)nd + (size_t)np;
    content = (size_t)n * step;
    if (alloc_words_local(b, 1 + content, &tag_word))
      return CAPNP_ERR_ALLOC;
    tag = capnp_wp_make_struct((int32_t)n, (uint16_t)nd, (uint16_t)np);
    capnp_store_le64(b->data + tag_word * CAPNP_WORD_BYTES, tag);
    off = (int32_t)((int64_t)tag_word - (int64_t)slot_word - 1);
    lw = capnp_wp_make_list(off, CAPNP_SZ_COMPOSITE, (uint32_t)content);
    capnp_store_le64(b->data + slot_word * CAPNP_WORD_BYTES, lw);
    first = tag_word + 1;
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      int k;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return CAPNP_ERR_KIND;
      write_struct_body_data(b, first + i * step, nd, &el);
      for (k = 0; k < np; k++) {
        size_t cslot = first + i * step + (size_t)nd + (size_t)k;
        capnp_ptr_t child;
        if (k < el.pwords) {
          if (capnp_getp(&el, (uint16_t)k, &child) != CAPNP_OK)
            return CAPNP_ERR_KIND;
          if (capnp_copy_ptr_to_word(b, cslot, &child, depth + 1))
            return CAPNP_ERR_ALLOC;
        } else {
          capnp_store_le64(b->data + cslot * CAPNP_WORD_BYTES, 0);
        }
      }
    }
    return CAPNP_OK;
  }

  if (esize == CAPNP_SZ_PTR) {
    size_t list_start;
    uint32_t i;
    int32_t off;
    uint64_t lw;
    if (n) {
      if (alloc_words_local(b, n, &list_start))
        return CAPNP_ERR_ALLOC;
    } else {
      list_start = slot_word + 1;
    }
    off = (int32_t)((int64_t)list_start - (int64_t)slot_word - 1);
    lw = capnp_wp_make_list(off, CAPNP_SZ_PTR, n);
    capnp_store_le64(b->data + slot_word * CAPNP_WORD_BYTES, lw);
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return CAPNP_ERR_KIND;
      if (capnp_copy_ptr_to_word(b, list_start + i, &el, depth + 1))
        return CAPNP_ERR_ALLOC;
    }
    return CAPNP_OK;
  }

  {
    size_t bits = 0, nbytes, nwords, start;
    int32_t off;
    uint64_t lw;
    const uint8_t *srcb;
    switch (esize) {
    case CAPNP_SZ_VOID:
      bits = 0;
      break;
    case CAPNP_SZ_BIT:
      bits = n;
      break;
    case CAPNP_SZ_BYTE:
      bits = (size_t)n * 8;
      break;
    case CAPNP_SZ_TWO:
      bits = (size_t)n * 16;
      break;
    case CAPNP_SZ_FOUR:
      bits = (size_t)n * 32;
      break;
    case CAPNP_SZ_EIGHT:
      bits = (size_t)n * 64;
      break;
    default:
      return CAPNP_ERR_KIND;
    }
    nbytes = (bits + 7) / 8;
    nwords = (nbytes + 7) / 8;
    if (nwords) {
      if (alloc_words_local(b, nwords, &start))
        return CAPNP_ERR_ALLOC;
      memset(b->data + start * CAPNP_WORD_BYTES, 0, nwords * CAPNP_WORD_BYTES);
      srcb = list->msg->segs[list->seg].data + list->word * CAPNP_WORD_BYTES;
      if (nbytes)
        memcpy(b->data + start * CAPNP_WORD_BYTES, srcb, nbytes);
    } else {
      start = slot_word + 1;
    }
    off = (int32_t)((int64_t)start - (int64_t)slot_word - 1);
    lw = capnp_wp_make_list(off, esize, n);
    capnp_store_le64(b->data + slot_word * CAPNP_WORD_BYTES, lw);
    return CAPNP_OK;
  }
}

int capnp_copy_ptr_to_word(capnp_builder_t *b, size_t slot_word,
                            const capnp_ptr_t *src, int depth) {
  if (depth > 64)
    return CAPNP_ERR_DEPTH;
  if (!src || src->kind == CAPNP_PK_NULL) {
    capnp_store_le64(b->data + slot_word * CAPNP_WORD_BYTES, 0);
    return CAPNP_OK;
  }
  if (src->kind == CAPNP_PK_CAP)
    return CAPNP_ERR_KIND;
  if (src->kind == CAPNP_PK_LIST)
    return write_list_to_slot(b, slot_word, src, depth);
  if (src->kind == CAPNP_PK_STRUCT) {
    int nd = trimmed_dwords(src);
    int np = trimmed_pwords(src);
    size_t body;
    int32_t off;
    uint64_t w;
    int k;

    if (nd == 0 && np == 0) {
      w = capnp_wp_make_struct(-1, 0, 0);
      capnp_store_le64(b->data + slot_word * CAPNP_WORD_BYTES, w);
      return CAPNP_OK;
    }
    if (alloc_words_local(b, (size_t)nd + (size_t)np, &body))
      return CAPNP_ERR_ALLOC;
    off = (int32_t)((int64_t)body - (int64_t)slot_word - 1);
    w = capnp_wp_make_struct(off, (uint16_t)nd, (uint16_t)np);
    capnp_store_le64(b->data + slot_word * CAPNP_WORD_BYTES, w);
    write_struct_body_data(b, body, nd, src);
    for (k = 0; k < np; k++) {
      capnp_ptr_t child;
      size_t cslot = body + (size_t)nd + (size_t)k;
      if (k < src->pwords) {
        if (capnp_getp(src, (uint16_t)k, &child) != CAPNP_OK)
          return CAPNP_ERR_KIND;
        if (capnp_copy_ptr_to_word(b, cslot, &child, depth + 1))
          return CAPNP_ERR_ALLOC;
      } else {
        capnp_store_le64(b->data + cslot * CAPNP_WORD_BYTES, 0);
      }
    }
    return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

int capnp_canonicalize(const capnp_message_t *m, uint8_t **out,
                       size_t *out_len) {
  capnp_builder_t b;
  capnp_ptr_t root;
  capnp_message_t mut;
  size_t root_word;
  size_t nbytes;
  uint8_t *buf;

  if (!m || !out || !out_len)
    return CAPNP_ERR_ARG;
  *out = NULL;
  *out_len = 0;

  mut = *m;
  mut.owned = NULL;
  mut.traversal_left = CAPNP_DEFAULT_TRAVERSAL_WORDS;
  mut.depth_limit = CAPNP_DEFAULT_DEPTH_LIMIT;

  if (capnp_root(&mut, &root))
    return CAPNP_ERR_KIND;

  capnp_builder_init(&b);
  if (alloc_words_local(&b, 1, &root_word)) {
    capnp_builder_free(&b);
    return CAPNP_ERR_ALLOC;
  }
  if (capnp_copy_ptr_to_word(&b, 0, &root, 0)) {
    capnp_builder_free(&b);
    return CAPNP_ERR_ALLOC;
  }

  nbytes = b.words * CAPNP_WORD_BYTES;
  buf = (uint8_t *)malloc(nbytes ? nbytes : 1);
  if (!buf) {
    capnp_builder_free(&b);
    return CAPNP_ERR_ALLOC;
  }
  if (nbytes)
    memcpy(buf, b.data, nbytes);
  *out = buf;
  *out_len = nbytes;
  capnp_builder_free(&b);
  return CAPNP_OK;
}

int capnp_canonicalize_framed(const capnp_message_t *m, uint8_t **out,
                              size_t *out_len) {
  uint8_t *raw = NULL;
  size_t raw_len = 0;
  uint8_t *framed = NULL;
  size_t nwords, total;
  int rc;

  rc = capnp_canonicalize(m, &raw, &raw_len);
  if (rc)
    return rc;
  if (raw_len % 8 != 0) {
    free(raw);
    return CAPNP_ERR_KIND;
  }
  nwords = raw_len / 8;
  total = 8 + raw_len;
  framed = (uint8_t *)malloc(total);
  if (!framed) {
    free(raw);
    return CAPNP_ERR_ALLOC;
  }
  capnp_store_le32(framed, 0);
  capnp_store_le32(framed + 4, (uint32_t)nwords);
  memcpy(framed + 8, raw, raw_len);
  free(raw);
  *out = framed;
  *out_len = total;
  return CAPNP_OK;
}
