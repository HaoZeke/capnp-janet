/* SPDX-License-Identifier: MIT */
#ifndef CAPNP_JANET_CANONICAL_H
#define CAPNP_JANET_CANONICAL_H

#include <capnp-janet/capnp_message.h>
#include <stddef.h>
#include <capnp-janet/capnp_kinds.h>
#include <stdint.h>

/*
 * Canonical form (encoding.html#canonicalization): single segment, no far
 * pointers, preorder layout, trailing zero data words and null pointer slots
 * truncated, zero-sized structs use offset -1.
 *
 * Output matches `capnp convert binary:canonical`: raw segment bytes with
 * no stream framing table.
 */
CAPNP_JANET_EXPORT int capnp_canonicalize(const capnp_message_t *m, uint8_t **out, size_t *out_len);

/* Stream-framed single-segment form of the canonical message (for roundtrip). */
CAPNP_JANET_EXPORT int capnp_canonicalize_framed(const capnp_message_t *m, uint8_t **out,
                              size_t *out_len);

#endif
