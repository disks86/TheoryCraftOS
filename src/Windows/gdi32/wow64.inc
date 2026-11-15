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

/* gdi32.dll for i386: the GDI services whose data a 32-bit program lays out differently (docs/Wow64.md). Included by gdi32.c when it is built for i386,
 * after the service stubs. The kernel reads and writes the 64-bit layouts; the rest of gdi32.dll (and the program) has the 32-bit ones. Each
 * function here stands in for the stub of the same service, by the #define after it.
 *   - GetObject of a bitmap (BITMAP: the bits' address; DIBSECTION: the section's handle), of a brush (LOGBRUSH: the hatch is pointer-sized) and of
 *     an extended pen (EXTLOGPEN: the same);
 *   - CreateDIBSection: the kernel writes the address of the bits as 8 bytes;
 *   - opening a device context by the device's name: two UNICODE_STRINGs. */
static UINT gdi_wow_u32(const BYTE *at) { UINT v; memcpy(&v, at, 4); return v; }

static INT gdi_wow_get_object(HGDIOBJ handle, INT count, void *buffer) {
  const DWORD type = (DWORD)(ULONG_PTR)handle & NTGDI_HANDLE_TYPE_MASK;
  BYTE wide[128], narrow[128];
  INT size;
  if (type != NTGDI_OBJ_BITMAP && type != NTGDI_OBJ_BRUSH && type != NTGDI_OBJ_EXTPEN) return NtGdiExtGetObjectW(handle, count, buffer);
  memset(wide, 0, sizeof(wide));
  const INT got = NtGdiExtGetObjectW(handle, sizeof(wide), wide);
  if (got <= 0) return got;
  if (type == NTGDI_OBJ_BITMAP) {      /* 32 bytes (BITMAP) or 104 (DIBSECTION) there; 24 or 84 here */
    memcpy(narrow, wide, 20);                       /* bmType ... bmBitsPixel */
    memcpy(narrow + 20, wide + 24, 4);              /* bmBits */
    size = 24;
    if (got >= 104 && (!buffer || count >= 84)) {
      memcpy(narrow + 24, wide + 32, 40);           /* dsBmih */
      memcpy(narrow + 64, wide + 72, 12);           /* dsBitfields */
      memcpy(narrow + 76, wide + 88, 4);            /* dshSection */
      memcpy(narrow + 80, wide + 96, 4);            /* dsOffset */
      size = 84;
    }
  } else if (type == NTGDI_OBJ_BRUSH) {      /* LOGBRUSH: 16 bytes there, 12 here */
    memcpy(narrow, wide, 12);
    size = 12;
  } else {                                    /* EXTLOGPEN: the hatch is 8 bytes there (at 16), so everything after it is 4 bytes further on */
    if (got < 32) return 0;
    memcpy(narrow, wide, 20);
    memcpy(narrow + 20, wide + 24, (size_t)got - 24);
    size = got - 4;
  }
  (void)gdi_wow_u32;
  if (!buffer) return size;
  if (count < size) size = count < 0 ? 0 : count;
  memcpy(buffer, narrow, (size_t)size);
  return size;
}
#define NtGdiExtGetObjectW gdi_wow_get_object

static HBITMAP gdi_wow_create_dib_section(HDC hdc, HANDLE section, DWORD offset, const BITMAPINFO *bmi, UINT usage, UINT header_size, ULONG flags, ULONG_PTR color_space, void **bits) {
  ULONGLONG wide = 0;
  const HBITMAP bitmap = NtGdiCreateDIBSection(hdc, section, offset, bmi, usage, header_size, flags, color_space, bits ? (void **)&wide : NULL);
  if (bitmap && bits) *bits = (void *)(ULONG_PTR)wide;
  return bitmap;
}
#define NtGdiCreateDIBSection gdi_wow_create_dib_section

typedef struct { USHORT length, maximum; ULONG pad; ULONGLONG buffer; } GdiWowString;
static UNICODE_STRING *gdi_wow_string(GdiWowString *out, const UNICODE_STRING *in) {
  if (!in) return NULL;
  out->length = in->Length; out->maximum = in->MaximumLength; out->pad = 0; out->buffer = (ULONG_PTR)in->Buffer;
  return (UNICODE_STRING *)out;
}
static HDC gdi_wow_open_dc(UNICODE_STRING *device, const DEVMODEW *devmode, UNICODE_STRING *output, ULONG type, BOOL is_display, HANDLE hspool, DRIVER_INFO_2W *driver_info, void *pdev) {
  GdiWowString device64, output64;
  return NtGdiOpenDCW(gdi_wow_string(&device64, device), devmode, gdi_wow_string(&output64, output), type, is_display, hspool, driver_info, pdev);
}
#define NtGdiOpenDCW gdi_wow_open_dc
