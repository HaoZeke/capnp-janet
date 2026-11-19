#include <capnp-janet/capnp_dyn.h>
#include <capnp-janet/capnp_builder.h>

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum {
  TY_VOID = 0,
  TY_BOOL = 1,
  TY_I8 = 2,
  TY_I16 = 3,
  TY_I32 = 4,
  TY_I64 = 5,
  TY_U8 = 6,
  TY_U16 = 7,
  TY_U32 = 8,
  TY_U64 = 9,
  TY_F32 = 10,
  TY_F64 = 11,
  TY_TEXT = 12,
  TY_DATA = 13,
  TY_LIST = 14,
  TY_ENUM = 15,
  TY_STRUCT = 16,
  TY_IFACE = 17,
  TY_ANY = 18
};

typedef struct dyn_field {
  char name[96];
  int is_group;
  uint16_t type_which;
  uint32_t offset;
  uint16_t disc;
  uint64_t def_bits;
  uint64_t type_id;
  uint16_t elem_which;
  uint64_t elem_id;
  uint16_t elem2_which;
  uint64_t elem2_id;
} dyn_field;

typedef struct dyn_struct {
  char name[160];
  uint64_t id;
  uint16_t dwords;
  uint16_t pwords;
  uint16_t disc_count;
  uint32_t disc_byte;
  int is_group;
  uint32_t nfields;
  uint32_t cap;
  dyn_field *fields;
} dyn_struct;

struct capnp_dyn {
  capnp_message_t msg;
  dyn_struct *structs;
  uint32_t nstructs;
  uint32_t scap;
};

typedef struct {
  char *data;
  size_t n;
  size_t cap;
} buf_t;

typedef struct {
  const capnp_dyn_t *d;
  const char *s;
  size_t n;
  size_t i;
  int json;
} ps_t;

static int is_ptr_type(uint16_t w) {
  return w == TY_TEXT || w == TY_DATA || w == TY_LIST || w == TY_STRUCT ||
         w == TY_IFACE || w == TY_ANY;
}

static uint32_t data_byte(uint16_t which, uint32_t offset) {
  switch (which) {
  case TY_I16:
  case TY_U16:
  case TY_ENUM:
    return offset * 2u;
  case TY_I32:
  case TY_U32:
  case TY_F32:
    return offset * 4u;
  case TY_I64:
  case TY_U64:
  case TY_F64:
    return offset * 8u;
  default:
    return offset;
  }
}

static const char *node_text(const capnp_ptr_t *s, uint16_t idx) {
  const char *t = "";
  size_t n = 0;
  if (capnp_get_text(s, idx, &t, &n) != CAPNP_OK || !t)
    return "";
  return t;
}

static void short_name(const char *display, char *out, size_t n) {
  const char *colon = strrchr(display, ':');
  snprintf(out, n, "%s", colon ? colon + 1 : display);
}

static dyn_struct *find_name(const capnp_dyn_t *d, const char *name) {
  uint32_t i;
  for (i = 0; i < d->nstructs; i++) {
    if (strcmp(d->structs[i].name, name) == 0)
      return &d->structs[i];
  }
  return NULL;
}

static dyn_struct *find_id(const capnp_dyn_t *d, uint64_t id) {
  uint32_t i;
  for (i = 0; i < d->nstructs; i++) {
    if (d->structs[i].id == id)
      return &d->structs[i];
  }
  return NULL;
}

static dyn_field *find_field(dyn_struct *st, const char *name) {
  uint32_t i;
  if (!st)
    return NULL;
  for (i = 0; i < st->nfields; i++) {
    if (strcmp(st->fields[i].name, name) == 0)
      return &st->fields[i];
  }
  return NULL;
}

static int add_struct(capnp_dyn_t *d, dyn_struct **out) {
  if (d->nstructs == d->scap) {
    uint32_t ncap = d->scap ? d->scap * 2u : 8u;
    dyn_struct *p =
        (dyn_struct *)realloc(d->structs, (size_t)ncap * sizeof(*p));
    if (!p)
      return CAPNP_ERR_ALLOC;
    d->structs = p;
    d->scap = ncap;
  }
  memset(&d->structs[d->nstructs], 0, sizeof(d->structs[0]));
  *out = &d->structs[d->nstructs++];
  return CAPNP_OK;
}

static int add_field(dyn_struct *st, dyn_field **out) {
  if (st->nfields == st->cap) {
    uint32_t ncap = st->cap ? st->cap * 2u : 8u;
    dyn_field *p = (dyn_field *)realloc(st->fields, (size_t)ncap * sizeof(*p));
    if (!p)
      return CAPNP_ERR_ALLOC;
    st->fields = p;
    st->cap = ncap;
  }
  memset(&st->fields[st->nfields], 0, sizeof(st->fields[0]));
  *out = &st->fields[st->nfields++];
  return CAPNP_OK;
}

static void load_default(const capnp_ptr_t *field, dyn_field *f) {
  capnp_ptr_t value;
  uint16_t which;
  f->def_bits = 0;
  if (capnp_getp(field, 3, &value) != CAPNP_OK || value.kind != CAPNP_PK_STRUCT)
    return;
  which = capnp_get_u16(&value, 0, 0xffff);
  switch (which) {
  case TY_BOOL:
    f->def_bits = (uint64_t)capnp_get_bool(&value, 16, 0);
    break;
  case TY_I8:
  case TY_U8:
    f->def_bits = capnp_get_u8(&value, 2, 0);
    break;
  case TY_I16:
  case TY_U16:
  case TY_ENUM:
    f->def_bits = capnp_get_u16(&value, 2, 0);
    break;
  case TY_I32:
  case TY_U32:
  case TY_F32:
    f->def_bits = capnp_get_u32(&value, 4, 0);
    break;
  case TY_I64:
  case TY_U64:
  case TY_F64:
    f->def_bits = capnp_get_u64(&value, 8, 0);
    break;
  default:
    break;
  }
}

static void load_type(const capnp_ptr_t *field, dyn_field *f) {
  capnp_ptr_t typ, el, inner;
  f->type_which = TY_VOID;
  if (capnp_getp(field, 2, &typ) != CAPNP_OK || typ.kind != CAPNP_PK_STRUCT)
    return;
  f->type_which = capnp_get_u16(&typ, 0, 0xffff);
  if (f->type_which == TY_STRUCT || f->type_which == TY_ENUM ||
      f->type_which == TY_IFACE)
    f->type_id = capnp_get_u64(&typ, 8, 0);
  if (f->type_which == TY_LIST &&
      capnp_getp(&typ, 0, &el) == CAPNP_OK && el.kind == CAPNP_PK_STRUCT) {
    f->elem_which = capnp_get_u16(&el, 0, 0xffff);
    if (f->elem_which == TY_STRUCT || f->elem_which == TY_ENUM)
      f->elem_id = capnp_get_u64(&el, 8, 0);
    if (f->elem_which == TY_LIST && capnp_getp(&el, 0, &inner) == CAPNP_OK &&
        inner.kind == CAPNP_PK_STRUCT) {
      f->elem2_which = capnp_get_u16(&inner, 0, 0xffff);
      if (f->elem2_which == TY_STRUCT || f->elem2_which == TY_ENUM)
        f->elem2_id = capnp_get_u64(&inner, 8, 0);
    }
  }
}

static int load_struct_node(capnp_dyn_t *d, const capnp_ptr_t *node) {
  dyn_struct *st;
  capnp_ptr_t fields;
  uint32_t i, n;
  int rc = add_struct(d, &st);
  if (rc)
    return rc;
  st->id = capnp_get_u64(node, 0, 0);
  short_name(node_text(node, 0), st->name, sizeof(st->name));
  st->dwords = capnp_get_u16(node, 14, 0);
  st->pwords = capnp_get_u16(node, 24, 0);
  st->disc_count = capnp_get_u16(node, 30, 0);
  st->disc_byte = capnp_get_u32(node, 32, 0) * 2u;
  st->is_group = capnp_get_bool(node, 224, 0);
  if (capnp_getp(node, 3, &fields) != CAPNP_OK || fields.kind != CAPNP_PK_LIST)
    return CAPNP_OK;
  n = capnp_list_len(&fields);
  for (i = 0; i < n; i++) {
    capnp_ptr_t f;
    dyn_field *df;
    uint16_t fwhich;
    if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
      continue;
    if (add_field(st, &df))
      return CAPNP_ERR_ALLOC;
    snprintf(df->name, sizeof(df->name), "%s", node_text(&f, 0));
    df->disc = (uint16_t)(capnp_get_u16(&f, 2, 0) ^ 0xffffu);
    fwhich = capnp_get_u16(&f, 8, 0xffff);
    if (fwhich == 1) {
      df->is_group = 1;
      df->type_id = capnp_get_u64(&f, 16, 0);
      df->type_which = TY_STRUCT;
      continue;
    }
    df->offset = capnp_get_u32(&f, 4, 0);
    load_type(&f, df);
    load_default(&f, df);
  }
  return CAPNP_OK;
}

int capnp_dyn_init(capnp_dyn_t **out, const uint8_t *request, size_t len) {
  capnp_dyn_t *d;
  capnp_ptr_t root, nodes;
  uint32_t i, n;
  if (!out || !request)
    return CAPNP_ERR_ARG;
  *out = NULL;
  d = (capnp_dyn_t *)calloc(1, sizeof(*d));
  if (!d)
    return CAPNP_ERR_ALLOC;
  if (capnp_message_from_flat(&d->msg, request, len) != CAPNP_OK ||
      capnp_root(&d->msg, &root) != CAPNP_OK ||
      capnp_getp(&root, 0, &nodes) != CAPNP_OK || nodes.kind != CAPNP_PK_LIST) {
    capnp_dyn_free(d);
    return CAPNP_ERR_FRAMING;
  }
  n = capnp_list_len(&nodes);
  for (i = 0; i < n; i++) {
    capnp_ptr_t node;
    if (capnp_list_getp(&nodes, i, &node) != CAPNP_OK)
      continue;
    if (capnp_get_u16(&node, 12, 0xffff) != 1)
      continue;
    if (load_struct_node(d, &node)) {
      capnp_dyn_free(d);
      return CAPNP_ERR_ALLOC;
    }
  }
  *out = d;
  return CAPNP_OK;
}

void capnp_dyn_free(capnp_dyn_t *d) {
  uint32_t i;
  if (!d)
    return;
  for (i = 0; i < d->nstructs; i++)
    free(d->structs[i].fields);
  free(d->structs);
  capnp_message_free(&d->msg);
  free(d);
}

static int logical_bits(const capnp_ptr_t *s, const dyn_field *f, uint64_t *out) {
  uint32_t b = data_byte(f->type_which, f->offset);
  uint64_t wire = 0;
  switch (f->type_which) {
  case TY_BOOL:
    wire = (uint64_t)capnp_get_bool(s, f->offset, 0);
    break;
  case TY_I8:
  case TY_U8:
    wire = capnp_get_u8(s, b, 0);
    break;
  case TY_I16:
  case TY_U16:
  case TY_ENUM:
    wire = capnp_get_u16(s, b, 0);
    break;
  case TY_I32:
  case TY_U32:
  case TY_F32:
    wire = capnp_get_u32(s, b, 0);
    break;
  case TY_I64:
  case TY_U64:
  case TY_F64:
    wire = capnp_get_u64(s, b, 0);
    break;
  default:
    return CAPNP_ERR_KIND;
  }
  *out = wire ^ f->def_bits;
  return CAPNP_OK;
}

static int64_t sext(uint64_t bits, uint16_t which) {
  switch (which) {
  case TY_I8:
    return (int8_t)bits;
  case TY_I16:
    return (int16_t)bits;
  case TY_I32:
    return (int32_t)bits;
  default:
    return (int64_t)bits;
  }
}

static int lookup(const capnp_dyn_t *d, const char *type_name, const char *field,
                  dyn_struct **st, dyn_field **f) {
  *st = find_name(d, type_name);
  if (!*st)
    return CAPNP_ERR_KIND;
  *f = find_field(*st, field);
  if (!*f)
    return CAPNP_ERR_KIND;
  return CAPNP_OK;
}

int capnp_dyn_get_bool(const capnp_dyn_t *d, const capnp_ptr_t *s,
                       const char *type_name, const char *field, int *out) {
  dyn_struct *st;
  dyn_field *f;
  uint64_t bits;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup(d, type_name, field, &st, &f);
  if (rc)
    return rc;
  rc = logical_bits(s, f, &bits);
  if (rc)
    return rc;
  *out = bits != 0;
  return CAPNP_OK;
}

int capnp_dyn_get_i64(const capnp_dyn_t *d, const capnp_ptr_t *s,
                      const char *type_name, const char *field, int64_t *out) {
  dyn_struct *st;
  dyn_field *f;
  uint64_t bits;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup(d, type_name, field, &st, &f);
  if (rc)
    return rc;
  rc = logical_bits(s, f, &bits);
  if (rc)
    return rc;
  *out = sext(bits, f->type_which);
  return CAPNP_OK;
}

int capnp_dyn_get_text(const capnp_dyn_t *d, const capnp_ptr_t *s,
                       const char *type_name, const char *field, const char **out,
                       size_t *len) {
  dyn_struct *st;
  dyn_field *f;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup(d, type_name, field, &st, &f);
  if (rc)
    return rc;
  if (f->type_which != TY_TEXT)
    return CAPNP_ERR_KIND;
  return capnp_get_text(s, (uint16_t)f->offset, out, len);
}

int capnp_dyn_get_ptr(const capnp_dyn_t *d, const capnp_ptr_t *s,
                      const char *type_name, const char *field, capnp_ptr_t *out) {
  dyn_struct *st;
  dyn_field *f;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup(d, type_name, field, &st, &f);
  if (rc)
    return rc;
  if (!is_ptr_type(f->type_which) || f->is_group)
    return CAPNP_ERR_KIND;
  return capnp_getp(s, (uint16_t)f->offset, out);
}

static int buf_add(buf_t *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t c = b->cap ? b->cap * 2u : 128u;
    char *p;
    while (c < b->n + n + 1)
      c *= 2u;
    p = (char *)realloc(b->data, c);
    if (!p)
      return -1;
    b->data = p;
    b->cap = c;
  }
  if (n)
    memcpy(b->data + b->n, s, n);
  b->n += n;
  b->data[b->n] = 0;
  return 0;
}

static int buf_str(buf_t *b, const char *s) { return buf_add(b, s, strlen(s)); }

static int buf_fmt_u(buf_t *b, unsigned long long v) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%llu", v);
  return buf_str(b, tmp);
}

static int buf_fmt_i(buf_t *b, long long v) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%lld", v);
  return buf_str(b, tmp);
}

static int quote_text(buf_t *b, const char *s, size_t n, int json) {
  size_t i;
  if (buf_str(b, "\""))
    return -1;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    char esc[8];
    if (c == '"' || c == '\\') {
      esc[0] = '\\';
      esc[1] = (char)c;
      if (buf_add(b, esc, 2))
        return -1;
    } else if (c == '\n') {
      if (buf_str(b, "\\n"))
        return -1;
    } else if (c < 0x20) {
      if (json)
        snprintf(esc, sizeof(esc), "\\u%04x", c);
      else
        snprintf(esc, sizeof(esc), "\\x%02x", c);
      if (buf_str(b, esc))
        return -1;
    } else if (buf_add(b, (const char *)&c, 1)) {
      return -1;
    }
  }
  return buf_str(b, "\"");
}

static int struct_which(const capnp_ptr_t *s, const dyn_struct *st, uint16_t *out) {
  if (!st->disc_count) {
    *out = 0xffff;
    return CAPNP_OK;
  }
  *out = capnp_get_u16(s, st->disc_byte, 0);
  return CAPNP_OK;
}

static int encode_struct(buf_t *b, const capnp_dyn_t *d, const dyn_struct *st,
                         const capnp_ptr_t *s, int json, int *any);

static int encode_list(buf_t *b, const capnp_dyn_t *d, const dyn_field *f,
                       const capnp_ptr_t *list, int json) {
  uint32_t i, n;
  int first = 1;
  if (buf_str(b, "["))
    return -1;
  if (!list || list->kind != CAPNP_PK_LIST) {
    return buf_str(b, "]");
  }
  n = capnp_list_len(list);
  for (i = 0; i < n; i++) {
    if (!first && buf_str(b, ", "))
      return -1;
    first = 0;
    if (f->elem_which == TY_STRUCT) {
      capnp_ptr_t el;
      dyn_struct *es = find_id(d, f->elem_id);
      int any = 0;
      if (!es || capnp_list_getp(list, i, &el) != CAPNP_OK)
        return -1;
      if (encode_struct(b, d, es, &el, json, &any))
        return -1;
    } else if (f->elem_which == TY_TEXT) {
      const char *t = "";
      size_t tn = 0;
      if (capnp_list_get_text(list, i, &t, &tn) != CAPNP_OK)
        return -1;
      if (quote_text(b, t, tn, json))
        return -1;
    } else if (f->elem_which == TY_LIST) {
      capnp_ptr_t el;
      dyn_field inner;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return -1;
      memset(&inner, 0, sizeof(inner));
      inner.elem_which = f->elem2_which;
      if (encode_list(b, d, &inner, &el, json))
        return -1;
    } else if (f->elem_which == TY_I32 || f->elem_which == TY_U32 ||
               f->elem_which == TY_ENUM) {
      if (buf_fmt_i(b, (int32_t)capnp_list_get_u32(list, i, 0)))
        return -1;
    } else if (f->elem_which == TY_I64 || f->elem_which == TY_U64) {
      if (buf_fmt_i(b, (int64_t)capnp_list_get_u64(list, i, 0)))
        return -1;
    } else if (f->elem_which == TY_F64) {
      char tmp[64];
      double v = capnp_list_get_f64(list, i, 0);
      snprintf(tmp, sizeof(tmp), "%.17g", v);
      if (buf_str(b, tmp))
        return -1;
    } else if (f->elem_which == TY_F32) {
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "%.9g", (double)capnp_list_get_f32(list, i, 0));
      if (buf_str(b, tmp))
        return -1;
    } else if (f->elem_which == TY_BOOL) {
      if (buf_str(b, capnp_list_get_bool(list, i, 0) ? "true" : "false"))
        return -1;
    } else {
      return -1;
    }
  }
  return buf_str(b, "]");
}

static int encode_data(buf_t *b, const uint8_t *p, size_t n, int json) {
  size_t i;
  if (!json)
    return quote_text(b, (const char *)p, n, json);
  if (buf_str(b, "["))
    return -1;
  for (i = 0; i < n; i++) {
    if (i && buf_str(b, ", "))
      return -1;
    if (buf_fmt_u(b, p[i]))
      return -1;
  }
  return buf_str(b, "]");
}

static int encode_struct(buf_t *b, const capnp_dyn_t *d, const dyn_struct *st,
                         const capnp_ptr_t *s, int json, int *any) {
  uint32_t i;
  uint16_t which = 0xffff;
  int first = 1;
  char open = json ? '{' : '(';
  char close = json ? '}' : ')';
  if (buf_add(b, &open, 1))
    return -1;
  struct_which(s, st, &which);
  for (i = 0; i < st->nfields; i++) {
    dyn_field *f = &st->fields[i];
    size_t mark;
    if (f->disc != 0xffff && st->disc_count && f->disc != which)
      continue;
    mark = b->n;
    if (!first && buf_str(b, ", "))
      return -1;
    if (json) {
      if (quote_text(b, f->name, strlen(f->name), json) || buf_str(b, ": "))
        return -1;
    } else if (buf_str(b, f->name) || buf_str(b, " = ")) {
      return -1;
    }
    if (f->is_group) {
      dyn_struct *g = find_id(d, f->type_id);
      int sub = 0;
      if (!g || encode_struct(b, d, g, s, json, &sub))
        return -1;
      if (!sub) {
        b->n = mark;
        b->data[b->n] = 0;
        continue;
      }
    } else if (f->type_which == TY_TEXT) {
      const char *t = NULL;
      size_t tn = 0;
      capnp_ptr_t p;
      if (capnp_getp(s, (uint16_t)f->offset, &p) != CAPNP_OK ||
          p.kind == CAPNP_PK_NULL) {
        b->n = mark;
        b->data[b->n] = 0;
        continue;
      }
      if (capnp_get_text(s, (uint16_t)f->offset, &t, &tn) != CAPNP_OK ||
          quote_text(b, t, tn, json))
        return -1;
    } else if (f->type_which == TY_DATA) {
      const uint8_t *p = NULL;
      size_t pn = 0;
      capnp_ptr_t q;
      if (capnp_getp(s, (uint16_t)f->offset, &q) != CAPNP_OK ||
          q.kind == CAPNP_PK_NULL) {
        b->n = mark;
        b->data[b->n] = 0;
        continue;
      }
      if (capnp_get_data(s, (uint16_t)f->offset, &p, &pn) != CAPNP_OK ||
          encode_data(b, p, pn, json))
        return -1;
    } else if (f->type_which == TY_STRUCT) {
      capnp_ptr_t child;
      dyn_struct *cs;
      int sub = 0;
      if (capnp_getp(s, (uint16_t)f->offset, &child) != CAPNP_OK ||
          child.kind == CAPNP_PK_NULL) {
        b->n = mark;
        b->data[b->n] = 0;
        continue;
      }
      cs = find_id(d, f->type_id);
      if (!cs || encode_struct(b, d, cs, &child, json, &sub))
        return -1;
    } else if (f->type_which == TY_LIST) {
      capnp_ptr_t list;
      if (capnp_getp(s, (uint16_t)f->offset, &list) != CAPNP_OK ||
          list.kind == CAPNP_PK_NULL) {
        b->n = mark;
        b->data[b->n] = 0;
        continue;
      }
      if (encode_list(b, d, f, &list, json))
        return -1;
    } else if (f->type_which == TY_VOID) {
      if (buf_str(b, json ? "null" : "void"))
        return -1;
    } else if (f->type_which == TY_BOOL) {
      uint64_t bits = 0;
      if (logical_bits(s, f, &bits))
        return -1;
      if (bits == f->def_bits && f->disc == 0xffff) {
        b->n = mark;
        b->data[b->n] = 0;
        continue;
      }
      if (buf_str(b, bits ? "true" : "false"))
        return -1;
    } else if (f->type_which == TY_F32 || f->type_which == TY_F64) {
      uint64_t bits = 0;
      char tmp[64];
      double v;
      if (logical_bits(s, f, &bits))
        return -1;
      if (bits == f->def_bits && f->disc == 0xffff) {
        b->n = mark;
        b->data[b->n] = 0;
        continue;
      }
      if (f->type_which == TY_F32) {
        uint32_t u = (uint32_t)bits;
        float fv;
        memcpy(&fv, &u, sizeof(fv));
        v = fv;
        snprintf(tmp, sizeof(tmp), "%.9g", v);
      } else {
        memcpy(&v, &bits, sizeof(v));
        snprintf(tmp, sizeof(tmp), "%.17g", v);
      }
      if (buf_str(b, tmp))
        return -1;
    } else {
      uint64_t bits = 0;
      if (logical_bits(s, f, &bits))
        return -1;
      if (bits == f->def_bits && f->disc == 0xffff) {
        b->n = mark;
        b->data[b->n] = 0;
        continue;
      }
      if (f->type_which == TY_I8 || f->type_which == TY_I16 ||
          f->type_which == TY_I32 || f->type_which == TY_I64) {
        if (buf_fmt_i(b, sext(bits, f->type_which)))
          return -1;
      } else if (buf_fmt_u(b, bits)) {
        return -1;
      }
    }
    first = 0;
    if (any)
      *any = 1;
  }
  if (buf_add(b, &close, 1))
    return -1;
  return 0;
}

static int encode_root(const capnp_dyn_t *d, const char *type_name,
                       const capnp_ptr_t *s, int json, char **out, size_t *len) {
  dyn_struct *st = find_name(d, type_name);
  buf_t b;
  int any = 0;
  memset(&b, 0, sizeof(b));
  if (!st)
    return CAPNP_ERR_KIND;
  if (encode_struct(&b, d, st, s, json, &any) || !b.data) {
    free(b.data);
    return CAPNP_ERR_ALLOC;
  }
  *out = b.data;
  *len = b.n;
  return CAPNP_OK;
}

int capnp_dyn_encode_text(const capnp_dyn_t *d, const char *type_name,
                          const capnp_ptr_t *s, char **out, size_t *len) {
  if (!d || !s || !out || !len)
    return CAPNP_ERR_ARG;
  return encode_root(d, type_name, s, 0, out, len);
}

int capnp_dyn_encode_json(const capnp_dyn_t *d, const char *type_name,
                          const capnp_ptr_t *s, char **out, size_t *len) {
  if (!d || !s || !out || !len)
    return CAPNP_ERR_ARG;
  return encode_root(d, type_name, s, 1, out, len);
}

static void pskip(ps_t *p) {
  while (p->i < p->n) {
    char c = p->s[p->i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ',') {
      p->i++;
      continue;
    }
    if (!p->json && c == '#') {
      while (p->i < p->n && p->s[p->i] != '\n')
        p->i++;
      continue;
    }
    break;
  }
}

static int ppeek(ps_t *p) {
  pskip(p);
  if (p->i >= p->n)
    return 0;
  return (unsigned char)p->s[p->i];
}

static int ident(ps_t *p, char *out, size_t n) {
  size_t k = 0;
  pskip(p);
  if (p->i >= p->n || !(isalpha((unsigned char)p->s[p->i]) || p->s[p->i] == '_'))
    return -1;
  while (p->i < p->n &&
         (isalnum((unsigned char)p->s[p->i]) || p->s[p->i] == '_')) {
    if (k + 1 < n)
      out[k++] = p->s[p->i];
    p->i++;
  }
  out[k] = 0;
  return 0;
}

static int hex_nibble(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static int buf_add_utf8(buf_t *b, unsigned value) {
  char bytes[3];
  size_t n;
  if (value < 0x80) {
    bytes[0] = (char)value;
    n = 1;
  } else if (value < 0x800) {
    bytes[0] = (char)(0xc0 | (value >> 6));
    bytes[1] = (char)(0x80 | (value & 0x3f));
    n = 2;
  } else {
    bytes[0] = (char)(0xe0 | (value >> 12));
    bytes[1] = (char)(0x80 | ((value >> 6) & 0x3f));
    bytes[2] = (char)(0x80 | (value & 0x3f));
    n = 3;
  }
  return buf_add(b, bytes, n);
}

static int parse_string(ps_t *p, char **out, size_t *len) {
  buf_t b;
  memset(&b, 0, sizeof(b));
  pskip(p);
  if (ppeek(p) != '"')
    return -1;
  p->i++;
  while (p->i < p->n && p->s[p->i] != '"') {
    unsigned char c = (unsigned char)p->s[p->i++];
    if (c == '\\') {
      if (p->i >= p->n)
        goto fail;
      c = (unsigned char)p->s[p->i++];
      if (c == 'n')
        c = '\n';
      else if (c == 't')
        c = '\t';
      else if (c == 'r')
        c = '\r';
      else if (c == 'b')
        c = '\b';
      else if (c == 'f')
        c = '\f';
      else if (c == '"' || c == '\\' || c == '/')
        ;
      else if (c == 'x') {
        int hi, lo;
        if (p->i + 2 > p->n)
          goto fail;
        if (p->json || (hi = hex_nibble((unsigned char)p->s[p->i])) < 0 ||
            (lo = hex_nibble((unsigned char)p->s[p->i + 1])) < 0)
          goto fail;
        p->i += 2;
        c = (unsigned char)((hi << 4) | lo);
      } else if (c == 'u') {
        int a, b4, c4, d;
        unsigned value;
        if (p->i + 4 > p->n ||
            (a = hex_nibble((unsigned char)p->s[p->i])) < 0 ||
            (b4 = hex_nibble((unsigned char)p->s[p->i + 1])) < 0 ||
            (c4 = hex_nibble((unsigned char)p->s[p->i + 2])) < 0 ||
            (d = hex_nibble((unsigned char)p->s[p->i + 3])) < 0)
          goto fail;
        value = (unsigned)((a << 12) | (b4 << 8) | (c4 << 4) | d);
        p->i += 4;
        if (value >= 0xd800 && value <= 0xdfff)
          goto fail;
        if (buf_add_utf8(&b, value))
          goto fail;
        continue;
      } else {
        goto fail;
      }
    }
    if (buf_add(&b, (char *)&c, 1))
      goto fail;
  }
  if (p->i >= p->n || p->s[p->i] != '"')
    goto fail;
  p->i++;
  *out = b.data ? b.data : (char *)calloc(1, 1);
  *len = b.n;
  return *out ? 0 : -1;
fail:
  free(b.data);
  return -1;
}

static int parse_number(ps_t *p, int *is_float, double *fv, int64_t *iv) {
  char tmp[128];
  size_t k = 0;
  int flt = 0;
  pskip(p);
  if (p->i < p->n && (p->s[p->i] == '-' || p->s[p->i] == '+'))
    tmp[k++] = p->s[p->i++];
  if (p->i + 1 < p->n && p->s[p->i] == '0' && (p->s[p->i + 1] == 'x' || p->s[p->i + 1] == 'X')) {
    tmp[k++] = p->s[p->i++];
    tmp[k++] = p->s[p->i++];
    while (p->i < p->n && isxdigit((unsigned char)p->s[p->i]) && k + 1 < sizeof(tmp))
      tmp[k++] = p->s[p->i++];
    tmp[k] = 0;
    *iv = (int64_t)strtoll(tmp, NULL, 0);
    *is_float = 0;
    return 0;
  }
  while (p->i < p->n && k + 1 < sizeof(tmp)) {
    char c = p->s[p->i];
    if (isdigit((unsigned char)c) || c == '.' || c == 'e' || c == 'E' || c == '+' ||
        c == '-') {
      if (c == '.' || c == 'e' || c == 'E')
        flt = 1;
      tmp[k++] = c;
      p->i++;
      continue;
    }
    break;
  }
  tmp[k] = 0;
  if (!k)
    return -1;
  if (flt) {
    *fv = strtod(tmp, NULL);
    *is_float = 1;
  } else {
    *iv = strtoll(tmp, NULL, 10);
    *is_float = 0;
  }
  return 0;
}

static uint64_t narrow_signed(int64_t v, uint16_t which) {
  switch (which) {
  case TY_I8:
  case TY_U8:
    return (uint8_t)v;
  case TY_I16:
  case TY_U16:
  case TY_ENUM:
    return (uint16_t)v;
  case TY_I32:
  case TY_U32:
    return (uint32_t)v;
  default:
    return (uint64_t)v;
  }
}

static int store_bits(const capnp_bptr_t *body, const dyn_field *f, uint64_t logical) {
  uint64_t wire = logical ^ f->def_bits;
  uint32_t b = data_byte(f->type_which, f->offset);
  switch (f->type_which) {
  case TY_BOOL:
    return capnp_builder_set_bool(body, f->offset, wire & 1u);
  case TY_I8:
  case TY_U8:
    return capnp_builder_set_u8(body, b, (uint8_t)wire);
  case TY_I16:
  case TY_U16:
  case TY_ENUM:
    return capnp_builder_set_u16(body, b, (uint16_t)wire);
  case TY_I32:
  case TY_U32:
  case TY_F32:
    return capnp_builder_set_u32(body, b, (uint32_t)wire);
  case TY_I64:
  case TY_U64:
  case TY_F64:
    return capnp_builder_set_u64(body, b, wire);
  default:
    return CAPNP_ERR_KIND;
  }
}

static int parse_value(ps_t *p, const dyn_struct *st, const dyn_field *f,
                       const capnp_bptr_t *body);

static int parse_struct_body(ps_t *p, const dyn_struct *st, const capnp_bptr_t *body,
                             char endc) {
  pskip(p);
  if (ppeek(p) == endc) {
    p->i++;
    return 0;
  }
  while (p->i < p->n) {
    char name[96];
    dyn_field *f;
    int c;
    if (p->json) {
      char *key = NULL;
      size_t kn = 0;
      if (parse_string(p, &key, &kn))
        return -1;
      snprintf(name, sizeof(name), "%.*s", (int)kn, key ? key : "");
      free(key);
      pskip(p);
      if (ppeek(p) != ':')
        return -1;
      p->i++;
    } else if (ident(p, name, sizeof(name))) {
      return -1;
    } else {
      pskip(p);
      if (ppeek(p) != '=')
        return -1;
      p->i++;
    }
    f = find_field((dyn_struct *)st, name);
    if (!f)
      return -1;
    if (f->disc != 0xffff) {
      if (capnp_builder_set_u16(body, st->disc_byte, f->disc))
        return -1;
    }
    if (parse_value(p, st, f, body))
      return -1;
    c = ppeek(p);
    if (c == endc) {
      p->i++;
      return 0;
    }
    if (c == 0)
      return -1;
  }
  return -1;
}

static int parse_list(ps_t *p, const dyn_field *f, const capnp_bptr_t *body,
                      uint16_t dwords, uint16_t index) {
  uint32_t count = 0, cap = 0, i;
  char **texts = NULL;
  size_t *tlens = NULL;
  int64_t *nums = NULL;
  capnp_bptr_t slot, first;
  pskip(p);
  if (ppeek(p) != '[')
    return -1;
  p->i++;
  if (ppeek(p) == ']') {
    p->i++;
    switch (f->elem_which) {
    case TY_VOID:
      return capnp_builder_set_list_void(body, dwords, index, 0);
    case TY_BOOL:
      return capnp_builder_set_list_bool(body, dwords, index, NULL, 0);
    case TY_I8:
    case TY_U8:
      return capnp_builder_set_list_u8(body, dwords, index, NULL, 0);
    case TY_I16:
    case TY_U16:
    case TY_ENUM:
      return capnp_builder_set_list_u16(body, dwords, index, NULL, 0);
    case TY_I32:
    case TY_U32:
    case TY_F32:
      return capnp_builder_set_list_u32(body, dwords, index, NULL, 0);
    case TY_I64:
    case TY_U64:
    case TY_F64:
      return capnp_builder_set_list_u64(body, dwords, index, NULL, 0);
    case TY_TEXT:
      return capnp_builder_set_list_text(body, dwords, index, NULL, 0);
    case TY_STRUCT: {
      dyn_struct *es = find_id(p->d, f->elem_id);
      if (!es)
        return -1;
      return capnp_builder_set_list_struct(body, dwords, index, 0, es->dwords,
                                           es->pwords, NULL);
    }
    default:
      return capnp_builder_init_ptr_list(body, dwords, index, 0, NULL);
    }
  }
  if (f->elem_which == TY_STRUCT) {
    dyn_struct *es = find_id(p->d, f->elem_id);
    /* Count by scanning is hard; parse into a temporary list of source spans.
     * Two-pass: record start offsets of each element, then build. */
    size_t starts[64];
    uint32_t n = 0;
    if (!es)
      return -1;
    while (ppeek(p) && ppeek(p) != ']' && n < 64) {
      starts[n++] = p->i;
      if (p->json) {
        if (ppeek(p) != '{')
          return -1;
      } else if (ppeek(p) != '(') {
        return -1;
      }
      /* skip one balanced value to count */
      {
        int depth = 0;
        char o = p->json ? '{' : '(';
        char c = p->json ? '}' : ')';
        do {
          char ch = p->s[p->i++];
          if (ch == '"') {
            while (p->i < p->n && p->s[p->i] != '"') {
              if (p->s[p->i] == '\\')
                p->i++;
              p->i++;
            }
            if (p->i < p->n)
              p->i++;
            continue;
          }
          if (ch == o)
            depth++;
          else if (ch == c)
            depth--;
        } while (p->i < p->n && depth > 0);
      }
      pskip(p);
    }
    if (ppeek(p) != ']')
      return -1;
    p->i++;
    if (capnp_builder_set_list_struct(body, dwords, index, n, es->dwords,
                                      es->pwords, &first))
      return -1;
    for (i = 0; i < n; i++) {
      ps_t sub = *p;
      capnp_bptr_t el = capnp_bptr_add(first, (size_t)i * (es->dwords + es->pwords));
      char endc = p->json ? '}' : ')';
      sub.i = starts[i];
      if (ppeek(&sub) != (p->json ? '{' : '('))
        return -1;
      sub.i++;
      if (parse_struct_body(&sub, es, &el, endc))
        return -1;
    }
    return 0;
  }
  if (f->elem_which == TY_TEXT || f->elem_which == TY_I32 || f->elem_which == TY_U32 ||
      f->elem_which == TY_I64 || f->elem_which == TY_U64 || f->elem_which == TY_LIST) {
    while (ppeek(p) && ppeek(p) != ']') {
      if (count == cap) {
        uint32_t nc = cap ? cap * 2u : 4u;
        char **nt = (char **)realloc(texts, (size_t)nc * sizeof(*nt));
        size_t *nl = (size_t *)realloc(tlens, (size_t)nc * sizeof(*nl));
        int64_t *nn = (int64_t *)realloc(nums, (size_t)nc * sizeof(*nn));
        if (nt)
          texts = nt;
        if (nl)
          tlens = nl;
        if (nn)
          nums = nn;
        if (!nt || !nl || !nn)
          goto list_fail;
        cap = nc;
      }
      if (f->elem_which == TY_TEXT) {
        if (parse_string(p, &texts[count], &tlens[count]))
          goto list_fail;
      } else if (f->elem_which == TY_LIST) {
        /* Keep the raw slice start; parsed after the pointer list exists. */
        texts[count] = (char *)(uintptr_t)p->i;
        if (ppeek(p) != '[')
          goto list_fail;
        {
          int depth = 0;
          do {
            char ch = p->s[p->i++];
            if (ch == '"') {
              while (p->i < p->n && p->s[p->i] != '"') {
                if (p->s[p->i] == '\\')
                  p->i++;
                p->i++;
              }
              if (p->i < p->n)
                p->i++;
              continue;
            }
            if (ch == '[')
              depth++;
            else if (ch == ']')
              depth--;
          } while (p->i < p->n && depth > 0);
        }
      } else {
        int flt = 0;
        double fv = 0;
        if (parse_number(p, &flt, &fv, &nums[count]))
          goto list_fail;
      }
      count++;
      pskip(p);
    }
    if (ppeek(p) != ']')
      goto list_fail;
    p->i++;
    if (f->elem_which == TY_TEXT) {
      int rc = capnp_builder_set_list_text_n(
          body, dwords, index, (const char *const *)texts, tlens, count);
      for (i = 0; i < count; i++)
        free(texts[i]);
      free(texts);
      free(tlens);
      free(nums);
      return rc;
    }
    if (f->elem_which == TY_LIST) {
      dyn_field inner;
      if (capnp_builder_init_ptr_list(body, dwords, index, count, &first))
        goto list_fail;
      memset(&inner, 0, sizeof(inner));
      inner.type_which = TY_LIST;
      inner.elem_which = f->elem2_which;
      for (i = 0; i < count; i++) {
        ps_t sub = *p;
        capnp_bptr_t el = capnp_bptr_add(first, i);
        sub.i = (size_t)(uintptr_t)texts[i];
        if (parse_list(&sub, &inner, &el, 0, 0))
          goto list_fail;
      }
      free(texts);
      free(tlens);
      free(nums);
      return 0;
    }
    {
      int rc;
      if (f->elem_which == TY_I32 || f->elem_which == TY_U32) {
        uint32_t *raw = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
        if (!raw)
          goto list_fail;
        for (i = 0; i < count; i++)
          raw[i] = (uint32_t)nums[i];
        rc = capnp_builder_set_list_u32(body, dwords, index, raw, count);
        free(raw);
      } else {
        uint64_t *raw = (uint64_t *)malloc((size_t)count * sizeof(uint64_t));
        if (!raw)
          goto list_fail;
        for (i = 0; i < count; i++)
          raw[i] = (uint64_t)nums[i];
        rc = capnp_builder_set_list_u64(body, dwords, index, raw, count);
        free(raw);
      }
      free(texts);
      free(tlens);
      free(nums);
      return rc;
    }
  list_fail:
    if (f->elem_which == TY_TEXT) {
      for (i = 0; i < count; i++)
        free(texts[i]);
    }
    free(texts);
    free(tlens);
    free(nums);
    return -1;
  }
  (void)slot;
  return -1;
}

static int parse_value(ps_t *p, const dyn_struct *st, const dyn_field *f,
                       const capnp_bptr_t *body) {
  int c;
  pskip(p);
  c = ppeek(p);
  if (f->is_group) {
    dyn_struct *g = find_id(p->d, f->type_id);
    char endc = p->json ? '}' : ')';
    char open = p->json ? '{' : '(';
    if (!g || c != open)
      return -1;
    p->i++;
    return parse_struct_body(p, g, body, endc);
  }
  if (f->type_which == TY_TEXT || (!p->json && f->type_which == TY_DATA && c == '"')) {
    char *str = NULL;
    size_t n = 0;
    int rc;
    if (parse_string(p, &str, &n))
      return -1;
    if (f->type_which == TY_DATA)
      rc = capnp_builder_set_data(body, st->dwords, (uint16_t)f->offset,
                                  (const uint8_t *)str, n);
    else
      rc = capnp_builder_set_text(body, st->dwords, (uint16_t)f->offset, str, n);
    free(str);
    return rc;
  }
  if (f->type_which == TY_DATA && c == '[') {
    uint8_t *bytes = NULL;
    size_t n = 0, cap = 0;
    p->i++;
    while (ppeek(p) && ppeek(p) != ']') {
      int flt = 0;
      double fv = 0;
      int64_t iv = 0;
      if (parse_number(p, &flt, &fv, &iv)) {
        free(bytes);
        return -1;
      }
      if (n == cap) {
        size_t nc = cap ? cap * 2u : 8u;
        uint8_t *nb = (uint8_t *)realloc(bytes, nc);
        if (!nb) {
          free(bytes);
          return -1;
        }
        bytes = nb;
        cap = nc;
      }
      bytes[n++] = (uint8_t)iv;
      pskip(p);
    }
    if (ppeek(p) != ']') {
      free(bytes);
      return -1;
    }
    p->i++;
    {
      int rc = capnp_builder_set_data(body, st->dwords, (uint16_t)f->offset, bytes, n);
      free(bytes);
      return rc;
    }
  }
  if (f->type_which == TY_STRUCT && (c == '(' || c == '{')) {
    dyn_struct *cs = find_id(p->d, f->type_id);
    capnp_bptr_t slot, child;
    char endc = c == '{' ? '}' : ')';
    if (!cs)
      return -1;
    p->i++;
    if (capnp_builder_slot(body, st->dwords, (uint16_t)f->offset, &slot) ||
        capnp_builder_struct(&slot, cs->dwords, cs->pwords, &child))
      return -1;
    return parse_struct_body(p, cs, &child, endc);
  }
  if (f->type_which == TY_LIST && c == '[')
    return parse_list(p, f, body, st->dwords, (uint16_t)f->offset);
  if (f->type_which == TY_VOID) {
    char word[16];
    if (p->json) {
      if (ident(p, word, sizeof(word)) || strcmp(word, "null") != 0)
        return -1;
    } else if (ident(p, word, sizeof(word)) || strcmp(word, "void") != 0) {
      return -1;
    }
    return 0;
  }
  if (f->type_which == TY_BOOL) {
    char word[16];
    if (ident(p, word, sizeof(word)))
      return -1;
    if (strcmp(word, "true") == 0)
      return store_bits(body, f, 1);
    if (strcmp(word, "false") == 0)
      return store_bits(body, f, 0);
    return -1;
  }
  if (f->type_which == TY_F32 || f->type_which == TY_F64) {
    int flt = 0;
    double fv = 0;
    int64_t iv = 0;
    uint64_t bits = 0;
    if (parse_number(p, &flt, &fv, &iv))
      return -1;
    if (!flt)
      fv = (double)iv;
    if (f->type_which == TY_F32) {
      float x = (float)fv;
      uint32_t u;
      memcpy(&u, &x, sizeof(u));
      bits = u;
    } else {
      memcpy(&bits, &fv, sizeof(bits));
    }
    return store_bits(body, f, bits);
  }
  {
    int flt = 0;
    double fv = 0;
    int64_t iv = 0;
    if (parse_number(p, &flt, &fv, &iv))
      return -1;
    if (flt)
      iv = (int64_t)fv;
    return store_bits(body, f, narrow_signed(iv, f->type_which));
  }
}

static int decode_root(const capnp_dyn_t *d, const char *type_name, const char *text,
                       size_t len, int json, uint8_t **out, size_t *out_len) {
  dyn_struct *st = find_name(d, type_name);
  capnp_builder_t b;
  capnp_bptr_t root, body;
  ps_t p;
  char open, endc;
  int rc;
  if (!st)
    return CAPNP_ERR_KIND;
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) ||
      capnp_builder_struct(&root, st->dwords, st->pwords, &body)) {
    capnp_builder_free(&b);
    return CAPNP_ERR_ALLOC;
  }
  memset(&p, 0, sizeof(p));
  p.d = d;
  p.s = text;
  p.n = len;
  p.json = json;
  open = json ? '{' : '(';
  endc = json ? '}' : ')';
  if (ppeek(&p) != open) {
    capnp_builder_free(&b);
    return CAPNP_ERR_KIND;
  }
  p.i++;
  if (parse_struct_body(&p, st, &body, endc)) {
    capnp_builder_free(&b);
    return CAPNP_ERR_KIND;
  }
  rc = capnp_builder_serialize(&b, out, out_len);
  capnp_builder_free(&b);
  return rc;
}

int capnp_dyn_decode_text(const capnp_dyn_t *d, const char *type_name,
                          const char *text, size_t len, uint8_t **out,
                          size_t *out_len) {
  if (!d || !text || !out || !out_len)
    return CAPNP_ERR_ARG;
  return decode_root(d, type_name, text, len, 0, out, out_len);
}

int capnp_dyn_decode_json(const capnp_dyn_t *d, const char *type_name,
                          const char *text, size_t len, uint8_t **out,
                          size_t *out_len) {
  if (!d || !text || !out || !out_len)
    return CAPNP_ERR_ARG;
  return decode_root(d, type_name, text, len, 1, out, out_len);
}

enum { EV_BITS = 1, EV_PTR = 2 };

typedef struct {
  int kind;
  uint16_t which;
  uint16_t elem_which;
  uint64_t elem_id;
  uint64_t bits;
  capnp_ptr_t ptr;
  capnp_ptr_t schema_type;
  int has_type;
  char tname[160];
} ev_t;

static int set_out(char **out, size_t *len, const char *s) {
  size_t n = strlen(s);
  char *p = (char *)malloc(n + 1);
  if (!p)
    return CAPNP_ERR_ALLOC;
  memcpy(p, s, n + 1);
  *out = p;
  *len = n;
  return CAPNP_OK;
}

static int eval_quote(const char *s, size_t n, char **out, size_t *len) {
  char *p = (char *)malloc(n * 4 + 3);
  size_t i, w = 0;
  if (!p)
    return CAPNP_ERR_ALLOC;
  p[w++] = '"';
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      p[w++] = '\\';
      p[w++] = (char)c;
    } else if (c == '\n') {
      p[w++] = '\\';
      p[w++] = 'n';
    } else if (c < 0x20 || c == 0x7f) {
      w += (size_t)sprintf(p + w, "\\x%02x", c);
    } else {
      p[w++] = (char)c;
    }
  }
  p[w++] = '"';
  p[w] = 0;
  *out = p;
  *len = w;
  return CAPNP_OK;
}

static int find_schema_node(const capnp_dyn_t *d, const char *name, int which,
                            capnp_ptr_t *node) {
  capnp_ptr_t root, nodes;
  uint32_t i, n;
  if (capnp_root((capnp_message_t *)&d->msg, &root) != CAPNP_OK ||
      capnp_getp(&root, 0, &nodes) != CAPNP_OK || nodes.kind != CAPNP_PK_LIST)
    return CAPNP_ERR_KIND;
  n = capnp_list_len(&nodes);
  for (i = 0; i < n; i++) {
    char shortn[160];
    if (capnp_list_getp(&nodes, i, node) != CAPNP_OK)
      continue;
    if (capnp_get_u16(node, 12, 0xffff) != (uint16_t)which)
      continue;
    short_name(node_text(node, 0), shortn, sizeof(shortn));
    if (strcmp(shortn, name) == 0)
      return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

static int field_node(const capnp_ptr_t *struct_node, const char *name,
                      capnp_ptr_t *field) {
  capnp_ptr_t fields;
  uint32_t i, n;
  if (capnp_getp(struct_node, 3, &fields) != CAPNP_OK ||
      fields.kind != CAPNP_PK_LIST)
    return CAPNP_ERR_KIND;
  n = capnp_list_len(&fields);
  for (i = 0; i < n; i++) {
    if (capnp_list_getp(&fields, i, field) != CAPNP_OK)
      continue;
    if (strcmp(node_text(field, 0), name) == 0)
      return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

static int ev_from_value(const capnp_dyn_t *d, const capnp_ptr_t *value,
                         const capnp_ptr_t *type, ev_t *ev) {
  uint16_t which;
  memset(ev, 0, sizeof(*ev));
  if (!value || value->kind != CAPNP_PK_STRUCT)
    return CAPNP_ERR_KIND;
  which = capnp_get_u16(value, 0, 0xffff);
  ev->which = which;
  if (which == TY_TEXT || which == TY_DATA) {
    ev->kind = EV_PTR;
    ev->ptr = *value;
    return CAPNP_OK;
  }
  if (which == TY_STRUCT || which == TY_LIST) {
    if (capnp_getp(value, 0, &ev->ptr) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    ev->kind = EV_PTR;
    if (which == TY_STRUCT && type && type->kind == CAPNP_PK_STRUCT) {
      dyn_struct *st = find_id(d, capnp_get_u64(type, 8, 0));
      if (st)
        snprintf(ev->tname, sizeof(ev->tname), "%s", st->name);
    }
    if (which == TY_LIST && type && type->kind == CAPNP_PK_STRUCT) {
      capnp_ptr_t el;
      if (capnp_getp(type, 0, &el) == CAPNP_OK && el.kind == CAPNP_PK_STRUCT)
        ev->elem_which = capnp_get_u16(&el, 0, 0xffff);
    }
    return CAPNP_OK;
  }
  ev->kind = EV_BITS;
  switch (which) {
  case TY_BOOL:
    ev->bits = (uint64_t)capnp_get_bool(value, 16, 0);
    break;
  case TY_I8:
  case TY_U8:
    ev->bits = capnp_get_u8(value, 2, 0);
    break;
  case TY_I16:
  case TY_U16:
  case TY_ENUM:
    ev->bits = capnp_get_u16(value, 2, 0);
    break;
  case TY_I32:
  case TY_U32:
  case TY_F32:
    ev->bits = capnp_get_u32(value, 4, 0);
    break;
  case TY_I64:
  case TY_U64:
  case TY_F64:
    ev->bits = capnp_get_u64(value, 8, 0);
    break;
  default:
    return CAPNP_ERR_KIND;
  }
  return CAPNP_OK;
}

static int union_inactive(const capnp_dyn_t *d, const ev_t *ev, const dyn_struct *st,
                          const dyn_field *f) {
  uint16_t which = 0xffff;
  (void)d;
  if (f->disc == 0xffff || !st->disc_count)
    return 0;
  if (struct_which(&ev->ptr, st, &which))
    return 0;
  return f->disc != which;
}

static int ev_field(const capnp_dyn_t *d, ev_t *ev, const char *name) {
  dyn_struct *st;
  dyn_field *f;
  capnp_ptr_t snode, fnode, typ;
  if (ev->kind != EV_PTR || ev->which != TY_STRUCT)
    return CAPNP_ERR_KIND;
  st = find_name(d, ev->tname);
  f = find_field(st, name);
  if (!f)
    return CAPNP_ERR_KIND;
  if (f->is_group) {
    dyn_struct *g = find_id(d, f->type_id);
    if (!g)
      return CAPNP_ERR_KIND;
    snprintf(ev->tname, sizeof(ev->tname), "%s", g->name);
    ev->has_type = 0;
    return CAPNP_OK;
  }
  memset(&typ, 0, sizeof(typ));
  if (find_schema_node(d, ev->tname, 1, &snode) == CAPNP_OK &&
      field_node(&snode, name, &fnode) == CAPNP_OK &&
      capnp_getp(&fnode, 2, &typ) == CAPNP_OK && typ.kind == CAPNP_PK_STRUCT) {
    ev->schema_type = typ;
    ev->has_type = 1;
  } else {
    ev->has_type = 0;
  }
  if (union_inactive(d, ev, st, f))
    return CAPNP_ERR_KIND;
  if (!is_ptr_type(f->type_which)) {
    if (logical_bits(&ev->ptr, f, &ev->bits))
      return CAPNP_ERR_KIND;
    ev->kind = EV_BITS;
    ev->which = f->type_which;
    return CAPNP_OK;
  }
  if (capnp_getp(&ev->ptr, (uint16_t)f->offset, &ev->ptr) != CAPNP_OK &&
      f->type_which != TY_TEXT)
    return CAPNP_ERR_KIND;
  ev->which = f->type_which;
  ev->elem_which = f->elem_which;
  ev->elem_id = f->elem_id;
  ev->tname[0] = 0;
  if (f->type_which == TY_STRUCT) {
    dyn_struct *child = find_id(d, f->type_id);
    if (child)
      snprintf(ev->tname, sizeof(ev->tname), "%s", child->name);
  }
  return CAPNP_OK;
}

static uint16_t element_which(const ev_t *ev) {
  capnp_ptr_t el;
  if (ev->has_type && ev->schema_type.kind == CAPNP_PK_STRUCT &&
      capnp_getp(&ev->schema_type, 0, &el) == CAPNP_OK &&
      el.kind == CAPNP_PK_STRUCT)
    return capnp_get_u16(&el, 0, 0xffff);
  return ev->elem_which;
}

static int ev_index(const capnp_dyn_t *d, ev_t *ev, uint32_t index) {
  uint16_t ew;
  capnp_ptr_t eltype, el;
  uint64_t bits = 0;
  if (ev->kind != EV_PTR || ev->which != TY_LIST)
    return CAPNP_ERR_KIND;
  ew = element_which(ev);
  memset(&eltype, 0, sizeof(eltype));
  if (ev->has_type && ev->schema_type.kind == CAPNP_PK_STRUCT)
    capnp_getp(&ev->schema_type, 0, &eltype);
  if (ew == TY_TEXT || ew == TY_DATA) {
    ev->which = ew;
    ev->elem_which = 1;
    ev->bits = index;
    ev->has_type = 0;
    return CAPNP_OK;
  }
  if (ew == TY_BOOL || ew == TY_I8 || ew == TY_U8 || ew == TY_I16 || ew == TY_U16 ||
      ew == TY_I32 || ew == TY_U32 || ew == TY_I64 || ew == TY_U64 || ew == TY_F32 ||
      ew == TY_F64 || ew == TY_ENUM) {
    switch (ew) {
    case TY_BOOL:
      bits = (uint64_t)capnp_list_get_bool(&ev->ptr, index, 0);
      break;
    case TY_I8:
    case TY_U8:
      bits = capnp_list_get_u8(&ev->ptr, index, 0);
      break;
    case TY_I16:
    case TY_U16:
    case TY_ENUM:
      bits = capnp_list_get_u16(&ev->ptr, index, 0);
      break;
    case TY_I32:
    case TY_U32:
    case TY_F32:
      bits = capnp_list_get_u32(&ev->ptr, index, 0);
      break;
    default:
      bits = capnp_list_get_u64(&ev->ptr, index, 0);
      break;
    }
    ev->kind = EV_BITS;
    ev->which = ew;
    ev->bits = bits;
    ev->has_type = 0;
    return CAPNP_OK;
  }
  if (capnp_list_getp(&ev->ptr, index, &el) != CAPNP_OK)
    return CAPNP_ERR_KIND;
  ev->ptr = el;
  ev->which = ew ? ew : TY_STRUCT;
  ev->has_type = eltype.kind == CAPNP_PK_STRUCT;
  if (ev->has_type)
    ev->schema_type = eltype;
  ev->tname[0] = 0;
  if (ev->which == TY_STRUCT) {
    uint64_t id = ev->elem_id;
    dyn_struct *st;
    if (eltype.kind == CAPNP_PK_STRUCT)
      id = capnp_get_u64(&eltype, 8, 0);
    st = find_id(d, id);
    if (st)
      snprintf(ev->tname, sizeof(ev->tname), "%s", st->name);
  }
  return CAPNP_OK;
}

static int format_ev(const capnp_dyn_t *d, const ev_t *ev, char **out,
                     size_t *len) {
  char tmp[64];
  if (ev->kind == EV_BITS) {
    if (ev->which == TY_BOOL)
      return set_out(out, len, ev->bits ? "true" : "false");
    if (ev->which == TY_I8 || ev->which == TY_I16 || ev->which == TY_I32 ||
        ev->which == TY_I64)
      snprintf(tmp, sizeof(tmp), "%lld", (long long)sext(ev->bits, ev->which));
    else if (ev->which == TY_F32) {
      float fv;
      uint32_t u = (uint32_t)ev->bits;
      memcpy(&fv, &u, sizeof(fv));
      snprintf(tmp, sizeof(tmp), "%.9g", (double)fv);
    } else if (ev->which == TY_F64) {
      double dv;
      memcpy(&dv, &ev->bits, sizeof(dv));
      snprintf(tmp, sizeof(tmp), "%.17g", dv);
    } else
      snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)ev->bits);
    return set_out(out, len, tmp);
  }
  if (ev->which == TY_TEXT && ev->elem_which == 1) {
    const char *t = NULL;
    size_t n = 0;
    if (capnp_list_get_text(&ev->ptr, (uint32_t)ev->bits, &t, &n) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    return eval_quote(t, n, out, len);
  }
  if (ev->which == TY_TEXT) {
    const char *t = NULL;
    size_t n = 0;
    if (ev->ptr.kind == CAPNP_PK_NULL)
      return eval_quote("", 0, out, len);
    if (ev->ptr.kind == CAPNP_PK_LIST && ev->ptr.esize == CAPNP_SZ_BYTE &&
        ev->ptr.count > 0 && ev->ptr.msg && ev->ptr.seg < ev->ptr.msg->nsegs) {
      const uint8_t *p = ev->ptr.msg->segs[ev->ptr.seg].data +
                         ev->ptr.word * 8;
      if (p[ev->ptr.count - 1] != 0)
        return CAPNP_ERR_KIND;
      return eval_quote((const char *)p, ev->ptr.count - 1, out, len);
    }
    if (capnp_get_text(&ev->ptr, 0, &t, &n) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    return eval_quote(t, n, out, len);
  }
  if (ev->which == TY_STRUCT)
    return capnp_dyn_encode_text(d, ev->tname, &ev->ptr, out, len);
  if (ev->which == TY_LIST &&
      (ev->elem_which == TY_TEXT || ev->elem_which == 0)) {
    uint32_t i, n = capnp_list_len(&ev->ptr);
    char *buf = (char *)malloc((size_t)n * 64 + 3);
    size_t w = 0;
    if (!buf)
      return CAPNP_ERR_ALLOC;
    buf[w++] = '[';
    for (i = 0; i < n; i++) {
      const char *t = NULL;
      size_t tn = 0;
      char *q = NULL;
      size_t qn = 0;
      if (i) {
        buf[w++] = ',';
        buf[w++] = ' ';
      }
      if (capnp_list_get_text(&ev->ptr, i, &t, &tn) != CAPNP_OK) {
        free(buf);
        return CAPNP_ERR_KIND;
      }
      if (eval_quote(t, tn, &q, &qn)) {
        free(buf);
        return CAPNP_ERR_ALLOC;
      }
      memcpy(buf + w, q, qn);
      w += qn;
      free(q);
    }
    buf[w++] = ']';
    buf[w] = 0;
    *out = buf;
    *len = w;
    return CAPNP_OK;
  }
  if (ev->which == TY_LIST && ev->elem_which == TY_STRUCT && ev->elem_id) {
    dyn_struct *st = find_id(d, ev->elem_id);
    uint32_t i, n = capnp_list_len(&ev->ptr);
    char *buf;
    size_t w = 0, cap;
    if (!st)
      return CAPNP_ERR_KIND;
    cap = (size_t)n * 128 + 3;
    buf = (char *)malloc(cap);
    if (!buf)
      return CAPNP_ERR_ALLOC;
    buf[w++] = '[';
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      char *piece = NULL;
      size_t pn = 0;
      if (capnp_list_getp(&ev->ptr, i, &el) ||
          capnp_dyn_encode_text(d, st->name, &el, &piece, &pn)) {
        free(buf);
        return CAPNP_ERR_KIND;
      }
      if (w + pn + 3 >= cap) {
        cap = (w + pn + 3) * 2;
        char *grown = (char *)realloc(buf, cap);
        if (!grown) {
          free(piece);
          free(buf);
          return CAPNP_ERR_ALLOC;
        }
        buf = grown;
      }
      if (i) {
        buf[w++] = ',';
        buf[w++] = ' ';
      }
      memcpy(buf + w, piece, pn);
      w += pn;
      free(piece);
    }
    buf[w++] = ']';
    buf[w] = 0;
    *out = buf;
    *len = w;
    return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

static int read_ident(const char **pp, char *out, size_t n) {
  const char *p = *pp;
  size_t i = 0;
  if (!isalpha((unsigned char)*p) && *p != '_')
    return -1;
  while (isalnum((unsigned char)*p) || *p == '_') {
    if (i + 1 >= n)
      return -1;
    out[i++] = *p++;
  }
  out[i] = 0;
  *pp = p;
  return 0;
}

int capnp_dyn_eval(const capnp_dyn_t *d, const char *expr, char **out,
                   size_t *len) {
  const char *p;
  char ident[160];
  ev_t ev;
  capnp_ptr_t node, value, type, field;
  int rc;
  if (!d || !expr || !out || !len)
    return CAPNP_ERR_ARG;
  p = expr;
  if (read_ident(&p, ident, sizeof(ident)))
    return CAPNP_ERR_KIND;
  memset(&value, 0, sizeof(value));
  memset(&type, 0, sizeof(type));
  if (find_schema_node(d, ident, 4, &node) == CAPNP_OK) {
    if (capnp_getp(&node, 4, &value) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    capnp_getp(&node, 3, &type);
    rc = ev_from_value(d, &value, &type, &ev);
    if (rc)
      return rc;
  } else if (find_schema_node(d, ident, 1, &node) == CAPNP_OK) {
    if (*p != '.')
      return CAPNP_ERR_KIND;
    p++;
    if (read_ident(&p, ident, sizeof(ident)))
      return CAPNP_ERR_KIND;
    if (field_node(&node, ident, &field) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    if (capnp_getp(&field, 3, &value) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    capnp_getp(&field, 2, &type);
    rc = ev_from_value(d, &value, &type, &ev);
    if (rc)
      return rc;
  } else {
    return CAPNP_ERR_KIND;
  }
  /* The struct-start branch advanced p with a side effect inside the
   * condition. Walk the rest of the path from the current p. */
  while (*p) {
    if (*p == '.') {
      p++;
      if (read_ident(&p, ident, sizeof(ident)))
        return CAPNP_ERR_KIND;
      rc = ev_field(d, &ev, ident);
      if (rc)
        return rc;
    } else if (*p == '[') {
      char *end = NULL;
      unsigned long idx;
      p++;
      idx = strtoul(p, &end, 10);
      if (end == p || *end != ']')
        return CAPNP_ERR_KIND;
      p = end + 1;
      rc = ev_index(d, &ev, (uint32_t)idx);
      if (rc)
        return rc;
    } else {
      return CAPNP_ERR_KIND;
    }
  }
  return format_ev(d, &ev, out, len);
}
