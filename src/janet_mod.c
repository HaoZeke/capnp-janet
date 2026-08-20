/*
 * Janet native module "capnp": Cap'n message view + builder for embeds/packs.
 */
#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#include <janet.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  capnp_message_t msg;
  JanetBuffer *view_buf;
} capnp_msg_wrap;

typedef struct {
  capnp_ptr_t ptr;
  Janet message_abs;
} capnp_ptr_wrap;

typedef struct {
  capnp_builder_t builder;
  int root_initialized;
} capnp_builder_wrap;

typedef struct {
  capnp_bptr_t body;
  uint16_t dwords;
  uint16_t pwords;
  Janet builder_abs;
} capnp_builder_ptr_wrap;

typedef struct {
  capnp_bptr_t first;
  uint16_t elem_dwords;
  uint16_t elem_pwords;
  uint32_t count;
  Janet builder_abs;
} capnp_builder_list_wrap;

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

static int builder_gc(void *p, size_t len) {
  (void)len;
  capnp_builder_wrap *w = (capnp_builder_wrap *)p;
  capnp_builder_free(&w->builder);
  return 0;
}

static int builder_ptr_gcmark(void *p, size_t len) {
  (void)len;
  capnp_builder_ptr_wrap *w = (capnp_builder_ptr_wrap *)p;
  janet_mark(w->builder_abs);
  return 0;
}

static int builder_list_gcmark(void *p, size_t len) {
  (void)len;
  capnp_builder_list_wrap *w = (capnp_builder_list_wrap *)p;
  janet_mark(w->builder_abs);
  return 0;
}

static void builder_tostring(void *p, JanetBuffer *buffer) {
  capnp_builder_wrap *w = (capnp_builder_wrap *)p;
  char tmp[64];
  snprintf(tmp, sizeof tmp, "arena %u segment%s",
           (unsigned)capnp_builder_nsegs(&w->builder),
           capnp_builder_nsegs(&w->builder) == 1 ? "" : "s");
  janet_buffer_push_cstring(buffer, tmp);
}

static void builder_ptr_tostring(void *p, JanetBuffer *buffer) {
  capnp_builder_ptr_wrap *w = (capnp_builder_ptr_wrap *)p;
  char tmp[64];
  snprintf(tmp, sizeof tmp, "struct %u data/%u pointer words",
           (unsigned)w->dwords, (unsigned)w->pwords);
  janet_buffer_push_cstring(buffer, tmp);
}

static void builder_list_tostring(void *p, JanetBuffer *buffer) {
  capnp_builder_list_wrap *w = (capnp_builder_list_wrap *)p;
  char tmp[48];
  snprintf(tmp, sizeof tmp, "struct-list %u", (unsigned)w->count);
  janet_buffer_push_cstring(buffer, tmp);
}

/* Protocol hooks, so a message and a pointer behave like built-in data
 * structures: (:root msg), (length people), (in people 0) and every
 * iteration form that rests on next. Defined below the CFUNs they
 * dispatch to. */
static int msg_get(void *p, Janet key, Janet *out);
static void msg_tostring(void *p, JanetBuffer *buffer);
static int ptr_get(void *p, Janet key, Janet *out);
static Janet ptr_next(void *p, Janet key);
static size_t ptr_length(void *p, size_t len);
static void ptr_tostring(void *p, JanetBuffer *buffer);

static const JanetAbstractType msg_type = {
    .name = "capnp/message",
    .gc = msg_gc,
    .gcmark = msg_gcmark,
    .get = msg_get,
    .tostring = msg_tostring,
};

static const JanetAbstractType ptr_type = {
    .name = "capnp/ptr",
    .gcmark = ptr_gcmark,
    .get = ptr_get,
    .tostring = ptr_tostring,
    .next = ptr_next,
    .length = ptr_length,
};

static const JanetAbstractType builder_type = {
    .name = "capnp/builder",
    .gc = builder_gc,
    .tostring = builder_tostring,
};

static const JanetAbstractType builder_ptr_type = {
    .name = "capnp/builder-ptr",
    .gcmark = builder_ptr_gcmark,
    .tostring = builder_ptr_tostring,
};

static const JanetAbstractType builder_list_type = {
    .name = "capnp/builder-list",
    .gcmark = builder_list_gcmark,
    .tostring = builder_list_tostring,
};

static const char *ptr_kind_name(const capnp_ptr_t *p) {
  switch (p->kind) {
  case CAPNP_PK_STRUCT:
    return "struct";
  case CAPNP_PK_LIST:
    return "list";
  case CAPNP_PK_CAP:
    return "cap";
  default:
    return "null";
  }
}

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

static capnp_builder_wrap *get_builder(Janet *argv, int32_t n) {
  return (capnp_builder_wrap *)janet_getabstract(argv, n, &builder_type);
}

static capnp_builder_ptr_wrap *get_builder_ptr(Janet *argv, int32_t n) {
  return (capnp_builder_ptr_wrap *)janet_getabstract(argv, n,
                                                      &builder_ptr_type);
}

static capnp_builder_list_wrap *get_builder_list(Janet *argv, int32_t n) {
  return (capnp_builder_list_wrap *)janet_getabstract(argv, n,
                                                       &builder_list_type);
}

static Janet make_builder_ptr(Janet builder_abs, const capnp_bptr_t *body,
                              uint16_t dwords, uint16_t pwords) {
  capnp_builder_ptr_wrap *w =
      janet_abstract(&builder_ptr_type, sizeof(capnp_builder_ptr_wrap));
  w->body = *body;
  w->dwords = dwords;
  w->pwords = pwords;
  w->builder_abs = builder_abs;
  return janet_wrap_abstract(w);
}

static Janet make_builder_list(Janet builder_abs, const capnp_bptr_t *first,
                               uint32_t count, uint16_t elem_dwords,
                               uint16_t elem_pwords) {
  capnp_builder_list_wrap *w =
      janet_abstract(&builder_list_type, sizeof(capnp_builder_list_wrap));
  w->first = *first;
  w->count = count;
  w->elem_dwords = elem_dwords;
  w->elem_pwords = elem_pwords;
  w->builder_abs = builder_abs;
  return janet_wrap_abstract(w);
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
  return janet_ckeywordv(ptr_kind_name(&p->ptr));
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

static Janet cfun_get_u8(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  uint8_t dflt = argc > 2 ? (uint8_t)janet_getinteger(argv, 2) : 0;
  uint8_t raw = capnp_get_u8(&p->ptr, (uint32_t)off, 0);
  return janet_wrap_number((double)(raw ^ dflt));
}

static Janet cfun_get_i8(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  int8_t dflt = argc > 2 ? (int8_t)janet_getinteger(argv, 2) : 0;
  uint8_t raw = capnp_get_u8(&p->ptr, (uint32_t)off, 0);
  return janet_wrap_number((double)(int8_t)(raw ^ (uint8_t)dflt));
}

static Janet cfun_get_u16(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  uint16_t dflt = argc > 2 ? (uint16_t)janet_getinteger(argv, 2) : 0;
  uint16_t raw = capnp_get_u16(&p->ptr, (uint32_t)off, 0);
  return janet_wrap_number((double)(raw ^ dflt));
}

static Janet cfun_get_i16(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  int16_t dflt = argc > 2 ? (int16_t)janet_getinteger(argv, 2) : 0;
  uint16_t raw = capnp_get_u16(&p->ptr, (uint32_t)off, 0);
  return janet_wrap_number((double)(int16_t)(raw ^ (uint16_t)dflt));
}

static Janet cfun_get_u32(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  uint32_t dflt = argc > 2 ? (uint32_t)janet_getinteger(argv, 2) : 0;
  uint32_t raw = capnp_get_u32(&p->ptr, (uint32_t)off, 0);
  return janet_wrap_number((double)(raw ^ dflt));
}

static Janet cfun_get_i32(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  int32_t dflt = argc > 2 ? janet_getinteger(argv, 2) : 0;
  uint32_t raw = capnp_get_u32(&p->ptr, (uint32_t)off, 0);
  return janet_wrap_number((double)(int32_t)(raw ^ (uint32_t)dflt));
}

static Janet cfun_get_u64(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  /* Number path: mantissa-safe integers (same as build-message :u64). */
  uint64_t dflt = argc > 2 ? (uint64_t)janet_getnumber(argv, 2) : 0;
  uint64_t raw = capnp_get_u64(&p->ptr, (uint32_t)off, 0);
  return janet_wrap_number((double)(raw ^ dflt));
}

static Janet cfun_get_i64(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  int64_t dflt = argc > 2 ? (int64_t)janet_getnumber(argv, 2) : 0;
  uint64_t bits = capnp_get_u64(&p->ptr, (uint32_t)off, 0) ^ (uint64_t)dflt;
  int64_t value;
  memcpy(&value, &bits, sizeof(value));
  return janet_wrap_number((double)value);
}

static Janet cfun_get_f32(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  float dflt = argc > 2 ? (float)janet_getnumber(argv, 2) : 0.0f;
  uint32_t dflt_bits;
  uint32_t bits;
  float value;
  memcpy(&dflt_bits, &dflt, sizeof(dflt_bits));
  bits = capnp_get_u32(&p->ptr, (uint32_t)off, 0) ^ dflt_bits;
  memcpy(&value, &bits, sizeof(value));
  return janet_wrap_number((double)value);
}

static Janet cfun_get_f64(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t off = janet_getinteger(argv, 1);
  double dflt = argc > 2 ? janet_getnumber(argv, 2) : 0.0;
  uint64_t dflt_bits;
  uint64_t bits;
  double value;
  memcpy(&dflt_bits, &dflt, sizeof(dflt_bits));
  bits = capnp_get_u64(&p->ptr, (uint32_t)off, 0) ^ dflt_bits;
  memcpy(&value, &bits, sizeof(value));
  return janet_wrap_number(value);
}

static Janet cfun_get_bool(int32_t argc, Janet *argv) {
  janet_arity(argc, 2, 3);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t bit = janet_getinteger(argv, 1);
  int dflt = argc > 2 ? janet_getboolean(argv, 2) : 0;
  return janet_wrap_boolean(capnp_get_bool(&p->ptr, (uint32_t)bit, 0) != dflt);
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

static Janet cfun_get_data(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  capnp_ptr_wrap *p = get_ptr(argv, 0);
  int32_t idx = janet_getinteger(argv, 1);
  const uint8_t *data = NULL;
  size_t len = 0;
  if (idx < 0 || idx > UINT16_MAX)
    janet_panic("capnp/get-data: pointer index out of range");
  if (capnp_get_data(&p->ptr, (uint16_t)idx, &data, &len) != CAPNP_OK)
    janet_panicf("capnp/get-data: error");
  if (len > INT32_MAX)
    janet_panic("capnp/get-data: value exceeds Janet's string limit");
  return janet_stringv(data, (int32_t)len);
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

static uint16_t get_word_count(const Janet *argv, int32_t n,
                               const char *where) {
  int32_t value = janet_getinteger(argv, n);
  if (value < 0 || value > UINT16_MAX)
    janet_panicf("%s: word count out of range", where);
  return (uint16_t)value;
}

static void require_data_range(const capnp_builder_ptr_wrap *p,
                               uint32_t byte_offset, size_t width,
                               const char *where) {
  size_t data_bytes = (size_t)p->dwords * CAPNP_WORD_BYTES;
  if ((size_t)byte_offset > data_bytes || width > data_bytes - byte_offset)
    janet_panicf("%s: data offset exceeds the struct section", where);
}

static void require_bool_range(const capnp_builder_ptr_wrap *p,
                               uint32_t bit_offset, const char *where) {
  size_t data_bits = (size_t)p->dwords * CAPNP_WORD_BYTES * 8;
  if ((size_t)bit_offset >= data_bits)
    janet_panicf("%s: bit offset exceeds the struct section", where);
}

static uint16_t get_pointer_index(const capnp_builder_ptr_wrap *p,
                                  const Janet *argv, int32_t n,
                                  const char *where) {
  int32_t index = janet_getinteger(argv, n);
  if (index < 0 || index >= p->pwords)
    janet_panicf("%s: pointer index exceeds the struct section", where);
  return (uint16_t)index;
}

static void check_builder_result(int rc, const char *where) {
  if (rc != CAPNP_OK)
    janet_panicf("%s: builder error %d", where, rc);
}

static Janet cfun_new_builder(int32_t argc, Janet *argv) {
  janet_arity(argc, 0, 1);
  capnp_builder_wrap *w =
      janet_abstract(&builder_type, sizeof(capnp_builder_wrap));
  memset(w, 0, sizeof(*w));
  if (argc == 0) {
    capnp_builder_init(&w->builder);
  } else {
    size_t first_words = janet_getsize(argv, 0);
    if (first_words == 0 || first_words > CAPNP_BUILDER_MAX_SEGMENT_WORDS)
      janet_panic("capnp/new-builder: segment word count out of range");
    capnp_builder_init_sized(&w->builder, first_words);
  }
  if (capnp_builder_nsegs(&w->builder) == 0)
    janet_panic("capnp/new-builder: arena allocation failed");
  return janet_wrap_abstract(w);
}

static Janet cfun_init_root(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  capnp_builder_wrap *w = get_builder(argv, 0);
  uint16_t dwords = get_word_count(argv, 1, "capnp/init-root");
  uint16_t pwords = get_word_count(argv, 2, "capnp/init-root");
  capnp_bptr_t root;
  capnp_bptr_t body;
  if (w->root_initialized)
    janet_panic("capnp/init-root: root is already initialized");
  check_builder_result(capnp_builder_root(&w->builder, &root),
                       "capnp/init-root");
  w->root_initialized = 1;
  check_builder_result(capnp_builder_struct(&root, dwords, pwords, &body),
                       "capnp/init-root");
  return make_builder_ptr(argv[0], &body, dwords, pwords);
}

static Janet cfun_init_struct(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 4);
  capnp_builder_ptr_wrap *parent = get_builder_ptr(argv, 0);
  uint16_t index =
      get_pointer_index(parent, argv, 1, "capnp/init-struct");
  uint16_t dwords = get_word_count(argv, 2, "capnp/init-struct");
  uint16_t pwords = get_word_count(argv, 3, "capnp/init-struct");
  capnp_bptr_t slot;
  capnp_bptr_t body;
  check_builder_result(
      capnp_builder_slot(&parent->body, parent->dwords, index, &slot),
      "capnp/init-struct");
  check_builder_result(capnp_builder_struct(&slot, dwords, pwords, &body),
                       "capnp/init-struct");
  return make_builder_ptr(parent->builder_abs, &body, dwords, pwords);
}

static Janet cfun_init_struct_list(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 5);
  capnp_builder_ptr_wrap *parent = get_builder_ptr(argv, 0);
  uint16_t index =
      get_pointer_index(parent, argv, 1, "capnp/init-struct-list");
  uint32_t count = janet_getuinteger(argv, 2);
  uint16_t dwords = get_word_count(argv, 3, "capnp/init-struct-list");
  uint16_t pwords = get_word_count(argv, 4, "capnp/init-struct-list");
  capnp_bptr_t first;
  check_builder_result(capnp_builder_set_list_struct(
                           &parent->body, parent->dwords, index, count, dwords,
                           pwords, &first),
                       "capnp/init-struct-list");
  return make_builder_list(parent->builder_abs, &first, count, dwords, pwords);
}

static Janet cfun_struct_list_at(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  capnp_builder_list_wrap *list = get_builder_list(argv, 0);
  uint32_t index = janet_getuinteger(argv, 1);
  size_t step = (size_t)list->elem_dwords + list->elem_pwords;
  capnp_bptr_t body;
  if (index >= list->count)
    janet_panic("capnp/struct-list-at: index out of range");
  body = capnp_bptr_add(list->first, (size_t)index * step);
  return make_builder_ptr(list->builder_abs, &body, list->elem_dwords,
                          list->elem_pwords);
}

static uint8_t get_u8_arg(const Janet *argv, int32_t n, const char *where) {
  int32_t value = janet_getinteger(argv, n);
  if (value < 0 || value > UINT8_MAX)
    janet_panicf("%s: UInt8 value out of range", where);
  return (uint8_t)value;
}

static int8_t get_i8_arg(const Janet *argv, int32_t n, const char *where) {
  int32_t value = janet_getinteger(argv, n);
  if (value < INT8_MIN || value > INT8_MAX)
    janet_panicf("%s: Int8 value out of range", where);
  return (int8_t)value;
}

static Janet cfun_set_u8(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  uint8_t value = get_u8_arg(argv, 2, "capnp/set-u8");
  uint8_t dflt = argc > 3 ? get_u8_arg(argv, 3, "capnp/set-u8") : 0;
  require_data_range(p, offset, sizeof(uint8_t), "capnp/set-u8");
  check_builder_result(capnp_builder_set_u8(&p->body, offset, value ^ dflt),
                       "capnp/set-u8");
  return argv[0];
}

static Janet cfun_set_i8(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  int8_t value = get_i8_arg(argv, 2, "capnp/set-i8");
  int8_t dflt = argc > 3 ? get_i8_arg(argv, 3, "capnp/set-i8") : 0;
  require_data_range(p, offset, sizeof(uint8_t), "capnp/set-i8");
  check_builder_result(capnp_builder_set_u8(
                           &p->body, offset,
                           (uint8_t)value ^ (uint8_t)dflt),
                       "capnp/set-i8");
  return argv[0];
}

static Janet cfun_set_u16(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  uint16_t value = janet_getuinteger16(argv, 2);
  uint16_t dflt = argc > 3 ? janet_getuinteger16(argv, 3) : 0;
  require_data_range(p, offset, sizeof(uint16_t), "capnp/set-u16");
  check_builder_result(capnp_builder_set_u16(&p->body, offset, value ^ dflt),
                       "capnp/set-u16");
  return argv[0];
}

static Janet cfun_set_i16(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  int16_t value = janet_getinteger16(argv, 2);
  int16_t dflt = argc > 3 ? janet_getinteger16(argv, 3) : 0;
  require_data_range(p, offset, sizeof(uint16_t), "capnp/set-i16");
  check_builder_result(capnp_builder_set_u16(
                           &p->body, offset,
                           (uint16_t)value ^ (uint16_t)dflt),
                       "capnp/set-i16");
  return argv[0];
}

static Janet cfun_set_u32(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  uint32_t value = janet_getuinteger(argv, 2);
  uint32_t dflt = argc > 3 ? janet_getuinteger(argv, 3) : 0;
  require_data_range(p, offset, sizeof(uint32_t), "capnp/set-u32");
  check_builder_result(capnp_builder_set_u32(&p->body, offset, value ^ dflt),
                       "capnp/set-u32");
  return argv[0];
}

static Janet cfun_set_i32(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  int32_t value = janet_getinteger(argv, 2);
  int32_t dflt = argc > 3 ? janet_getinteger(argv, 3) : 0;
  require_data_range(p, offset, sizeof(uint32_t), "capnp/set-i32");
  check_builder_result(capnp_builder_set_u32(
                           &p->body, offset,
                           (uint32_t)value ^ (uint32_t)dflt),
                       "capnp/set-i32");
  return argv[0];
}

static Janet cfun_set_u64(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  uint64_t value = janet_getuinteger64(argv, 2);
  uint64_t dflt = argc > 3 ? janet_getuinteger64(argv, 3) : 0;
  require_data_range(p, offset, sizeof(uint64_t), "capnp/set-u64");
  check_builder_result(capnp_builder_set_u64(&p->body, offset, value ^ dflt),
                       "capnp/set-u64");
  return argv[0];
}

static Janet cfun_set_i64(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  int64_t value = janet_getinteger64(argv, 2);
  int64_t dflt = argc > 3 ? janet_getinteger64(argv, 3) : 0;
  require_data_range(p, offset, sizeof(uint64_t), "capnp/set-i64");
  check_builder_result(capnp_builder_set_u64(
                           &p->body, offset,
                           (uint64_t)value ^ (uint64_t)dflt),
                       "capnp/set-i64");
  return argv[0];
}

static Janet cfun_set_f32(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  float value = (float)janet_getnumber(argv, 2);
  float dflt = argc > 3 ? (float)janet_getnumber(argv, 3) : 0.0f;
  uint32_t value_bits;
  uint32_t dflt_bits;
  require_data_range(p, offset, sizeof(uint32_t), "capnp/set-f32");
  memcpy(&value_bits, &value, sizeof(value_bits));
  memcpy(&dflt_bits, &dflt, sizeof(dflt_bits));
  check_builder_result(
      capnp_builder_set_u32(&p->body, offset, value_bits ^ dflt_bits),
      "capnp/set-f32");
  return argv[0];
}

static Janet cfun_set_f64(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  double value = janet_getnumber(argv, 2);
  double dflt = argc > 3 ? janet_getnumber(argv, 3) : 0.0;
  uint64_t value_bits;
  uint64_t dflt_bits;
  require_data_range(p, offset, sizeof(uint64_t), "capnp/set-f64");
  memcpy(&value_bits, &value, sizeof(value_bits));
  memcpy(&dflt_bits, &dflt, sizeof(dflt_bits));
  check_builder_result(
      capnp_builder_set_u64(&p->body, offset, value_bits ^ dflt_bits),
      "capnp/set-f64");
  return argv[0];
}

static Janet cfun_set_bool(int32_t argc, Janet *argv) {
  janet_arity(argc, 3, 4);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint32_t offset = janet_getuinteger(argv, 1);
  int value = janet_getboolean(argv, 2);
  int dflt = argc > 3 ? janet_getboolean(argv, 3) : 0;
  require_bool_range(p, offset, "capnp/set-bool");
  check_builder_result(capnp_builder_set_bool(&p->body, offset, value != dflt),
                       "capnp/set-bool");
  return argv[0];
}

static Janet cfun_set_text(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint16_t index = get_pointer_index(p, argv, 1, "capnp/set-text");
  JanetByteView value = janet_getbytes(argv, 2);
  check_builder_result(capnp_builder_set_text(
                           &p->body, p->dwords, index,
                           (const char *)value.bytes, (size_t)value.len),
                       "capnp/set-text");
  return argv[0];
}

static Janet cfun_set_data(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint16_t index = get_pointer_index(p, argv, 1, "capnp/set-data");
  JanetByteView value = janet_getbytes(argv, 2);
  check_builder_result(capnp_builder_set_data(
                           &p->body, p->dwords, index, value.bytes,
                           (size_t)value.len),
                       "capnp/set-data");
  return argv[0];
}

static Janet cfun_clear_pointer(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  capnp_builder_ptr_wrap *p = get_builder_ptr(argv, 0);
  uint16_t index = get_pointer_index(p, argv, 1, "capnp/clear-pointer");
  check_builder_result(
      capnp_builder_clear_ptr(&p->body, p->dwords, index),
      "capnp/clear-pointer");
  return argv[0];
}

static Janet cfun_finish_builder(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  capnp_builder_wrap *w = get_builder(argv, 0);
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  JanetBuffer *out;
  if (!w->root_initialized)
    janet_panic("capnp/finish-builder: root is not initialized");
  check_builder_result(capnp_builder_serialize(&w->builder, &flat, &flat_len),
                       "capnp/finish-builder");
  if (flat_len > INT32_MAX) {
    free(flat);
    janet_panic("capnp/finish-builder: message exceeds Janet's buffer limit");
  }
  out = janet_buffer((int32_t)flat_len);
  janet_buffer_push_bytes(out, flat, (int32_t)flat_len);
  free(flat);
  return janet_wrap_buffer(out);
}

static Janet cfun_build_message(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  int32_t dwords = janet_getinteger(argv, 0);
  int32_t pwords = janet_getinteger(argv, 1);
  if (dwords < 0 || dwords > 0xffff || pwords < 0 || pwords > 0xffff)
    janet_panic("capnp/build-message: bad dwords/pwords");
  JanetView fields = janet_getindexed(argv, 2);

  capnp_builder_t b;
  capnp_builder_init(&b);
  capnp_bptr_t root, body;
  if (capnp_builder_root(&b, &root) ||
      capnp_builder_struct(&root, (uint16_t)dwords, (uint16_t)pwords, &body)) {
    capnp_builder_free(&b);
    janet_panic("capnp/build-message: struct alloc failed");
  }

  for (int32_t i = 0; i < fields.len; i++) {
    const Janet *tup;
    int32_t tlen;
    if (!janet_indexed_view(fields.items[i], &tup, &tlen))
      janet_panicf("capnp/build-message: field %d must be indexed", i);
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

/* Method tables back the (:verb receiver ...) call form. The receiver is
 * already argv[0] of each CFUN, so they need no wrappers. */
static const JanetMethod msg_methods[] = {
    {"root", cfun_root},
    {NULL, NULL},
};

static const JanetMethod ptr_methods[] = {
    {"kind", cfun_kind},
    {"ptr", cfun_getp},
    {"u8", cfun_get_u8},
    {"i8", cfun_get_i8},
    {"u16", cfun_get_u16},
    {"i16", cfun_get_i16},
    {"u32", cfun_get_u32},
    {"i32", cfun_get_i32},
    {"u64", cfun_get_u64},
    {"i64", cfun_get_i64},
    {"f32", cfun_get_f32},
    {"f64", cfun_get_f64},
    {"bool", cfun_get_bool},
    {"text", cfun_get_text},
    {"text-at", cfun_list_get_text},
    {NULL, NULL},
};

static int msg_get(void *p, Janet key, Janet *out) {
  (void)p;
  if (!janet_checktype(key, JANET_KEYWORD))
    return 0;
  return janet_getmethod(janet_unwrap_keyword(key), msg_methods, out);
}

static void msg_tostring(void *p, JanetBuffer *buffer) {
  capnp_msg_wrap *w = (capnp_msg_wrap *)p;
  janet_buffer_push_cstring(buffer, w->view_buf ? "view" : "owned");
}

/* Keyword keys dispatch methods; an integer key indexes a list, which is
 * what makes (in lst i), (each x lst ...) and the rest of the sequence
 * forms work without a wrapper layer in Janet. */
static int ptr_get(void *p, Janet key, Janet *out) {
  capnp_ptr_wrap *w = (capnp_ptr_wrap *)p;
  if (janet_checktype(key, JANET_KEYWORD))
    return janet_getmethod(janet_unwrap_keyword(key), ptr_methods, out);
  if (!janet_checktype(key, JANET_NUMBER))
    return 0;
  if (w->ptr.kind != CAPNP_PK_LIST)
    janet_panicf("capnp/ptr: cannot index a %s, only a list",
                 ptr_kind_name(&w->ptr));
  int32_t i = (int32_t)janet_unwrap_number(key);
  int32_t n = (int32_t)capnp_list_len(&w->ptr);
  if (i < 0 || i >= n)
    return 0; /* out of range reads as nil, as for a Janet array */
  capnp_ptr_t el;
  if (capnp_list_getp(&w->ptr, (uint32_t)i, &el) != CAPNP_OK)
    janet_panicf("capnp/ptr: element %d unreadable", (int)i);
  *out = make_ptr(w->message_abs, &el);
  return 1;
}

static Janet ptr_next(void *p, Janet key) {
  capnp_ptr_wrap *w = (capnp_ptr_wrap *)p;
  /* A non-list has no elements to walk, so iteration exposes the method
   * table instead, matching how Janet's own abstract types behave. */
  if (w->ptr.kind != CAPNP_PK_LIST)
    return janet_nextmethod(ptr_methods, key);
  int32_t n = (int32_t)capnp_list_len(&w->ptr);
  if (janet_checktype(key, JANET_NIL))
    return n > 0 ? janet_wrap_number(0) : janet_wrap_nil();
  if (!janet_checktype(key, JANET_NUMBER))
    return janet_wrap_nil();
  int32_t i = (int32_t)janet_unwrap_number(key) + 1;
  return i < n ? janet_wrap_number(i) : janet_wrap_nil();
}

static size_t ptr_length(void *p, size_t len) {
  (void)len;
  capnp_ptr_wrap *w = (capnp_ptr_wrap *)p;
  if (w->ptr.kind != CAPNP_PK_LIST)
    janet_panicf("capnp/ptr: length needs a list, got a %s",
                 ptr_kind_name(&w->ptr));
  return (size_t)capnp_list_len(&w->ptr);
}

static void ptr_tostring(void *p, JanetBuffer *buffer) {
  capnp_ptr_wrap *w = (capnp_ptr_wrap *)p;
  if (w->ptr.kind == CAPNP_PK_LIST) {
    char tmp[48];
    snprintf(tmp, sizeof tmp, "list %u", (unsigned)capnp_list_len(&w->ptr));
    janet_buffer_push_cstring(buffer, tmp);
  } else {
    janet_buffer_push_cstring(buffer, ptr_kind_name(&w->ptr));
  }
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
    {"get-u8", cfun_get_u8,
     "(capnp/get-u8 struct-ptr byte-offset &opt schema-default)"},
    {"get-i8", cfun_get_i8,
     "(capnp/get-i8 struct-ptr byte-offset &opt schema-default)"},
    {"get-u16", cfun_get_u16,
     "(capnp/get-u16 struct-ptr byte-offset &opt schema-default)"},
    {"get-i16", cfun_get_i16,
     "(capnp/get-i16 struct-ptr byte-offset &opt schema-default)"},
    {"get-u32", cfun_get_u32,
     "(capnp/get-u32 struct-ptr byte-offset &opt schema-default)"},
    {"get-i32", cfun_get_i32,
     "(capnp/get-i32 struct-ptr byte-offset &opt schema-default)"},
    {"get-u64", cfun_get_u64,
     "(capnp/get-u64 struct-ptr byte-offset &opt schema-default)"},
    {"get-i64", cfun_get_i64,
     "(capnp/get-i64 struct-ptr byte-offset &opt schema-default)"},
    {"get-f32", cfun_get_f32,
     "(capnp/get-f32 struct-ptr byte-offset &opt schema-default)"},
    {"get-f64", cfun_get_f64,
     "(capnp/get-f64 struct-ptr byte-offset &opt schema-default)"},
    {"get-bool", cfun_get_bool,
     "(capnp/get-bool struct-ptr bit-offset &opt schema-default)"},
    {"get-text", cfun_get_text,
     "(capnp/get-text struct-ptr ptr-index)\n\nText at pointer slot."},
    {"get-data", cfun_get_data,
     "(capnp/get-data struct-ptr ptr-index)\n\nData at pointer slot."},
    {"list-len", cfun_list_len, "(capnp/list-len list-ptr)"},
    {"list-getp", cfun_list_getp, "(capnp/list-getp list-ptr index)"},
    {"list-get-text", cfun_list_get_text,
     "(capnp/list-get-text list-ptr index)\n\nElement of List(Text)."},
    {"new-builder", cfun_new_builder,
     "(capnp/new-builder &opt first-segment-words)\n\n"
     "Create a growable message arena."},
    {"init-root", cfun_init_root,
     "(capnp/init-root builder data-words pointer-words)\n\n"
     "Initialize and return the root struct body."},
    {"init-struct", cfun_init_struct,
     "(capnp/init-struct body pointer-index data-words pointer-words)\n\n"
     "Initialize and return a nested struct body."},
    {"init-struct-list", cfun_init_struct_list,
     "(capnp/init-struct-list body pointer-index count data-words "
     "pointer-words)\n\nInitialize a List(Struct) in the same arena."},
    {"struct-list-at", cfun_struct_list_at,
     "(capnp/struct-list-at list index)\n\nMutable struct-list element."},
    {"finish-builder", cfun_finish_builder,
     "(capnp/finish-builder builder)\n\nSerialize as a stream-framed buffer."},
    {"set-u8", cfun_set_u8,
     "(capnp/set-u8 body byte-offset value &opt schema-default)"},
    {"set-i8", cfun_set_i8,
     "(capnp/set-i8 body byte-offset value &opt schema-default)"},
    {"set-u16", cfun_set_u16,
     "(capnp/set-u16 body byte-offset value &opt schema-default)"},
    {"set-i16", cfun_set_i16,
     "(capnp/set-i16 body byte-offset value &opt schema-default)"},
    {"set-u32", cfun_set_u32,
     "(capnp/set-u32 body byte-offset value &opt schema-default)"},
    {"set-i32", cfun_set_i32,
     "(capnp/set-i32 body byte-offset value &opt schema-default)"},
    {"set-u64", cfun_set_u64,
     "(capnp/set-u64 body byte-offset value &opt schema-default)"},
    {"set-i64", cfun_set_i64,
     "(capnp/set-i64 body byte-offset value &opt schema-default)"},
    {"set-f32", cfun_set_f32,
     "(capnp/set-f32 body byte-offset value &opt schema-default)"},
    {"set-f64", cfun_set_f64,
     "(capnp/set-f64 body byte-offset value &opt schema-default)"},
    {"set-bool", cfun_set_bool,
     "(capnp/set-bool body bit-offset value &opt schema-default)"},
    {"set-text", cfun_set_text,
     "(capnp/set-text body pointer-index value)"},
    {"set-data", cfun_set_data,
     "(capnp/set-data body pointer-index value)"},
    {"clear-pointer", cfun_clear_pointer,
     "(capnp/clear-pointer body pointer-index)\n\nSet a pointer slot to null."},
    {"build-message", cfun_build_message,
     "(capnp/build-message dwords pwords fields)\n\n"
     "Build a framed root struct. fields: array of "
     "[:u16|:u32|:u64|:f64|:bool|:text off val]."},
    {NULL, NULL, NULL}};

/* Embedding path: the host injects into a root env that applies no import
 * prefix of its own, so bind the qualified capnp/… names here. */
void capnp_janet_register(JanetTable *env) {
  janet_cfuns_prefix(env, "capnp", capnp_cfuns);
}

void capnp_janet_env(JanetTable *env) { capnp_janet_register(env); }

/* (import capnp) prefixes every binding with the module name, so the module
 * entry binds bare names; call sites stay at capnp/build-message. */
JANET_MODULE_ENTRY(JanetTable *env) { janet_cfuns(env, "capnp", capnp_cfuns); }

void capnp_janet_lookup_into(JanetTable *lookup) {
  JanetTable *tmp;

  if (!lookup)
    return;
  tmp = janet_table(32);
  capnp_janet_register(tmp);
  janet_env_lookup_into(lookup, tmp, NULL, 1);
}
