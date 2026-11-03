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

/* A JPEG decoder to a 24 bit DIB: baseline, extended sequential and progressive Huffman coding, 8 bit samples, 1, 3 or 4 components, any sampling factors, restart
 * intervals. Arithmetic coding, lossless and 12 bit JPEG are not supported. Corrupt or truncated entropy data ends the scan that it is in (the image decoded up to there is
 * kept); a bad header fails. All of the coefficients are kept until the end, so the work is the same for progressive and sequential files. */

static const BYTE so_jpeg_zigzag[64] = {
  0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct so_jhuff {
  BOOL present;
  BYTE vals[256];
  int mincode[17], maxcode[17], valptr[17];
} so_jhuff;

typedef struct so_jcomp {
  int id, h, v, tq;
  int dc_table, ac_table;
  int blocks_w, blocks_h;        /* in blocks, padded to whole MCUs */
  int real_w, real_h;            /* the blocks that hold image samples (non-interleaved scans visit only these) */
  int pred;
  short *coef;                   /* blocks_w * blocks_h * 64, natural order */
  BYTE *plane;                   /* blocks_w * 8 by blocks_h * 8 samples */
} so_jcomp;

typedef struct so_jpeg {
  const BYTE *d;
  SIZE_T len, pos;
  int width, height, ncomp, hmax, vmax, mcus_x, mcus_y;
  BOOL progressive, have_frame;
  int restart_interval;
  WORD qt[4][64];
  so_jhuff dc[4], ac[4];
  so_jcomp comp[4];
  int adobe_transform;           /* -1 when there is no Adobe marker */
  int x_density, y_density, density_unit;
  /* the entropy decoder */
  DWORD bit_buffer; int bit_count;
  int marker;                    /* a marker met inside the data, 0 for none */
  BOOL data_end;
  int eobrun;
} so_jpeg;

static void so_jpeg_free(so_jpeg *j) {
  for (int i = 0; i < 4; i++) { oa_free(j->comp[i].coef); oa_free(j->comp[i].plane); j->comp[i].coef = NULL; j->comp[i].plane = NULL; }
}

static int so_jpeg_get_bit(so_jpeg *j) {
  if (j->bit_count == 0) {
    int byte = 0;
    if (!j->marker && j->pos < j->len) {
      byte = j->d[j->pos++];
      if (byte == 0xFF) {
        int next = j->pos < j->len ? j->d[j->pos] : 0xD9;
        if (next == 0) j->pos++;
        else {
          while (next == 0xFF && j->pos + 1 < j->len) { j->pos++; next = j->d[j->pos]; }
          j->marker = next;
          j->pos--;                  /* (back on the 0xFF: the marker is read again by the marker loop) */
          byte = 0;
        }
      }
    } else { j->data_end = TRUE; }
    j->bit_buffer = (DWORD)byte;
    j->bit_count = 8;
  }
  j->bit_count--;
  return (int)((j->bit_buffer >> j->bit_count) & 1);
}
static int so_jpeg_get_bits(so_jpeg *j, int n) {
  int v = 0;
  while (n-- > 0) v = (v << 1) | so_jpeg_get_bit(j);
  return v;
}
static int so_jpeg_extend(so_jpeg *j, int n) {
  if (n == 0) return 0;
  int v = so_jpeg_get_bits(j, n);
  return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}
static int so_jpeg_decode(so_jpeg *j, const so_jhuff *h) {
  int code = 0;
  for (int l = 1; l <= 16; l++) {
    code = (code << 1) | so_jpeg_get_bit(j);
    if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) return h->vals[h->valptr[l] + code - h->mincode[l]];
    if (j->data_end) return -1;
  }
  return -1;
}

static BOOL so_jpeg_build_huff(so_jhuff *h, const BYTE *counts, const BYTE *vals, int nvals) {
  int code = 0, k = 0;
  memset(h, 0, sizeof(*h));
  for (int l = 1; l <= 16; l++) {
    h->valptr[l] = k;
    h->mincode[l] = code;
    k += counts[l - 1];
    code += counts[l - 1];
    h->maxcode[l] = counts[l - 1] ? code - 1 : -1;
    if (code > (1 << l)) return FALSE;
    code <<= 1;
  }
  if (k != nvals || k > 256) return FALSE;
  memcpy(h->vals, vals, (SIZE_T)k);
  h->present = TRUE;
  return TRUE;
}

/* The restart marker after an interval: the bit reader is realigned and the RSTn skipped. */
static void so_jpeg_restart(so_jpeg *j) {
  j->bit_count = 0;
  j->eobrun = 0;
  for (int i = 0; i < 4; i++) j->comp[i].pred = 0;
  j->data_end = FALSE;
  if (!j->marker) {
    /* look for the marker (the entropy coded segment may have junk or be short) */
    while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) {
      if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && j->d[j->pos + 1] != 0xFF) return;      /* some other marker: leave it */
      j->pos++;
    }
  }
  if (j->pos + 1 < j->len && j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7) { j->pos += 2; j->marker = 0; }
  else if (j->marker >= 0xD0 && j->marker <= 0xD7) { j->pos += 2; j->marker = 0; }
}

/* ---- one block ------------------------------------------------------------------------------------------------------------------------------------------- */
static BOOL so_jpeg_block(so_jpeg *j, so_jcomp *c, short *blk, int ss, int se, int ah, int al) {
  if (!j->progressive) {
    int t = so_jpeg_decode(j, &j->dc[c->dc_table]);
    if (t < 0 || t > 16) return FALSE;
    c->pred += so_jpeg_extend(j, t);
    blk[0] = (short)c->pred;
    for (int k = 1; k < 64;) {
      int rs = so_jpeg_decode(j, &j->ac[c->ac_table]);
      if (rs < 0) return FALSE;
      int r = rs >> 4, s = rs & 15;
      if (s == 0) {
        if (r != 15) break;
        k += 16;
        continue;
      }
      k += r;
      if (k > 63) return FALSE;
      blk[so_jpeg_zigzag[k]] = (short)so_jpeg_extend(j, s);
      k++;
    }
    return !j->data_end;
  }
  if (ss == 0) {
    if (ah == 0) {
      int t = so_jpeg_decode(j, &j->dc[c->dc_table]);
      if (t < 0 || t > 16) return FALSE;
      c->pred += so_jpeg_extend(j, t);
      blk[0] = (short)(c->pred * (1 << al));
    } else if (so_jpeg_get_bit(j)) blk[0] |= (short)(1 << al);
    return !j->data_end;
  }
  if (ah == 0) {
    if (j->eobrun > 0) { j->eobrun--; return TRUE; }
    for (int k = ss; k <= se;) {
      int rs = so_jpeg_decode(j, &j->ac[c->ac_table]);
      if (rs < 0) return FALSE;
      int r = rs >> 4, s = rs & 15;
      if (s == 0) {
        if (r < 15) {
          j->eobrun = (1 << r) - 1;
          if (r) j->eobrun += so_jpeg_get_bits(j, r);
          break;
        }
        k += 16;
        continue;
      }
      k += r;
      if (k > 63) return FALSE;
      blk[so_jpeg_zigzag[k]] = (short)(so_jpeg_extend(j, s) * (1 << al));
      k++;
    }
    return !j->data_end;
  }
  /* AC refinement */
  int p1 = 1 << al, m1 = -(1 << al), k = ss;
  if (j->eobrun <= 0) {
    for (; k <= se; k++) {
      int rs = so_jpeg_decode(j, &j->ac[c->ac_table]);
      if (rs < 0) return FALSE;
      int r = rs >> 4, s = rs & 15;
      if (s == 0) {
        if (r < 15) {
          j->eobrun = 1 << r;
          if (r) j->eobrun += so_jpeg_get_bits(j, r);
          break;
        }
      } else {
        if (s != 1) return FALSE;
        s = so_jpeg_get_bit(j) ? p1 : m1;
      }
      do {
        short *coef = &blk[so_jpeg_zigzag[k]];
        if (*coef != 0) {
          if (so_jpeg_get_bit(j) && (*coef & p1) == 0) *coef = (short)(*coef + (*coef >= 0 ? p1 : m1));
        } else if (--r < 0) break;
        k++;
      } while (k <= se);
      if (s && k <= se) blk[so_jpeg_zigzag[k]] = (short)s;
    }
  }
  if (j->eobrun > 0) {
    for (; k <= se; k++) {
      short *coef = &blk[so_jpeg_zigzag[k]];
      if (*coef != 0 && so_jpeg_get_bit(j) && (*coef & p1) == 0) *coef = (short)(*coef + (*coef >= 0 ? p1 : m1));
    }
    j->eobrun--;
  }
  return !j->data_end;
}

/* ---- a scan -------------------------------------------------------------------------------------------------------------------------------------------- */
static BOOL so_jpeg_scan(so_jpeg *j, const BYTE *hdr, int hdr_len) {
  if (!j->have_frame || hdr_len < 1) return FALSE;
  int ns = hdr[0];
  if (ns < 1 || ns > 4 || hdr_len < 1 + 2 * ns + 3) return FALSE;
  so_jcomp *sc[4];
  for (int i = 0; i < ns; i++) {
    int id = hdr[1 + 2 * i], td = hdr[2 + 2 * i] >> 4, ta = hdr[2 + 2 * i] & 15;
    sc[i] = NULL;
    for (int k = 0; k < j->ncomp; k++) if (j->comp[k].id == id) sc[i] = &j->comp[k];
    if (!sc[i] || td > 3 || ta > 3) return FALSE;
    sc[i]->dc_table = td; sc[i]->ac_table = ta;
  }
  int ss = hdr[1 + 2 * ns], se = hdr[2 + 2 * ns], ah = hdr[3 + 2 * ns] >> 4, al = hdr[3 + 2 * ns] & 15;
  if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
  else if (ss > se || se > 63 || al > 13 || (ss == 0 && se != 0) || (ss > 0 && ns != 1)) return FALSE;
  for (int i = 0; i < ns; i++) {
    if (ss == 0 && ah == 0 && !j->dc[sc[i]->dc_table].present) return FALSE;
    if ((ss > 0 || !j->progressive) && !j->ac[sc[i]->ac_table].present) return FALSE;
    if (!j->progressive && !j->dc[sc[i]->dc_table].present) return FALSE;
  }
  j->bit_count = 0; j->marker = 0; j->data_end = FALSE; j->eobrun = 0;
  for (int i = 0; i < 4; i++) j->comp[i].pred = 0;

  int total, restart_left = j->restart_interval;
  if (ns == 1) total = sc[0]->real_w * sc[0]->real_h;
  else total = j->mcus_x * j->mcus_y;
  for (int unit = 0; unit < total; unit++) {
    if (j->restart_interval && restart_left == 0) {
      so_jpeg_restart(j);
      restart_left = j->restart_interval;
    }
    if (ns == 1) {
      so_jcomp *c = sc[0];
      int bx = unit % c->real_w, by = unit / c->real_w;
      if (!so_jpeg_block(j, c, c->coef + ((SIZE_T)by * (SIZE_T)c->blocks_w + (SIZE_T)bx) * 64, ss, se, ah, al)) return TRUE;
    } else {
      int mx = unit % j->mcus_x, my = unit / j->mcus_x;
      for (int i = 0; i < ns; i++) {
        so_jcomp *c = sc[i];
        for (int vv = 0; vv < c->v; vv++)
          for (int hh = 0; hh < c->h; hh++) {
            int bx = mx * c->h + hh, by = my * c->v + vv;
            if (!so_jpeg_block(j, c, c->coef + ((SIZE_T)by * (SIZE_T)c->blocks_w + (SIZE_T)bx) * 64, ss, se, ah, al)) return TRUE;
          }
      }
    }
    if (j->restart_interval) restart_left--;
  }
  return TRUE;
}

/* ---- the inverse DCT (the accurate integer method: 13 bit constants) --------------------------------------------------------------------------------------------- */
#define SO_FIX_0_298631336 2446
#define SO_FIX_0_390180644 3196
#define SO_FIX_0_541196100 4433
#define SO_FIX_0_765366865 6270
#define SO_FIX_0_899976223 7373
#define SO_FIX_1_175875602 9633
#define SO_FIX_1_501321110 12299
#define SO_FIX_1_847759065 15137
#define SO_FIX_1_961570560 16069
#define SO_FIX_2_053119869 16819
#define SO_FIX_2_562915447 20995
#define SO_FIX_3_072711026 25172

/* One 8 point inverse transform of in[0], in[stride], ... into out[0..7], descaled by `shift`. The intermediate values are 64 bit so that corrupt coefficients cannot overflow. */
static int so_idct_sat(LONGLONG v) { return v > 0x3FFFFFFF ? 0x3FFFFFFF : v < -0x3FFFFFFF ? -0x3FFFFFFF : (int)v; }
static void so_idct_1d(const int *in, int stride, int *out, int shift) {
  #define SO_DESCALE(x) so_idct_sat(((x) + ((LONGLONG)1 << (shift - 1))) >> shift)
  LONGLONG z2 = in[2 * stride], z3 = in[6 * stride];
  LONGLONG z1 = (z2 + z3) * SO_FIX_0_541196100;
  LONGLONG tmp2 = z1 + z3 * -SO_FIX_1_847759065;
  LONGLONG tmp3 = z1 + z2 * SO_FIX_0_765366865;
  z2 = in[0]; z3 = in[4 * stride];
  LONGLONG tmp0 = (z2 + z3) * 8192, tmp1 = (z2 - z3) * 8192;
  LONGLONG tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
  tmp0 = in[7 * stride]; tmp1 = in[5 * stride]; tmp2 = in[3 * stride]; tmp3 = in[stride];
  z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2;
  LONGLONG z4 = tmp1 + tmp3, z5 = (z3 + z4) * SO_FIX_1_175875602;
  tmp0 *= SO_FIX_0_298631336; tmp1 *= SO_FIX_2_053119869; tmp2 *= SO_FIX_3_072711026; tmp3 *= SO_FIX_1_501321110;
  z1 *= -SO_FIX_0_899976223; z2 *= -SO_FIX_2_562915447; z3 *= -SO_FIX_1_961570560; z4 *= -SO_FIX_0_390180644;
  z3 += z5; z4 += z5;
  tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
  out[0] = SO_DESCALE(tmp10 + tmp3); out[7] = SO_DESCALE(tmp10 - tmp3);
  out[1] = SO_DESCALE(tmp11 + tmp2); out[6] = SO_DESCALE(tmp11 - tmp2);
  out[2] = SO_DESCALE(tmp12 + tmp1); out[5] = SO_DESCALE(tmp12 - tmp1);
  out[3] = SO_DESCALE(tmp13 + tmp0); out[4] = SO_DESCALE(tmp13 - tmp0);
  #undef SO_DESCALE
}
static void so_idct_block(const short *coef, const WORD *q, BYTE *out, SIZE_T out_stride) {
  int deq[64], ws[64], column[8], row[8];
  for (int i = 0; i < 64; i++) { int v = (int)coef[i] * (int)q[i]; deq[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
  for (int c = 0; c < 8; c++) {
    so_idct_1d(deq + c, 8, column, 13 - 2);
    for (int r = 0; r < 8; r++) ws[r * 8 + c] = column[r];
  }
  for (int r = 0; r < 8; r++) {
    so_idct_1d(ws + r * 8, 1, row, 13 + 2 + 3);
    for (int c = 0; c < 8; c++) {
      int v = row[c] > 4096 ? 4096 : row[c] < -4096 ? -4096 : row[c];
      v += 128;
      out[(SIZE_T)r * out_stride + (SIZE_T)c] = (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
}

/* A sample of a component at an image pixel: components with fewer samples are interpolated linearly (centered, like the "fancy" upsampling of libjpeg). */
static int so_jpeg_sample(const so_jpeg *j, const so_jcomp *c, int x, int y) {
  SIZE_T stride = (SIZE_T)c->blocks_w * 8;
  if (c->h == j->hmax && c->v == j->vmax) return c->plane[(SIZE_T)y * stride + (SIZE_T)x];
  int cw = (j->width * c->h + j->hmax - 1) / j->hmax, ch = (j->height * c->v + j->vmax - 1) / j->vmax;
  int fx = ((2 * x + 1) * c->h * 256) / (2 * j->hmax) - 128, fy = ((2 * y + 1) * c->v * 256) / (2 * j->vmax) - 128;
  if (fx < 0) fx = 0;
  if (fy < 0) fy = 0;
  int x0 = fx >> 8, y0 = fy >> 8, wx = fx & 255, wy = fy & 255, x1 = x0 + 1, y1 = y0 + 1;
  if (x0 >= cw) x0 = cw - 1;
  if (x1 >= cw) x1 = cw - 1;
  if (y0 >= ch) y0 = ch - 1;
  if (y1 >= ch) y1 = ch - 1;
  int top = c->plane[(SIZE_T)y0 * stride + (SIZE_T)x0] * (256 - wx) + c->plane[(SIZE_T)y0 * stride + (SIZE_T)x1] * wx;
  int bottom = c->plane[(SIZE_T)y1 * stride + (SIZE_T)x0] * (256 - wx) + c->plane[(SIZE_T)y1 * stride + (SIZE_T)x1] * wx;
  return (top * (256 - wy) + bottom * wy + 32768) >> 16;
}

static BYTE so_clamp8(int v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* ---- the whole image --------------------------------------------------------------------------------------------------------------------------------------- */
static BOOL so_decode_jpeg(const BYTE *data, SIZE_T len, so_dib *out) {
  so_jpeg *j = oa_calloc(sizeof(*j));
  BOOL ok = FALSE, scanned = FALSE;
  memset(out, 0, sizeof(*out));
  if (!j) return FALSE;
  j->d = data; j->len = len; j->adobe_transform = -1;
  if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) goto done;
  j->pos = 2;
  for (;;) {
    /* the next marker */
    while (j->pos < len && data[j->pos] != 0xFF) j->pos++;
    while (j->pos < len && data[j->pos] == 0xFF) j->pos++;
    if (j->pos >= len) break;
    int marker = data[j->pos++];
    if (marker == 0xD9) break;
    if (marker == 0 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) continue;
    if (len - j->pos < 2) break;
    int seg = (data[j->pos] << 8) | data[j->pos + 1];
    if (seg < 2 || (SIZE_T)seg > len - j->pos) break;
    const BYTE *p = data + j->pos + 2;
    int n = seg - 2;
    if (marker == 0xDB) {
      while (n >= 1) {
        int pq = p[0] >> 4, tq = p[0] & 15;
        int need = 1 + (pq ? 128 : 64);
        if (tq > 3 || n < need) goto done;
        for (int i = 0; i < 64; i++) j->qt[tq][so_jpeg_zigzag[i]] = pq ? (WORD)((p[1 + 2 * i] << 8) | p[2 + 2 * i]) : p[1 + i];
        p += need; n -= need;
      }
    } else if (marker == 0xC4) {
      while (n >= 17) {
        int tc = p[0] >> 4, th = p[0] & 15, total = 0;
        if (tc > 1 || th > 3) goto done;
        for (int i = 1; i <= 16; i++) total += p[i];
        if (total > 256 || n < 17 + total) goto done;
        if (!so_jpeg_build_huff(tc ? &j->ac[th] : &j->dc[th], p + 1, p + 17, total)) goto done;
        p += 17 + total; n -= 17 + total;
      }
    } else if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      if (j->have_frame || n < 6 || p[0] != 8) goto done;
      j->progressive = marker == 0xC2;
      j->height = (p[1] << 8) | p[2]; j->width = (p[3] << 8) | p[4]; j->ncomp = p[5];
      if (j->width <= 0 || j->height <= 0 || (j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) || n < 6 + 3 * j->ncomp) goto done;
      for (int i = 0; i < j->ncomp; i++) {
        so_jcomp *c = &j->comp[i];
        c->id = p[6 + 3 * i]; c->h = p[7 + 3 * i] >> 4; c->v = p[7 + 3 * i] & 15; c->tq = p[8 + 3 * i];
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) goto done;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
      }
      if (j->ncomp == 1) { j->comp[0].h = j->comp[0].v = 1; j->hmax = j->vmax = 1; }      /* (a single component has no interleaving: its blocks are the MCUs) */
      j->mcus_x = (j->width + 8 * j->hmax - 1) / (8 * j->hmax);
      j->mcus_y = (j->height + 8 * j->vmax - 1) / (8 * j->vmax);
      ULONGLONG total = 0;
      for (int i = 0; i < j->ncomp; i++) {
        so_jcomp *c = &j->comp[i];
        c->blocks_w = j->mcus_x * c->h; c->blocks_h = j->mcus_y * c->v;
        c->real_w = (((j->width * c->h + j->hmax - 1) / j->hmax) + 7) / 8;
        c->real_h = (((j->height * c->v + j->vmax - 1) / j->vmax) + 7) / 8;
        total += (ULONGLONG)c->blocks_w * (ULONGLONG)c->blocks_h * 64 * 3;
      }
      if (total > SO_MAX_BLOB || (ULONGLONG)j->width * (ULONGLONG)j->height > ((ULONGLONG)len + 1024) * 4096) goto done;
      for (int i = 0; i < j->ncomp; i++) {
        so_jcomp *c = &j->comp[i];
        SIZE_T blocks = (SIZE_T)c->blocks_w * (SIZE_T)c->blocks_h;
        c->coef = oa_calloc(blocks * 64 * sizeof(short));
        c->plane = oa_calloc(blocks * 64);
        if (!c->coef || !c->plane) goto done;
      }
      j->have_frame = TRUE;
    } else if ((marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)) {
      goto done;                                           /* lossless, arithmetic, differential: not supported */
    } else if (marker == 0xDD) {
      if (n >= 2) j->restart_interval = (p[0] << 8) | p[1];
    } else if (marker == 0xE0 && n >= 12 && p[0] == 'J' && p[1] == 'F' && p[2] == 'I' && p[3] == 'F') {
      j->density_unit = p[7]; j->x_density = (p[8] << 8) | p[9]; j->y_density = (p[10] << 8) | p[11];
    } else if (marker == 0xEE && n >= 12 && p[0] == 'A' && p[1] == 'd' && p[2] == 'o' && p[3] == 'b' && p[4] == 'e') {
      j->adobe_transform = p[11];
    } else if (marker == 0xDA) {
      j->pos += (SIZE_T)seg;                              /* (the entropy coded data follows the scan header) */
      if (!so_jpeg_scan(j, p, n)) goto done;
      scanned = TRUE;
      continue;                                            /* (pos is in the data after the scan: the loop finds the next marker) */
    }
    j->pos += (SIZE_T)seg;
  }
  if (!j->have_frame || !scanned) goto done;

  /* the samples of each component */
  for (int i = 0; i < j->ncomp; i++) {
    so_jcomp *c = &j->comp[i];
    const WORD *q = j->qt[c->tq];
    SIZE_T plane_stride = (SIZE_T)c->blocks_w * 8;
    for (int by = 0; by < c->blocks_h; by++)
      for (int bx = 0; bx < c->blocks_w; bx++)
        so_idct_block(c->coef + ((SIZE_T)by * (SIZE_T)c->blocks_w + (SIZE_T)bx) * 64, q, c->plane + (SIZE_T)by * 8 * plane_stride + (SIZE_T)bx * 8, plane_stride);
  }
  if (!so_dib_alloc(out, j->width, j->height, 24)) goto done;
  if (j->density_unit == 1 && j->x_density > 0 && j->y_density > 0) { out->x_ppm = (LONG)((ULONGLONG)j->x_density * 3937 / 100); out->y_ppm = (LONG)((ULONGLONG)j->y_density * 3937 / 100); }
  else if (j->density_unit == 2 && j->x_density > 0 && j->y_density > 0) { out->x_ppm = j->x_density * 100; out->y_ppm = j->y_density * 100; }
  BOOL ycc = j->ncomp == 3 ? (j->adobe_transform != 0 && !(j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B')) : j->adobe_transform == 2;
  for (int y = 0; y < j->height; y++) {
    BYTE *dst = out->bits + (SIZE_T)(j->height - 1 - y) * out->stride;
    for (int x = 0; x < j->width; x++) {
      int s[4] = {0, 0, 0, 0};
      for (int i = 0; i < j->ncomp; i++) {
        s[i] = so_jpeg_sample(j, &j->comp[i], x, y);
      }
      int r, g, b;
      if (j->ncomp == 1) r = g = b = s[0];
      else {
        if (ycc) {
          int cb = s[1] - 128, cr = s[2] - 128;
          r = so_clamp8(s[0] + ((91881 * cr + 32768) >> 16));
          g = so_clamp8(s[0] - ((22554 * cb + 46802 * cr + 32768) >> 16));
          b = so_clamp8(s[0] + ((116130 * cb + 32768) >> 16));
        } else { r = s[0]; g = s[1]; b = s[2]; }
        if (j->ncomp == 4) {                               /* (Adobe CMYK is stored inverted) */
          r = r * s[3] / 255; g = g * s[3] / 255; b = b * s[3] / 255;
        }
      }
      dst[3 * x] = (BYTE)b; dst[3 * x + 1] = (BYTE)g; dst[3 * x + 2] = (BYTE)r;
    }
  }
  ok = TRUE;
done:
  so_jpeg_free(j);
  oa_free(j);
  if (!ok) so_dib_free(out);
  return ok;
}
