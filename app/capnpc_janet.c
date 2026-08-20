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
#include <capnp-janet/capnp_message.h>

#include <ctype.h>
#include <errno.h>
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
    if (bits32 == UINT32_C(0x80000000))
      snprintf(out, outn, "-0.0");
    else
      snprintf(out, outn, "%.9g", (double)f32);
    break;
  case TYPE_FLOAT64:
    bits64 = capnp_get_u64(&value, 8, 0);
    memcpy(&f64, &bits64, sizeof(f64));
    if (bits64 == UINT64_C(0x8000000000000000))
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

/* Emit an interface's id and one entry per method.
 *
 * The caller needs three things the schema already carries: which
 * interface, which ordinal, and how big the parameter struct is. Emitting
 * them keeps those out of every call site. */
static void emit_interface(FILE *out, const capnp_ptr_t *node,
                           const capnp_ptr_t *nodes) {
  char iname[256];
  capnp_ptr_t methods;
  uint32_t i, count;

  if (capnp_get_u16(node, 12, 0xffff) != NODE_IFACE)
    return;
  type_short_name(get_text(node, 0), iname, sizeof(iname));

  fprintf(out, "\n# Interface %s\n", iname);
  fprintf(out, "(def %s-interface-id (int/u64 \"0x%016llx\"))\n", iname,
          (unsigned long long)capnp_get_u64(node, 0, 0));

  /* interface.methods shares pointer slot 3 with struct.fields. */
  if (capnp_getp(node, 3, &methods) != CAPNP_OK)
    return;
  count = capnp_list_len(&methods);
  if (count == 0)
    return;

  fprintf(out, "(def %s-methods\n  {", iname);
  for (i = 0; i < count; i++) {
    capnp_ptr_t m;
    const char *mname;
    int pdw, ppw, rdw, rpw;
    if (capnp_list_get_struct(&methods, i, &m) != CAPNP_OK)
      continue;
    mname = get_text(&m, 0);
    struct_shape(nodes, capnp_get_u64(&m, 8, 0), &pdw, &ppw);
    struct_shape(nodes, capnp_get_u64(&m, 16, 0), &rdw, &rpw);
    fprintf(out,
            "\n   :%s {:ordinal %u :params-dwords %d :params-pwords %d"
            " :results-dwords %d :results-pwords %d}",
            mname ? mname : "?", i, pdw, ppw, rdw, rpw);
  }
  fprintf(out, "})\n");
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
  uint32_t discriminant_byte = capnp_get_u32(node, 32, 0) * 2u;

  fprintf(out, "\n# struct %s\n", sname);
  fprintf(out, "(def %s-data-words %u)\n", sname, (unsigned)dwords);
  fprintf(out, "(def %s-pointer-words %u)\n", sname, (unsigned)pwords);

  capnp_ptr_t fields;
  if (capnp_getp(node, 3, &fields) != CAPNP_OK || fields.kind != CAPNP_PK_LIST)
    return;

  uint32_t nf = capnp_list_len(&fields);
  if (discriminant_count > 0) {
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
      janet_ident(get_text(&f, 0), fname, sizeof(fname));
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
    janet_ident(get_text(&f, 0), fname, sizeof(fname));
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
    fprintf(out, "\n    :%s @{:offset %u :type :%s}", fname, (unsigned)offset,
            type_kw(tw));
  }
  fprintf(out, "})\n");

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
    janet_ident(get_text(&f, 0), fname, sizeof(fname));
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
      break;
    }
    case TYPE_LIST: {
      capnp_ptr_t element_type;
      uint16_t element_which = 0xffff;
      const char *list_suffix;
      if (capnp_getp(&typ, 0, &element_type) == CAPNP_OK &&
          element_type.kind == CAPNP_PK_STRUCT)
        element_which = capnp_get_u16(&element_type, 0, 0xffff);
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
    janet_ident(get_text(&f, 0), fname, sizeof(fname));
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
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-text ptr %u))\n", sname,
              fname, (unsigned)offset);
      break;
    case TYPE_DATA:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-data ptr %u))\n", sname,
              fname, (unsigned)offset);
      break;
    case TYPE_LIST: {
      capnp_ptr_t element_type;
      uint16_t element_which = 0xffff;
      const char *list_suffix;
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
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/getp ptr %u))\n", sname, fname,
              (unsigned)offset);
      break;
    default:
      break;
    }
  }
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

  uint32_t nn = capnp_list_len(nodes);
  for (uint32_t i = 0; i < nn; i++) {
    capnp_ptr_t node;
    if (capnp_list_getp(nodes, i, &node) != CAPNP_OK)
      continue;
    uint64_t scope = capnp_get_u64(&node, 16, 0); /* scopeId bits[128,192) */
    /* Emit nodes scoped to this file (scopeId == file_id) or nested. */
    uint16_t which = capnp_get_u16(&node, 12, 0xffff);
    if (which == NODE_FILE)
      continue;
    /* Heuristic: emit all structs/enums whose displayName mentions the stem
     * or whose scopeId matches file or a nested id we do not track yet —
     * for v1 emit every struct/enum in the request (small schemas). */
    (void)scope;
    if (which == NODE_STRUCT)
      emit_struct(out, &node, nodes);
    else if (which == NODE_ENUM)
      emit_enum(out, &node);
    else if (which == NODE_IFACE)
      emit_interface(out, &node, nodes);
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
  capnp_message_t m;
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
