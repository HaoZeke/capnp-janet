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
#include <unistd.h>

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
  case TYPE_UINT8:
    return "u8";
  case TYPE_INT16:
  case TYPE_UINT16:
    return "u16";
  case TYPE_INT32:
  case TYPE_UINT32:
    return "u32";
  case TYPE_INT64:
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

static void emit_struct(FILE *out, const capnp_ptr_t *node) {
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

  fprintf(out, "\n# struct %s\n", sname);
  fprintf(out, "(def %s-data-words %u)\n", sname, (unsigned)dwords);
  fprintf(out, "(def %s-pointer-words %u)\n", sname, (unsigned)pwords);

  capnp_ptr_t fields;
  if (capnp_getp(node, 3, &fields) != CAPNP_OK || fields.kind != CAPNP_PK_LIST)
    return;

  uint32_t nf = capnp_list_len(&fields);
  fprintf(out, "(def %s-fields\n  @{", sname);
  for (uint32_t i = 0; i < nf; i++) {
    capnp_ptr_t f;
    if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
      continue;
    char fname[128];
    janet_ident(get_text(&f, 0), fname, sizeof(fname));
    uint16_t fwhich = capnp_get_u16(&f, 8, 0xffff);
    if (fwhich != FIELD_SLOT)
      continue; /* skip groups for v1 */
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

  /* Thin readers for common scalar/text fields */
  for (uint32_t i = 0; i < nf; i++) {
    capnp_ptr_t f;
    char fname[128];
    if (capnp_list_getp(&fields, i, &f) != CAPNP_OK)
      continue;
    janet_ident(get_text(&f, 0), fname, sizeof(fname));
    uint16_t fwhich = capnp_get_u16(&f, 8, 0xffff);
    if (fwhich != FIELD_SLOT)
      continue;
    uint32_t offset = capnp_get_u32(&f, 4, 0);
    capnp_ptr_t typ;
    uint16_t tw = 0xffff;
    if (capnp_getp(&f, 2, &typ) == CAPNP_OK && typ.kind == CAPNP_PK_STRUCT)
      tw = capnp_get_u16(&typ, 0, 0xffff);

    switch (tw) {
    case TYPE_BOOL:
      /* offset is bit index */
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-bool ptr %u))\n", sname,
              fname, (unsigned)offset);
      break;
    case TYPE_UINT16:
    case TYPE_INT16:
    case TYPE_ENUM:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-u16 ptr %u))\n", sname,
              fname, (unsigned)(offset * 2));
      break;
    case TYPE_UINT32:
    case TYPE_INT32:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-u32 ptr %u))\n", sname,
              fname, (unsigned)(offset * 4));
      break;
    case TYPE_UINT64:
    case TYPE_INT64:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-u64 ptr %u))\n", sname,
              fname, (unsigned)(offset * 8));
      break;
    case TYPE_FLOAT64:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-f64 ptr %u))\n", sname,
              fname, (unsigned)(offset * 8));
      break;
    case TYPE_TEXT:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/get-text ptr %u))\n", sname,
              fname, (unsigned)offset);
      break;
    case TYPE_DATA:
      fprintf(out,
              "(defn %s-get-%s [ptr]\n  (capnp/getp ptr %u))\n", sname, fname,
              (unsigned)offset);
      break;
    case TYPE_LIST:
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
  fprintf(out, "(def file-id 0x%llx)\n", (unsigned long long)file_id);

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
      emit_struct(out, &node);
    else if (which == NODE_ENUM)
      emit_enum(out, &node);
  }

  fclose(out);

  /* Plugin protocol: print absolute or relative path of generated file */
  {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)))
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
