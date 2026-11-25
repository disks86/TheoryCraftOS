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
/* Direct3D's 64 KiB standard swizzle (D3D11_TEXTURE_LAYOUT_64K_STANDARD_SWIZZLE, D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE), shared by d3d11.dll and d3d12.dll.
 *
 * A subresource is laid out in 64 KiB tiles, in row-major order of the tiles (across, then down, then deep), with no padding between them; within a tile an element's offset
 * interleaves the bits of its coordinates as Direct3D's patterns say (D3D12_TEXTURE_LAYOUT's documentation, the 2D and 3D tables by bits per element): the bits of the
 * x offset in bytes go where the pattern has X or '-' (the bytes within the element), those of y where it has Y, of z where it has Z, the first letter the most significant
 * bit. The tile shapes follow: 256 x 256 elements of 8 bits down to 64 x 64 of 128 in 2D, 64 x 32 x 32 down to 16 x 16 x 16 in 3D. A block-compressed format's element is a
 * 4 x 4 block (8 or 16 bytes). Where the levels of a resource are packed is adapter-specific: here every level, however small, has tiles of its own (a level after the other,
 * in subresource order). */
#ifndef TCOS_SWIZZLE_H
#define TCOS_SWIZZLE_H

#include <windows.h>
#include <string.h>

typedef struct TcosSwizzle {
  UINT xmask, ymask, zmask;    /* where the x offset (in bytes), y and z go in the 16 bits of a tile's offset */
  UINT tw, th, td;             /* the tile's elements across, down and deep */
  UINT bpe;                    /* bytes an element */
} TcosSwizzle;

/* The pattern of `bpe` bytes an element (1, 2, 4, 8, 16) of a 2D or 3D texture. FALSE for another size. */
static BOOL tcos_swizzle_init(TcosSwizzle *s, UINT bpe, BOOL three_d) {
  static const char *const two[5] = {"XYXYXYXYYYYYXXXX", "XYXYXYXYXYYYXXX-", "XYXYXYXYXYYYXX--", "XYXYXYXYXXYYX---", "XYXYXYXYXXYY----"};
  static const char *const three[5] = {"XYZXYZYZZZYYXXXX", "XYZXYZYZZZYYXXX-", "XYZXYZXYZZYYXX--", "XYZXYZXXZZYYX---", "XYZXYZXXZZYY----"};
  const int index = bpe == 1 ? 0 : bpe == 2 ? 1 : bpe == 4 ? 2 : bpe == 8 ? 3 : bpe == 16 ? 4 : -1;
  memset(s, 0, sizeof *s);
  if (index < 0) return FALSE;
  const char *p = three_d ? three[index] : two[index];
  UINT xs = 0, ys = 0, zs = 0;
  for (UINT i = 0; i < 16; ++i) {
    const UINT bit = 1u << (15 - i);
    if (p[i] == 'X' || p[i] == '-') s->xmask |= bit;
    if (p[i] == 'X') ++xs;
    if (p[i] == 'Y') { s->ymask |= bit; ++ys; }
    if (p[i] == 'Z') { s->zmask |= bit; ++zs; }
  }
  s->tw = 1u << xs;
  s->th = 1u << ys;
  s->td = 1u << zs;
  s->bpe = bpe;
  return TRUE;
}

/* The bits of `v` put in the places `mask` has (the lowest bit of v in the lowest place): x86's PDEP. */
static UINT tcos_pdep(UINT v, UINT mask) {
  UINT out = 0;
  for (UINT bit = 1; mask; bit <<= 1) {
    const UINT lowest = mask & (0u - mask);
    if (v & bit) out |= lowest;
    mask &= mask - 1;
  }
  return out;
}

/* The tiles of a subresource of w x h x d elements across, down and deep (0 when the size is too large to lay out). */
static BOOL tcos_swizzle_tiles(const TcosSwizzle *s, UINT w, UINT h, UINT d, UINT *across, UINT *down, UINT *deep) {
  if (!s->tw || !w || !h || !d) return FALSE;
  *across = w / s->tw + (w % s->tw ? 1u : 0u);
  *down = h / s->th + (h % s->th ? 1u : 0u);
  *deep = d / s->td + (d % s->td ? 1u : 0u);
  return TRUE;
}
/* The bytes of such a subresource (0: too large for the address space). */
static SIZE_T tcos_swizzle_bytes(const TcosSwizzle *s, UINT w, UINT h, UINT d) {
  UINT a = 0, b = 0, c = 0;
  if (!tcos_swizzle_tiles(s, w, h, d, &a, &b, &c)) return 0;
  const unsigned long long tiles = (unsigned long long)a * b * c;
  if (tiles > ((SIZE_T)-1) / 65536u) return 0;
  return (SIZE_T)(tiles * 65536u);
}

/* The offset of element (x, y, z) of such a subresource. */
static SIZE_T tcos_swizzle_offset(const TcosSwizzle *s, UINT across, UINT down, UINT x, UINT y, UINT z) {
  const SIZE_T tile = ((SIZE_T)(z / s->td) * down + y / s->th) * across + x / s->tw;
  return tile * 65536u + (SIZE_T)(tcos_pdep((x % s->tw) * s->bpe, s->xmask) | tcos_pdep(y % s->th, s->ymask) | tcos_pdep(z % s->td, s->zmask));
}

/* Copies w x h x d elements between the swizzled subresource (`swizzled`, of `swizzled_bytes`, the subresource's size in elements full_w x full_h x full_d) at (x0, y0, z0) and
 * linear memory (rows `row_pitch` apart, slices `slice_pitch`). `to_swizzled`: from the linear memory into the subresource. FALSE when the box is not within the subresource. */
static BOOL tcos_swizzle_copy(const TcosSwizzle *s, BYTE *swizzled, SIZE_T swizzled_bytes, UINT full_w, UINT full_h, UINT full_d, UINT x0, UINT y0, UINT z0, UINT w, UINT h,
                              UINT d, BYTE *linear, SIZE_T row_pitch, SIZE_T slice_pitch, BOOL to_swizzled) {
  UINT across = 0, down = 0, deep = 0;
  if (!tcos_swizzle_tiles(s, full_w, full_h, full_d, &across, &down, &deep)) return FALSE;
  if (x0 > full_w || w > full_w - x0 || y0 > full_h || h > full_h - y0 || z0 > full_d || d > full_d - z0) return FALSE;
  if (swizzled_bytes < tcos_swizzle_bytes(s, full_w, full_h, full_d)) return FALSE;
  for (UINT z = 0; z < d; ++z)
    for (UINT y = 0; y < h; ++y) {
      BYTE *row = linear + (SIZE_T)z * slice_pitch + (SIZE_T)y * row_pitch;
      for (UINT x = 0; x < w; ++x) {
        BYTE *element = swizzled + tcos_swizzle_offset(s, across, down, x0 + x, y0 + y, z0 + z);
        if (to_swizzled) memcpy(element, row + (SIZE_T)x * s->bpe, s->bpe);
        else memcpy(row + (SIZE_T)x * s->bpe, element, s->bpe);
      }
    }
  return TRUE;
}

#endif
