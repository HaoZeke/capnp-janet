/* SPDX-License-Identifier: MIT */
/*
 * kj::BufferedInputStreamWrapper and capnp::_::PackedInputStream from
 * kj/io.c++ and capnp/serialize-packed.c++ (1.4.0), step for step, so a
 * truncated or corrupt stream ends where `capnp convert` ends it.
 */
#include "kj_stream.h"

#include <string.h>

void kj_stream_init(kj_stream_t *s, const uint8_t *data, size_t len,
                    size_t chunk) {
  memset(s, 0, sizeof(*s));
  s->data = data;
  s->len = len;
  s->chunk = chunk ? chunk : 1;
}

/* read() on a regular file returns all it is asked for until the end. */
static size_t fd_read(kj_stream_t *s, uint8_t *dst, size_t max) {
  size_t n = s->len - s->fdpos;
  if (n > max)
    n = max;
  if (dst && n)
    memcpy(dst, s->data + s->fdpos, n);
  s->fdpos += n;
  return n;
}

static const uint8_t *buf_start(const kj_stream_t *s) {
  return s->data + s->fdpos - s->avail;
}

size_t kj_stream_buffered(kj_stream_t *s) {
  if (s->avail == 0)
    s->avail = fd_read(s, NULL, s->chunk);
  return s->avail;
}

/* getReadBuffer() fails on an empty buffer, and kj cannot recover. */
static size_t get_buffer(kj_stream_t *s) {
  size_t n = kj_stream_buffered(s);
  if (n == 0)
    s->fatal = 1;
  return n;
}

static size_t wrapper_try_read(kj_stream_t *s, uint8_t *dst, size_t min,
                               size_t max) {
  size_t first, n, second;
  if (min <= s->avail) {
    n = s->avail < max ? s->avail : max;
    memcpy(dst, buf_start(s), n);
    s->avail -= n;
    return n;
  }
  first = s->avail;
  memcpy(dst, buf_start(s), first);
  s->avail = 0;
  max -= first;
  if (max <= s->chunk) {
    n = fd_read(s, NULL, s->chunk);
    second = n < max ? n : max;
    memcpy(dst + first, s->data + s->fdpos - n, second);
    s->avail = n - second;
    return first + second;
  }
  return first + fd_read(s, dst + first, max);
}

static size_t wrapper_read(kj_stream_t *s, uint8_t *dst, size_t min,
                           size_t max) {
  size_t n = wrapper_try_read(s, dst, min, max);
  if (n < min) {
    s->soft = 1;
    return min;
  }
  return n;
}

static void wrapper_skip(kj_stream_t *s, size_t bytes) {
  size_t n;
  if (bytes <= s->avail) {
    s->avail -= bytes;
    return;
  }
  bytes -= s->avail;
  s->avail = 0;
  if (bytes <= s->chunk) {
    n = fd_read(s, NULL, s->chunk);
    if (n < bytes)
      s->soft = 1;
    else
      s->avail = n - bytes;
    return;
  }
  if (fd_read(s, NULL, bytes) < bytes)
    s->soft = 1;
}

static size_t packed_try_read(kj_stream_t *s, uint8_t *dst, size_t min,
                              size_t max, size_t *progress) {
  uint8_t *out = dst;
  uint8_t *const out_end = dst + max;
  uint8_t *const out_min = dst + min;
  const uint8_t *buf, *in;
  size_t bsize;

#define REMAINING ((size_t)(buf + bsize - in))
#define REFRESH()                                                              \
  do {                                                                         \
    wrapper_skip(s, bsize);                                                    \
    bsize = get_buffer(s);                                                     \
    if (s->fatal) {                                                            \
      *progress = (size_t)(out - dst);                                         \
      return 0;                                                                \
    }                                                                          \
    buf = in = buf_start(s);                                                   \
  } while (0)

  *progress = 0;
  if (max == 0)
    return 0;
  bsize = kj_stream_buffered(s);
  if (bsize == 0)
    return 0;
  buf = in = buf_start(s);

  for (;;) {
    uint8_t tag;
    unsigned i;
    if (REMAINING < 10) {
      if (out >= out_min) {
        wrapper_skip(s, (size_t)(in - buf));
        *progress = (size_t)(out - dst);
        return (size_t)(out - dst);
      }
      if (REMAINING == 0) {
        REFRESH();
        continue;
      }
      tag = *in++;
      for (i = 0; i < 8; i++) {
        if (tag & (1u << i)) {
          if (REMAINING == 0)
            REFRESH();
          *out++ = *in++;
        } else {
          *out++ = 0;
        }
      }
      if (REMAINING == 0 && (tag == 0 || tag == 0xff))
        REFRESH();
    } else {
      tag = *in++;
      for (i = 0; i < 8; i++) {
        int nz = (tag & (1u << i)) != 0;
        *out++ = nz ? *in : 0;
        in += nz;
      }
    }

    if (tag == 0) {
      size_t run = (size_t)*in++ * 8u;
      /* "Packed input did not end cleanly": kj returns without skipping
       * what this buffer gave, so the stream rereads it. */
      if (run > (size_t)(out_end - out)) {
        s->soft = 1;
        *progress = (size_t)(out - dst);
        return (size_t)(out - dst);
      }
      memset(out, 0, run);
      out += run;
    } else if (tag == 0xff) {
      size_t run = (size_t)*in++ * 8u, in_rem;
      if (run > (size_t)(out_end - out)) {
        s->soft = 1;
        *progress = (size_t)(out - dst);
        return (size_t)(out - dst);
      }
      in_rem = REMAINING;
      if (in_rem >= run) {
        memcpy(out, in, run);
        out += run;
        in += run;
      } else {
        memcpy(out, in, in_rem);
        out += in_rem;
        run -= in_rem;
        wrapper_skip(s, bsize);
        wrapper_read(s, out, run, run);
        out += run;
        if (out == out_end) {
          *progress = max;
          return max;
        }
        bsize = get_buffer(s);
        if (s->fatal) {
          *progress = (size_t)(out - dst);
          return 0;
        }
        buf = in = buf_start(s);
        continue;
      }
    }

    if (out == out_end) {
      wrapper_skip(s, (size_t)(in - buf));
      *progress = max;
      return max;
    }
  }
#undef REFRESH
#undef REMAINING
}

size_t kj_stream_packed_try_read(kj_stream_t *s, uint8_t *dst, size_t min,
                                 size_t max) {
  size_t progress;
  return packed_try_read(s, dst, min, max, &progress);
}

size_t kj_stream_read(kj_stream_t *s, int packed, uint8_t *dst, size_t min,
                      size_t max, size_t *progress) {
  size_t n, reached;
  if (packed) {
    n = packed_try_read(s, dst, min, max, &reached);
    if (s->fatal) {
      if (progress)
        *progress = reached;
      return 0;
    }
  } else {
    n = wrapper_try_read(s, dst, min, max);
  }
  if (progress)
    *progress = n;
  if (n < min) {
    s->soft = 1;
    return min;
  }
  return n;
}
