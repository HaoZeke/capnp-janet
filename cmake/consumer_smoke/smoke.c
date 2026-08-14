/* Consumes the installed/added capnp-janet runtime through its public
 * headers only: build a framed message, read it back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <capnp-janet/capnp_builder.h>
#include <capnp-janet/capnp_message.h>

int main(void) {
	capnp_builder_t b;
	capnp_bptr_t root, body;
	uint8_t *flat = NULL;
	size_t flat_len = 0;
	capnp_message_t msg;
	capnp_ptr_t r;
	const char *s = NULL;
	size_t sl = 0;

	capnp_builder_init(&b);
	if (capnp_builder_root(&b, &root) ||
	    capnp_builder_struct(&root, 1, 1, &body)) {
		fprintf(stderr, "alloc failed\n");
		return 1;
	}
	/* set_text takes the struct's data-word count so it can find the
	 * pointer section; this root is 1 data word + 1 pointer word. */
	if (capnp_builder_set_u32(&body, 0, 42u) ||
	    capnp_builder_set_text(&body, 1, 0, "hello", 5)) {
		fprintf(stderr, "set failed\n");
		return 1;
	}
	if (capnp_builder_serialize(&b, &flat, &flat_len)) {
		fprintf(stderr, "serialize failed\n");
		return 1;
	}
	capnp_builder_free(&b);

	if (capnp_message_from_flat(&msg, flat, flat_len) != CAPNP_OK ||
	    capnp_root(&msg, &r) != CAPNP_OK) {
		fprintf(stderr, "read back failed\n");
		return 1;
	}
	if (capnp_get_u32(&r, 0, 0) != 42u) {
		fprintf(stderr, "u32 mismatch\n");
		return 1;
	}
	if (capnp_get_text(&r, 0, &s, &sl) != CAPNP_OK || sl != 5 ||
	    memcmp(s, "hello", 5) != 0) {
		fprintf(stderr, "text mismatch\n");
		return 1;
	}
	capnp_message_free(&msg);
	free(flat);
	printf("capnp-janet consumer smoke ok\n");
	return 0;
}
