/* SPDX-License-Identifier: MIT */
#ifndef CAPNP_JANET_TEST_HARNESS_H
#define CAPNP_JANET_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);            \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ_U32(a, b, msg)                                                \
  do {                                                                         \
    uint32_t _a = (uint32_t)(a);                                               \
    uint32_t _b = (uint32_t)(b);                                               \
    if (_a != _b) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s (got %u want %u)\n", __FILE__, __LINE__, \
              msg, (unsigned)_a, (unsigned)_b);                                \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_STREQ(s, n, want, msg)                                           \
  do {                                                                         \
    const char *_s = (s);                                                      \
    size_t _n = (n);                                                           \
    const char *_w = (want);                                                   \
    size_t _wn = strlen(_w);                                                   \
    if (_n != _wn || memcmp(_s, _w, _n) != 0) {                                \
      fprintf(stderr, "FAIL %s:%d: %s (got \"%.*s\" want \"%s\")\n", __FILE__, \
              __LINE__, msg, (int)_n, _s ? _s : "", _w);                       \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_NEAR(a, b, eps, msg)                                             \
  do {                                                                         \
    double _a = (double)(a);                                                   \
    double _b = (double)(b);                                                   \
    double _e = (double)(eps);                                                 \
    double _d = _a > _b ? _a - _b : _b - _a;                                   \
    if (_d > _e) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s (got %g want %g)\n", __FILE__, __LINE__, \
              msg, _a, _b);                                                    \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

static int harness_finish(void) {
  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("ok\n");
  return 0;
}

#endif
