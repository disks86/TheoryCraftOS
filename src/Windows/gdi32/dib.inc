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
/* gdi32.dll: bitmaps and device-independent bitmaps. Included by gdi32.c. */

/* What the header of a BITMAPINFO says. Returns 0 if it is not a header Windows accepts. */
typedef struct DibHeader { LONG width, height; WORD planes, bpp; DWORD compression, image_size, colors_used, header_size, color_entry; } DibHeader;

static int dib_parse(const BITMAPINFO *bmi, UINT usage, DibHeader *h) {
  if (!bmi) return 0;
  const DWORD size = bmi->bmiHeader.biSize;
  if (size == sizeof(BITMAPCOREHEADER)) {
    const BITMAPCOREHEADER *c = (const BITMAPCOREHEADER *)bmi;
    h->width = c->bcWidth; h->height = c->bcHeight; h->planes = c->bcPlanes; h->bpp = c->bcBitCount;
    h->compression = BI_RGB; h->image_size = 0; h->colors_used = 0; h->header_size = size; h->color_entry = usage == DIB_PAL_COLORS ? 2 : 3;
  } else if (size >= sizeof(BITMAPINFOHEADER) && (size == sizeof(BITMAPINFOHEADER) || size == 52 || size == 56 || size == 64 || size == sizeof(BITMAPV4HEADER) || size == sizeof(BITMAPV5HEADER))) {
    h->width = bmi->bmiHeader.biWidth; h->height = bmi->bmiHeader.biHeight; h->planes = bmi->bmiHeader.biPlanes; h->bpp = bmi->bmiHeader.biBitCount;
    h->compression = bmi->bmiHeader.biCompression; h->image_size = bmi->bmiHeader.biSizeImage; h->colors_used = bmi->bmiHeader.biClrUsed; h->header_size = size;
    h->color_entry = usage == DIB_PAL_COLORS ? 2 : 4;
  } else return 0;
  if (h->planes != 1) return 0;
  switch (h->bpp) { case 1: case 4: case 8: case 16: case 24: case 32: break; default: return 0; }
  return 1;
}
static DWORD dib_color_count(const DibHeader *h) {
  if (h->bpp <= 8) { const DWORD max = 1u << h->bpp; return h->colors_used && h->colors_used <= max ? h->colors_used : max; }
  return h->colors_used > 0x10000 ? 0x10000 : h->colors_used;
}
/* The bytes of header plus colour table (and the three masks that follow a plain 40-byte header for BI_BITFIELDS). */
static DWORD dib_info_bytes(const DibHeader *h) {
  DWORD n = h->header_size + dib_color_count(h) * h->color_entry;
  if (h->header_size == sizeof(BITMAPINFOHEADER) && (h->compression == BI_BITFIELDS || h->compression == 6 /* BI_ALPHABITFIELDS */)) n += h->compression == 6 ? 16 : 12;
  return n;
}
/* The bytes of pixels; (DWORD)-1 if absurd. */
static DWORD dib_image_bytes(const DibHeader *h) {
  if (h->compression != BI_RGB && h->compression != BI_BITFIELDS && h->compression != 6) return h->image_size;
  const LONGLONG w = h->width < 0 ? -(LONGLONG)h->width : h->width, ht = h->height < 0 ? -(LONGLONG)h->height : h->height;
  const LONGLONG row = ((w * h->bpp + 31) / 32) * 4;
  const LONGLONG total = row * ht;
  return total > 0x1fffffff ? (DWORD)-1 : (DWORD)total;
}
static DWORD dib_packed_size(const BITMAPINFO *bmi, UINT usage) {
  DibHeader h;
  if (!dib_parse(bmi, usage, &h)) return 0;
  const DWORD image = dib_image_bytes(&h);
  return image == (DWORD)-1 ? 0 : dib_info_bytes(&h) + image;
}

/* ---- DDBs --------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HBITMAP WINAPI CreateBitmap(int width, int height, UINT planes, UINT bpp, const void *bits) {
  if (width < 0 || height < 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (width > 0x7ffffff || height > 0x7ffffff) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (!width || !height) { width = height = 1; planes = 1; bpp = 1; bits = NULL; }
  HBITMAP bm = NtGdiCreateBitmap(width, height, planes, bpp, bits);
  if (!bm) SetLastError(planes > 1 && bpp == 0 ? ERROR_INVALID_PARAMETER : ERROR_NOT_ENOUGH_MEMORY);
  return bm;
}
EXPORT HBITMAP WINAPI CreateBitmapIndirect(const BITMAP *bm) {
  if (!bm) return NULL;
  return CreateBitmap(bm->bmWidth, bm->bmHeight, bm->bmPlanes, bm->bmBitsPixel, bm->bmBits);
}
EXPORT HBITMAP WINAPI CreateCompatibleBitmap(HDC hdc, int width, int height) {
  if (width < 0 || height < 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (!width || !height) return NtGdiCreateBitmap(1, 1, 1, 1, NULL);
  return NtGdiCreateCompatibleBitmap(RD(hdc), width, height);
}
EXPORT HBITMAP WINAPI CreateDiscardableBitmap(HDC hdc, int width, int height) { return CreateCompatibleBitmap(hdc, width, height); }
EXPORT LONG WINAPI GetBitmapBits(HBITMAP bm, LONG count, LPVOID bits) { return NtGdiGetBitmapBits(bm, count, bits); }
EXPORT LONG WINAPI SetBitmapBits(HBITMAP bm, DWORD count, const void *bits) { return NtGdiSetBitmapBits(bm, count, bits); }
EXPORT BOOL WINAPI GetBitmapDimensionEx(HBITMAP bm, SIZE *size) { return NtGdiGetBitmapDimension(bm, size); }
EXPORT BOOL WINAPI SetBitmapDimensionEx(HBITMAP bm, int w, int h, SIZE *old) { return NtGdiSetBitmapDimension(bm, w, h, old); }

/* ---- DIB sections ------------------------------------------------------------------------------------------------------------------------------------------- */

/* The kernel allocates the pixels in the program's address space and gives their address back; a file-mapping section as the storage is refused by the kernel. */
EXPORT HBITMAP WINAPI CreateDIBSection(HDC hdc, const BITMAPINFO *bmi, UINT usage, void **bits_out, HANDLE section, DWORD offset) {
  DibHeader h;
  void *bits = NULL;
  if (bits_out) *bits_out = NULL;
  if (!dib_parse(bmi, usage, &h) || !h.width || !h.height || h.width < 0 || (h.compression != BI_RGB && h.compression != BI_BITFIELDS) ||
      (h.compression == BI_BITFIELDS && h.bpp != 16 && h.bpp != 32) || usage > DIB_PAL_COLORS) return NULL;
  const DWORD image = dib_image_bytes(&h);
  if (image == (DWORD)-1 || image > 0x1fffffff) return NULL;
  HBITMAP bitmap = NtGdiCreateDIBSection(RD(hdc), section, offset, bmi, usage, dib_info_bytes(&h), 0, 0, &bits);
  if (!bitmap) return NULL;
  if (bits_out) *bits_out = bits;
  return bitmap;
}

/* ---- DIBs --------------------------------------------------------------------------------------------------------------------------------------------------- */

#define DIB_UNKNOWN_SIZE (~0u)      /* the max_info / max_bits arguments of the NtGdi DIB services: gdi32 does not know how much memory the program gave */

EXPORT HBITMAP WINAPI CreateDIBitmap(HDC hdc, const BITMAPINFOHEADER *header, DWORD init, const void *bits, const BITMAPINFO *data, UINT usage) {
  LONG width, height;
  if (!header) return NULL;
  if (header->biSize == sizeof(BITMAPCOREHEADER)) { const BITMAPCOREHEADER *c = (const BITMAPCOREHEADER *)header; width = c->bcWidth; height = c->bcHeight; }
  else { width = header->biWidth; height = header->biHeight; }
  if (height < 0) height = -height;
  if (width < 0 || usage > DIB_PAL_COLORS + 1) return NULL;
  if (!width || !height) return NtGdiCreateBitmap(1, 1, 1, 1, NULL);
  return NtGdiCreateDIBitmapInternal(RD(hdc), width, height, init, bits, data, usage, DIB_UNKNOWN_SIZE, DIB_UNKNOWN_SIZE, 0, NULL);
}
EXPORT int WINAPI GetDIBits(HDC hdc, HBITMAP bitmap, UINT start, UINT lines, LPVOID bits, LPBITMAPINFO info, UINT usage) {
  if (usage > DIB_PAL_COLORS) return 0;
  if (bits && info) { DibHeader h; if (!dib_parse(info, usage, &h) || !h.width || h.width < 0 || !h.height) return 0; }
  return NtGdiGetDIBitsInternal(RD(hdc), bitmap, start, lines, bits, info, usage, DIB_UNKNOWN_SIZE, DIB_UNKNOWN_SIZE);
}
EXPORT int WINAPI SetDIBitsToDevice(HDC hdc, int xd, int yd, DWORD cx, DWORD cy, int xs, int ys, UINT start, UINT lines, const void *bits, const BITMAPINFO *info, UINT usage) { if (is_meta_dc(hdc)) return meta_dibs(hdc, 0, xd, yd, cx, cy, xs, ys, cx, cy, start, lines, bits, info, usage, 0) ? (int)lines : 0;
  DibHeader h;
  if (usage > DIB_PAL_COLORS + 1 || !dib_parse(info, usage, &h) || h.width <= 0 || !h.height) return 0;
  return NtGdiSetDIBitsToDeviceInternal(RD(hdc), xd, yd, cx, cy, xs, ys, start, lines, bits, info, usage, DIB_UNKNOWN_SIZE, DIB_UNKNOWN_SIZE, FALSE, NULL);
}
EXPORT int WINAPI StretchDIBits(HDC hdc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, const void *bits, const BITMAPINFO *info, UINT usage, DWORD rop) { if (is_meta_dc(hdc)) return meta_dibs(hdc, 1, xd, yd, wd, hd, xs, ys, ws, hs, 0, 0, bits, info, usage, rop) ? hs : (int)GDI_ERROR;
  DibHeader h;
  if (usage > DIB_PAL_COLORS + 1 || !dib_parse(info, usage, &h) || h.width <= 0 || !h.height) return 0;
  return NtGdiStretchDIBitsInternal(RD(hdc), xd, yd, wd, hd, xs, ys, ws, hs, bits, info, usage, rop, DIB_UNKNOWN_SIZE, DIB_UNKNOWN_SIZE, NULL);
}
EXPORT int WINAPI SetDIBits(HDC hdc, HBITMAP bitmap, UINT start, UINT lines, const void *bits, const BITMAPINFO *info, UINT usage) {
  DibHeader h;
  BITMAP bm;
  HDC mem;
  HGDIOBJ previous;
  int result = 0;
  if (!bits || usage > DIB_PAL_COLORS + 1 || !dib_parse(info, usage, &h) || h.width <= 0 || !h.height) return 0;
  if (!NtGdiExtGetObjectW(bitmap, sizeof(bm), &bm)) return 0;
  if (!(mem = NtGdiCreateCompatibleDC(hdc))) return 0;
  previous = NtGdiSelectBitmap(mem, bitmap);
  if (previous) {
    const LONG height = h.height < 0 ? -h.height : h.height;
    result = NtGdiSetDIBitsToDeviceInternal(mem, 0, 0, h.width < 0 ? -h.width : h.width, height, 0, 0, start, lines, bits, info, usage, DIB_UNKNOWN_SIZE, DIB_UNKNOWN_SIZE, FALSE, NULL);
    NtGdiSelectBitmap(mem, previous);
  }
  NtGdiDeleteObjectApp(mem);
  return result;
}
EXPORT UINT WINAPI GetDIBColorTable(HDC hdc, UINT start, UINT count, RGBQUAD *colors) { return NtGdiDoPalette(RD(hdc), (WORD)start, (WORD)count, colors, NtGdiGetDIBColorTable, FALSE); }
EXPORT UINT WINAPI SetDIBColorTable(HDC hdc, UINT start, UINT count, const RGBQUAD *colors) { return NtGdiDoPalette(RD(hdc), (WORD)start, (WORD)count, (void *)colors, NtGdiSetDIBColorTable, TRUE); }
EXPORT DWORD WINAPI GdiGetBitmapBitsSize(const BITMAPINFO *info) { DibHeader h; if (!dib_parse(info, DIB_RGB_COLORS, &h)) return 0; const DWORD n = dib_image_bytes(&h); return n == (DWORD)-1 ? 0 : n; }
