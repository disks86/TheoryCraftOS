/* The decoded surfaces of tests/windows/hevcrext_dxva.h's streams taken apart into the planar pictures their FNV hashes are of (d3d12videotest.c, d3d11videotest.c):
 * the luma plane and, unless monochrome, the two chroma planes of the picture's own chroma format, each sample a byte (8 bits) or a 16-bit little-endian word
 * holding the value in its low bits. `p0` holds the surface's first plane read back with rows of tight bytes (the packed formats AYUV, Y410, Y416, YUY2, Y210, Y216
 * are that plane alone), `p1` the CbCr plane of NV12 / P010 / P016. Returns the bytes written to `out`, and counts in *bad the samples with something below their bits
 * (the formats hold a sample in the top bits of its field), alpha that is not opaque, and monochrome chroma that is not neutral. */
#pragma once

static unsigned hevcrext_word(const BYTE *q) { return (unsigned)q[0] | ((unsigned)q[1] << 8); }

static size_t hevcrext_unpack(DXGI_FORMAT format, unsigned chroma, unsigned depth, unsigned w, unsigned h, const BYTE *p0, const BYTE *p1, BYTE *out, unsigned *bad) {
  const unsigned cw = chroma == 3 ? w : w / 2, ch = chroma >= 2 ? h : h / 2, bytes = depth > 8 ? 2 : 1;
  BYTE *planes[3] = {out, out + (size_t)w * h * bytes, out + ((size_t)w * h + (size_t)cw * ch) * bytes};
  /* a field of `bits` bits with the sample in its top bits */
#define HEVCREXT_TAKE(field, bits) (((field) & ((1u << ((bits) - depth)) - 1u)) ? (++*bad, (field) >> ((bits) - depth)) : (field) >> ((bits) - depth))
#define HEVCREXT_PUT(c, x, y, v)                                                                                                   \
  do {                                                                                                                             \
    const unsigned stride_ = (c) ? cw : w, v_ = (v);                                                                               \
    if (bytes == 2) { planes[c][((size_t)(y) * stride_ + (x)) * 2] = (BYTE)v_; planes[c][((size_t)(y) * stride_ + (x)) * 2 + 1] = (BYTE)(v_ >> 8); } \
    else planes[c][(size_t)(y) * stride_ + (x)] = (BYTE)v_;                                                                        \
  } while (0)
  switch (format) {
    case DXGI_FORMAT_AYUV:
      for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
          const BYTE *q = p0 + ((size_t)y * w + x) * 4;
          HEVCREXT_PUT(2, x, y, HEVCREXT_TAKE((unsigned)q[0], 8));
          HEVCREXT_PUT(1, x, y, HEVCREXT_TAKE((unsigned)q[1], 8));
          HEVCREXT_PUT(0, x, y, HEVCREXT_TAKE((unsigned)q[2], 8));
          if (q[3] != 0xFF) ++*bad;
        }
      break;
    case DXGI_FORMAT_Y410:
      for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
          const BYTE *q = p0 + ((size_t)y * w + x) * 4;
          const unsigned v = hevcrext_word(q) | (hevcrext_word(q + 2) << 16);
          HEVCREXT_PUT(1, x, y, HEVCREXT_TAKE(v & 0x3FFu, 10));
          HEVCREXT_PUT(0, x, y, HEVCREXT_TAKE((v >> 10) & 0x3FFu, 10));
          HEVCREXT_PUT(2, x, y, HEVCREXT_TAKE((v >> 20) & 0x3FFu, 10));
          if ((v >> 30) != 3) ++*bad;
        }
      break;
    case DXGI_FORMAT_Y416:
      for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
          const BYTE *q = p0 + ((size_t)y * w + x) * 8;
          HEVCREXT_PUT(1, x, y, HEVCREXT_TAKE(hevcrext_word(q), 16));
          HEVCREXT_PUT(0, x, y, HEVCREXT_TAKE(hevcrext_word(q + 2), 16));
          HEVCREXT_PUT(2, x, y, HEVCREXT_TAKE(hevcrext_word(q + 4), 16));
          if (hevcrext_word(q + 6) != 0xFFFF) ++*bad;
        }
      break;
    case DXGI_FORMAT_YUY2:
    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216: {
      const unsigned pair = format == DXGI_FORMAT_YUY2 ? 4 : 8, bits = format == DXGI_FORMAT_YUY2 ? 8 : 16;
      for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; x += 2) {
          const BYTE *q = p0 + ((size_t)y * ((w + 1) / 2) + x / 2) * pair;
          unsigned v[4];
          for (int k = 0; k < 4; ++k) v[k] = bits == 8 ? q[k] : hevcrext_word(q + 2 * k);
          HEVCREXT_PUT(0, x, y, HEVCREXT_TAKE(v[0], bits));
          HEVCREXT_PUT(1, x / 2, y, HEVCREXT_TAKE(v[1], bits));
          if (x + 1 < w) HEVCREXT_PUT(0, x + 1, y, HEVCREXT_TAKE(v[2], bits));
          HEVCREXT_PUT(2, x / 2, y, HEVCREXT_TAKE(v[3], bits));
        }
      break;
    }
    default: {   /* NV12, P010, P016 */
      const unsigned bits = format == DXGI_FORMAT_NV12 ? 8 : 16, sb = bits / 8;
      for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
          const BYTE *q = p0 + ((size_t)y * w + x) * sb;
          HEVCREXT_PUT(0, x, y, HEVCREXT_TAKE(sb == 2 ? hevcrext_word(q) : q[0], bits));
        }
      for (unsigned y = 0; y < h / 2; ++y)
        for (unsigned x = 0; x < w / 2; ++x) {
          const BYTE *q = p1 + ((size_t)y * (w / 2) + x) * 2 * sb;
          const unsigned u = HEVCREXT_TAKE(sb == 2 ? hevcrext_word(q) : q[0], bits), v = HEVCREXT_TAKE(sb == 2 ? hevcrext_word(q + 2) : q[1], bits);
          if (chroma == 0) {
            if (u != 1u << (depth - 1) || v != 1u << (depth - 1)) ++*bad;
          } else {
            HEVCREXT_PUT(1, x, y, u);
            HEVCREXT_PUT(2, x, y, v);
          }
        }
      break;
    }
  }
#undef HEVCREXT_TAKE
#undef HEVCREXT_PUT
  return ((size_t)w * h + (chroma ? 2 * (size_t)cw * ch : 0)) * bytes;
}
