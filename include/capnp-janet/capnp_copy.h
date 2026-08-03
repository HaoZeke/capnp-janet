/* SPDX-License-Identifier: MIT */
#ifndef CAPNP_JANET_COPY_H
#define CAPNP_JANET_COPY_H

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>
#include <stddef.h>
#include <stdint.h>

/* Deep-copy @src into builder pointer at (slot_seg, slot_word). */
int capnp_copy_ptr_to_word(capnp_builder_t *b, uint32_t slot_seg,
                           size_t slot_word, const capnp_ptr_t *src, int depth);

#endif
