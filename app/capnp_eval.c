#define _POSIX_C_SOURCE 200809L
/*
 * capnp-eval: evaluate constants and convert messages between binary, packed,
 * flat, flat-packed, canonical, text, and json formats, matching the official
 * `capnp eval`, `capnp convert`, `capnp decode`, and `capnp encode` tools.
 *
 * The schema is parsed by `capnp compile -o-`, which is the compiler's
 * CodeGeneratorRequest. This program does not reimplement the schema language.
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_copy.h>
#include <capnp-janet/capnp_dyn.h>
#include <capnp-janet/capnp_message.h>
#include <capnp-janet/capnp_packed.h>
#include <capnp-janet/capnp_pointer.h>

#include "capnp_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  FMT_BINARY = 0,
  FMT_PACKED = 1,
  FMT_FLAT = 2,
  FMT_FLAT_PACKED = 3,
  FMT_CANONICAL = 4,
  FMT_TEXT = 5,
  FMT_JSON = 6
};

static int parse_fmt(const char *s) {
  if (strcmp(s, "binary") == 0)
    return FMT_BINARY;
  if (strcmp(s, "packed") == 0)
    return FMT_PACKED;
  if (strcmp(s, "flat") == 0)
    return FMT_FLAT;
  if (strcmp(s, "flat-packed") == 0)
    return FMT_FLAT_PACKED;
  if (strcmp(s, "canonical") == 0)
    return FMT_CANONICAL;
  if (strcmp(s, "text") == 0)
    return FMT_TEXT;
  if (strcmp(s, "json") == 0)
    return FMT_JSON;
  return -1;
}

static char *read_all(FILE *f, size_t *len) {
  char *buf = NULL;
  size_t n = 0, cap = 0;
  for (;;) {
    size_t got;
    if (n == cap) {
      size_t ncap = cap ? cap * 2 : 4096;
      char *p = (char *)realloc(buf, ncap + 1);
      if (!p) {
        free(buf);
        return NULL;
      }
      buf = p;
      cap = ncap;
    }
    got = fread(buf + n, 1, cap - n, f);
    n += got;
    if (got == 0)
      break;
  }
  if (buf)
    buf[n] = 0;
  *len = n;
  return buf;
}

static int cmd_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
  va_list ap;
  int need;
  char *next;
  size_t ncap;
  va_start(ap, fmt);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0 || *len > SIZE_MAX - (size_t)need - 1)
    return -1;
  if (*len + (size_t)need + 1 > *cap) {
    ncap = *cap ? *cap : 128u;
    while (ncap < *len + (size_t)need + 1) {
      if (ncap > SIZE_MAX / 2u)
        return -1;
      ncap *= 2u;
    }
    next = (char *)realloc(*buf, ncap);
    if (!next)
      return -1;
    *buf = next;
    *cap = ncap;
  }
  va_start(ap, fmt);
  if (vsnprintf(*buf + *len, *cap - *len, fmt, ap) != need) {
    va_end(ap);
    return -1;
  }
  va_end(ap);
  *len += (size_t)need;
  return 0;
}

static int compile_schema(const char *schema_path, const char **import_dirs,
                          int nimports, capnp_dyn_t **out_dyn) {
  char *cmd = NULL;
  size_t len = 0, cap = 0;
  FILE *pipe;
  char *req = NULL;
  size_t req_len = 0;
  int i;

  for (i = 0; schema_path[i]; i++) {
    if (schema_path[i] == '\'' || schema_path[i] == '"' ||
        schema_path[i] == '\n') {
      fprintf(stderr, "capnp-eval: schema path has a quote\n");
      return -1;
    }
  }
  if (cmd_append(&cmd, &len, &cap,
                 "INC=$(dirname \"$(dirname \"$(command -v capnp)\")\")/include; "
                 "capnp compile"))
    goto fail;
  if (nimports == 0 && cmd_append(&cmd, &len, &cap, " -I\"$INC\""))
    goto fail;
  for (i = 0; i < nimports; i++) {
    int j;
    for (j = 0; import_dirs[i][j]; j++) {
      if (import_dirs[i][j] == '\'' || import_dirs[i][j] == '"' ||
          import_dirs[i][j] == '\n') {
        fprintf(stderr, "capnp-eval: import path has a quote\n");
        free(cmd);
        return -1;
      }
    }
    if (cmd_append(&cmd, &len, &cap, " -I'%s'", import_dirs[i]))
      goto fail;
  }
  if (cmd_append(&cmd, &len, &cap, " -o- '%s'", schema_path))
    goto fail;
  pipe = popen(cmd, "r");
  free(cmd);
  cmd = NULL;
  if (!pipe) {
    fprintf(stderr, "capnp-eval: could not run capnp compile\n");
    return -1;
  }
  req = read_all(pipe, &req_len);
  if (pclose(pipe) != 0 || !req) {
    fprintf(stderr, "capnp-eval: capnp compile failed\n");
    free(req);
    return -1;
  }
  if (capnp_dyn_init(out_dyn, (const uint8_t *)req, req_len) != CAPNP_OK) {
    free(req);
    return -1;
  }
  free(req);
  return 0;
fail:
  fprintf(stderr, "capnp-eval: could not build the compile command\n");
  free(cmd);
  return -1;
}

static int copy_ptr_untrimmed(capnp_builder_t *b, uint32_t slot_seg,
                              size_t slot_word, const capnp_ptr_t *src,
                              int depth);

static int copy_list_untrimmed(capnp_builder_t *b, uint32_t slot_seg,
                               size_t slot_word, const capnp_ptr_t *list,
                               int depth) {
  uint32_t n = list->count;
  int esize = list->esize;

  if (esize == CAPNP_SZ_COMPOSITE) {
    int nd = list->dwords;
    int np = list->pwords;
    size_t first, step = (size_t)nd + (size_t)np;
    uint32_t tag_seg, i;
    capnp_bptr_t first_bptr, body_bptr;

    body_bptr.b = b;
    body_bptr.seg = slot_seg;
    body_bptr.word = slot_word;
    if (capnp_builder_set_list_struct(&body_bptr, 0, 0, n, (uint16_t)nd,
                                      (uint16_t)np, &first_bptr))
      return -1;
    tag_seg = first_bptr.seg;
    first = first_bptr.word;
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      int k;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return -1;
      for (k = 0; k < nd; k++) {
        uint64_t w =
            (k < el.dwords) ? capnp_get_u64(&el, (uint32_t)k * 8, 0) : 0;
        capnp_store_le64(
            b->segs[tag_seg].data + (first + i * step + (size_t)k) * 8u, w);
      }
      for (k = 0; k < np; k++) {
        size_t cslot = first + i * step + (size_t)nd + (size_t)k;
        capnp_ptr_t child;
        if (k < el.pwords) {
          if (capnp_getp(&el, (uint16_t)k, &child) != CAPNP_OK)
            return -1;
          if (copy_ptr_untrimmed(b, tag_seg, cslot, &child, depth + 1))
            return -1;
        }
      }
    }
    return 0;
  }

  if (esize == CAPNP_SZ_PTR) {
    capnp_bptr_t body_bptr, first_bptr;
    uint32_t i;
    body_bptr.b = b;
    body_bptr.seg = slot_seg;
    body_bptr.word = slot_word;
    if (capnp_builder_init_ptr_list(&body_bptr, 0, 0, n, &first_bptr))
      return -1;
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return -1;
      if (copy_ptr_untrimmed(b, first_bptr.seg, first_bptr.word + i, &el,
                             depth + 1))
        return -1;
    }
    return 0;
  }

  {
    capnp_bptr_t body_bptr;
    const uint8_t *srcb =
        list->msg->segs[list->seg].data + list->word * CAPNP_WORD_BYTES;
    body_bptr.b = b;
    body_bptr.seg = slot_seg;
    body_bptr.word = slot_word;
    switch (esize) {
    case CAPNP_SZ_VOID:
      return capnp_builder_set_list_void(&body_bptr, 0, 0, n);
    case CAPNP_SZ_BIT: {
      size_t nbytes = ((size_t)n + 7u) / 8u;
      capnp_bptr_t start;
      if (capnp_builder_init_list(&body_bptr, 0, 0, CAPNP_SZ_BIT, n, &start))
        return -1;
      if (nbytes) {
        uint8_t *dst =
            b->segs[start.seg].data + start.word * CAPNP_WORD_BYTES;
        memcpy(dst, srcb, nbytes);
        if (n % 8u)
          dst[nbytes - 1] &= (uint8_t)((1u << (n % 8u)) - 1u);
      }
      return 0;
    }
    case CAPNP_SZ_BYTE:
    case CAPNP_SZ_TWO:
    case CAPNP_SZ_FOUR:
    case CAPNP_SZ_EIGHT: {
      static const size_t width[] = {0, 0, 1, 2, 4, 8};
      size_t nbytes = (size_t)n * width[esize];
      capnp_bptr_t start;
      if (capnp_builder_init_list(&body_bptr, 0, 0, esize, n, &start))
        return -1;
      if (nbytes)
        memcpy(b->segs[start.seg].data + start.word * CAPNP_WORD_BYTES, srcb,
               nbytes);
      return 0;
    }
    default:
      return -1;
    }
  }
}

static int copy_ptr_untrimmed(capnp_builder_t *b, uint32_t slot_seg,
                              size_t slot_word, const capnp_ptr_t *src,
                              int depth) {
  if (depth > 64)
    return -1;
  if (!src || src->kind == CAPNP_PK_NULL) {
    capnp_store_le64(b->segs[slot_seg].data + slot_word * 8u, 0);
    return 0;
  }
  if (src->kind == CAPNP_PK_LIST)
    return copy_list_untrimmed(b, slot_seg, slot_word, src, depth);
  if (src->kind == CAPNP_PK_STRUCT) {
    int nd = src->dwords;
    int np = src->pwords;
    capnp_bptr_t slot_bptr, body_bptr;
    int k;
    slot_bptr.b = b;
    slot_bptr.seg = slot_seg;
    slot_bptr.word = slot_word;
    if (capnp_builder_struct(&slot_bptr, (uint16_t)nd, (uint16_t)np,
                             &body_bptr))
      return -1;
    for (k = 0; k < nd; k++) {
      uint64_t w = capnp_get_u64(src, (uint32_t)k * 8, 0);
      capnp_store_le64(
          b->segs[body_bptr.seg].data + (body_bptr.word + (size_t)k) * 8u, w);
    }
    for (k = 0; k < np; k++) {
      capnp_ptr_t child;
      if (capnp_getp(src, (uint16_t)k, &child) != CAPNP_OK)
        return -1;
      if (copy_ptr_untrimmed(b, body_bptr.seg,
                             body_bptr.word + (size_t)nd + (size_t)k, &child,
                             depth + 1))
        return -1;
    }
    return 0;
  }
  return -1;
}

/* Rebuild a message into a single-segment (or segment_size-bounded) builder
 * using copy_ptr_untrimmed, matching MallocMessageBuilder + setRoot(reader)
 * in C++ writeConversion. */
static int rebuild_message(const capnp_message_t *src, size_t seg_words,
                           int single_flat, uint8_t **out_bytes,
                           size_t *out_len) {
  capnp_message_t mut = *src;
  capnp_ptr_t root;
  capnp_builder_t b;
  capnp_bptr_t broot;
  int rc;

  mut.owned = NULL;
  mut.ready = 0;
  if (capnp_root(&mut, &root) != CAPNP_OK)
    return -1;
  if (root.kind != CAPNP_PK_STRUCT && root.kind != CAPNP_PK_NULL)
    return -1;
  if (single_flat) {
    capnp_builder_init_sized(&b, CAPNP_BUILDER_DEFAULT_FIRST_WORDS);
    b.force_single = 1;
  } else if (seg_words > 0) {
    capnp_builder_init_malloc(&b, seg_words, CAPNP_ALLOC_FIXED_SIZE);
  } else {
    capnp_builder_init_malloc(&b, CAPNP_BUILDER_DEFAULT_FIRST_WORDS,
                              CAPNP_ALLOC_GROW_HEURISTICALLY);
  }
  if (capnp_builder_root(&b, &broot)) {
    capnp_builder_free(&b);
    return -1;
  }
  /* setRoot(getRoot<AnyStruct>()) writes a null root as an empty struct. */
  if (root.kind == CAPNP_PK_NULL) {
    capnp_bptr_t body;
    if (capnp_builder_struct(&broot, 0, 0, &body)) {
      capnp_builder_free(&b);
      return -1;
    }
  } else if (copy_ptr_untrimmed(&b, broot.seg, broot.word, &root, 0)) {
    capnp_builder_free(&b);
    return -1;
  }
  if (single_flat) {
    size_t nbytes = b.segs[0].words * CAPNP_WORD_BYTES;
    uint8_t *buf = (uint8_t *)malloc(nbytes ? nbytes : 1);
    if (!buf) {
      capnp_builder_free(&b);
      return -1;
    }
    if (nbytes)
      memcpy(buf, b.segs[0].data, nbytes);
    capnp_builder_free(&b);
    *out_bytes = buf;
    *out_len = nbytes;
    return 0;
  }
  rc = capnp_builder_serialize(&b, out_bytes, out_len);
  capnp_builder_free(&b);
  return rc == CAPNP_OK ? 0 : -1;
}

static int write_converted_message(capnp_dyn_t *dyn,
                                   const char *type_name,
                                   const capnp_message_t *m, int to_fmt,
                                   int pretty, size_t seg_words) {
  switch (to_fmt) {
  case FMT_BINARY: {
    uint8_t *framed = NULL;
    size_t flen = 0;
    if (rebuild_message(m, seg_words, 0, &framed, &flen))
      return -1;
    if (fwrite(framed, 1, flen, stdout) != flen) {
      free(framed);
      return -1;
    }
    free(framed);
    return 0;
  }
  case FMT_PACKED: {
    uint8_t *framed = NULL, *packed = NULL;
    size_t flen = 0, plen = 0;
    if (rebuild_message(m, seg_words, 0, &framed, &flen))
      return -1;
    if (capnp_pack(framed, flen, &packed, &plen) != CAPNP_OK) {
      free(framed);
      return -1;
    }
    free(framed);
    if (fwrite(packed, 1, plen, stdout) != plen) {
      free(packed);
      return -1;
    }
    free(packed);
    return 0;
  }
  case FMT_FLAT: {
    uint8_t *flat = NULL;
    size_t flen = 0;
    if (rebuild_message(m, 0, 1, &flat, &flen))
      return -1;
    if (fwrite(flat, 1, flen, stdout) != flen) {
      free(flat);
      return -1;
    }
    free(flat);
    return 0;
  }
  case FMT_FLAT_PACKED: {
    uint8_t *flat = NULL, *packed = NULL;
    size_t flen = 0, plen = 0;
    if (rebuild_message(m, 0, 1, &flat, &flen))
      return -1;
    if (capnp_pack(flat, flen, &packed, &plen) != CAPNP_OK) {
      free(flat);
      return -1;
    }
    free(flat);
    if (fwrite(packed, 1, plen, stdout) != plen) {
      free(packed);
      return -1;
    }
    free(packed);
    return 0;
  }
  case FMT_CANONICAL: {
    capnp_message_t mut = *m;
    capnp_ptr_t root;
    uint8_t *can = NULL;
    size_t clen = 0;
    mut.owned = NULL;
    mut.ready = 0;
    if (capnp_root(&mut, &root) != CAPNP_OK ||
        (root.kind != CAPNP_PK_STRUCT && root.kind != CAPNP_PK_NULL))
      return -1;
    if (capnp_canonicalize(m, &can, &clen) != CAPNP_OK)
      return -1;
    if (fwrite(can, 1, clen, stdout) != clen) {
      free(can);
      return -1;
    }
    free(can);
    return 0;
  }
  case FMT_TEXT: {
    capnp_message_t mut = *m;
    capnp_ptr_t root;
    char *text = NULL;
    size_t tlen = 0;
    int rc;
    mut.owned = NULL;
    mut.ready = 0;
    if (!dyn || !type_name || capnp_root(&mut, &root) != CAPNP_OK)
      return -1;
    rc = pretty ? capnp_dyn_encode_text(dyn, type_name, &root, &text, &tlen)
                : capnp_dyn_encode_text_short(dyn, type_name, &root, &text,
                                              &tlen);
    if (rc != CAPNP_OK)
      return -1;
    fwrite(text, 1, tlen, stdout);
    fputc('\n', stdout);
    free(text);
    return 0;
  }
  case FMT_JSON: {
    capnp_message_t mut = *m;
    capnp_ptr_t root;
    char *text = NULL;
    size_t tlen = 0;
    int rc;
    mut.owned = NULL;
    mut.ready = 0;
    if (!dyn || !type_name || capnp_root(&mut, &root) != CAPNP_OK)
      return -1;
    rc = pretty ? capnp_dyn_encode_json(dyn, type_name, &root, &text, &tlen)
                : capnp_dyn_encode_json_short(dyn, type_name, &root, &text,
                                              &tlen);
    if (rc != CAPNP_OK)
      return -1;
    fwrite(text, 1, tlen, stdout);
    fputc('\n', stdout);
    free(text);
    return 0;
  }
  default:
    return -1;
  }
}

/* Compute exact byte size of one stream-framed message at `data` */
static int framed_message_size(const uint8_t *data, size_t avail,
                               size_t *out_size) {
  uint32_t nsegs, i;
  size_t table_bytes, body_words = 0;
  if (avail < 8)
    return -1;
  nsegs = capnp_load_le32(data) + 1u;
  if (nsegs == 0 || nsegs > CAPNP_MAX_SEGMENTS)
    return -1;
  table_bytes = 4u + 4u * (size_t)nsegs;
  if (table_bytes % 8u != 0)
    table_bytes += 4u;
  if (avail < table_bytes)
    return -1;
  for (i = 0; i < nsegs; i++) {
    uint32_t w = capnp_load_le32(data + 4u + 4u * i);
    if (w > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
      return -1;
    body_words += w;
  }
  if (body_words * CAPNP_WORD_BYTES > avail - table_bytes)
    return -1;
  *out_size = table_bytes + body_words * CAPNP_WORD_BYTES;
  return 0;
}

/* Split one top-level parenthesized or brace-delimited message from `s + *pos`
 * like readOneText / readOneJson in capnp.c++. */
static int next_delimited_chunk(const char *s, size_t n, size_t *pos, int json,
                                size_t *start_out, size_t *len_out) {
  char open_c = json ? '{' : '(';
  char close_c = json ? '}' : ')';
  int depth = 0;
  int saw_close = 0;
  int state = 0; /* 0 normal, 1 comment, 2 sq, 3 sq_esc, 4 dq, 5 dq_esc, 6 slash */
  size_t i = *pos;

  while (i < n && isspace((unsigned char)s[i]))
    i++;
  if (i >= n)
    return 0;
  *start_out = i;
  for (; i < n; i++) {
    char c = s[i];
    switch (state) {
    case 6:
      if (c == '/') {
        state = 1;
        break;
      }
      state = 0;
      /* fall through */
    case 0:
      if (c == '#') {
        state = 1;
      } else if (json && c == '/') {
        state = 6;
      } else if (c == '\'') {
        state = 2;
      } else if (c == '"') {
        state = 4;
      } else if (c == open_c) {
        if (depth == 0 && saw_close) {
          *len_out = i - *start_out;
          *pos = i;
          return 1;
        }
        depth++;
      } else if (c == close_c) {
        if (depth > 0 && --depth == 0)
          saw_close = 1;
      }
      break;
    case 1:
      if (c == '\n')
        state = 0;
      break;
    case 2:
      if (c == '\'')
        state = 0;
      else if (c == '\\')
        state = 3;
      break;
    case 3:
      state = 2;
      break;
    case 4:
      if (c == '"')
        state = 0;
      else if (c == '\\')
        state = 5;
      break;
    case 5:
      state = 4;
      break;
    }
  }
  *len_out = n - *start_out;
  *pos = n;
  return 1;
}

static int run_convert(capnp_dyn_t *dyn, const char *type_name,
                       int from_fmt, int to_fmt, int pretty, size_t seg_words) {
  size_t in_len = 0;
  char *in_buf = read_all(stdin, &in_len);
  if (!in_buf)
    return 1;
  if (in_len == 0) {
    free(in_buf);
    return 0;
  }

  if (from_fmt == FMT_BINARY) {
    size_t off = 0;
    while (off < in_len) {
      size_t fsz = 0;
      capnp_message_t m = {0};
      if (framed_message_size((const uint8_t *)in_buf + off, in_len - off,
                              &fsz) ||
          capnp_message_from_flat(&m, (const uint8_t *)in_buf + off, fsz) !=
              CAPNP_OK) {
        free(in_buf);
        return 1;
      }
      if (write_converted_message(dyn, type_name, &m, to_fmt, pretty,
                                  seg_words)) {
        capnp_message_free(&m);
        free(in_buf);
        return 1;
      }
      capnp_message_free(&m);
      off += fsz;
    }
    free(in_buf);
    return 0;
  }

  if (from_fmt == FMT_PACKED) {
    uint8_t *unpacked = NULL;
    size_t ulen = 0, off = 0;
    if (capnp_unpack((const uint8_t *)in_buf, in_len, &unpacked, &ulen) !=
        CAPNP_OK) {
      free(in_buf);
      return 1;
    }
    free(in_buf);
    while (off < ulen) {
      size_t fsz = 0;
      capnp_message_t m = {0};
      if (framed_message_size(unpacked + off, ulen - off, &fsz) ||
          capnp_message_from_flat(&m, unpacked + off, fsz) != CAPNP_OK) {
        free(unpacked);
        return 1;
      }
      if (write_converted_message(dyn, type_name, &m, to_fmt, pretty,
                                  seg_words)) {
        capnp_message_free(&m);
        free(unpacked);
        return 1;
      }
      capnp_message_free(&m);
      off += fsz;
    }
    free(unpacked);
    return 0;
  }

  if (from_fmt == FMT_FLAT || from_fmt == FMT_CANONICAL ||
      from_fmt == FMT_FLAT_PACKED) {
    uint8_t *raw = (uint8_t *)in_buf;
    size_t rlen = in_len;
    capnp_segment_t seg;
    capnp_message_t m = {0};
    if (from_fmt == FMT_FLAT_PACKED) {
      uint8_t *unpacked = NULL;
      size_t ulen = 0;
      if (capnp_unpack((const uint8_t *)in_buf, in_len, &unpacked, &ulen) !=
          CAPNP_OK) {
        free(in_buf);
        return 1;
      }
      free(in_buf);
      raw = unpacked;
      rlen = ulen;
    }
    seg.data = raw;
    seg.words = rlen / CAPNP_WORD_BYTES;
    if (capnp_message_from_segments(&m, &seg, 1) != CAPNP_OK) {
      free(raw);
      return 1;
    }
    if (from_fmt == FMT_CANONICAL) {
      uint8_t *can = NULL;
      size_t clen = 0;
      size_t wbytes = seg.words * CAPNP_WORD_BYTES;
      int canon = seg.words == 1 && capnp_load_le64(raw) == 0;
      if (!canon)
        canon = capnp_canonicalize(&m, &can, &clen) == CAPNP_OK &&
                clen == wbytes && memcmp(can, raw, wbytes) == 0;
      free(can);
      if (!canon) {
        capnp_message_free(&m);
        free(raw);
        return 1;
      }
    }
    if (write_converted_message(dyn, type_name, &m, to_fmt, pretty,
                                seg_words)) {
      capnp_message_free(&m);
      free(raw);
      return 1;
    }
    capnp_message_free(&m);
    free(raw);
    return 0;
  }

  if (from_fmt == FMT_TEXT || from_fmt == FMT_JSON) {
    size_t pos = 0, cstart = 0, clen = 0;
    int is_json = (from_fmt == FMT_JSON);
    if (!dyn || !type_name) {
      free(in_buf);
      return 1;
    }
    while (next_delimited_chunk(in_buf, in_len, &pos, is_json, &cstart,
                                &clen)) {
      uint8_t *msg_bytes = NULL;
      size_t mlen = 0;
      capnp_message_t m = {0};
      int rc = is_json ? capnp_dyn_decode_json(dyn, type_name, in_buf + cstart,
                                               clen, &msg_bytes, &mlen)
                       : capnp_dyn_decode_text(dyn, type_name, in_buf + cstart,
                                               clen, &msg_bytes, &mlen);
      if (rc != CAPNP_OK ||
          capnp_message_from_flat(&m, msg_bytes, mlen) != CAPNP_OK) {
        if (rc != CAPNP_OK && capnp_dyn_error(dyn)[0])
          fprintf(stderr, "capnp-eval: %s\n", capnp_dyn_error(dyn));
        free(msg_bytes);
        free(in_buf);
        return 1;
      }
      free(msg_bytes);
      if (write_converted_message(dyn, type_name, &m, to_fmt, pretty,
                                  seg_words)) {
        capnp_message_free(&m);
        free(in_buf);
        return 1;
      }
      capnp_message_free(&m);
    }
    free(in_buf);
    return 0;
  }

  free(in_buf);
  return 1;
}

int main(int argc, char **argv) {
  const char *import_dirs[32];
  int nimports = 0;
  const char *pos_args[8];
  int npos = 0;
  int pretty = 1;
  int flag_flat = 0, flag_packed = 0, flag_binary = 0;
  int eval_fmt = FMT_TEXT;
  size_t seg_words = 0;
  int i;

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--short") == 0) {
      pretty = 0;
    } else if (strcmp(a, "--flat") == 0) {
      flag_flat = 1;
    } else if (strcmp(a, "-p") == 0 || strcmp(a, "--packed") == 0) {
      flag_packed = 1;
    } else if (strcmp(a, "-b") == 0 || strcmp(a, "--binary") == 0) {
      flag_binary = 1;
    } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0 ||
               strcmp(a, "--no-standard-import") == 0) {
      /* accepted for CLI compatibility */
    } else if (strncmp(a, "-I", 2) == 0) {
      const char *dir = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
      if (!dir || nimports >= 32)
        return 2;
      import_dirs[nimports++] = dir;
    } else if (strncmp(a, "--import-path=", 14) == 0) {
      if (nimports >= 32)
        return 2;
      import_dirs[nimports++] = a + 14;
    } else if (strncmp(a, "-o", 2) == 0) {
      const char *f = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
      if (!f || (eval_fmt = parse_fmt(f)) < 0)
        return 2;
    } else if (strncmp(a, "--output=", 9) == 0) {
      if ((eval_fmt = parse_fmt(a + 9)) < 0)
        return 2;
    } else if (strncmp(a, "--segment-size=", 15) == 0) {
      seg_words = (size_t)strtoul(a + 15, NULL, 0);
    } else if (strcmp(a, "--segment-size") == 0 && i + 1 < argc) {
      seg_words = (size_t)strtoul(argv[++i], NULL, 0);
    } else if (a[0] == '-') {
      fprintf(stderr, "capnp-eval: unknown option %s\n", a);
      return 2;
    } else {
      if (npos >= 8)
        return 2;
      pos_args[npos++] = a;
    }
  }

  if (npos < 1) {
    fprintf(stderr, "usage: capnp-eval [eval|convert|decode|encode] ...\n");
    return 2;
  }

  /* Subcommand `convert <from>:<to> [<schema> <type>]` */
  if (strcmp(pos_args[0], "convert") == 0) {
    const char *spec, *colon;
    char from_s[32];
    int from_fmt, to_fmt;
    capnp_dyn_t *dyn = NULL;
    const char *type_name = NULL;
    int rc;
    if (npos != 2 && npos != 4)
      return 2;
    spec = pos_args[1];
    colon = strchr(spec, ':');
    if (!colon || (size_t)(colon - spec) >= sizeof(from_s))
      return 2;
    memcpy(from_s, spec, (size_t)(colon - spec));
    from_s[colon - spec] = 0;
    from_fmt = parse_fmt(from_s);
    to_fmt = parse_fmt(colon + 1);
    if (from_fmt < 0 || to_fmt < 0)
      return 2;
    if ((from_fmt == FMT_TEXT || from_fmt == FMT_JSON || to_fmt == FMT_TEXT ||
         to_fmt == FMT_JSON) &&
        npos < 4)
      return 2;
    if (npos == 4) {
      if (compile_schema(pos_args[2], import_dirs, nimports, &dyn))
        return 1;
      type_name = pos_args[3];
    }
    rc = run_convert(dyn, type_name, from_fmt, to_fmt, pretty, seg_words);
    capnp_dyn_free(dyn);
    return rc;
  }

  /* Subcommand `decode <schema> <type>` */
  if (strcmp(pos_args[0], "decode") == 0) {
    capnp_dyn_t *dyn = NULL;
    int from_fmt = flag_flat ? (flag_packed ? FMT_FLAT_PACKED : FMT_FLAT)
                             : (flag_packed ? FMT_PACKED : FMT_BINARY);
    int rc;
    if (npos != 3)
      return 2;
    if (compile_schema(pos_args[1], import_dirs, nimports, &dyn))
      return 1;
    rc = run_convert(dyn, pos_args[2], from_fmt, FMT_TEXT, pretty, seg_words);
    capnp_dyn_free(dyn);
    return rc;
  }

  /* Subcommand `encode <schema> <type>` */
  if (strcmp(pos_args[0], "encode") == 0) {
    capnp_dyn_t *dyn = NULL;
    int to_fmt = flag_flat ? (flag_packed ? FMT_FLAT_PACKED : FMT_FLAT)
                           : (flag_packed ? FMT_PACKED : FMT_BINARY);
    int rc;
    if (npos != 3)
      return 2;
    if (compile_schema(pos_args[1], import_dirs, nimports, &dyn))
      return 1;
    rc = run_convert(dyn, pos_args[2], FMT_TEXT, to_fmt, pretty, seg_words);
    capnp_dyn_free(dyn);
    return rc;
  }

  /* Default or `eval`: `capnp-eval [eval] <schema> <name>` */
  {
    const char *schema_path;
    const char *expr;
    capnp_dyn_t *dyn = NULL;
    if (strcmp(pos_args[0], "eval") == 0) {
      if (npos != 3)
        return 2;
      schema_path = pos_args[1];
      expr = pos_args[2];
    } else {
      if (npos != 2)
        return 2;
      schema_path = pos_args[0];
      expr = pos_args[1];
    }
    if (flag_flat)
      eval_fmt = flag_packed ? FMT_FLAT_PACKED : FMT_FLAT;
    else if (flag_packed)
      eval_fmt = FMT_PACKED;
    else if (flag_binary)
      eval_fmt = FMT_BINARY;

    if (compile_schema(schema_path, import_dirs, nimports, &dyn)) {
      fprintf(stderr, "capnp-eval: capnp compile failed\n");
      return 1;
    }
    if (eval_fmt == FMT_TEXT || eval_fmt == FMT_JSON) {
      char *text = NULL;
      size_t text_len = 0;
      if (capnp_dyn_eval_ex(dyn, expr, eval_fmt == FMT_JSON, pretty, &text,
                            &text_len) != CAPNP_OK) {
        fprintf(stderr, "capnp-eval: could not evaluate %s\n", expr);
        capnp_dyn_free(dyn);
        return 1;
      }
      fwrite(text, 1, text_len, stdout);
      fputc('\n', stdout);
      free(text);
      capnp_dyn_free(dyn);
      return 0;
    } else {
      uint8_t *msg_bytes = NULL;
      size_t mlen = 0;
      capnp_message_t m = {0};
      int rc;
      if (capnp_dyn_eval_struct(dyn, expr, &msg_bytes, &mlen) != CAPNP_OK ||
          capnp_message_from_flat(&m, msg_bytes, mlen) != CAPNP_OK) {
        free(msg_bytes);
        capnp_dyn_free(dyn);
        return 1;
      }
      free(msg_bytes);
      rc = write_converted_message(dyn, NULL, &m, eval_fmt, pretty, seg_words);
      capnp_message_free(&m);
      capnp_dyn_free(dyn);
      return rc ? 1 : 0;
    }
  }
}
