#ifndef CAPNP_JANET_DYN_H
#define CAPNP_JANET_DYN_H

#include <capnp-janet/capnp_message.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Schema reflection over a CodeGeneratorRequest (the bytes a capnp
 * compiler plugin reads) plus text and JSON codecs for one struct.
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

CAPNP_JANET_EXPORT int capnp_dyn_get_bool(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                          const char *type_name, const char *field,
                                          int *out);
CAPNP_JANET_EXPORT int capnp_dyn_get_i64(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                         const char *type_name, const char *field,
                                         int64_t *out);
CAPNP_JANET_EXPORT int capnp_dyn_get_text(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                          const char *type_name, const char *field,
                                          const char **out, size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_get_ptr(const capnp_dyn_t *d, const capnp_ptr_t *s,
                                         const char *type_name, const char *field,
                                         capnp_ptr_t *out);

CAPNP_JANET_EXPORT int capnp_dyn_encode_text(const capnp_dyn_t *d, const char *type_name,
                                             const capnp_ptr_t *s, char **out,
                                             size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_decode_text(const capnp_dyn_t *d, const char *type_name,
                                             const char *text, size_t len, uint8_t **out,
                                             size_t *out_len);
CAPNP_JANET_EXPORT int capnp_dyn_encode_json(const capnp_dyn_t *d, const char *type_name,
                                             const capnp_ptr_t *s, char **out,
                                             size_t *len);
CAPNP_JANET_EXPORT int capnp_dyn_decode_json(const capnp_dyn_t *d, const char *type_name,
                                             const char *text, size_t len, uint8_t **out,
                                             size_t *out_len);

#endif
