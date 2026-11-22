/* SPDX-License-Identifier: MIT */
/*
 * capnpc-janet: Cap'n Proto schema compiler plugin.
 *
 * Install on PATH as capnpc-janet, then:
 *   capnp compile -ojanet schema.capnp
 *   # or: capnp compile -o/path/to/capnpc-janet schema.capnp
 *
 * Reads CodeGeneratorRequest from stdin (stream-framed). Emits one
 * <file>.janet module per requested schema file with struct/enum
 * metadata and thin accessors over the capnp/ CFUN module.
 *
 * Node / Field layouts match /usr/include/capnp/schema.capnp
 * (capnp compile -ocapnp).
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_message.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* getcwd below. MSVC has no <unistd.h> and spells it _getcwd in
 * <direct.h>; MinGW and the MSYS2 environments have the POSIX header. */
#ifdef _MSC_VER
#include <direct.h>
#define capnp_janet_getcwd _getcwd
#else
#include <unistd.h>
#define capnp_janet_getcwd getcwd
#endif

/* CodeGeneratorRequest: 0 data, 4 ptrs — nodes@0, requestedFiles@1, ... */
/* Node: 6 data words, 6 ptrs — see schema.capnp */
enum {
  NODE_D = 6,
  NODE_P = 6,
  FIELD_D = 3,
  FIELD_P = 4,
  TYPE_D = 3,
  TYPE_P = 1
};

/* Node which() */
enum {
  NODE_FILE = 0,
  NODE_STRUCT = 1,
  NODE_ENUM = 2,
  NODE_IFACE = 3,
  NODE_INTERFACE = 3,
  NODE_CONST = 4,
  NODE_ANNOTATION = 5
};

/* Field which(): 0=slot, 1=group */
enum { FIELD_SLOT = 0, FIELD_GROUP = 1 };

/* Type which() — subset we emit */
enum {
  TYPE_VOID = 0,
  TYPE_BOOL = 1,
  TYPE_INT8 = 2,
  TYPE_INT16 = 3,
  TYPE_INT32 = 4,
  TYPE_INT64 = 5,
  TYPE_UINT8 = 6,
  TYPE_UINT16 = 7,
  TYPE_UINT32 = 8,
  TYPE_UINT64 = 9,
  TYPE_FLOAT32 = 10,
  TYPE_FLOAT64 = 11,
  TYPE_TEXT = 12,
  TYPE_DATA = 13,
  TYPE_LIST = 14,
  TYPE_ENUM = 15,
  TYPE_STRUCT = 16,
  TYPE_INTERFACE = 17,
  TYPE_ANY_POINTER = 18
};

static char *read_all_stdin(size_t *out_len) {
  size_t cap = 4096, n = 0;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  for (;;) {
    size_t got;
    if (n + 4096 > cap) {
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        return NULL;
      }
      buf = nb;
    }
    got = fread(buf + n, 1, 4096, stdin);
    n += got;
    if (got < 4096)
      break;
  }
  *out_len = n;
  return buf;
}

static const char *get_text(const capnp_ptr_t *s, uint16_t idx) {
  const char *t = "";
  size_t n = 0;
  if (capnp_get_text(s, idx, &t, &n) != CAPNP_OK)
    return "";
  return t ? t : "";
}

/* basename without .capnp */
static void stem_name(const char *path, char *out, size_t outn) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  snprintf(out, outn, "%s", base);
  char *dot = strrchr(out, '.');
  if (dot && strcmp(dot, ".capnp") == 0)
    *dot = '\0';
  /* sanitize to janet-ish identifier */
  for (char *p = out; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
      *p = '-';
  }
}

/* Cap'n nested names use '.'; Janet symbols cannot. */
static void janet_ident(const char *in, char *out, size_t outn) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < outn; i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c == '-' || c == '_')
      out[j++] = (char)c;
    else if (c == '.')
      out[j++] = '-';
    else
      out[j++] = '-';
  }
  out[j] = '\0';
  if (j == 0) {
    snprintf(out, outn, "anon");
  }
}

static void janet_field_ident(const char *in, char *out, size_t outn) {
  size_t i, j = 0;
  for (i = 0; in[i] && j + 1 < outn; i++) {
    unsigned char c = (unsigned char)in[i];
    if (isupper(c)) {
      if (j > 0 && out[j - 1] != '-' && j + 2 < outn)
        out[j++] = '-';
      out[j++] = (char)tolower(c);
    } else if (isalnum(c) || c == '-' || c == '_') {
      out[j++] = (char)c;
    } else {
      out[j++] = '-';
    }
  }
  out[j] = '\0';
  if (j == 0)
    snprintf(out, outn, "anon");
}

static void type_short_name(const char *display, char *out, size_t outn) {
  const char *colon = strrchr(display, ':');
  const char *sname = colon ? colon + 1 : display;
  const char *slash = strrchr(sname, '/');
  if (slash)
    sname = slash + 1;
  janet_ident(sname, out, outn);
}

static const char *type_kw(uint16_t which) {
  switch (which) {
  case TYPE_VOID:
    return "void";
  case TYPE_BOOL:
    return "bool";
  case TYPE_INT8:
    return "i8";
  case TYPE_UINT8:
    return "u8";
  case TYPE_INT16:
    return "i16";
  case TYPE_UINT16:
    return "u16";
  case TYPE_INT32:
    return "i32";
  case TYPE_UINT32:
    return "u32";
  case TYPE_INT64:
    return "i64";
  case TYPE_UINT64:
    return "u64";
  case TYPE_FLOAT32:
    return "f32";
  case TYPE_FLOAT64:
    return "f64";
  case TYPE_TEXT:
    return "text";
  case TYPE_DATA:
    return "data";
  case TYPE_LIST:
    return "list";
  case TYPE_ENUM:
    return "enum";
  case TYPE_STRUCT:
    return "struct";
  case TYPE_INTERFACE:
    return "interface";
  case TYPE_ANY_POINTER:
    return "any-pointer";
  default:
    return "unknown";
  }
}

static void scalar_default_literal(const capnp_ptr_t *field, uint16_t type,
                                   char *out, size_t outn) {
  capnp_ptr_t value;
  uint32_t bits32;
  uint64_t bits64;
  int64_t i64;
  uint64_t u64;
  float f32;
  double f64;

  snprintf(out, outn, "0");
  if (capnp_getp(field, 3, &value) != CAPNP_OK ||
      value.kind != CAPNP_PK_STRUCT)
    return;

  switch (type) {
  case TYPE_BOOL:
    snprintf(out, outn, "%s", capnp_get_bool(&value, 16, 0) ? "true" : "false");
    break;
  case TYPE_INT8:
    snprintf(out, outn, "%d", (int)(int8_t)capnp_get_u8(&value, 2, 0));
    break;
  case TYPE_INT16:
    snprintf(out, outn, "%d", (int)(int16_t)capnp_get_u16(&value, 2, 0));
    break;
  case TYPE_INT32:
    snprintf(out, outn, "%d", (int32_t)capnp_get_u32(&value, 4, 0));
    break;
  case TYPE_INT64:
    bits64 = capnp_get_u64(&value, 8, 0);
    memcpy(&i64, &bits64, sizeof(i64));
    if (i64 < INT64_C(-9007199254740992) ||
        i64 > INT64_C(9007199254740992))
      snprintf(out, outn, "(int/s64 \"%lld\")", (long long)i64);
    else
      snprintf(out, outn, "%lld", (long long)i64);
    break;
  case TYPE_UINT8:
    snprintf(out, outn, "%u", (unsigned)capnp_get_u8(&value, 2, 0));
    break;
  case TYPE_UINT16:
  case TYPE_ENUM:
    snprintf(out, outn, "%u", (unsigned)capnp_get_u16(&value, 2, 0));
    break;
  case TYPE_UINT32:
    snprintf(out, outn, "%u", (unsigned)capnp_get_u32(&value, 4, 0));
    break;
  case TYPE_UINT64:
    u64 = capnp_get_u64(&value, 8, 0);
    if (u64 > UINT64_C(9007199254740992))
      snprintf(out, outn, "(int/u64 \"%llu\")",
               (unsigned long long)u64);
    else
      snprintf(out, outn, "%llu", (unsigned long long)u64);
    break;
  case TYPE_FLOAT32:
    bits32 = capnp_get_u32(&value, 4, 0);
    memcpy(&f32, &bits32, sizeof(f32));
    if (isnan(f32))
      snprintf(out, outn, "math/nan");
    else if (isinf(f32))
      snprintf(out, outn, f32 < 0 ? "(- math/inf)" : "math/inf");
    else if (bits32 == UINT32_C(0x80000000))
      snprintf(out, outn, "-0.0");
    else
      snprintf(out, outn, "%.9g", (double)f32);
    break;
  case TYPE_FLOAT64:
    bits64 = capnp_get_u64(&value, 8, 0);
    memcpy(&f64, &bits64, sizeof(f64));
    if (isnan(f64))
      snprintf(out, outn, "math/nan");
    else if (isinf(f64))
      snprintf(out, outn, f64 < 0 ? "(- math/inf)" : "math/inf");
    else if (bits64 == UINT64_C(0x8000000000000000))
      snprintf(out, outn, "-0.0");
    else
      snprintf(out, outn, "%.17g", f64);
    break;
  default:
    break;
  }
}


/* Look up a struct node by id and report its shape.
 *
 * A method's parameter and result structs are implicit nodes; their
 * dimensions are what a caller needs and what it would otherwise have to
 * guess. Guessing low drops any field past the end silently. */
static void struct_shape(const capnp_ptr_t *nodes, uint64_t id, int *dw,
                         int *pw) {
  uint32_t i, n;
  *dw = 0;
  *pw = 0;
  n = capnp_list_len(nodes);
  for (i = 0; i < n; i++) {
    capnp_ptr_t node;
    if (capnp_list_getp(nodes, i, &node) != CAPNP_OK)
      continue;
    if (capnp_get_u64(&node, 0, 0) != id)
      continue;
    if (capnp_get_u16(&node, 12, 0xffff) != NODE_STRUCT)
      return;
    *dw = (int)capnp_get_u16(&node, 14, 0);
    *pw = (int)capnp_get_u16(&node, 24, 0);
    return;
  }
}

static int node_type_name(const capnp_ptr_t *nodes, uint64_t id, char *out,
                          size_t outn) {
  uint32_t i, n = capnp_list_len(nodes);
  for (i = 0; i < n; i++) {
    capnp_ptr_t node;
    if (capnp_list_getp(nodes, i, &node) != CAPNP_OK)
      continue;
    if (capnp_get_u64(&node, 0, 0) != id)
      continue;
    type_short_name(get_text(&node, 0), out, outn);
    return 1;
  }
  return 0;
}

static int node_by_id(const capnp_ptr_t *nodes, uint64_t id,
                      capnp_ptr_t *out) {
  uint32_t i, n = capnp_list_len(nodes);
  for (i = 0; i < n; i++) {
    capnp_ptr_t node;
    if (capnp_list_getp(nodes, i, &node) != CAPNP_OK)
      continue;
    if (capnp_get_u64(&node, 0, 0) == id) {
      *out = node;
      return 1;
    }
  }
  return 0;
}

static uint32_t scalar_byte_offset(uint16_t type, uint32_t offset) {
  switch (type) {
  case TYPE_INT16:
  case TYPE_UINT16:
  case TYPE_ENUM:
    return offset * 2u;
  case TYPE_INT32:
  case TYPE_UINT32:
  case TYPE_FLOAT32:
    return offset * 4u;
  case TYPE_INT64:
  case TYPE_UINT64:
  case TYPE_FLOAT64:
    return offset * 8u;
  default:
    return offset;
  }
}

static const char *list_accessor_suffix(uint16_t element_type) {
  switch (element_type) {
  case TYPE_BOOL:
    return "bool";
  case TYPE_INT8:
    return "i8";
  case TYPE_UINT8:
    return "u8";
  case TYPE_INT16:
    return "i16";
  case TYPE_UINT16:
  case TYPE_ENUM:
    return "u16";
  case TYPE_INT32:
    return "i32";
  case TYPE_UINT32:
    return "u32";
  case TYPE_INT64:
    return "i64";
  case TYPE_UINT64:
    return "u64";
  case TYPE_FLOAT32:
    return "f32";
  case TYPE_FLOAT64:
    return "f64";
  case TYPE_TEXT:
    return "text";
  case TYPE_VOID:
    return "void";
  default:
    return NULL;
  }
}

/* A group initializer restores the group's fields to their zero wire state.
 * This matches C++ init<Group>(): scalar schema defaults are represented by
 * zero data bits and pointer fields become null. */
static void emit_group_clear(FILE *out, const capnp_ptr_t *nodes,
                             uint64_t group_id) {
  capnp_ptr_t group;
  capnp_ptr_t fields;
  uint32_t i, count;
  if (!node_by_id(nodes, group_id, &group) ||
      capnp_getp(&group, 3, &fields) != CAPNP_OK ||
      fields.kind != CAPNP_PK_LIST)
    return;
  count = capnp_list_len(&fields);
  for (i = 0; i < count; i++) {
    capnp_ptr_t field;
    capnp_ptr_t type;
    uint16_t field_which;
    uint16_t type_which;
    uint32_t offset;
    if (capnp_list_getp(&fields, i, &field) != CAPNP_OK)
      continue;
    field_which = capnp_get_u16(&field, 8, 0xffff);
    if (field_which == FIELD_GROUP) {
      emit_group_clear(out, nodes, capnp_get_u64(&field, 16, 0));
      continue;
    }
    if (field_which != FIELD_SLOT ||
        capnp_getp(&field, 2, &type) != CAPNP_OK ||
        type.kind != CAPNP_PK_STRUCT)
      continue;
    type_which = capnp_get_u16(&type, 0, 0xffff);
    offset = capnp_get_u32(&field, 4, 0);
    switch (type_which) {
    case TYPE_BOOL:
      fprintf(out, "  (capnp/set-bool ptr %u false)\n", (unsigned)offset);
      break;
    case TYPE_INT8:
      fprintf(out, "  (capnp/set-i8 ptr %u 0)\n", (unsigned)offset);
      break;
    case TYPE_UINT8:
      fprintf(out, "  (capnp/set-u8 ptr %u 0)\n", (unsigned)offset);
      break;
    case TYPE_INT16:
      fprintf(out, "  (capnp/set-i16 ptr %u 0)\n",
              (unsigned)scalar_byte_offset(type_which, offset));
      break;
    case TYPE_UINT16:
    case TYPE_ENUM:
      fprintf(out, "  (capnp/set-u16 ptr %u 0)\n",
              (unsigned)scalar_byte_offset(type_which, offset));
      break;
    case TYPE_INT32:
      fprintf(out, "  (capnp/set-i32 ptr %u 0)\n",
              (unsigned)scalar_byte_offset(type_which, offset));
      break;
    case TYPE_UINT32:
      fprintf(out, "  (capnp/set-u32 ptr %u 0)\n",
              (unsigned)scalar_byte_offset(type_which, offset));
      break;
    case TYPE_INT64:
      fprintf(out, "  (capnp/set-i64 ptr %u 0)\n",
              (unsigned)scalar_byte_offset(type_which, offset));
      break;
    case TYPE_UINT64:
      fprintf(out, "  (capnp/set-u64 ptr %u 0)\n",
              (unsigned)scalar_byte_offset(type_which, offset));
      break;
    case TYPE_FLOAT32:
      fprintf(out, "  (capnp/set-f32 ptr %u 0)\n",
              (unsigned)scalar_byte_offset(type_which, offset));
      break;
    case TYPE_FLOAT64:
      fprintf(out, "  (capnp/set-f64 ptr %u 0)\n",
              (unsigned)scalar_byte_offset(type_which, offset));
      break;
    case TYPE_TEXT:
    case TYPE_DATA:
    case TYPE_LIST:
    case TYPE_STRUCT:
    case TYPE_INTERFACE:
    case TYPE_ANY_POINTER:
      fprintf(out, "  (capnp/clear-pointer ptr %u)\n", (unsigned)offset);
      break;
    default:
      break;
    }
  }
}

static void emit_union_select(FILE *out, const char *struct_name,
                              const char *field_name, int is_union_member) {
  if (is_union_member)
    fprintf(out,
            "  (capnp/set-u16 ptr %s-discriminant-byte %s-%s-tag)\n",
            struct_name, struct_name, field_name);
}

#define MAX_IFACE_WALK 64
#define MAX_IFACE_METHODS 1024

typedef struct {
  uint64_t seen[MAX_IFACE_WALK];
  int nseen;
  const char *names[MAX_IFACE_METHODS];
  int nnames;
  int any;
} method_walk_t;

/* One entry per method of `node` and, depth first, of its superclasses,
 * as capnpc-c++ gives `Derived::Client` every inherited method. Each
 * entry names the interface that declares it, since a call must carry
 * that id. A name already emitted, own methods first, is kept. With a
 * NULL `out` this only counts. */
static void emit_methods(FILE *out, const capnp_ptr_t *node,
                         const capnp_ptr_t *nodes, method_walk_t *w) {
  capnp_ptr_t methods, supers;
  uint64_t id = capnp_get_u64(node, 0, 0);
  uint32_t i, count;
  int k;

  if (capnp_get_u16(node, 12, 0xffff) != NODE_IFACE)
    return;
  for (k = 0; k < w->nseen; k++)
    if (w->seen[k] == id)
      return;
  if (w->nseen == MAX_IFACE_WALK)
    return;
  w->seen[w->nseen++] = id;

  /* interface.methods shares pointer slot 3 with struct.fields. */
  count = capnp_getp(node, 3, &methods) == CAPNP_OK ? capnp_list_len(&methods) : 0;
  for (i = 0; i < count; i++) {
    capnp_ptr_t m;
    const char *mname;
    int pdw, ppw, rdw, rpw, dup = 0;
    if (capnp_list_get_struct(&methods, i, &m) != CAPNP_OK)
      continue;
    mname = get_text(&m, 0);
    if (!mname)
      mname = "?";
    for (k = 0; k < w->nnames; k++)
      if (strcmp(w->names[k], mname) == 0)
        dup = 1;
    if (dup)
      continue;
    if (w->nnames < MAX_IFACE_METHODS)
      w->names[w->nnames++] = mname;
    struct_shape(nodes, capnp_get_u64(&m, 8, 0), &pdw, &ppw);
    struct_shape(nodes, capnp_get_u64(&m, 16, 0), &rdw, &rpw);
    w->any = 1;
    if (!out)
      continue;
    fprintf(out,
            "\n   :%s {:ordinal %u :params-dwords %d :params-pwords %d"
            " :results-dwords %d :results-pwords %d"
            " :interface-id (int/u64 \"0x%016llx\")}",
            mname, i, pdw, ppw, rdw, rpw, (unsigned long long)id);
  }

  /* interface.superclasses is pointer slot 4; Superclass.id is word 0. */
  count = capnp_getp(node, 4, &supers) == CAPNP_OK ? capnp_list_len(&supers) : 0;
  for (i = 0; i < count; i++) {
    capnp_ptr_t sc, base;
    if (capnp_list_get_struct(&supers, i, &sc) == CAPNP_OK &&
        node_by_id(nodes, capnp_get_u64(&sc, 0, 0), &base))
      emit_methods(out, &base, nodes, w);
  }
}

/* Emit an interface's id, its direct superclass ids, and one entry per
 * method it can be called with.
 *
 * The caller needs three things the schema already carries: which
 * interface, which ordinal, and how big the parameter struct is. Emitting
 * them keeps those out of every call site. */
static void emit_interface(FILE *out, const capnp_ptr_t *node,
                           const capnp_ptr_t *nodes) {
  char iname[256];
  capnp_ptr_t supers;
  uint32_t i, count;
  method_walk_t *w;

  if (capnp_get_u16(node, 12, 0xffff) != NODE_IFACE)
    return;
  type_short_name(get_text(node, 0), iname, sizeof(iname));

  fprintf(out, "\n# Interface %s\n", iname);
  fprintf(out, "(def %s-interface-id (int/u64 \"0x%016llx\"))\n", iname,
          (unsigned long long)capnp_get_u64(node, 0, 0));

  count = capnp_getp(node, 4, &supers) == CAPNP_OK ? capnp_list_len(&supers) : 0;
  if (count > 0) {
    fprintf(out, "(def %s-superclasses\n  [", iname);
    for (i = 0; i < count; i++) {
      capnp_ptr_t sc;
      if (capnp_list_get_struct(&supers, i, &sc) == CAPNP_OK)
        fprintf(out, "%s(int/u64 \"0x%016llx\")", i ? " " : "",
                (unsigned long long)capnp_get_u64(&sc, 0, 0));
    }
    fprintf(out, "])\n");
  }

  w = calloc(1, sizeof(*w));
  if (!w)
    return;
  emit_methods(NULL, node, nodes, w);
  if (w->any) {
    memset(w, 0, sizeof(*w));
    fprintf(out, "(def %s-methods\n  {", iname);
    emit_methods(out, node, nodes, w);
    fprintf(out, "})\n");
  }
  free(w);
}

static int type_is_parameter(const capnp_ptr_t *typ, uint64_t *scope,
                             uint16_t *index) {
  if (capnp_get_u16(typ, 0, 0xffff) != TYPE_ANY_POINTER)
    return 0;
  /* AnyPointer.which: 0 unconstrained, 1 parameter, 2 implicit method. */
  if (capnp_get_u16(typ, 8, 0xffff) != 1)
    return 0;
  if (scope)
    *scope = capnp_get_u64(typ, 16, 0);
  if (index)
    *index = capnp_get_u16(typ, 10, 0);
  return 1;
}

static int parameter_name(const capnp_ptr_t *nodes, uint64_t scope,
                          uint16_t index, char *out, size_t outn) {
  capnp_ptr_t node, params, param;
  if (!node_by_id(nodes, scope, &node))
    return 0;
  if (capnp_getp(&node, 5, &params) != CAPNP_OK || params.kind != CAPNP_PK_LIST)
    return 0;
  if (capnp_list_getp(&params, index, &param) != CAPNP_OK)
    return 0;
  janet_ident(get_text(&param, 0), out, outn);
  return 1;
}

static void janet_quote(FILE *out, const char *s, size_t n);

static void emit_value_expr(FILE *out, const capnp_ptr_t *value) {
  uint16_t which;
  const char *text;
  size_t text_len;
  if (!value || value->kind != CAPNP_PK_STRUCT) {
    fputs("nil", out);
    return;
  }
  which = capnp_get_u16(value, 0, 0xffff);
  switch (which) {
  case TYPE_BOOL:
    fputs(capnp_get_bool(value, 16, 0) ? "true" : "false", out);
    break;
  case TYPE_INT8:
    fprintf(out, "%d", (int)(int8_t)capnp_get_u8(value, 2, 0));
    break;
  case TYPE_UINT8:
    fprintf(out, "%u", (unsigned)capnp_get_u8(value, 2, 0));
    break;
  case TYPE_INT16:
    fprintf(out, "%d", (int)(int16_t)capnp_get_u16(value, 2, 0));
    break;
  case TYPE_UINT16:
  case TYPE_ENUM:
    fprintf(out, "%u", (unsigned)capnp_get_u16(value, 2, 0));
    break;
  case TYPE_INT32:
    fprintf(out, "%d", (int)(int32_t)capnp_get_u32(value, 4, 0));
    break;
  case TYPE_UINT32:
    fprintf(out, "%u", (unsigned)capnp_get_u32(value, 4, 0));
    break;
  case TYPE_INT64: {
    int64_t i64;
    uint64_t bits = capnp_get_u64(value, 8, 0);
    memcpy(&i64, &bits, sizeof(i64));
    fprintf(out, "%lld", (long long)i64);
    break;
  }
  case TYPE_UINT64:
    fprintf(out, "%llu", (unsigned long long)capnp_get_u64(value, 8, 0));
    break;
  case TYPE_TEXT:
    if (capnp_get_text(value, 0, &text, &text_len) != CAPNP_OK)
      fputs("\"\"", out);
    else
      janet_quote(out, text, text_len);
    break;
  default:
    fputs("nil", out);
    break;
  }
}

static void emit_annotation_list(FILE *out, const char *title,
                                 const capnp_ptr_t *list) {
  uint32_t i, n;
  if (!list || list->kind != CAPNP_PK_LIST)
    return;
  n = capnp_list_len(list);
  if (n == 0)
    return;
  fprintf(out, "(def %s @[\n", title);
  for (i = 0; i < n; i++) {
    capnp_ptr_t ann, val;
    if (capnp_list_getp(list, i, &ann) != CAPNP_OK)
      continue;
    fprintf(out, "  {:id (int/u64 \"0x%016llx\") :value ",
            (unsigned long long)capnp_get_u64(&ann, 0, 0));
    if (capnp_getp(&ann, 0, &val) == CAPNP_OK)
      emit_value_expr(out, &val);
    else
      fputs("nil", out);
    fprintf(out, "}\n");
  }
  fprintf(out, "])\n");
}

static void emit_parameters(FILE *out, const capnp_ptr_t *node,
                            const char *sname) {
  capnp_ptr_t params;
  uint32_t i, n;
  if (!capnp_get_bool(node, 288, 0))
    return;
  if (capnp_getp(node, 5, &params) != CAPNP_OK || params.kind != CAPNP_PK_LIST)
    return;
  n = capnp_list_len(&params);
  fprintf(out, "(def %s-generic true)\n", sname);
  fprintf(out, "(def %s-parameters @[", sname);
  for (i = 0; i < n; i++) {
    capnp_ptr_t param;
    char name[128];
    if (capnp_list_getp(&params, i, &param) != CAPNP_OK)
      continue;
    janet_ident(get_text(&param, 0), name, sizeof(name));
    fprintf(out, " \"%s\"", name);
  }
  fprintf(out, "])\n");
}

static void emit_type_token(FILE *out, const capnp_ptr_t *typ,
                            const capnp_ptr_t *nodes) {
  uint64_t scope = 0;
  uint16_t index = 0;
  char pname[128];
  if (type_is_parameter(typ, &scope, &index) &&
      parameter_name(nodes, scope, index, pname, sizeof(pname))) {
    fprintf(out, "\"%s\"", pname);
    return;
  }
  fprintf(out, ":%s", type_kw(capnp_get_u16(typ, 0, 0xffff)));
}

/* Brand.scopes is pointer 0. A scope's which is at byte 8: 0 bind, 1 inherit.
 * Bindings are pointer 0; a binding's which is at byte 0: 0 unbound, 1 type. */
static void emit_brand(FILE *out, const capnp_ptr_t *typ,
                       const capnp_ptr_t *nodes) {
  capnp_ptr_t brand, scopes;
  uint16_t tw;
  uint32_t i, n;
  tw = capnp_get_u16(typ, 0, 0xffff);
  if (tw != TYPE_STRUCT && tw != TYPE_ENUM && tw != TYPE_INTERFACE)
    return;
  if (capnp_getp(typ, 0, &brand) != CAPNP_OK || brand.kind != CAPNP_PK_STRUCT)
    return;
  if (capnp_getp(&brand, 0, &scopes) != CAPNP_OK || scopes.kind != CAPNP_PK_LIST)
    return;
  n = capnp_list_len(&scopes);
  if (n == 0)
    return;
  fprintf(out, " :brand @[");
  for (i = 0; i < n; i++) {
    capnp_ptr_t scope, binds;
    uint16_t which;
    uint32_t j, nb;
    if (capnp_list_getp(&scopes, i, &scope) != CAPNP_OK)
      continue;
    which = capnp_get_u16(&scope, 8, 0xffff);
    fprintf(out, "{:scope (int/u64 \"0x%016llx\")",
            (unsigned long long)capnp_get_u64(&scope, 0, 0));
    if (which == 1) {
      fprintf(out, " :inherit true}");
      continue;
    }
    fprintf(out, " :bind @[");
    if (capnp_getp(&scope, 0, &binds) == CAPNP_OK &&
        binds.kind == CAPNP_PK_LIST) {
      nb = capnp_list_len(&binds);
      for (j = 0; j < nb; j++) {
        capnp_ptr_t binding, btype;
        fprintf(out, " ");
        if (capnp_list_getp(&binds, j, &binding) != CAPNP_OK) {
          fprintf(out, ":unbound");
          continue;
        }
        if (capnp_get_u16(&binding, 0, 0xffff) == 0 ||
            capnp_getp(&binding, 0, &btype) != CAPNP_OK ||
            btype.kind != CAPNP_PK_STRUCT) {
          fprintf(out, ":unbound");
          continue;
        }
        emit_type_token(out, &btype, nodes);
      }
    }
    fprintf(out, "]}");
  }
  fprintf(out, "]");
}

static const char *bound_getter(uint16_t which) {
  switch (which) {
  case TYPE_BOOL:
    return "get-bool";
  case TYPE_INT8:
    return "get-i8";
  case TYPE_UINT8:
    return "get-u8";
  case TYPE_INT16:
    return "get-i16";
  case TYPE_UINT16:
  case TYPE_ENUM:
    return "get-u16";
  case TYPE_INT32:
    return "get-i32";
  case TYPE_UINT32:
    return "get-u32";
  case TYPE_INT64:
    return "get-i64";
  case TYPE_UINT64:
    return "get-u64";
  case TYPE_FLOAT32:
    return "get-f32";
  case TYPE_FLOAT64:
    return "get-f64";
  case TYPE_TEXT:
    return "get-text";
  case TYPE_DATA:
    return "get-data";
  default:
    return NULL;
  }
}

/* A use site Box(Text) binds Box.T. Emit a getter for each parameter field
 * using the bound type, so the caller does not re-decode the brand. */
static void emit_brand_accessors(FILE *out, const char *parent,
                                 const char *field, const capnp_ptr_t *typ,
                                 const capnp_ptr_t *nodes) {
  capnp_ptr_t target, params, fields, brand, scopes;
  uint64_t type_id;
  uint32_t i, nf, nbind = 0;
  capnp_ptr_t binds;
  int have_binds = 0;
  if (capnp_get_u16(typ, 0, 0xffff) != TYPE_STRUCT)
    return;
  type_id = capnp_get_u64(typ, 8, 0);
  if (!node_by_id(nodes, type_id, &target))
    return;
  if (!capnp_get_bool(&target, 288, 0))
    return;
  if (capnp_getp(typ, 0, &brand) != CAPNP_OK || brand.kind != CAPNP_PK_STRUCT)
    return;
  if (capnp_getp(&brand, 0, &scopes) != CAPNP_OK || scopes.kind != CAPNP_PK_LIST)
    return;
  for (i = 0; i < capnp_list_len(&scopes); i++) {
    capnp_ptr_t scope;
    if (capnp_list_getp(&scopes, i, &scope) != CAPNP_OK)
      continue;
    if (capnp_get_u64(&scope, 0, 0) != type_id)
      continue;
    if (capnp_get_u16(&scope, 8, 0xffff) != 0)
      continue;
    if (capnp_getp(&scope, 0, &binds) == CAPNP_OK && binds.kind == CAPNP_PK_LIST) {
      have_binds = 1;
      nbind = capnp_list_len(&binds);
    }
  }
  if (!have_binds)
    return;
  if (capnp_getp(&target, 5, &params) != CAPNP_OK || params.kind != CAPNP_PK_LIST)
    return;
  if (capnp_getp(&target, 3, &fields) != CAPNP_OK || fields.kind != CAPNP_PK_LIST)
    return;
  nf = capnp_list_len(&fields);
  for (i = 0; i < nf; i++) {
    capnp_ptr_t f, ftype;
    uint64_t scope = 0;
    uint16_t pindex = 0;
    uint32_t offset;
    capnp_ptr_t binding, btype;
    uint16_t bwhich;
    const char *getter;
    char fname[128];
    if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
      continue;
    if (capnp_get_u16(&f, 8, 0xffff) != FIELD_SLOT)
      continue;
    if (capnp_getp(&f, 2, &ftype) != CAPNP_OK || ftype.kind != CAPNP_PK_STRUCT)
      continue;
    if (!type_is_parameter(&ftype, &scope, &pindex) || scope != type_id)
      continue;
    if (pindex >= nbind || capnp_list_getp(&binds, pindex, &binding) != CAPNP_OK)
      continue;
    if (capnp_get_u16(&binding, 0, 0xffff) != 1)
      continue;
    if (capnp_getp(&binding, 0, &btype) != CAPNP_OK ||
        btype.kind != CAPNP_PK_STRUCT)
      continue;
    bwhich = capnp_get_u16(&btype, 0, 0xffff);
    getter = bound_getter(bwhich);
    if (!getter)
      continue;
    janet_field_ident(get_text(&f, 0), fname, sizeof(fname));
    offset = capnp_get_u32(&f, 4, 0);
    if (bwhich == TYPE_TEXT || bwhich == TYPE_DATA) {
      fprintf(out, "(defn %s-%s-get-%s [ptr]\n  (capnp/%s ptr %u))\n", parent,
              field, fname, getter, (unsigned)offset);
    } else if (bwhich == TYPE_BOOL) {
      fprintf(out, "(defn %s-%s-get-%s [ptr]\n  (capnp/%s ptr %u false))\n",
              parent, field, fname, getter, (unsigned)offset);
    } else {
      uint32_t byte = scalar_byte_offset(bwhich, offset);
      fprintf(out, "(defn %s-%s-get-%s [ptr]\n  (capnp/%s ptr %u 0))\n", parent,
              field, fname, getter, (unsigned)byte);
    }
  }
  (void)params;
}

static void emit_hex_string(FILE *out, const uint8_t *p, size_t n) {
  size_t i;
  fputc('"', out);
  for (i = 0; i < n; i++)
    fprintf(out, "\\x%02x", p[i]);
  fputc('"', out);
}

/* Framed canonical bytes of a message whose root is the struct or list in
 * `value`, a schema Value. 1 on success, 0 when it holds no such pointer,
 * -1 on failure. */
static int pointer_value_bytes(const capnp_ptr_t *value, uint8_t **canon,
                               size_t *clen) {
  capnp_ptr_t child;
  capnp_builder_t b;
  capnp_bptr_t root;
  capnp_message_t msg = {0};
  uint8_t *flat = NULL;
  size_t flen = 0;
  uint16_t which;
  int rc;
  which = capnp_get_u16(value, 0, 0xffff);
  if (which != TYPE_STRUCT && which != TYPE_LIST)
    return 0;
  if (capnp_getp(value, 0, &child) != CAPNP_OK || child.kind == CAPNP_PK_NULL)
    return 0;
  capnp_builder_init(&b);
  if (capnp_builder_root(&b, &root) || capnp_builder_copy_ptr(&root, &child)) {
    capnp_builder_free(&b);
    return -1;
  }
  rc = capnp_builder_serialize(&b, &flat, &flen);
  capnp_builder_free(&b);
  if (rc) {
    free(flat);
    return -1;
  }
  if (capnp_message_from_flat(&msg, flat, flen) != CAPNP_OK) {
    free(flat);
    return -1;
  }
  free(flat);
  rc = capnp_canonicalize_framed(&msg, canon, clen);
  capnp_message_free(&msg);
  if (rc != CAPNP_OK) {
    free(*canon);
    *canon = NULL;
    return -1;
  }
  return 1;
}

/* 1 when a struct or list default was emitted, 0 when there is none, -1
 * on failure. The bytes are a framed canonical message whose root is the
 * default value. */
static int emit_pointer_default(FILE *out, const char *sname, const char *fname,
                                uint32_t offset, const capnp_ptr_t *field) {
  capnp_ptr_t value;
  uint8_t *canon = NULL;
  size_t clen = 0;
  int rc;
  if (capnp_getp(field, 3, &value) != CAPNP_OK || value.kind != CAPNP_PK_STRUCT)
    return 0;
  rc = pointer_value_bytes(&value, &canon, &clen);
  if (rc <= 0)
    return rc;
  fprintf(out, "(def %s-%s-default ", sname, fname);
  emit_hex_string(out, canon, clen);
  fprintf(out, ")\n");
  fprintf(out,
          "(defn %s-get-%s [ptr]\n"
          "  (def v (capnp/getp ptr %u))\n"
          "  (if (= (capnp/kind v) :null)\n"
          "    (:root (capnp/message-from-buffer (buffer %s-%s-default)))\n"
          "    v))\n",
          sname, fname, (unsigned)offset, sname, fname);
  free(canon);
  return 1;
}

/* A Text or Data getter that returns the schema default for a null
 * pointer, as C++ getters do. 0 when the field has no default. */
static int emit_blob_default(FILE *out, const char *sname, const char *fname,
                             uint32_t offset, const capnp_ptr_t *field,
                             uint16_t tw) {
  capnp_ptr_t value, child;
  const char *text = NULL;
  const uint8_t *data = NULL;
  size_t n = 0;
  if (capnp_getp(field, 3, &value) != CAPNP_OK || value.kind != CAPNP_PK_STRUCT ||
      capnp_get_u16(&value, 0, 0xffff) != tw)
    return 0;
  if (capnp_getp(&value, 0, &child) != CAPNP_OK || child.kind == CAPNP_PK_NULL)
    return 0;
  if (tw == TYPE_TEXT ? capnp_get_text(&value, 0, &text, &n) != CAPNP_OK
                      : capnp_get_data(&value, 0, &data, &n) != CAPNP_OK)
    return 0;
  fprintf(out,
          "(defn %s-get-%s [ptr]\n"
          "  (if (= (capnp/kind (capnp/getp ptr %u)) :null)\n    ",
          sname, fname, (unsigned)offset);
  if (tw == TYPE_TEXT)
    janet_quote(out, text, n);
  else
    emit_hex_string(out, data, n);
  fprintf(out, "\n    (capnp/get-%s ptr %u)))\n",
          tw == TYPE_TEXT ? "text" : "data", (unsigned)offset);
  return 1;
}

static void emit_struct(FILE *out, const capnp_ptr_t *node,
                        const capnp_ptr_t *nodes) {
  /* Node struct group: dataWordCount @bits 112-128 = byte 14 as u16?
   * From compile: dataWordCount @7 :UInt16 bits[112, 128)
   * pointerCount @8 bits[192, 208)
   * fields @13 List(Field) ptr[3]
   * which tag bits[96,112) = byte 12
   */
  uint16_t which = capnp_get_u16(node, 12, 0xffff);
  if (which != NODE_STRUCT)
    return;

  char sname[256];
  type_short_name(get_text(node, 0), sname, sizeof(sname));

  uint16_t dwords = capnp_get_u16(node, 14, 0);
  uint16_t pwords = capnp_get_u16(node, 24, 0); /* bits 192 = byte 24 */
  uint16_t discriminant_count = capnp_get_u16(node, 30, 0);
  uint32_t disc_off = capnp_get_u32(node, 32, 0);
  uint64_t disc_bytes = (uint64_t)disc_off * 2u;
  int disc_ok = discriminant_count > 0 && disc_bytes <= UINT32_MAX &&
                disc_bytes + 2u <= (uint64_t)dwords * 8u;
  uint32_t discriminant_byte = disc_ok ? (uint32_t)disc_bytes : 0u;

  fprintf(out, "\n# struct %s\n", sname);
  fprintf(out, "(def %s-data-words %u)\n", sname, (unsigned)dwords);
  fprintf(out, "(def %s-pointer-words %u)\n", sname, (unsigned)pwords);
  emit_parameters(out, node, sname);
  {
    capnp_ptr_t anns;
    char title[320];
    if (capnp_getp(node, 2, &anns) == CAPNP_OK) {
      snprintf(title, sizeof(title), "%s-annotations", sname);
      emit_annotation_list(out, title, &anns);
    }
  }

  capnp_ptr_t fields;
  if (capnp_getp(node, 3, &fields) != CAPNP_OK || fields.kind != CAPNP_PK_LIST)
    return;

  uint32_t nf = capnp_list_len(&fields);
  if (disc_ok) {
    fprintf(out, "(def %s-discriminant-byte %u)\n", sname,
            (unsigned)discriminant_byte);
    fprintf(out,
            "(defn %s-which [ptr]\n  (capnp/get-u16 ptr %u))\n", sname,
            (unsigned)discriminant_byte);
    for (uint32_t i = 0; i < nf; i++) {
      capnp_ptr_t f;
      char fname[128];
      uint16_t discriminant;
      if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
        continue;
      discriminant = capnp_get_u16(&f, 2, 0) ^ UINT16_C(0xffff);
      if (discriminant == UINT16_C(0xffff))
        continue;
      janet_field_ident(get_text(&f, 0), fname, sizeof(fname));
      fprintf(out, "(def %s-%s-tag %u)\n", sname, fname,
              (unsigned)discriminant);
    }
  }

  fprintf(out, "(def %s-fields\n  @{", sname);
  for (uint32_t i = 0; i < nf; i++) {
    capnp_ptr_t f;
    if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
      continue;
    char fname[128];
    janet_field_ident(get_text(&f, 0), fname, sizeof(fname));
    uint16_t fwhich = capnp_get_u16(&f, 8, 0xffff);
    if (fwhich == FIELD_GROUP) {
      fprintf(out, "\n    :%s @{:type :group}", fname);
      continue;
    }
    if (fwhich != FIELD_SLOT)
      continue;
    uint32_t offset = capnp_get_u32(&f, 4, 0);
    capnp_ptr_t typ;
    uint16_t tw = 0xffff;
    if (capnp_getp(&f, 2, &typ) == CAPNP_OK && typ.kind == CAPNP_PK_STRUCT)
      tw = capnp_get_u16(&typ, 0, 0xffff); /* Type which at bits[0,16) */

    /* Slot offset meaning depends on type: data bit offset or pointer index.
     * Cap'n: for data fields offset is in units of the type size; for
     * pointers it is the pointer index. Emit raw offset + type keyword. */
    if (type_is_parameter(&typ, NULL, NULL))
      fprintf(out, "\n    :%s @{:offset %u :type :parameter", fname,
              (unsigned)offset);
    else
      fprintf(out, "\n    :%s @{:offset %u :type :%s", fname, (unsigned)offset,
              type_kw(tw));
    {
      uint64_t pscope = 0;
      uint16_t pindex = 0;
      char pname[128];
      if (type_is_parameter(&typ, &pscope, &pindex)) {
        fprintf(out, " :parameter-index %u", (unsigned)pindex);
        if (parameter_name(nodes, pscope, pindex, pname, sizeof(pname)))
          fprintf(out, " :parameter \"%s\"", pname);
      }
    }
    emit_brand(out, &typ, nodes);
    fprintf(out, "}");
  }
  fprintf(out, "})\n");
  for (uint32_t ai = 0; ai < nf; ai++) {
    capnp_ptr_t f, anns;
    char fname[128];
    char title[512];
    if (capnp_list_getp(&fields, ai, &f) != CAPNP_OK)
      continue;
    if (capnp_getp(&f, 1, &anns) != CAPNP_OK || anns.kind != CAPNP_PK_LIST)
      continue;
    janet_field_ident(get_text(&f, 0), fname, sizeof(fname));
    snprintf(title, sizeof(title), "%s-%s-annotations", sname, fname);
    emit_annotation_list(out, title, &anns);
  }

  if (!capnp_get_bool(node, 224, 0)) {
    fprintf(out,
            "(defn %s-init-root [builder]\n"
            "  (capnp/init-root builder %s-data-words "
            "%s-pointer-words))\n",
            sname, sname, sname);
  }

  /* Mutable accessors mirror the generated C++ Builder surface. Each body
   * view retains its arena owner, so nested initialization does not copy or
   * allocate a second message. */
  for (uint32_t i = 0; i < nf; i++) {
    capnp_ptr_t f;
    capnp_ptr_t typ;
    char fname[128];
    char dflt[96];
    uint16_t fwhich;
    uint16_t tw = 0xffff;
    uint16_t discriminant;
    uint32_t offset;
    int is_union_member;
    const char *setter = NULL;
    if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
      continue;
    janet_field_ident(get_text(&f, 0), fname, sizeof(fname));
    fwhich = capnp_get_u16(&f, 8, 0xffff);
    discriminant = capnp_get_u16(&f, 2, 0) ^ UINT16_C(0xffff);
    is_union_member = discriminant_count > 0 &&
                      discriminant != UINT16_C(0xffff);

    if (fwhich == FIELD_GROUP) {
      fprintf(out, "(defn %s-init-%s [ptr]\n", sname, fname);
      emit_union_select(out, sname, fname, is_union_member);
      emit_group_clear(out, nodes, capnp_get_u64(&f, 16, 0));
      fprintf(out, "  ptr)\n");
      continue;
    }
    if (fwhich != FIELD_SLOT ||
        capnp_getp(&f, 2, &typ) != CAPNP_OK ||
        typ.kind != CAPNP_PK_STRUCT)
      continue;
    tw = capnp_get_u16(&typ, 0, 0xffff);
    offset = capnp_get_u32(&f, 4, 0);
    scalar_default_literal(&f, tw, dflt, sizeof(dflt));

    switch (tw) {
    case TYPE_BOOL:
      setter = "set-bool";
      break;
    case TYPE_INT8:
      setter = "set-i8";
      break;
    case TYPE_UINT8:
      setter = "set-u8";
      break;
    case TYPE_INT16:
      setter = "set-i16";
      break;
    case TYPE_UINT16:
    case TYPE_ENUM:
      setter = "set-u16";
      break;
    case TYPE_INT32:
      setter = "set-i32";
      break;
    case TYPE_UINT32:
      setter = "set-u32";
      break;
    case TYPE_INT64:
      setter = "set-i64";
      break;
    case TYPE_UINT64:
      setter = "set-u64";
      break;
    case TYPE_FLOAT32:
      setter = "set-f32";
      break;
    case TYPE_FLOAT64:
      setter = "set-f64";
      break;
    default:
      break;
    }

    if (setter) {
      uint32_t wire_offset =
          tw == TYPE_BOOL ? offset : scalar_byte_offset(tw, offset);
      fprintf(out, "(defn %s-set-%s [ptr value]\n", sname, fname);
      emit_union_select(out, sname, fname, is_union_member);
      fprintf(out, "  (capnp/%s ptr %u value %s))\n", setter,
              (unsigned)wire_offset, dflt);
      continue;
    }

    switch (tw) {
    case TYPE_VOID:
      fprintf(out, "(defn %s-set-%s [ptr]\n", sname, fname);
      emit_union_select(out, sname, fname, is_union_member);
      fprintf(out, "  ptr)\n");
      break;
    case TYPE_TEXT:
      fprintf(out, "(defn %s-set-%s [ptr value]\n", sname, fname);
      emit_union_select(out, sname, fname, is_union_member);
      fprintf(out, "  (capnp/set-text ptr %u value))\n", (unsigned)offset);
      break;
    case TYPE_DATA:
      fprintf(out, "(defn %s-set-%s [ptr value]\n", sname, fname);
      emit_union_select(out, sname, fname, is_union_member);
      fprintf(out, "  (capnp/set-data ptr %u value))\n", (unsigned)offset);
      break;
    case TYPE_STRUCT: {
      int child_dwords, child_pwords;
      struct_shape(nodes, capnp_get_u64(&typ, 8, 0), &child_dwords,
                   &child_pwords);
      fprintf(out, "(defn %s-init-%s [ptr]\n", sname, fname);
      emit_union_select(out, sname, fname, is_union_member);
      fprintf(out, "  (capnp/init-struct ptr %u %d %d))\n",
              (unsigned)offset, child_dwords, child_pwords);
      emit_brand_accessors(out, sname, fname, &typ, nodes);
      break;
    }
    case TYPE_LIST: {
      capnp_ptr_t element_type;
      uint16_t element_which = 0xffff;
      const char *list_suffix;
      if (capnp_getp(&typ, 0, &element_type) == CAPNP_OK &&
          element_type.kind == CAPNP_PK_STRUCT)
        element_which = capnp_get_u16(&element_type, 0, 0xffff);
      if (element_which == TYPE_LIST) {
        fprintf(out, "(defn %s-init-%s [ptr count]\n", sname, fname);
        emit_union_select(out, sname, fname, is_union_member);
        fprintf(out, "  (capnp/init-ptr-list ptr %u count))\n",
                (unsigned)offset);
        break;
      }
      if (element_which == TYPE_STRUCT) {
        int child_dwords, child_pwords;
        struct_shape(nodes, capnp_get_u64(&element_type, 8, 0),
                     &child_dwords, &child_pwords);
        fprintf(out, "(defn %s-init-%s [ptr count]\n", sname, fname);
        emit_union_select(out, sname, fname, is_union_member);
        fprintf(out, "  (capnp/init-struct-list ptr %u count %d %d))\n",
                (unsigned)offset, child_dwords, child_pwords);
        break;
      }
      list_suffix = list_accessor_suffix(element_which);
      if (!list_suffix)
        break;
      if (element_which == TYPE_VOID) {
        fprintf(out, "(defn %s-set-%s [ptr count]\n", sname, fname);
        emit_union_select(out, sname, fname, is_union_member);
        fprintf(out, "  (capnp/set-list-void ptr %u count))\n",
                (unsigned)offset);
      } else {
        fprintf(out, "(defn %s-set-%s [ptr values]\n", sname, fname);
        emit_union_select(out, sname, fname, is_union_member);
        fprintf(out, "  (capnp/set-list-%s ptr %u values))\n", list_suffix,
                (unsigned)offset);
      }
      break;
    }
    default:
      break;
    }
  }

  /* Thin readers for common scalar/text fields */
  for (uint32_t i = 0; i < nf; i++) {
    capnp_ptr_t f;
    char fname[128];
    char dflt[96];
    if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
      continue;
    janet_field_ident(get_text(&f, 0), fname, sizeof(fname));
    uint16_t fwhich = capnp_get_u16(&f, 8, 0xffff);
    if (fwhich == FIELD_GROUP) {
      char group_name[256];
      uint64_t group_id = capnp_get_u64(&f, 16, 0);
      if (node_type_name(nodes, group_id, group_name, sizeof(group_name)))
        fprintf(out, "(defn %s-%s [ptr]\n  ptr)\n", sname, fname);
      continue;
    }
    if (fwhich != FIELD_SLOT)
      continue;
    uint32_t offset = capnp_get_u32(&f, 4, 0);
    capnp_ptr_t typ;
    uint16_t tw = 0xffff;
    if (capnp_getp(&f, 2, &typ) == CAPNP_OK && typ.kind == CAPNP_PK_STRUCT)
      tw = capnp_get_u16(&typ, 0, 0xffff);
    scalar_default_literal(&f, tw, dflt, sizeof(dflt));

    switch (tw) {
    case TYPE_BOOL:
      /* offset is bit index */
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-bool ptr %u %s))\n",
              sname, fname, (unsigned)offset, dflt);
      break;
    case TYPE_INT8:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-i8 ptr %u %s))\n", sname,
              fname, (unsigned)offset, dflt);
      break;
    case TYPE_UINT8:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-u8 ptr %u %s))\n", sname,
              fname, (unsigned)offset, dflt);
      break;
    case TYPE_INT16:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-i16 ptr %u %s))\n",
              sname, fname, (unsigned)(offset * 2), dflt);
      break;
    case TYPE_UINT16:
    case TYPE_ENUM:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-u16 ptr %u %s))\n",
              sname, fname, (unsigned)(offset * 2), dflt);
      break;
    case TYPE_INT32:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-i32 ptr %u %s))\n",
              sname, fname, (unsigned)(offset * 4), dflt);
      break;
    case TYPE_UINT32:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-u32 ptr %u %s))\n",
              sname, fname, (unsigned)(offset * 4), dflt);
      break;
    case TYPE_INT64:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-i64 ptr %u %s))\n",
              sname, fname, (unsigned)(offset * 8), dflt);
      break;
    case TYPE_UINT64:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-u64 ptr %u %s))\n",
              sname, fname, (unsigned)(offset * 8), dflt);
      break;
    case TYPE_FLOAT32:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-f32 ptr %u %s))\n",
              sname, fname, (unsigned)(offset * 4), dflt);
      break;
    case TYPE_FLOAT64:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-f64 ptr %u %s))\n",
              sname, fname, (unsigned)(offset * 8), dflt);
      break;
    case TYPE_TEXT:
      if (!emit_blob_default(out, sname, fname, offset, &f, tw))
        fprintf(out,
                "(defn %s-get-%s [ptr]\n  (capnp/get-text ptr %u))\n", sname,
                fname, (unsigned)offset);
      break;
    case TYPE_DATA:
      if (!emit_blob_default(out, sname, fname, offset, &f, tw))
        fprintf(out,
                "(defn %s-get-%s [ptr]\n  (capnp/get-data ptr %u))\n", sname,
                fname, (unsigned)offset);
      break;
    case TYPE_LIST: {
      capnp_ptr_t element_type;
      uint16_t element_which = 0xffff;
      const char *list_suffix;
      if (emit_pointer_default(out, sname, fname, offset, &f) <= 0)
        fprintf(out,
                "(defn %s-get-%s [ptr]\n  (capnp/getp ptr %u))\n", sname, fname,
                (unsigned)offset);
      if (capnp_getp(&typ, 0, &element_type) == CAPNP_OK &&
          element_type.kind == CAPNP_PK_STRUCT)
        element_which = capnp_get_u16(&element_type, 0, 0xffff);
      list_suffix = list_accessor_suffix(element_which);
      if (list_suffix && element_which != TYPE_VOID)
        fprintf(out,
                "(defn %s-get-%s-at [list index]\n"
                "  (capnp/list-get-%s list index))\n",
                sname, fname, list_suffix);
      break;
    }
    case TYPE_STRUCT:
      if (emit_pointer_default(out, sname, fname, offset, &f) > 0)
        break;
      /* fall through */
    case TYPE_ANY_POINTER:
    case 25: /* AnyStruct */
    case 26: /* AnyList */
    case 27: /* Capability */
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/getp ptr %u))\n", sname, fname,
              (unsigned)offset);
      break;
    default:
      break;
    }
  }
}

static void janet_quote(FILE *out, const char *s, size_t n) {
  size_t i;
  fputc('"', out);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      fputc('\\', out);
      fputc((char)c, out);
    } else if (c == '\n') {
      fputs("\\n", out);
    } else if (c == '\t') {
      fputs("\\t", out);
    } else if (c < 0x20 || c == 0x7f) {
      fprintf(out, "\\x%02x", c);
    } else {
      fputc((char)c, out);
    }
  }
  fputc('"', out);
}

static void emit_const(FILE *out, const capnp_ptr_t *node) {
  capnp_ptr_t value;
  char name[256];
  uint16_t which;
  const char *text;
  size_t text_len;
  if (capnp_get_u16(node, 12, 0xffff) != NODE_CONST)
    return;
  type_short_name(get_text(node, 0), name, sizeof(name));
  if (capnp_getp(node, 4, &value) != CAPNP_OK || value.kind != CAPNP_PK_STRUCT)
    return;
  which = capnp_get_u16(&value, 0, 0xffff);
  fprintf(out, "(def %s ", name);
  switch (which) {
  case TYPE_VOID:
    fputs("nil", out);
    break;
  case TYPE_BOOL:
    fputs(capnp_get_bool(&value, 16, 0) ? "true" : "false", out);
    break;
  case TYPE_INT8:
    fprintf(out, "%d", (int)(int8_t)capnp_get_u8(&value, 2, 0));
    break;
  case TYPE_UINT8:
    fprintf(out, "%u", (unsigned)capnp_get_u8(&value, 2, 0));
    break;
  case TYPE_INT16:
    fprintf(out, "%d", (int)(int16_t)capnp_get_u16(&value, 2, 0));
    break;
  case TYPE_UINT16:
  case TYPE_ENUM:
    fprintf(out, "%u", (unsigned)capnp_get_u16(&value, 2, 0));
    break;
  case TYPE_INT32:
    fprintf(out, "%d", (int)(int32_t)capnp_get_u32(&value, 4, 0));
    break;
  case TYPE_UINT32:
    fprintf(out, "%u", (unsigned)capnp_get_u32(&value, 4, 0));
    break;
  case TYPE_INT64: {
    int64_t i64;
    uint64_t bits = capnp_get_u64(&value, 8, 0);
    memcpy(&i64, &bits, sizeof(i64));
    if (i64 < INT64_C(-9007199254740992) || i64 > INT64_C(9007199254740992))
      fprintf(out, "(int/s64 \"%lld\")", (long long)i64);
    else
      fprintf(out, "%lld", (long long)i64);
    break;
  }
  case TYPE_UINT64: {
    uint64_t u64 = capnp_get_u64(&value, 8, 0);
    if (u64 > UINT64_C(9007199254740992))
      fprintf(out, "(int/u64 \"%llu\")", (unsigned long long)u64);
    else
      fprintf(out, "%llu", (unsigned long long)u64);
    break;
  }
  case TYPE_FLOAT32: {
    uint32_t bits = capnp_get_u32(&value, 4, 0);
    float f32;
    memcpy(&f32, &bits, sizeof(f32));
    if (isnan(f32))
      fputs("math/nan", out);
    else if (isinf(f32))
      fputs(f32 < 0 ? "(- math/inf)" : "math/inf", out);
    else if (bits == UINT32_C(0x80000000))
      fputs("-0.0", out);
    else
      fprintf(out, "%.9g", (double)f32);
    break;
  }
  case TYPE_FLOAT64: {
    uint64_t bits = capnp_get_u64(&value, 8, 0);
    double f64;
    memcpy(&f64, &bits, sizeof(f64));
    if (isnan(f64))
      fputs("math/nan", out);
    else if (isinf(f64))
      fputs(f64 < 0 ? "(- math/inf)" : "math/inf", out);
    else if (bits == UINT64_C(0x8000000000000000))
      fputs("-0.0", out);
    else
      fprintf(out, "%.17g", f64);
    break;
  }
  case TYPE_TEXT:
    if (capnp_get_text(&value, 0, &text, &text_len) != CAPNP_OK)
      fputs("\"\"", out);
    else
      janet_quote(out, text, text_len);
    break;
  case TYPE_DATA: {
    const uint8_t *data = NULL;
    size_t n = 0;
    if (capnp_get_data(&value, 0, &data, &n) != CAPNP_OK || !data)
      fputs("\"\"", out);
    else
      janet_quote(out, (const char *)data, n);
    break;
  }
  case TYPE_STRUCT:
  case TYPE_LIST: {
    uint8_t *canon = NULL;
    size_t clen = 0;
    int rc = pointer_value_bytes(&value, &canon, &clen);
    if (rc > 0) {
      fputs("(:root (capnp/message-from-buffer (buffer ", out);
      emit_hex_string(out, canon, clen);
      fputs(")))", out);
    } else if (rc == 0) {
      fputs("nil", out);
    } else {
      fputs("(error \"unreadable Cap'n Proto pointer constant\")", out);
    }
    free(canon);
    break;
  }
  default:
    fputs("(error \"unsupported Cap'n Proto pointer constant\")", out);
    break;
  }
  fputs(")\n", out);
}

static void emit_enum(FILE *out, const capnp_ptr_t *node) {
  uint16_t which = capnp_get_u16(node, 12, 0xffff);
  if (which != NODE_ENUM)
    return;
  char ename[256];
  type_short_name(get_text(node, 0), ename, sizeof(ename));

  capnp_ptr_t enumerants;
  if (capnp_getp(node, 3, &enumerants) != CAPNP_OK ||
      enumerants.kind != CAPNP_PK_LIST)
    return;

  fprintf(out, "\n# enum %s\n", ename);
  fprintf(out, "(def %s\n  @{", ename);
  uint32_t n = capnp_list_len(&enumerants);
  for (uint32_t i = 0; i < n; i++) {
    capnp_ptr_t e;
    char en[128];
    if (capnp_list_getp(&enumerants, i, &e) != CAPNP_OK)
      continue;
    janet_ident(get_text(&e, 0), en, sizeof(en));
    fprintf(out, "\n    :%s %u", en, (unsigned)i);
  }
  fprintf(out, "})\n");
}

static int write_module(const char *filename, const capnp_ptr_t *nodes,
                        uint64_t file_id) {
  char stem[256];
  char outpath[512];
  stem_name(filename, stem, sizeof(stem));
  snprintf(outpath, sizeof(outpath), "%s.janet", stem);

  FILE *out = fopen(outpath, "w");
  if (!out) {
    fprintf(stderr, "capnpc-janet: cannot write %s: %s\n", outpath,
            strerror(errno));
    return -1;
  }

  fprintf(out, "# Generated by capnpc-janet from %s — do not edit by hand.\n",
          filename);
  fprintf(out, "# Requires the capnp/ Janet module (capnp-janet runtime).\n\n");
  fprintf(out, "(import capnp)\n\n");
  fprintf(out, "(def file-id (int/u64 \"0x%llx\"))\n",
          (unsigned long long)file_id);

  /* Every node's display name starts with its file's name and a colon,
   * including method parameter structs whose scopeId is 0. */
  capnp_ptr_t file_node;
  const char *file_name = NULL;
  size_t file_name_len = 0;
  if (node_by_id(nodes, file_id, &file_node)) {
    file_name = get_text(&file_node, 0);
    file_name_len = strlen(file_name);
  }

  uint32_t nn = capnp_list_len(nodes);
  for (uint32_t i = 0; i < nn; i++) {
    capnp_ptr_t node;
    const char *display;
    if (capnp_list_getp(nodes, i, &node) != CAPNP_OK)
      continue;
    uint16_t which = capnp_get_u16(&node, 12, 0xffff);
    if (which == NODE_FILE)
      continue;
    display = get_text(&node, 0);
    if (!file_name || strncmp(display, file_name, file_name_len) != 0 ||
        display[file_name_len] != ':')
      continue;
    if (which == NODE_STRUCT)
      emit_struct(out, &node, nodes);
    else if (which == NODE_ENUM)
      emit_enum(out, &node);
    else if (which == NODE_IFACE)
      emit_interface(out, &node, nodes);
    else if (which == NODE_CONST)
      emit_const(out, &node);
  }

  fclose(out);

  /* Plugin protocol: print absolute or relative path of generated file */
  {
    char cwd[1024];
    if (capnp_janet_getcwd(cwd, sizeof(cwd)))
      printf("%s/%s\n", cwd, outpath);
    else
      printf("%s\n", outpath);
  }
  return 0;
}

int main(void) {
  size_t len = 0;
  char *buf = read_all_stdin(&len);
  capnp_message_t m = {0};
  capnp_ptr_t root, nodes, req_files;

  if (!buf || len == 0) {
    fprintf(stderr, "capnpc-janet: empty stdin (expect CodeGeneratorRequest)\n");
    free(buf);
    return 1;
  }
  if (capnp_message_from_flat(&m, (const uint8_t *)buf, len) != CAPNP_OK) {
    fprintf(stderr, "capnpc-janet: failed to parse request framing\n");
    free(buf);
    return 1;
  }
  free(buf);

  if (capnp_root(&m, &root) != CAPNP_OK) {
    fprintf(stderr, "capnpc-janet: no root\n");
    capnp_message_free(&m);
    return 1;
  }
  if (capnp_getp(&root, 0, &nodes) != CAPNP_OK ||
      nodes.kind != CAPNP_PK_LIST) {
    fprintf(stderr, "capnpc-janet: no nodes list\n");
    capnp_message_free(&m);
    return 1;
  }
  if (capnp_getp(&root, 1, &req_files) != CAPNP_OK ||
      req_files.kind != CAPNP_PK_LIST) {
    fprintf(stderr, "capnpc-janet: no requestedFiles\n");
    capnp_message_free(&m);
    return 1;
  }

  uint32_t nf = capnp_list_len(&req_files);
  if (nf == 0) {
    fprintf(stderr, "capnpc-janet: requestedFiles empty\n");
    capnp_message_free(&m);
    return 1;
  }

  for (uint32_t i = 0; i < nf; i++) {
    capnp_ptr_t rf;
    if (capnp_list_getp(&req_files, i, &rf) != CAPNP_OK)
      continue;
    uint64_t id = capnp_get_u64(&rf, 0, 0);
    const char *filename = get_text(&rf, 0);
    if (!filename[0])
      filename = "schema";
    if (write_module(filename, &nodes, id) != 0) {
      capnp_message_free(&m);
      return 1;
    }
  }

  capnp_message_free(&m);
  return 0;
}
