#ifndef CAPNP_JANET_POINTER_H
#define CAPNP_JANET_POINTER_H

#include <capnp-janet/capnp_kinds.h>
#include <stdint.h>

/* Decode / encode 64-bit little-endian Cap'n pointer words. */

static inline uint64_t capnp_load_le64(const uint8_t *p) {
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}

static inline uint32_t capnp_load_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void capnp_store_le64(uint8_t *p, uint64_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
  p[4] = (uint8_t)(v >> 32);
  p[5] = (uint8_t)(v >> 40);
  p[6] = (uint8_t)(v >> 48);
  p[7] = (uint8_t)(v >> 56);
}

static inline void capnp_store_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline int capnp_wp_kind(uint64_t w) { return (int)(w & 3ull); }

/* Signed 30-bit word offset for struct/list pointers. */
static inline int32_t capnp_wp_offset(uint64_t w) {
  uint64_t u = (w >> 2) & 0x3fffffffull;
  if (u >= 0x20000000ull)
    u -= 0x40000000ull;
  return (int32_t)u;
}

static inline uint16_t capnp_wp_struct_dwords(uint64_t w) {
  return (uint16_t)((w >> 32) & 0xffffull);
}

static inline uint16_t capnp_wp_struct_pwords(uint64_t w) {
  return (uint16_t)((w >> 48) & 0xffffull);
}

static inline int capnp_wp_list_esize(uint64_t w) {
  return (int)((w >> 32) & 7ull);
}

static inline uint32_t capnp_wp_list_count(uint64_t w) {
  return (uint32_t)((w >> 35) & 0x1fffffffull);
}

static inline int capnp_wp_far_two(uint64_t w) { return (int)((w >> 2) & 1ull); }

static inline uint32_t capnp_wp_far_off(uint64_t w) {
  return (uint32_t)((w >> 3) & 0x1fffffffull);
}

static inline uint32_t capnp_wp_far_seg(uint64_t w) {
  return (uint32_t)((w >> 32) & 0xffffffffull);
}

static inline uint64_t capnp_wp_make_struct(int32_t off, uint16_t dwords,
                                           uint16_t pwords) {
  uint64_t w = ((uint64_t)((uint32_t)off & 0x3fffffffu) << 2);
  w |= ((uint64_t)dwords << 32);
  w |= ((uint64_t)pwords << 48);
  return w;
}

static inline uint64_t capnp_wp_make_list(int32_t off, int esize,
                                         uint32_t count) {
  uint64_t w = 1ull | ((uint64_t)((uint32_t)off & 0x3fffffffu) << 2);
  w |= ((uint64_t)(esize & 7) << 32);
  w |= ((uint64_t)(count & 0x1fffffffu) << 35);
  return w;
}

static inline uint64_t capnp_wp_make_far(int two_word_pad, uint32_t word_off,
                                         uint32_t seg_id) {
  uint64_t w = 2ull;
  if (two_word_pad)
    w |= 4ull;
  w |= ((uint64_t)(word_off & 0x1fffffffu) << 3);
  w |= ((uint64_t)seg_id << 32);
  return w;
}

#endif /* CAPNP_JANET_POINTER_H */
