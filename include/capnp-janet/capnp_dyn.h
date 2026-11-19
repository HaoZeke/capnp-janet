#ifndef CAPNP_JANET_DYN_H
#define CAPNP_JANET_DYN_H

#include <capnp-janet/capnp_message.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Schema reflection over a CodeGeneratorRequest plus text and JSON codecs.
 *
 * Obtain the request with `capnp compile -o- schema.capnp`, which writes
 * the same bytes a plugin would read. This library does not parse `.capnp`
 * source itself: the official compiler already resolves imports, generics,
 * and annotations, and a second parser would drift from it.
 *
 * Text matches the struct-literal language `capnp encode` / `capnp decode`
 * use. JSON matches `capnp convert binary:json`: objects for structs and
 * named groups, arrays for lists, and Data as an array of byte values.
 * Scalar schema defaults are applied, so the values are logical values.
 */

typedef struct capnp_dyn capnp_dyn_t;

CAPNP_JANET_EXPORT int capnp_dyn_init(capnp_dyn_t **out, const uint8_t *request,
                                      size_t len);
CAPNP_JANET_EXPORT void capnp_dyn_free(capnp_dyn_t *d);

/* Empty string when the last decode succeeded or no decode has run.
 * Otherwise a short message that names the field or token. */
CAPNP_JANET_EXPORT const char *capnp_dyn_error(const capnp_dyn_t *d);

CAPNP_JANET_EXPORT int capnp_dyn_get_bool(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                          const char *type_name, const char *field,
                                          int *out);
CAPNP_JANET_EXPORT int capnp_dyn_get_i64(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                         const char *type_name, const char *field,
                                         int64_t *out);
CAPNP_JANET_EXPORT int capnp_dyn_get_u64(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                         const char *type_name, const char *field,
                                         uint64_t *out);
CAPNP_JANET_EXPORT int capnp_dyn_get_f64(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                         const char *type_name, const char *field,
                                         double *out);
CAPNP_JANET_EXPORT int capnp_dyn_get_text(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                          const char *type_name, const char *field,
                                          const char **out, size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_get_ptr(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                         const char *type_name, const char *field,
                                         capnp_ptr_t *out);
CAPNP_JANET_EXPORT int capnp_dyn_which(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                       const char *type_name, const char **out);

CAPNP_JANET_EXPORT int capnp_dyn_encode_text(const capnp_dyn_t *d, const char *type_name,
                                             const capnp_ptr_t *s, char **out,
                                             size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_encode_text_short(const capnp_dyn_t *d,
                                                   const char *type_name,
                                                   const capnp_ptr_t *s,
                                                   char **out, size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_decode_text(const capnp_dyn_t *d, const char *type_name,
                                             const char *text, size_t len, uint8_t **out,
                                             size_t *out_len);
CAPNP_JANET_EXPORT int capnp_dyn_encode_json(const capnp_dyn_t *d, const char *type_name,
                                             const capnp_ptr_t *s, char **out,
                                             size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_encode_json_short(const capnp_dyn_t *d,
                                                   const char *type_name,
                                                   const capnp_ptr_t *s,
                                                   char **out, size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_decode_json(const capnp_dyn_t *d, const char *type_name,
                                             const char *text, size_t len, uint8_t **out,
                                             size_t *out_len);

/* Print the text form of a const, or of a field default, the way
 * `capnp eval schema.capnp name` does. `expr` is `name`, `Type.field`,
 * `Type.field.nested`, or `Type.field[index]`. The result has no trailing
 * newline. */
CAPNP_JANET_EXPORT int capnp_dyn_eval(const capnp_dyn_t *d, const char *expr,
                                      char **out, size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_eval_ex(const capnp_dyn_t *d, const char *expr,
                                         int json, int pretty, char **out,
                                         size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_eval_struct(const capnp_dyn_t *d,
                                             const char *expr, uint8_t **out,
                                             size_t *out_len);

#endif
