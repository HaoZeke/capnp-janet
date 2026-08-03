/* SPDX-License-Identifier: MIT */
#ifndef CAPNP_JANET_PACKED_H
#define CAPNP_JANET_PACKED_H

#include <capnp-janet/capnp_kinds.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Cap'n Proto packing (encoding.html#packing).
 * Applied on top of stream-framed (or flat) word-aligned bytes.
 *
 * Pack: each word → tag byte + nonzero payload bytes; 0x00 / 0xff escapes
 * for zero runs and verbatim runs. After an 0xff tag, Cap'n C++
 * PackedOutputStream includes up to 255 following words that each contain
 * fewer than two zero bytes (matches `capnp convert binary:packed`).
 */

/* Pack word-aligned input. *out is malloc'd; caller frees. */
int capnp_pack(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/* Unpack to word-aligned output. *out is malloc'd; caller frees. */
int capnp_unpack(const uint8_t *in, size_t in_len, uint8_t **out,
                 size_t *out_len);

#endif /* CAPNP_JANET_PACKED_H */
