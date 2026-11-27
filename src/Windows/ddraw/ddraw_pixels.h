/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* The pixels of DirectDraw surfaces (ddraw.dll, docs/DirectDraw.md): what the formats are, how big a surface of one is, and the blitter - colour and
 * depth fills, copies with stretching, mirroring and rotation, source and destination colour keys, conversion between the formats, and the
 * conversion of a rectangle to A8R8G8B8 (with the colour key as alpha) that Direct3D and the presentation of the primary surface use.
 *
 * It needs no library: ddraw.dll has no C runtime, and the host test (tests/DdrawPixelsTest.cpp) includes it as it is. Every access is checked
 * against the size of the image's memory (DdImage.size): an image whose rows do not fit is refused (dd_image_check), a rectangle that leaves the
 * image is refused, so nothing here reads or writes outside the bytes it was given.
 *
 * Formats (DD_FMT_*): palette indices of 1, 2, 4 and 8 bits; RGB of 8 (3-3-2), 16 (5-6-5, x1-5-5-5, 1-5-5-5, x4-4-4-4, 4-4-4-4, a8-3-3-2), 24 and 32
 * bits (x8r8g8b8, a8r8g8b8, x8b8g8r8, a8b8g8r8) and the 10-bit 2-10-10-10 forms; alpha (A8); luminance (L8, A8L8, A4L4, L16); bump maps (V8U8,
 * L6V5U5, X8L8V8U8, Q8W8V8U8, V16U16); depth with DirectDraw's bit layouts (D16; D15S1 with the stencil bit on top; D24X8 and D24S8/D24X4S4 with the
 * depth in the low 24 bits and the stencil above; D24X8H with the depth in the high 24 bits; D32); the
 * packed YUV FourCCs YUY2 and UYVY, the planar YV12 and NV12, and the block-compressed DXT1 .. DXT5. A pixel of a packed format is its raw value: the
 * bytes of the pixel read little-endian (`raw`), which is what colour keys and DDBLTFX.dwFillColor are made of. */
#ifndef TCOS_DDRAW_PIXELS_H
#define TCOS_DDRAW_PIXELS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  DD_FMT_UNKNOWN = 0,
  DD_FMT_P1, DD_FMT_P2, DD_FMT_P4, DD_FMT_P8,
  DD_FMT_R3G3B2, DD_FMT_A8R3G3B2,
  DD_FMT_R5G6B5, DD_FMT_X1R5G5B5, DD_FMT_A1R5G5B5, DD_FMT_X4R4G4B4, DD_FMT_A4R4G4B4,
  DD_FMT_R8G8B8, DD_FMT_X8R8G8B8, DD_FMT_A8R8G8B8, DD_FMT_X8B8G8R8, DD_FMT_A8B8G8R8,
  DD_FMT_A2R10G10B10, DD_FMT_A2B10G10R10,
  DD_FMT_A8, DD_FMT_L8, DD_FMT_A8L8, DD_FMT_A4L4, DD_FMT_L16,
  DD_FMT_V8U8, DD_FMT_L6V5U5, DD_FMT_X8L8V8U8, DD_FMT_Q8W8V8U8, DD_FMT_V16U16,
  DD_FMT_D16, DD_FMT_D15S1, DD_FMT_D24X8, DD_FMT_D24X8H, DD_FMT_D24S8, DD_FMT_D24X4S4, DD_FMT_D32,
  DD_FMT_YUY2, DD_FMT_UYVY, DD_FMT_YV12, DD_FMT_NV12,
  DD_FMT_DXT1, DD_FMT_DXT2, DD_FMT_DXT3, DD_FMT_DXT4, DD_FMT_DXT5,
  DD_FMT_COUNT
};

/* What kind of pixels a format has. */
enum {
  DD_KIND_NONE = 0,
  DD_KIND_PALETTE,      /* indices into a palette (mask r: the index bits) */
  DD_KIND_RGB,          /* red, green, blue (and alpha) in masks */
  DD_KIND_ALPHA,        /* alpha only (mask a) */
  DD_KIND_LUMINANCE,    /* luminance in mask r (and alpha) */
  DD_KIND_BUMP,         /* signed du in r, dv in g, unsigned luminance in b (and w in a for Q8W8V8U8) */
  DD_KIND_DEPTH,        /* depth in mask r, stencil in mask g */
  DD_KIND_YUV_PACKED,   /* two pixels in four bytes (YUY2: Y0 U Y1 V; UYVY: U Y0 V Y1) */
  DD_KIND_YUV_PLANAR,   /* a luminance plane of `width` bytes a row, then the chroma at half resolution */
  DD_KIND_BLOCKS        /* 4x4 blocks of block_bytes bytes */
};

typedef struct DdFormatInfo {
  uint8_t kind;
  uint8_t bits;          /* bits a pixel (YV12/NV12: of the luminance plane, 8; blocks: 0) */
  uint8_t bytes;         /* bytes a pixel (0: less than a byte, a planar or block format) */
  uint8_t block_bytes;   /* bytes a 4x4 block (DXT) */
  uint32_t r, g, b, a;   /* the masks of the raw value (see DD_KIND_*) */
  uint32_t fourcc;       /* the FourCC code of a FourCC format */
} DdFormatInfo;

#define DD_FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

static const DdFormatInfo dd_formats[DD_FMT_COUNT] = {
  /* UNKNOWN  */ {DD_KIND_NONE, 0, 0, 0, 0, 0, 0, 0, 0},
  /* P1       */ {DD_KIND_PALETTE, 1, 0, 0, 0x1, 0, 0, 0, 0},
  /* P2       */ {DD_KIND_PALETTE, 2, 0, 0, 0x3, 0, 0, 0, 0},
  /* P4       */ {DD_KIND_PALETTE, 4, 0, 0, 0xF, 0, 0, 0, 0},
  /* P8       */ {DD_KIND_PALETTE, 8, 1, 0, 0xFF, 0, 0, 0, 0},
  /* R3G3B2   */ {DD_KIND_RGB, 8, 1, 0, 0xE0, 0x1C, 0x03, 0, 0},
  /* A8R3G3B2 */ {DD_KIND_RGB, 16, 2, 0, 0xE0, 0x1C, 0x03, 0xFF00, 0},
  /* R5G6B5   */ {DD_KIND_RGB, 16, 2, 0, 0xF800, 0x07E0, 0x001F, 0, 0},
  /* X1R5G5B5 */ {DD_KIND_RGB, 16, 2, 0, 0x7C00, 0x03E0, 0x001F, 0, 0},
  /* A1R5G5B5 */ {DD_KIND_RGB, 16, 2, 0, 0x7C00, 0x03E0, 0x001F, 0x8000, 0},
  /* X4R4G4B4 */ {DD_KIND_RGB, 16, 2, 0, 0x0F00, 0x00F0, 0x000F, 0, 0},
  /* A4R4G4B4 */ {DD_KIND_RGB, 16, 2, 0, 0x0F00, 0x00F0, 0x000F, 0xF000, 0},
  /* R8G8B8   */ {DD_KIND_RGB, 24, 3, 0, 0xFF0000, 0xFF00, 0xFF, 0, 0},
  /* X8R8G8B8 */ {DD_KIND_RGB, 32, 4, 0, 0xFF0000, 0xFF00, 0xFF, 0, 0},
  /* A8R8G8B8 */ {DD_KIND_RGB, 32, 4, 0, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u, 0},
  /* X8B8G8R8 */ {DD_KIND_RGB, 32, 4, 0, 0xFF, 0xFF00, 0xFF0000, 0, 0},
  /* A8B8G8R8 */ {DD_KIND_RGB, 32, 4, 0, 0xFF, 0xFF00, 0xFF0000, 0xFF000000u, 0},
  /* A2R10G10B10 */ {DD_KIND_RGB, 32, 4, 0, 0x3FF00000u, 0xFFC00, 0x3FF, 0xC0000000u, 0},
  /* A2B10G10R10 */ {DD_KIND_RGB, 32, 4, 0, 0x3FF, 0xFFC00, 0x3FF00000u, 0xC0000000u, 0},
  /* A8       */ {DD_KIND_ALPHA, 8, 1, 0, 0, 0, 0, 0xFF, 0},
  /* L8       */ {DD_KIND_LUMINANCE, 8, 1, 0, 0xFF, 0, 0, 0, 0},
  /* A8L8     */ {DD_KIND_LUMINANCE, 16, 2, 0, 0xFF, 0, 0, 0xFF00, 0},
  /* A4L4     */ {DD_KIND_LUMINANCE, 8, 1, 0, 0x0F, 0, 0, 0xF0, 0},
  /* L16      */ {DD_KIND_LUMINANCE, 16, 2, 0, 0xFFFF, 0, 0, 0, 0},
  /* V8U8     */ {DD_KIND_BUMP, 16, 2, 0, 0xFF, 0xFF00, 0, 0, 0},
  /* L6V5U5   */ {DD_KIND_BUMP, 16, 2, 0, 0x1F, 0x3E0, 0xFC00, 0, 0},
  /* X8L8V8U8 */ {DD_KIND_BUMP, 32, 4, 0, 0xFF, 0xFF00, 0xFF0000, 0, 0},
  /* Q8W8V8U8 */ {DD_KIND_BUMP, 32, 4, 0, 0xFF, 0xFF00, 0xFF0000, 0xFF000000u, 0},
  /* V16U16   */ {DD_KIND_BUMP, 32, 4, 0, 0xFFFF, 0xFFFF0000u, 0, 0, 0},
  /* D16      */ {DD_KIND_DEPTH, 16, 2, 0, 0xFFFF, 0, 0, 0, 0},
  /* D15S1    */ {DD_KIND_DEPTH, 16, 2, 0, 0x7FFF, 0x8000, 0, 0, 0},
  /* D24X8    */ {DD_KIND_DEPTH, 32, 4, 0, 0x00FFFFFFu, 0, 0, 0, 0},
  /* D24X8H   */ {DD_KIND_DEPTH, 32, 4, 0, 0xFFFFFF00u, 0, 0, 0, 0},
  /* D24S8    */ {DD_KIND_DEPTH, 32, 4, 0, 0x00FFFFFFu, 0xFF000000u, 0, 0, 0},
  /* D24X4S4  */ {DD_KIND_DEPTH, 32, 4, 0, 0x00FFFFFFu, 0x0F000000u, 0, 0, 0},
  /* D32      */ {DD_KIND_DEPTH, 32, 4, 0, 0xFFFFFFFFu, 0, 0, 0, 0},
  /* YUY2     */ {DD_KIND_YUV_PACKED, 16, 2, 0, 0, 0, 0, 0, DD_FOURCC('Y', 'U', 'Y', '2')},
  /* UYVY     */ {DD_KIND_YUV_PACKED, 16, 2, 0, 0, 0, 0, 0, DD_FOURCC('U', 'Y', 'V', 'Y')},
  /* YV12     */ {DD_KIND_YUV_PLANAR, 8, 0, 0, 0, 0, 0, 0, DD_FOURCC('Y', 'V', '1', '2')},
  /* NV12     */ {DD_KIND_YUV_PLANAR, 8, 0, 0, 0, 0, 0, 0, DD_FOURCC('N', 'V', '1', '2')},
  /* DXT1     */ {DD_KIND_BLOCKS, 0, 0, 8, 0, 0, 0, 0, DD_FOURCC('D', 'X', 'T', '1')},
  /* DXT2     */ {DD_KIND_BLOCKS, 0, 0, 16, 0, 0, 0, 0, DD_FOURCC('D', 'X', 'T', '2')},
  /* DXT3     */ {DD_KIND_BLOCKS, 0, 0, 16, 0, 0, 0, 0, DD_FOURCC('D', 'X', 'T', '3')},
  /* DXT4     */ {DD_KIND_BLOCKS, 0, 0, 16, 0, 0, 0, 0, DD_FOURCC('D', 'X', 'T', '4')},
  /* DXT5     */ {DD_KIND_BLOCKS, 0, 0, 16, 0, 0, 0, 0, DD_FOURCC('D', 'X', 'T', '5')},
};

static inline const DdFormatInfo *dd_format(uint32_t fmt) { return &dd_formats[fmt < (uint32_t)DD_FMT_COUNT ? fmt : (uint32_t)DD_FMT_UNKNOWN]; }
static inline int dd_format_is_palette(uint32_t fmt) { return dd_format(fmt)->kind == DD_KIND_PALETTE; }
static inline int dd_format_is_depth(uint32_t fmt) { return dd_format(fmt)->kind == DD_KIND_DEPTH; }
static inline int dd_format_is_blocks(uint32_t fmt) { return dd_format(fmt)->kind == DD_KIND_BLOCKS; }
static inline int dd_format_is_yuv(uint32_t fmt) { const uint8_t k = dd_format(fmt)->kind; return k == DD_KIND_YUV_PACKED || k == DD_KIND_YUV_PLANAR; }
/* Pixels addressed by bytes (1 .. 4 a pixel): every RGB, alpha, luminance, bump, depth and P8 format, and the packed YUV formats (2 bytes a pixel,
 * a pair sharing its chroma). */
static inline int dd_format_is_bytewise(uint32_t fmt) { return dd_format(fmt)->bytes != 0; }
/* The formats whose pixels have a colour (dd_raw_to_argb can read them). */
static inline int dd_format_has_colour(uint32_t fmt) {
  const uint8_t k = dd_format(fmt)->kind;
  return k == DD_KIND_PALETTE || k == DD_KIND_RGB || k == DD_KIND_ALPHA || k == DD_KIND_LUMINANCE || k == DD_KIND_YUV_PACKED || k == DD_KIND_YUV_PLANAR || k == DD_KIND_BLOCKS;
}

/* The bits of a raw value that make up the colour (the colour key compares these): red, green and blue of an RGB format, the index of a palette
 * format, the luminance, the alpha of an alpha format, every bit of the others. */
static inline uint32_t dd_colour_mask(uint32_t fmt) {
  const DdFormatInfo *f = dd_format(fmt);
  switch (f->kind) {
    case DD_KIND_PALETTE: return f->r;
    case DD_KIND_RGB: return f->r | f->g | f->b;
    case DD_KIND_LUMINANCE: return f->r;
    case DD_KIND_ALPHA: return f->a;
    default: return f->bytes >= 4 ? 0xFFFFFFFFu : ((uint32_t)1 << (8u * f->bytes)) - 1u;
  }
}

/* ---- sizes ----------------------------------------------------------------------------------------------------------------------------------------- */

/* The smallest pitch of a row of `width` pixels (blocks: of a row of blocks; planar: of the luminance plane); 0 if it would not fit 32 bits. */
static inline uint32_t dd_min_pitch(uint32_t fmt, uint32_t width) {
  const DdFormatInfo *f = dd_format(fmt);
  uint64_t n;
  if (f->kind == DD_KIND_BLOCKS) n = (((uint64_t)width + 3u) / 4u) * f->block_bytes;
  else if (f->kind == DD_KIND_YUV_PLANAR) n = width;
  else if (f->kind == DD_KIND_YUV_PACKED) n = (((uint64_t)width + 1u) & ~(uint64_t)1u) * 2u;
  else if (f->bits) n = ((uint64_t)width * f->bits + 7u) / 8u;
  else return 0;
  return n > 0x7FFFFFFFu ? 0 : (uint32_t)n;
}
/* That pitch rounded up to `align` (a power of two). */
static inline uint32_t dd_aligned_pitch(uint32_t fmt, uint32_t width, uint32_t align) {
  const uint64_t p = dd_min_pitch(fmt, width);
  if (!p || !align || (align & (align - 1))) return 0;
  const uint64_t a = (p + align - 1u) & ~(uint64_t)(align - 1u);
  return a > 0x7FFFFFFFu ? 0 : (uint32_t)a;
}
/* The rows a surface has in its first plane (blocks: rows of blocks). */
static inline uint32_t dd_rows(uint32_t fmt, uint32_t height) { return dd_format_is_blocks(fmt) ? (height + 3u) / 4u : height; }
/* The bytes a surface of `pitch` takes (every plane); 0 for an unknown format or a size beyond `limit`. */
static inline uint64_t dd_image_bytes(uint32_t fmt, uint32_t width, uint32_t height, uint32_t pitch) {
  const DdFormatInfo *f = dd_format(fmt);
  const uint32_t least = dd_min_pitch(fmt, width);      /* (0: a row too wide for 31 bits) */
  if (f->kind == DD_KIND_NONE || !width || !height || !least || pitch < least) return 0;
  uint64_t n = (uint64_t)pitch * dd_rows(fmt, height);
  if (f->kind == DD_KIND_YUV_PLANAR) {
    /* YV12: V then U planes of (pitch / 2) x (height / 2); NV12: one plane of interleaved U and V of pitch x (height / 2) */
    const uint64_t ch = ((uint64_t)height + 1u) / 2u;
    n += fmt == DD_FMT_NV12 ? (uint64_t)pitch * ch : 2u * ((uint64_t)pitch / 2u) * ch;
  }
  return n;
}

typedef struct DdImage {
  uint8_t *data;              /* the first byte of the surface */
  size_t size;                /* how many bytes from `data` are the surface's */
  uint32_t width, height;
  uint32_t pitch;             /* bytes from a row to the next (blocks: a row of blocks) */
  uint32_t fmt;
  const uint32_t *palette;    /* A8R8G8B8 entries of a palette format (NULL: none, which reads as black) */
  uint32_t palette_count;
} DdImage;

typedef struct DdRect { int32_t left, top, right, bottom; } DdRect;

/* Do the image's rows (all its planes) fit in its memory? */
static inline int dd_image_check(const DdImage *im) {
  if (!im || !im->data || dd_format(im->fmt)->kind == DD_KIND_NONE) return 0;
  const uint64_t n = dd_image_bytes(im->fmt, im->width, im->height, im->pitch);
  return n != 0 && n <= im->size;
}
/* Is the rectangle inside the image and not empty? */
static inline int dd_rect_inside(const DdImage *im, const DdRect *r) {
  return r->left >= 0 && r->top >= 0 && r->left < r->right && r->top < r->bottom && (uint32_t)r->right <= im->width && (uint32_t)r->bottom <= im->height;
}

/* ---- raw pixels ------------------------------------------------------------------------------------------------------------------------------------ */

/* The raw value of the pixel at (x, y) of an image dd_image_check accepted, for a byte-addressed or palette format (x, y inside). A packed YUV pixel is
 * its 2 bytes. */
static inline uint32_t dd_get_raw(const DdImage *im, uint32_t x, uint32_t y) {
  const DdFormatInfo *f = dd_format(im->fmt);
  const uint8_t *row = im->data + (size_t)y * im->pitch;
  if (f->bytes) {
    const uint8_t *p = row + (size_t)x * f->bytes;
    uint32_t v = 0;
    for (uint32_t i = 0; i < f->bytes; ++i) v |= (uint32_t)p[i] << (8u * i);
    return v;
  }
  if (f->kind == DD_KIND_PALETTE) {      /* (1, 2, 4 bits: the leftmost pixel in the high bits of its byte, as in a DIB) */
    const uint32_t bit = x * f->bits;
    const uint32_t shift = 8u - f->bits - (bit & 7u);
    return (row[bit >> 3] >> shift) & f->r;
  }
  return 0;
}
static inline void dd_put_raw(DdImage *im, uint32_t x, uint32_t y, uint32_t v) {
  const DdFormatInfo *f = dd_format(im->fmt);
  uint8_t *row = im->data + (size_t)y * im->pitch;
  if (f->bytes) {
    uint8_t *p = row + (size_t)x * f->bytes;
    for (uint32_t i = 0; i < f->bytes; ++i) p[i] = (uint8_t)(v >> (8u * i));
    return;
  }
  if (f->kind == DD_KIND_PALETTE) {
    const uint32_t bit = x * f->bits;
    const uint32_t shift = 8u - f->bits - (bit & 7u);
    uint8_t *b = &row[bit >> 3];
    *b = (uint8_t)((*b & ~(f->r << shift)) | ((v & f->r) << shift));
  }
}

/* ---- colours --------------------------------------------------------------------------------------------------------------------------------------- */

static inline uint32_t dd_mask_shift(uint32_t mask) { uint32_t s = 0; if (!mask) return 0; while (!(mask & 1u)) { mask >>= 1; ++s; } return s; }
static inline uint32_t dd_mask_max(uint32_t mask) { return mask ? mask >> dd_mask_shift(mask) : 0; }
/* A component of a raw value as 8 bits (rounded); `none` if the format has no such component. */
static inline uint32_t dd_component8(uint32_t raw, uint32_t mask, uint32_t none) {
  if (!mask) return none;
  const uint32_t max = dd_mask_max(mask), v = (raw & mask) >> dd_mask_shift(mask);
  return (uint32_t)(((uint64_t)v * 255u + max / 2u) / max);
}
/* ... and an 8-bit component into its place. */
static inline uint32_t dd_component_from8(uint32_t c8, uint32_t mask) {
  if (!mask) return 0;
  const uint32_t max = dd_mask_max(mask);
  return ((uint32_t)(((uint64_t)(c8 & 0xFFu) * max + 127u) / 255u) << dd_mask_shift(mask)) & mask;
}
static inline uint8_t dd_clamp8(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
/* A colour of BT.601 YUV (studio range) as A8R8G8B8. */
static inline uint32_t dd_yuv_to_argb(uint32_t y, uint32_t u, uint32_t v) {
  const int32_t c = (int32_t)y - 16, d = (int32_t)u - 128, e = (int32_t)v - 128;
  const uint32_t r = dd_clamp8((298 * c + 409 * e + 128) >> 8);
  const uint32_t g = dd_clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
  const uint32_t b = dd_clamp8((298 * c + 516 * d + 128) >> 8);
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}
static inline void dd_argb_to_yuv(uint32_t argb, uint32_t *y, uint32_t *u, uint32_t *v) {
  const int32_t r = (int32_t)((argb >> 16) & 0xFF), g = (int32_t)((argb >> 8) & 0xFF), b = (int32_t)(argb & 0xFF);
  *y = dd_clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
  *u = dd_clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
  *v = dd_clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

/* The colour of a raw value of a palette, RGB, alpha or luminance format as A8R8G8B8 (a format without alpha reads as opaque; an alpha format as
 * black with its alpha). Palette entries beyond the palette read as black. */
static inline uint32_t dd_raw_to_argb(const DdImage *im, uint32_t raw) {
  const DdFormatInfo *f = dd_format(im->fmt);
  switch (f->kind) {
    case DD_KIND_PALETTE: {
      const uint32_t i = raw & f->r;
      return im->palette && i < im->palette_count ? im->palette[i] : 0xFF000000u;
    }
    case DD_KIND_RGB:
      return (dd_component8(raw, f->a, 0xFF) << 24) | (dd_component8(raw, f->r, 0) << 16) | (dd_component8(raw, f->g, 0) << 8) | dd_component8(raw, f->b, 0);
    case DD_KIND_ALPHA: return dd_component8(raw, f->a, 0xFF) << 24;
    case DD_KIND_LUMINANCE: {
      const uint32_t l = dd_component8(raw, f->r, 0);
      return (dd_component8(raw, f->a, 0xFF) << 24) | (l << 16) | (l << 8) | l;
    }
    default: return 0xFF000000u;
  }
}
/* The palette index nearest to a colour (the first of equals). */
static inline uint32_t dd_nearest_index(const DdImage *im, uint32_t argb) {
  const uint32_t limit = 1u << dd_format(im->fmt)->bits;
  const uint32_t n = im->palette ? (im->palette_count < limit ? im->palette_count : limit) : 0;
  uint32_t best = 0;
  uint64_t best_d = ~(uint64_t)0;
  for (uint32_t i = 0; i < n; ++i) {
    const int32_t dr = (int32_t)((im->palette[i] >> 16) & 0xFF) - (int32_t)((argb >> 16) & 0xFF);
    const int32_t dg = (int32_t)((im->palette[i] >> 8) & 0xFF) - (int32_t)((argb >> 8) & 0xFF);
    const int32_t db = (int32_t)(im->palette[i] & 0xFF) - (int32_t)(argb & 0xFF);
    const uint64_t d = (uint64_t)(dr * dr) + (uint64_t)(dg * dg) + (uint64_t)(db * db);
    if (d < best_d) { best_d = d; best = i; if (!d) break; }
  }
  return best;
}
/* An A8R8G8B8 colour as the raw value of a palette, RGB, alpha or luminance format. */
static inline uint32_t dd_argb_to_raw(const DdImage *im, uint32_t argb) {
  const DdFormatInfo *f = dd_format(im->fmt);
  switch (f->kind) {
    case DD_KIND_PALETTE: return dd_nearest_index(im, argb);
    case DD_KIND_RGB:
      return dd_component_from8(argb >> 24, f->a) | dd_component_from8(argb >> 16, f->r) | dd_component_from8(argb >> 8, f->g) | dd_component_from8(argb, f->b);
    case DD_KIND_ALPHA: return dd_component_from8(argb >> 24, f->a);
    case DD_KIND_LUMINANCE: {
      const uint32_t l = (((argb >> 16) & 0xFF) * 77u + ((argb >> 8) & 0xFF) * 150u + (argb & 0xFF) * 29u + 128u) >> 8;
      return dd_component_from8(l, f->r) | dd_component_from8(argb >> 24, f->a);
    }
    default: return 0;
  }
}

/* The colour of pixel (x, y) of any colour format (dd_format_has_colour), x and y inside an image dd_image_check accepted. */
static inline uint32_t dd_bc_colour(const uint8_t *block, uint32_t px, uint32_t py, int dxt1, uint32_t *alpha_out);
static inline uint32_t dd_read_argb(const DdImage *im, uint32_t x, uint32_t y) {
  const DdFormatInfo *f = dd_format(im->fmt);
  if (f->kind == DD_KIND_YUV_PACKED) {
    const uint8_t *p = im->data + (size_t)y * im->pitch + (size_t)(x & ~1u) * 2u;
    const uint32_t yy = im->fmt == DD_FMT_YUY2 ? p[(x & 1u) ? 2 : 0] : p[(x & 1u) ? 3 : 1];
    const uint32_t u = im->fmt == DD_FMT_YUY2 ? p[1] : p[0], v = im->fmt == DD_FMT_YUY2 ? p[3] : p[2];
    return dd_yuv_to_argb(yy, u, v);
  }
  if (f->kind == DD_KIND_YUV_PLANAR) {
    const size_t luma = (size_t)im->pitch * im->height;
    const uint32_t yy = im->data[(size_t)y * im->pitch + x];
    uint32_t u, v;
    if (im->fmt == DD_FMT_NV12) {
      const uint8_t *c = im->data + luma + (size_t)(y / 2u) * im->pitch + (size_t)(x & ~1u);
      u = c[0]; v = c[1];
    } else {      /* YV12: the V plane, then the U plane, each of pitch / 2 */
      const size_t cp = im->pitch / 2u, ch = (im->height + 1u) / 2u;
      v = im->data[luma + (size_t)(y / 2u) * cp + x / 2u];
      u = im->data[luma + cp * ch + (size_t)(y / 2u) * cp + x / 2u];
    }
    return dd_yuv_to_argb(yy, u, v);
  }
  if (f->kind == DD_KIND_BLOCKS) {
    const uint8_t *block = im->data + (size_t)(y / 4u) * im->pitch + (size_t)(x / 4u) * f->block_bytes;
    uint32_t alpha = 0xFF;
    const uint32_t rgb = dd_bc_colour(f->block_bytes == 8 ? block : block + 8, x & 3u, y & 3u, im->fmt == DD_FMT_DXT1, &alpha);
    if (im->fmt == DD_FMT_DXT2 || im->fmt == DD_FMT_DXT3) {
      const uint32_t i = (y & 3u) * 4u + (x & 3u);
      alpha = ((block[i / 2u] >> ((i & 1u) * 4u)) & 0xFu) * 17u;
    } else if (im->fmt == DD_FMT_DXT4 || im->fmt == DD_FMT_DXT5) {
      const uint32_t a0 = block[0], a1 = block[1];
      uint64_t bits = 0;
      for (int i = 0; i < 6; ++i) bits |= (uint64_t)block[2 + i] << (8 * i);
      const uint32_t code = (uint32_t)(bits >> (3u * ((y & 3u) * 4u + (x & 3u)))) & 7u;
      if (code == 0) alpha = a0;
      else if (code == 1) alpha = a1;
      else if (a0 > a1) alpha = ((8u - code) * a0 + (code - 1u) * a1) / 7u;
      else alpha = code == 6 ? 0 : code == 7 ? 255 : ((6u - code) * a0 + (code - 1u) * a1) / 5u;
    }
    return (alpha << 24) | rgb;
  }
  return dd_raw_to_argb(im, dd_get_raw(im, x, y));
}
/* A 5-6-5 colour as 8-8-8. */
static inline uint32_t dd_565_to_888(uint32_t c) {
  const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}
/* The colour of pixel (px, py) of the colour half of a DXT block (8 bytes); DXT1's punch-through black is transparent (*alpha_out 0). */
static inline uint32_t dd_bc_colour(const uint8_t *block, uint32_t px, uint32_t py, int dxt1, uint32_t *alpha_out) {
  const uint32_t c0 = (uint32_t)block[0] | ((uint32_t)block[1] << 8), c1 = (uint32_t)block[2] | ((uint32_t)block[3] << 8);
  const uint32_t bits = (uint32_t)block[4] | ((uint32_t)block[5] << 8) | ((uint32_t)block[6] << 16) | ((uint32_t)block[7] << 24);
  const uint32_t code = (bits >> (2u * (py * 4u + px))) & 3u;
  const uint32_t a = dd_565_to_888(c0), b = dd_565_to_888(c1);
  if (code == 0) return a;
  if (code == 1) return b;
  uint32_t out = 0;
  for (int shift = 0; shift < 24; shift += 8) {
    const uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
    uint32_t c;
    if (c0 > c1 || !dxt1) c = code == 2 ? (2u * ca + cb + 1u) / 3u : (ca + 2u * cb + 1u) / 3u;
    else c = code == 2 ? (ca + cb) / 2u : 0u;
    out |= c << shift;
  }
  if (dxt1 && c0 <= c1 && code == 3) *alpha_out = 0;
  return out;
}

/* ---- DXT encoding (what a blit into a DXT surface writes) ------------------------------------------------------------------------------------------ */

static inline uint32_t dd_888_to_565(uint32_t c) {
  const uint32_t r = ((c >> 16) & 0xFF) * 31u + 127u, g = ((c >> 8) & 0xFF) * 63u + 127u, b = (c & 0xFF) * 31u + 127u;
  return ((r / 255u) << 11) | ((g / 255u) << 5) | (b / 255u);
}
static inline uint32_t dd_rgb_distance(uint32_t a, uint32_t b) {
  const int32_t dr = (int32_t)((a >> 16) & 0xFF) - (int32_t)((b >> 16) & 0xFF);
  const int32_t dg = (int32_t)((a >> 8) & 0xFF) - (int32_t)((b >> 8) & 0xFF);
  const int32_t db = (int32_t)(a & 0xFF) - (int32_t)(b & 0xFF);
  return (uint32_t)(dr * dr + dg * dg + db * db);
}
/* The colours of a colour half with endpoints c0, c1 (the decoder's arithmetic, dd_bc_colour). */
static inline void dd_bc_palette(uint32_t c0, uint32_t c1, int three, uint32_t pal[4]) {
  const uint32_t a = dd_565_to_888(c0), b = dd_565_to_888(c1);
  pal[0] = a;
  pal[1] = b;
  pal[2] = pal[3] = 0;
  for (int shift = 0; shift < 24; shift += 8) {
    const uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
    if (three) { pal[2] |= ((ca + cb) / 2u) << shift; }
    else { pal[2] |= ((2u * ca + cb + 1u) / 3u) << shift; pal[3] |= ((ca + 2u * cb + 1u) / 3u) << shift; }
  }
}
/* Codes for the pixels of a block with these endpoints; the summed error. `transparent` pixels (DXT1 three-colour blocks) take code 3. */
static inline uint64_t dd_bc_fit(const uint32_t px[16], const uint8_t transparent[16], uint32_t c0, uint32_t c1, int three, uint32_t *bits) {
  uint32_t pal[4];
  dd_bc_palette(c0, c1, three, pal);
  uint64_t err = 0;
  *bits = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t code = 0;
    if (transparent[i]) code = 3;
    else {
      uint32_t best = ~0u;
      for (uint32_t k = 0; k < (three ? 3u : 4u); ++k) {
        const uint32_t d = dd_rgb_distance(px[i], pal[k]);
        if (d < best) { best = d; code = k; }
      }
      err += best;
    }
    *bits |= code << (2u * i);
  }
  return err;
}
/* Encodes the colour half (8 bytes) of a block of 16 A8R8G8B8 pixels (row by row). With `dxt1`, pixels of alpha below 128 become DXT1's transparent
 * black (a three-colour block); otherwise the block has four colours (c0 > c1) when its endpoints differ. The endpoints are the pair of the block's
 * colours farthest apart or the corners of their bounding box, whichever fits better; a block of two colours (or one) is exact up to 5-6-5. */
static inline void dd_bc_encode_colour(const uint32_t px[16], int dxt1, uint8_t out[8]) {
  uint8_t transparent[16];
  int any_transparent = 0, any_opaque = 0;
  uint32_t lo = 0x00FFFFFFu, hi = 0;
  uint32_t ea = 0, eb = 0, farthest = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    transparent[i] = (uint8_t)(dxt1 && (px[i] >> 24) < 128u);
    if (transparent[i]) { any_transparent = 1; continue; }
    if (!any_opaque) { ea = eb = px[i]; }
    any_opaque = 1;
    for (int shift = 0; shift < 24; shift += 8) {
      const uint32_t c = (px[i] >> shift) & 0xFF;
      if (c < ((lo >> shift) & 0xFF)) lo = (lo & ~(0xFFu << shift)) | (c << shift);
      if (c > ((hi >> shift) & 0xFF)) hi = (hi & ~(0xFFu << shift)) | (c << shift);
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (transparent[j]) continue;
      const uint32_t d = dd_rgb_distance(px[i], px[j]);
      if (d > farthest) { farthest = d; ea = px[i]; eb = px[j]; }
    }
  }
  uint32_t c0 = 0, c1 = 0, bits = 0xFFFFFFFFu;
  if (any_opaque) {
    const uint32_t cand[2][2] = {{dd_888_to_565(ea), dd_888_to_565(eb)}, {dd_888_to_565(hi), dd_888_to_565(lo)}};
    uint64_t best = ~(uint64_t)0;
    for (int k = 0; k < 2; ++k) {
      uint32_t a = cand[k][0], b = cand[k][1];
      const int three = any_transparent;
      /* (four colours: c0 > c1; three colours and transparency: c0 <= c1) */
      if (three ? a > b : a < b) { const uint32_t t = a; a = b; b = t; }
      uint32_t kb;
      const uint64_t e = dd_bc_fit(px, transparent, a, b, three, &kb);
      if (e < best) { best = e; c0 = a; c1 = b; bits = kb; }
    }
    /* (equal endpoints without transparency: every pixel code 0, c0 in either mode - a DXT1 block of c0 == c1 is a three-colour one) */
    if (!any_transparent && c0 == c1) bits = 0;
  }
  out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
  out[4] = (uint8_t)bits; out[5] = (uint8_t)(bits >> 8); out[6] = (uint8_t)(bits >> 16); out[7] = (uint8_t)(bits >> 24);
}
/* The alphas of a DXT4/5 alpha half (the decoder's arithmetic, dd_read_argb). */
static inline void dd_bc_alpha_palette(uint32_t a0, uint32_t a1, uint32_t pal[8]) {
  pal[0] = a0;
  pal[1] = a1;
  for (uint32_t code = 2; code < 8; ++code) {
    if (a0 > a1) pal[code] = ((8u - code) * a0 + (code - 1u) * a1) / 7u;
    else pal[code] = code == 6 ? 0 : code == 7 ? 255 : ((6u - code) * a0 + (code - 1u) * a1) / 5u;
  }
}
/* Encodes DXT4/5's alpha half (8 bytes): eight alphas between the block's extremes, or six and 0 and 255 when that fits better. */
static inline void dd_bc_encode_alpha(const uint32_t px[16], uint8_t out[8]) {
  uint32_t lo = 255, hi = 0, lo_inner = 255, hi_inner = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t a = px[i] >> 24;
    if (a < lo) lo = a;
    if (a > hi) hi = a;
    if (a != 0 && a != 255) { if (a < lo_inner) lo_inner = a; if (a > hi_inner) hi_inner = a; }
  }
  if (lo_inner > hi_inner) lo_inner = hi_inner = lo;
  const uint32_t cand[2][2] = {{hi, lo}, {lo_inner, hi_inner}};      /* (a0 > a1: eight alphas; a0 <= a1: six, 0 and 255) */
  uint64_t best = ~(uint64_t)0, bbits = 0;
  uint32_t ba0 = hi, ba1 = lo;
  for (int k = 0; k < 2; ++k) {
    const uint32_t a0 = cand[k][0], a1 = cand[k][1];
    uint32_t pal[8];
    dd_bc_alpha_palette(a0, a1, pal);
    uint64_t err = 0, bits = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      const uint32_t a = px[i] >> 24;
      uint32_t code = 0, bd = ~0u;
      for (uint32_t c = 0; c < 8; ++c) {
        const uint32_t d = a > pal[c] ? a - pal[c] : pal[c] - a;
        if (d < bd) { bd = d; code = c; }
      }
      err += (uint64_t)bd * bd;
      bits |= (uint64_t)code << (3u * i);
    }
    if (err < best) { best = err; ba0 = a0; ba1 = a1; bbits = bits; }
  }
  out[0] = (uint8_t)ba0;
  out[1] = (uint8_t)ba1;
  for (int i = 0; i < 6; ++i) out[2 + i] = (uint8_t)(bbits >> (8 * i));
}
/* Encodes 16 A8R8G8B8 pixels (row by row) as one block of a DXT format (DXT2 and DXT4 store the colours as given, as the decoder reads them). */
static inline void dd_bc_encode_block(uint32_t fmt, const uint32_t px[16], uint8_t *out) {
  if (fmt == DD_FMT_DXT1) { dd_bc_encode_colour(px, 1, out); return; }
  if (fmt == DD_FMT_DXT2 || fmt == DD_FMT_DXT3) {
    for (uint32_t i = 0; i < 16; i += 2) {
      const uint32_t a = ((px[i] >> 24) * 15u + 127u) / 255u, b = ((px[i + 1] >> 24) * 15u + 127u) / 255u;
      out[i / 2] = (uint8_t)(a | (b << 4));
    }
  } else {
    dd_bc_encode_alpha(px, out);
  }
  dd_bc_encode_colour(px, 0, out + 8);
}

/* Writes a colour to pixel (x, y) of a palette, RGB, alpha, luminance or packed YUV format (a YUV pixel takes the luminance of the colour and its pair
 * the chroma). */
static inline void dd_write_argb(DdImage *im, uint32_t x, uint32_t y, uint32_t argb) {
  const DdFormatInfo *f = dd_format(im->fmt);
  if (f->kind == DD_KIND_YUV_PACKED) {
    uint8_t *p = im->data + (size_t)y * im->pitch + (size_t)(x & ~1u) * 2u;
    uint32_t yy, u, v;
    dd_argb_to_yuv(argb, &yy, &u, &v);
    if (im->fmt == DD_FMT_YUY2) { p[(x & 1u) ? 2 : 0] = (uint8_t)yy; p[1] = (uint8_t)u; p[3] = (uint8_t)v; }
    else { p[(x & 1u) ? 3 : 1] = (uint8_t)yy; p[0] = (uint8_t)u; p[2] = (uint8_t)v; }
    return;
  }
  if (f->kind == DD_KIND_PALETTE || f->kind == DD_KIND_RGB || f->kind == DD_KIND_ALPHA || f->kind == DD_KIND_LUMINANCE) dd_put_raw(im, x, y, dd_argb_to_raw(im, argb));
}
/* Formats a blit can write colours into. */
static inline int dd_format_writable_colour(uint32_t fmt) {
  const uint8_t k = dd_format(fmt)->kind;
  return k == DD_KIND_PALETTE || k == DD_KIND_RGB || k == DD_KIND_ALPHA || k == DD_KIND_LUMINANCE || k == DD_KIND_YUV_PACKED;
}

/* A depth raw value as a fraction of the largest (32 bits of precision), and back; the stencil bits are kept apart. */
static inline uint32_t dd_depth_to_u32(uint32_t fmt, uint32_t raw) {
  const DdFormatInfo *f = dd_format(fmt);
  const uint32_t max = dd_mask_max(f->r), v = (raw & f->r) >> dd_mask_shift(f->r);
  return max ? (uint32_t)(((uint64_t)v * 0xFFFFFFFFu + max / 2u) / max) : 0;
}
static inline uint32_t dd_depth_from_u32(uint32_t fmt, uint32_t z) {
  const DdFormatInfo *f = dd_format(fmt);
  const uint32_t max = dd_mask_max(f->r);
  return (uint32_t)((((uint64_t)z * max + 0x7FFFFFFFu) / 0xFFFFFFFFu) << dd_mask_shift(f->r)) & f->r;
}

/* ---- fills ----------------------------------------------------------------------------------------------------------------------------------------- */

enum { DD_PIX_OK = 0, DD_PIX_BAD_IMAGE, DD_PIX_BAD_RECT, DD_PIX_UNSUPPORTED, DD_PIX_OVERLAP };

/* Fills a rectangle with a raw value (a colour fill's DDBLTFX.dwFillColor, a depth fill's dwFillDepth). Byte-addressed and palette formats pixel by
 * pixel; a packed YUV format by pairs, the value being the four bytes of a pair (the rectangle's left and right are taken to even pixels). Block and
 * planar formats cannot be filled (DD_PIX_UNSUPPORTED). */
static inline int dd_fill(DdImage *im, const DdRect *r, uint32_t raw) {
  if (!dd_image_check(im)) return DD_PIX_BAD_IMAGE;
  if (!dd_rect_inside(im, r)) return DD_PIX_BAD_RECT;
  const DdFormatInfo *f = dd_format(im->fmt);
  if (f->kind == DD_KIND_BLOCKS || f->kind == DD_KIND_YUV_PLANAR) return DD_PIX_UNSUPPORTED;
  if (f->kind == DD_KIND_YUV_PACKED) {
    const uint32_t l = (uint32_t)r->left & ~1u;
    uint32_t right = ((uint32_t)r->right + 1u) & ~1u;
    if (right > ((im->width + 1u) & ~1u)) right = (im->width + 1u) & ~1u;
    for (uint32_t y = (uint32_t)r->top; y < (uint32_t)r->bottom; ++y) {
      uint8_t *p = im->data + (size_t)y * im->pitch + (size_t)l * 2u;
      for (uint32_t x = l; x < right; x += 2, p += 4) { p[0] = (uint8_t)raw; p[1] = (uint8_t)(raw >> 8); p[2] = (uint8_t)(raw >> 16); p[3] = (uint8_t)(raw >> 24); }
    }
    return DD_PIX_OK;
  }
  const uint32_t mask = f->bytes >= 4 ? 0xFFFFFFFFu : f->bytes ? ((uint32_t)1 << (8u * f->bytes)) - 1u : f->r;
  raw &= mask;
  if (f->bytes == 1 || (f->bytes && ((raw & 0xFF) * 0x01010101u & mask) == raw)) {      /* (every byte the same: rows of bytes) */
    for (uint32_t y = (uint32_t)r->top; y < (uint32_t)r->bottom; ++y) {
      uint8_t *p = im->data + (size_t)y * im->pitch + (size_t)r->left * f->bytes;
      const size_t n = (size_t)(r->right - r->left) * f->bytes;
      for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)raw;
    }
    return DD_PIX_OK;
  }
  for (uint32_t y = (uint32_t)r->top; y < (uint32_t)r->bottom; ++y)
    for (uint32_t x = (uint32_t)r->left; x < (uint32_t)r->right; ++x) dd_put_raw(im, x, y, raw);
  return DD_PIX_OK;
}

/* ---- blits ----------------------------------------------------------------------------------------------------------------------------------------- */

#define DD_BLIT_SRC_KEY     0x01u   /* skip source pixels whose colour bits are in [src_key_low, src_key_high] */
#define DD_BLIT_DST_KEY     0x02u   /* write only over destination pixels whose colour bits are in [dst_key_low, dst_key_high] */
#define DD_BLIT_MIRROR_LR   0x04u   /* the picture mirrored left to right */
#define DD_BLIT_MIRROR_UD   0x08u   /* ... top to bottom */
#define DD_BLIT_ROTATE90    0x10u   /* turned 90 degrees clockwise (DDBLTFX_ROTATE90) */
#define DD_BLIT_ROTATE180   0x20u
#define DD_BLIT_ROTATE270   0x40u

typedef struct DdBlit {
  uint32_t flags;
  uint32_t src_key_low, src_key_high;
  uint32_t dst_key_low, dst_key_high;
} DdBlit;

/* The format of the same layout with alpha in the bits an X format leaves unused (the format itself for the others). */
static inline uint32_t dd_alpha_twin(uint32_t fmt) {
  switch (fmt) {
    case DD_FMT_X1R5G5B5: return DD_FMT_A1R5G5B5;
    case DD_FMT_X4R4G4B4: return DD_FMT_A4R4G4B4;
    case DD_FMT_X8R8G8B8: return DD_FMT_A8R8G8B8;
    case DD_FMT_X8B8G8R8: return DD_FMT_A8B8G8R8;
    default: return fmt;
  }
}
/* 16-bit formats of one layout with and without alpha (a blit between them copies the raw values). */
static inline int dd_layout_twins(uint32_t a, uint32_t b) {
  return (a == DD_FMT_X1R5G5B5 && b == DD_FMT_A1R5G5B5) || (a == DD_FMT_A1R5G5B5 && b == DD_FMT_X1R5G5B5) ||
         (a == DD_FMT_X4R4G4B4 && b == DD_FMT_A4R4G4B4) || (a == DD_FMT_A4R4G4B4 && b == DD_FMT_X4R4G4B4);
}

/* Do two rectangles of images share bytes? (The same memory overlapping.) */
static inline int dd_blit_overlaps(const DdImage *a, const DdRect *ra, const DdImage *b, const DdRect *rb) {
  /* (the bytes of each image's rows the rectangles touch, every plane counted: a block image's rows are rows of blocks) */
  const uintptr_t abase = (uintptr_t)a->data, bbase = (uintptr_t)b->data;
  const uint32_t at = dd_format_is_blocks(a->fmt) ? (uint32_t)ra->top / 4u : (uint32_t)ra->top;
  const uint32_t bt = dd_format_is_blocks(b->fmt) ? (uint32_t)rb->top / 4u : (uint32_t)rb->top;
  const uintptr_t a0 = abase + (uintptr_t)at * a->pitch, a1 = abase + a->size;
  const uintptr_t b0 = bbase + (uintptr_t)bt * b->pitch, b1 = bbase + b->size;
  if (a1 <= b0 || b1 <= a0) return 0;
  if (a->data == b->data && a->pitch == b->pitch) return !(ra->right <= rb->left || rb->right <= ra->left || ra->bottom <= rb->top || rb->bottom <= ra->top);
  return 1;
}

static inline int dd_key_in(uint32_t v, uint32_t mask, uint32_t low, uint32_t high) { v &= mask; return v >= low && v <= high; }

/* The source pixel that destination pixel (u, v) of a w x h destination takes, by the transform of the blit (point sampling, the way Windows' and
 * Wine's software blitters step: 16.16 increments from the rectangle's corner). */
static inline void dd_blit_source(const DdBlit *b, uint32_t u, uint32_t v, uint32_t dw, uint32_t dh, uint32_t sw, uint32_t sh, uint32_t *su, uint32_t *sv) {
  uint32_t x = u, y = v, w = dw, h = dh;
  const uint32_t f = b ? b->flags : 0;
  if (f & DD_BLIT_MIRROR_LR) x = w - 1u - x;
  if (f & DD_BLIT_MIRROR_UD) y = h - 1u - y;
  if (f & DD_BLIT_ROTATE180) { x = w - 1u - x; y = h - 1u - y; }
  if (f & (DD_BLIT_ROTATE90 | DD_BLIT_ROTATE270)) {
    /* (the source is turned: a destination column is a source row) */
    const uint32_t tx = (f & DD_BLIT_ROTATE90) ? y : h - 1u - y, ty = (f & DD_BLIT_ROTATE90) ? w - 1u - x : x;
    x = tx; y = ty;
    const uint32_t t = w; w = h; h = t;
  }
  *su = (uint32_t)(((uint64_t)x * (((uint64_t)sw << 16) / w)) >> 16);
  *sv = (uint32_t)(((uint64_t)y * (((uint64_t)sh << 16) / h)) >> 16);
  if (*su >= sw) *su = sw - 1u;
  if (*sv >= sh) *sv = sh - 1u;
}

/* Copies `sr` of `src` to `dr` of `dst`, stretching or shrinking to fit, with the blit's effects and colour keys. Between images of one format the raw
 * values are copied; between colour formats each pixel goes through A8R8G8B8 (a palette destination takes the nearest entry); between depth formats the
 * depth is scaled. Block formats copy whole blocks to the same format; any other blit into one re-encodes the blocks it touches. Planar formats only as a
 * source of colour (or whole copies of one format).
 * Overlapping rectangles of the same memory are handled for plain copies of one format and size (the direction is chosen); other blits of
 * overlapping memory answer DD_PIX_OVERLAP, and the caller copies the source away first. */
static inline int dd_blit(DdImage *dst, const DdRect *dr, const DdImage *src, const DdRect *sr, const DdBlit *b) {
  if (!dd_image_check(dst) || !dd_image_check(src)) return DD_PIX_BAD_IMAGE;
  if (!dd_rect_inside(dst, dr) || !dd_rect_inside(src, sr)) return DD_PIX_BAD_RECT;
  const DdFormatInfo *df = dd_format(dst->fmt), *sf = dd_format(src->fmt);
  const uint32_t flags = b ? b->flags : 0;
  const uint32_t dw = (uint32_t)(dr->right - dr->left), dh = (uint32_t)(dr->bottom - dr->top);
  const uint32_t sw = (uint32_t)(sr->right - sr->left), sh = (uint32_t)(sr->bottom - sr->top);
  /* (16-bit formats with and without alpha of one layout copy raw: Windows' blitter copies X1R5G5B5 into A1R5G5B5 as it is, test_surface_format_conversion_alpha) */
  const int same = dst->fmt == src->fmt || dd_layout_twins(dst->fmt, src->fmt);
  const int stretch = dw != sw || dh != sh;
  const int effects = (flags & (DD_BLIT_MIRROR_LR | DD_BLIT_MIRROR_UD | DD_BLIT_ROTATE90 | DD_BLIT_ROTATE180 | DD_BLIT_ROTATE270)) != 0;
  const int turned = (flags & (DD_BLIT_ROTATE90 | DD_BLIT_ROTATE270)) != 0;

  /* block formats: whole blocks of one format are copied; anything else into a block format decodes the blocks it touches, writes the pixels and
   * encodes them again (Windows' blitter takes any rectangle, stretching and converting: Wine's test_compressed_surface_stretch) */
  if (df->kind == DD_KIND_BLOCKS || sf->kind == DD_KIND_BLOCKS) {
    if (df->kind == DD_KIND_BLOCKS) {
      const int whole = same && !stretch && !effects && !(flags & (DD_BLIT_SRC_KEY | DD_BLIT_DST_KEY)) &&
                        !((dr->left & 3) || (dr->top & 3) || (sr->left & 3) || (sr->top & 3)) &&
                        !(((dr->right & 3) && (uint32_t)dr->right != dst->width) || ((dr->bottom & 3) && (uint32_t)dr->bottom != dst->height)) &&
                        !(((sr->right & 3) && (uint32_t)sr->right != src->width) || ((sr->bottom & 3) && (uint32_t)sr->bottom != src->height));
      if (dd_blit_overlaps(dst, dr, src, sr)) return DD_PIX_OVERLAP;
      if (!whole) {
        if (!dd_format_has_colour(src->fmt)) return DD_PIX_UNSUPPORTED;
        const uint32_t skeymask = dd_colour_mask(src->fmt);
        const int src_raw_key = sf->bytes || sf->kind == DD_KIND_PALETTE;
        for (uint32_t by = (uint32_t)dr->top / 4u; by * 4u < (uint32_t)dr->bottom; ++by)
          for (uint32_t bx = (uint32_t)dr->left / 4u; bx * 4u < (uint32_t)dr->right; ++bx) {
            uint32_t px[16];
            for (uint32_t i = 0; i < 16; ++i) {
              const uint32_t x = bx * 4u + (i & 3u), y = by * 4u + i / 4u;
              /* (a block's pixels past the image's edge: whatever the block has there) */
              px[i] = dd_read_argb(dst, x < dst->width ? x : dst->width - 1u, y < dst->height ? y : dst->height - 1u);
              if (x < (uint32_t)dr->left || x >= (uint32_t)dr->right || y < (uint32_t)dr->top || y >= (uint32_t)dr->bottom) continue;
              uint32_t su, sv;
              dd_blit_source(b, x - (uint32_t)dr->left, y - (uint32_t)dr->top, dw, dh, sw, sh, &su, &sv);
              const uint32_t sx = (uint32_t)sr->left + su, sy = (uint32_t)sr->top + sv;
              if ((flags & DD_BLIT_SRC_KEY) && src_raw_key && dd_key_in(dd_get_raw(src, sx, sy), skeymask, b->src_key_low, b->src_key_high)) continue;
              px[i] = dd_read_argb(src, sx, sy);
            }
            dd_bc_encode_block(dst->fmt, px, dst->data + (size_t)by * dst->pitch + (size_t)bx * df->block_bytes);
          }
        return DD_PIX_OK;
      }
      const uint32_t bw = (dw + 3u) / 4u, bh = (dh + 3u) / 4u;
      for (uint32_t y = 0; y < bh; ++y) {
        uint8_t *d = dst->data + (size_t)((uint32_t)dr->top / 4u + y) * dst->pitch + (size_t)((uint32_t)dr->left / 4u) * df->block_bytes;
        const uint8_t *s = src->data + (size_t)((uint32_t)sr->top / 4u + y) * src->pitch + (size_t)((uint32_t)sr->left / 4u) * sf->block_bytes;
        for (size_t i = 0; i < (size_t)bw * df->block_bytes; ++i) d[i] = s[i];
      }
      return DD_PIX_OK;
    }
    /* (a block source into a colour destination: decoded below) */
  }
  if (df->kind == DD_KIND_YUV_PLANAR) {
    if (!same || stretch || effects || (flags & (DD_BLIT_SRC_KEY | DD_BLIT_DST_KEY))) return DD_PIX_UNSUPPORTED;
    if (dr->left || dr->top || sr->left || sr->top || dw != dst->width || dh != dst->height || dst->width != src->width || dst->height != src->height) return DD_PIX_UNSUPPORTED;
    if (dd_blit_overlaps(dst, dr, src, sr)) return DD_PIX_OVERLAP;
    const uint64_t n = dd_image_bytes(dst->fmt, dst->width, dst->height, dst->pitch);
    if (src->pitch != dst->pitch) {
      /* (luminance rows, then the chroma rows; each plane row by row) */
      for (uint32_t y = 0; y < dst->height; ++y)
        for (uint32_t x = 0; x < dst->width; ++x) dst->data[(size_t)y * dst->pitch + x] = src->data[(size_t)y * src->pitch + x];
      const uint32_t ch = (dst->height + 1u) / 2u;
      if (dst->fmt == DD_FMT_NV12) {
        for (uint32_t y = 0; y < ch; ++y)
          for (uint32_t x = 0; x < ((dst->width + 1u) & ~1u) && x < dst->pitch && x < src->pitch; ++x)
            dst->data[(size_t)dst->pitch * dst->height + (size_t)y * dst->pitch + x] = src->data[(size_t)src->pitch * src->height + (size_t)y * src->pitch + x];
      } else {
        const uint32_t dcp = dst->pitch / 2u, scp = src->pitch / 2u, cw = (dst->width + 1u) / 2u;
        for (uint32_t plane = 0; plane < 2; ++plane)
          for (uint32_t y = 0; y < ch; ++y)
            for (uint32_t x = 0; x < cw && x < dcp && x < scp; ++x)
              dst->data[(size_t)dst->pitch * dst->height + plane * (size_t)dcp * ch + (size_t)y * dcp + x] = src->data[(size_t)src->pitch * src->height + plane * (size_t)scp * ch + (size_t)y * scp + x];
      }
    } else {
      for (uint64_t i = 0; i < n; ++i) dst->data[i] = src->data[i];
    }
    return DD_PIX_OK;
  }

  /* one byte-addressed or palette format: raw values (packed YUV only as plain copies of whole pairs; anything else of it goes through colours) */
  const int yuv = df->kind == DD_KIND_YUV_PACKED;
  const int keyed = (flags & (DD_BLIT_SRC_KEY | DD_BLIT_DST_KEY)) != 0;
  if (same && (df->bytes || df->kind == DD_KIND_PALETTE) && (!yuv || (!stretch && !effects && !keyed && !(dr->left & 1) && !(sr->left & 1)))) {
    const uint32_t keymask = dd_colour_mask(dst->fmt);
    if (!stretch && !effects && !keyed && df->bytes) {
      /* a plain copy: rows of bytes, in the direction that is safe when the rectangles overlap in the same memory */
      const size_t n = (size_t)dw * df->bytes;
      const int overlap = dd_blit_overlaps(dst, dr, src, sr);
      const int up = overlap && dst->data + (size_t)dr->top * dst->pitch > src->data + (size_t)sr->top * src->pitch;
      for (uint32_t i = 0; i < dh; ++i) {
        const uint32_t y = up ? dh - 1u - i : i;
        uint8_t *d = dst->data + (size_t)((uint32_t)dr->top + y) * dst->pitch + (size_t)dr->left * df->bytes;
        const uint8_t *s = src->data + (size_t)((uint32_t)sr->top + y) * src->pitch + (size_t)sr->left * sf->bytes;
        if (overlap && d > s) { for (size_t k = n; k-- > 0;) d[k] = s[k]; }
        else { for (size_t k = 0; k < n; ++k) d[k] = s[k]; }
      }
      return DD_PIX_OK;
    }
    if (dd_blit_overlaps(dst, dr, src, sr)) return DD_PIX_OVERLAP;
    for (uint32_t y = 0; y < dh; ++y)
      for (uint32_t x = 0; x < dw; ++x) {
        uint32_t su, sv;
        dd_blit_source(b, x, y, dw, dh, sw, sh, &su, &sv);
        const uint32_t v = dd_get_raw(src, (uint32_t)sr->left + su, (uint32_t)sr->top + sv);
        if ((flags & DD_BLIT_SRC_KEY) && dd_key_in(v, keymask, b->src_key_low, b->src_key_high)) continue;
        if ((flags & DD_BLIT_DST_KEY) && !dd_key_in(dd_get_raw(dst, (uint32_t)dr->left + x, (uint32_t)dr->top + y), keymask, b->dst_key_low, b->dst_key_high)) continue;
        dd_put_raw(dst, (uint32_t)dr->left + x, (uint32_t)dr->top + y, v);
      }
    return DD_PIX_OK;
  }

  if (dd_blit_overlaps(dst, dr, src, sr)) return DD_PIX_OVERLAP;
  /* depth into depth: the depth scaled */
  if (df->kind == DD_KIND_DEPTH || sf->kind == DD_KIND_DEPTH) {
    if (df->kind != DD_KIND_DEPTH || sf->kind != DD_KIND_DEPTH || (flags & (DD_BLIT_SRC_KEY | DD_BLIT_DST_KEY))) return DD_PIX_UNSUPPORTED;
    for (uint32_t y = 0; y < dh; ++y)
      for (uint32_t x = 0; x < dw; ++x) {
        uint32_t su, sv;
        dd_blit_source(b, x, y, dw, dh, sw, sh, &su, &sv);
        const uint32_t v = dd_get_raw(src, (uint32_t)sr->left + su, (uint32_t)sr->top + sv);
        uint32_t out = dd_depth_from_u32(dst->fmt, dd_depth_to_u32(src->fmt, v));
        if (df->g && sf->g) out |= dd_component_from8(dd_component8(v, sf->g, 0), df->g);
        dd_put_raw(dst, (uint32_t)dr->left + x, (uint32_t)dr->top + y, out);
      }
    return DD_PIX_OK;
  }
  /* colours through A8R8G8B8 */
  if (!dd_format_has_colour(src->fmt) || !dd_format_writable_colour(dst->fmt)) return DD_PIX_UNSUPPORTED;
  if (turned && df->kind == DD_KIND_YUV_PACKED) return DD_PIX_UNSUPPORTED;
  const uint32_t skeymask = dd_colour_mask(src->fmt), dkeymask = dd_colour_mask(dst->fmt);
  const int src_raw_key = sf->bytes || sf->kind == DD_KIND_PALETTE;
  /* (a decoded DXT block keeps its alpha in the unused bits of an X8R8G8B8-like destination: Windows' decoder writes A8R8G8B8, Wine's
   * test_surface_format_conversion_alpha records 0xff there) */
  DdImage out = *dst;
  if (sf->kind == DD_KIND_BLOCKS) out.fmt = dd_alpha_twin(dst->fmt);
  for (uint32_t y = 0; y < dh; ++y)
    for (uint32_t x = 0; x < dw; ++x) {
      uint32_t su, sv;
      dd_blit_source(b, x, y, dw, dh, sw, sh, &su, &sv);
      const uint32_t sx = (uint32_t)sr->left + su, sy = (uint32_t)sr->top + sv;
      if ((flags & DD_BLIT_SRC_KEY) && src_raw_key && dd_key_in(dd_get_raw(src, sx, sy), skeymask, b->src_key_low, b->src_key_high)) continue;
      const uint32_t px = (uint32_t)dr->left + x, py = (uint32_t)dr->top + y;
      if ((flags & DD_BLIT_DST_KEY) && (df->bytes || df->kind == DD_KIND_PALETTE) && !dd_key_in(dd_get_raw(dst, px, py), dkeymask, b->dst_key_low, b->dst_key_high)) continue;
      /* (a P8 source into a colour destination: Windows' blitter copies the index into each channel, as if it were L8 - it does not look the
       * palette up; Wine's test_p8_blit records 0x10 -> 0x101010 on every driver but WARP) */
      const uint32_t c = src->fmt == DD_FMT_P8 ? 0xFF000000u | (dd_get_raw(src, sx, sy) & 0xFFu) * 0x010101u : dd_read_argb(src, sx, sy);
      dd_write_argb(&out, px, py, c);
    }
  return DD_PIX_OK;
}

/* A rectangle of an image as A8R8G8B8 rows (`out_pitch` bytes apart, at least 4 * width; `out_size` bytes in all). With `key` (what Direct3D makes of a
 * colour-keyed texture), pixels whose bits - colour and alpha, of the pixel and of the key alike - are in [key_low, key_high] get alpha 0, and the
 * others keep their alpha, except that an alpha of 0 becomes 1 (of 255), so that alpha 0 means "keyed" and nothing else: the pixel stage takes such
 * texels away (d3d9's TCOS_TSS_KEY_KILL). Windows compares a texture's alpha with the key's (Wine's test_ck_rgba). Depth and bump formats cannot be
 * read as colours (DD_PIX_UNSUPPORTED). */
static inline int dd_to_argb(const DdImage *src, const DdRect *r, uint8_t *out, size_t out_size, uint32_t out_pitch, int key, uint32_t key_low, uint32_t key_high) {
  if (!dd_image_check(src)) return DD_PIX_BAD_IMAGE;
  if (!dd_rect_inside(src, r)) return DD_PIX_BAD_RECT;
  if (!dd_format_has_colour(src->fmt)) return DD_PIX_UNSUPPORTED;
  const uint32_t w = (uint32_t)(r->right - r->left), h = (uint32_t)(r->bottom - r->top);
  if (!out || out_pitch < (uint64_t)w * 4u || (uint64_t)out_pitch * (h - 1u) + (uint64_t)w * 4u > out_size) return DD_PIX_BAD_IMAGE;
  const DdFormatInfo *f = dd_format(src->fmt);
  const int raw_key = key && (f->bytes || f->kind == DD_KIND_PALETTE) && f->kind != DD_KIND_YUV_PACKED;
  const uint32_t mask = dd_colour_mask(src->fmt) | (f->kind == DD_KIND_RGB ? f->a : 0u);
  const uint32_t low = key_low & mask, high = key_high & mask;
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t *o = out + (size_t)y * out_pitch;
    for (uint32_t x = 0; x < w; ++x, o += 4) {
      const uint32_t sx = (uint32_t)r->left + x, sy = (uint32_t)r->top + y;
      uint32_t c;
      if (raw_key || (f->bytes && f->kind != DD_KIND_YUV_PACKED) || f->kind == DD_KIND_PALETTE) {
        const uint32_t raw = dd_get_raw(src, sx, sy);
        c = dd_raw_to_argb(src, raw);
        if (raw_key && dd_key_in(raw, mask, low, high)) c &= 0x00FFFFFFu;
        else if (raw_key && !(c >> 24)) c |= 0x01000000u;
      } else {
        c = dd_read_argb(src, sx, sy);
      }
      o[0] = (uint8_t)c; o[1] = (uint8_t)(c >> 8); o[2] = (uint8_t)(c >> 16); o[3] = (uint8_t)(c >> 24);
    }
  }
  return DD_PIX_OK;
}

#ifdef __cplusplus
}
#endif

#endif
