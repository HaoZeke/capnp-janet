#include <capnp-janet/capnp_dyn.h>
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_pointer.h>

#include "capnp_internal.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

enum {
  ND_FILE = 0,
  ND_STRUCT = 1,
  ND_ENUM = 2,
  ND_IFACE = 3,
  ND_CONST = 4,
  ND_ANN = 5
};

#define JSON_NAME_ANN_ID 0xfa5b1fd61c2e7c3dull
#define JSON_FLATTEN_ANN_ID 0x82d3e852af0336bfull
#define JSON_DISC_ANN_ID 0xcfa794e8d19a0162ull
#define JSON_BASE64_ANN_ID 0xd7d879450a253e4bull
#define JSON_HEX_ANN_ID 0xf061e22f0ae5c7b5ull
#define JSON_VALUE_TYPE_ID 0xa3fa7845f919dd83ull
#define JSON_FIELD_TYPE_ID 0xe31026e735d69ddfull
#define JSON_CALL_TYPE_ID 0xa0d9f6eca1c93d48ull

#define TYPE_MAX_DEPTH 64
/* JsonCodec's default maxNestingDepth. */
#define JSON_MAX_DEPTH 64
/* Nesting allowed while encoding a value or parsing and building one from
 * text; set from `dyn_max_depth` on entry to each public call. */
static _Thread_local int value_depth = CAPNP_DEFAULT_DEPTH_LIMIT;
static int dyn_max_depth = CAPNP_DEFAULT_DEPTH_LIMIT;

typedef struct {
  char *data;
  size_t n;
  size_t cap;
} buf_t;

typedef struct rtype rtype_t;

typedef struct {
  uint64_t scope_id;
  uint32_t nbinds;
  rtype_t *binds;
} brand_scope_t;

typedef struct {
  brand_scope_t *scopes;
  uint32_t nscopes;
} brand_env_t;

struct rtype {
  uint16_t which;
  uint16_t list_depth;
  uint64_t type_id;
  brand_env_t brand;
  uint16_t any_kind; /* 0 any, 1 struct, 2 list, 3 cap */
};

typedef struct {
  char name[96];
  char json_name[96];
  uint16_t code_order;
} dyn_enum_val_t;

typedef struct {
  char name[160];
  char short_name[160];
  uint64_t id;
  uint64_t scope_id;
  uint32_t nvals;
  dyn_enum_val_t *vals;
} dyn_enum_t;

typedef struct {
  int present;
  int has_name;
  char name[96];
  int has_value_name;
  char value_name[96];
} disc_opts_t;

typedef struct {
  char name[96];
  char json_name[96];
  char disc_name[96];
  int is_group;
  uint16_t code_order;
  uint32_t offset;
  uint16_t disc;
  uint64_t def_bits;
  int has_def_val;
  capnp_ptr_t def_val;
  capnp_ptr_t type_proto;
  int has_type_proto;
  uint64_t group_id;
  int json_flatten;
  char flatten_prefix[96];
  disc_opts_t sub_disc;
  int json_data_mode; /* 0 array, 1 base64, 2 hex */
} dyn_field_t;

typedef struct {
  char name[160];
  char short_name[160];
  uint64_t id;
  uint64_t scope_id;
  uint16_t dwords;
  uint16_t pwords;
  uint16_t disc_count;
  uint32_t disc_byte;
  int is_group;
  uint32_t nfields;
  uint32_t cap;
  dyn_field_t *fields;
  uint32_t nparams;
  char (*params)[96];
  disc_opts_t disc_opts;
  capnp_ptr_t node_ptr;
} dyn_struct_t;

struct capnp_dyn {
  capnp_message_t msg;
  capnp_ptr_t nodes_list;
  dyn_struct_t *structs;
  uint32_t nstructs;
  uint32_t scap;
  dyn_enum_t *enums;
  uint32_t nenums;
  uint32_t ecap;
  char err[384];
  /* JSON value that failed a type or range check. Points into the
   * input, which outlives the decode. Not owned. */
  const char *bad_src;
  size_t bad_n;
};

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

static int buf_ch(buf_t *b, char c) { return buf_add(b, &c, 1); }

static int buf_fmt_u(buf_t *b, uint64_t v) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
  return buf_str(b, tmp);
}

static int buf_fmt_i(buf_t *b, int64_t v) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
  return buf_str(b, tmp);
}

static void free_rtype(rtype_t *rt);

static void free_brand_env(brand_env_t *env) {
  uint32_t i, j;
  if (!env)
    return;
  for (i = 0; i < env->nscopes; i++) {
    for (j = 0; j < env->scopes[i].nbinds; j++)
      free_rtype(&env->scopes[i].binds[j]);
    free(env->scopes[i].binds);
  }
  free(env->scopes);
  env->scopes = NULL;
  env->nscopes = 0;
}

static void free_rtype(rtype_t *rt) {
  if (rt)
    free_brand_env(&rt->brand);
}

static int clone_rtype(const rtype_t *src, rtype_t *dst);

static int clone_brand_env(const brand_env_t *src, brand_env_t *dst) {
  uint32_t i, j;
  memset(dst, 0, sizeof(*dst));
  if (!src || !src->nscopes)
    return 0;
  dst->scopes =
      (brand_scope_t *)calloc(src->nscopes, sizeof(brand_scope_t));
  if (!dst->scopes)
    return -1;
  dst->nscopes = src->nscopes;
  for (i = 0; i < src->nscopes; i++) {
    dst->scopes[i].scope_id = src->scopes[i].scope_id;
    dst->scopes[i].nbinds = src->scopes[i].nbinds;
    if (src->scopes[i].nbinds) {
      dst->scopes[i].binds =
          (rtype_t *)calloc(src->scopes[i].nbinds, sizeof(rtype_t));
      if (!dst->scopes[i].binds) {
        free_brand_env(dst);
        return -1;
      }
      for (j = 0; j < src->scopes[i].nbinds; j++) {
        if (clone_rtype(&src->scopes[i].binds[j], &dst->scopes[i].binds[j])) {
          free_brand_env(dst);
          return -1;
        }
      }
    }
  }
  return 0;
}

static int clone_rtype(const rtype_t *src, rtype_t *dst) {
  *dst = *src;
  memset(&dst->brand, 0, sizeof(dst->brand));
  return clone_brand_env(&src->brand, &dst->brand);
}

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

static void extract_names(const capnp_ptr_t *node, char *full, size_t fn,
                          char *shortn, size_t sn) {
  const char *disp = node_text(node, 0);
  uint32_t plen = capnp_get_u32(node, 8, 0);
  const char *colon = strrchr(disp, ':');
  snprintf(full, fn, "%s", colon ? colon + 1 : disp);
  if (plen <= strlen(disp))
    snprintf(shortn, sn, "%s", disp + plen);
  else
    snprintf(shortn, sn, "%s", full);
}

static dyn_struct_t *find_struct_id(const capnp_dyn_t *d, uint64_t id) {
  uint32_t i;
  for (i = 0; i < d->nstructs; i++) {
    if (d->structs[i].id == id)
      return &d->structs[i];
  }
  return NULL;
}

static dyn_enum_t *find_enum_id(capnp_dyn_t *d, uint64_t id) {
  uint32_t i;
  for (i = 0; i < d->nenums; i++) {
    if (d->enums[i].id == id)
      return &d->enums[i];
  }
  return NULL;
}

static dyn_struct_t *find_struct_by_simple_name(const capnp_dyn_t *d,
                                                const char *name) {
  uint32_t i;
  for (i = 0; i < d->nstructs; i++) {
    if (strcmp(d->structs[i].name, name) == 0 ||
        strcmp(d->structs[i].short_name, name) == 0)
      return &d->structs[i];
  }
  return NULL;
}

static dyn_enum_t *find_enum_by_simple_name(const capnp_dyn_t *d,
                                            const char *name) {
  uint32_t i;
  for (i = 0; i < d->nenums; i++) {
    if (strcmp(d->enums[i].name, name) == 0 ||
        strcmp(d->enums[i].short_name, name) == 0)
      return &d->enums[i];
  }
  return NULL;
}

static dyn_field_t *find_field(const dyn_struct_t *st, const char *name) {
  uint32_t i;
  if (!st)
    return NULL;
  for (i = 0; i < st->nfields; i++) {
    if (strcmp(st->fields[i].name, name) == 0)
      return &st->fields[i];
  }
  return NULL;
}

static void parse_disc_options(const capnp_ptr_t *val, disc_opts_t *out) {
  capnp_ptr_t st, p0, p1;
  if (!val || val->kind != CAPNP_PK_STRUCT)
    return;
  if (capnp_get_u16(val, 0, 0xffff) != TY_STRUCT)
    return;
  if (capnp_getp(val, 0, &st) != CAPNP_OK || st.kind != CAPNP_PK_STRUCT)
    return;
  out->present = 1;
  if (capnp_getp(&st, 0, &p0) == CAPNP_OK && p0.kind != CAPNP_PK_NULL) {
    out->has_name = 1;
    snprintf(out->name, sizeof(out->name), "%s", node_text(&st, 0));
  }
  if (capnp_getp(&st, 1, &p1) == CAPNP_OK && p1.kind != CAPNP_PK_NULL) {
    out->has_value_name = 1;
    snprintf(out->value_name, sizeof(out->value_name), "%s", node_text(&st, 1));
  }
}

static void parse_flatten_options(const capnp_ptr_t *val, char *prefix,
                                  size_t n) {
  capnp_ptr_t st, p0;
  prefix[0] = 0;
  if (!val || val->kind != CAPNP_PK_STRUCT)
    return;
  if (capnp_get_u16(val, 0, 0xffff) != TY_STRUCT)
    return;
  if (capnp_getp(val, 0, &st) != CAPNP_OK || st.kind != CAPNP_PK_STRUCT)
    return;
  if (capnp_getp(&st, 0, &p0) == CAPNP_OK && p0.kind != CAPNP_PK_NULL)
    snprintf(prefix, n, "%s", node_text(&st, 0));
}

static void load_field_annotations(const capnp_ptr_t *fnode, dyn_field_t *df) {
  capnp_ptr_t anns;
  uint32_t i, n;
  if (capnp_getp(fnode, 1, &anns) != CAPNP_OK || anns.kind != CAPNP_PK_LIST)
    return;
  n = capnp_list_len(&anns);
  for (i = 0; i < n; i++) {
    capnp_ptr_t ann, val;
    uint64_t id;
    if (capnp_list_getp(&anns, i, &ann) != CAPNP_OK)
      continue;
    id = capnp_get_u64(&ann, 0, 0);
    if (capnp_getp(&ann, 0, &val) != CAPNP_OK)
      memset(&val, 0, sizeof(val));
    if (id == JSON_NAME_ANN_ID && val.kind == CAPNP_PK_STRUCT) {
      snprintf(df->json_name, sizeof(df->json_name), "%s", node_text(&val, 0));
      snprintf(df->disc_name, sizeof(df->disc_name), "%s", df->json_name);
    } else if (id == JSON_FLATTEN_ANN_ID) {
      df->json_flatten = 1;
      parse_flatten_options(&val, df->flatten_prefix,
                            sizeof(df->flatten_prefix));
    } else if (id == JSON_DISC_ANN_ID) {
      parse_disc_options(&val, &df->sub_disc);
    } else if (id == JSON_BASE64_ANN_ID) {
      df->json_data_mode = 1;
    } else if (id == JSON_HEX_ANN_ID) {
      df->json_data_mode = 2;
    }
  }
}

static void load_struct_annotations(const capnp_ptr_t *node, dyn_struct_t *st) {
  capnp_ptr_t anns;
  uint32_t i, n;
  if (capnp_getp(node, 2, &anns) != CAPNP_OK || anns.kind != CAPNP_PK_LIST)
    return;
  n = capnp_list_len(&anns);
  for (i = 0; i < n; i++) {
    capnp_ptr_t ann, val;
    if (capnp_list_getp(&anns, i, &ann) != CAPNP_OK)
      continue;
    if (capnp_get_u64(&ann, 0, 0) == JSON_DISC_ANN_ID &&
        capnp_getp(&ann, 0, &val) == CAPNP_OK) {
      parse_disc_options(&val, &st->disc_opts);
    }
  }
}

static void load_default(const capnp_ptr_t *field, dyn_field_t *f) {
  capnp_ptr_t value;
  uint16_t which;
  f->def_bits = 0;
  f->has_def_val = 0;
  if (capnp_getp(field, 3, &value) != CAPNP_OK || value.kind != CAPNP_PK_STRUCT)
    return;
  f->def_val = value;
  f->has_def_val = 1;
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

static int add_struct(capnp_dyn_t *d, dyn_struct_t **out) {
  if (d->nstructs == d->scap) {
    uint32_t ncap = d->scap ? d->scap * 2u : 16u;
    dyn_struct_t *p =
        (dyn_struct_t *)realloc(d->structs, (size_t)ncap * sizeof(*p));
    if (!p)
      return CAPNP_ERR_ALLOC;
    d->structs = p;
    d->scap = ncap;
  }
  memset(&d->structs[d->nstructs], 0, sizeof(d->structs[0]));
  *out = &d->structs[d->nstructs++];
  return CAPNP_OK;
}

static int add_field(dyn_struct_t *st, dyn_field_t **out) {
  if (st->nfields == st->cap) {
    uint32_t ncap = st->cap ? st->cap * 2u : 8u;
    dyn_field_t *p =
        (dyn_field_t *)realloc(st->fields, (size_t)ncap * sizeof(*p));
    if (!p)
      return CAPNP_ERR_ALLOC;
    st->fields = p;
    st->cap = ncap;
  }
  memset(&st->fields[st->nfields], 0, sizeof(st->fields[0]));
  *out = &st->fields[st->nfields++];
  return CAPNP_OK;
}

static int load_struct_node(capnp_dyn_t *d, const capnp_ptr_t *node) {
  dyn_struct_t *st;
  capnp_ptr_t fields, params;
  uint32_t i, n;
  int rc = add_struct(d, &st);
  if (rc)
    return rc;
  st->node_ptr = *node;
  st->id = capnp_get_u64(node, 0, 0);
  st->scope_id = capnp_get_u64(node, 16, 0);
  extract_names(node, st->name, sizeof(st->name), st->short_name,
                sizeof(st->short_name));
  st->dwords = capnp_get_u16(node, 14, 0);
  st->pwords = capnp_get_u16(node, 24, 0);
  st->disc_count = capnp_get_u16(node, 30, 0);
  {
    /* schema-loader rejects (discriminantOffset + 1) * 16 bits past
     * the data section. A uint32 multiply wraps that offset to 0. */
    uint32_t disc_off = capnp_get_u32(node, 32, 0);
    uint64_t disc_bytes = (uint64_t)disc_off * 2u;
    if (st->disc_count > 0 &&
        (disc_bytes > UINT32_MAX ||
         disc_bytes + 2u > (uint64_t)st->dwords * 8u))
      return CAPNP_ERR_SCHEMA;
    st->disc_byte = disc_bytes > UINT32_MAX ? 0u : (uint32_t)disc_bytes;
  }
  st->is_group = capnp_get_bool(node, 224, 0);
  load_struct_annotations(node, st);

  if (capnp_getp(node, 5, &params) == CAPNP_OK &&
      params.kind == CAPNP_PK_LIST) {
    uint32_t np = capnp_list_len(&params);
    if (np > 0) {
      st->params = (char (*)[96])calloc(np, sizeof(*st->params));
      if (!st->params)
        return CAPNP_ERR_ALLOC;
      st->nparams = np;
      for (i = 0; i < np; i++) {
        capnp_ptr_t p;
        if (capnp_list_getp(&params, i, &p) == CAPNP_OK)
          snprintf(st->params[i], sizeof(st->params[i]), "%s",
                   node_text(&p, 0));
      }
    }
  }

  if (capnp_getp(node, 3, &fields) != CAPNP_OK || fields.kind != CAPNP_PK_LIST)
    return CAPNP_OK;
  n = capnp_list_len(&fields);
  for (i = 0; i < n; i++) {
    capnp_ptr_t f, typ;
    dyn_field_t *df;
    uint16_t fwhich;
    if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
      continue;
    if (add_field(st, &df))
      return CAPNP_ERR_ALLOC;
    snprintf(df->name, sizeof(df->name), "%s", node_text(&f, 0));
    snprintf(df->json_name, sizeof(df->json_name), "%s", df->name);
    snprintf(df->disc_name, sizeof(df->disc_name), "%s", df->name);
    df->code_order = capnp_get_u16(&f, 0, 0);
    df->disc = (uint16_t)(capnp_get_u16(&f, 2, 0) ^ 0xffffu);
    fwhich = capnp_get_u16(&f, 8, 0xffff);
    load_field_annotations(&f, df);
    if (fwhich == 1) {
      df->is_group = 1;
      df->group_id = capnp_get_u64(&f, 16, 0);
      continue;
    }
    df->offset = capnp_get_u32(&f, 4, 0);
    if (capnp_getp(&f, 2, &typ) == CAPNP_OK && typ.kind == CAPNP_PK_STRUCT) {
      df->type_proto = typ;
      df->has_type_proto = 1;
    }
    load_default(&f, df);
  }
  return CAPNP_OK;
}

static int load_enum_node(capnp_dyn_t *d, const capnp_ptr_t *node) {
  dyn_enum_t *en;
  capnp_ptr_t enumerants;
  uint32_t i, n;
  if (d->nenums == d->ecap) {
    uint32_t ncap = d->ecap ? d->ecap * 2u : 8u;
    dyn_enum_t *p =
        (dyn_enum_t *)realloc(d->enums, (size_t)ncap * sizeof(*p));
    if (!p)
      return CAPNP_ERR_ALLOC;
    d->enums = p;
    d->ecap = ncap;
  }
  en = &d->enums[d->nenums++];
  memset(en, 0, sizeof(*en));
  en->id = capnp_get_u64(node, 0, 0);
  en->scope_id = capnp_get_u64(node, 16, 0);
  extract_names(node, en->name, sizeof(en->name), en->short_name,
                sizeof(en->short_name));
  if (capnp_getp(node, 3, &enumerants) != CAPNP_OK ||
      enumerants.kind != CAPNP_PK_LIST)
    return CAPNP_OK;
  n = capnp_list_len(&enumerants);
  if (n > 0) {
    en->vals = (dyn_enum_val_t *)calloc(n, sizeof(dyn_enum_val_t));
    if (!en->vals)
      return CAPNP_ERR_ALLOC;
    en->nvals = n;
    for (i = 0; i < n; i++) {
      capnp_ptr_t ev, anns;
      if (capnp_list_getp(&enumerants, i, &ev) != CAPNP_OK)
        continue;
      snprintf(en->vals[i].name, sizeof(en->vals[i].name), "%s",
               node_text(&ev, 0));
      snprintf(en->vals[i].json_name, sizeof(en->vals[i].json_name), "%s",
               en->vals[i].name);
      en->vals[i].code_order = capnp_get_u16(&ev, 0, 0);
      if (capnp_getp(&ev, 1, &anns) == CAPNP_OK &&
          anns.kind == CAPNP_PK_LIST) {
        uint32_t j, na = capnp_list_len(&anns);
        for (j = 0; j < na; j++) {
          capnp_ptr_t ann, val;
          if (capnp_list_getp(&anns, j, &ann) == CAPNP_OK &&
              capnp_get_u64(&ann, 0, 0) == JSON_NAME_ANN_ID &&
              capnp_getp(&ann, 0, &val) == CAPNP_OK &&
              val.kind == CAPNP_PK_STRUCT) {
            snprintf(en->vals[i].json_name, sizeof(en->vals[i].json_name), "%s",
                     node_text(&val, 0));
          }
        }
      }
    }
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
      capnp_getp(&root, 0, &nodes) != CAPNP_OK ||
      nodes.kind != CAPNP_PK_LIST) {
    capnp_dyn_free(d);
    return CAPNP_ERR_FRAMING;
  }
  d->nodes_list = nodes;
  n = capnp_list_len(&nodes);
  for (i = 0; i < n; i++) {
    capnp_ptr_t node;
    uint16_t which;
    if (capnp_list_getp(&nodes, i, &node) != CAPNP_OK)
      continue;
    which = capnp_get_u16(&node, 12, 0xffff);
    if (which == ND_STRUCT) {
      int rc = load_struct_node(d, &node);
      if (rc) {
        capnp_dyn_free(d);
        return rc;
      }
    } else if (which == ND_ENUM) {
      if (load_enum_node(d, &node)) {
        capnp_dyn_free(d);
        return CAPNP_ERR_ALLOC;
      }
    }
  }
  for (i = 0; i < d->nstructs; i++) {
    uint32_t j;
    for (j = 0; j < d->structs[i].nfields; j++) {
      dyn_field_t *f = &d->structs[i].fields[j];
      if (f->is_group && f->sub_disc.present) {
        dyn_struct_t *g = find_struct_id(d, f->group_id);
        if (g && !g->disc_opts.present) {
          g->disc_opts = f->sub_disc;
          if (!g->disc_opts.has_name && f->json_flatten) {
            g->disc_opts.has_name = 1;
            snprintf(g->disc_opts.name, sizeof(g->disc_opts.name), "%s",
                     f->name);
          }
        }
      }
    }
  }
  *out = d;
  return CAPNP_OK;
}

static void dyn_set_err(capnp_dyn_t *d, const char *msg) {
  if (!d)
    return;
  snprintf(d->err, sizeof(d->err), "%s", msg ? msg : "");
}

static void dyn_err_text(capnp_dyn_t *d, const char *what, const char *text,
                         size_t n) {
  char tok[49];
  size_t i;
  if (!d)
    return;
  if (n > sizeof(tok) - 1)
    n = sizeof(tok) - 1;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    tok[i] = (c < 32 || c == '"' || c == '\'') ? '?' : (char)c;
  }
  tok[n] = 0;
  if (n == 0)
    snprintf(d->err, sizeof(d->err), "%s", what);
  else
    snprintf(d->err, sizeof(d->err), "%s: %s", what, tok);
}

const char *capnp_dyn_error(const capnp_dyn_t *d) {
  if (!d || !d->err[0])
    return "";
  return d->err;
}

void capnp_dyn_set_max_depth(int depth) {
  dyn_max_depth = depth < 1 ? 1 : depth;
}

void capnp_dyn_free(capnp_dyn_t *d) {
  uint32_t i;
  if (!d)
    return;
  for (i = 0; i < d->nstructs; i++) {
    free(d->structs[i].fields);
    free(d->structs[i].params);
  }
  for (i = 0; i < d->nenums; i++)
    free(d->enums[i].vals);
  free(d->structs);
  free(d->enums);
  capnp_message_free(&d->msg);
  free(d);
}

static int resolve_type_proto(const capnp_dyn_t *d, const capnp_ptr_t *typ,
                              const brand_env_t *env, rtype_t *out, int depth);

static int merge_brand_proto(const capnp_dyn_t *d, const capnp_ptr_t *brand,
                             const brand_env_t *parent_env, brand_env_t *out,
                             int depth) {
  capnp_ptr_t scopes;
  uint32_t i, ns;
  if (clone_brand_env(parent_env, out))
    return -1;
  if (!brand || brand->kind != CAPNP_PK_STRUCT)
    return 0;
  if (capnp_getp(brand, 0, &scopes) != CAPNP_OK ||
      scopes.kind != CAPNP_PK_LIST)
    return 0;
  ns = capnp_list_len(&scopes);
  for (i = 0; i < ns; i++) {
    capnp_ptr_t sc, binds;
    uint64_t sid;
    uint16_t swhich;
    uint32_t j, nb;
    brand_scope_t *target = NULL;
    if (capnp_list_getp(&scopes, i, &sc) != CAPNP_OK)
      continue;
    sid = capnp_get_u64(&sc, 0, 0);
    swhich = capnp_get_u16(&sc, 8, 0xffff);
    if (swhich == 1) /* inherit */
      continue;
    if (capnp_getp(&sc, 0, &binds) != CAPNP_OK || binds.kind != CAPNP_PK_LIST)
      continue;
    nb = capnp_list_len(&binds);
    for (j = 0; j < out->nscopes; j++) {
      if (out->scopes[j].scope_id == sid) {
        target = &out->scopes[j];
        break;
      }
    }
    if (!target) {
      brand_scope_t *ns_arr = (brand_scope_t *)realloc(
          out->scopes, (out->nscopes + 1) * sizeof(brand_scope_t));
      if (!ns_arr) {
        free_brand_env(out);
        return -1;
      }
      out->scopes = ns_arr;
      target = &out->scopes[out->nscopes++];
      memset(target, 0, sizeof(*target));
      target->scope_id = sid;
    } else {
      for (j = 0; j < target->nbinds; j++)
        free_rtype(&target->binds[j]);
      free(target->binds);
      target->binds = NULL;
      target->nbinds = 0;
    }
    if (nb > 0) {
      target->binds = (rtype_t *)calloc(nb, sizeof(rtype_t));
      if (!target->binds) {
        free_brand_env(out);
        return -1;
      }
      target->nbinds = nb;
      for (j = 0; j < nb; j++) {
        capnp_ptr_t bnd, btype;
        target->binds[j].which = TY_ANY;
        if (capnp_list_getp(&binds, j, &bnd) != CAPNP_OK)
          continue;
        if (capnp_get_u16(&bnd, 0, 0xffff) == 1 &&
            capnp_getp(&bnd, 0, &btype) == CAPNP_OK &&
            btype.kind == CAPNP_PK_STRUCT) {
          if (resolve_type_proto(d, &btype, parent_env, &target->binds[j],
                                 depth + 1)) {
            free_brand_env(out);
            return -1;
          }
        }
      }
    }
  }
  return 0;
}

static int resolve_type_proto(const capnp_dyn_t *d, const capnp_ptr_t *typ,
                              const brand_env_t *env, rtype_t *out, int depth) {
  uint16_t w;
  memset(out, 0, sizeof(*out));
  if (depth > TYPE_MAX_DEPTH || !typ || typ->kind != CAPNP_PK_STRUCT)
    return -1;
  w = capnp_get_u16(typ, 0, 0xffff);
  if (w == TY_LIST) {
    capnp_ptr_t el;
    if (capnp_getp(typ, 0, &el) != CAPNP_OK || el.kind != CAPNP_PK_STRUCT)
      return -1;
    if (resolve_type_proto(d, &el, env, out, depth + 1))
      return -1;
    out->list_depth++;
    return 0;
  }
  if (w == TY_ANY) {
    uint16_t aw = capnp_get_u16(typ, 8, 0xffff);
    if (aw == 1) { /* parameter */
      uint64_t sid = capnp_get_u64(typ, 16, 0);
      uint16_t pidx = capnp_get_u16(typ, 10, 0);
      if (env) {
        uint32_t i;
        for (i = 0; i < env->nscopes; i++) {
          if (env->scopes[i].scope_id == sid) {
            if (pidx < env->scopes[i].nbinds)
              return clone_rtype(&env->scopes[i].binds[pidx], out);
            break;
          }
        }
      }
      out->which = TY_ANY;
      return 0;
    }
    out->which = TY_ANY;
    out->any_kind = capnp_get_u16(typ, 10, 0);
    return 0;
  }
  out->which = w;
  if (w == TY_STRUCT || w == TY_ENUM || w == TY_IFACE) {
    capnp_ptr_t br;
    out->type_id = capnp_get_u64(typ, 8, 0);
    if (capnp_getp(typ, 0, &br) == CAPNP_OK && br.kind == CAPNP_PK_STRUCT) {
      if (merge_brand_proto(d, &br, env, &out->brand, depth + 1))
        return -1;
    } else if (clone_brand_env(env, &out->brand)) {
      return -1;
    }
  }
  return 0;
}

static int resolve_field_type(const capnp_dyn_t *d, const dyn_field_t *f,
                              const brand_env_t *env, rtype_t *out) {
  memset(out, 0, sizeof(*out));
  if (f->is_group) {
    out->which = TY_STRUCT;
    out->type_id = f->group_id;
    return clone_brand_env(env, &out->brand);
  }
  if (!f->has_type_proto)
    return -1;
  return resolve_type_proto(d, &f->type_proto, env, out, 0);
}

static void skip_spaces(const char **pp) {
  while (**pp && isspace((unsigned char)**pp))
    (*pp)++;
}

static int parse_type_expr(const capnp_dyn_t *d, const char **pp, rtype_t *out,
                           int depth) {
  char name[160];
  size_t k = 0;
  memset(out, 0, sizeof(*out));
  if (depth > TYPE_MAX_DEPTH)
    return -1;
  skip_spaces(pp);
  if (**pp == '.')
    (*pp)++;
  while (**pp && (isalnum((unsigned char)**pp) || **pp == '_' || **pp == '.')) {
    if (k + 1 < sizeof(name))
      name[k++] = **pp;
    (*pp)++;
  }
  name[k] = 0;
  if (!k)
    return -1;
  skip_spaces(pp);
  if (strcmp(name, "Void") == 0) {
    out->which = TY_VOID;
    return 0;
  }
  if (strcmp(name, "Bool") == 0) {
    out->which = TY_BOOL;
    return 0;
  }
  if (strcmp(name, "Int8") == 0) {
    out->which = TY_I8;
    return 0;
  }
  if (strcmp(name, "Int16") == 0) {
    out->which = TY_I16;
    return 0;
  }
  if (strcmp(name, "Int32") == 0) {
    out->which = TY_I32;
    return 0;
  }
  if (strcmp(name, "Int64") == 0) {
    out->which = TY_I64;
    return 0;
  }
  if (strcmp(name, "UInt8") == 0) {
    out->which = TY_U8;
    return 0;
  }
  if (strcmp(name, "UInt16") == 0) {
    out->which = TY_U16;
    return 0;
  }
  if (strcmp(name, "UInt32") == 0) {
    out->which = TY_U32;
    return 0;
  }
  if (strcmp(name, "UInt64") == 0) {
    out->which = TY_U64;
    return 0;
  }
  if (strcmp(name, "Float32") == 0) {
    out->which = TY_F32;
    return 0;
  }
  if (strcmp(name, "Float64") == 0) {
    out->which = TY_F64;
    return 0;
  }
  if (strcmp(name, "Text") == 0) {
    out->which = TY_TEXT;
    return 0;
  }
  if (strcmp(name, "Data") == 0) {
    out->which = TY_DATA;
    return 0;
  }
  if (strcmp(name, "AnyPointer") == 0) {
    out->which = TY_ANY;
    out->any_kind = 0;
    return 0;
  }
  if (strcmp(name, "AnyStruct") == 0) {
    out->which = TY_ANY;
    out->any_kind = 1;
    return 0;
  }
  if (strcmp(name, "AnyList") == 0) {
    out->which = TY_ANY;
    out->any_kind = 2;
    return 0;
  }
  if (strcmp(name, "Capability") == 0) {
    out->which = TY_ANY;
    out->any_kind = 3;
    return 0;
  }
  if (strcmp(name, "List") == 0) {
    if (**pp != '(')
      return -1;
    (*pp)++;
    if (parse_type_expr(d, pp, out, depth + 1))
      return -1;
    skip_spaces(pp);
    if (**pp != ')') {
      free_rtype(out);
      return -1;
    }
    (*pp)++;
    out->list_depth++;
    return 0;
  }
  {
    dyn_struct_t *st = find_struct_by_simple_name(d, name);
    dyn_enum_t *en = st ? NULL : find_enum_by_simple_name(d, name);
    if (!st && !en)
      return -1;
    if (en) {
      out->which = TY_ENUM;
      out->type_id = en->id;
      return 0;
    }
    out->which = TY_STRUCT;
    out->type_id = st->id;
    if (**pp == '(') {
      brand_scope_t *sc;
      uint32_t cap = 4, nb = 0;
      (*pp)++;
      out->brand.scopes = (brand_scope_t *)calloc(1, sizeof(brand_scope_t));
      if (!out->brand.scopes)
        return -1;
      out->brand.nscopes = 1;
      sc = &out->brand.scopes[0];
      sc->scope_id = st->id;
      sc->binds = (rtype_t *)calloc(cap, sizeof(rtype_t));
      if (!sc->binds) {
        free_rtype(out);
        return -1;
      }
      skip_spaces(pp);
      while (**pp && **pp != ')') {
        if (nb == cap) {
          rtype_t *nb_arr =
              (rtype_t *)realloc(sc->binds, (cap * 2) * sizeof(rtype_t));
          if (!nb_arr) {
            sc->nbinds = nb;
            free_rtype(out);
            return -1;
          }
          memset(nb_arr + cap, 0, cap * sizeof(rtype_t));
          sc->binds = nb_arr;
          cap *= 2;
        }
        if (parse_type_expr(d, pp, &sc->binds[nb], depth + 1)) {
          sc->nbinds = nb;
          free_rtype(out);
          return -1;
        }
        nb++;
        skip_spaces(pp);
        if (**pp == ',') {
          (*pp)++;
          skip_spaces(pp);
        } else if (**pp != ')') {
          sc->nbinds = nb;
          free_rtype(out);
          return -1;
        }
      }
      sc->nbinds = nb;
      if (**pp != ')') {
        free_rtype(out);
        return -1;
      }
      (*pp)++;
    }
    return 0;
  }
}

static int resolve_root_type_name(const capnp_dyn_t *d, const char *type_name,
                                  rtype_t *out, dyn_struct_t **st_out) {
  const char *p = type_name;
  dyn_struct_t *st;
  if (!type_name || parse_type_expr(d, &p, out, 0))
    return CAPNP_ERR_KIND;
  skip_spaces(&p);
  if (*p != 0 || out->list_depth != 0 || out->which != TY_STRUCT) {
    free_rtype(out);
    return CAPNP_ERR_KIND;
  }
  st = find_struct_id(d, out->type_id);
  if (!st) {
    free_rtype(out);
    return CAPNP_ERR_KIND;
  }
  *st_out = st;
  return CAPNP_OK;
}

static int struct_which(const capnp_ptr_t *s, const dyn_struct_t *st,
                        uint16_t *out) {
  if (!st->disc_count) {
    *out = 0xffff;
    return CAPNP_OK;
  }
  *out = s && s->kind == CAPNP_PK_STRUCT ? capnp_get_u16(s, st->disc_byte, 0) : 0;
  return CAPNP_OK;
}

static int logical_bits_for_type(const capnp_ptr_t *s, const dyn_field_t *f,
                                 uint16_t which, uint64_t *out) {
  uint32_t b = data_byte(which, f->offset);
  uint64_t wire = 0;
  if (s && s->kind == CAPNP_PK_STRUCT) {
    switch (which) {
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

static int lookup_active(const capnp_dyn_t *d, const capnp_ptr_t *s,
                         const char *type_name, const char *field,
                         dyn_struct_t **st, dyn_field_t **f, rtype_t *ft) {
  rtype_t rt;
  uint16_t active = 0xffff;
  if (!type_name || !field)
    return CAPNP_ERR_ARG;
  if (resolve_root_type_name(d, type_name, &rt, st) != CAPNP_OK)
    return CAPNP_ERR_KIND;
  *f = find_field(*st, field);
  if (!*f) {
    free_rtype(&rt);
    return CAPNP_ERR_KIND;
  }
  if ((*f)->disc != 0xffff && (*st)->disc_count) {
    struct_which(s, *st, &active);
    if ((*f)->disc != active) {
      free_rtype(&rt);
      return CAPNP_ERR_KIND;
    }
  }
  if (resolve_field_type(d, *f, &rt.brand, ft)) {
    free_rtype(&rt);
    return CAPNP_ERR_KIND;
  }
  free_rtype(&rt);
  return CAPNP_OK;
}

int capnp_dyn_which(const capnp_dyn_t *d, const capnp_ptr_t *s,
                    const char *type_name, const char **out) {
  rtype_t rt;
  dyn_struct_t *st;
  uint16_t active = 0xffff;
  uint32_t i;
  if (!d || !s || !type_name || !out)
    return CAPNP_ERR_ARG;
  if (resolve_root_type_name(d, type_name, &rt, &st) != CAPNP_OK)
    return CAPNP_ERR_KIND;
  free_rtype(&rt);
  if (!st->disc_count)
    return CAPNP_ERR_KIND;
  struct_which(s, st, &active);
  for (i = 0; i < st->nfields; i++) {
    if (st->fields[i].disc == active) {
      *out = st->fields[i].name;
      return CAPNP_OK;
    }
  }
  return CAPNP_ERR_KIND;
}

int capnp_dyn_get_bool(const capnp_dyn_t *d, const capnp_ptr_t *s,
                       const char *type_name, const char *field, int *out) {
  dyn_struct_t *st;
  dyn_field_t *f;
  rtype_t ft;
  uint64_t bits;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup_active(d, s, type_name, field, &st, &f, &ft);
  if (rc)
    return rc;
  if (ft.list_depth != 0 || ft.which != TY_BOOL) {
    free_rtype(&ft);
    return CAPNP_ERR_KIND;
  }
  rc = logical_bits_for_type(s, f, ft.which, &bits);
  free_rtype(&ft);
  if (rc)
    return rc;
  *out = bits != 0;
  return CAPNP_OK;
}

int capnp_dyn_get_i64(const capnp_dyn_t *d, const capnp_ptr_t *s,
                      const char *type_name, const char *field, int64_t *out) {
  dyn_struct_t *st;
  dyn_field_t *f;
  rtype_t ft;
  uint64_t bits;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup_active(d, s, type_name, field, &st, &f, &ft);
  if (rc)
    return rc;
  if (ft.list_depth != 0 ||
      (ft.which != TY_I8 && ft.which != TY_I16 && ft.which != TY_I32 &&
       ft.which != TY_I64 && ft.which != TY_U8 && ft.which != TY_U16 &&
       ft.which != TY_U32 && ft.which != TY_U64 && ft.which != TY_ENUM)) {
    free_rtype(&ft);
    return CAPNP_ERR_KIND;
  }
  rc = logical_bits_for_type(s, f, ft.which, &bits);
  if (rc == CAPNP_OK)
    *out = sext(bits, ft.which);
  free_rtype(&ft);
  return rc;
}

int capnp_dyn_get_u64(const capnp_dyn_t *d, const capnp_ptr_t *s,
                      const char *type_name, const char *field, uint64_t *out) {
  dyn_struct_t *st;
  dyn_field_t *f;
  rtype_t ft;
  uint64_t bits;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup_active(d, s, type_name, field, &st, &f, &ft);
  if (rc)
    return rc;
  if (ft.list_depth != 0 ||
      (ft.which != TY_U8 && ft.which != TY_U16 && ft.which != TY_U32 &&
       ft.which != TY_U64 && ft.which != TY_ENUM && ft.which != TY_I8 &&
       ft.which != TY_I16 && ft.which != TY_I32 && ft.which != TY_I64)) {
    free_rtype(&ft);
    return CAPNP_ERR_KIND;
  }
  rc = logical_bits_for_type(s, f, ft.which, &bits);
  free_rtype(&ft);
  if (rc)
    return rc;
  *out = bits;
  return CAPNP_OK;
}

int capnp_dyn_get_f64(const capnp_dyn_t *d, const capnp_ptr_t *s,
                      const char *type_name, const char *field, double *out) {
  dyn_struct_t *st;
  dyn_field_t *f;
  rtype_t ft;
  uint64_t bits;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup_active(d, s, type_name, field, &st, &f, &ft);
  if (rc)
    return rc;
  if (ft.list_depth != 0 || (ft.which != TY_F32 && ft.which != TY_F64)) {
    free_rtype(&ft);
    return CAPNP_ERR_KIND;
  }
  rc = logical_bits_for_type(s, f, ft.which, &bits);
  if (rc == CAPNP_OK) {
    if (ft.which == TY_F32) {
      uint32_t u = (uint32_t)bits;
      float fv;
      memcpy(&fv, &u, sizeof(fv));
      *out = (double)fv;
    } else {
      memcpy(out, &bits, sizeof(*out));
    }
  }
  free_rtype(&ft);
  return rc;
}

/* C++ readTextPointer and readDataPointer do not check the nesting limit,
 * so a Text or Data field reads even where a struct could not. */
static int getp_blob(const capnp_ptr_t *s, uint16_t idx, capnp_ptr_t *out) {
  capnp_ptr_t holder = *s;
  if (holder.depth_left < 1)
    holder.depth_left = 1;
  return capnp_getp(&holder, idx, out);
}

static int list_fits(const capnp_ptr_t *l, int want);
static int elem_esize(const rtype_t *elem);

/* The kind checks DynamicStruct::Reader::get makes through the layout
 * readers before it returns a pointer field. */
static int ptr_fits_type(const capnp_ptr_t *p, const rtype_t *ft) {
  rtype_t elem;
  if (p->kind == CAPNP_PK_NULL)
    return 1;
  if (ft->list_depth > 0) {
    elem = *ft;
    elem.list_depth--;
    return p->kind == CAPNP_PK_LIST && list_fits(p, elem_esize(&elem));
  }
  switch (ft->which) {
  case TY_TEXT:
  case TY_DATA:
    return p->kind == CAPNP_PK_LIST && p->esize == CAPNP_SZ_BYTE;
  case TY_STRUCT:
    return p->kind == CAPNP_PK_STRUCT;
  case TY_IFACE:
    return p->kind == CAPNP_PK_CAP;
  case TY_ANY:
    if (ft->any_kind == 1)
      return p->kind == CAPNP_PK_STRUCT;
    if (ft->any_kind == 2)
      return p->kind == CAPNP_PK_LIST;
    if (ft->any_kind == 3)
      return p->kind == CAPNP_PK_CAP;
    return 1;
  default:
    return 1;
  }
}

int capnp_dyn_get_text(const capnp_dyn_t *d, const capnp_ptr_t *s,
                       const char *type_name, const char *field,
                       const char **out, size_t *len) {
  dyn_struct_t *st;
  dyn_field_t *f;
  rtype_t ft;
  capnp_ptr_t p;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup_active(d, s, type_name, field, &st, &f, &ft);
  if (rc)
    return rc;
  if (ft.list_depth != 0 || ft.which != TY_TEXT) {
    free_rtype(&ft);
    return CAPNP_ERR_KIND;
  }
  free_rtype(&ft);
  rc = getp_blob(s, (uint16_t)f->offset, &p);
  if (rc)
    return rc;
  if (p.kind != CAPNP_PK_NULL)
    return capnp_get_text(s, (uint16_t)f->offset, out, len);
  if (f->has_def_val && capnp_get_u16(&f->def_val, 0, 0xffff) == TY_TEXT) {
    if (capnp_getp(&f->def_val, 0, &p) == CAPNP_OK && p.kind != CAPNP_PK_NULL)
      return capnp_get_text(&f->def_val, 0, out, len);
  }
  *out = "";
  if (len)
    *len = 0;
  return CAPNP_OK;
}

int capnp_dyn_get_ptr(const capnp_dyn_t *d, const capnp_ptr_t *s,
                      const char *type_name, const char *field,
                      capnp_ptr_t *out) {
  dyn_struct_t *st;
  dyn_field_t *f;
  rtype_t ft;
  capnp_ptr_t p;
  int rc;
  if (!d || !s || !out)
    return CAPNP_ERR_ARG;
  rc = lookup_active(d, s, type_name, field, &st, &f, &ft);
  if (rc)
    return rc;
  if (f->is_group) {
    free_rtype(&ft);
    *out = *s;
    return CAPNP_OK;
  }
  if (ft.list_depth == 0 && !is_ptr_type(ft.which)) {
    free_rtype(&ft);
    return CAPNP_ERR_KIND;
  }
  if (ft.list_depth == 0 && (ft.which == TY_TEXT || ft.which == TY_DATA))
    rc = getp_blob(s, (uint16_t)f->offset, &p);
  else
    rc = capnp_getp(s, (uint16_t)f->offset, &p);
  if (rc == CAPNP_OK && !ptr_fits_type(&p, &ft))
    rc = CAPNP_ERR_KIND;
  free_rtype(&ft);
  if (rc != CAPNP_OK)
    return rc;
  if (p.kind == CAPNP_PK_NULL && f->has_def_val) {
    uint16_t vw = capnp_get_u16(&f->def_val, 0, 0xffff);
    if (vw == TY_STRUCT || vw == TY_LIST || vw == TY_ANY || vw == TY_TEXT ||
        vw == TY_DATA) {
      capnp_ptr_t dp;
      if (capnp_getp(&f->def_val, 0, &dp) == CAPNP_OK &&
          dp.kind != CAPNP_PK_NULL) {
        *out = dp;
        return CAPNP_OK;
      }
    }
  }
  *out = p;
  return CAPNP_OK;
}
/* --- Float and string formatting (KJ DoubleToBuffer / FloatToBuffer / encodeCEscape) --- */

static void remove_plus(char *buf) {
  char *p;
  while ((p = strchr(buf, '+')) != NULL)
    memmove(p, p + 1, strlen(p + 1) + 1);
}

static void fmt_f64(double value, char *buf, size_t n) {
  volatile double parsed;
  if (isnan(value)) {
    snprintf(buf, n, "nan");
    return;
  }
  if (isinf(value)) {
    snprintf(buf, n, value < 0 ? "-inf" : "inf");
    return;
  }
  snprintf(buf, n, "%.*g", DBL_DIG, value);
  parsed = strtod(buf, NULL);
  if (parsed != value)
    snprintf(buf, n, "%.*g", DBL_DIG + 2, value);
  remove_plus(buf);
}

static void fmt_f32(float value, char *buf, size_t n) {
  char *endp = NULL;
  float parsed;
  if (isnan(value)) {
    snprintf(buf, n, "nan");
    return;
  }
  if (isinf(value)) {
    snprintf(buf, n, value < 0 ? "-inf" : "inf");
    return;
  }
  snprintf(buf, n, "%.*g", FLT_DIG, value);
  errno = 0;
  parsed = strtof(buf, &endp);
  if (!buf[0] || (endp && *endp != 0) || errno != 0 || parsed != value)
    snprintf(buf, n, "%.*g", FLT_DIG + 2, value);
  remove_plus(buf);
}

static int quote_text_capnp(buf_t *b, const uint8_t *s, size_t n, int is_binary) {
  size_t i;
  if (buf_ch(b, '"'))
    return -1;
  for (i = 0; i < n; i++) {
    uint8_t c = s[i];
    switch (c) {
    case '\a':
      if (buf_str(b, "\\a"))
        return -1;
      break;
    case '\b':
      if (buf_str(b, "\\b"))
        return -1;
      break;
    case '\f':
      if (buf_str(b, "\\f"))
        return -1;
      break;
    case '\n':
      if (buf_str(b, "\\n"))
        return -1;
      break;
    case '\r':
      if (buf_str(b, "\\r"))
        return -1;
      break;
    case '\t':
      if (buf_str(b, "\\t"))
        return -1;
      break;
    case '\v':
      if (buf_str(b, "\\v"))
        return -1;
      break;
    case '\'':
      if (buf_str(b, "\\'"))
        return -1;
      break;
    case '\"':
      if (buf_str(b, "\\\""))
        return -1;
      break;
    case '\\':
      if (buf_str(b, "\\\\"))
        return -1;
      break;
    default:
      if (c < 0x20 || c == 0x7f || (is_binary && c > 0x7f)) {
        char oct[5];
        oct[0] = '\\';
        oct[1] = (char)('0' + (c / 64));
        oct[2] = (char)('0' + ((c / 8) % 8));
        oct[3] = (char)('0' + (c % 8));
        oct[4] = 0;
        if (buf_add(b, oct, 4))
          return -1;
      } else if (buf_ch(b, (char)c)) {
        return -1;
      }
      break;
    }
  }
  return buf_ch(b, '"');
}

static int quote_json_string(buf_t *b, const char *s, size_t n) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  if (buf_ch(b, '"'))
    return -1;
  for (i = 0; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    switch (c) {
    case '\"':
      if (buf_str(b, "\\\""))
        return -1;
      break;
    case '\\':
      if (buf_str(b, "\\\\"))
        return -1;
      break;
    case '\b':
      if (buf_str(b, "\\b"))
        return -1;
      break;
    case '\f':
      if (buf_str(b, "\\f"))
        return -1;
      break;
    case '\n':
      if (buf_str(b, "\\n"))
        return -1;
      break;
    case '\r':
      if (buf_str(b, "\\r"))
        return -1;
      break;
    case '\t':
      if (buf_str(b, "\\t"))
        return -1;
      break;
    default:
      if (c < 0x20) {
        char u[7];
        u[0] = '\\';
        u[1] = 'u';
        u[2] = '0';
        u[3] = '0';
        u[4] = hex[c / 16];
        u[5] = hex[c % 16];
        u[6] = 0;
        if (buf_add(b, u, 6))
          return -1;
      } else if (buf_ch(b, (char)c)) {
        return -1;
      }
      break;
    }
  }
  return buf_ch(b, '"');
}

static int encode_base64(buf_t *b, const uint8_t *data, size_t len) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0;
  if (buf_ch(b, '"'))
    return -1;
  while (i + 3 <= len) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                 (uint32_t)data[i + 2];
    char out[4];
    out[0] = tbl[(v >> 18) & 0x3f];
    out[1] = tbl[(v >> 12) & 0x3f];
    out[2] = tbl[(v >> 6) & 0x3f];
    out[3] = tbl[v & 0x3f];
    if (buf_add(b, out, 4))
      return -1;
    i += 3;
  }
  if (len - i == 1) {
    uint32_t v = (uint32_t)data[i] << 16;
    char out[4];
    out[0] = tbl[(v >> 18) & 0x3f];
    out[1] = tbl[(v >> 12) & 0x3f];
    out[2] = '=';
    out[3] = '=';
    if (buf_add(b, out, 4))
      return -1;
  } else if (len - i == 2) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    char out[4];
    out[0] = tbl[(v >> 18) & 0x3f];
    out[1] = tbl[(v >> 12) & 0x3f];
    out[2] = tbl[(v >> 6) & 0x3f];
    out[3] = '=';
    if (buf_add(b, out, 4))
      return -1;
  }
  return buf_ch(b, '"');
}

static int encode_hex(buf_t *b, const uint8_t *data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  if (buf_ch(b, '"'))
    return -1;
  for (i = 0; i < len; i++) {
    char pair[2];
    pair[0] = hex[data[i] >> 4];
    pair[1] = hex[data[i] & 0x0f];
    if (buf_add(b, pair, 2))
      return -1;
  }
  return buf_ch(b, '"');
}

/* --- Text encoder (KJ stringify / prettyPrint) --- */

enum { TMODE_BARE = 0, TMODE_PREFIXED = 1 };

typedef struct {
  char **items;
  size_t *lens;
  uint32_t count;
  uint32_t cap;
} piece_list_t;

static void free_pieces(piece_list_t *pl) {
  uint32_t i;
  for (i = 0; i < pl->count; i++)
    free(pl->items[i]);
  free(pl->items);
  free(pl->lens);
  memset(pl, 0, sizeof(*pl));
}

/* Set when a list growth malloc fails, so the public encoder can
 * report allocation failure instead of a type error. One slot per
 * thread: two encoders must not share it. */
static _Thread_local int encode_oom;

static int add_piece(piece_list_t *pl, char *s, size_t len) {
  if (pl->count == pl->cap) {
    uint32_t ncap;
    char **ni;
    size_t *nl;
    if (pl->cap > UINT32_MAX / 2u)
      return -1;
    ncap = pl->cap ? pl->cap * 2u : 8u;
#if SIZE_MAX <= UINT32_MAX
    if ((size_t)ncap > SIZE_MAX / sizeof(char *) ||
        (size_t)ncap > SIZE_MAX / sizeof(size_t))
      return -1;
#endif
    ni = (char **)malloc((size_t)ncap * sizeof(char *));
    nl = (size_t *)malloc((size_t)ncap * sizeof(size_t));
    if (!ni || !nl) {
      free(ni);
      free(nl);
      encode_oom = 1;
      return -1;
    }
    if (pl->count) {
      memcpy(ni, pl->items, (size_t)pl->count * sizeof(char *));
      memcpy(nl, pl->lens, (size_t)pl->count * sizeof(size_t));
    }
    free(pl->items);
    free(pl->lens);
    pl->items = ni;
    pl->lens = nl;
    pl->cap = ncap;
  }
  pl->items[pl->count] = s;
  pl->lens[pl->count] = len;
  pl->count++;
  return 0;
}

static int delimit_text(buf_t *b, const piece_list_t *pl, int indent, int mode,
                        int is_record) {
  uint32_t i;
  int inline_ok = (indent == 0);
  if (!inline_ok) {
    size_t total = 0;
    inline_ok = 1;
    for (i = 0; i < pl->count; i++) {
      if (pl->lens[i] > 24 || memchr(pl->items[i], '\n', pl->lens[i]) != NULL) {
        inline_ok = 0;
        break;
      }
      if (is_record) {
        total += pl->lens[i];
        if (total > 64) {
          inline_ok = 0;
          break;
        }
      }
    }
  }
  if (pl->count == 0)
    return 0;
  if (inline_ok) {
    for (i = 0; i < pl->count; i++) {
      if (i > 0 && buf_str(b, ", "))
        return -1;
      if (buf_add(b, pl->items[i], pl->lens[i]))
        return -1;
    }
    return 0;
  }
  if (mode == TMODE_BARE) {
    if (buf_ch(b, ' '))
      return -1;
  } else {
    int sp;
    if (buf_ch(b, '\n'))
      return -1;
    for (sp = 0; sp < indent * 2; sp++) {
      if (buf_ch(b, ' '))
        return -1;
    }
  }
  for (i = 0; i < pl->count; i++) {
    if (i > 0) {
      int sp;
      if (buf_str(b, ",\n"))
        return -1;
      for (sp = 0; sp < indent * 2; sp++) {
        if (buf_ch(b, ' '))
          return -1;
      }
    }
    if (buf_add(b, pl->items[i], pl->lens[i]))
      return -1;
  }
  return buf_ch(b, ' ');
}

/* First reader error met while encoding. A corrupt pointer reads as
 * null so the walk can go on, and the public encoder then fails with
 * this code, as `capnp convert` fails after printing defaults. */
static _Thread_local int encode_wire_err;
/* Set when the walk met a segment that is not there to read, which
 * `capnp convert` cannot recover from. */
static _Thread_local int encode_io_err;
/* Set by capnp_dyn_encode_recover. `capnp` has no capability context, so
 * reading any capability fails there and ends the conversion. */
static _Thread_local int encode_no_caps;
static _Thread_local int encode_cap_err;

static void note_wire(int rc) {
  if (rc == CAPNP_ERR_IO)
    encode_io_err = 1;
  if (rc != CAPNP_OK && encode_wire_err == CAPNP_OK)
    encode_wire_err = rc;
}

static void null_ptr(capnp_ptr_t *p) {
  memset(p, 0, sizeof(*p));
  p->kind = CAPNP_PK_NULL;
}

static void enc_getp(const capnp_ptr_t *s, uint16_t idx, capnp_ptr_t *out) {
  int rc;
  null_ptr(out);
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return;
  rc = capnp_getp(s, idx, out);
  if (rc != CAPNP_OK) {
    note_wire(rc);
    null_ptr(out);
  }
}

static void enc_expect_struct(capnp_ptr_t *p) {
  if (p->kind != CAPNP_PK_NULL && p->kind != CAPNP_PK_STRUCT) {
    note_wire(CAPNP_ERR_KIND);
    null_ptr(p);
  }
}

static int elem_esize(const rtype_t *elem) {
  if (elem->list_depth > 0)
    return CAPNP_SZ_PTR;
  switch (elem->which) {
  case TY_VOID:
    return CAPNP_SZ_VOID;
  case TY_BOOL:
    return CAPNP_SZ_BIT;
  case TY_I8:
  case TY_U8:
    return CAPNP_SZ_BYTE;
  case TY_I16:
  case TY_U16:
  case TY_ENUM:
    return CAPNP_SZ_TWO;
  case TY_I32:
  case TY_U32:
  case TY_F32:
    return CAPNP_SZ_FOUR;
  case TY_I64:
  case TY_U64:
  case TY_F64:
    return CAPNP_SZ_EIGHT;
  case TY_STRUCT:
    return CAPNP_SZ_COMPOSITE;
  default:
    return CAPNP_SZ_PTR;
  }
}

static uint32_t esize_data_bits(int es) {
  static const uint32_t bits[] = {0, 1, 8, 16, 32, 64, 0, 0};
  return bits[es & 7];
}

/* readListPointer's element checks: a wider element reads as its low
 * bytes, a narrower one or a bit list read as anything else fails. */
static int list_fits(const capnp_ptr_t *l, int want) {
  if (l->esize == CAPNP_SZ_COMPOSITE) {
    if (want == CAPNP_SZ_BIT)
      return 0;
    if (want == CAPNP_SZ_PTR)
      return l->pwords > 0;
    if (want != CAPNP_SZ_VOID && want != CAPNP_SZ_COMPOSITE)
      return l->dwords > 0;
    return 1;
  }
  if (l->esize == CAPNP_SZ_BIT && want != CAPNP_SZ_BIT)
    return 0;
  if (want == CAPNP_SZ_COMPOSITE)
    return 1;
  if (esize_data_bits(want) > esize_data_bits(l->esize))
    return 0;
  return want != CAPNP_SZ_PTR || l->esize == CAPNP_SZ_PTR;
}

static void enc_expect_cap(capnp_ptr_t *p) {
  if (encode_no_caps)
    encode_cap_err = 1;
  if (p->kind != CAPNP_PK_NULL && p->kind != CAPNP_PK_CAP) {
    note_wire(CAPNP_ERR_KIND);
    null_ptr(p);
  }
}

static void enc_expect_list(capnp_ptr_t *p, const rtype_t *elem) {
  if (p->kind == CAPNP_PK_NULL)
    return;
  if (p->kind != CAPNP_PK_LIST || !list_fits(p, elem_esize(elem))) {
    note_wire(CAPNP_ERR_KIND);
    null_ptr(p);
  }
}

static void enc_expect_struct_list(capnp_ptr_t *p) {
  if (p->kind == CAPNP_PK_NULL)
    return;
  if (p->kind != CAPNP_PK_LIST || !list_fits(p, CAPNP_SZ_COMPOSITE)) {
    note_wire(CAPNP_ERR_KIND);
    null_ptr(p);
  }
}

static void enc_get_text(const capnp_ptr_t *s, uint16_t idx, const char **t,
                         size_t *tn) {
  int rc;
  *t = "";
  *tn = 0;
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return;
  rc = capnp_get_text(s, idx, t, tn);
  if (rc != CAPNP_OK) {
    note_wire(rc);
    *t = "";
    *tn = 0;
  }
}

/* Pointer element of a list. A struct list reads its first pointer. */
static void enc_list_ptr(const capnp_ptr_t *list, uint32_t i,
                         capnp_ptr_t *out) {
  capnp_ptr_t el;
  int rc;
  null_ptr(out);
  if (list->esize == CAPNP_SZ_COMPOSITE) {
    rc = capnp_list_get_struct(list, i, &el);
    if (rc == CAPNP_OK)
      rc = capnp_getp(&el, 0, out);
  } else {
    rc = capnp_list_getp(list, i, out);
  }
  if (rc != CAPNP_OK) {
    note_wire(rc);
    null_ptr(out);
  }
}

static void enc_list_struct(const capnp_ptr_t *list, uint32_t i,
                            capnp_ptr_t *out) {
  int rc = capnp_list_get_struct(list, i, out);
  if (rc != CAPNP_OK) {
    note_wire(rc);
    null_ptr(out);
  }
}

static void enc_list_text(const capnp_ptr_t *list, uint32_t i, const char **t,
                          size_t *tn) {
  int rc = capnp_list_get_text(list, i, t, tn);
  if (rc != CAPNP_OK) {
    note_wire(rc);
    *t = "";
    *tn = 0;
  }
}

static void enc_list_data(const capnp_ptr_t *list, uint32_t i,
                          const uint8_t **p, size_t *pn) {
  capnp_ptr_t el;
  int rc = capnp_list_get_struct(list, i, &el);
  *p = NULL;
  *pn = 0;
  if (rc == CAPNP_OK)
    rc = capnp_get_data(&el, 0, p, pn);
  if (rc != CAPNP_OK) {
    note_wire(rc);
    *p = NULL;
    *pn = 0;
  }
}

/* A Text or Data field. readTextPointer and readDataPointer fall back
 * to the default for a null pointer and for one they cannot read. */
static void enc_field_blob(const capnp_ptr_t *s, const dyn_field_t *f,
                           uint16_t which, const void **p, size_t *n) {
  capnp_ptr_t q;
  enc_getp(s, (uint16_t)f->offset, &q);
  if (q.kind != CAPNP_PK_NULL) {
    int rc = which == TY_TEXT
                 ? capnp_get_text(s, (uint16_t)f->offset, (const char **)p, n)
                 : capnp_get_data(s, (uint16_t)f->offset,
                                  (const uint8_t **)p, n);
    if (rc == CAPNP_OK)
      return;
    note_wire(rc);
  }
  *p = which == TY_TEXT ? (const void *)"" : NULL;
  *n = 0;
  if (f->has_def_val && capnp_get_u16(&f->def_val, 0, 0xffff) == which) {
    if (which == TY_TEXT)
      capnp_get_text(&f->def_val, 0, (const char **)p, n);
    else
      capnp_get_data(&f->def_val, 0, (const uint8_t **)p, n);
  }
}

static int far_segment_ok(const capnp_message_t *m, uint32_t seg) {
  if (seg < m->nsegs && !m->segs[seg].data) {
    note_wire(CAPNP_ERR_IO);
    return 0;
  }
  /* tryGetSegment knows no segment of zero words. */
  if (seg >= m->nsegs || m->segs[seg].words == 0) {
    note_wire(CAPNP_ERR_SEGMENT);
    return 0;
  }
  return 1;
}

/* DynamicStruct::Reader::has for a pointer field: PointerReader::isNull,
 * which follows far pointers but not the object. A pointer it cannot
 * follow is absent after a recoverable error; one to a corrupt object is
 * present and then reads as its default. */
static int pointer_present(const capnp_ptr_t *s, uint32_t idx) {
  const capnp_message_t *m = s->msg;
  uint64_t w;
  if (idx >= s->pwords)
    return 0;
  w = capnp_load_le64(m->segs[s->seg].data +
                      (s->word + (size_t)s->dwords + idx) * CAPNP_WORD_BYTES);
  if (w == 0)
    return 0;
  if (capnp_wp_kind(w) == CAPNP_WK_FAR) {
    uint32_t seg = capnp_wp_far_seg(w);
    size_t pos = capnp_wp_far_off(w);
    int two = capnp_wp_far_two(w);
    uint64_t pad;
    if (!far_segment_ok(m, seg))
      return 0;
    if (pos + (two ? 2u : 1u) > m->segs[seg].words) {
      note_wire(CAPNP_ERR_BOUNDS);
      return 0;
    }
    pad = capnp_load_le64(m->segs[seg].data + pos * CAPNP_WORD_BYTES);
    if (two) {
      if (!far_segment_ok(m, capnp_wp_far_seg(pad)))
        return 0;
      if (capnp_wp_kind(pad) != CAPNP_WK_FAR) {
        note_wire(CAPNP_ERR_KIND);
        return 0;
      }
      w = capnp_load_le64(m->segs[seg].data + (pos + 1u) * CAPNP_WORD_BYTES);
    } else {
      w = pad;
    }
    if (capnp_wp_kind(w) == CAPNP_WK_FAR) {
      note_wire(CAPNP_ERR_KIND);
      return 0;
    }
  }
  if (capnp_wp_kind(w) == CAPNP_WK_CAP && (uint32_t)w != CAPNP_WK_CAP) {
    note_wire(CAPNP_ERR_KIND);
    return 0;
  }
  return 1;
}

/* A struct pointer that is null or could not be read gives C++ an empty
 * StructReader, whose fields all read as their defaults. */
static const capnp_ptr_t *as_struct(const capnp_ptr_t *s, capnp_ptr_t *tmp) {
  if (s && s->kind != CAPNP_PK_NULL)
    return s;
  memset(tmp, 0, sizeof(*tmp));
  if (s)
    *tmp = *s;
  tmp->kind = CAPNP_PK_STRUCT;
  tmp->dwords = 0;
  tmp->pwords = 0;
  tmp->data_bits = 0;
  tmp->body_byte = 0;
  return tmp;
}

static int field_has_wire(capnp_dyn_t *d, const capnp_ptr_t *s,
                          const dyn_field_t *f, const brand_env_t *env,
                          int non_null) {
  rtype_t ft;
  if (f->is_group)
    return 1;
  if (!s || s->kind != CAPNP_PK_STRUCT)
    return 0;
  if (resolve_field_type(d, f, env, &ft))
    return 0;
  if (ft.list_depth > 0 || is_ptr_type(ft.which)) {
    free_rtype(&ft);
    return pointer_present(s, f->offset);
  }
  if (non_null) {
    free_rtype(&ft);
    return 1;
  }
  {
    uint32_t b = data_byte(ft.which, f->offset);
    uint64_t wire = 0;
    uint16_t w = ft.which;
    free_rtype(&ft);
    switch (w) {
    case TY_VOID:
      return 0;
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
      return 0;
    }
    return wire != 0;
  }
}

static int encode_text_struct(buf_t *b, capnp_dyn_t *d,
                              const dyn_struct_t *st, const brand_env_t *env,
                              const capnp_ptr_t *s, int indent, int mode,
                              int depth);

static int encode_text_enum(buf_t *b, capnp_dyn_t *d, uint64_t enum_id,
                            uint16_t ord) {
  dyn_enum_t *en = find_enum_id(d, enum_id);
  if (en && ord < en->nvals)
    return buf_str(b, en->vals[ord].name);
  if (buf_ch(b, '(') || buf_fmt_u(b, ord) || buf_ch(b, ')'))
    return -1;
  return 0;
}

static int encode_text_list(buf_t *b, capnp_dyn_t *d,
                            const rtype_t *elem_rt, const capnp_ptr_t *list,
                            int indent, int mode, int depth) {
  piece_list_t pl;
  uint32_t i, n;
  int next_ind = indent ? indent + 1 : 0;
  if (depth > value_depth)
    return -1;
  if (buf_ch(b, '['))
    return -1;
  if (!list || list->kind != CAPNP_PK_LIST)
    return buf_ch(b, ']');
  memset(&pl, 0, sizeof(pl));
  n = capnp_list_len(list);
  for (i = 0; i < n; i++) {
    buf_t eb;
    memset(&eb, 0, sizeof(eb));
    if (elem_rt->list_depth > 0) {
      capnp_ptr_t sub;
      rtype_t inner = *elem_rt;
      inner.list_depth--;
      enc_list_ptr(list, i, &sub);
      enc_expect_list(&sub, &inner);
      if (encode_text_list(&eb, d, &inner, &sub, next_ind, TMODE_BARE,
                           depth + 1)) {
        free(eb.data);
        free_pieces(&pl);
        return -1;
      }
    } else {
      switch (elem_rt->which) {
      case TY_VOID:
        if (buf_str(&eb, "void")) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_BOOL:
        if (buf_str(&eb, capnp_list_get_bool(list, i, 0) ? "true" : "false")) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_I8:
        if (buf_fmt_i(&eb, (int8_t)capnp_list_get_u8(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_U8:
        if (buf_fmt_u(&eb, capnp_list_get_u8(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_I16:
        if (buf_fmt_i(&eb, (int16_t)capnp_list_get_u16(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_U16:
        if (buf_fmt_u(&eb, capnp_list_get_u16(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_I32:
        if (buf_fmt_i(&eb, (int32_t)capnp_list_get_u32(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_U32:
        if (buf_fmt_u(&eb, capnp_list_get_u32(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_I64:
        if (buf_fmt_i(&eb, (int64_t)capnp_list_get_u64(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_U64:
        if (buf_fmt_u(&eb, capnp_list_get_u64(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_F32: {
        char tmp[64];
        fmt_f32(capnp_list_get_f32(list, i, 0), tmp, sizeof(tmp));
        if (buf_str(&eb, tmp)) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      }
      case TY_F64: {
        char tmp[64];
        fmt_f64(capnp_list_get_f64(list, i, 0), tmp, sizeof(tmp));
        if (buf_str(&eb, tmp)) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      }
      case TY_ENUM:
        if (encode_text_enum(&eb, d, elem_rt->type_id,
                             capnp_list_get_u16(list, i, 0))) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      case TY_TEXT: {
        const char *t = "";
        size_t tn = 0;
        enc_list_text(list, i, &t, &tn);
        if (quote_text_capnp(&eb, (const uint8_t *)t, tn, 0)) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      }
      case TY_DATA: {
        const uint8_t *p = NULL;
        size_t pn = 0;
        enc_list_data(list, i, &p, &pn);
        if (quote_text_capnp(&eb, p, pn, 1)) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      }
      case TY_STRUCT: {
        capnp_ptr_t el;
        dyn_struct_t *es = find_struct_id(d, elem_rt->type_id);
        if (es)
          enc_list_struct(list, i, &el);
        if (!es || encode_text_struct(&eb, d, es, &elem_rt->brand, &el,
                                      next_ind, TMODE_BARE, depth + 1)) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      }
      case TY_IFACE: {
        capnp_ptr_t c;
        enc_list_ptr(list, i, &c);
        enc_expect_cap(&c);
        if (buf_str(&eb, "<external capability>")) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      }
      case TY_ANY:
        if (buf_str(&eb, "<opaque pointer>")) {
          free(eb.data);
          free_pieces(&pl);
          return -1;
        }
        break;
      default:
        free(eb.data);
        free_pieces(&pl);
        return -1;
      }
    }
    if (!eb.data)
      eb.data = (char *)calloc(1, 1);
    if (!eb.data || add_piece(&pl, eb.data, eb.n)) {
      free(eb.data);
      free_pieces(&pl);
      return -1;
    }
  }
  if (delimit_text(b, &pl, indent, mode, 0)) {
    free_pieces(&pl);
    return -1;
  }
  free_pieces(&pl);
  return buf_ch(b, ']');
}

static int encode_text_field_value(buf_t *b, capnp_dyn_t *d,
                                   const dyn_field_t *f,
                                   const brand_env_t *env,
                                   const capnp_ptr_t *s, int indent,
                                   int depth) {
  rtype_t ft;
  int rc = 0;
  if (f->is_group) {
    dyn_struct_t *g = find_struct_id(d, f->group_id);
    if (!g)
      return -1;
    return encode_text_struct(b, d, g, env, s, indent, TMODE_PREFIXED,
                              depth + 1);
  }
  if (resolve_field_type(d, f, env, &ft))
    return -1;
  if (ft.list_depth > 0) {
    capnp_ptr_t list;
    rtype_t elem = ft;
    elem.list_depth--;
    enc_getp(s, (uint16_t)f->offset, &list);
    enc_expect_list(&list, &elem);
    if (list.kind == CAPNP_PK_NULL && f->has_def_val)
      capnp_getp(&f->def_val, 0, &list);
    rc = encode_text_list(b, d, &elem, &list, indent, TMODE_PREFIXED,
                          depth + 1);
    free_rtype(&ft);
    return rc;
  }
  switch (ft.which) {
  case TY_VOID:
    rc = buf_str(b, "void");
    break;
  case TY_BOOL: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, TY_BOOL, &bits);
    rc = buf_str(b, bits ? "true" : "false");
    break;
  }
  case TY_I8:
  case TY_I16:
  case TY_I32:
  case TY_I64: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, ft.which, &bits);
    rc = buf_fmt_i(b, sext(bits, ft.which));
    break;
  }
  case TY_U8:
  case TY_U16:
  case TY_U32:
  case TY_U64: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, ft.which, &bits);
    rc = buf_fmt_u(b, bits);
    break;
  }
  case TY_F32: {
    uint64_t bits = 0;
    uint32_t u;
    float fv;
    char tmp[64];
    logical_bits_for_type(s, f, TY_F32, &bits);
    u = (uint32_t)bits;
    memcpy(&fv, &u, sizeof(fv));
    fmt_f32(fv, tmp, sizeof(tmp));
    rc = buf_str(b, tmp);
    break;
  }
  case TY_F64: {
    uint64_t bits = 0;
    double dv;
    char tmp[64];
    logical_bits_for_type(s, f, TY_F64, &bits);
    memcpy(&dv, &bits, sizeof(dv));
    fmt_f64(dv, tmp, sizeof(tmp));
    rc = buf_str(b, tmp);
    break;
  }
  case TY_ENUM: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, TY_ENUM, &bits);
    rc = encode_text_enum(b, d, ft.type_id, (uint16_t)bits);
    break;
  }
  case TY_TEXT: {
    const char *t = "";
    size_t tn = 0;
    enc_field_blob(s, f, TY_TEXT, (const void **)&t, &tn);
    rc = quote_text_capnp(b, (const uint8_t *)t, tn, 0);
    break;
  }
  case TY_DATA: {
    const uint8_t *p = NULL;
    size_t pn = 0;
    enc_field_blob(s, f, TY_DATA, (const void **)&p, &pn);
    rc = quote_text_capnp(b, p, pn, 1);
    break;
  }
  case TY_STRUCT: {
    capnp_ptr_t child;
    dyn_struct_t *cs = find_struct_id(d, ft.type_id);
    memset(&child, 0, sizeof(child));
    if (!cs) {
      free_rtype(&ft);
      return -1;
    }
    enc_getp(s, (uint16_t)f->offset, &child);
    enc_expect_struct(&child);
    if (child.kind == CAPNP_PK_NULL && f->has_def_val)
      capnp_getp(&f->def_val, 0, &child);
    rc = encode_text_struct(b, d, cs, &ft.brand, &child, indent,
                            TMODE_PREFIXED, depth + 1);
    break;
  }
  case TY_IFACE: {
    capnp_ptr_t c;
    enc_getp(s, (uint16_t)f->offset, &c);
    enc_expect_cap(&c);
    rc = buf_str(b, "<external capability>");
    break;
  }
  case TY_ANY:
    rc = buf_str(b, "<opaque pointer>");
    break;
  default:
    rc = -1;
    break;
  }
  free_rtype(&ft);
  return rc;
}

static int encode_text_struct(buf_t *b, capnp_dyn_t *d,
                              const dyn_struct_t *st, const brand_env_t *env,
                              const capnp_ptr_t *s, int indent, int mode,
                              int depth) {
  piece_list_t pl;
  uint32_t i;
  uint16_t which = 0xffff;
  int next_ind = indent ? indent + 1 : 0;
  capnp_ptr_t empty;
  if (depth > value_depth)
    return -1;
  s = as_struct(s, &empty);
  if (buf_ch(b, '('))
    return -1;
  memset(&pl, 0, sizeof(pl));
  struct_which(s, st, &which);
  for (i = 0; i < st->nfields; i++) {
    const dyn_field_t *f = &st->fields[i];
    buf_t fb;
    int has = field_has_wire(d, s, f, env, 1);
    if (f->disc != 0xffff && st->disc_count) {
      if (f->disc != which)
        continue;
      if (f->disc == 0 && !has)
        continue;
    } else if (!has) {
      continue;
    }
    memset(&fb, 0, sizeof(fb));
    if (buf_str(&fb, f->name) || buf_str(&fb, " = ") ||
        encode_text_field_value(&fb, d, f, env, s, next_ind, depth)) {
      free(fb.data);
      free_pieces(&pl);
      return -1;
    }
    if (add_piece(&pl, fb.data, fb.n)) {
      free(fb.data);
      free_pieces(&pl);
      return -1;
    }
  }
  if (delimit_text(b, &pl, indent, mode, 1)) {
    free_pieces(&pl);
    return -1;
  }
  free_pieces(&pl);
  return buf_ch(b, ')');
}
/* --- JSON encoder (JsonCodec + AnnotatedHandler + JsonValueHandler) --- */

typedef struct {
  char **items;
  size_t *lens;
  int *multiline;
  uint32_t count;
  uint32_t cap;
} json_list_t;

static void free_json_list(json_list_t *jl) {
  uint32_t i;
  for (i = 0; i < jl->count; i++)
    free(jl->items[i]);
  free(jl->items);
  free(jl->lens);
  free(jl->multiline);
  memset(jl, 0, sizeof(*jl));
}

static int add_json_item(json_list_t *jl, char *s, size_t len, int multi) {
  if (jl->count == jl->cap) {
    uint32_t ncap;
    char **ni;
    size_t *nl;
    int *nm;
    if (jl->cap > UINT32_MAX / 2u)
      return -1;
    ncap = jl->cap ? jl->cap * 2u : 8u;
#if SIZE_MAX <= UINT32_MAX
    if ((size_t)ncap > SIZE_MAX / sizeof(char *) ||
        (size_t)ncap > SIZE_MAX / sizeof(size_t) ||
        (size_t)ncap > SIZE_MAX / sizeof(int))
      return -1;
#endif
    ni = (char **)malloc((size_t)ncap * sizeof(char *));
    nl = (size_t *)malloc((size_t)ncap * sizeof(size_t));
    nm = (int *)malloc((size_t)ncap * sizeof(int));
    if (!ni || !nl || !nm) {
      free(ni);
      free(nl);
      free(nm);
      encode_oom = 1;
      return -1;
    }
    if (jl->count) {
      memcpy(ni, jl->items, (size_t)jl->count * sizeof(char *));
      memcpy(nl, jl->lens, (size_t)jl->count * sizeof(size_t));
      memcpy(nm, jl->multiline, (size_t)jl->count * sizeof(int));
    }
    free(jl->items);
    free(jl->lens);
    free(jl->multiline);
    jl->items = ni;
    jl->lens = nl;
    jl->multiline = nm;
    jl->cap = ncap;
  }
  jl->items[jl->count] = s;
  jl->lens[jl->count] = len;
  jl->multiline[jl->count] = multi;
  jl->count++;
  return 0;
}

static int join_json_list(buf_t *b, const json_list_t *jl, int indent,
                          int *multiline, int has_prefix, int pretty) {
  size_t max_child = 0;
  int has_multi = 0;
  uint32_t i;
  for (i = 0; i < jl->count; i++) {
    if (jl->lens[i] > max_child)
      max_child = jl->lens[i];
    if (jl->multiline[i])
      has_multi = 1;
  }
  if (jl->count == 0)
    return 0;
  if (!pretty) {
    for (i = 0; i < jl->count; i++) {
      if (i > 0 && buf_ch(b, ','))
        return -1;
      if (buf_add(b, jl->items[i], jl->lens[i]))
        return -1;
    }
    return 0;
  }
  if (jl->count > 1 && (has_multi || max_child > 50)) {
    int sp;
    *multiline = 1;
    if (has_prefix) {
      if (buf_ch(b, '\n'))
        return -1;
      for (sp = 0; sp < (indent + 1) * 2; sp++) {
        if (buf_ch(b, ' '))
          return -1;
      }
    } else if (buf_ch(b, ' ')) {
      return -1;
    }
    for (i = 0; i < jl->count; i++) {
      if (i > 0) {
        if (buf_str(b, ",\n"))
          return -1;
        for (sp = 0; sp < (indent + 1) * 2; sp++) {
          if (buf_ch(b, ' '))
            return -1;
        }
      }
      if (buf_add(b, jl->items[i], jl->lens[i]))
        return -1;
    }
    return buf_ch(b, ' ');
  }
  for (i = 0; i < jl->count; i++) {
    if (i > 0 && buf_str(b, ", "))
      return -1;
    if (buf_add(b, jl->items[i], jl->lens[i]))
      return -1;
  }
  return 0;
}

typedef struct {
  char name[256];
  int is_tag_text;
  char tag_text[128];
  const dyn_struct_t *st;
  const dyn_field_t *f;
  brand_env_t env;
  capnp_ptr_t s;
} flat_json_field_t;

typedef struct {
  flat_json_field_t *items;
  uint32_t count;
  uint32_t cap;
} flat_json_list_t;

static void free_flat_json_list(flat_json_list_t *fl) {
  uint32_t i;
  for (i = 0; i < fl->count; i++)
    free_brand_env(&fl->items[i].env);
  free(fl->items);
  memset(fl, 0, sizeof(*fl));
}

static int push_flat_json(flat_json_list_t *fl, const flat_json_field_t *item) {
  if (fl->count == fl->cap) {
    uint32_t ncap = fl->cap ? fl->cap * 2u : 8u;
    flat_json_field_t *ni = (flat_json_field_t *)realloc(
        fl->items, (size_t)ncap * sizeof(flat_json_field_t));
    if (!ni)
      return -1;
    fl->items = ni;
    fl->cap = ncap;
  }
  fl->items[fl->count] = *item;
  memset(&fl->items[fl->count].env, 0, sizeof(brand_env_t));
  if (clone_brand_env(&item->env, &fl->items[fl->count].env))
    return -1;
  fl->count++;
  return 0;
}

static int gather_json_fields(capnp_dyn_t *d, const dyn_struct_t *st,
                              const brand_env_t *env, const capnp_ptr_t *s,
                              const disc_opts_t *disc_override,
                              const char *union_decl_name, const char *prefix,
                              flat_json_list_t *out, int depth) {
  uint32_t i;
  uint16_t which = 0xffff;
  disc_opts_t dopts;
  const char *tag_name = NULL;
  capnp_ptr_t empty;

  if (depth > value_depth)
    return -1;
  s = as_struct(s, &empty);
  memset(&dopts, 0, sizeof(dopts));
  if (disc_override && disc_override->present)
    dopts = *disc_override;
  else if (st->disc_opts.present)
    dopts = st->disc_opts;
  if (dopts.present) {
    if (dopts.has_name)
      tag_name = dopts.name;
    else if (union_decl_name && union_decl_name[0])
      tag_name = union_decl_name;
  }

  /* 1. Non-union fields in schema order */
  for (i = 0; i < st->nfields; i++) {
    const dyn_field_t *f = &st->fields[i];
    if (f->disc != 0xffff && st->disc_count)
      continue;
    if (!field_has_wire(d, s, f, env, 1))
      continue;
    if (f->json_flatten) {
      char next_prefix[256];
      snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix ? prefix : "",
               f->flatten_prefix);
      if (f->is_group) {
        dyn_struct_t *g = find_struct_id(d, f->group_id);
        if (!g ||
            gather_json_fields(d, g, env, s, &f->sub_disc, f->name, next_prefix,
                               out, depth + 1))
          return -1;
      } else {
        rtype_t ft;
        capnp_ptr_t child;
        dyn_struct_t *cs;
        if (resolve_field_type(d, f, env, &ft))
          return -1;
        cs = find_struct_id(d, ft.type_id);
        enc_getp(s, (uint16_t)f->offset, &child);
        enc_expect_struct(&child);
        if (!cs ||
            gather_json_fields(d, cs, &ft.brand, &child, NULL, NULL,
                               next_prefix, out, depth + 1)) {
          free_rtype(&ft);
          return -1;
        }
        free_rtype(&ft);
      }
    } else {
      flat_json_field_t item;
      memset(&item, 0, sizeof(item));
      snprintf(item.name, sizeof(item.name), "%s%s", prefix ? prefix : "",
               f->json_name);
      item.st = st;
      item.f = f;
      item.env = *env;
      if (s)
        item.s = *s;
      if (push_flat_json(out, &item))
        return -1;
    }
  }

  /* 2. Active union field */
  if (st->disc_count) {
    const dyn_field_t *uf = NULL;
    struct_which(s, st, &which);
    for (i = 0; i < st->nfields; i++) {
      if (st->fields[i].disc == which) {
        uf = &st->fields[i];
        break;
      }
    }
    if (uf) {
      if (tag_name) {
        flat_json_field_t tag_item;
        memset(&tag_item, 0, sizeof(tag_item));
        snprintf(tag_item.name, sizeof(tag_item.name), "%s%s",
                 prefix ? prefix : "", tag_name);
        tag_item.is_tag_text = 1;
        snprintf(tag_item.tag_text, sizeof(tag_item.tag_text), "%s",
                 uf->disc_name);
        if (push_flat_json(out, &tag_item))
          return -1;
      }
      if (uf->json_flatten) {
        char next_prefix[256];
        snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix ? prefix : "",
                 uf->flatten_prefix);
        if (uf->is_group) {
          dyn_struct_t *g = find_struct_id(d, uf->group_id);
          if (!g ||
              gather_json_fields(d, g, env, s, &uf->sub_disc, uf->name,
                                 next_prefix, out, depth + 1))
            return -1;
        } else {
          rtype_t ft;
          capnp_ptr_t child;
          dyn_struct_t *cs;
          if (resolve_field_type(d, uf, env, &ft))
            return -1;
          cs = find_struct_id(d, ft.type_id);
          enc_getp(s, (uint16_t)uf->offset, &child);
          enc_expect_struct(&child);
          if (!cs ||
              gather_json_fields(d, cs, &ft.brand, &child, NULL, NULL,
                                 next_prefix, out, depth + 1)) {
            free_rtype(&ft);
            return -1;
          }
          free_rtype(&ft);
        }
      } else {
        rtype_t ft;
        if (resolve_field_type(d, uf, env, &ft))
          return -1;
        if (ft.list_depth == 0 && ft.which == TY_VOID && tag_name != NULL) {
          /* When we have an explicit union discriminant, we don't need to encode void fields. */
          free_rtype(&ft);
        } else {
          flat_json_field_t item;
          const char *fname = uf->json_name;
          if (dopts.present && dopts.has_value_name)
            fname = dopts.value_name;
          memset(&item, 0, sizeof(item));
          snprintf(item.name, sizeof(item.name), "%s%s", prefix ? prefix : "",
                   fname);
          item.st = st;
          item.f = uf;
          item.env = *env;
          if (s)
            item.s = *s;
          free_rtype(&ft);
          if (push_flat_json(out, &item))
            return -1;
        }
      }
    }
  }
  return 0;
}
static int encode_json_struct(buf_t *b, capnp_dyn_t *d,
                              const dyn_struct_t *st, const brand_env_t *env,
                              const capnp_ptr_t *s,
                              const disc_opts_t *disc_override,
                              const char *union_decl_name, int indent,
                              int *multiline, int has_prefix, int pretty,
                              int depth);

static int encode_json_enum(buf_t *b, capnp_dyn_t *d, uint64_t enum_id,
                            uint16_t ord) {
  dyn_enum_t *en = find_enum_id(d, enum_id);
  if (en && ord < en->nvals)
    return quote_json_string(b, en->vals[ord].json_name,
                             strlen(en->vals[ord].json_name));
  return buf_fmt_u(b, ord);
}

static int encode_json_float(buf_t *b, double v) {
  char tmp[64];
  if (isnan(v))
    return buf_str(b, "\"NaN\"");
  if (isinf(v))
    return buf_str(b, v < 0 ? "\"-Infinity\"" : "\"Infinity\"");
  fmt_f64(v, tmp, sizeof(tmp));
  return buf_str(b, tmp);
}

static int encode_json_data_bytes(buf_t *b, const uint8_t *data, size_t len,
                                  int mode, int indent, int *multiline,
                                  int has_prefix, int pretty) {
  json_list_t jl;
  size_t i;
  if (mode == 1)
    return encode_base64(b, data, len);
  if (mode == 2)
    return encode_hex(b, data, len);
  if (buf_ch(b, '['))
    return -1;
  memset(&jl, 0, sizeof(jl));
  for (i = 0; i < len; i++) {
    buf_t eb;
    memset(&eb, 0, sizeof(eb));
    if (buf_fmt_u(&eb, data[i]) || add_json_item(&jl, eb.data, eb.n, 0)) {
      free(eb.data);
      free_json_list(&jl);
      return -1;
    }
  }
  if (join_json_list(b, &jl, indent, multiline, has_prefix, pretty)) {
    free_json_list(&jl);
    return -1;
  }
  free_json_list(&jl);
  return buf_ch(b, ']');
}

/* Encode a capnp/compat/json.capnp:Value struct directly as raw JSON */
static int encode_json_value_struct(buf_t *b, const capnp_ptr_t *s, int indent,
                                    int *multiline, int has_prefix, int pretty,
                                    int depth) {
  uint16_t which;
  if (depth > value_depth)
    return -1;
  which = (s && s->kind == CAPNP_PK_STRUCT) ? capnp_get_u16(s, 0, 0) : 0;
  switch (which) {
  case 0: /* null */
    return buf_str(b, "null");
  case 1: /* boolean */
    return buf_str(b, (s && capnp_get_bool(s, 16, 0)) ? "true" : "false");
  case 2: { /* number */
    double dv = s ? capnp_get_f64(s, 8, 0.0) : 0.0;
    char tmp[64];
    fmt_f64(dv, tmp, sizeof(tmp));
    return buf_str(b, tmp);
  }
  case 3: { /* string */
    const char *t = "";
    size_t tn = 0;
    enc_get_text(s, 0, &t, &tn);
    return quote_json_string(b, t, tn);
  }
  case 4: { /* array @0 :List(Value) */
    capnp_ptr_t arr;
    json_list_t jl;
    uint32_t i, n = 0;
    int sub_indent;
    if (buf_ch(b, '['))
      return -1;
    enc_getp(s, 0, &arr);
    enc_expect_struct_list(&arr);
    if (arr.kind == CAPNP_PK_LIST)
      n = capnp_list_len(&arr);
    sub_indent = indent + (n > 1 ? 1 : 0);
    memset(&jl, 0, sizeof(jl));
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      buf_t eb;
      int cm = 0;
      memset(&eb, 0, sizeof(eb));
      enc_list_struct(&arr, i, &el);
      if (encode_json_value_struct(&eb, &el, sub_indent, &cm, 0, pretty,
                                   depth + 1) ||
          add_json_item(&jl, eb.data, eb.n, cm)) {
        free(eb.data);
        free_json_list(&jl);
        return -1;
      }
    }
    if (join_json_list(b, &jl, indent, multiline, has_prefix, pretty)) {
      free_json_list(&jl);
      return -1;
    }
    free_json_list(&jl);
    return buf_ch(b, ']');
  }
  case 5: { /* object @0 :List(Field) */
    capnp_ptr_t obj;
    json_list_t jl;
    uint32_t i, n = 0;
    int sub_indent;
    if (buf_ch(b, '{'))
      return -1;
    enc_getp(s, 0, &obj);
    enc_expect_struct_list(&obj);
    if (obj.kind == CAPNP_PK_LIST)
      n = capnp_list_len(&obj);
    sub_indent = indent + (n > 1 ? 1 : 0);
    memset(&jl, 0, sizeof(jl));
    for (i = 0; i < n; i++) {
      capnp_ptr_t fld, fval;
      const char *fn = "";
      size_t fn_len = 0;
      buf_t eb;
      int cm = 0;
      memset(&eb, 0, sizeof(eb));
      enc_list_struct(&obj, i, &fld);
      enc_get_text(&fld, 0, &fn, &fn_len);
      enc_getp(&fld, 1, &fval);
      enc_expect_struct(&fval);
      if (quote_json_string(&eb, fn, fn_len) ||
          buf_str(&eb, pretty ? ": " : ":") ||
          encode_json_value_struct(&eb, &fval, sub_indent, &cm, 1, pretty,
                                   depth + 1) ||
          add_json_item(&jl, eb.data, eb.n, cm)) {
        free(eb.data);
        free_json_list(&jl);
        return -1;
      }
    }
    if (join_json_list(b, &jl, indent, multiline, has_prefix, pretty)) {
      free_json_list(&jl);
      return -1;
    }
    free_json_list(&jl);
    return buf_ch(b, '}');
  }
  case 6: { /* call @0 :Call (function @0 :Text, params @1 :List(Value)) */
    capnp_ptr_t call, params;
    const char *fn = "";
    size_t fn_len = 0;
    json_list_t jl;
    uint32_t i, n = 0;
    int sub_indent;
    enc_getp(s, 0, &call);
    enc_expect_struct(&call);
    enc_get_text(&call, 0, &fn, &fn_len);
    enc_getp(&call, 1, &params);
    enc_expect_struct_list(&params);
    if (buf_add(b, fn, fn_len) || buf_ch(b, '('))
      return -1;
    if (params.kind == CAPNP_PK_LIST)
      n = capnp_list_len(&params);
    sub_indent = indent + (n > 1 ? 1 : 0);
    memset(&jl, 0, sizeof(jl));
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      buf_t eb;
      int cm = 0;
      memset(&eb, 0, sizeof(eb));
      enc_list_struct(&params, i, &el);
      if (encode_json_value_struct(&eb, &el, sub_indent, &cm, 1, pretty,
                                   depth + 1) ||
          add_json_item(&jl, eb.data, eb.n, cm)) {
        free(eb.data);
        free_json_list(&jl);
        return -1;
      }
    }
    if (join_json_list(b, &jl, indent, multiline, 1, pretty)) {
      free_json_list(&jl);
      return -1;
    }
    free_json_list(&jl);
    return buf_ch(b, ')');
  }
  case 7: { /* raw @0 :Text */
    const char *t = "";
    size_t tn = 0;
    enc_get_text(s, 0, &t, &tn);
    return buf_add(b, t, tn);
  }
  default:
    return -1;
  }
}

static int encode_json_list(buf_t *b, capnp_dyn_t *d,
                            const rtype_t *elem_rt, const capnp_ptr_t *list,
                            int indent, int *multiline, int has_prefix,
                            int pretty, int depth) {
  json_list_t jl;
  uint32_t i, n;
  int sub_indent;
  if (depth > value_depth)
    return -1;
  if (buf_ch(b, '['))
    return -1;
  if (!list || list->kind != CAPNP_PK_LIST)
    return buf_ch(b, ']');
  n = capnp_list_len(list);
  sub_indent = indent + (n > 1 ? 1 : 0);
  memset(&jl, 0, sizeof(jl));
  for (i = 0; i < n; i++) {
    buf_t eb;
    int cm = 0;
    int rc = 0;
    memset(&eb, 0, sizeof(eb));
    if (elem_rt->list_depth > 0) {
      capnp_ptr_t sub;
      rtype_t inner = *elem_rt;
      inner.list_depth--;
      enc_list_ptr(list, i, &sub);
      enc_expect_list(&sub, &inner);
      if (encode_json_list(&eb, d, &inner, &sub, sub_indent, &cm, 0, pretty,
                           depth + 1)) {
        free(eb.data);
        free_json_list(&jl);
        return -1;
      }
    } else {
      switch (elem_rt->which) {
      case TY_VOID:
        rc = buf_str(&eb, "null");
        break;
      case TY_BOOL:
        rc = buf_str(&eb, capnp_list_get_bool(list, i, 0) ? "true" : "false");
        break;
      case TY_I8:
        rc = buf_fmt_i(&eb, (int8_t)capnp_list_get_u8(list, i, 0));
        break;
      case TY_U8:
        rc = buf_fmt_u(&eb, capnp_list_get_u8(list, i, 0));
        break;
      case TY_I16:
        rc = buf_fmt_i(&eb, (int16_t)capnp_list_get_u16(list, i, 0));
        break;
      case TY_U16:
        rc = buf_fmt_u(&eb, capnp_list_get_u16(list, i, 0));
        break;
      case TY_I32:
        rc = buf_fmt_i(&eb, (int32_t)capnp_list_get_u32(list, i, 0));
        break;
      case TY_U32:
        rc = buf_fmt_u(&eb, capnp_list_get_u32(list, i, 0));
        break;
      case TY_I64:
        rc = buf_ch(&eb, '"') ||
             buf_fmt_i(&eb, (int64_t)capnp_list_get_u64(list, i, 0)) ||
             buf_ch(&eb, '"');
        break;
      case TY_U64:
        rc = buf_ch(&eb, '"') ||
             buf_fmt_u(&eb, capnp_list_get_u64(list, i, 0)) || buf_ch(&eb, '"');
        break;
      case TY_F32:
        rc = encode_json_float(&eb, (double)capnp_list_get_f32(list, i, 0));
        break;
      case TY_F64:
        rc = encode_json_float(&eb, capnp_list_get_f64(list, i, 0));
        break;
      case TY_ENUM:
        rc = encode_json_enum(&eb, d, elem_rt->type_id,
                              capnp_list_get_u16(list, i, 0));
        break;
      case TY_TEXT: {
        const char *t = "";
        size_t tn = 0;
        enc_list_text(list, i, &t, &tn);
        rc = quote_json_string(&eb, t, tn);
        break;
      }
      case TY_DATA: {
        const uint8_t *p = NULL;
        size_t pn = 0;
        enc_list_data(list, i, &p, &pn);
        rc = encode_json_data_bytes(&eb, p, pn, 0, sub_indent, &cm, 0, pretty);
        break;
      }
      case TY_STRUCT: {
        capnp_ptr_t el;
        dyn_struct_t *es = find_struct_id(d, elem_rt->type_id);
        if (es)
          enc_list_struct(list, i, &el);
        if (!es)
          rc = -1;
        else if (es->id == JSON_VALUE_TYPE_ID)
          rc = encode_json_value_struct(&eb, &el, sub_indent, &cm, 0, pretty,
                                        depth + 1);
        else
          rc = encode_json_struct(&eb, d, es, &elem_rt->brand, &el, NULL, NULL,
                                  sub_indent, &cm, 0, pretty, depth + 1);
        break;
      }
      default:
        rc = -1;
        break;
      }
      if (rc) {
        free(eb.data);
        free_json_list(&jl);
        return -1;
      }
    }
    if (!eb.data)
      eb.data = (char *)calloc(1, 1);
    if (!eb.data || add_json_item(&jl, eb.data, eb.n, cm)) {
      free(eb.data);
      free_json_list(&jl);
      return -1;
    }
  }
  if (join_json_list(b, &jl, indent, multiline, has_prefix, pretty)) {
    free_json_list(&jl);
    return -1;
  }
  free_json_list(&jl);
  return buf_ch(b, ']');
}

static int encode_json_field_item(buf_t *b, capnp_dyn_t *d,
                                  const flat_json_field_t *item, int indent,
                                  int *multiline, int pretty, int depth) {
  rtype_t ft;
  const dyn_field_t *f = item->f;
  const capnp_ptr_t *s = &item->s;
  int rc = 0;

  if (item->is_tag_text)
    return quote_json_string(b, item->tag_text, strlen(item->tag_text));

  if (f->is_group) {
    dyn_struct_t *g = find_struct_id(d, f->group_id);
    if (!g)
      return -1;
    return encode_json_struct(b, d, g, &item->env, s, &f->sub_disc, f->name,
                              indent, multiline, 1, pretty, depth + 1);
  }
  if (resolve_field_type(d, f, &item->env, &ft))
    return -1;
  if (ft.list_depth > 0) {
    capnp_ptr_t list;
    rtype_t elem = ft;
    elem.list_depth--;
    enc_getp(s, (uint16_t)f->offset, &list);
    enc_expect_list(&list, &elem);
    if (list.kind == CAPNP_PK_NULL && f->has_def_val)
      capnp_getp(&f->def_val, 0, &list);
    rc = encode_json_list(b, d, &elem, &list, indent, multiline, 1, pretty,
                          depth + 1);
    free_rtype(&ft);
    return rc;
  }
  switch (ft.which) {
  case TY_VOID:
    rc = buf_str(b, "null");
    break;
  case TY_BOOL: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, TY_BOOL, &bits);
    rc = buf_str(b, bits ? "true" : "false");
    break;
  }
  case TY_I8:
  case TY_I16:
  case TY_I32: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, ft.which, &bits);
    rc = buf_fmt_i(b, sext(bits, ft.which));
    break;
  }
  case TY_U8:
  case TY_U16:
  case TY_U32: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, ft.which, &bits);
    rc = buf_fmt_u(b, bits);
    break;
  }
  case TY_I64: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, TY_I64, &bits);
    rc = buf_ch(b, '"') || buf_fmt_i(b, (int64_t)bits) || buf_ch(b, '"');
    break;
  }
  case TY_U64: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, TY_U64, &bits);
    rc = buf_ch(b, '"') || buf_fmt_u(b, bits) || buf_ch(b, '"');
    break;
  }
  case TY_F32: {
    uint64_t bits = 0;
    uint32_t u;
    float fv;
    logical_bits_for_type(s, f, TY_F32, &bits);
    u = (uint32_t)bits;
    memcpy(&fv, &u, sizeof(fv));
    rc = encode_json_float(b, (double)fv);
    break;
  }
  case TY_F64: {
    uint64_t bits = 0;
    double dv;
    logical_bits_for_type(s, f, TY_F64, &bits);
    memcpy(&dv, &bits, sizeof(dv));
    rc = encode_json_float(b, dv);
    break;
  }
  case TY_ENUM: {
    uint64_t bits = 0;
    logical_bits_for_type(s, f, TY_ENUM, &bits);
    rc = encode_json_enum(b, d, ft.type_id, (uint16_t)bits);
    break;
  }
  case TY_TEXT: {
    const char *t = "";
    size_t tn = 0;
    enc_field_blob(s, f, TY_TEXT, (const void **)&t, &tn);
    rc = quote_json_string(b, t, tn);
    break;
  }
  case TY_DATA: {
    const uint8_t *p = NULL;
    size_t pn = 0;
    enc_field_blob(s, f, TY_DATA, (const void **)&p, &pn);
    rc = encode_json_data_bytes(b, p, pn, f->json_data_mode, indent, multiline,
                                1, pretty);
    break;
  }
  case TY_STRUCT: {
    capnp_ptr_t child;
    dyn_struct_t *cs = find_struct_id(d, ft.type_id);
    memset(&child, 0, sizeof(child));
    if (!cs) {
      free_rtype(&ft);
      return -1;
    }
    enc_getp(s, (uint16_t)f->offset, &child);
    enc_expect_struct(&child);
    if (child.kind == CAPNP_PK_NULL && f->has_def_val)
      capnp_getp(&f->def_val, 0, &child);
    if (cs->id == JSON_VALUE_TYPE_ID)
      rc = encode_json_value_struct(b, &child, indent, multiline, 1, pretty,
                                    depth + 1);
    else
      rc = encode_json_struct(b, d, cs, &ft.brand, &child, NULL, NULL, indent,
                              multiline, 1, pretty, depth + 1);
    break;
  }
  default:
    rc = -1;
    break;
  }
  free_rtype(&ft);
  return rc;
}

static int encode_json_struct(buf_t *b, capnp_dyn_t *d,
                              const dyn_struct_t *st, const brand_env_t *env,
                              const capnp_ptr_t *s,
                              const disc_opts_t *disc_override,
                              const char *union_decl_name, int indent,
                              int *multiline, int has_prefix, int pretty,
                              int depth) {
  flat_json_list_t fl;
  json_list_t jl;
  uint32_t i;
  int sub_indent;
  capnp_ptr_t empty;

  if (depth > value_depth)
    return -1;
  if (st->id == JSON_VALUE_TYPE_ID)
    return encode_json_value_struct(b, s, indent, multiline, has_prefix, pretty,
                                    depth);
  s = as_struct(s, &empty);
  memset(&fl, 0, sizeof(fl));
  if (gather_json_fields(d, st, env, s, disc_override, union_decl_name, "", &fl,
                         depth)) {
    free_flat_json_list(&fl);
    return -1;
  }
  if (buf_ch(b, '{')) {
    free_flat_json_list(&fl);
    return -1;
  }
  sub_indent = indent + (fl.count > 1 ? 1 : 0);
  memset(&jl, 0, sizeof(jl));
  for (i = 0; i < fl.count; i++) {
    buf_t eb;
    int cm = 0;
    memset(&eb, 0, sizeof(eb));
    if (quote_json_string(&eb, fl.items[i].name, strlen(fl.items[i].name)) ||
        buf_str(&eb, pretty ? ": " : ":") ||
        encode_json_field_item(&eb, d, &fl.items[i], sub_indent, &cm, pretty,
                               depth) ||
        add_json_item(&jl, eb.data, eb.n, cm)) {
      free(eb.data);
      free_json_list(&jl);
      free_flat_json_list(&fl);
      return -1;
    }
  }
  free_flat_json_list(&fl);
  if (join_json_list(b, &jl, indent, multiline, has_prefix, pretty)) {
    free_json_list(&jl);
    return -1;
  }
  free_json_list(&jl);
  return buf_ch(b, '}');
}
/* With @wire_err set, a walk that met reader errors keeps its output and
 * reports the first error there. */
static int encode_root_impl(capnp_dyn_t *d, const char *type_name,
                            const capnp_ptr_t *s, int json, int pretty,
                            char **out, size_t *len, int *wire_err) {
  rtype_t rt;
  dyn_struct_t *st = NULL;
  capnp_ptr_t empty;
  buf_t b;
  int rc;
  if (!d || !s || !out || !len)
    return CAPNP_ERR_ARG;
  if (s->kind == CAPNP_PK_NULL) {
    empty = *s;
    empty.kind = CAPNP_PK_STRUCT;
    empty.dwords = 0;
    empty.pwords = 0;
    empty.data_bits = 0;
    empty.body_byte = 0;
    s = &empty;
  }
  *out = NULL;
  *len = 0;
  encode_oom = 0;
  encode_wire_err = CAPNP_OK;
  encode_io_err = 0;
  encode_cap_err = 0;
  encode_no_caps = wire_err != NULL;
  if (wire_err)
    *wire_err = CAPNP_OK;
  value_depth = dyn_max_depth;
  if (s->kind != CAPNP_PK_STRUCT && s->kind != CAPNP_PK_NULL)
    return CAPNP_ERR_KIND;
  if (resolve_root_type_name(d, type_name, &rt, &st) != CAPNP_OK)
    return CAPNP_ERR_KIND;
  memset(&b, 0, sizeof(b));
  if (json) {
    int multi = 0;
    rc = encode_json_struct(&b, d, st, &rt.brand, s, NULL, NULL, 0, &multi, 0,
                            pretty, 0);
  } else {
    rc = encode_text_struct(&b, d, st, &rt.brand, s, pretty ? 1 : 0, TMODE_BARE,
                            0);
  }
  free_rtype(&rt);
  encode_no_caps = 0;
  if (encode_io_err || encode_cap_err) {
    free(b.data);
    return encode_io_err ? CAPNP_ERR_IO : CAPNP_ERR_KIND;
  }
  if (rc || !b.data || (encode_wire_err && !wire_err)) {
    free(b.data);
    if (encode_oom)
      return CAPNP_ERR_ALLOC;
    if (encode_wire_err)
      return encode_wire_err;
    return CAPNP_ERR_KIND;
  }
  if (wire_err)
    *wire_err = encode_wire_err;
  *out = b.data;
  *len = b.n;
  return CAPNP_OK;
}

int capnp_dyn_encode_text(capnp_dyn_t *d, const char *type_name,
                          const capnp_ptr_t *s, char **out, size_t *len) {
  return encode_root_impl(d, type_name, s, 0, 1, out, len, NULL);
}

int capnp_dyn_encode_text_short(capnp_dyn_t *d, const char *type_name,
                                const capnp_ptr_t *s, char **out, size_t *len) {
  return encode_root_impl(d, type_name, s, 0, 0, out, len, NULL);
}

int capnp_dyn_encode_json(capnp_dyn_t *d, const char *type_name,
                          const capnp_ptr_t *s, char **out, size_t *len) {
  return encode_root_impl(d, type_name, s, 1, 1, out, len, NULL);
}

int capnp_dyn_encode_json_short(capnp_dyn_t *d, const char *type_name,
                                const capnp_ptr_t *s, char **out, size_t *len) {
  return encode_root_impl(d, type_name, s, 1, 0, out, len, NULL);
}

int capnp_dyn_encode_recover(capnp_dyn_t *d, const char *type_name,
                             const capnp_ptr_t *s, int json, int pretty,
                             char **out, size_t *len, int *wire_err) {
  int ignored;
  return encode_root_impl(d, type_name, s, json, pretty, out, len,
                          wire_err ? wire_err : &ignored);
}
/* --- JSON and Text AST parser & builder --- */

enum {
  VK_NULL = 0,
  VK_VOID = 1,
  VK_BOOL = 2,
  VK_INT = 3,
  VK_UINT = 4,
  VK_FLOAT = 5,
  VK_STR = 6,
  VK_BYTES = 7,
  VK_IDENT = 8,
  VK_LIST = 9,
  VK_OBJ = 10,
  /* Text: a list item the parser could not read, after a recoverable
   * error. */
  VK_UNKNOWN = 11,
  /* Text: a member access, application, absolute name, import or embed,
   * which only the resolver could evaluate. */
  VK_EXTERN = 12
};

typedef struct vnode vnode_t;

struct vnode {
  int kind;
  int bval;
  int64_t ival;
  uint64_t uval;
  double fval;
  char *str;
  size_t slen;
  char *key;
  vnode_t **kids;
  uint32_t count;
  uint32_t cap;
  /* JSON source span, including quotes for strings. Not owned. */
  const char *src;
  size_t src_n;
  /* Text: written as a bare name (void, true, false, inf, nan), which
   * ValueTranslator resolves as an enumerant first. */
  int ident;
  /* Text: a negative integer beyond INT64_MIN, which ValueTranslator
   * drops with "Integer is too big to be negative". */
  int neg_big;
};

static void free_vnode(vnode_t *v) {
  uint32_t i;
  if (!v)
    return;
  for (i = 0; i < v->count; i++)
    free_vnode(v->kids[i]);
  free(v->kids);
  free(v->str);
  free(v->key);
  free(v);
}

static vnode_t *new_vnode(int kind) {
  vnode_t *v = (vnode_t *)calloc(1, sizeof(vnode_t));
  if (v)
    v->kind = kind;
  return v;
}

static int push_vnode(vnode_t *parent, vnode_t *child) {
  if (parent->count == parent->cap) {
    uint32_t ncap = parent->cap ? parent->cap * 2u : 8u;
    vnode_t **nk =
        (vnode_t **)realloc(parent->kids, (size_t)ncap * sizeof(vnode_t *));
    if (!nk)
      return -1;
    parent->kids = nk;
    parent->cap = ncap;
  }
  parent->kids[parent->count++] = child;
  return 0;
}

typedef struct {
  const char *s;
  size_t n;
  size_t i;
  int depth;
  /* Text: an error the C++ parser does not recover from. */
  int fatal;
} scan_t;

static int hex_nibble(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static int oct_digit(unsigned char c) {
  if (c >= '0' && c <= '7')
    return c - '0';
  return -1;
}

static int buf_add_utf8(buf_t *b, uint32_t cp) {
  char bytes[4];
  size_t n;
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    return -1;
  if (cp < 0x80) {
    bytes[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    bytes[0] = (char)(0xc0 | (cp >> 6));
    bytes[1] = (char)(0x80 | (cp & 0x3f));
    n = 2;
  } else if (cp < 0x10000) {
    bytes[0] = (char)(0xe0 | (cp >> 12));
    bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    bytes[2] = (char)(0x80 | (cp & 0x3f));
    n = 3;
  } else {
    bytes[0] = (char)(0xf0 | (cp >> 18));
    bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    bytes[3] = (char)(0x80 | (cp & 0x3f));
    n = 4;
  }
  return buf_add(b, bytes, n);
}

/* --- Strict JSON parser --- */

static void jskip(scan_t *sc) {
  while (sc->i < sc->n) {
    char c = sc->s[sc->i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
      sc->i++;
    else
      break;
  }
}

static int jpeek(scan_t *sc) {
  jskip(sc);
  if (sc->i >= sc->n || sc->s[sc->i] == '\0')
    return 0;
  return (unsigned char)sc->s[sc->i];
}

static int parse_json_str_raw(scan_t *sc, char **out, size_t *len) {
  buf_t b;
  memset(&b, 0, sizeof(b));
  jskip(sc);
  if (sc->i >= sc->n || sc->s[sc->i] != '"')
    return -1;
  sc->i++;
  while (sc->i < sc->n && sc->s[sc->i] != '\0' && sc->s[sc->i] != '"') {
    unsigned char c = (unsigned char)sc->s[sc->i++];
    if (c == '\\') {
      if (sc->i >= sc->n || sc->s[sc->i] == '\0') {
        free(b.data);
        return -1;
      }
      c = (unsigned char)sc->s[sc->i++];
      switch (c) {
      case '"':
      case '\\':
      case '/':
        if (buf_ch(&b, (char)c)) {
          free(b.data);
          return -1;
        }
        break;
      case 'b':
        if (buf_ch(&b, '\b')) {
          free(b.data);
          return -1;
        }
        break;
      case 'f':
        if (buf_ch(&b, '\f')) {
          free(b.data);
          return -1;
        }
        break;
      case 'n':
        if (buf_ch(&b, '\n')) {
          free(b.data);
          return -1;
        }
        break;
      case 'r':
        if (buf_ch(&b, '\r')) {
          free(b.data);
          return -1;
        }
        break;
      case 't':
        if (buf_ch(&b, '\t')) {
          free(b.data);
          return -1;
        }
        break;
      case 'u': {
        int d0, d1, d2, d3;
        uint32_t cp;
        if (sc->i + 4 > sc->n ||
            (d0 = hex_nibble((unsigned char)sc->s[sc->i])) < 0 ||
            (d1 = hex_nibble((unsigned char)sc->s[sc->i + 1])) < 0 ||
            (d2 = hex_nibble((unsigned char)sc->s[sc->i + 2])) < 0 ||
            (d3 = hex_nibble((unsigned char)sc->s[sc->i + 3])) < 0) {
          free(b.data);
          return -1;
        }
        sc->i += 4;
        cp = (uint32_t)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
        /* JsonCodec decodes each escape alone with kj::decodeUtf16, so a
         * surrogate becomes its own three-byte sequence (CESU-8). */
        if (cp >= 0xd800 && cp <= 0xdfff) {
          char sur[3];
          sur[0] = (char)(0xe0 | (cp >> 12));
          sur[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
          sur[2] = (char)(0x80 | (cp & 0x3f));
          if (buf_add(&b, sur, 3)) {
            free(b.data);
            return -1;
          }
        } else if (buf_add_utf8(&b, cp)) {
          free(b.data);
          return -1;
        }
        break;
      }
      default:
        free(b.data);
        return -1;
      }
    } else {
      if (buf_ch(&b, (char)c)) {
        free(b.data);
        return -1;
      }
    }
  }
  if (sc->i >= sc->n || sc->s[sc->i] != '"') {
    free(b.data);
    return -1;
  }
  sc->i++;
  if (!b.data)
    b.data = (char *)calloc(1, 1);
  if (!b.data)
    return -1;
  *out = b.data;
  *len = b.n;
  return 0;
}

static vnode_t *parse_json_val(scan_t *sc);

/* strtod over exactly `len` bytes, which must all be consumed. Literals
 * have no length limit, as in kj. */
static int strtod_n(const char *s, size_t len, double *out) {
  char small[128];
  char *buf = small, *endp = NULL;
  int rc = 0;
  if (len >= sizeof(small)) {
    buf = (char *)malloc(len + 1);
    if (!buf)
      return -1;
  }
  memcpy(buf, s, len);
  buf[len] = 0;
  *out = strtod(buf, &endp);
  if (!endp || *endp != 0)
    rc = -1;
  if (buf != small)
    free(buf);
  return rc;
}

static vnode_t *parse_json_num(scan_t *sc) {
  size_t start = sc->i;
  size_t len;
  double dv;
  vnode_t *v;

  if (sc->i < sc->n && sc->s[sc->i] == '-')
    sc->i++;
  if (sc->i >= sc->n || sc->s[sc->i] == '\0')
    return NULL;
  if (sc->s[sc->i] == '0') {
    sc->i++;
  } else if (sc->s[sc->i] >= '1' && sc->s[sc->i] <= '9') {
    while (sc->i < sc->n && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9')
      sc->i++;
  } else {
    return NULL;
  }
  if (sc->i < sc->n && sc->s[sc->i] == '.') {
    sc->i++;
    while (sc->i < sc->n && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9')
      sc->i++;
  }
  if (sc->i < sc->n && (sc->s[sc->i] == 'e' || sc->s[sc->i] == 'E')) {
    sc->i++;
    if (sc->i < sc->n && (sc->s[sc->i] == '+' || sc->s[sc->i] == '-'))
      sc->i++;
    while (sc->i < sc->n && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9')
      sc->i++;
  }
  len = sc->i - start;
  if (len == 0 || strtod_n(sc->s + start, len, &dv))
    return NULL;
  v = new_vnode(VK_FLOAT);
  if (!v)
    return NULL;
  v->fval = dv;
  return v;
}

static vnode_t *parse_json_arr(scan_t *sc) {
  vnode_t *arr;
  int expect_comma = 0;
  sc->i++; /* consume '[' */
  if (++sc->depth > JSON_MAX_DEPTH)
    return NULL;
  arr = new_vnode(VK_LIST);
  if (!arr)
    return NULL;
  while (jpeek(sc) != ']') {
    vnode_t *el;
    if (jpeek(sc) == 0) {
      free_vnode(arr);
      return NULL;
    }
    if (expect_comma) {
      if (jpeek(sc) != ',') {
        free_vnode(arr);
        return NULL;
      }
      sc->i++;
      if (jpeek(sc) == ']' || jpeek(sc) == 0) {
        free_vnode(arr);
        return NULL;
      }
    }
    el = parse_json_val(sc);
    if (!el || push_vnode(arr, el)) {
      free_vnode(el);
      free_vnode(arr);
      return NULL;
    }
    expect_comma = 1;
  }
  sc->i++; /* consume ']' */
  sc->depth--;
  return arr;
}

static vnode_t *parse_json_obj(scan_t *sc) {
  vnode_t *obj;
  int expect_comma = 0;
  sc->i++; /* consume '{' */
  if (++sc->depth > JSON_MAX_DEPTH)
    return NULL;
  obj = new_vnode(VK_OBJ);
  if (!obj)
    return NULL;
  while (jpeek(sc) != '}') {
    char *key = NULL;
    size_t klen = 0;
    vnode_t *val;
    if (jpeek(sc) == 0) {
      free_vnode(obj);
      return NULL;
    }
    if (expect_comma) {
      if (jpeek(sc) != ',') {
        free_vnode(obj);
        return NULL;
      }
      sc->i++;
      if (jpeek(sc) == '}' || jpeek(sc) == 0) {
        free_vnode(obj);
        return NULL;
      }
    }
    if (parse_json_str_raw(sc, &key, &klen)) {
      free_vnode(obj);
      return NULL;
    }
    if (jpeek(sc) != ':') {
      free(key);
      free_vnode(obj);
      return NULL;
    }
    sc->i++;
    val = parse_json_val(sc);
    if (!val) {
      free(key);
      free_vnode(obj);
      return NULL;
    }
    val->key = key;
    if (push_vnode(obj, val)) {
      free_vnode(val);
      free_vnode(obj);
      return NULL;
    }
    expect_comma = 1;
  }
  sc->i++; /* consume '}' */
  sc->depth--;
  return obj;
}

static vnode_t *parse_json_val_raw(scan_t *sc) {
  int c = jpeek(sc);
  if (c == 0)
    return NULL;
  if (c == 'n') {
    if (sc->i + 4 <= sc->n && memcmp(sc->s + sc->i, "null", 4) == 0) {
      sc->i += 4;
      return new_vnode(VK_NULL);
    }
    return NULL;
  }
  if (c == 't') {
    if (sc->i + 4 <= sc->n && memcmp(sc->s + sc->i, "true", 4) == 0) {
      vnode_t *v = new_vnode(VK_BOOL);
      sc->i += 4;
      if (v)
        v->bval = 1;
      return v;
    }
    return NULL;
  }
  if (c == 'f') {
    if (sc->i + 5 <= sc->n && memcmp(sc->s + sc->i, "false", 5) == 0) {
      vnode_t *v = new_vnode(VK_BOOL);
      sc->i += 5;
      if (v)
        v->bval = 0;
      return v;
    }
    return NULL;
  }
  if (c == '"') {
    char *s = NULL;
    size_t slen = 0;
    vnode_t *v;
    if (parse_json_str_raw(sc, &s, &slen))
      return NULL;
    v = new_vnode(VK_STR);
    if (!v) {
      free(s);
      return NULL;
    }
    v->str = s;
    v->slen = slen;
    return v;
  }
  if (c == '[')
    return parse_json_arr(sc);
  if (c == '{')
    return parse_json_obj(sc);
  if (c == '-' || (c >= '0' && c <= '9'))
    return parse_json_num(sc);
  return NULL;
}

static vnode_t *parse_json_val(scan_t *sc) {
  size_t start;
  vnode_t *v;
  jskip(sc);
  start = sc->i;
  v = parse_json_val_raw(sc);
  if (v && sc->s && sc->i >= start) {
    v->src = sc->s + start;
    v->src_n = sc->i - start;
  }
  return v;
}
/* --- Cap'n Proto Text format parser --- */

static void tskip(scan_t *sc) {
  for (;;) {
    while (sc->i < sc->n) {
      unsigned char c = (unsigned char)sc->s[sc->i];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' ||
          c == '\f') {
        sc->i++;
        continue;
      }
      if (sc->i + 3 <= sc->n && (unsigned char)sc->s[sc->i] == 0xef &&
          (unsigned char)sc->s[sc->i + 1] == 0xbb &&
          (unsigned char)sc->s[sc->i + 2] == 0xbf) {
        sc->i += 3;
        continue;
      }
      break;
    }
    if (sc->i < sc->n && sc->s[sc->i] == '#') {
      while (sc->i < sc->n && sc->s[sc->i] != '\n')
        sc->i++;
      continue;
    }
    break;
  }
}

static int tpeek(scan_t *sc) {
  tskip(sc);
  if (sc->i >= sc->n)
    return 0;
  return (unsigned char)sc->s[sc->i];
}

/* The C++ lexer reads a run of operator characters as one token, so `=-5`
 * contains the operator `=-` and is a parse error. */
static int op_ends(const scan_t *sc) {
  return sc->i >= sc->n || sc->s[sc->i] == '\0' ||
         strchr("!$%&*+-./:<=>?@^|~", sc->s[sc->i]) == NULL;
}

static int parse_text_quoted_one(scan_t *sc, buf_t *b) {
  char q;
  tskip(sc);
  if (sc->i >= sc->n || sc->s[sc->i] != '"')
    return -1;
  q = sc->s[sc->i++];
  while (sc->i < sc->n && sc->s[sc->i] != q) {
    unsigned char c = (unsigned char)sc->s[sc->i++];
    if (c == '\n')
      return -1;
    if (c == '\\') {
      if (sc->i >= sc->n)
        return -1;
      c = (unsigned char)sc->s[sc->i++];
      switch (c) {
      case 'a':
        c = '\a';
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;
      case 'v':
        c = '\v';
        break;
      case '\'':
      case '"':
      case '\\':
      case '?':
        break;
      case 'x': {
        int hi, lo;
        if (sc->i + 2 > sc->n ||
            (hi = hex_nibble((unsigned char)sc->s[sc->i])) < 0 ||
            (lo = hex_nibble((unsigned char)sc->s[sc->i + 1])) < 0)
          return -1;
        sc->i += 2;
        c = (unsigned char)((hi << 4) | lo);
        break;
      }
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7': {
        unsigned val = (unsigned)(c - '0');
        int d;
        if (sc->i < sc->n &&
            (d = oct_digit((unsigned char)sc->s[sc->i])) >= 0) {
          sc->i++;
          val = (val << 3) | (unsigned)d;
          if (sc->i < sc->n &&
              (d = oct_digit((unsigned char)sc->s[sc->i])) >= 0) {
            sc->i++;
            val = (val << 3) | (unsigned)d;
          }
        }
        c = (unsigned char)val;
        break;
      }
      default:
        return -1;
      }
    }
    if (buf_ch(b, (char)c))
      return -1;
  }
  if (sc->i >= sc->n || sc->s[sc->i] != q)
    return -1;
  sc->i++;
  return 0;
}

static vnode_t *parse_text_hex_binary(scan_t *sc) {
  buf_t b;
  vnode_t *v;
  sc->i += 3; /* consume 0x" */
  memset(&b, 0, sizeof(b));
  for (;;) {
    int hi, lo;
    while (sc->i < sc->n && isspace((unsigned char)sc->s[sc->i]))
      sc->i++;
    if (sc->i < sc->n && sc->s[sc->i] == '"') {
      sc->i++;
      break;
    }
    if (sc->i + 2 > sc->n ||
        (hi = hex_nibble((unsigned char)sc->s[sc->i])) < 0 ||
        (lo = hex_nibble((unsigned char)sc->s[sc->i + 1])) < 0) {
      free(b.data);
      return NULL;
    }
    sc->i += 2;
    if (buf_ch(&b, (char)((hi << 4) | lo))) {
      free(b.data);
      return NULL;
    }
  }
  if (b.n == 0) {
    /* 0x"" requires oneOrMore hex bytes in kj::parse::doubleQuotedHexBinary */
    free(b.data);
    return NULL;
  }
  v = new_vnode(VK_BYTES);
  if (!v) {
    free(b.data);
    return NULL;
  }
  v->str = b.data;
  v->slen = b.n;
  return v;
}

static int parse_text_ident(scan_t *sc, char *out, size_t n) {
  size_t k = 0;
  tskip(sc);
  if (sc->i >= sc->n ||
      !(isalpha((unsigned char)sc->s[sc->i]) || sc->s[sc->i] == '_'))
    return -1;
  while (sc->i < sc->n &&
         (isalnum((unsigned char)sc->s[sc->i]) || sc->s[sc->i] == '_')) {
    if (k + 1 >= n)
      return -1;
    out[k++] = sc->s[sc->i++];
  }
  out[k] = 0;
  return 0;
}

static vnode_t *parse_text_expr(scan_t *sc);

#define TEXT_OPS "!$%&*+-./:<=>?@^|~"

static int soft_error(void);

static char *dup_cstr(const char *src) {
  size_t n = strlen(src) + 1;
  char *out = (char *)malloc(n);
  if (out)
    memcpy(out, src, n);
  return out;
}

/* Offset of the ',' or closing bracket that ends the list item at
 * sc->i, or sc->n. The input has lexed, so brackets balance and strings
 * end. */
static size_t text_item_end(const scan_t *sc) {
  scan_t t = *sc;
  int depth = 0;
  for (;;) {
    int c;
    tskip(&t);
    if (t.i >= t.n)
      return t.n;
    c = (unsigned char)t.s[t.i];
    if (c == '"') {
      buf_t b;
      int r;
      memset(&b, 0, sizeof(b));
      r = parse_text_quoted_one(&t, &b);
      free(b.data);
      if (r)
        return t.n;
      continue;
    }
    if (c == '(' || c == '[') {
      depth++;
    } else if (c == ')' || c == ']') {
      if (depth == 0)
        return t.i;
      depth--;
    } else if (c == ',' && depth == 0) {
      return t.i;
    }
    t.i++;
  }
}

/* The items of a parenthesized or bracketed list, each parsed alone as
 * ParseListItems does (compiler/parser.c++). An item that does not
 * parse, or is empty, becomes an unnamed VK_UNKNOWN after a recoverable
 * error. The lexer drops one trailing empty item. */
static vnode_t *parse_text_items(scan_t *sc, int tuple) {
  vnode_t *arr = new_vnode(tuple ? VK_OBJ : VK_LIST);
  int commas = 0;
  if (!arr)
    return NULL;
  sc->i++; /* consume '(' or '[' */
  if (++sc->depth > value_depth) {
    sc->fatal = 1;
    free_vnode(arr);
    return NULL;
  }
  for (;;) {
    size_t end = text_item_end(sc);
    scan_t sub = *sc;
    vnode_t *val = NULL;
    char *key = NULL;
    int last, bad = 0;
    if (end >= sc->n) {
      sc->fatal = 1;
      free_vnode(arr);
      return NULL;
    }
    sub.n = end;
    last = sc->s[end] != ',';
    if (last && tpeek(&sub) == 0 && (commas > 0 || arr->count == 0)) {
      sc->i = end + 1;
      break;
    }
    if (tuple) {
      size_t save = sub.i;
      char id[128];
      if (parse_text_ident(&sub, id, sizeof(id)) == 0 && tpeek(&sub) == '=') {
        sub.i++;
        if (!op_ends(&sub))
          bad = 1;
        else if (!(key = dup_cstr(id))) {
          free_vnode(arr);
          return NULL;
        }
      } else {
        sub.i = save;
      }
    }
    if (!bad)
      val = parse_text_expr(&sub);
    if (sub.fatal) {
      sc->fatal = 1;
      free(key);
      free_vnode(val);
      free_vnode(arr);
      return NULL;
    }
    if (!val || tpeek(&sub) != 0) {
      free_vnode(val);
      free(key);
      key = NULL;
      if (soft_error() || !(val = new_vnode(VK_UNKNOWN))) {
        free_vnode(arr);
        return NULL;
      }
    }
    val->key = key;
    if (push_vnode(arr, val)) {
      free_vnode(val);
      free_vnode(arr);
      return NULL;
    }
    sc->i = end + 1;
    sc->depth = sub.depth;
    if (last)
      break;
    commas++;
  }
  sc->depth--;
  return arr;
}

/* A tuple of one unnamed value is that value. */
static vnode_t *parse_text_tuple(scan_t *sc) {
  vnode_t *t = parse_text_items(sc, 1);
  if (t && t->count == 1 && !t->kids[0]->key) {
    vnode_t *v = t->kids[0];
    t->count = 0;
    free_vnode(t);
    return v;
  }
  return t;
}

static vnode_t *parse_text_list(scan_t *sc) {
  return parse_text_items(sc, 0);
}

static int text_num_end_ok(const scan_t *sc, size_t i) {
  return i >= sc->n || !(isalpha((unsigned char)sc->s[i]) ||
                         sc->s[i] == '_' || sc->s[i] == '.');
}

/* Lexes a token that starts with a digit as kj::parse does: a hex
 * binary, then `integer`, then `number`. Both numbers may not run into a
 * letter, `_` or `.`. */
static int text_lex_number(scan_t *sc) {
  const char *s = sc->s;
  size_t n = sc->n, i = sc->i, j;
  if (i + 2 < n && s[i] == '0' && s[i + 1] == 'x' && s[i + 2] == '"') {
    scan_t t = *sc;
    vnode_t *v = parse_text_hex_binary(&t);
    if (v) {
      free_vnode(v);
      sc->i = t.i;
      return 0;
    }
  }
  j = i;
  if (s[i] == '0' && i + 2 < n && s[i + 1] == 'x' &&
      hex_nibble((unsigned char)s[i + 2]) >= 0) {
    j = i + 2;
    while (j < n && hex_nibble((unsigned char)s[j]) >= 0)
      j++;
  } else if (s[i] == '0') {
    j = i + 1;
    while (j < n && oct_digit((unsigned char)s[j]) >= 0)
      j++;
  } else {
    while (j < n && isdigit((unsigned char)s[j]))
      j++;
  }
  if (text_num_end_ok(sc, j)) {
    sc->i = j;
    return 0;
  }
  j = i;
  while (j < n && isdigit((unsigned char)s[j]))
    j++;
  if (j < n && s[j] == '.') {
    j++;
    while (j < n && isdigit((unsigned char)s[j]))
      j++;
  }
  if (j < n && (s[j] == 'e' || s[j] == 'E')) {
    j++;
    if (j < n && (s[j] == '+' || s[j] == '-'))
      j++;
    while (j < n && isdigit((unsigned char)s[j]))
      j++;
  }
  if (text_num_end_ok(sc, j)) {
    sc->i = j;
    return 0;
  }
  return -1;
}

/* Whether the C++ lexer reads @s as one token sequence. When it does
 * not, it returns no tokens and `capnp` fails the message outright.
 * Splits it makes differently, such as `0x""` into `0`, `x` and `""`,
 * still lex; their parse errors are recoverable. */
static int text_lexes(const char *s, size_t n) {
  scan_t sc;
  char *stack = NULL;
  size_t depth = 0, cap = 0;
  int ok = 0;
  memset(&sc, 0, sizeof(sc));
  sc.s = s;
  sc.n = n;
  for (;;) {
    int c;
    tskip(&sc);
    if (sc.i >= n) {
      ok = depth == 0;
      break;
    }
    c = (unsigned char)s[sc.i];
    if (c == '"') {
      buf_t b;
      int r;
      memset(&b, 0, sizeof(b));
      r = parse_text_quoted_one(&sc, &b);
      free(b.data);
      if (r)
        break;
      continue;
    }
    if (c == '(' || c == '[') {
      if (depth == cap) {
        size_t ncap = cap ? cap * 2 : 16;
        char *ns = (char *)realloc(stack, ncap);
        if (!ns)
          break;
        stack = ns;
        cap = ncap;
      }
      stack[depth++] = (char)c;
    } else if (c == ')' || c == ']') {
      if (depth == 0 || stack[depth - 1] != (c == ')' ? '(' : '['))
        break;
      depth--;
    } else if (c == ',') {
      if (depth == 0)
        break;
    } else if (isdigit(c)) {
      if (text_lex_number(&sc))
        break;
      continue;
    } else if (isalpha(c) || c == '_') {
      while (sc.i < n && (isalnum((unsigned char)s[sc.i]) || s[sc.i] == '_'))
        sc.i++;
      continue;
    } else if (!(isalnum(c) || c == '_' || (c != 0 && strchr(TEXT_OPS, c)))) {
      break;
    }
    sc.i++;
  }
  free(stack);
  return ok;
}

/* A number token may not run into a letter, `_` or `.`. */
static int number_ends(const scan_t *sc, size_t q) {
  int c;
  if (q >= sc->n)
    return 1;
  c = (unsigned char)sc->s[q];
  return !(isalpha(c) || c == '_' || c == '.');
}

static vnode_t *parse_text_number(scan_t *sc, int neg) {
  size_t start = sc->i, q;
  int is_flt = 0;
  int base = 10;
  size_t len;
  vnode_t *v;

  /* kj tries an integer first: `0x` hex, `0` octal, or decimal. A leading
   * zero stops at the first non-octal digit, so `08` is two tokens. */
  if (start + 2 < sc->n && sc->s[start] == '0' && sc->s[start + 1] == 'x' &&
      isxdigit((unsigned char)sc->s[start + 2])) {
    base = 16;
    q = start + 2;
    while (q < sc->n && isxdigit((unsigned char)sc->s[q]))
      q++;
  } else if (start < sc->n && sc->s[start] == '0') {
    base = 8;
    q = start + 1;
    while (q < sc->n && sc->s[q] >= '0' && sc->s[q] <= '7')
      q++;
  } else {
    q = start;
    while (q < sc->n && isdigit((unsigned char)sc->s[q]))
      q++;
  }
  if (!number_ends(sc, q)) {
    is_flt = 1;
    q = start;
    while (q < sc->n && isdigit((unsigned char)sc->s[q]))
      q++;
    if (q < sc->n && sc->s[q] == '.') {
      q++;
      while (q < sc->n && isdigit((unsigned char)sc->s[q]))
        q++;
    }
    if (q < sc->n && (sc->s[q] == 'e' || sc->s[q] == 'E')) {
      q++;
      if (q < sc->n && (sc->s[q] == '+' || sc->s[q] == '-'))
        q++;
      if (q >= sc->n || !isdigit((unsigned char)sc->s[q]))
        return NULL;
      while (q < sc->n && isdigit((unsigned char)sc->s[q]))
        q++;
    }
    if (!number_ends(sc, q))
      return NULL;
  }
  sc->i = q;
  len = sc->i - start;
  if (len == 0)
    return NULL;

  if (is_flt) {
    double dv;
    if (strtod_n(sc->s + start, len, &dv))
      return NULL;
    v = new_vnode(VK_FLOAT);
    if (!v)
      return NULL;
    v->fval = neg ? -dv : dv;
    return v;
  } else {
    /* kj's ParseInteger wraps modulo 2^64 without complaint. */
    uint64_t uv = 0;
    size_t k;
    for (k = start + (base == 16 ? 2u : 0u); k < sc->i; k++) {
      int c = (unsigned char)sc->s[k];
      uv = uv * (uint64_t)base +
           (uint64_t)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    if (neg) {
      v = new_vnode(VK_INT);
      if (!v)
        return NULL;
      if (uv > (uint64_t)INT64_MAX + 1ull)
        v->neg_big = 1;
      else
        v->ival = (uv == (uint64_t)INT64_MAX + 1ull) ? INT64_MIN : -(int64_t)uv;
      return v;
    } else {
      v = new_vnode(VK_UINT);
      if (!v)
        return NULL;
      v->uval = uv;
      return v;
    }
  }
}

/* A standalone `.` operator token, not part of a longer run. */
static int text_dot(const scan_t *sc) {
  return sc->i < sc->n && sc->s[sc->i] == '.' &&
         (sc->i + 1 >= sc->n || sc->s[sc->i + 1] == '\0' ||
          strchr(TEXT_OPS, sc->s[sc->i + 1]) == NULL);
}

static vnode_t *parse_text_base(scan_t *sc);

/* An expression with its suffixes. A member access or application can
 * only be resolved as an external constant, so it is VK_EXTERN. */
static vnode_t *parse_text_expr(scan_t *sc) {
  vnode_t *v = parse_text_base(sc);
  while (v) {
    int c = tpeek(sc);
    if (c == '.' && text_dot(sc)) {
      char id[128];
      sc->i++;
      free_vnode(v);
      v = NULL;
      if (parse_text_ident(sc, id, sizeof(id)) == 0)
        v = new_vnode(VK_EXTERN);
    } else if (c == '(') {
      vnode_t *params = parse_text_tuple(sc);
      free_vnode(v);
      v = params ? new_vnode(VK_EXTERN) : NULL;
      free_vnode(params);
    } else {
      break;
    }
  }
  return v;
}

static vnode_t *parse_text_base(scan_t *sc) {
  int c = tpeek(sc);
  if (c == 0)
    return NULL;
  if (c == '.' && text_dot(sc)) {
    char id[128];
    sc->i++;
    if (parse_text_ident(sc, id, sizeof(id)))
      return NULL;
    return new_vnode(VK_EXTERN);
  }
  if (c == '(')
    return parse_text_tuple(sc);
  if (c == '[')
    return parse_text_list(sc);
  if (c == '0' && sc->i + 3 <= sc->n && sc->s[sc->i + 1] == 'x' &&
      sc->s[sc->i + 2] == '"') {
    return parse_text_hex_binary(sc);
  }
  if (c == '"') {
    buf_t b;
    vnode_t *v;
    memset(&b, 0, sizeof(b));
    while (tpeek(sc) == '"') {
      if (parse_text_quoted_one(sc, &b)) {
        free(b.data);
        return NULL;
      }
    }
    if (!b.data)
      b.data = (char *)calloc(1, 1);
    if (!b.data)
      return NULL;
    v = new_vnode(VK_STR);
    if (!v) {
      free(b.data);
      return NULL;
    }
    v->str = b.data;
    v->slen = b.n;
    return v;
  }
  if (c == '-') {
    sc->i++;
    if (!op_ends(sc))
      return NULL;
    tskip(sc);
    if (sc->i + 3 <= sc->n && memcmp(sc->s + sc->i, "inf", 3) == 0 &&
        (sc->i + 3 == sc->n ||
         !(isalnum((unsigned char)sc->s[sc->i + 3]) ||
           sc->s[sc->i + 3] == '_'))) {
      vnode_t *v = new_vnode(VK_FLOAT);
      sc->i += 3;
      if (v)
        v->fval = -INFINITY;
      return v;
    }
    return parse_text_number(sc, 1);
  }
  if (isdigit(c))
    return parse_text_number(sc, 0);
  if (isalpha(c) || c == '_') {
    char id[128];
    vnode_t *v;
    if (parse_text_ident(sc, id, sizeof(id)))
      return NULL;
    if ((strcmp(id, "import") == 0 || strcmp(id, "embed") == 0) &&
        tpeek(sc) == '"') {
      buf_t b;
      int r;
      memset(&b, 0, sizeof(b));
      r = parse_text_quoted_one(sc, &b);
      free(b.data);
      return r ? NULL : new_vnode(VK_EXTERN);
    }
    if (strcmp(id, "void") == 0) {
      v = new_vnode(VK_VOID);
      if (v)
        v->ident = 1;
      return v;
    }
    if (strcmp(id, "true") == 0 || strcmp(id, "false") == 0) {
      v = new_vnode(VK_BOOL);
      if (v) {
        v->bval = id[0] == 't';
        v->ident = 1;
      }
      return v;
    }
    if (strcmp(id, "inf") == 0 || strcmp(id, "nan") == 0) {
      v = new_vnode(VK_FLOAT);
      if (v) {
        v->fval = id[0] == 'i' ? INFINITY : NAN;
        v->ident = 1;
      }
      return v;
    }
    v = new_vnode(VK_IDENT);
    if (!v)
      return NULL;
    v->str = (char *)malloc(strlen(id) + 1);
    if (!v->str) {
      free_vnode(v);
      return NULL;
    }
    memcpy(v->str, id, strlen(id) + 1);
    v->slen = strlen(id);
    return v;
  }
  return NULL;
}
/* --- Builder from AST (vnode_t) --- */

/* `capnp convert` goes on after a recoverable kj error in a text or
 * JSON value, using what kj substitutes or dropping the value, and fails
 * the run at the end. With dec_recover unset such an error fails the
 * decode. */
static _Thread_local int dec_recover;
static _Thread_local int dec_soft;

static int soft_error(void) {
  if (!dec_recover)
    return -1;
  dec_soft = 1;
  return 0;
}

static int parse_str_i64(const char *s, size_t n, int64_t min_v, int64_t max_v,
                         int64_t *out) {
  char *endp = NULL;
  const char *p = s;
  int base = 10;
  long long v;
  if (!s || n == 0 || memchr(s, 0, n) != NULL)
    return -1;
  if (*p == '-')
    p++;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    base = 16;
  errno = 0;
  v = strtoll(s, &endp, base);
  if (endp != s + n || errno == ERANGE || v < min_v || v > max_v)
    return -1;
  *out = (int64_t)v;
  return 0;
}

static int parse_str_u64(const char *s, size_t n, uint64_t max_v,
                         uint64_t *out) {
  char *endp = NULL;
  const char *p = s;
  int base = 10;
  unsigned long long v;
  if (!s || n == 0 || s[0] == '-' || memchr(s, 0, n) != NULL)
    return -1;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    base = 16;
  errno = 0;
  v = strtoull(s, &endp, base);
  if (endp != s + n || errno == ERANGE || v > max_v)
    return -1;
  *out = (uint64_t)v;
  return 0;
}

static int parse_str_f64(const char *s, size_t n, double *out) {
  char *endp = NULL;
  double v;
  if (!s || n == 0 || memchr(s, 0, n) != NULL)
    return -1;
  errno = 0;
  v = strtod(s, &endp);
  if (endp != s + n)
    return -1;
  *out = v;
  return 0;
}

/* JsonCodec reaches an integer field with a double that passed its
 * range check, then casts it. 2^63 and 2^64 pass that check and are
 * outside int64 and uint64, so the C++ cast has no defined result and the
 * output follows the CPU: x86-64 gives INT64_MIN and 0, s390x and other
 * saturating CPUs give INT64_MAX and UINT64_MAX. These helpers return
 * the same values without casting a double out of range. */
#if defined(__x86_64__) || defined(_M_X64)
#define DYN_F2I_SATURATES 0
#else
#define DYN_F2I_SATURATES 1
#endif

/* d is in [-2^63, 2^63]. */
static int64_t f2i64_host(double d) {
  if (d >= 9223372036854775808.0)
    return DYN_F2I_SATURATES ? INT64_MAX : INT64_MIN;
  return (int64_t)d;
}

/* d is in [0, 2^64]. */
static uint64_t f2u64_host(double d) {
  if (d >= 18446744073709551616.0)
    return DYN_F2I_SATURATES ? UINT64_MAX : 0;
  if (d >= 9223372036854775808.0)
    return (uint64_t)(d - 9223372036854775808.0) + ((uint64_t)1 << 63);
  return (uint64_t)d;
}

/* static_cast<uint16_t>(double), for any double. x86-64 truncates to a
 * 32-bit integer, which reads as INT32_MIN when out of range. Saturating
 * CPUs clamp to the unsigned 32-bit range, with NaN as 0. Both keep the
 * low 16 bits. */
static uint16_t f2u16_host(double d) {
#if DYN_F2I_SATURATES
  if (!(d >= 0.0))
    return 0;
  if (d >= 4294967296.0)
    return UINT16_MAX;
  return (uint16_t)(uint32_t)d;
#else
  if (!(d > -2147483649.0 && d < 2147483648.0))
    return 0;
  return (uint16_t)(uint32_t)(int32_t)d;
#endif
}

static int vnode_to_int(const vnode_t *v, uint16_t which, int json,
                        uint64_t *out_bits) {
  int64_t smin = 0, smax = 0;
  uint64_t umax = 0;
  int is_signed = (which == TY_I8 || which == TY_I16 || which == TY_I32 ||
                   which == TY_I64);
  switch (which) {
  case TY_I8:
    smin = INT8_MIN;
    smax = INT8_MAX;
    break;
  case TY_I16:
    smin = INT16_MIN;
    smax = INT16_MAX;
    break;
  case TY_I32:
    smin = INT32_MIN;
    smax = INT32_MAX;
    break;
  case TY_I64:
    smin = INT64_MIN;
    smax = INT64_MAX;
    break;
  case TY_U8:
    umax = UINT8_MAX;
    break;
  case TY_U16:
    umax = UINT16_MAX;
    break;
  case TY_U32:
    umax = UINT32_MAX;
    break;
  case TY_U64:
    umax = UINT64_MAX;
    break;
  default:
    return -1;
  }
  /* ValueTranslator::matchesType clamps an integer out of range to the
   * type's bound and reports it. */
  if (v->kind == VK_INT && v->ival < 0) {
    if (!is_signed) {
      *out_bits = 0;
      return soft_error();
    }
    if (v->ival < smin) {
      *out_bits = (uint64_t)smin;
      return soft_error();
    }
    *out_bits = (uint64_t)v->ival;
    return 0;
  }
  if (v->kind == VK_INT || v->kind == VK_UINT) {
    uint64_t u = v->kind == VK_INT ? (uint64_t)v->ival : v->uval;
    uint64_t max = is_signed ? (uint64_t)smax : umax;
    *out_bits = u > max ? max : u;
    return u > max ? soft_error() : 0;
  }
  /* JSON numbers are doubles, converted by checkRoundTripFromFloat:
   * clamped to the range, then truncated, then checked for a round trip. */
  if (json && v->kind == VK_FLOAT) {
    double d = v->fval;
    if (is_signed) {
      int64_t iv;
      if (!(d >= (double)smin)) {
        *out_bits = (uint64_t)smin;
        return soft_error();
      }
      if (!(d <= (double)smax)) {
        *out_bits = (uint64_t)smax;
        return soft_error();
      }
      iv = f2i64_host(d);
      *out_bits = (uint64_t)iv;
      return (double)iv == d ? 0 : soft_error();
    } else {
      uint64_t uv;
      if (!(d >= 0.0)) {
        *out_bits = 0;
        return soft_error();
      }
      if (!(d <= (double)umax)) {
        *out_bits = umax;
        return soft_error();
      }
      uv = f2u64_host(d);
      *out_bits = uv;
      return (double)uv == d ? 0 : soft_error();
    }
  }
  /* Strings go through kj parseAs<int64_t> or parseAs<uint64_t>, which
   * read a bad number as 0, and then wrap to the field's width. */
  if (json && v->kind == VK_STR) {
    if (is_signed) {
      int64_t iv = 0;
      if (parse_str_i64(v->str, v->slen, INT64_MIN, INT64_MAX, &iv)) {
        *out_bits = 0;
        return soft_error();
      }
      *out_bits = (uint64_t)iv;
      return iv < smin || iv > smax ? soft_error() : 0;
    } else {
      uint64_t uv = 0;
      if (parse_str_u64(v->str, v->slen, UINT64_MAX, &uv)) {
        *out_bits = 0;
        return soft_error();
      }
      *out_bits = uv;
      return uv > umax ? soft_error() : 0;
    }
  }
  return -1;
}

/* An element of a JSON byte array, which JsonCodec checks with
 * KJ_REQUIRE(byte(x) == x) and cannot recover from. */
static int json_byte(const vnode_t *v, uint64_t *out) {
  if (v->kind != VK_FLOAT || !(v->fval >= 0.0 && v->fval <= 255.0) ||
      v->fval != (double)(int)v->fval)
    return -1;
  *out = (uint64_t)v->fval;
  return 0;
}

static int vnode_to_float(const vnode_t *v, uint16_t which, int json,
                          uint64_t *out_bits) {
  double dv = 0.0;
  if (v->kind == VK_FLOAT) {
    dv = v->fval;
  } else if (v->kind == VK_INT) {
    dv = (double)v->ival;
  } else if (v->kind == VK_UINT) {
    dv = (double)v->uval;
  } else if (json && v->kind == VK_NULL) {
    dv = NAN;
  } else if (json && v->kind == VK_STR) {
    if (parse_str_f64(v->str, v->slen, &dv)) {
      dv = 0.0;
      if (soft_error())
        return -1;
    }
  } else {
    return -1;
  }
  if (which == TY_F32) {
    float fv = (float)dv;
    uint32_t u = 0;
    memcpy(&u, &fv, sizeof(u));
    *out_bits = u;
  } else {
    memcpy(out_bits, &dv, sizeof(*out_bits));
  }
  return 0;
}

static int vnode_to_enum(capnp_dyn_t *d, uint64_t enum_id,
                         const vnode_t *v, int json, uint16_t *out_ord) {
  dyn_enum_t *en = find_enum_id(d, enum_id);
  uint32_t i;
  if (!en)
    return -1;
  if (json) {
    if (v->kind == VK_FLOAT) {
      /* static_cast<uint16_t> of the double. */
      *out_ord = f2u16_host(v->fval);
      return 0;
    }
    if (v->kind == VK_STR) {
      for (i = 0; i < en->nvals; i++) {
        if (strlen(en->vals[i].json_name) == v->slen &&
            memcmp(en->vals[i].json_name, v->str, v->slen) == 0) {
          *out_ord = (uint16_t)i;
          return 0;
        }
      }
    }
    return -1;
  }
  if (v->kind == VK_IDENT) {
    for (i = 0; i < en->nvals; i++) {
      if (strlen(en->vals[i].name) == v->slen &&
          memcmp(en->vals[i].name, v->str, v->slen) == 0) {
        *out_ord = (uint16_t)i;
        return 0;
      }
    }
  }
  return -1;
}

static int base64_sextet(unsigned char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

/* `capnp convert` decodes with kj::decodeBase64 and drops its error flag:
 * characters outside the alphabet are skipped, padding included, and a
 * trailing partial byte is lost. */
static int decode_base64_str(const char *s, size_t n, uint8_t **out,
                             size_t *out_len) {
  uint8_t *buf;
  size_t i, w = 0;
  uint32_t acc = 0;
  unsigned nbits = 0;
  buf = (uint8_t *)malloc(n / 4 * 3 + 3);
  if (!buf)
    return -1;
  for (i = 0; i < n; i++) {
    int v = base64_sextet((unsigned char)s[i]);
    if (v < 0)
      continue;
    acc = (acc << 6) | (uint32_t)v;
    nbits += 6;
    if (nbits >= 8) {
      nbits -= 8;
      buf[w++] = (uint8_t)(acc >> nbits);
      acc &= (1u << nbits) - 1u;
    }
  }
  *out = buf;
  *out_len = w;
  return 0;
}

/* kj::decodeHex with its error flag dropped: a bad digit reads as zero and
 * an odd final character is ignored. */
static int decode_hex_str(const char *s, size_t n, uint8_t **out,
                          size_t *out_len) {
  uint8_t *buf;
  size_t i;
  buf = (uint8_t *)malloc(n / 2 + 1);
  if (!buf)
    return -1;
  for (i = 0; i < n / 2; i++) {
    int hi = hex_nibble((unsigned char)s[2 * i]);
    int lo = hex_nibble((unsigned char)s[2 * i + 1]);
    buf[i] = (uint8_t)(((hi < 0 ? 0 : hi) << 4) | (lo < 0 ? 0 : lo));
  }
  *out = buf;
  *out_len = n / 2;
  return 0;
}

static int store_bits_for_type(const capnp_bptr_t *body, const dyn_field_t *f,
                               uint16_t which, uint64_t logical) {
  uint64_t wire = logical ^ f->def_bits;
  uint32_t b = data_byte(which, f->offset);
  switch (which) {
  case TY_BOOL:
    return capnp_builder_set_bool(body, f->offset, (int)(wire & 1u));
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

static void clear_field_wire(capnp_dyn_t *d, const dyn_struct_t *st,
                             const dyn_field_t *f, const brand_env_t *env,
                             const capnp_bptr_t *body, int depth);

/* The discriminant is two bytes inside the data section. body_span only
 * checks the segment, so a tag past dwords would overwrite a pointer. */
static int write_disc(const capnp_bptr_t *body, const dyn_struct_t *st,
                      uint16_t tag) {
  if (!st || !st->disc_count)
    return 0;
  if ((uint64_t)st->disc_byte + 2u > (uint64_t)st->dwords * 8u)
    return -1;
  if (capnp_builder_set_u16(body, st->disc_byte, tag))
    return -1;
  return 0;
}

static void clear_group_wire(capnp_dyn_t *d, const dyn_struct_t *g,
                             const brand_env_t *env, const capnp_bptr_t *body,
                             int depth) {
  uint32_t i;
  if (!g || depth > value_depth)
    return;
  if (g->disc_count) {
    if (write_disc(body, g, 0))
      return;
    for (i = 0; i < g->nfields; i++) {
      if (g->fields[i].disc == 0) {
        clear_field_wire(d, g, &g->fields[i], env, body, depth + 1);
        break;
      }
    }
  }
  for (i = 0; i < g->nfields; i++) {
    if (g->fields[i].disc == 0xffff || !g->disc_count)
      clear_field_wire(d, g, &g->fields[i], env, body, depth + 1);
  }
}

static void clear_field_wire(capnp_dyn_t *d, const dyn_struct_t *st,
                             const dyn_field_t *f, const brand_env_t *env,
                             const capnp_bptr_t *body, int depth) {
  rtype_t ft;
  if (depth > value_depth)
    return;
  if (f->is_group) {
    dyn_struct_t *g = find_struct_id(d, f->group_id);
    clear_group_wire(d, g, env, body, depth + 1);
    return;
  }
  if (resolve_field_type(d, f, env, &ft))
    return;
  if (ft.list_depth > 0 || is_ptr_type(ft.which)) {
    capnp_builder_clear_ptr(body, st->dwords, (uint16_t)f->offset);
  } else {
    uint32_t b = data_byte(ft.which, f->offset);
    switch (ft.which) {
    case TY_BOOL:
      capnp_builder_set_bool(body, f->offset, 0);
      break;
    case TY_I8:
    case TY_U8:
      capnp_builder_set_u8(body, b, 0);
      break;
    case TY_I16:
    case TY_U16:
    case TY_ENUM:
      capnp_builder_set_u16(body, b, 0);
      break;
    case TY_I32:
    case TY_U32:
    case TY_F32:
      capnp_builder_set_u32(body, b, 0);
      break;
    case TY_I64:
    case TY_U64:
    case TY_F64:
      capnp_builder_set_u64(body, b, 0);
      break;
    default:
      break;
    }
  }
  free_rtype(&ft);
}
static void dyn_note_bad(capnp_dyn_t *d, const vnode_t *v) {
  if (!d || d->bad_src || !v || !v->src || v->src_n == 0)
    return;
  d->bad_src = v->src;
  d->bad_n = v->src_n;
}

static void dyn_err_bad_json(capnp_dyn_t *d, const char *field) {
  char tok[49];
  char name[97];
  size_t i, n, fn;
  if (!d || d->err[0])
    return;
  fn = field ? strlen(field) : 0;
  if (fn > sizeof(name) - 1)
    fn = sizeof(name) - 1;
  for (i = 0; i < fn; i++) {
    unsigned char c = (unsigned char)field[i];
    name[i] = (c < 32 || c == '"') ? '?' : (char)c;
  }
  name[fn] = 0;
  n = d->bad_n;
  if (n > sizeof(tok) - 1)
    n = sizeof(tok) - 1;
  if (!d->bad_src)
    n = 0;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)d->bad_src[i];
    tok[i] = (c < 32 || c == 127) ? '?' : (char)c;
  }
  tok[n] = 0;
  if (n == 0)
    snprintf(d->err, sizeof(d->err), "bad value for field %s", name);
  else
    snprintf(d->err, sizeof(d->err), "bad value for field %s: %s", name, tok);
  d->bad_src = NULL;
  d->bad_n = 0;
}

static const char *field_json_name(const dyn_field_t *f) {
  if (f && f->json_name[0])
    return f->json_name;
  return f ? f->name : "";
}

/* Emit a field error once a type or range check has failed. An earlier
 * nested field may already have written err, in which case it stands. */
static void dyn_finish_json_field(capnp_dyn_t *d, const dyn_field_t *f,
                                  const vnode_t *v, int rc, int value_bad) {
  if (!d || rc == 0 || d->err[0])
    return;
  if (!value_bad && !d->bad_src)
    return;
  if (!d->bad_src)
    dyn_note_bad(d, v);
  if (d->bad_src)
    dyn_err_bad_json(d, field_json_name(f));
}

static int build_json_value_struct(const vnode_t *v, const capnp_bptr_t *body,
                                   int depth) {
  uint32_t i;
  if (depth > value_depth || !v)
    return -1;
  switch (v->kind) {
  case VK_NULL:
    return capnp_builder_set_u16(body, 0, 0);
  case VK_BOOL:
    if (capnp_builder_set_u16(body, 0, 1))
      return -1;
    return capnp_builder_set_bool(body, 16, v->bval);
  case VK_FLOAT:
  case VK_INT:
  case VK_UINT: {
    double dv = v->kind == VK_FLOAT
                    ? v->fval
                    : (v->kind == VK_INT ? (double)v->ival : (double)v->uval);
    if (capnp_builder_set_u16(body, 0, 2))
      return -1;
    return capnp_builder_set_f64(body, 8, dv);
  }
  case VK_STR:
    if (capnp_builder_set_u16(body, 0, 3))
      return -1;
    return capnp_builder_set_text(body, 2, 0, v->str ? v->str : "", v->slen);
  case VK_LIST: {
    capnp_bptr_t first;
    if (capnp_builder_set_u16(body, 0, 4))
      return -1;
    if (capnp_builder_set_list_struct(body, 2, 0, v->count, 2, 1, &first))
      return -1;
    for (i = 0; i < v->count; i++) {
      capnp_bptr_t el = capnp_bptr_add(first, (size_t)i * 3u);
      if (build_json_value_struct(v->kids[i], &el, depth + 1))
        return -1;
    }
    return 0;
  }
  case VK_OBJ: {
    capnp_bptr_t first;
    if (capnp_builder_set_u16(body, 0, 5))
      return -1;
    if (capnp_builder_set_list_struct(body, 2, 0, v->count, 0, 2, &first))
      return -1;
    for (i = 0; i < v->count; i++) {
      capnp_bptr_t fld = capnp_bptr_add(first, (size_t)i * 2u);
      capnp_bptr_t slot, val_body;
      const char *k = v->kids[i]->key ? v->kids[i]->key : "";
      if (capnp_builder_set_text(&fld, 0, 0, k, strlen(k)) ||
          capnp_builder_slot(&fld, 0, 1, &slot) ||
          capnp_builder_struct(&slot, 2, 1, &val_body) ||
          build_json_value_struct(v->kids[i], &val_body, depth + 1))
        return -1;
    }
    return 0;
  }
  default:
    return -1;
  }
}

static int build_struct_from_vnode(capnp_dyn_t *d,
                                   const dyn_struct_t *st,
                                   const brand_env_t *env, const vnode_t *v,
                                   const capnp_bptr_t *body, int json,
                                   int depth);

/* What ValueTranslator::compileValue does with a text value, decided
 * before anything is written (compiler/node-translator.c++). TV_OK
 * compiles it, TV_SKIP drops it with a recoverable error, and TV_FATAL
 * is a name the resolver must look up, which `capnp` cannot do. */
enum { TV_OK = 0, TV_SKIP = 1, TV_FATAL = 2 };
enum { TR_VOID, TR_BOOL, TR_INT, TR_FLOAT, TR_TEXT, TR_DATA };

static int text_matches(int r, uint16_t which, uint16_t list_depth) {
  if (list_depth > 0)
    return 0;
  switch (r) {
  case TR_VOID:
    return which == TY_VOID;
  case TR_BOOL:
    return which == TY_BOOL;
  case TR_INT:
    return (which >= TY_I8 && which <= TY_U64) || which == TY_F32 ||
           which == TY_F64;
  case TR_FLOAT:
    return which == TY_F32 || which == TY_F64;
  case TR_TEXT:
    return which == TY_TEXT;
  case TR_DATA:
    return which == TY_DATA;
  default:
    return 0;
  }
}

static int text_verdict(capnp_dyn_t *d, uint16_t which, uint16_t list_depth,
                        uint64_t type_id, const brand_env_t *env,
                        const vnode_t *v) {
  int is_enum = list_depth == 0 && which == TY_ENUM;
  int is_data = list_depth == 0 && which == TY_DATA;
  int is_struct = list_depth == 0 && which == TY_STRUCT;
  int r;
  switch (v->kind) {
  case VK_IDENT: {
    uint16_t ord;
    if (is_enum && vnode_to_enum(d, type_id, v, 0, &ord) == 0)
      return TV_OK;
    return TV_FATAL;
  }
  case VK_VOID:
  case VK_BOOL:
  case VK_FLOAT:
    if (v->ident && is_enum)
      return TV_FATAL;
    r = v->kind == VK_VOID ? TR_VOID : v->kind == VK_BOOL ? TR_BOOL : TR_FLOAT;
    break;
  case VK_INT:
    if (v->neg_big)
      return TV_SKIP;
    r = TR_INT;
    break;
  case VK_UINT:
    r = TR_INT;
    break;
  case VK_STR:
    r = is_data ? TR_DATA : TR_TEXT;
    break;
  case VK_EXTERN:
    return TV_FATAL;
  case VK_BYTES:
    return is_data ? TV_OK : TV_SKIP;
  case VK_LIST:
    return list_depth > 0 ? TV_OK : TV_SKIP;
  case VK_OBJ:
    return is_struct ? TV_OK : TV_SKIP;
  default:
    return TV_SKIP;
  }
  if (text_matches(r, which, list_depth))
    return TV_OK;
  /* A struct takes a value that matches its first field. */
  if (is_struct) {
    dyn_struct_t *st = find_struct_id(d, type_id);
    rtype_t f0;
    int ok = 0;
    if (st && st->nfields > 0 && !st->fields[0].is_group &&
        resolve_field_type(d, &st->fields[0], env, &f0) == 0) {
      ok = text_matches(r, f0.which, f0.list_depth);
      free_rtype(&f0);
    }
    if (ok)
      return TV_OK;
  }
  return TV_SKIP;
}

/* For a text list element: 1 to fill it, 0 to leave it default after a
 * recoverable error, -1 to fail. JSON elements are always filled. */
static int text_elem(capnp_dyn_t *d, const rtype_t *rt, const vnode_t *kv,
                     int json) {
  int tv;
  if (json)
    return 1;
  tv = text_verdict(d, rt->which, rt->list_depth, rt->type_id, &rt->brand, kv);
  if (tv == TV_FATAL)
    return -1;
  if (tv == TV_SKIP)
    return soft_error() ? -1 : 0;
  return 1;
}

static int build_list_from_vnode(capnp_dyn_t *d, const rtype_t *elem_rt,
                                 const vnode_t *v, const capnp_bptr_t *body,
                                 uint16_t dwords, uint16_t index, int json,
                                 int depth) {
  uint32_t n, i;
  if (depth > value_depth || !v)
    return -1;
  /* JsonCodec reads a non-array as an empty list, recoverably. */
  if (v->kind != VK_LIST && (!json || soft_error()))
    return -1;
  n = v->kind == VK_LIST ? v->count : 0;
  if (elem_rt->list_depth > 0) {
    capnp_bptr_t first;
    rtype_t inner = *elem_rt;
    inner.list_depth--;
    if (capnp_builder_init_ptr_list(body, dwords, index, n, &first))
      return -1;
    for (i = 0; i < n; i++) {
      capnp_bptr_t el = capnp_bptr_add(first, i);
      int use = text_elem(d, elem_rt, v->kids[i], json);
      if (use < 0)
        return -1;
      if (use &&
          build_list_from_vnode(d, &inner, v->kids[i], &el, 0, 0, json,
                                depth + 1))
        return -1;
    }
    return 0;
  }
  switch (elem_rt->which) {
  case TY_VOID:
    for (i = 0; i < n; i++) {
      if (text_elem(d, elem_rt, v->kids[i], json) < 0)
        return -1;
    }
    return capnp_builder_set_list_void(body, dwords, index, n);
  case TY_BOOL: {
    uint8_t *items = n ? (uint8_t *)malloc(n) : NULL;
    int rc;
    if (n && !items)
      return -1;
    for (i = 0; i < n; i++) {
      int use = text_elem(d, elem_rt, v->kids[i], json);
      if (use < 0 || (use && v->kids[i]->kind != VK_BOOL)) {
        dyn_note_bad(d, v->kids[i]);
        free(items);
        return -1;
      }
      items[i] = (uint8_t)(use && v->kids[i]->bval ? 1 : 0);
    }
    rc = capnp_builder_set_list_bool(body, dwords, index, items, n);
    free(items);
    return rc;
  }
  case TY_I8:
  case TY_U8: {
    uint8_t *items = n ? (uint8_t *)malloc(n) : NULL;
    int rc;
    if (n && !items)
      return -1;
    for (i = 0; i < n; i++) {
      uint64_t bits = 0;
      int use = text_elem(d, elem_rt, v->kids[i], json);
      if (use < 0 ||
          (use && vnode_to_int(v->kids[i], elem_rt->which, json, &bits))) {
        dyn_note_bad(d, v->kids[i]);
        free(items);
        return -1;
      }
      items[i] = (uint8_t)bits;
    }
    rc = capnp_builder_set_list_u8(body, dwords, index, items, n);
    free(items);
    return rc;
  }
  case TY_I16:
  case TY_U16:
  case TY_ENUM: {
    uint16_t *items = n ? (uint16_t *)malloc((size_t)n * sizeof(uint16_t)) : NULL;
    int rc;
    if (n && !items)
      return -1;
    for (i = 0; i < n; i++) {
      int use = text_elem(d, elem_rt, v->kids[i], json);
      if (use < 0) {
        free(items);
        return -1;
      }
      items[i] = 0;
      if (!use)
        continue;
      if (elem_rt->which == TY_ENUM) {
        if (vnode_to_enum(d, elem_rt->type_id, v->kids[i], json, &items[i])) {
          dyn_note_bad(d, v->kids[i]);
          free(items);
          return -1;
        }
      } else {
        uint64_t bits = 0;
        if (vnode_to_int(v->kids[i], elem_rt->which, json, &bits)) {
          dyn_note_bad(d, v->kids[i]);
          free(items);
          return -1;
        }
        items[i] = (uint16_t)bits;
      }
    }
    rc = capnp_builder_set_list_u16(body, dwords, index, items, n);
    free(items);
    return rc;
  }
  case TY_I32:
  case TY_U32:
  case TY_F32: {
    uint32_t *items = n ? (uint32_t *)malloc((size_t)n * sizeof(uint32_t)) : NULL;
    int rc;
    if (n && !items)
      return -1;
    for (i = 0; i < n; i++) {
      uint64_t bits = 0;
      int use = text_elem(d, elem_rt, v->kids[i], json);
      int err = use < 0 ? -1
                : !use  ? 0
                : (elem_rt->which == TY_F32)
                    ? vnode_to_float(v->kids[i], TY_F32, json, &bits)
                    : vnode_to_int(v->kids[i], elem_rt->which, json, &bits);
      if (err) {
        dyn_note_bad(d, v->kids[i]);
        free(items);
        return -1;
      }
      items[i] = (uint32_t)bits;
    }
    rc = capnp_builder_set_list_u32(body, dwords, index, items, n);
    free(items);
    return rc;
  }
  case TY_I64:
  case TY_U64:
  case TY_F64: {
    uint64_t *items = n ? (uint64_t *)malloc((size_t)n * sizeof(uint64_t)) : NULL;
    int rc;
    if (n && !items)
      return -1;
    for (i = 0; i < n; i++) {
      uint64_t bits = 0;
      int use = text_elem(d, elem_rt, v->kids[i], json);
      int err = use < 0 ? -1
                : !use  ? 0
                : (elem_rt->which == TY_F64)
                    ? vnode_to_float(v->kids[i], TY_F64, json, &bits)
                    : vnode_to_int(v->kids[i], elem_rt->which, json, &bits);
      if (err) {
        dyn_note_bad(d, v->kids[i]);
        free(items);
        return -1;
      }
      items[i] = bits;
    }
    rc = capnp_builder_set_list_u64(body, dwords, index, items, n);
    free(items);
    return rc;
  }
  case TY_TEXT: {
    const char **texts =
        n ? (const char **)malloc((size_t)n * sizeof(const char *)) : NULL;
    size_t *lens = n ? (size_t *)malloc((size_t)n * sizeof(size_t)) : NULL;
    int rc, dropped = 0;
    if (n && (!texts || !lens)) {
      free(texts);
      free(lens);
      return -1;
    }
    for (i = 0; i < n; i++) {
      int use = text_elem(d, elem_rt, v->kids[i], json);
      if (use < 0 || (use && v->kids[i]->kind != VK_STR)) {
        dyn_note_bad(d, v->kids[i]);
        free(texts);
        free(lens);
        return -1;
      }
      texts[i] = use ? (v->kids[i]->str ? v->kids[i]->str : "") : NULL;
      lens[i] = use ? v->kids[i]->slen : 0;
      if (!use)
        dropped = 1;
    }
    if (!dropped) {
      rc = capnp_builder_set_list_text_n(body, dwords, index, texts, lens, n);
    } else {
      /* A dropped element stays null. */
      capnp_bptr_t first;
      rc = capnp_builder_init_ptr_list(body, dwords, index, n, &first);
      for (i = 0; i < n && rc == 0; i++) {
        capnp_bptr_t el = capnp_bptr_add(first, i);
        if (texts[i])
          rc = capnp_builder_set_text(&el, 0, 0, texts[i], lens[i]);
      }
    }
    free(texts);
    free(lens);
    return rc;
  }
  case TY_DATA: {
    capnp_bptr_t first;
    if (capnp_builder_init_ptr_list(body, dwords, index, n, &first))
      return -1;
    for (i = 0; i < n; i++) {
      capnp_bptr_t el = capnp_bptr_add(first, i);
      const vnode_t *kv = v->kids[i];
      int use = text_elem(d, elem_rt, kv, json);
      if (use < 0)
        return -1;
      if (!use)
        continue;
      if (!json && (kv->kind == VK_STR || kv->kind == VK_BYTES)) {
        if (capnp_builder_set_data(&el, 0, 0,
                                   (const uint8_t *)(kv->str ? kv->str : ""),
                                   kv->slen))
          return -1;
      } else if (json && kv->kind == VK_LIST) {
        uint32_t j, bn = kv->count;
        uint8_t *bytes = bn ? (uint8_t *)malloc(bn) : NULL;
        int rc;
        if (bn && !bytes)
          return -1;
        for (j = 0; j < bn; j++) {
          uint64_t bval = 0;
          if (json_byte(kv->kids[j], &bval)) {
            dyn_note_bad(d, kv->kids[j]);
            free(bytes);
            return -1;
          }
          bytes[j] = (uint8_t)bval;
        }
        rc = capnp_builder_set_data(&el, 0, 0, bytes, bn);
        free(bytes);
        if (rc)
          return -1;
      } else {
        dyn_note_bad(d, kv);
        return -1;
      }
    }
    return 0;
  }
  case TY_STRUCT: {
    dyn_struct_t *es = find_struct_id(d, elem_rt->type_id);
    capnp_bptr_t first;
    if (!es)
      return -1;
    if (capnp_builder_set_list_struct(body, dwords, index, n, es->dwords,
                                      es->pwords, &first))
      return -1;
    for (i = 0; i < n; i++) {
      capnp_bptr_t el =
          capnp_bptr_add(first, (size_t)i * (es->dwords + es->pwords));
      int use = text_elem(d, elem_rt, v->kids[i], json);
      if (use < 0)
        return -1;
      if (!use)
        continue;
      if (es->id == JSON_VALUE_TYPE_ID && json) {
        if (build_json_value_struct(v->kids[i], &el, depth + 1))
          return -1;
      } else {
        if (build_struct_from_vnode(d, es, &elem_rt->brand, v->kids[i], &el,
                                    json, depth + 1))
          return -1;
      }
    }
    return 0;
  }
  default:
    return -1;
  }
}

static int copy_ptr_full(capnp_builder_t *b, uint32_t slot_seg, size_t slot_word,
                         const capnp_ptr_t *src, int depth);

/* Copy a list at the size it was built, the way adopt moves an orphan.
 * The canonical copier trims composite elements and would not match
 * capnp convert. */
static int copy_list_full(capnp_builder_t *b, uint32_t slot_seg, size_t slot_word,
                          const capnp_ptr_t *list, int depth) {
  uint32_t n = list->count;
  int esize = list->esize;
  if (esize == CAPNP_SZ_COMPOSITE) {
    int nd = list->dwords;
    int np = list->pwords;
    size_t step = (size_t)nd + (size_t)np;
    capnp_bptr_t slot, first;
    uint32_t i, k;
    slot.b = b;
    slot.seg = slot_seg;
    slot.word = slot_word;
    if (capnp_builder_set_list_struct(&slot, 0, 0, n, (uint16_t)nd,
                                      (uint16_t)np, &first))
      return -1;
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return -1;
      for (k = 0; k < (uint32_t)nd; k++) {
        uint64_t w = (k < el.dwords) ? capnp_get_u64(&el, k * 8u, 0) : 0;
        capnp_store_le64(b->segs[first.seg].data +
                             (first.word + (size_t)i * step + k) * 8u,
                         w);
      }
      for (k = 0; k < (uint32_t)np; k++) {
        capnp_ptr_t child;
        size_t cslot = first.word + (size_t)i * step + (size_t)nd + k;
        if (k < el.pwords) {
          if (capnp_getp(&el, (uint16_t)k, &child) != CAPNP_OK)
            return -1;
          if (copy_ptr_full(b, first.seg, cslot, &child, depth + 1))
            return -1;
        }
      }
    }
    return 0;
  }
  if (esize == CAPNP_SZ_PTR) {
    capnp_bptr_t slot, first;
    uint32_t i;
    slot.b = b;
    slot.seg = slot_seg;
    slot.word = slot_word;
    if (capnp_builder_init_ptr_list(&slot, 0, 0, n, &first))
      return -1;
    for (i = 0; i < n; i++) {
      capnp_ptr_t el;
      if (capnp_list_getp(list, i, &el) != CAPNP_OK)
        return -1;
      if (copy_ptr_full(b, first.seg, first.word + i, &el, depth + 1))
        return -1;
    }
    return 0;
  }
  {
    capnp_bptr_t slot;
    const uint8_t *srcb =
        list->msg->segs[list->seg].data + list->word * 8u;
    slot.b = b;
    slot.seg = slot_seg;
    slot.word = slot_word;
    switch (esize) {
    case CAPNP_SZ_VOID:
      return capnp_builder_set_list_void(&slot, 0, 0, n);
    case CAPNP_SZ_BIT: {
      size_t nbytes = ((size_t)n + 7u) / 8u;
      capnp_orphan_t o;
      capnp_bptr_t start;
      if (capnp_builder_new_orphan_list(b, CAPNP_SZ_BIT, n, &o, &start))
        return -1;
      if (nbytes) {
        uint8_t *dst = b->segs[start.seg].data + start.word * 8u;
        memcpy(dst, srcb, nbytes);
        if (n % 8u)
          dst[nbytes - 1] &= (uint8_t)((1u << (n % 8u)) - 1u);
      }
      return capnp_builder_adopt(&slot, &o);
    }
    case CAPNP_SZ_BYTE:
      return capnp_builder_set_list_u8(&slot, 0, 0, srcb, n);
    case CAPNP_SZ_TWO:
    case CAPNP_SZ_FOUR:
    case CAPNP_SZ_EIGHT: {
      size_t nbytes = (size_t)n * (esize == CAPNP_SZ_TWO ? 2u : esize == CAPNP_SZ_FOUR ? 4u : 8u);
      capnp_orphan_t o;
      capnp_bptr_t start;
      if (capnp_builder_new_orphan_list(b, esize, n, &o, &start))
        return -1;
      if (nbytes)
        memcpy(b->segs[start.seg].data + start.word * 8u, srcb, nbytes);
      return capnp_builder_adopt(&slot, &o);
    }
    default:
      return -1;
    }
  }
}

static int copy_ptr_full(capnp_builder_t *b, uint32_t slot_seg, size_t slot_word,
                         const capnp_ptr_t *src, int depth) {
  if (depth > value_depth)
    return -1;
  if (!src || src->kind == CAPNP_PK_NULL) {
    capnp_store_le64(b->segs[slot_seg].data + slot_word * 8u, 0);
    return 0;
  }
  if (src->kind == CAPNP_PK_LIST)
    return copy_list_full(b, slot_seg, slot_word, src, depth);
  if (src->kind == CAPNP_PK_STRUCT) {
    int nd = src->dwords;
    int np = src->pwords;
    capnp_bptr_t slot, body;
    int k;
    slot.b = b;
    slot.seg = slot_seg;
    slot.word = slot_word;
    if (capnp_builder_struct(&slot, (uint16_t)nd, (uint16_t)np, &body))
      return -1;
    for (k = 0; k < nd; k++) {
      uint64_t w = capnp_get_u64(src, (uint32_t)k * 8u, 0);
      capnp_store_le64(b->segs[body.seg].data + (body.word + (size_t)k) * 8u, w);
    }
    for (k = 0; k < np; k++) {
      capnp_ptr_t child;
      if (capnp_getp(src, (uint16_t)k, &child) != CAPNP_OK)
        return -1;
      if (copy_ptr_full(b, body.seg, body.word + (size_t)nd + (size_t)k, &child,
                        depth + 1))
        return -1;
    }
    return 0;
  }
  return -1;
}

static int adopt_group_fields(capnp_dyn_t *d, const dyn_struct_t *g,
                              const brand_env_t *env, const capnp_ptr_t *src,
                              const capnp_bptr_t *dst, int depth);

/* Copy one field from a decoded group onto the parent body.
 * Data fields are always present in NON_NULL mode. A null pointer is
 * absent. A nested group is transferred the same way. */
static int copy_group_field(capnp_dyn_t *d, const dyn_struct_t *g,
                            const dyn_field_t *f, const brand_env_t *env,
                            const capnp_ptr_t *src, const capnp_bptr_t *dst,
                            int depth) {
  rtype_t ft;
  int rc = 0;
  if (f->disc != 0xffff && g->disc_count) {
    if (write_disc(dst, g, f->disc))
      return -1;
  }
  if (f->is_group) {
    dyn_struct_t *ng = find_struct_id(d, f->group_id);
    if (!ng)
      return -1;
    return adopt_group_fields(d, ng, env, src, dst, depth + 1);
  }
  if (resolve_field_type(d, f, env, &ft))
    return -1;
  if (ft.list_depth > 0 || is_ptr_type(ft.which)) {
    capnp_ptr_t child;
    capnp_bptr_t slot;
    if (capnp_getp(src, (uint16_t)f->offset, &child) != CAPNP_OK)
      rc = -1;
    else if (child.kind != CAPNP_PK_NULL) {
      if (capnp_builder_slot(dst, g->dwords, (uint16_t)f->offset, &slot) ||
          copy_ptr_full(slot.b, slot.seg, slot.word, &child, depth + 1))
        rc = -1;
    }
    free_rtype(&ft);
    return rc;
  }
  switch (ft.which) {
  case TY_VOID:
    break;
  case TY_BOOL:
    rc = capnp_builder_set_bool(dst, f->offset, capnp_get_bool(src, f->offset, 0));
    break;
  case TY_I8:
  case TY_U8: {
    uint32_t b = data_byte(ft.which, f->offset);
    rc = capnp_builder_set_u8(dst, b, capnp_get_u8(src, b, 0));
    break;
  }
  case TY_I16:
  case TY_U16:
  case TY_ENUM: {
    uint32_t b = data_byte(ft.which, f->offset);
    rc = capnp_builder_set_u16(dst, b, capnp_get_u16(src, b, 0));
    break;
  }
  case TY_I32:
  case TY_U32:
  case TY_F32: {
    uint32_t b = data_byte(ft.which, f->offset);
    rc = capnp_builder_set_u32(dst, b, capnp_get_u32(src, b, 0));
    break;
  }
  case TY_I64:
  case TY_U64:
  case TY_F64: {
    uint32_t b = data_byte(ft.which, f->offset);
    rc = capnp_builder_set_u64(dst, b, capnp_get_u64(src, b, 0));
    break;
  }
  default:
    rc = -1;
    break;
  }
  free_rtype(&ft);
  return rc ? -1 : 0;
}

static int adopt_group_fields(capnp_dyn_t *d, const dyn_struct_t *g,
                              const brand_env_t *env, const capnp_ptr_t *src,
                              const capnp_bptr_t *dst, int depth) {
  uint32_t i;
  uint16_t which = 0xffff;
  if (!g || depth > value_depth)
    return -1;
  if (g->disc_count) {
    const dyn_field_t *af = NULL;
    struct_which(src, g, &which);
    for (i = 0; i < g->nfields; i++) {
      if (g->fields[i].disc == which) {
        af = &g->fields[i];
        break;
      }
    }
    if (af && copy_group_field(d, g, af, env, src, dst, depth + 1))
      return -1;
  }
  for (i = 0; i < g->nfields; i++) {
    const dyn_field_t *f = &g->fields[i];
    if (g->disc_count && f->disc != 0xffff)
      continue;
    if (copy_group_field(d, g, f, env, src, dst, depth + 1))
      return -1;
  }
  return 0;
}

/* Decode a non-flattened group or named union into a temporary struct,
 * then copy across only what DynamicStruct::Builder::adopt keeps. */
static int adopt_json_group(capnp_dyn_t *d, const dyn_struct_t *g,
                            const brand_env_t *env, const vnode_t *v,
                            const capnp_bptr_t *dst, int depth) {
  capnp_builder_t tmp;
  capnp_bptr_t root, body;
  capnp_message_t msg = {0};
  capnp_ptr_t src;
  capnp_segment_t *segs = NULL;
  uint32_t i, n;
  int rc;
  capnp_builder_init(&tmp);
  if (capnp_builder_root(&tmp, &root) ||
      capnp_builder_struct(&root, g->dwords, g->pwords, &body)) {
    capnp_builder_free(&tmp);
    return -1;
  }
  rc = build_struct_from_vnode(d, g, env, v, &body, 1, depth + 1);
  if (rc) {
    capnp_builder_free(&tmp);
    return rc;
  }
  n = capnp_builder_nsegs(&tmp);
  segs = n ? (capnp_segment_t *)calloc(n, sizeof(*segs)) : NULL;
  if (!segs) {
    capnp_builder_free(&tmp);
    return -1;
  }
  for (i = 0; i < n; i++) {
    segs[i].data = capnp_builder_seg_data(&tmp, i);
    segs[i].words = capnp_builder_seg_words(&tmp, i);
  }
  capnp_message_init(&msg);
  if (capnp_message_from_segments(&msg, segs, n) != CAPNP_OK ||
      capnp_root(&msg, &src) != CAPNP_OK) {
    free(segs);
    capnp_message_free(&msg);
    capnp_builder_free(&tmp);
    return -1;
  }
  free(segs);
  clear_group_wire(d, g, env, dst, depth + 1);
  rc = adopt_group_fields(d, g, env, &src, dst, depth + 1);
  capnp_message_free(&msg);
  capnp_builder_free(&tmp);
  return rc;
}

static int build_field_from_vnode(capnp_dyn_t *d, const dyn_struct_t *st,
                                  const dyn_field_t *f, const brand_env_t *env,
                                  const vnode_t *v, const capnp_bptr_t *body,
                                  int json, int depth) {
  rtype_t ft;
  int rc = 0;
  int value_bad = 0;

  if (depth > value_depth || !v)
    return -1;
  if (f->disc != 0xffff && st->disc_count) {
    /* DynamicStruct::Builder::set calls setInUnion, which stores the
     * discriminant and leaves every other arm in place (dynamic.c++).
     * JsonCodec::decodeField reaches that set through adopt
     * (compat/json.c++). */
    if (write_disc(body, st, f->disc))
      return -1;
  }
  if (f->is_group) {
    dyn_struct_t *g = find_struct_id(d, f->group_id);
    if (!g)
      return -1;
    if (json) {
      /* JsonCodec::decodeField builds the group in an orphan.
       * adopt then keeps the active union arm and present
       * non-union fields (dynamic.c++). Text stays in place. */
      rc = adopt_json_group(d, g, env, v, body, depth);
    } else {
      clear_group_wire(d, g, env, body, depth + 1);
      rc = build_struct_from_vnode(d, g, env, v, body, 0, depth + 1);
    }
    if (json && v->kind != VK_OBJ)
      value_bad = 1;
    if (json)
      dyn_finish_json_field(d, f, v, rc, value_bad);
    return rc;
  }
  if (resolve_field_type(d, f, env, &ft))
    return -1;
  if (ft.list_depth > 0) {
    rtype_t elem = ft;
    elem.list_depth--;
    if (json && v->kind != VK_LIST)
      value_bad = 1;
    rc = build_list_from_vnode(d, &elem, v, body, st->dwords,
                               (uint16_t)f->offset, json, depth + 1);
    free_rtype(&ft);
    if (json)
      dyn_finish_json_field(d, f, v, rc, value_bad);
    return rc;
  }
  switch (ft.which) {
  case TY_VOID:
    /* JsonCodec reads any JSON value as Void. */
    if (!json && v->kind != VK_VOID) {
      rc = -1;
      value_bad = 1;
    }
    break;
  case TY_BOOL:
    if (v->kind != VK_BOOL) {
      rc = -1;
      value_bad = 1;
    } else
      rc = store_bits_for_type(body, f, TY_BOOL, v->bval ? 1 : 0);
    break;
  case TY_I8:
  case TY_I16:
  case TY_I32:
  case TY_I64:
  case TY_U8:
  case TY_U16:
  case TY_U32:
  case TY_U64: {
    uint64_t bits = 0;
    if (vnode_to_int(v, ft.which, json, &bits)) {
      rc = -1;
      value_bad = 1;
    } else
      rc = store_bits_for_type(body, f, ft.which, bits);
    break;
  }
  case TY_F32:
  case TY_F64: {
    uint64_t bits = 0;
    if (vnode_to_float(v, ft.which, json, &bits)) {
      rc = -1;
      value_bad = 1;
    } else
      rc = store_bits_for_type(body, f, ft.which, bits);
    break;
  }
  case TY_ENUM: {
    uint16_t ord = 0;
    if (vnode_to_enum(d, ft.type_id, v, json, &ord)) {
      rc = -1;
      value_bad = 1;
    } else
      rc = store_bits_for_type(body, f, TY_ENUM, ord);
    break;
  }
  case TY_TEXT:
    if (v->kind != VK_STR) {
      rc = -1;
      value_bad = 1;
    } else
      rc = capnp_builder_set_text(body, st->dwords, (uint16_t)f->offset,
                                  v->str ? v->str : "", v->slen);
    break;
  case TY_DATA:
    if (!json) {
      if (v->kind != VK_STR && v->kind != VK_BYTES)
        rc = -1;
      else
        rc = capnp_builder_set_data(
            body, st->dwords, (uint16_t)f->offset,
            (const uint8_t *)(v->str ? v->str : ""), v->slen);
    } else if (f->json_data_mode == 1) {
      uint8_t *raw = NULL;
      size_t rlen = 0;
      if (v->kind != VK_STR || decode_base64_str(v->str, v->slen, &raw, &rlen)) {
        rc = -1;
        value_bad = 1;
      } else
        rc = capnp_builder_set_data(body, st->dwords, (uint16_t)f->offset, raw,
                                    rlen);
      free(raw);
    } else if (f->json_data_mode == 2) {
      uint8_t *raw = NULL;
      size_t rlen = 0;
      if (v->kind != VK_STR || decode_hex_str(v->str, v->slen, &raw, &rlen)) {
        rc = -1;
        value_bad = 1;
      } else
        rc = capnp_builder_set_data(body, st->dwords, (uint16_t)f->offset, raw,
                                    rlen);
      free(raw);
    } else if (v->kind == VK_LIST) {
      uint32_t i, bn = v->count;
      uint8_t *bytes = bn ? (uint8_t *)malloc(bn) : NULL;
      if (bn && !bytes) {
        free_rtype(&ft);
        return -1;
      }
      for (i = 0; i < bn && rc == 0; i++) {
        uint64_t bval = 0;
        if (json_byte(v->kids[i], &bval)) {
          dyn_note_bad(d, v->kids[i]);
          rc = -1;
          value_bad = 1;
        } else {
          bytes[i] = (uint8_t)bval;
        }
      }
      if (rc == 0)
        rc = capnp_builder_set_data(body, st->dwords, (uint16_t)f->offset,
                                    bytes, bn);
      free(bytes);
    } else {
      rc = -1;
      value_bad = 1;
    }
    break;
  case TY_STRUCT: {
    dyn_struct_t *cs = find_struct_id(d, ft.type_id);
    capnp_bptr_t slot, child;
    if (!cs || capnp_builder_slot(body, st->dwords, (uint16_t)f->offset, &slot))
      rc = -1;
    else if (cs->id == JSON_VALUE_TYPE_ID && json) {
      if (capnp_builder_struct(&slot, cs->dwords, cs->pwords, &child))
        rc = -1;
      else
        rc = build_json_value_struct(v, &child, depth + 1);
    } else {
      if (capnp_builder_struct(&slot, cs->dwords, cs->pwords, &child))
        rc = -1;
      else
        rc = build_struct_from_vnode(d, cs, &ft.brand, v, &child, json,
                                     depth + 1);
    }
    break;
  }
  default:
    rc = -1;
    if (json)
      value_bad = 1;
    break;
  }
  free_rtype(&ft);
  if (json)
    dyn_finish_json_field(d, f, v, rc, value_bad);
  return rc;
}
typedef struct {
  const dyn_struct_t *st;
  uint32_t disc_byte;
} union_seen_t;

typedef struct {
  union_seen_t *items;
  uint32_t count;
  uint32_t cap;
} union_seen_set_t;

static int has_union_seen(const union_seen_set_t *us, const dyn_struct_t *st) {
  uint32_t i;
  for (i = 0; i < us->count; i++) {
    if (us->items[i].st == st ||
        (st->disc_count && us->items[i].disc_byte == st->disc_byte))
      return 1;
  }
  return 0;
}

static int mark_union_seen(union_seen_set_t *us, const dyn_struct_t *st) {
  /* json.c++ inserts the tag and throws when that row exists. */
  if (has_union_seen(us, st))
    return -1;
  if (us->count == us->cap) {
    uint32_t ncap = us->cap ? us->cap * 2u : 8u;
    union_seen_t *ni =
        (union_seen_t *)realloc(us->items, (size_t)ncap * sizeof(union_seen_t));
    if (!ni)
      return -1;
    us->items = ni;
    us->cap = ncap;
  }
  us->items[us->count].st = st;
  us->items[us->count].disc_byte = st->disc_byte;
  us->count++;
  return 0;
}

/* Check if a flattened struct or group can match `key` (used to know whether
 * a key belongs to a flattened union member before the union tag is seen) */
static int schema_claims_flat_key(capnp_dyn_t *d, const dyn_struct_t *st,
                                  const disc_opts_t *disc_override,
                                  const char *union_decl_name, const char *key,
                                  int *needs_union_tag, int depth) {
  uint32_t i;
  disc_opts_t dopts;
  const char *tag_name = NULL;

  if (!st || depth > value_depth)
    return 0;
  memset(&dopts, 0, sizeof(dopts));
  if (disc_override && disc_override->present)
    dopts = *disc_override;
  else if (st->disc_opts.present)
    dopts = st->disc_opts;
  if (dopts.present) {
    if (dopts.has_name)
      tag_name = dopts.name;
    else if (union_decl_name && union_decl_name[0])
      tag_name = union_decl_name;
    if (tag_name && strcmp(key, tag_name) == 0)
      return 1;
    if (dopts.has_value_name && strcmp(key, dopts.value_name) == 0) {
      if (needs_union_tag)
        *needs_union_tag = 1;
      return 1;
    }
  }
  for (i = 0; i < st->nfields; i++) {
    const dyn_field_t *f = &st->fields[i];
    int in_union = (f->disc != 0xffff && st->disc_count > 0);
    if (f->json_flatten) {
      size_t plen = strlen(f->flatten_prefix);
      if (strncmp(key, f->flatten_prefix, plen) == 0) {
        const char *sub_key = key + plen;
        const dyn_struct_t *sub_st = NULL;
        if (f->is_group) {
          sub_st = find_struct_id(d, f->group_id);
        } else if (f->has_type_proto &&
                   capnp_get_u16(&f->type_proto, 0, 0xffff) == TY_STRUCT) {
          sub_st = find_struct_id(d, capnp_get_u64(&f->type_proto, 8, 0));
        }
        if (sub_st &&
            schema_claims_flat_key(d, sub_st, f->is_group ? &f->sub_disc : NULL,
                                   f->is_group ? f->name : NULL, sub_key, NULL,
                                   depth + 1)) {
          if (in_union && needs_union_tag)
            *needs_union_tag = 1;
          return 1;
        }
      }
    } else {
      if (in_union && dopts.present && dopts.has_value_name)
        continue;
      if (strcmp(key, f->json_name) == 0)
        return 1;
    }
  }
  return 0;
}

static int ensure_flattened_struct_slot(const dyn_struct_t *st,
                                        const dyn_field_t *f,
                                        const dyn_struct_t *cs,
                                        const capnp_bptr_t *body,
                                        capnp_bptr_t *child) {
  capnp_bptr_t slot;
  uint64_t w;
  if (capnp_builder_slot(body, st->dwords, (uint16_t)f->offset, &slot))
    return -1;
  w = capnp_load_le64(slot.b->segs[slot.seg].data +
                      slot.word * CAPNP_WORD_BYTES);
  if (w == 0) {
    return capnp_builder_struct(&slot, cs->dwords, cs->pwords, child);
  }
  if (capnp_wp_kind(w) == CAPNP_WK_STRUCT) {
    if (cs->dwords == 0 && cs->pwords == 0) {
      *child = slot;
      return 0;
    }
    child->b = slot.b;
    child->seg = slot.seg;
    child->word = (size_t)((int64_t)slot.word + 1 + capnp_wp_offset(w));
    return 0;
  }
  if (capnp_wp_kind(w) == CAPNP_WK_FAR && !capnp_wp_far_two(w)) {
    uint32_t tseg = capnp_wp_far_seg(w);
    size_t toff = capnp_wp_far_off(w);
    uint64_t pad =
        capnp_load_le64(slot.b->segs[tseg].data + toff * CAPNP_WORD_BYTES);
    child->b = slot.b;
    child->seg = tseg;
    child->word = (size_t)((int64_t)toff + 1 + capnp_wp_offset(pad));
    return 0;
  }
  return -1;
}

static int read_disc(const capnp_bptr_t *body, uint32_t disc_byte, uint16_t dwords,
                     uint16_t *out) {
  size_t nbytes, abs, seg_bytes;
  nbytes = (size_t)dwords * 8u;
  if ((size_t)disc_byte > nbytes || nbytes - (size_t)disc_byte < 2u)
    return -1;
  if (!body || !body->b || body->seg >= body->b->nsegs ||
      body->word > SIZE_MAX / 8u ||
      (size_t)disc_byte > SIZE_MAX - body->word * 8u)
    return -1;
  abs = body->word * 8u + (size_t)disc_byte;
  seg_bytes = body->b->segs[body->seg].words * 8u;
  if (abs > seg_bytes || seg_bytes - abs < 2u)
    return -1;
  *out = capnp_load_le16(body->b->segs[body->seg].data + abs);
  return 0;
}

/* Return: 1 = handled (or unknown ignored), 0 = defer (waiting on union tag),
 * -1 = error */
static int decode_json_field_entry(
    capnp_dyn_t *d, const dyn_struct_t *st, const brand_env_t *env,
    const disc_opts_t *disc_override, const char *union_decl_name,
    const char *key, const vnode_t *val, const capnp_bptr_t *body,
    union_seen_set_t *unions_seen, int depth) {
  uint32_t i;
  disc_opts_t dopts;
  const char *tag_name = NULL;

  if (depth > value_depth)
    return -1;
  memset(&dopts, 0, sizeof(dopts));
  if (disc_override && disc_override->present)
    dopts = *disc_override;
  else if (st->disc_opts.present)
    dopts = st->disc_opts;
  if (dopts.present) {
    if (dopts.has_name)
      tag_name = dopts.name;
    else if (union_decl_name && union_decl_name[0])
      tag_name = union_decl_name;
  }

  /* 1. Check if `key` is the union discriminator tag */
  if (tag_name && strcmp(key, tag_name) == 0) {
    if (val->kind != VK_STR) {
      dyn_note_bad(d, val);
      dyn_err_bad_json(d, key);
      return -1;
    }
    for (i = 0; i < st->nfields; i++) {
      const dyn_field_t *uf = &st->fields[i];
      if (uf->disc != 0xffff && st->disc_count &&
          strlen(uf->disc_name) == val->slen &&
          memcmp(uf->disc_name, val->str, val->slen) == 0) {
        /* A discriminator tag is Builder::clear on that arm only
         * (compat/json.c++). clear() stores the discriminant and
         * zeroes this arm. Other arms stay. */
        if (write_disc(body, st, uf->disc))
          return -1;
        clear_field_wire(d, st, uf, env, body, depth + 1);
        if (mark_union_seen(unions_seen, st))
          return -1;
        break;
      }
    }
    return 1;
  }

  /* 2. Check if `key` is the union valueName */
  if (dopts.present && dopts.has_value_name &&
      strcmp(key, dopts.value_name) == 0) {
    uint16_t active = 0xffff;
    if (!has_union_seen(unions_seen, st))
      return 0; /* retry after union tag is seen */
    if (read_disc(body, st->disc_byte, st->dwords, &active))
      return -1;
    for (i = 0; i < st->nfields; i++) {
      if (st->fields[i].disc == active) {
        if (build_field_from_vnode(d, st, &st->fields[i], env, val, body, 1,
                                   depth + 1))
          return -1;
        return 1;
      }
    }
    return -1;
  }

  /* 3. Check normal (non-flattened) fields */
  for (i = 0; i < st->nfields; i++) {
    const dyn_field_t *f = &st->fields[i];
    int in_union = (f->disc != 0xffff && st->disc_count > 0);
    if (f->json_flatten)
      continue;
    if (in_union && dopts.present && dopts.has_value_name)
      continue;
    if (strcmp(key, f->json_name) == 0) {
      if (build_field_from_vnode(d, st, f, env, val, body, 1, depth + 1))
        return -1;
      return 1;
    }
  }

  /* 4. Check flattened non-union fields */
  for (i = 0; i < st->nfields; i++) {
    const dyn_field_t *f = &st->fields[i];
    size_t plen;
    const char *sub_key;
    if (!f->json_flatten || (f->disc != 0xffff && st->disc_count > 0))
      continue;
    plen = strlen(f->flatten_prefix);
    if (strncmp(key, f->flatten_prefix, plen) != 0)
      continue;
    sub_key = key + plen;
    if (f->is_group) {
      dyn_struct_t *g = find_struct_id(d, f->group_id);
      int needs_tag = 0;
      if (g && schema_claims_flat_key(d, g, &f->sub_disc, f->name, sub_key,
                                      &needs_tag, depth + 1)) {
        return decode_json_field_entry(d, g, env, &f->sub_disc, f->name,
                                       sub_key, val, body, unions_seen,
                                       depth + 1);
      }
    } else {
      rtype_t ft;
      dyn_struct_t *cs;
      int needs_tag = 0;
      if (resolve_field_type(d, f, env, &ft))
        return -1;
      cs = find_struct_id(d, ft.type_id);
      if (cs && schema_claims_flat_key(d, cs, NULL, NULL, sub_key, &needs_tag,
                                       depth + 1)) {
        capnp_bptr_t child;
        int rc;
        if (ensure_flattened_struct_slot(st, f, cs, body, &child)) {
          free_rtype(&ft);
          return -1;
        }
        rc = decode_json_field_entry(d, cs, &ft.brand, NULL, NULL, sub_key, val,
                                     &child, unions_seen, depth + 1);
        free_rtype(&ft);
        return rc;
      }
      free_rtype(&ft);
    }
  }

  /* 5. Check flattened union fields */
  for (i = 0; i < st->nfields; i++) {
    const dyn_field_t *f = &st->fields[i];
    size_t plen;
    const char *sub_key;
    int claims = 0;
    if (!f->json_flatten || f->disc == 0xffff || !st->disc_count)
      continue;
    plen = strlen(f->flatten_prefix);
    if (strncmp(key, f->flatten_prefix, plen) != 0)
      continue;
    sub_key = key + plen;
    if (f->is_group) {
      dyn_struct_t *g = find_struct_id(d, f->group_id);
      if (g && schema_claims_flat_key(d, g, &f->sub_disc, f->name, sub_key,
                                      NULL, depth + 1))
        claims = 1;
    } else {
      rtype_t ft;
      if (resolve_field_type(d, f, env, &ft) == 0) {
        dyn_struct_t *cs = find_struct_id(d, ft.type_id);
        if (cs &&
            schema_claims_flat_key(d, cs, NULL, NULL, sub_key, NULL, depth + 1))
          claims = 1;
        free_rtype(&ft);
      }
    }
    if (claims) {
      uint16_t active;
      const dyn_field_t *af = NULL;
      uint32_t j;
      if (!has_union_seen(unions_seen, st))
        return 0; /* wait for union tag */
      if (read_disc(body, st->disc_byte, st->dwords, &active))
        return -1;
      for (j = 0; j < st->nfields; j++) {
        if (st->fields[j].disc == active) {
          af = &st->fields[j];
          break;
        }
      }
      if (!af || !af->json_flatten)
        return 1;
      sub_key = key + strlen(af->flatten_prefix);
      if (af->is_group) {
        dyn_struct_t *g = find_struct_id(d, af->group_id);
        if (!g)
          return -1;
        return decode_json_field_entry(d, g, env, &af->sub_disc, af->name,
                                       sub_key, val, body, unions_seen,
                                       depth + 1);
      } else {
        rtype_t ft;
        dyn_struct_t *cs;
        capnp_bptr_t child;
        int rc;
        if (resolve_field_type(d, af, env, &ft))
          return -1;
        cs = find_struct_id(d, ft.type_id);
        if (!cs || ensure_flattened_struct_slot(st, af, cs, body, &child)) {
          free_rtype(&ft);
          return -1;
        }
        rc = decode_json_field_entry(d, cs, &ft.brand, NULL, NULL, sub_key, val,
                                     &child, unions_seen, depth + 1);
        free_rtype(&ft);
        return rc;
      }
    }
  }

  /* Unknown field in JSON is ignored by default (schema evolution) */
  return 1;
}

static int build_struct_from_vnode(capnp_dyn_t *d,
                                   const dyn_struct_t *st,
                                   const brand_env_t *env, const vnode_t *v,
                                   const capnp_bptr_t *body, int json,
                                   int depth) {
  uint32_t i;
  if (depth > value_depth || !v)
    return -1;
  if (st->id == JSON_VALUE_TYPE_ID && json)
    return build_json_value_struct(v, body, depth);

  if (v->kind != VK_OBJ) {
    /* In text format, C++ coerces a non-tuple value to the struct's first field
     * if it matches */
    if (!json) {
      int tv = text_verdict(d, TY_STRUCT, 0, st->id, env, v);
      if (tv == TV_FATAL)
        return -1;
      if (tv == TV_SKIP)
        return soft_error();
      return build_field_from_vnode(d, st, &st->fields[0], env, v, body, 0,
                                    depth + 1);
    }
    if (json)
      dyn_note_bad(d, v);
    return -1;
  }

  if (!json) {
    /* ValueTranslator::fillStructValue adopts each assignment in
     * order (compiler/node-translator.c++). TextCodec::decode calls
     * it (serialize-text.c++). A repeated name overwrites the slot,
     * so the last value remains. */
    for (i = 0; i < v->count; i++) {
      const vnode_t *kv = v->kids[i];
      const char *key = kv->key ? kv->key : "";
      const dyn_field_t *f;
      f = find_field(st, key);
      if (!f) {
        if (soft_error()) {
          dyn_err_text(d, "unknown field", key, strlen(key));
          return -1;
        }
        continue;
      }
      /* A value compileValue rejects is dropped before adopt, so a union
       * member's discriminant is not written either. */
      if (!f->is_group) {
        rtype_t ft;
        int tv;
        if (resolve_field_type(d, f, env, &ft))
          return -1;
        tv = text_verdict(d, ft.which, ft.list_depth, ft.type_id, &ft.brand,
                          kv);
        free_rtype(&ft);
        if (tv == TV_FATAL || (tv == TV_SKIP && soft_error())) {
          dyn_err_text(d, "bad value", key, strlen(key));
          return -1;
        }
        if (tv == TV_SKIP)
          continue;
      }
      if (build_field_from_vnode(d, st, f, env, kv, body, 0, depth + 1)) {
        if (!d->err[0])
          dyn_err_text(d, "bad value", key, strlen(key));
        return -1;
      }
    }
    return 0;
  }

  {
    union_seen_set_t unions_seen;
    uint8_t *done = v->count ? (uint8_t *)calloc(v->count, 1) : NULL;
    uint32_t remaining = v->count;
    memset(&unions_seen, 0, sizeof(unions_seen));
    if (v->count && !done)
      return -1;
    /* JsonCodec keeps the last value when a name is repeated. */
    while (remaining > 0) {
      uint32_t progressed = 0;
      for (i = 0; i < v->count; i++) {
        int rc;
        if (done[i])
          continue;
        rc = decode_json_field_entry(
            d, st, env, NULL, NULL, v->kids[i]->key ? v->kids[i]->key : "",
            v->kids[i], body, &unions_seen, depth);
        if (rc < 0) {
          free(done);
          free(unions_seen.items);
          return -1;
        }
        if (rc > 0) {
          done[i] = 1;
          remaining--;
          progressed++;
        }
      }
      if (progressed == 0)
        break;
    }
    free(done);
    free(unions_seen.items);
    return 0;
  }
}

static void dyn_err_scan(capnp_dyn_t *d, const char *what, const scan_t *sc) {
  const char *s;
  size_t n = 0;
  if (!sc || sc->i >= sc->n) {
    dyn_set_err(d, what);
    return;
  }
  s = sc->s + sc->i;
  while (n < 48 && sc->i + n < sc->n) {
    char c = s[n];
    if (c == 0 || c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == ',' ||
        c == ')' || c == '(' || c == '}' || c == '{' || c == ']')
      break;
    n++;
  }
  if (n == 0)
    n = 1;
  dyn_err_text(d, what, s, n);
}

static int decode_root_impl(capnp_dyn_t *d, const char *type_name,
                            const char *text, size_t len, int json,
                            uint8_t **out, size_t *out_len) {
  rtype_t rt;
  dyn_struct_t *st = NULL;
  scan_t sc;
  vnode_t *root_v = NULL;
  capnp_builder_t b;
  capnp_bptr_t root, body;
  int rc;

  if (!d || !text || !out || !out_len)
    return CAPNP_ERR_ARG;
  *out = NULL;
  *out_len = 0;
  dyn_set_err(d, "");
  d->bad_src = NULL;
  d->bad_n = 0;
  value_depth = dyn_max_depth;
  if (resolve_root_type_name(d, type_name, &rt, &st) != CAPNP_OK) {
    dyn_set_err(d, "unknown type");
    return CAPNP_ERR_KIND;
  }

  memset(&sc, 0, sizeof(sc));
  sc.s = text;
  sc.n = len;
  if (json) {
    root_v = parse_json_val(&sc);
    if (!root_v || jpeek(&sc) != 0 || root_v->kind != VK_OBJ) {
      dyn_err_scan(d, "json", &sc);
      free_vnode(root_v);
      free_rtype(&rt);
      return CAPNP_ERR_KIND;
    }
  } else {
    if (!text_lexes(text, len) || tpeek(&sc) != '(') {
      dyn_err_scan(d, "text", &sc);
      free_rtype(&rt);
      return CAPNP_ERR_KIND;
    }
    root_v = parse_text_expr(&sc);
    if (!root_v || sc.fatal || tpeek(&sc) != 0 ||
        (root_v->kind != VK_OBJ && soft_error())) {
      dyn_err_scan(d, "text", &sc);
      free_vnode(root_v);
      free_rtype(&rt);
      return CAPNP_ERR_KIND;
    }
    /* TextCodec::decode recovers from "Input does not contain a struct"
     * and leaves the root empty. */
    if (root_v->kind != VK_OBJ) {
      free_vnode(root_v);
      root_v = new_vnode(VK_OBJ);
      if (!root_v) {
        free_rtype(&rt);
        return CAPNP_ERR_ALLOC;
      }
    }
  }

  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) ||
      capnp_builder_struct(&root, st->dwords, st->pwords, &body)) {
    capnp_builder_free(&b);
    free_vnode(root_v);
    free_rtype(&rt);
    return CAPNP_ERR_ALLOC;
  }
  if (build_struct_from_vnode(d, st, &rt.brand, root_v, &body, json, 0)) {
    if (!d->err[0])
      dyn_set_err(d, "parse error");
    capnp_builder_free(&b);
    free_vnode(root_v);
    free_rtype(&rt);
    return CAPNP_ERR_KIND;
  }
  free_vnode(root_v);
  free_rtype(&rt);
  rc = capnp_builder_serialize(&b, out, out_len);
  capnp_builder_free(&b);
  return rc;
}

int capnp_dyn_decode_text(capnp_dyn_t *d, const char *type_name,
                          const char *text, size_t len, uint8_t **out,
                          size_t *out_len) {
  return decode_root_impl(d, type_name, text, len, 0, out, out_len);
}

int capnp_dyn_decode_json(capnp_dyn_t *d, const char *type_name,
                          const char *text, size_t len, uint8_t **out,
                          size_t *out_len) {
  return decode_root_impl(d, type_name, text, len, 1, out, out_len);
}

int capnp_dyn_decode_json_recover(capnp_dyn_t *d, const char *type_name,
                                  const char *text, size_t len, uint8_t **out,
                                  size_t *out_len, int *soft) {
  int rc;
  dec_recover = 1;
  dec_soft = 0;
  rc = decode_root_impl(d, type_name, text, len, 1, out, out_len);
  dec_recover = 0;
  if (soft)
    *soft = dec_soft;
  return rc;
}

int capnp_dyn_decode_text_recover(capnp_dyn_t *d, const char *type_name,
                                  const char *text, size_t len, uint8_t **out,
                                  size_t *out_len, int *soft) {
  int rc;
  dec_recover = 1;
  dec_soft = 0;
  rc = decode_root_impl(d, type_name, text, len, 0, out, out_len);
  dec_recover = 0;
  if (soft)
    *soft = dec_soft;
  return rc;
}
/* --- capnp eval implementation --- */

enum { EV_BITS = 1, EV_PTR = 2, EV_TEXT_SLICE = 3, EV_DATA_SLICE = 4 };

typedef struct {
  int kind;
  rtype_t rt;
  uint64_t bits;
  capnp_ptr_t ptr;
  const char *slice_ptr;
  size_t slice_len;
} ev_t;

static void free_ev(ev_t *ev) {
  if (ev)
    free_rtype(&ev->rt);
}

static int find_schema_node_by_name(capnp_dyn_t *d, const char *name,
                                    uint16_t which, capnp_ptr_t *node) {
  uint32_t i, n = capnp_list_len(&d->nodes_list);
  for (i = 0; i < n; i++) {
    char full[160], shortn[160];
    if (capnp_list_getp(&d->nodes_list, i, node) != CAPNP_OK)
      continue;
    if (capnp_get_u16(node, 12, 0xffff) != which)
      continue;
    extract_names(node, full, sizeof(full), shortn, sizeof(shortn));
    if (strcmp(full, name) == 0 || strcmp(shortn, name) == 0)
      return CAPNP_OK;
  }
  return CAPNP_ERR_KIND;
}

static int ev_from_value(capnp_dyn_t *d, const capnp_ptr_t *value,
                         const capnp_ptr_t *type, const brand_env_t *env,
                         ev_t *ev) {
  uint16_t which;
  memset(ev, 0, sizeof(*ev));
  if (!value || value->kind != CAPNP_PK_STRUCT || !type ||
      type->kind != CAPNP_PK_STRUCT)
    return CAPNP_ERR_KIND;
  if (resolve_type_proto(d, type, env, &ev->rt, 0))
    return CAPNP_ERR_KIND;
  which = capnp_get_u16(value, 0, 0xffff);
  if (ev->rt.list_depth > 0) {
    ev->kind = EV_PTR;
    if (which == TY_LIST)
      capnp_getp(value, 0, &ev->ptr);
    return CAPNP_OK;
  }
  if (ev->rt.which == TY_TEXT) {
    const char *t = "";
    size_t tn = 0;
    capnp_ptr_t p;
    ev->kind = EV_TEXT_SLICE;
    if (which == TY_TEXT && capnp_getp(value, 0, &p) == CAPNP_OK &&
        p.kind != CAPNP_PK_NULL) {
      if (capnp_get_text(value, 0, &t, &tn) != CAPNP_OK) {
        free_ev(ev);
        return CAPNP_ERR_KIND;
      }
    }
    ev->slice_ptr = t;
    ev->slice_len = tn;
    return CAPNP_OK;
  }
  if (ev->rt.which == TY_DATA) {
    const uint8_t *p = NULL;
    size_t pn = 0;
    capnp_ptr_t q;
    ev->kind = EV_DATA_SLICE;
    if (which == TY_DATA && capnp_getp(value, 0, &q) == CAPNP_OK &&
        q.kind != CAPNP_PK_NULL) {
      if (capnp_get_data(value, 0, &p, &pn) != CAPNP_OK) {
        free_ev(ev);
        return CAPNP_ERR_KIND;
      }
    }
    ev->slice_ptr = (const char *)p;
    ev->slice_len = pn;
    return CAPNP_OK;
  }
  if (ev->rt.which == TY_STRUCT || ev->rt.which == TY_ANY ||
      ev->rt.which == TY_IFACE) {
    ev->kind = EV_PTR;
    if (which == TY_STRUCT || which == TY_ANY)
      capnp_getp(value, 0, &ev->ptr);
    return CAPNP_OK;
  }
  ev->kind = EV_BITS;
  switch (ev->rt.which) {
  case TY_VOID:
    ev->bits = 0;
    break;
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
    free_ev(ev);
    return CAPNP_ERR_KIND;
  }
  return CAPNP_OK;
}

static int ev_step_field(capnp_dyn_t *d, ev_t *ev, const char *name) {
  dyn_struct_t *st;
  dyn_field_t *f;
  rtype_t ft;
  uint16_t active = 0xffff;

  if (ev->kind != EV_PTR || ev->rt.list_depth != 0 ||
      ev->rt.which != TY_STRUCT)
    return CAPNP_ERR_KIND;
  st = find_struct_id(d, ev->rt.type_id);
  f = find_field(st, name);
  if (!f)
    return CAPNP_ERR_KIND;
  if (f->disc != 0xffff && st->disc_count) {
    struct_which(&ev->ptr, st, &active);
    if (f->disc != active)
      return CAPNP_ERR_KIND;
  }
  if (f->is_group) {
    ev->rt.type_id = f->group_id;
    return CAPNP_OK;
  }
  if (resolve_field_type(d, f, &ev->rt.brand, &ft))
    return CAPNP_ERR_KIND;
  if (ft.list_depth == 0 && !is_ptr_type(ft.which)) {
    uint64_t bits = 0;
    if (logical_bits_for_type(&ev->ptr, f, ft.which, &bits)) {
      free_rtype(&ft);
      return CAPNP_ERR_KIND;
    }
    free_rtype(&ev->rt);
    ev->rt = ft;
    ev->kind = EV_BITS;
    ev->bits = bits;
    return CAPNP_OK;
  }
  if (ft.list_depth == 0 && ft.which == TY_TEXT) {
    const char *t = "";
    size_t tn = 0;
    capnp_ptr_t p;
    memset(&p, 0, sizeof(p));
    if (ev->ptr.kind == CAPNP_PK_STRUCT)
      capnp_getp(&ev->ptr, (uint16_t)f->offset, &p);
    if (p.kind != CAPNP_PK_NULL) {
      if (capnp_get_text(&ev->ptr, (uint16_t)f->offset, &t, &tn) != CAPNP_OK) {
        free_rtype(&ft);
        return CAPNP_ERR_KIND;
      }
    } else if (f->has_def_val &&
               capnp_get_u16(&f->def_val, 0, 0xffff) == TY_TEXT) {
      capnp_get_text(&f->def_val, 0, &t, &tn);
    }
    free_rtype(&ev->rt);
    ev->rt = ft;
    ev->kind = EV_TEXT_SLICE;
    ev->slice_ptr = t;
    ev->slice_len = tn;
    return CAPNP_OK;
  }
  if (ft.list_depth == 0 && ft.which == TY_DATA) {
    const uint8_t *p = NULL;
    size_t pn = 0;
    capnp_ptr_t q;
    memset(&q, 0, sizeof(q));
    if (ev->ptr.kind == CAPNP_PK_STRUCT)
      capnp_getp(&ev->ptr, (uint16_t)f->offset, &q);
    if (q.kind != CAPNP_PK_NULL) {
      if (capnp_get_data(&ev->ptr, (uint16_t)f->offset, &p, &pn) != CAPNP_OK) {
        free_rtype(&ft);
        return CAPNP_ERR_KIND;
      }
    } else if (f->has_def_val &&
               capnp_get_u16(&f->def_val, 0, 0xffff) == TY_DATA) {
      capnp_get_data(&f->def_val, 0, &p, &pn);
    }
    free_rtype(&ev->rt);
    ev->rt = ft;
    ev->kind = EV_DATA_SLICE;
    ev->slice_ptr = (const char *)p;
    ev->slice_len = pn;
    return CAPNP_OK;
  }
  {
    capnp_ptr_t child;
    memset(&child, 0, sizeof(child));
    if (ev->ptr.kind == CAPNP_PK_STRUCT) {
      if (capnp_getp(&ev->ptr, (uint16_t)f->offset, &child) != CAPNP_OK) {
        free_rtype(&ft);
        return CAPNP_ERR_KIND;
      }
    }
    if (child.kind == CAPNP_PK_NULL && f->has_def_val)
      capnp_getp(&f->def_val, 0, &child);
    free_rtype(&ev->rt);
    ev->rt = ft;
    ev->kind = EV_PTR;
    ev->ptr = child;
    return CAPNP_OK;
  }
}

static int ev_step_index(ev_t *ev, uint32_t index) {
  if (ev->kind != EV_PTR || ev->rt.list_depth == 0 ||
      ev->ptr.kind != CAPNP_PK_LIST || index >= capnp_list_len(&ev->ptr))
    return CAPNP_ERR_KIND;
  ev->rt.list_depth--;
  if (ev->rt.list_depth > 0) {
    capnp_ptr_t sub;
    if (capnp_list_getp(&ev->ptr, index, &sub) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    ev->ptr = sub;
    return CAPNP_OK;
  }
  if (ev->rt.which == TY_TEXT) {
    const char *t = "";
    size_t tn = 0;
    if (capnp_list_get_text(&ev->ptr, index, &t, &tn) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    ev->kind = EV_TEXT_SLICE;
    ev->slice_ptr = t;
    ev->slice_len = tn;
    return CAPNP_OK;
  }
  if (ev->rt.which == TY_DATA) {
    capnp_ptr_t el;
    const uint8_t *p = NULL;
    size_t pn = 0;
    if (capnp_list_get_struct(&ev->ptr, index, &el) != CAPNP_OK ||
        capnp_get_data(&el, 0, &p, &pn) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    ev->kind = EV_DATA_SLICE;
    ev->slice_ptr = (const char *)p;
    ev->slice_len = pn;
    return CAPNP_OK;
  }
  if (ev->rt.which == TY_STRUCT) {
    capnp_ptr_t el;
    if (capnp_list_get_struct(&ev->ptr, index, &el) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    ev->ptr = el;
    return CAPNP_OK;
  }
  if (ev->rt.which == TY_ANY || ev->rt.which == TY_IFACE) {
    capnp_ptr_t el;
    if (capnp_list_getp(&ev->ptr, index, &el) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    ev->ptr = el;
    return CAPNP_OK;
  }
  ev->kind = EV_BITS;
  switch (ev->rt.which) {
  case TY_VOID:
    ev->bits = 0;
    break;
  case TY_BOOL:
    ev->bits = (uint64_t)capnp_list_get_bool(&ev->ptr, index, 0);
    break;
  case TY_I8:
  case TY_U8:
    ev->bits = capnp_list_get_u8(&ev->ptr, index, 0);
    break;
  case TY_I16:
  case TY_U16:
  case TY_ENUM:
    ev->bits = capnp_list_get_u16(&ev->ptr, index, 0);
    break;
  case TY_I32:
  case TY_U32:
  case TY_F32:
    ev->bits = capnp_list_get_u32(&ev->ptr, index, 0);
    break;
  case TY_I64:
  case TY_U64:
  case TY_F64:
    ev->bits = capnp_list_get_u64(&ev->ptr, index, 0);
    break;
  default:
    return CAPNP_ERR_KIND;
  }
  return CAPNP_OK;
}

static int read_ident_eval(const char **pp, char *out, size_t n) {
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

static int resolve_eval_expr(capnp_dyn_t *d, const char *expr, ev_t *ev) {
  const char *p = expr;
  char prefix[160];
  char ident[160];
  capnp_ptr_t node, value, type;
  dyn_struct_t *st = NULL;
  int found = 0;

  memset(ev, 0, sizeof(*ev));
  if (read_ident_eval(&p, prefix, sizeof(prefix)))
    return CAPNP_ERR_KIND;

  /* Longest prefix matching a CONST or STRUCT node */
  for (;;) {
    if (find_schema_node_by_name(d, prefix, ND_CONST, &node) == CAPNP_OK) {
      found = ND_CONST;
    } else if ((st = find_struct_by_simple_name(d, prefix)) != NULL) {
      found = ND_STRUCT;
    }
    if (*p == '.') {
      const char *save = p;
      char next_id[160];
      char trial[320];
      p++;
      if (read_ident_eval(&p, next_id, sizeof(next_id)) == 0 &&
          strlen(prefix) + 1 + strlen(next_id) < sizeof(prefix)) {
        snprintf(trial, sizeof(trial), "%s.%s", prefix, next_id);
        if (find_schema_node_by_name(d, trial, ND_CONST, &node) == CAPNP_OK ||
            find_struct_by_simple_name(d, trial) != NULL) {
          memcpy(prefix, trial, strlen(trial) + 1);
          continue;
        }
      }
      p = save;
    }
    break;
  }

  if (found == ND_CONST) {
    if (capnp_getp(&node, 4, &value) != CAPNP_OK ||
        capnp_getp(&node, 3, &type) != CAPNP_OK)
      return CAPNP_ERR_KIND;
    if (ev_from_value(d, &value, &type, NULL, ev))
      return CAPNP_ERR_KIND;
  } else if (found == ND_STRUCT && st) {
    if (*p != '.')
      return CAPNP_ERR_KIND;
    ev->kind = EV_PTR;
    ev->rt.which = TY_STRUCT;
    ev->rt.type_id = st->id;
    memset(&ev->ptr, 0, sizeof(ev->ptr));
  } else {
    return CAPNP_ERR_KIND;
  }

  while (*p) {
    if (*p == '.') {
      p++;
      if (read_ident_eval(&p, ident, sizeof(ident))) {
        free_ev(ev);
        return CAPNP_ERR_KIND;
      }
      if (ev_step_field(d, ev, ident)) {
        free_ev(ev);
        return CAPNP_ERR_KIND;
      }
    } else if (*p == '[') {
      char *end = NULL;
      unsigned long idx;
      p++;
      if (!isdigit((unsigned char)*p)) {
        free_ev(ev);
        return CAPNP_ERR_KIND;
      }
      idx = strtoul(p, &end, 10);
      if (end == p || *end != ']') {
        free_ev(ev);
        return CAPNP_ERR_KIND;
      }
      p = end + 1;
      if (ev_step_index(ev, (uint32_t)idx)) {
        free_ev(ev);
        return CAPNP_ERR_KIND;
      }
    } else {
      free_ev(ev);
      return CAPNP_ERR_KIND;
    }
  }
  return CAPNP_OK;
}

static int format_ev(capnp_dyn_t *d, const ev_t *ev, int json, int pretty,
                     char **out, size_t *len) {
  buf_t b;
  int rc = 0;

  memset(&b, 0, sizeof(b));
  if (json) {
    /* In C++ `capnp eval -ojson`, only struct values are allowed (`not a struct;
     * binary output is only available on structs`). */
    int multi = 0;
    dyn_struct_t *st;
    if (ev->kind != EV_PTR || ev->rt.list_depth != 0 ||
        ev->rt.which != TY_STRUCT)
      return CAPNP_ERR_KIND;
    st = find_struct_id(d, ev->rt.type_id);
    if (!st)
      return CAPNP_ERR_KIND;
    if (encode_json_struct(&b, d, st, &ev->rt.brand, &ev->ptr, NULL, NULL, 0,
                           &multi, 0, pretty, 0)) {
      free(b.data);
      return CAPNP_ERR_KIND;
    }
    *out = b.data;
    *len = b.n;
    return CAPNP_OK;
  }

  if (ev->kind == EV_BITS) {
    switch (ev->rt.which) {
    case TY_VOID:
      rc = buf_str(&b, "void");
      break;
    case TY_BOOL:
      rc = buf_str(&b, ev->bits ? "true" : "false");
      break;
    case TY_I8:
    case TY_I16:
    case TY_I32:
    case TY_I64:
      rc = buf_fmt_i(&b, sext(ev->bits, ev->rt.which));
      break;
    case TY_U8:
    case TY_U16:
    case TY_U32:
    case TY_U64:
      rc = buf_fmt_u(&b, ev->bits);
      break;
    case TY_F32: {
      uint32_t u = (uint32_t)ev->bits;
      float fv;
      char tmp[64];
      memcpy(&fv, &u, sizeof(fv));
      fmt_f32(fv, tmp, sizeof(tmp));
      rc = buf_str(&b, tmp);
      break;
    }
    case TY_F64: {
      double dv;
      char tmp[64];
      memcpy(&dv, &ev->bits, sizeof(dv));
      fmt_f64(dv, tmp, sizeof(tmp));
      rc = buf_str(&b, tmp);
      break;
    }
    case TY_ENUM:
      rc = encode_text_enum(&b, d, ev->rt.type_id, (uint16_t)ev->bits);
      break;
    default:
      rc = -1;
      break;
    }
  } else if (ev->kind == EV_TEXT_SLICE) {
    rc = quote_text_capnp(
        &b, (const uint8_t *)(ev->slice_ptr ? ev->slice_ptr : ""),
        ev->slice_len, 0);
  } else if (ev->kind == EV_DATA_SLICE) {
    rc = quote_text_capnp(
        &b, (const uint8_t *)(ev->slice_ptr ? ev->slice_ptr : ""),
        ev->slice_len, 1);
  } else if (ev->kind == EV_PTR) {
    if (ev->rt.list_depth > 0) {
      rtype_t elem = ev->rt;
      elem.list_depth--;
      rc = encode_text_list(&b, d, &elem, &ev->ptr, pretty ? 1 : 0, TMODE_BARE,
                            0);
    } else if (ev->rt.which == TY_STRUCT) {
      dyn_struct_t *st = find_struct_id(d, ev->rt.type_id);
      if (!st)
        rc = -1;
      else
        rc = encode_text_struct(&b, d, st, &ev->rt.brand, &ev->ptr,
                                pretty ? 1 : 0, TMODE_BARE, 0);
    } else if (ev->rt.which == TY_ANY) {
      rc = buf_str(&b, "<opaque pointer>");
    } else if (ev->rt.which == TY_IFACE) {
      rc = buf_str(&b, "<external capability>");
    } else {
      rc = -1;
    }
  } else {
    rc = -1;
  }

  if (rc || !b.data) {
    free(b.data);
    return CAPNP_ERR_KIND;
  }
  *out = b.data;
  *len = b.n;
  return CAPNP_OK;
}

int capnp_dyn_eval_ex(capnp_dyn_t *d, const char *expr, int json,
                      int pretty, char **out, size_t *len) {
  ev_t ev;
  int rc;
  if (!d || !expr || !out || !len)
    return CAPNP_ERR_ARG;
  *out = NULL;
  *len = 0;
  value_depth = dyn_max_depth;
  rc = resolve_eval_expr(d, expr, &ev);
  if (rc)
    return rc;
  rc = format_ev(d, &ev, json, pretty, out, len);
  free_ev(&ev);
  return rc;
}

int capnp_dyn_eval(capnp_dyn_t *d, const char *expr, char **out,
                   size_t *len) {
  return capnp_dyn_eval_ex(d, expr, 0, 1, out, len);
}

int capnp_dyn_eval_struct(capnp_dyn_t *d, const char *expr, uint8_t **out,
                          size_t *out_len) {
  ev_t ev;
  dyn_struct_t *st;
  capnp_builder_t b;
  capnp_bptr_t root;
  int rc;
  if (!d || !expr || !out || !out_len)
    return CAPNP_ERR_ARG;
  *out = NULL;
  *out_len = 0;
  rc = resolve_eval_expr(d, expr, &ev);
  if (rc)
    return rc;
  if (ev.kind != EV_PTR || ev.rt.list_depth != 0 || ev.rt.which != TY_STRUCT) {
    free_ev(&ev);
    return CAPNP_ERR_KIND;
  }
  st = find_struct_id(d, ev.rt.type_id);
  if (!st) {
    free_ev(&ev);
    return CAPNP_ERR_KIND;
  }
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root)) {
    capnp_builder_free(&b);
    free_ev(&ev);
    return CAPNP_ERR_ALLOC;
  }
  if (ev.ptr.kind == CAPNP_PK_STRUCT) {
    if (capnp_builder_copy_ptr(&root, &ev.ptr)) {
      capnp_builder_free(&b);
      free_ev(&ev);
      return CAPNP_ERR_ALLOC;
    }
  } else {
    capnp_bptr_t body;
    if (capnp_builder_struct(&root, st->dwords, st->pwords, &body)) {
      capnp_builder_free(&b);
      free_ev(&ev);
      return CAPNP_ERR_ALLOC;
    }
  }
  free_ev(&ev);
  rc = capnp_builder_serialize(&b, out, out_len);
  capnp_builder_free(&b);
  return rc;
}
