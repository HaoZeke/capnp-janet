/*
 * AddressBook sample parity with Cap'n C++/Python samples:
 *   https://github.com/capnproto/capnproto/blob/master/c++/samples/addressbook.c++
 *   https://github.com/capnproto/pycapnp/blob/master/examples/addressbook.py
 *
 * Person wire layout (capnp compile -ocapnp):
 *   8 data bytes, 4 pointers
 *   id u32 @byte0; employment union tag u16 @byte4 (bits 32-48)
 *   name ptr0, email ptr1, phones ptr2, employment-text ptr3
 * PhoneNumber: 8 data bytes, 1 ptr; type u16 @0, number ptr0
 */
#include "harness.h"

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Employment union tags (schema ordinals). */
enum {
  EMP_UNEMPLOYED = 0,
  EMP_EMPLOYER = 1,
  EMP_SCHOOL = 2,
  EMP_SELF = 3
};

enum {
  PHONE_MOBILE = 0,
  PHONE_HOME = 1,
  PHONE_WORK = 2
};

/* Person: 1 data word, 4 ptrs. Phone: 1 data word, 1 ptr. */
enum { PERSON_D = 1, PERSON_P = 4, PHONE_D = 1, PHONE_P = 1 };

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

static void check_person(const capnp_ptr_t *p, uint32_t id, const char *name,
                         const char *email, uint16_t emp_tag,
                         const char *emp_text, uint32_t nphones) {
  const char *s;
  size_t n;
  capnp_ptr_t phones;

  CHECK_EQ_U32(capnp_get_u32(p, 0, 0), id, "person id");
  CHECK_EQ_U32(capnp_get_u16(p, 4, 0xffff), emp_tag, "employment tag");
  CHECK(capnp_get_text(p, 0, &s, &n) == CAPNP_OK, "name text");
  CHECK_STREQ(s, n, name, "name");
  CHECK(capnp_get_text(p, 1, &s, &n) == CAPNP_OK, "email text");
  CHECK_STREQ(s, n, email, "email");
  if (emp_text) {
    CHECK(capnp_get_text(p, 3, &s, &n) == CAPNP_OK, "emp text");
    CHECK_STREQ(s, n, emp_text, "employment text");
  }
  CHECK(capnp_getp(p, 2, &phones) == CAPNP_OK, "phones list");
  CHECK(phones.kind == CAPNP_PK_LIST, "phones is list");
  CHECK_EQ_U32(capnp_list_len(&phones), nphones, "nphones");
}

static void check_phone(const capnp_ptr_t *list, uint32_t idx,
                        const char *number, uint16_t type) {
  capnp_ptr_t ph;
  const char *s;
  size_t n;
  CHECK(capnp_list_getp(list, idx, &ph) == CAPNP_OK, "phone elem");
  CHECK_EQ_U32(capnp_get_u16(&ph, 0, 0xffff), type, "phone type");
  CHECK(capnp_get_text(&ph, 0, &s, &n) == CAPNP_OK, "phone number");
  CHECK_STREQ(s, n, number, "phone number text");
}

static void verify_alice_bob_book(const capnp_ptr_t *book) {
  capnp_ptr_t people, phones, alice, bob;

  CHECK(book->kind == CAPNP_PK_STRUCT, "book struct");
  CHECK(capnp_getp(book, 0, &people) == CAPNP_OK, "people");
  CHECK(people.kind == CAPNP_PK_LIST && people.esize == CAPNP_SZ_COMPOSITE,
        "people composite");
  CHECK_EQ_U32(capnp_list_len(&people), 2, "2 people");

  CHECK(capnp_list_getp(&people, 0, &alice) == CAPNP_OK, "alice");
  check_person(&alice, 123, "Alice", "alice@example.com", EMP_SCHOOL, "MIT",
               1);
  CHECK(capnp_getp(&alice, 2, &phones) == CAPNP_OK, "alice phones");
  check_phone(&phones, 0, "555-1212", PHONE_MOBILE);

  CHECK(capnp_list_getp(&people, 1, &bob) == CAPNP_OK, "bob");
  check_person(&bob, 456, "Bob", "bob@example.com", EMP_UNEMPLOYED, NULL, 2);
  CHECK(capnp_getp(&bob, 2, &phones) == CAPNP_OK, "bob phones");
  check_phone(&phones, 0, "555-4567", PHONE_HOME);
  check_phone(&phones, 1, "555-7654", PHONE_WORK);
}

/* Hand-built with the same content as the C++ sample writeAddressBook. */
static void test_builder_alice_bob(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body, phone_slot;
  capnp_bptr_t people0, phones0, phones1;
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t book;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  /* AddressBook: 0 data, 1 ptr */
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "book");
  CHECK(capnp_builder_set_list_struct(&body, 0, 0, 2, PERSON_D,
                                      PERSON_P, &people0) == CAPNP_OK,
        "people list");

  /* Alice: schema / C++ text-encoder order (name, email, phones, school). */
  CHECK(capnp_builder_set_u32(&people0, 0, 123) == CAPNP_OK, "alice id");
  CHECK(capnp_builder_set_text(&people0, PERSON_D, 0, "Alice", 5) ==
            CAPNP_OK,
        "alice name");
  CHECK(capnp_builder_set_text(&people0, PERSON_D, 1, "alice@example.com",
                               17) == CAPNP_OK,
        "alice email");
  CHECK(capnp_builder_set_list_struct(&people0, PERSON_D, 2, 1, PHONE_D,
                                      PHONE_P, &phones0) == CAPNP_OK,
        "alice phones");
  CHECK(capnp_builder_set_text(&phones0, PHONE_D, 0, "555-1212", 8) ==
            CAPNP_OK,
        "alice num");
  CHECK(capnp_builder_set_u16(&phones0, 0, PHONE_MOBILE) == CAPNP_OK,
        "alice type");
  CHECK(capnp_builder_set_u16(&people0, 4, EMP_SCHOOL) == CAPNP_OK,
        "alice emp tag");
  CHECK(capnp_builder_set_text(&people0, PERSON_D, 3, "MIT", 3) == CAPNP_OK,
        "alice school");

  /* Bob: people0 + PERSON_D+PERSON_P */
  {
    capnp_bptr_t bob = capnp_bptr_add(people0, PERSON_D + PERSON_P);
    CHECK(capnp_builder_set_u32(&bob, 0, 456) == CAPNP_OK, "bob id");
    CHECK(capnp_builder_set_text(&bob, PERSON_D, 0, "Bob", 3) == CAPNP_OK,
          "bob name");
    CHECK(capnp_builder_set_text(&bob, PERSON_D, 1, "bob@example.com", 15) ==
              CAPNP_OK,
          "bob email");
    CHECK(capnp_builder_set_list_struct(&bob, PERSON_D, 2, 2, PHONE_D,
                                        PHONE_P, &phones1) == CAPNP_OK,
          "bob phones");
    CHECK(capnp_builder_set_text(&phones1, PHONE_D, 0, "555-4567", 8) ==
              CAPNP_OK,
          "hn");
    CHECK(capnp_builder_set_u16(&phones1, 0, PHONE_HOME) == CAPNP_OK, "h");
    {
      capnp_bptr_t p1 = capnp_bptr_add(phones1, PHONE_D + PHONE_P);
      CHECK(capnp_builder_set_text(&p1, PHONE_D, 0, "555-7654", 8) ==
                CAPNP_OK,
            "wn");
      CHECK(capnp_builder_set_u16(&p1, 0, PHONE_WORK) == CAPNP_OK, "w");
    }
    CHECK(capnp_builder_set_u16(&bob, 4, EMP_UNEMPLOYED) == CAPNP_OK,
          "bob emp");
  }

  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "ser");
  capnp_builder_free(&b);

  {
    const char *src = getenv("CAPNP_JANET_SOURCE_ROOT");
    char path[1024];
    uint8_t *want = NULL;
    size_t want_len = 0;
    if (!src || !src[0])
      src = ".";
    snprintf(path, sizeof(path), "%s/test/fixtures/addressbook_alice_bob.bin",
             src);
    CHECK(load_file(path, &want, &want_len) == 0, "load encode golden");
    if (want) {
      CHECK(flat_len == want_len && memcmp(flat, want, flat_len) == 0,
            "schema-order encode == capnp encode");
      free(want);
    }
  }

  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "from_flat");
  free(flat);
  CHECK(capnp_root(&m, &book) == CAPNP_OK, "root read");
  verify_alice_bob_book(&book);
  capnp_message_free(&m);
  (void)phone_slot;
}

/* Official-encoder golden (scripts/gen-sample-fixtures.sh). */
static void test_golden_alice_bob(void) {
  const char *src = getenv("CAPNP_JANET_SOURCE_ROOT");
  char path[1024];
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t book;

  if (!src || !src[0])
    src = ".";
  snprintf(path, sizeof(path), "%s/test/fixtures/addressbook_alice_bob.bin",
           src);
  if (load_file(path, &flat, &flat_len) != 0) {
    fprintf(stderr, "SKIP golden: cannot open %s\n", path);
    return;
  }
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "golden flat");
  free(flat);
  CHECK(capnp_root(&m, &book) == CAPNP_OK, "golden root");
  verify_alice_bob_book(&book);
  capnp_message_free(&m);
}

/* Self-employed person (union void) with zero phones. */
static void test_self_employed_empty_phones(void) {
  capnp_builder_t b;
  capnp_bptr_t root, body;
  capnp_bptr_t pe, ph;
  uint8_t *flat = NULL;
  size_t flat_len = 0;
  capnp_message_t m;
  capnp_ptr_t book, people, p, phones;
  const char *s;
  size_t n;

  capnp_builder_init(&b);
  CHECK(capnp_builder_root(&b, &root) == CAPNP_OK, "root");
  CHECK(capnp_builder_struct(&root, 0, 1, &body) == CAPNP_OK, "book");
  CHECK(capnp_builder_set_list_struct(&body, 0, 0, 1, PERSON_D,
                                      PERSON_P, &pe) == CAPNP_OK,
        "ppl");
  CHECK(capnp_builder_set_u32(&pe, 0, 1) == CAPNP_OK, "id");
  CHECK(capnp_builder_set_u16(&pe, 4, EMP_SELF) == CAPNP_OK, "self tag");
  CHECK(capnp_builder_set_text(&pe, PERSON_D, 0, "Cara", 4) == CAPNP_OK,
        "name");
  CHECK(capnp_builder_set_text(&pe, PERSON_D, 1, "c@x", 3) == CAPNP_OK,
        "email");
  CHECK(capnp_builder_set_list_struct(&pe, PERSON_D, 2, 0, PHONE_D, PHONE_P,
                                      &ph) == CAPNP_OK,
        "0 phones");
  (void)ph;
  CHECK(capnp_builder_serialize(&b, &flat, &flat_len) == CAPNP_OK, "ser");
  capnp_builder_free(&b);
  CHECK(capnp_message_from_flat(&m, flat, flat_len) == CAPNP_OK, "flat");
  free(flat);
  CHECK(capnp_root(&m, &book) == CAPNP_OK, "root");
  CHECK(capnp_getp(&book, 0, &people) == CAPNP_OK, "people");
  CHECK(capnp_list_getp(&people, 0, &p) == CAPNP_OK, "p0");
  CHECK_EQ_U32(capnp_get_u16(&p, 4, 0xffff), EMP_SELF, "self tag");
  CHECK(capnp_get_text(&p, 0, &s, &n) == CAPNP_OK, "name");
  CHECK_STREQ(s, n, "Cara", "Cara");
  CHECK(capnp_getp(&p, 2, &phones) == CAPNP_OK, "phones");
  CHECK_EQ_U32(capnp_list_len(&phones), 0, "no phones");
  capnp_message_free(&m);
}

int main(void) {
  test_builder_alice_bob();
  test_golden_alice_bob();
  test_self_employed_empty_phones();
  return harness_finish();
}
