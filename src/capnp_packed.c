/* SPDX-License-Identifier: MIT */
#include <capnp-janet/capnp_packed.h>

#include <stdlib.h>
#include <string.h>

static int word_all_zero(const uint8_t *w) {
  return w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0 &&
         w[5] == 0 && w[6] == 0 && w[7] == 0;
}

static int word_no_zero_byte(const uint8_t *w) {
  return w[0] && w[1] && w[2] && w[3] && w[4] && w[5] && w[6] && w[7];
}

int capnp_pack(const uint8_t *in, size_t in_len, uint8_t **out,
               size_t *out_len) {
  size_t nwords, w, opos, cap, run, k;
  uint8_t *buf = NULL;
  uint8_t tag;
  int nz;

  if (!in || !out || !out_len)
    return CAPNP_ERR_ARG;
  if (in_len % 8 != 0)
    return CAPNP_ERR_ARG;
  *out = NULL;
  *out_len = 0;
  nwords = in_len / 8;
  /* Worst case ~10 bytes/word */
  cap = nwords ? nwords * 10 : 1;
  buf = (uint8_t *)malloc(cap);
  if (!buf)
    return CAPNP_ERR_ALLOC;
  opos = 0;
  w = 0;
  while (w < nwords) {
    const uint8_t *word = in + w * 8;
    if (opos + 10 + 8 * 255 > cap) {
      size_t ncap = cap * 2 + 8 * 256;
      uint8_t *nb = (uint8_t *)realloc(buf, ncap);
      if (!nb) {
        free(buf);
        return CAPNP_ERR_ALLOC;
      }
      buf = nb;
      cap = ncap;
    }
    tag = 0;
    nz = 0;
    for (k = 0; k < 8; k++) {
      if (word[k]) {
        tag = (uint8_t)(tag | (1u << k));
        nz++;
      }
    }
    buf[opos++] = tag;
    for (k = 0; k < 8; k++) {
      if (tag & (1u << k))
        buf[opos++] = word[k];
    }
    w++;
    if (tag == 0) {
      run = 0;
      while (w + run < nwords && run < 255 &&
             word_all_zero(in + (w + run) * 8))
        run++;
      buf[opos++] = (uint8_t)run;
      w += run;
    } else if (nz == 8) {
      /* C++ heuristic: following words with no zero byte, up to 255 */
      run = 0;
      while (w + run < nwords && run < 255 &&
             word_no_zero_byte(in + (w + run) * 8))
        run++;
      buf[opos++] = (uint8_t)run;
      if (run) {
        memcpy(buf + opos, in + w * 8, run * 8);
        opos += run * 8;
        w += run;
      }
    }
  }
  *out = buf;
  *out_len = opos;
  return CAPNP_OK;
}

int capnp_unpack(const uint8_t *in, size_t in_len, uint8_t **out,
                 size_t *out_len) {
  size_t ipos, opos, cap, k;
  uint8_t *buf = NULL;
  uint8_t tag, cnt;

  if (!in || !out || !out_len)
    return CAPNP_ERR_ARG;
  *out = NULL;
  *out_len = 0;
  /* Estimate: packed is usually smaller; grow as needed */
  cap = in_len * 4 + 64;
  if (cap < 64)
    cap = 64;
  buf = (uint8_t *)malloc(cap);
  if (!buf)
    return CAPNP_ERR_ALLOC;
  memset(buf, 0, cap);
  ipos = 0;
  opos = 0;
  while (ipos < in_len) {
    if (opos + 8 + 255 * 8 > cap) {
      size_t ncap = cap * 2 + 8 * 256;
      uint8_t *nb = (uint8_t *)realloc(buf, ncap);
      if (!nb) {
        free(buf);
        return CAPNP_ERR_ALLOC;
      }
      memset(nb + cap, 0, ncap - cap);
      buf = nb;
      cap = ncap;
    }
    tag = in[ipos++];
    for (k = 0; k < 8; k++) {
      if (tag & (1u << k)) {
        if (ipos >= in_len) {
          free(buf);
          return CAPNP_ERR_PACKED;
        }
        buf[opos + k] = in[ipos++];
      } else {
        buf[opos + k] = 0;
      }
    }
    opos += 8;
    if (tag == 0) {
      if (ipos >= in_len) {
        free(buf);
        return CAPNP_ERR_PACKED;
      }
      cnt = in[ipos++];
      if (opos + (size_t)cnt * 8 > cap) {
        size_t ncap = opos + (size_t)cnt * 8 + 64;
        uint8_t *nb = (uint8_t *)realloc(buf, ncap);
        if (!nb) {
          free(buf);
          return CAPNP_ERR_PACKED;
        }
        memset(nb + cap, 0, ncap - cap);
        buf = nb;
        cap = ncap;
      }
      memset(buf + opos, 0, (size_t)cnt * 8);
      opos += (size_t)cnt * 8;
    } else if (tag == 0xff) {
      if (ipos >= in_len) {
        free(buf);
        return CAPNP_ERR_PACKED;
      }
      cnt = in[ipos++];
      if (ipos + (size_t)cnt * 8 > in_len) {
        free(buf);
        return CAPNP_ERR_PACKED;
      }
      if (opos + (size_t)cnt * 8 > cap) {
        size_t ncap = opos + (size_t)cnt * 8 + 64;
        uint8_t *nb = (uint8_t *)realloc(buf, ncap);
        if (!nb) {
          free(buf);
          return CAPNP_ERR_PACKED;
        }
        memset(nb + cap, 0, ncap - cap);
        buf = nb;
        cap = ncap;
      }
      if (cnt) {
        memcpy(buf + opos, in + ipos, (size_t)cnt * 8);
        ipos += (size_t)cnt * 8;
        opos += (size_t)cnt * 8;
      }
    }
  }
  if (opos % 8 != 0) {
    free(buf);
    return CAPNP_ERR_PACKED;
  }
  *out = buf;
  *out_len = opos;
  return CAPNP_OK;
}
