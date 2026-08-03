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

static uint8_t *wptr(capnp_builder_t *b, uint32_t seg, size_t word) {
  return b->segs[seg].data + word * CAPNP_WORD_BYTES;
}

/* Always allocate in last segment, growing it (force_single semantics). */
static int alloc_local(capnp_builder_t *b, size_t n, uint32_t *out_seg,
                       size_t *out_word) {
  capnp_bseg_t *last;
  if (n == 0) {
    *out_seg = b->nsegs - 1;
    *out_word = b->segs[b->nsegs - 1].words;
    return CAPNP_OK;
  }
  if (b->nsegs == 0)
    return CAPNP_ERR_ARG;
  last = &b->segs[b->nsegs - 1];
  if (last->words + n > last->cap) {
    size_t ncap = last->cap ? last->cap * 2 : 16;
    uint8_t *p;
    while (ncap < last->words + n)
      ncap *= 2;
    p = (uint8_t *)realloc(last->data, ncap * CAPNP_WORD_BYTES);
    if (!p)
      return CAPNP_ERR_ALLOC;
    if (ncap > last->cap)
      memset(p + last->cap * CAPNP_WORD_BYTES, 0,
             (ncap - last->cap) * CAPNP_WORD_BYTES);
    last->data = p;
    last->cap = ncap;
  }
  *out_seg = b->nsegs - 1;
  *out_word = last->words;
  last->words += n;
  return CAPNP_OK;
}

static int trimmed_dwords(const capnp_ptr_t *p) {
  int nd = p->dwords;
  while (nd > 0) {
    if (capnp_get_u64(p, (uint32_t)(nd - 1) * 8, 0) != 0)
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

int capnp_copy_ptr_to_word(capnp_builder_t *b, uint32_t slot_seg,
                           size_t slot_word, const capnp_ptr_t *src, int depth);

static int write_struct_body_data(capnp_builder_t *b, uint32_t seg, size_t body,
                                  int nd, const capnp_ptr_t *src) {
  int i;
  for (i = 0; i < nd; i++) {
    uint64_t w = 0;
    if (i < src->dwords)
      w = capnp_get_u64(src, (uint32_t)i * 8, 0);
    capnp_store_le64(wptr(b, seg, body + (size_t)i), w);
  }
  return CAPNP_OK;
}

static int write_list_to_slot(capnp_builder_t *b, uint32_t slot_seg,
                              size_t slot_word, const capnp_ptr_t *list,
                              int depth) {
  uint32_t n = list->count;
  int esize = list->esize;

  if (esize == CAPNP_SZ_COMPOSITE) {
    int nd, np;
    size_t first, tag_word, step, content;
    uint32_t tag_seg, i;
    int32_t off;
    uint64_t lw, tag;
    composite_trim(list, &nd, &np);
    step = (size_t)nd + (size_t)np;
    content = (size_t)n * step;
    if (alloc_local(b, 1 + content, &tag_seg, &tag_word))
      return CAPNP_ERR_ALLOC;
    tag = capnp_wp_make_struct((int32_t)n, (uint16_t)nd, (uint16_t)np);
    capnp_store_le64(wptr(b, tag_seg, tag_word), tag);
    off = (int32_t)((int64_t)tag_word - (int64_t)slot_word - 1);
    lw = capnp_wp_make_list(off, CAPNP_SZ_COMPOSITE, (uint32_t)content);
    capnp_store_le64(wptr(b, slot_seg, slot_word), lw);
    first = tag_word + 1;
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      int k;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return CAPNP_ERR_KIND;
      write_struct_body_data(b, tag_seg, first + i * step, nd, &el);
      for (k = 0; k < np; k++) {
        size_t cslot = first + i * step + (size_t)nd + (size_t)k;
        capnp_ptr_t child;
        if (k < el.pwords) {
          if (capnp_getp(&el, (uint16_t)k, &child) != CAPNP_OK)
            return CAPNP_ERR_KIND;
          if (capnp_copy_ptr_to_word(b, tag_seg, cslot, &child, depth + 1))
            return CAPNP_ERR_ALLOC;
        } else {
          capnp_store_le64(wptr(b, tag_seg, cslot), 0);
        }
      }
    }
    return CAPNP_OK;
  }

  if (esize == CAPNP_SZ_PTR) {
    size_t list_start;
    uint32_t list_seg, i;
    int32_t off;
    uint64_t lw;
    if (n) {
      if (alloc_local(b, n, &list_seg, &list_start))
        return CAPNP_ERR_ALLOC;
    } else {
      list_seg = slot_seg;
      list_start = slot_word + 1;
    }
    off = (int32_t)((int64_t)list_start - (int64_t)slot_word - 1);
    lw = capnp_wp_make_list(off, CAPNP_SZ_PTR, n);
    capnp_store_le64(wptr(b, slot_seg, slot_word), lw);
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return CAPNP_ERR_KIND;
      if (capnp_copy_ptr_to_word(b, list_seg, list_start + i, &el, depth + 1))
        return CAPNP_ERR_ALLOC;
    }
    return CAPNP_OK;
  }

  {
    size_t bits = 0, nbytes, nwords, start;
    uint32_t start_seg;
    int32_t off;
    uint64_t lw;
    const uint8_t *srcb;
    switch (esize) {
    case CAPNP_SZ_VOID: bits = 0; break;
    case CAPNP_SZ_BIT: bits = n; break;
    case CAPNP_SZ_BYTE: bits = (size_t)n * 8; break;
    case CAPNP_SZ_TWO: bits = (size_t)n * 16; break;
    case CAPNP_SZ_FOUR: bits = (size_t)n * 32; break;
    case CAPNP_SZ_EIGHT: bits = (size_t)n * 64; break;
    default: return CAPNP_ERR_KIND;
    }
    nbytes = (bits + 7) / 8;
    nwords = (nbytes + 7) / 8;
    if (nwords) {
      if (alloc_local(b, nwords, &start_seg, &start))
        return CAPNP_ERR_ALLOC;
      memset(wptr(b, start_seg, start), 0, nwords * CAPNP_WORD_BYTES);
      srcb = list->msg->segs[list->seg].data + list->word * CAPNP_WORD_BYTES;
      if (nbytes)
        memcpy(wptr(b, start_seg, start), srcb, nbytes);
    } else {
      start_seg = slot_seg;
      start = slot_word + 1;
    }
    off = (int32_t)((int64_t)start - (int64_t)slot_word - 1);
    lw = capnp_wp_make_list(off, esize, n);
    capnp_store_le64(wptr(b, slot_seg, slot_word), lw);
    return CAPNP_OK;
  }
}

int capnp_copy_ptr_to_word(capnp_builder_t *b, uint32_t slot_seg,
                           size_t slot_word, const capnp_ptr_t *src, int depth) {
  if (depth > 64)
    return CAPNP_ERR_DEPTH;
  if (!src || src->kind == CAPNP_PK_NULL) {
    capnp_store_le64(wptr(b, slot_seg, slot_word), 0);
    return CAPNP_OK;
  }
  if (src->kind == CAPNP_PK_CAP)
    return CAPNP_ERR_KIND;
  if (src->kind == CAPNP_PK_LIST)
    return write_list_to_slot(b, slot_seg, slot_word, src, depth);
  if (src->kind == CAPNP_PK_STRUCT) {
    int nd = trimmed_dwords(src);
    int np = trimmed_pwords(src);
    size_t body;
    uint32_t body_seg;
    int32_t off;
    uint64_t w;
    int k;
    if (nd == 0 && np == 0) {
      w = capnp_wp_make_struct(-1, 0, 0);
      capnp_store_le64(wptr(b, slot_seg, slot_word), w);
      return CAPNP_OK;
    }
    if (alloc_local(b, (size_t)nd + (size_t)np, &body_seg, &body))
      return CAPNP_ERR_ALLOC;
    off = (int32_t)((int64_t)body - (int64_t)slot_word - 1);
    w = capnp_wp_make_struct(off, (uint16_t)nd, (uint16_t)np);
    capnp_store_le64(wptr(b, slot_seg, slot_word), w);
    write_struct_body_data(b, body_seg, body, nd, src);
    for (k = 0; k < np; k++) {
      capnp_ptr_t child;
      size_t cslot = body + (size_t)nd + (size_t)k;
      if (k < src->pwords) {
        if (capnp_getp(src, (uint16_t)k, &child) != CAPNP_OK)
          return CAPNP_ERR_KIND;
        if (capnp_copy_ptr_to_word(b, body_seg, cslot, &child, depth + 1))
          return CAPNP_ERR_ALLOC;
      } else {
        capnp_store_le64(wptr(b, body_seg, cslot), 0);
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
  size_t root_word, nbytes;
  uint32_t root_seg;
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

  capnp_builder_init_sized(&b, CAPNP_BUILDER_DEFAULT_FIRST_WORDS);
  b.force_single = 1;
  if (alloc_local(&b, 1, &root_seg, &root_word)) {
    capnp_builder_free(&b);
    return CAPNP_ERR_ALLOC;
  }
  if (capnp_copy_ptr_to_word(&b, root_seg, root_word, &root, 0)) {
    capnp_builder_free(&b);
    return CAPNP_ERR_ALLOC;
  }
  nbytes = b.segs[0].words * CAPNP_WORD_BYTES;
  buf = (uint8_t *)malloc(nbytes ? nbytes : 1);
  if (!buf) {
    capnp_builder_free(&b);
    return CAPNP_ERR_ALLOC;
  }
  if (nbytes)
    memcpy(buf, b.segs[0].data, nbytes);
  capnp_builder_free(&b);
  *out = buf;
  *out_len = nbytes;
  return CAPNP_OK;
}

int capnp_canonicalize_framed(const capnp_message_t *m, uint8_t **out,
                              size_t *out_len) {
  uint8_t *raw = NULL;
  size_t raw_len = 0;
  uint8_t *buf;
  size_t total;
  int rc = capnp_canonicalize(m, &raw, &raw_len);
  if (rc)
    return rc;
  total = 8 + raw_len;
  buf = (uint8_t *)malloc(total ? total : 1);
  if (!buf) {
    free(raw);
    return CAPNP_ERR_ALLOC;
  }
  capnp_store_le32(buf, 0);
  capnp_store_le32(buf + 4, (uint32_t)(raw_len / CAPNP_WORD_BYTES));
  if (raw_len)
    memcpy(buf + 8, raw, raw_len);
  free(raw);
  *out = buf;
  *out_len = total;
  return CAPNP_OK;
}
