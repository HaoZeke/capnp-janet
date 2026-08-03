/* SPDX-License-Identifier: MIT */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_canonical.h>
#include <capnp-janet/capnp_message.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_file(const char *path, uint8_t **out, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  long sz;
  uint8_t *buf;
  if (!f)
    return -1;
  if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return -1;
  }
  buf = malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  *out = buf;
  *out_len = (size_t)sz;
  return 0;
}

/* Trailing unset fields truncated; re-read still works. */
static void test_truncate_defaults(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  uint8_t *flat = NULL, *canon = NULL, *framed = NULL;
  size_t flen = 0, clen = 0, frlen = 0;
  capnp_message_t m, m2;
  capnp_ptr_t r;

  /* Struct with 2 data words but only first used; second stays zero → truncate. */
  capnp_builder_init(&b);
  capnp_builder_root(&b, &root);
  capnp_builder_struct(&root, 2, 0, &body);
  capnp_builder_set_u32(&body, 0, 7);
  /* word 1 remains zero */
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flen) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_canonicalize(&m, &canon, &clen) == CAPNP_OK, "canon");
  /* Canonical should be smaller than 2-data-word form (root + 1 data word). */
  CHECK(clen == 16, "root+1 data word"); /* 2 words */
  CHECK(capnp_canonicalize_framed(&m, &framed, &frlen) == CAPNP_OK, "framed");
  capnp_message_free(&m);
  CHECK(capnp_message_from_flat(&m2, framed, frlen) == CAPNP_OK, "reopen");
  free(framed);
  free(canon);
  CHECK(capnp_root(&m2, &r) == CAPNP_OK, "root");
  CHECK_EQ_U32(capnp_get_u32(&r, 0, 0), 7, "value");
  /* past end default still works on truncated wire */
  CHECK_EQ_U32(capnp_get_u32(&r, 8, 99), 99, "default past end");
  capnp_message_free(&m2);
}

static void test_addressbook_canonical_readable(void) {
  const char *src = getenv("CAPNP_JANET_SOURCE_ROOT");
  char path[1024];
  uint8_t *bin = NULL, *canon = NULL, *framed = NULL;
  size_t blen = 0, clen = 0, frlen = 0;
  capnp_message_t m, m2;
  capnp_ptr_t book, people, alice;
  const char *s;
  size_t n;

  if (!src || !src[0])
    src = ".";
  snprintf(path, sizeof(path), "%s/test/fixtures/addressbook_alice_bob.bin",
           src);
  if (load_file(path, &bin, &blen) != 0) {
    fprintf(stderr, "SKIP addressbook canonical\n");
    return;
  }
  CHECK(capnp_message_from_flat(&m, bin, blen) == CAPNP_OK, "flat");
  free(bin);
  CHECK(capnp_canonicalize(&m, &canon, &clen) == CAPNP_OK, "canon");
  CHECK(clen > 0 && clen % 8 == 0, "aligned");
  CHECK(capnp_canonicalize_framed(&m, &framed, &frlen) == CAPNP_OK, "framed");
  capnp_message_free(&m);

  /* Compare to official golden if present */
  {
    char gpath[1024];
    uint8_t *golden = NULL;
    size_t glen = 0;
    snprintf(gpath, sizeof(gpath),
             "%s/test/fixtures/addressbook_alice_bob.canonical", src);
    if (load_file(gpath, &golden, &glen) == 0) {
      CHECK(glen == clen && memcmp(golden, canon, clen) == 0,
            "matches official canonical");
      free(golden);
    }
  }

  free(canon);
  CHECK(capnp_message_from_flat(&m2, framed, frlen) == CAPNP_OK, "reopen");
  free(framed);
  CHECK(capnp_root(&m2, &book) == CAPNP_OK, "root");
  CHECK(capnp_getp(&book, 0, &people) == CAPNP_OK, "people");
  CHECK_EQ_U32(capnp_list_len(&people), 2, "2");
  CHECK(capnp_list_getp(&people, 0, &alice) == CAPNP_OK, "alice");
  CHECK(capnp_get_text(&alice, 0, &s, &n) == CAPNP_OK, "name");
  CHECK_STREQ(s, n, "Alice", "Alice");
  capnp_message_free(&m2);
}

int main(void) {
  test_truncate_defaults();
  test_addressbook_canonical_readable();
  return harness_finish();
}
