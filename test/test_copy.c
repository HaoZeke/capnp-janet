/* SPDX-License-Identifier: MIT */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#include <stdlib.h>
#include <string.h>

/* Deep-copy AddressBook people[0] (Alice) into a new root pointer slot. */
static void test_copy_struct_slot(void) {
  const char *src = getenv("CAPNP_JANET_SOURCE_ROOT");
  char path[1024];
  FILE *f;
  long sz;
  uint8_t *bin = NULL;
  capnp_message_t m;
  capnp_ptr_t book, people, alice;
  capnp_builder_t b;
  capnp_bptr_t root, body, slot;
  uint8_t *flat = NULL;
  size_t flen = 0;
  capnp_message_t m2;
  capnp_ptr_t r;
  const char *name;
  size_t n;

  if (!src || !src[0])
    src = ".";
  snprintf(path, sizeof(path), "%s/test/fixtures/addressbook_alice_bob.bin",
           src);
  f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "SKIP copy test (no fixture)\n");
    return;
  }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  bin = malloc((size_t)sz);
  fread(bin, 1, (size_t)sz, f);
  fclose(f);

  CHECK(capnp_message_from_flat(&m, bin, (size_t)sz) == CAPNP_OK, "from");
  free(bin);
  CHECK(capnp_root(&m, &book) == CAPNP_OK, "root");
  CHECK(capnp_getp(&book, 0, &people) == CAPNP_OK, "people");
  CHECK(capnp_list_getp(&people, 0, &alice) == CAPNP_OK, "alice");

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "b root");
  /* Outer { person @0 :Person } — 0 data, 1 ptr */
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "outer");
  CHECK(capnp_builder_slot(&body, 0, 0, &slot) == CAPNP_OK, "slot");
  CHECK(capnp_builder_copy_ptr(&slot, &alice) == CAPNP_OK, "copy alice");
  CHECK(capnp_builder_serialize(&b, &flat, &flen) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  capnp_message_free(&m);

  CHECK(capnp_message_from_flat(&m2, flat, flen) == CAPNP_OK, "m2");
  free(flat);
  CHECK(capnp_root(&m2, &r) == CAPNP_OK, "r");
  {
    capnp_ptr_t p;
    CHECK(capnp_getp(&r, 0, &p) == CAPNP_OK, "person");
    CHECK_EQ_U32(capnp_get_u32(&p, 0, 0), 123, "id");
    CHECK(capnp_get_text(&p, 0, &name, &n) == CAPNP_OK, "name");
    CHECK_STREQ(name, n, "Alice", "Alice");
  }
  capnp_message_free(&m2);
}

int main(void) {
  test_copy_struct_slot();
  return harness_finish();
}
