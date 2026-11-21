/* SPDX-License-Identifier: MIT */
#ifndef CAPNP_EVAL_KJ_STREAM_H
#define CAPNP_EVAL_KJ_STREAM_H
/*
 * The kj input streams `capnp convert` reads with, over input held in
 * memory: BufferedInputStreamWrapper on stdin, read as a regular file
 * whose read() fills what it is asked for, and PackedInputStream on top.
 * A short read that kj recovers from sets `soft`; one it cannot recover
 * from sets `fatal`, after which the stream is not used again.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const uint8_t *data;
  size_t len;
  /* Bytes of the input the buffer has taken in. */
  size_t fdpos;
  /* The unread end of the buffer: data[fdpos - avail, fdpos). */
  size_t avail;
  /* Buffer size: 8192 on stdin; `len` for kj::ArrayInputStream. */
  size_t chunk;
  int soft;
  int fatal;
} kj_stream_t;

void kj_stream_init(kj_stream_t *s, const uint8_t *data, size_t len,
                    size_t chunk);

/* BufferedInputStream::tryGetReadBuffer().size(). */
size_t kj_stream_buffered(kj_stream_t *s);

/* InputStream::read(dst, min, max) on the plain stream, or on a
 * PackedInputStream over it when @packed is set. kj fills the rest of a
 * short read with zeros, so @dst has to be zeroed. *progress, when given,
 * receives how far the bytes written reach, including after a fatal
 * error. */
size_t kj_stream_read(kj_stream_t *s, int packed, uint8_t *dst, size_t min,
                      size_t max, size_t *progress);

/* PackedInputStream::tryRead, for the check that nothing follows. */
size_t kj_stream_packed_try_read(kj_stream_t *s, uint8_t *dst, size_t min,
                                 size_t max);

#endif
