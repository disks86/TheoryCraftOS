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

/* Image decoders for OleLoadPicture: BMP (all depths, RLE, bit fields), GIF (the first frame), and the choice of the right ICO / CUR entry. Every decoder works on a byte
 * span and checks each read against its end; the sizes of the results are limited (SO_MAX_BLOB) before anything is allocated. The result is a DIB in memory (so_dib). */

typedef struct so_dib {
  LONG width, height;                   /* height is positive; rows are stored bottom-up */
  WORD bpp;                             /* 1, 4, 8, 24 or 32 */
  DWORD colors;                         /* entries in palette (0 for more than 8 bits) */
  RGBQUAD palette[256];
  BYTE *bits;
  SIZE_T stride, size;
  LONG x_ppm, y_ppm;                    /* pixels per metre when the file says (0 otherwise) */
} so_dib;

static WORD so_u16(const BYTE *p) { return (WORD)(p[0] | (p[1] << 8)); }
static DWORD so_u32(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24); }

static void so_dib_free(so_dib *d) { oa_free(d->bits); d->bits = NULL; }

static BOOL so_dib_alloc(so_dib *d, LONG width, LONG height, WORD bpp) {
  memset(d, 0, sizeof(*d));
  if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return FALSE;
  ULONGLONG stride = (((ULONGLONG)width * bpp + 31) / 32) * 4;
  ULONGLONG size = stride * (ULONGLONG)height;
  if (size > SO_MAX_BLOB) return FALSE;
  d->bits = oa_calloc((SIZE_T)size);
  if (!d->bits) return FALSE;
  d->width = width; d->height = height; d->bpp = bpp;
  d->stride = (SIZE_T)stride; d->size = (SIZE_T)size;
  return TRUE;
}

/* ---- BMP ------------------------------------------------------------------------------------------------------------------------------------------------------ */
/* One channel of a pixel as 8 bits, given its mask. */
static BYTE so_mask_channel(DWORD pixel, DWORD mask) {
  if (!mask) return 0;
  int shift = 0;
  while (!((mask >> shift) & 1)) shift++;
  DWORD max = mask >> shift;
  if (max == 0 || max > 0xFFFF) return 0;
  return (BYTE)((((pixel & mask) >> shift) * 255u + max / 2) / max);
}

/* The run-length encodings: the pixels are decoded to indices first (one byte each), the rows bottom-up. */
static BOOL so_bmp_rle(const BYTE *d, SIZE_T len, BOOL rle4, LONG width, LONG height, BYTE *indices) {
  SIZE_T pos = 0;
  LONG x = 0, y = 0;
  #define SO_PUT(value) do { if (x < width && y < height) indices[(SIZE_T)y * (SIZE_T)width + (SIZE_T)x] = (BYTE)(value); x++; } while (0)
  while (len - pos >= 2) {
    BYTE n = d[pos], v = d[pos + 1];
    pos += 2;
    if (n) {
      for (BYTE i = 0; i < n; i++) SO_PUT(rle4 ? ((i & 1) ? (v & 15) : (v >> 4)) : v);
      continue;
    }
    if (v == 0) { x = 0; y++; }
    else if (v == 1) break;
    else if (v == 2) {
      if (len - pos < 2) break;
      x += d[pos]; y += d[pos + 1];
      pos += 2;
    } else {
      SIZE_T bytes = rle4 ? ((SIZE_T)v + 1) / 2 : v;
      if (bytes > len - pos) return FALSE;
      for (BYTE i = 0; i < v; i++) SO_PUT(rle4 ? ((i & 1) ? (d[pos + i / 2] & 15) : (d[pos + i / 2] >> 4)) : d[pos + i]);
      pos += bytes + (bytes & 1);
      if (pos > len) pos = len;
    }
    if (y >= height && x >= width) break;
  }
  #undef SO_PUT
  return TRUE;
}

static BOOL so_decode_bmp(const BYTE *data, SIZE_T len, BOOL file_header, so_dib *out) {
  SIZE_T off = 0, bits_off = 0;
  memset(out, 0, sizeof(*out));
  if (file_header) {
    if (len < 14 || data[0] != 'B' || data[1] != 'M') return FALSE;
    bits_off = so_u32(data + 10);
    off = 14;
  }
  if (len - off < 12) return FALSE;
  DWORD hsize = so_u32(data + off);
  if (hsize < 12 || hsize > len - off) return FALSE;
  LONG width, height;
  WORD bpp;
  DWORD comp = 0, clrused = 0, masks[3] = {0, 0, 0};
  LONG xppm = 0, yppm = 0;
  if (hsize == 12) {
    width = so_u16(data + off + 4); height = so_u16(data + off + 6); bpp = so_u16(data + off + 10);
  } else if (hsize >= 40) {
    width = (LONG)so_u32(data + off + 4); height = (LONG)so_u32(data + off + 8); bpp = so_u16(data + off + 14);
    comp = so_u32(data + off + 16); xppm = (LONG)so_u32(data + off + 24); yppm = (LONG)so_u32(data + off + 28); clrused = so_u32(data + off + 32);
    if (hsize >= 52) for (int i = 0; i < 3; i++) masks[i] = so_u32(data + off + 40 + 4 * i);
  } else return FALSE;
  BOOL top_down = height < 0;
  if (height == (LONG)0x80000000) return FALSE;
  if (top_down) height = -height;
  if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return FALSE;
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return FALSE;
  SIZE_T pal_off = off + hsize;
  if (comp == BI_BITFIELDS && (bpp == 16 || bpp == 32)) {
    if (hsize == 40) {
      if (len - pal_off < 12) return FALSE;
      for (int i = 0; i < 3; i++) masks[i] = so_u32(data + pal_off + 4 * i);
      pal_off += 12;
    }
  } else if (bpp == 16) { masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; }
  else if (bpp == 32) { masks[0] = 0x00FF0000; masks[1] = 0x0000FF00; masks[2] = 0x000000FF; }
  if (comp != BI_RGB && comp != BI_RLE8 && comp != BI_RLE4 && comp != BI_BITFIELDS) return FALSE;
  if ((comp == BI_RLE8 && bpp != 8) || (comp == BI_RLE4 && bpp != 4)) return FALSE;

  DWORD colors = 0, entry = hsize == 12 ? 3 : 4;
  if (bpp <= 8) {
    colors = clrused ? clrused : (1u << bpp);
    if (colors > (1u << bpp)) colors = 1u << bpp;
    if ((SIZE_T)colors * entry > len - pal_off) return FALSE;
  }
  SIZE_T data_off = pal_off + (SIZE_T)colors * entry;
  if (file_header && bits_off >= off + hsize) data_off = bits_off;
  if (data_off > len) return FALSE;
  const BYTE *pixels = data + data_off;
  SIZE_T pixel_len = len - data_off;

  if (comp == BI_RLE8 || comp == BI_RLE4) {
    if ((ULONGLONG)width * (ULONGLONG)height > ((ULONGLONG)pixel_len + 16) * 2048) return FALSE;     /* (an expansion no real file needs) */
  } else if ((((ULONGLONG)width * bpp + 31) / 32) * 4 * (ULONGLONG)height > pixel_len) return FALSE;
  /* the result: indexed images keep their depth (RLE is expanded; RLE8 with few colours is stored with 4 bits), 16 bits become 24 */
  WORD out_bpp = bpp == 16 ? 24 : bpp;
  if (comp == BI_RLE8 && colors <= 16) out_bpp = 4;
  if (!so_dib_alloc(out, width, height, out_bpp)) return FALSE;
  out->x_ppm = xppm > 0 ? xppm : 0; out->y_ppm = yppm > 0 ? yppm : 0;
  if (bpp <= 8) {
    out->colors = out_bpp == 4 && bpp == 8 && colors > 16 ? 16 : colors;
    for (DWORD i = 0; i < colors && i < 256; i++) {
      const BYTE *e = data + pal_off + (SIZE_T)i * entry;
      out->palette[i].rgbBlue = e[0]; out->palette[i].rgbGreen = e[1]; out->palette[i].rgbRed = e[2]; out->palette[i].rgbReserved = 0;
    }
  }
  if (comp == BI_RLE8 || comp == BI_RLE4) {
    ULONGLONG count = (ULONGLONG)width * (ULONGLONG)height;
    if (count > SO_MAX_BLOB) { so_dib_free(out); return FALSE; }
    BYTE *indices = oa_calloc((SIZE_T)count);
    if (!indices) { so_dib_free(out); return FALSE; }
    if (!so_bmp_rle(pixels, pixel_len, comp == BI_RLE4, width, height, indices)) { oa_free(indices); so_dib_free(out); return FALSE; }
    for (LONG y = 0; y < height; y++) {
      LONG row = top_down ? height - 1 - y : y;
      BYTE *dst = out->bits + (SIZE_T)row * out->stride;
      for (LONG x = 0; x < width; x++) {
        BYTE v = indices[(SIZE_T)y * (SIZE_T)width + (SIZE_T)x];
        if (out_bpp == 8) dst[x] = v;
        else if (x & 1) dst[x / 2] |= v & 15;
        else dst[x / 2] = (BYTE)(v << 4);
      }
    }
    oa_free(indices);
    return TRUE;
  }
  ULONGLONG src_stride = (((ULONGLONG)width * bpp + 31) / 32) * 4;
  if (src_stride * (ULONGLONG)height > pixel_len) { so_dib_free(out); return FALSE; }
  for (LONG y = 0; y < height; y++) {
    const BYTE *src = pixels + (SIZE_T)src_stride * (SIZE_T)y;
    BYTE *dst = out->bits + (SIZE_T)(top_down ? height - 1 - y : y) * out->stride;
    if (bpp == 16) {
      for (LONG x = 0; x < width; x++) {
        DWORD px = so_u16(src + 2 * x);
        dst[3 * x] = so_mask_channel(px, masks[2]); dst[3 * x + 1] = so_mask_channel(px, masks[1]); dst[3 * x + 2] = so_mask_channel(px, masks[0]);
      }
    } else if (bpp == 32 && comp == BI_BITFIELDS) {
      for (LONG x = 0; x < width; x++) {
        DWORD px = so_u32(src + 4 * x);
        dst[4 * x] = so_mask_channel(px, masks[2]); dst[4 * x + 1] = so_mask_channel(px, masks[1]); dst[4 * x + 2] = so_mask_channel(px, masks[0]); dst[4 * x + 3] = 0;
      }
    } else {
      memcpy(dst, src, (SIZE_T)(((ULONGLONG)width * bpp + 7) / 8));
    }
  }
  return TRUE;
}

/* ---- GIF ------------------------------------------------------------------------------------------------------------------------------------------------------ */
static BOOL so_gif_skip_blocks(const BYTE *d, SIZE_T len, SIZE_T *pos) {
  for (;;) {
    if (*pos >= len) return FALSE;
    BYTE n = d[(*pos)++];
    if (n == 0) return TRUE;
    if (n > len - *pos) return FALSE;
    *pos += n;
  }
}

typedef struct so_gif_bits {
  const BYTE *d;
  SIZE_T len, pos;
  unsigned left;                  /* bytes left in the current sub-block */
  DWORD buffer; int count;
} so_gif_bits;

static int so_gif_byte(so_gif_bits *b) {
  while (b->left == 0) {
    if (b->pos >= b->len) return -1;
    b->left = b->d[b->pos++];
    if (b->left == 0) return -1;                      /* the terminating block */
  }
  if (b->pos >= b->len) return -1;
  b->left--;
  return b->d[b->pos++];
}

static BOOL so_decode_gif(const BYTE *d, SIZE_T len, so_dib *out) {
  memset(out, 0, sizeof(*out));
  if (len < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F' || d[3] != '8') return FALSE;
  LONG screen_w = so_u16(d + 6), screen_h = so_u16(d + 8);
  BYTE flags = d[10], background = d[11];
  SIZE_T pos = 13;
  BYTE global_palette[768];
  unsigned global_n = 0;
  if (flags & 0x80) {
    global_n = 2u << (flags & 7);
    if ((SIZE_T)global_n * 3 > len - pos) return FALSE;
    memcpy(global_palette, d + pos, global_n * 3);
    pos += global_n * 3;
  }
  for (;;) {
    if (pos >= len) return FALSE;
    BYTE block = d[pos++];
    if (block == 0x3B) return FALSE;
    if (block == 0x21) {
      if (pos >= len) return FALSE;
      pos++;
      if (!so_gif_skip_blocks(d, len, &pos)) return FALSE;
      continue;
    }
    if (block != 0x2C) return FALSE;
    break;
  }
  if (len - pos < 9) return FALSE;
  LONG left = so_u16(d + pos), top = so_u16(d + pos + 2), fw = so_u16(d + pos + 4), fh = so_u16(d + pos + 6);
  BYTE iflags = d[pos + 8];
  pos += 9;
  BYTE local_palette[768];
  const BYTE *palette = global_palette;
  unsigned n = global_n;
  if (iflags & 0x80) {
    n = 2u << (iflags & 7);
    if ((SIZE_T)n * 3 > len - pos) return FALSE;
    memcpy(local_palette, d + pos, n * 3);
    pos += n * 3;
    palette = local_palette;
  }
  if (fw <= 0 || fh <= 0) return FALSE;
  if (pos >= len) return FALSE;
  int min_code = d[pos++];
  if (min_code < 2 || min_code > 8) return FALSE;
  ULONGLONG total = (ULONGLONG)fw * (ULONGLONG)fh;
  if (total > SO_MAX_BLOB || total > ((ULONGLONG)len + 64) * 8192) return FALSE;
  BYTE *frame = oa_calloc((SIZE_T)total);
  WORD *prefix = oa_alloc(4096 * sizeof(WORD));
  BYTE *suffix = oa_alloc(4096), *stack = oa_alloc(4097);
  if (!frame || !prefix || !suffix || !stack) { oa_free(frame); oa_free(prefix); oa_free(suffix); oa_free(stack); return FALSE; }

  so_gif_bits bits = {d, len, pos, 0, 0, 0};
  int clear = 1 << min_code, eoi = clear + 1, next = clear + 2, size = min_code + 1;
  int prev = -1, first = 0;
  SIZE_T produced = 0;
  while (produced < total) {
    while (bits.count < size) {
      int c = so_gif_byte(&bits);
      if (c < 0) goto decoded;
      bits.buffer |= (DWORD)c << bits.count;
      bits.count += 8;
    }
    int code = (int)(bits.buffer & ((1u << size) - 1));
    bits.buffer >>= size; bits.count -= size;
    if (code == clear) { size = min_code + 1; next = clear + 2; prev = -1; continue; }
    if (code == eoi) break;
    if (prev < 0) {
      if (code >= clear) break;
      frame[produced++] = (BYTE)code;
      prev = code; first = code;
      continue;
    }
    int cur = code, sp = 0;
    if (code >= next) {
      if (code > next) break;                      /* corrupt */
      stack[sp++] = (BYTE)first;
      cur = prev;
    }
    while (cur >= clear) {
      if (sp >= 4096 || cur >= 4096) goto decoded;
      stack[sp++] = suffix[cur];
      cur = prefix[cur];
    }
    stack[sp++] = (BYTE)cur;
    first = cur;
    while (sp && produced < total) frame[produced++] = stack[--sp];
    if (next < 4096) {
      prefix[next] = (WORD)prev; suffix[next] = (BYTE)first; next++;
      if (next == (1 << size) && size < 12) size++;
    }
    prev = code;
  }
decoded:
  oa_free(prefix); oa_free(suffix); oa_free(stack);

  if (screen_w <= 0 || screen_h <= 0) { screen_w = fw; screen_h = fh; left = top = 0; }
  if (n == 0) { n = 2; local_palette[0] = local_palette[1] = local_palette[2] = 0; local_palette[3] = local_palette[4] = local_palette[5] = 255; palette = local_palette; }
  WORD out_bpp = n <= 2 ? 1 : n <= 16 ? 4 : 8;
  if (!so_dib_alloc(out, screen_w, screen_h, out_bpp)) { oa_free(frame); return FALSE; }
  out->colors = n;
  for (unsigned i = 0; i < n && i < 256; i++) {
    out->palette[i].rgbRed = palette[3 * i]; out->palette[i].rgbGreen = palette[3 * i + 1]; out->palette[i].rgbBlue = palette[3 * i + 2];
  }
  BYTE fill = (flags & 0x80) && background < n ? background : 0;
  for (LONG y = 0; y < screen_h; y++) {
    BYTE *row = out->bits + (SIZE_T)(screen_h - 1 - y) * out->stride;
    for (LONG x = 0; x < screen_w; x++) {
      if (out_bpp == 8) row[x] = fill;
      else if (out_bpp == 4) { if (x & 1) row[x / 2] |= fill & 15; else row[x / 2] = (BYTE)(fill << 4); }
      else if (fill & 1) row[x / 8] |= (BYTE)(0x80 >> (x & 7));
    }
  }
  /* the frame, deinterlaced, onto the screen */
  for (LONG fy = 0; fy < fh; fy++) {
    LONG y;
    if (iflags & 0x40) {
      LONG rows_pass1 = (fh + 7) / 8, rows_pass2 = (fh + 3) / 8, rows_pass3 = (fh + 1) / 4;
      if (fy < rows_pass1) y = fy * 8;
      else if (fy < rows_pass1 + rows_pass2) y = (fy - rows_pass1) * 8 + 4;
      else if (fy < rows_pass1 + rows_pass2 + rows_pass3) y = (fy - rows_pass1 - rows_pass2) * 4 + 2;
      else y = (fy - rows_pass1 - rows_pass2 - rows_pass3) * 2 + 1;
    } else y = fy;
    LONG sy = top + y;
    if (sy < 0 || sy >= screen_h) continue;
    BYTE *row = out->bits + (SIZE_T)(screen_h - 1 - sy) * out->stride;
    for (LONG fx = 0; fx < fw; fx++) {
      LONG sx = left + fx;
      if (sx < 0 || sx >= screen_w) continue;
      BYTE v = frame[(SIZE_T)fy * (SIZE_T)fw + (SIZE_T)fx];
      if (v >= n) v = 0;
      if (out_bpp == 8) row[sx] = v;
      else if (out_bpp == 4) { if (sx & 1) row[sx / 2] = (BYTE)((row[sx / 2] & 0xF0) | v); else row[sx / 2] = (BYTE)((row[sx / 2] & 0x0F) | (v << 4)); }
      else { BYTE mask = (BYTE)(0x80 >> (sx & 7)); if (v & 1) row[sx / 8] |= mask; else row[sx / 8] &= (BYTE)~mask; }
    }
  }
  oa_free(frame);
  return TRUE;
}

/* ---- ICO / CUR ------------------------------------------------------------------------------------------------------------------------------------------- */
/* The entry of an icon file that suits the default icon size best (a bigger one rather than a smaller one; more colours on a tie). Returns FALSE when the file is bad. */
static BOOL so_ico_pick(const BYTE *d, SIZE_T len, int want, SIZE_T *offset, SIZE_T *size, BOOL *is_icon) {
  if (len < 6 || so_u16(d) != 0) return FALSE;
  WORD type = so_u16(d + 2), count = so_u16(d + 4);
  if ((type != 1 && type != 2) || count == 0) return FALSE;
  if ((SIZE_T)count > (len - 6) / 16) return FALSE;
  LONGLONG best_score = -1;
  BOOL found = FALSE;
  for (WORD i = 0; i < count; i++) {
    const BYTE *e = d + 6 + 16 * (SIZE_T)i;
    SIZE_T entry_size = so_u32(e + 8), entry_offset = so_u32(e + 12);
    if (entry_size < 4 || entry_offset > len || entry_size > len - entry_offset) continue;
    int w = e[0] ? e[0] : 256;
    int bpp = type == 1 ? so_u16(e + 6) : 1;
    LONGLONG score = (w >= want ? 100000 - (w - want) * 100 : 50000 - (want - w) * 100) + bpp;
    if (score > best_score) { best_score = score; *offset = entry_offset; *size = entry_size; found = TRUE; }
  }
  *is_icon = type == 1;
  return found;
}
