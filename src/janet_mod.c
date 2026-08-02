/*
 * Janet native module "capnp": zero-copy Cap'n message view + small builder.
 *
 * Standalone: build as shared module for (import capnp).
 * Embed (policyd): call capnp_janet_register(vm) after janet_init.
 */

#include "capnp_builder.h"
#include "capnp_message.h"

#include <janet.h>
#include <string.h>

static const JanetAbstractType capnp_msg_type = {
    "capnp/message",
    NULL, /* gc */
    NULL, /* gcmark */
    NULL, /* get */
    NULL, /* put */
    NULL, /* marshal */
    NULL, /* unmarshal */
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    NULL, /* next */
    NULL, /* call */
    NULL, /* length */
    NULL, /* bytes */
};

typedef struct {
  capnp_message_t msg;
  /* Keep a Janet buffer ref alive for view path via gcmark if needed.
   * For from-buffer copy path, owned bytes live in msg.owned. */
  JanetBuffer *view_buf; /* may be NULL */
} capnp_msg_wrap;

static const JanetAbstractType capnp_ptr_type = {
    "capnp/ptr",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

typedef struct {
  capnp_ptr_t ptr;
  /* Keep parent message abstract alive. */
  Janet message_abs;
} capnp_ptr_wrap;

static void msg_gc(void *p, size_t len) {
  (void)len;
  capnp_msg_wrap *w = (capnp_msg_wrap *)p;
  capnp_message_free(&w->msg);
  w->view_buf = NULL;
}

static void msg_gcmark(void *p, size_t len) {
  (void)len;
  capnp_msg_wrap *w = (capnp_msg_wrap *)p;
  if (w->view_buf)
    janet_mark(janet_wrap_buffer(w->view_buf));
}

static void ptr_gcmark(void *p, size_t len) {
  (void)len;
  capnp_ptr_wrap *w = (capnp_ptr_wrap *)p;
  janet_mark(w->message_abs);
}

/* Patch types with gc after definition (C89-ish fixed initializers). */
static JanetAbstractType msg_type;
static JanetAbstractType ptr_type;
static int types_ready = 0;

static void ensure_types(void) {
  if (types_ready)
    return;
  msg_type = capnp_msg_type;
  msg_type.gc = msg_gc;
  msg_type.gcmark = msg_gcmark;
  ptr_type = capnp_ptr_type;
  ptr_type.gcmark = ptr_gcmark;
  types_ready = 1;
}

static Janet make_ptr(Janet msg_abs, const capnp_ptr_t *p) {
  ensure_types();
  capnp_ptr_wrap *w = janet_abstract(&ptr_type, sizeof(capnp_ptr_wrap));
  w->ptr = *p;
  w->message_abs = msg_abs;
  return janet_wrap_abstract(w);
}

static capnp_msg_wrap *get_msg(Janet x) {
  ensure_types();
  return (capnp_msg_wrap *)janet_getabstract(x, 0, &msg_type);
}

static capnp_ptr_wrap *get_ptr(Janet x) {
  ensure_types();
  return (capnp_ptr_wrap *)janet_getabstract(x, 0, &ptr_type);
}

static Janet cfun_message_from_buffer(int32_t argc, Janet *argv) {
  janet_fix(argc, 1);
  ensure_types();
  JanetBuffer *buf = janet_getbuffer(argv, 0);
  capnp_msg_wrap *w = janet_abstract(&msg_type, sizeof(capnp_msg_wrap));
  memset(w, 0, sizeof(*w));
  int rc = capnp_message_from_flat(&w->msg, buf->data, (size_t)buf->count);
  if (rc != CAPNP_OK)
    janet_panicf("capnp/message-from-buffer: framing error %d", rc);
  w->view_buf = NULL;
  return janet_wrap_abstract(w);
}

static Janet cfun_message_view_buffer(int32_t argc, Janet *argv) {
  janet_fix(argc, 1);
  ensure_types();
  JanetBuffer *buf = janet_getbuffer(argv, 0);
  capnp_msg_wrap *w = janet_abstract(&msg_type, sizeof(capnp_msg_wrap));
  memset(w, 0, sizeof(*w));
  int rc = capnp_message_view_flat(&w->msg, buf->data, (size_t)buf->count);
  if (rc != CAPNP_OK)
    janet_panicf("capnp/message-view-buffer: framing error %d", rc);
  w->view_buf = buf;
  return janet_wrap_abstract(w);
}

static Janet cfun_root(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  Janet msg_j = argv[0];
  capnp_msg_wrap *w = get_msg(msg_j);
  capnp_ptr_t root;
  int rc = capnp_root(&w->msg, &root);
  if (rc != CAPNP_OK)
    janet_panicf("capnp/root: error %d", rc);
  return make_ptr(msg_j, &root);
}

static Janet cfun_kind(int32_t argc, Janet *argv) {
  janet_fix(argc, 1);
  capnp_ptr_wrap *p = get_ptr(argv[0]);
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
  janet_fix(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv[0]);
  int32_t idx = janet_getinteger(argv, 1);
  if (idx < 0)
    janet_panic("capnp/getp: negative index");
  capnp_ptr_t out;
  int rc = capnp_getp(&p->ptr, (uint16_t)idx, &out);
  if (rc != CAPNP_OK)
    janet_panicf("capnp/getp: error %d", rc);
  return make_ptr(p->message_abs, &out);
}

static Janet cfun_get_u32(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv[0]);
  int32_t off = janet_getinteger(argv, 1);
  uint32_t dflt = argc > 2 ? (uint32_t)janet_getinteger(argv, 2) : 0;
  return janet_wrap_number(
      (double)capnp_get_u32(&p->ptr, (uint32_t)off, dflt));
}

static Janet cfun_get_bool(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv[0]);
  int32_t bit = janet_getinteger(argv, 1);
  int dflt = argc > 2 ? janet_getboolean(argv, 2) : 0;
  return janet_wrap_boolean(capnp_get_bool(&p->ptr, (uint32_t)bit, dflt));
}

static Janet cfun_get_text(int32_t argc, Janet *argv) {
  janet_fix(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv[0]);
  int32_t idx = janet_getinteger(argv, 1);
  const char *s = NULL;
  size_t n = 0;
  int rc = capnp_get_text(&p->ptr, (uint16_t)idx, &s, &n);
  if (rc != CAPNP_OK)
    janet_panicf("capnp/get-text: error %d", rc);
  return janet_stringv((const uint8_t *)s, (int32_t)n);
}

static Janet cfun_list_len(int32_t argc, Janet *argv) {
  janet_fix(argc, 1);
  capnp_ptr_wrap *p = get_ptr(argv[0]);
  return janet_wrap_number((double)capnp_list_len(&p->ptr));
}

static Janet cfun_list_getp(int32_t argc, Janet *argv) {
  janet_fix(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv[0]);
  int32_t idx = janet_getinteger(argv, 1);
  if (idx < 0)
    janet_panic("capnp/list-getp: negative");
  capnp_ptr_t out;
  int rc = capnp_list_getp(&p->ptr, (uint32_t)idx, &out);
  if (rc != CAPNP_OK)
    janet_panicf("capnp/list-getp: error %d", rc);
  return make_ptr(p->message_abs, &out);
}

static Janet cfun_list_get_text(int32_t argc, Janet *argv) {
  janet_fix(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv[0]);
  int32_t idx = janet_getinteger(argv, 1);
  if (idx < 0)
    janet_panic("capnp/list-get-text: negative");
  const char *s = NULL;
  size_t n = 0;
  int rc = capnp_list_get_text(&p->ptr, (uint32_t)idx, &s, &n);
  if (rc != CAPNP_OK)
    janet_panicf("capnp/list-get-text: error %d", rc);
  return janet_stringv((const uint8_t *)s, (int32_t)n);
}

/* ---- builder helpers exposed for tests / packs writing PolicyDecision ---- */

static Janet cfun_build_demo(int32_t argc, Janet *argv) {
  /*
   * (capnp/build-demo value name &opt items)
   * Builds root { value @0 :UInt32; name @0 :Text; items @1 :List(Text) }
   * returns buffer of framed message.
   */
  janet_arity(argc, 2, 3);
  uint32_t value = (uint32_t)janet_getinteger(argv, 0);
  const uint8_t *name = janet_getstring(argv, 1);
  int32_t name_len = (int32_t)janet_string_length(name);

  const char **items = NULL;
  uint32_t nitems = 0;
  JanetArray *arr = NULL;
  if (argc > 2) {
    arr = janet_getarray(argv, 2);
    nitems = (uint32_t)arr->count;
    items = janet_smalloc(sizeof(char *) * (nitems ? nitems : 1));
    for (uint32_t i = 0; i < nitems; i++) {
      const uint8_t *s = janet_getstring(arr->data, (int32_t)i);
      items[i] = (const char *)s;
    }
  }

  capnp_builder_t b;
  capnp_builder_init(&b);
  capnp_bptr_t root, body;
  if (capnp_builder_root(&b, &root) ||
      capnp_builder_struct(&root, 1, 2, &body) ||
      capnp_builder_set_u32(&b, body.word, 0, value) ||
      capnp_builder_set_text(&b, body.word, 1, 0, (const char *)name,
                             (size_t)name_len) ||
      (items && capnp_builder_set_list_text(&b, body.word, 1, 1, items,
                                            nitems)) ||
      (!items && capnp_builder_set_list_text(&b, body.word, 1, 1, NULL, 0))) {
    capnp_builder_free(&b);
    if (items)
      janet_sfree(items);
    janet_panic("capnp/build-demo: builder failed");
  }

  uint8_t *flat = NULL;
  size_t flat_len = 0;
  if (capnp_builder_serialize(&b, &flat, &flat_len)) {
    capnp_builder_free(&b);
    if (items)
      janet_sfree(items);
    janet_panic("capnp/build-demo: serialize failed");
  }
  capnp_builder_free(&b);
  if (items)
    janet_sfree(items);

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
    {"get-u32", cfun_get_u32,
     "(capnp/get-u32 struct-ptr byte-offset &opt default)"},
    {"get-bool", cfun_get_bool,
     "(capnp/get-bool struct-ptr bit-offset &opt default)"},
    {"get-text", cfun_get_text,
     "(capnp/get-text struct-ptr ptr-index)\n\nText at pointer slot."},
    {"list-len", cfun_list_len, "(capnp/list-len list-ptr)"},
    {"list-getp", cfun_list_getp, "(capnp/list-getp list-ptr index)"},
    {"list-get-text", cfun_list_get_text,
     "(capnp/list-get-text list-ptr index)\n\nElement of List(Text)."},
    {"build-demo", cfun_build_demo,
     "(capnp/build-demo value name &opt items)\n\n"
     "Build framed demo message {value:UInt32, name:Text, items:List(Text)}."},
    {NULL, NULL, NULL}};

void capnp_janet_register(JanetTable *env) {
  ensure_types();
  janet_cfuns(env, "capnp", capnp_cfuns);
}

#ifdef JANET_MODULE_ENTRY
JANET_MODULE_ENTRY(JanetTable *env) { capnp_janet_register(env); }
#endif

/* Also export for static link from policyd without module entry macro. */
void capnp_janet_env(JanetTable *env) { capnp_janet_register(env); }
