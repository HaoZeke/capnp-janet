/*
 * Janet native module "capnp": Cap'n message view + builder for embeds/packs.
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#include <janet.h>
#include <string.h>

typedef struct {
  capnp_message_t msg;
  JanetBuffer *view_buf;
} capnp_msg_wrap;

typedef struct {
  capnp_ptr_t ptr;
  Janet message_abs;
} capnp_ptr_wrap;

static int msg_gc(void *p, size_t len) {
  (void)len;
  capnp_msg_wrap *w = (capnp_msg_wrap *)p;
  capnp_message_free(&w->msg);
  w->view_buf = NULL;
  return 0;
}

static int msg_gcmark(void *p, size_t len) {
  (void)len;
  capnp_msg_wrap *w = (capnp_msg_wrap *)p;
  if (w->view_buf)
    janet_mark(janet_wrap_buffer(w->view_buf));
  return 0;
}

static int ptr_gcmark(void *p, size_t len) {
  (void)len;
  capnp_ptr_wrap *w = (capnp_ptr_wrap *)p;
  janet_mark(w->message_abs);
  return 0;
}

static const JanetAbstractType msg_type = {
    .name = "capnp/message",
    .gc = msg_gc,
    .gcmark = msg_gcmark,
};

static const JanetAbstractType ptr_type = {
    .name = "capnp/ptr",
    .gcmark = ptr_gcmark,
};

static Janet make_ptr(Janet msg_abs, const capnp_ptr_t *p) {
  capnp_ptr_wrap *w = janet_abstract(&ptr_type, sizeof(capnp_ptr_wrap));
  w->ptr = *p;
  w->message_abs = msg_abs;
  return janet_wrap_abstract(w);
}

static capnp_msg_wrap *get_msg(Janet *argv, int32_t n) {
  return (capnp_msg_wrap *)janet_getabstract(argv, n, &msg_type);
}

static capnp_ptr_wrap *get_ptr(Janet *argv, int32_t n) {
  return (capnp_ptr_wrap *)janet_getabstract(argv, n, &ptr_type);
}

static Janet cfun_message_from_buffer(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  JanetBuffer *buf = janet_getbuffer(argv, 0);
  capnp_msg_wrap *w = janet_abstract(&msg_type, sizeof(capnp_msg_wrap));
  memset(w, 0, sizeof(*w));
  if (capnp_message_from_flat(&w->msg, buf->data, (size_t)buf->count) !=
      CAPNP_OK)
    janet_panicf("capnp/message-from-buffer: framing error");
  w->view_buf = NULL;
  return janet_wrap_abstract(w);
}

static Janet cfun_message_view_buffer(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  JanetBuffer *buf = janet_getbuffer(argv, 0);
  capnp_msg_wrap *w = janet_abstract(&msg_type, sizeof(capnp_msg_wrap));
  memset(w, 0, sizeof(*w));
  if (capnp_message_view_flat(&w->msg, buf->data, (size_t)buf->count) !=
      CAPNP_OK)
    janet_panicf("capnp/message-view-buffer: framing error");
  w->view_buf = buf;
  return janet_wrap_abstract(w);
}

static Janet cfun_root(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  Janet msg_j = argv[0];
  capnp_msg_wrap *w = get_msg(argv, 0);
  capnp_ptr_t root;
  if (capnp_root(&w->msg, &root) != CAPNP_OK)
    janet_panicf("capnp/root: error");
  return make_ptr(msg_j, &root);
}

static Janet cfun_kind(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  const char *k = "null";
  switch (p->ptr.kind) {
  case CAPNP_PK_STRUCT:
    k = "struct";
    break;
  case CAPNP_PK_LIST:
    k = "list";
    break;
  case CAPNP_PK_CAP:
    k = "cap";
    break;
  default:
    break;
  }
  return janet_ckeywordv(k);
}

static Janet cfun_getp(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t idx = janet_getinteger(argv, 1);
  if (idx < 0)
    janet_panic("capnp/getp: negative index");
  capnp_ptr_t out;
  if (capnp_getp(&p->ptr, (uint16_t)idx, &out) != CAPNP_OK)
    janet_panicf("capnp/getp: error");
  return make_ptr(p->message_abs, &out);
}

static Janet cfun_get_u16(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  uint16_t dflt = argc > 2 ? (uint16_t)janet_getinteger(argv, 2) : 0;
  return janet_wrap_number(
      (double)capnp_get_u16(&p->ptr, (uint32_t)off, dflt));
}

static Janet cfun_get_u32(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  uint32_t dflt = argc > 2 ? (uint32_t)janet_getinteger(argv, 2) : 0;
  return janet_wrap_number(
      (double)capnp_get_u32(&p->ptr, (uint32_t)off, dflt));
}

static Janet cfun_get_u64(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  /* Number path: mantissa-safe integers (same as build-message :u64). */
  uint64_t dflt = 0;
  if (argc > 2) {
    if (!janet_checktype(argv[2], JANET_NUMBER))
      janet_panic("capnp/get-u64: default must be number");
    dflt = (uint64_t)janet_unwrap_number(argv[2]);
  }
  return janet_wrap_number(
      (double)capnp_get_u64(&p->ptr, (uint32_t)off, dflt));
}

static Janet cfun_get_f64(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  double dflt = argc > 2 ? janet_getnumber(argv, 2) : 0.0;
  return janet_wrap_number(capnp_get_f64(&p->ptr, (uint32_t)off, dflt));
}

static Janet cfun_get_bool(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t bit = janet_getinteger(argv, 1);
  int dflt = argc > 2 ? janet_getboolean(argv, 2) : 0;
  return janet_wrap_boolean(capnp_get_bool(&p->ptr, (uint32_t)bit, dflt));
}

static Janet cfun_get_text(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t idx = janet_getinteger(argv, 1);
  const char *s = NULL;
  size_t n = 0;
  if (capnp_get_text(&p->ptr, (uint16_t)idx, &s, &n) != CAPNP_OK)
    janet_panicf("capnp/get-text: error");
  return janet_stringv((const uint8_t *)s, (int32_t)n);
}

static Janet cfun_list_len(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  return janet_wrap_number((double)capnp_list_len(&p->ptr));
}

static Janet cfun_list_getp(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t idx = janet_getinteger(argv, 1);
  if (idx < 0)
    janet_panic("capnp/list-getp: negative");
  capnp_ptr_t out;
  if (capnp_list_getp(&p->ptr, (uint32_t)idx, &out) != CAPNP_OK)
    janet_panicf("capnp/list-getp: error");
  return make_ptr(p->message_abs, &out);
}

static Janet cfun_list_get_text(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t idx = janet_getinteger(argv, 1);
  if (idx < 0)
    janet_panic("capnp/list-get-text: negative");
  const char *s = NULL;
  size_t n = 0;
  if (capnp_list_get_text(&p->ptr, (uint32_t)idx, &s, &n) != CAPNP_OK)
    janet_panicf("capnp/list-get-text: error");
  return janet_stringv((const uint8_t *)s, (int32_t)n);
}

static Janet cfun_build_message(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  int32_t dwords = janet_getinteger(argv, 0);
  int32_t pwords = janet_getinteger(argv, 1);
  if (dwords < 0 || dwords > 0xffff || pwords < 0 || pwords > 0xffff)
    janet_panic("capnp/build-message: bad dwords/pwords");
  JanetArray *fields = janet_getarray(argv, 2);

  capnp_builder_t b;
  capnp_builder_init(&b);
  capnp_bptr_t root, body;
  if (capnp_builder_root(&b, &root) ||
      capnp_builder_struct(&root, (uint16_t)dwords, (uint16_t)pwords, &body)) {
    capnp_builder_free(&b);
    janet_panic("capnp/build-message: struct alloc failed");
  }

  for (int32_t i = 0; i < fields->count; i++) {
    if (!janet_checktype(fields->data[i], JANET_TUPLE))
      janet_panicf("capnp/build-message: field %d must be tuple", i);
    const Janet *tup = janet_unwrap_tuple(fields->data[i]);
    int32_t tlen = janet_tuple_length(tup);
    if (tlen < 3)
      janet_panicf("capnp/build-message: field %d needs [kind off val]", i);
    if (!janet_checktype(tup[0], JANET_KEYWORD))
      janet_panic("capnp/build-message: field kind must be keyword");
    const uint8_t *kind = janet_unwrap_keyword(tup[0]);
    if (!janet_checktype(tup[1], JANET_NUMBER))
      janet_panic("capnp/build-message: field offset must be number");
    int32_t off = janet_unwrap_integer(tup[1]);
    if (off < 0)
      janet_panic("capnp/build-message: negative offset");

    if (strcmp((const char *)kind, "u16") == 0) {
      int32_t v = janet_unwrap_integer(tup[2]);
      if (capnp_builder_set_u16(&body, (uint32_t)off, (uint16_t)v))
        janet_panic("capnp/build-message: set-u16 failed");
    } else if (strcmp((const char *)kind, "u32") == 0) {
      int32_t v = janet_unwrap_integer(tup[2]);
      if (capnp_builder_set_u32(&body, (uint32_t)off, (uint32_t)v))
        janet_panic("capnp/build-message: set-u32 failed");
    } else if (strcmp((const char *)kind, "u64") == 0) {
      /* Number (mantissa-safe ints). */
      if (!janet_checktype(tup[2], JANET_NUMBER))
        janet_panic("capnp/build-message: u64 value must be number");
      {
        double dv = janet_unwrap_number(tup[2]);
        uint64_t v = (uint64_t)dv;
        if (capnp_builder_set_u64(&body, (uint32_t)off, v))
          janet_panic("capnp/build-message: set-u64 failed");
      }
    } else if (strcmp((const char *)kind, "f64") == 0) {
      if (!janet_checktype(tup[2], JANET_NUMBER))
        janet_panic("capnp/build-message: f64 value must be number");
      {
        double v = janet_unwrap_number(tup[2]);
        if (capnp_builder_set_f64(&body, (uint32_t)off, v))
          janet_panic("capnp/build-message: set-f64 failed");
      }
    } else if (strcmp((const char *)kind, "bool") == 0) {
      int v = janet_truthy(tup[2]);
      if (capnp_builder_set_bool(&body, (uint32_t)off, v))
        janet_panic("capnp/build-message: set-bool failed");
    } else if (strcmp((const char *)kind, "text") == 0) {
      if (!janet_checktype(tup[2], JANET_STRING))
        janet_panic("capnp/build-message: text value must be string");
      const uint8_t *s = janet_unwrap_string(tup[2]);
      int32_t slen = (int32_t)janet_string_length(s);
      if (capnp_builder_set_text(&body, (uint16_t)dwords,
                                 (uint16_t)off, (const char *)s,
                                 (size_t)slen))
        janet_panic("capnp/build-message: set-text failed");
    } else {
      janet_panicf("capnp/build-message: unknown field kind %s", kind);
    }
  }

  uint8_t *flat = NULL;
  size_t flat_len = 0;
  if (capnp_builder_serialize(&b, &flat, &flat_len)) {
    capnp_builder_free(&b);
    janet_panic("capnp/build-message: serialize failed");
  }
  capnp_builder_free(&b);
  JanetBuffer *out = janet_buffer((int32_t)flat_len);
  janet_buffer_push_bytes(out, flat, (int32_t)flat_len);
  free(flat);
  return janet_wrap_buffer(out);
}

static const JanetReg capnp_cfuns[] = {
    {"message-from-buffer", cfun_message_from_buffer,
     "(capnp/message-from-buffer buf)\n\n"
     "Deserialize a stream-framed Cap'n message (copy)."},
    {"message-view-buffer", cfun_message_view_buffer,
     "(capnp/message-view-buffer buf)\n\n"
     "Zero-copy view of a stream-framed Cap'n message; buf must stay live."},
    {"root", cfun_root, "(capnp/root msg)\n\nRoot pointer of the message."},
    {"kind", cfun_kind, "(capnp/kind ptr)\n\n:null | :struct | :list | :cap"},
    {"getp", cfun_getp, "(capnp/getp struct-ptr index)\n\nPointer slot."},
    {"get-u16", cfun_get_u16,
     "(capnp/get-u16 struct-ptr byte-offset &opt default)"},
    {"get-u32", cfun_get_u32,
     "(capnp/get-u32 struct-ptr byte-offset &opt default)"},
    {"get-u64", cfun_get_u64,
     "(capnp/get-u64 struct-ptr byte-offset &opt default)"},
    {"get-f64", cfun_get_f64,
     "(capnp/get-f64 struct-ptr byte-offset &opt default)"},
    {"get-bool", cfun_get_bool,
     "(capnp/get-bool struct-ptr bit-offset &opt default)"},
    {"get-text", cfun_get_text,
     "(capnp/get-text struct-ptr ptr-index)\n\nText at pointer slot."},
    {"list-len", cfun_list_len, "(capnp/list-len list-ptr)"},
    {"list-getp", cfun_list_getp, "(capnp/list-getp list-ptr index)"},
    {"list-get-text", cfun_list_get_text,
     "(capnp/list-get-text list-ptr index)\n\nElement of List(Text)."},
    {"build-message", cfun_build_message,
     "(capnp/build-message dwords pwords fields)\n\n"
     "Build a framed root struct. fields: array of "
     "[:u16|:u32|:u64|:f64|:bool|:text off val]."},
    {NULL, NULL, NULL}};

void capnp_janet_register(JanetTable *env) {
  /* Prefix so packs call (capnp/build-message …) etc. */
  janet_cfuns_prefix(env, "capnp", capnp_cfuns);
}

void capnp_janet_env(JanetTable *env) { capnp_janet_register(env); }
