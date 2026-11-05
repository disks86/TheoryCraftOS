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

/* Carets, cursors, icons and images of user32.dll (included by user32.c after window.inc). The objects are win32k's (src/Process/Win32k/Win32kUserMiscCursor.cpp: one kind of object for
 * HCURSOR and HICON, the images are GDI bitmaps it owns); the loading of images from resources and files, the creation from raw bits and the drawing are here, over gdi32 and kernel32.
 * Supported: system cursors and icons, icons and cursors from resources (RT_GROUP_ICON / RT_GROUP_CURSOR with DIB images) and from .ico / .cur files, bitmaps from resources and .bmp
 * files, CreateIcon / CreateCursor / CreateIconIndirect, GetIconInfo, CopyIcon, DrawIcon(Ex). Not done: PNG-compressed icons, animated cursors, colour-reduction flags of LoadImage. */

SVC NtUserCreateCaret(ULONG_PTR hwnd, ULONG_PTR bitmap, ULONG_PTR width, ULONG_PTR height);
SVC NtUserDestroyCaret(void);
SVC NtUserSetCaretPos(ULONG_PTR x, ULONG_PTR y);
SVC NtUserGetCaretPos(ULONG_PTR point);
SVC NtUserShowCaret(ULONG_PTR hwnd);
SVC NtUserHideCaret(ULONG_PTR hwnd);
SVC NtUserGetCaretBlinkTime(void);
SVC NtUserSetCaretBlinkTime(ULONG_PTR ms);
SVC NtUserSetCursorIconData(ULONG_PTR handle, ULONG_PTR module, ULONG_PTR name, ULONG_PTR data, ULONG_PTR a, ULONG_PTR b);
SVC NtUserFindExistingCursorIcon(ULONG_PTR module, ULONG_PTR name, ULONG_PTR rsrc);
SVC NtUserDestroyCursor(ULONG_PTR handle, ULONG_PTR force);
SVC NtUserGetIconInfo(ULONG_PTR handle, ULONG_PTR info, ULONG_PTR module, ULONG_PTR name, ULONG_PTR bpp, ULONG_PTR a);
SVC NtUserSetCursor(ULONG_PTR handle);
SVC NtUserGetCursor(void);
SVC NtUserShowCursor(ULONG_PTR show);
SVC NtUserGetCursorInfo(ULONG_PTR info);
SVC NtUserClipCursor(ULONG_PTR rect);
SVC NtUserGetClipCursor(ULONG_PTR rect);
IMPORT HANDLE WINAPI CreateBitmap(int width, int height, UINT planes, UINT bits, const void *data);
IMPORT HANDLE WINAPI CreateDIBitmap(HANDLE hdc, const void *header, DWORD init, const void *bits, const void *info, UINT usage);
IMPORT int WINAPI SetDIBits(HANDLE hdc, HANDLE bitmap, UINT start, UINT lines, const void *bits, const void *info, UINT usage);
IMPORT BOOL WINAPI StretchBlt(HANDLE hdc, int x, int y, int cx, int cy, HANDLE source, int sx, int sy, int sw, int sh, DWORD rop);
IMPORT HANDLE WINAPI FindResourceW(HANDLE module, LPCWSTR name, LPCWSTR type);
IMPORT HANDLE WINAPI LoadResource(HANDLE module, HANDLE resource);
IMPORT void *WINAPI LockResource(HANDLE resource);
IMPORT DWORD WINAPI SizeofResource(HANDLE module, HANDLE resource);
IMPORT HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags, HANDLE template_file);
IMPORT BOOL WINAPI ReadFile(HANDLE file, void *buffer, DWORD bytes, DWORD *read, void *overlapped);
IMPORT DWORD WINAPI GetFileSize(HANDLE file, DWORD *high);
IMPORT BOOL WINAPI CloseHandle(HANDLE handle);
EXPORT HANDLE WINAPI GetDC(HANDLE window);
EXPORT int WINAPI ReleaseDC(HANDLE window, HANDLE dc);
#define RT_CURSOR_ 1
#define RT_BITMAP_ 2
#define RT_ICON_ 3
#define RT_GROUP_CURSOR_ 12
#define RT_GROUP_ICON_ 14
#define LR_LOADFROMFILE_ 0x10
#define LR_SHARED_ 0x8000
#define LR_DEFAULTSIZE_ 0x40
#define SRCAND_ 0x008800C6u
#define SRCINVERT_ 0x00660046u
#define SRCPAINT_ 0x00EE0086u

/* ---- carets --------------------------------------------------------------------------------------------------------------------------------------------------------- */
EXPORT BOOL WINAPI CreateCaret(HANDLE hwnd, HANDLE bitmap, int width, int height) { return BOOL_OF(NtUserCreateCaret((ULONG_PTR)hwnd, (ULONG_PTR)bitmap, (ULONG_PTR)(LONG_PTR)width, (ULONG_PTR)(LONG_PTR)height)); }
EXPORT BOOL WINAPI DestroyCaret(void) { return BOOL_OF(NtUserDestroyCaret()); }
EXPORT BOOL WINAPI SetCaretPos(int x, int y) { return BOOL_OF(NtUserSetCaretPos((ULONG_PTR)(LONG_PTR)x, (ULONG_PTR)(LONG_PTR)y)); }
EXPORT BOOL WINAPI GetCaretPos(POINT_ *p) { return p && BOOL_OF(NtUserGetCaretPos((ULONG_PTR)p)); }
EXPORT BOOL WINAPI ShowCaret(HANDLE hwnd) { return BOOL_OF(NtUserShowCaret((ULONG_PTR)hwnd)); }
EXPORT BOOL WINAPI HideCaret(HANDLE hwnd) { return BOOL_OF(NtUserHideCaret((ULONG_PTR)hwnd)); }
EXPORT UINT WINAPI GetCaretBlinkTime(void) { return (UINT)NtUserGetCaretBlinkTime(); }
EXPORT BOOL WINAPI SetCaretBlinkTime(UINT ms) { return BOOL_OF(NtUserSetCaretBlinkTime(ms)); }

/* ---- cursor state ----------------------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HANDLE WINAPI SetCursor(HANDLE cursor) { return (HANDLE)NtUserSetCursor((ULONG_PTR)cursor); }
EXPORT HANDLE WINAPI GetCursor(void) { return (HANDLE)NtUserGetCursor(); }
EXPORT int WINAPI ShowCursor(BOOL show) { return (int)(LONG)NtUserShowCursor(show); }
EXPORT BOOL WINAPI GetCursorInfo(void *info) { return info && BOOL_OF(NtUserGetCursorInfo((ULONG_PTR)info)); }
EXPORT BOOL WINAPI ClipCursor(const RECT_ *r) { return BOOL_OF(NtUserClipCursor((ULONG_PTR)r)); }
EXPORT BOOL WINAPI GetClipCursor(RECT_ *r) { return r && BOOL_OF(NtUserGetClipCursor((ULONG_PTR)r)); }
EXPORT BOOL WINAPI DestroyIcon(HANDLE icon) { return BOOL_OF(NtUserDestroyCursor((ULONG_PTR)icon, 0)); }
EXPORT BOOL WINAPI DestroyCursor(HANDLE cursor) { return BOOL_OF(NtUserDestroyCursor((ULONG_PTR)cursor, 0)); }

/* ---- icon objects ------------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct { BOOL is_icon; DWORD hot_x, hot_y; HANDLE mask, color; } IconInfo_;
typedef struct { DWORD size; LONG width, height; WORD planes, bit_count; DWORD compression, size_image; LONG x_ppm, y_ppm; DWORD used, important; } BmiHeader_;
typedef struct { DWORD flags, steps, frames, delay; ULONG_PTR frames_at, sequence_at, rates_at, rsrc; } IconData_;
typedef struct { DWORD width, height; ULONG_PTR color, alpha, mask; LONG hot_x, hot_y; } IconFrame_;

/* A copy of a bitmap (for the new icon, which owns its images). */
static HANDLE copy_bitmap(HANDLE source) {
  Bitmap_ bm;
  if (!source || GetObjectW(source, sizeof bm, &bm) == 0) return NULL;
  HANDLE copy = CreateBitmap(bm.width, bm.height, bm.planes, bm.bits, NULL);
  HANDLE screen = GetDC(NULL), from = CreateCompatibleDC(screen), to = CreateCompatibleDC(screen);
  if (copy && from && to) {
    HANDLE old_from = SelectObject(from, source), old_to = SelectObject(to, copy);
    BitBlt(to, 0, 0, bm.width, bm.height, from, 0, 0, SRCCOPY_);
    SelectObject(from, old_from);
    SelectObject(to, old_to);
  }
  if (from) DeleteDC(from);
  if (to) DeleteDC(to);
  ReleaseDC(NULL, screen);
  return copy;
}
/* An icon or cursor object made of a colour bitmap and a mask (both are taken over by the object). */
static HANDLE icon_object(BOOL is_icon, int width, int height, HANDLE color, HANDLE mask, int hot_x, int hot_y, ULONG_PTR rsrc, DWORD flags) {
  HANDLE handle = (HANDLE)NtUserCallOneParam(is_icon ? 1 : 0, 0 /* CreateCursorIcon */);
  if (!handle) return NULL;
  IconFrame_ frame = {(DWORD)width, (DWORD)height, (ULONG_PTR)color, 0, (ULONG_PTR)mask, hot_x, hot_y};
  IconData_ data = {flags, 0, 1, 0, (ULONG_PTR)&frame, 0, 0, rsrc};
  if (!NtUserSetCursorIconData((ULONG_PTR)handle, 0, 0, (ULONG_PTR)&data, 0, 0)) { NtUserDestroyCursor((ULONG_PTR)handle, 1); return NULL; }
  return handle;
}
EXPORT HANDLE WINAPI CreateIconIndirect(const IconInfo_ *info) {
  Bitmap_ bm;
  if (!info || !info->mask) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (GetObjectW(info->color ? info->color : info->mask, sizeof bm, &bm) == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE color = copy_bitmap(info->color), mask = copy_bitmap(info->mask);
  const int height = info->color ? bm.height : bm.height / 2;
  return icon_object(info->is_icon, bm.width, height, color, mask, (int)info->hot_x, (int)info->hot_y, 0, 0);
}
EXPORT BOOL WINAPI GetIconInfo(HANDLE icon, IconInfo_ *info) {
  if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return BOOL_OF(NtUserGetIconInfo((ULONG_PTR)icon, (ULONG_PTR)info, 0, 0, 0, 0));
}
EXPORT HANDLE WINAPI CopyIcon(HANDLE icon) {
  IconInfo_ info;
  if (!GetIconInfo(icon, &info)) return NULL;
  HANDLE copy = CreateIconIndirect(&info);
  DeleteObject(info.mask);
  if (info.color) DeleteObject(info.color);
  return copy;
}
EXPORT HANDLE WINAPI CreateIcon(HANDLE instance, int width, int height, BYTE planes, BYTE bits, const BYTE *and_bits, const BYTE *xor_bits) {
  (void)instance;
  IconInfo_ info = {TRUE, 0, 0, NULL, NULL};
  info.mask = CreateBitmap(width, height, 1, 1, and_bits);
  info.color = bits == 1 && planes == 1 ? NULL : CreateBitmap(width, height, planes, bits, xor_bits);
  if (!info.color) { info.mask = NULL; }
  HANDLE icon = NULL;
  if (info.color && info.mask) icon = CreateIconIndirect(&info);
  if (info.mask) DeleteObject(info.mask);
  if (info.color) DeleteObject(info.color);
  return icon;
}
EXPORT HANDLE WINAPI CreateCursor(HANDLE instance, int hot_x, int hot_y, int width, int height, const BYTE *and_bits, const BYTE *xor_bits) {
  (void)instance;
  IconInfo_ info = {FALSE, (DWORD)hot_x, (DWORD)hot_y, NULL, NULL};
  info.mask = CreateBitmap(width, height, 1, 1, and_bits);
  info.color = CreateBitmap(width, height, 1, 1, xor_bits);
  HANDLE cursor = NULL;
  if (info.mask && info.color) cursor = CreateIconIndirect(&info);
  if (info.mask) DeleteObject(info.mask);
  if (info.color) DeleteObject(info.color);
  return cursor;
}

/* ---- images from resource data and files ------------------------------------------------------------------------------------------------------------------------------- */
static UINT dib_colors(const BmiHeader_ *h) {
  if (h->bit_count > 8) return h->compression == 3 /* BI_BITFIELDS */ ? 3 : 0;
  return h->used ? h->used : 1u << h->bit_count;
}
/* The bitmap of a packed DIB (header, colour table, bits). */
static HANDLE bitmap_from_dib(const BYTE *dib, SIZE_T size) {
  const BmiHeader_ *h = (const BmiHeader_ *)dib;
  if (size < sizeof *h || h->size < sizeof *h) return NULL;
  const SIZE_T offset = (SIZE_T)h->size + (SIZE_T)dib_colors(h) * 4;
  if (offset > size) return NULL;
  HANDLE screen = GetDC(NULL);
  HANDLE bitmap = CreateDIBitmap(screen, h, 4 /* CBM_INIT */, dib + offset, h, 0 /* DIB_RGB_COLORS */);
  ReleaseDC(NULL, screen);
  return bitmap;
}
/* An icon or cursor from the DIB image of an icon resource (or the image of a file entry): the XOR image on top of the AND mask; a cursor resource has the hot spot first. */
EXPORT HANDLE WINAPI CreateIconFromResourceEx(BYTE *bits, DWORD size, BOOL icon, DWORD version, int cx, int cy, UINT flags) {
  (void)version; (void)cx; (void)cy; (void)flags;
  int hot_x = 0, hot_y = 0;
  if (!bits || size < sizeof(BmiHeader_)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (!icon) { hot_x = ((WORD *)bits)[0]; hot_y = ((WORD *)bits)[1]; bits += 4; size -= 4; }
  BmiHeader_ h = *(const BmiHeader_ *)bits;
  if (h.size < sizeof h || h.width <= 0 || h.height == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  const int height = (h.height < 0 ? -h.height : h.height) / 2;
  const SIZE_T color_bytes = (SIZE_T)dib_colors(&h) * 4, offset = (SIZE_T)h.size + color_bytes;
  const SIZE_T xor_stride = (((SIZE_T)h.width * h.bit_count + 31) / 32) * 4, and_stride = (((SIZE_T)h.width + 31) / 32) * 4;
  if (offset + xor_stride * (SIZE_T)height + and_stride * (SIZE_T)height > size) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  BYTE *copy = u32_alloc(offset);
  if (!copy) return NULL;
  for (SIZE_T i = 0; i < offset; ++i) copy[i] = bits[i];
  ((BmiHeader_ *)copy)->height = height;      /* (the image alone, not both halves) */
  ((BmiHeader_ *)copy)->size_image = 0;
  HANDLE screen = GetDC(NULL);
  HANDLE color = CreateDIBitmap(screen, (const BmiHeader_ *)copy, 4, bits + offset, copy, 0);
  ReleaseDC(NULL, screen);
  struct { BmiHeader_ h; DWORD palette[2]; } mono = {{sizeof(BmiHeader_), h.width, height, 1, 1, 0, 0, 0, 0, 2, 0}, {0, 0x00FFFFFF}};
  HANDLE mask = CreateBitmap(h.width, height, 1, 1, NULL);
  HANDLE dc = GetDC(NULL);
  if (mask) SetDIBits(dc, mask, 0, (UINT)height, bits + offset + xor_stride * (SIZE_T)height, &mono, 0);
  ReleaseDC(NULL, dc);
  u32_free(copy);
  if (!color || !mask) { if (color) DeleteObject(color); if (mask) DeleteObject(mask); return NULL; }
  return icon_object(icon, h.width, height, color, mask, hot_x, hot_y, 0, 0);
}
EXPORT HANDLE WINAPI CreateIconFromResource(BYTE *bits, DWORD size, BOOL icon, DWORD version) { return CreateIconFromResourceEx(bits, size, icon, version, 0, 0, 0); }

typedef struct { WORD reserved, type, count; } IconDir_;
typedef struct { BYTE width, height, colors, reserved; WORD planes_or_hot_x, bits_or_hot_y; DWORD size, offset; } IconDirEntry_;
typedef struct { BYTE width, height, colors, reserved; WORD planes, bits; DWORD size; WORD id; } __attribute__((packed)) GroupEntry_;
static int icon_entry_score(int width, int height, int wanted_w, int wanted_h, int bits) {
  const int w = width ? width : 256, h = height ? height : 256;
  const int dw = w > wanted_w ? w - wanted_w : wanted_w - w, dh = h > wanted_h ? h - wanted_h : wanted_h - h;
  return (dw + dh) * 100 - bits;      /* closest size, then the most colours (lower is better) */
}
static void icon_wanted_size(BOOL icon, int cx, int cy, UINT flags, int *w, int *h) {
  if (cx == 0 || (flags & LR_DEFAULTSIZE_)) cx = GetSystemMetrics(icon ? 11 /* SM_CXICON */ : 13 /* SM_CXCURSOR */);
  if (cy == 0 || (flags & LR_DEFAULTSIZE_)) cy = GetSystemMetrics(icon ? 12 : 14);
  *w = cx; *h = cy;
}
/* The icon of a file image (.ico / .cur): the entry closest to the wanted size. */
static HANDLE icon_from_file_data(const BYTE *data, SIZE_T size, BOOL icon, int cx, int cy, UINT flags) {
  const IconDir_ *dir = (const IconDir_ *)data;
  int w, h, best = -1, best_score = 0x7FFFFFFF;
  if (size < sizeof *dir || dir->type != (icon ? 1 : 2) || sizeof *dir + (SIZE_T)dir->count * sizeof(IconDirEntry_) > size) return NULL;
  icon_wanted_size(icon, cx, cy, flags, &w, &h);
  const IconDirEntry_ *entries = (const IconDirEntry_ *)(dir + 1);
  for (int i = 0; i < dir->count; ++i) {
    if ((SIZE_T)entries[i].offset > size || entries[i].size > size - entries[i].offset) continue;
    const int score = icon_entry_score(entries[i].width, entries[i].height, w, h, icon ? entries[i].bits_or_hot_y : 1);
    if (score < best_score) { best = i; best_score = score; }
  }
  if (best < 0) return NULL;
  const IconDirEntry_ *e = &entries[best];
  if (icon) return CreateIconFromResourceEx((BYTE *)data + e->offset, e->size, TRUE, 0x30000, cx, cy, flags);
  /* (a cursor file entry has the hot spot in the directory; the resource form wants it in front of the image) */
  BYTE *image = u32_alloc((SIZE_T)e->size + 4);
  if (!image) return NULL;
  ((WORD *)image)[0] = e->planes_or_hot_x;
  ((WORD *)image)[1] = e->bits_or_hot_y;
  for (DWORD i = 0; i < e->size; ++i) image[4 + i] = data[e->offset + i];
  HANDLE r = CreateIconFromResourceEx(image, e->size + 4, FALSE, 0x30000, cx, cy, flags);
  u32_free(image);
  return r;
}
static BYTE *read_whole_file(const WCHAR *path, SIZE_T *size) {
  HANDLE file = CreateFileW(path, 0x80000000u /* GENERIC_READ */, 1, NULL, 3 /* OPEN_EXISTING */, 0, NULL);
  if (file == INVALID_HANDLE_VALUE) return NULL;
  const DWORD n = GetFileSize(file, NULL);
  BYTE *data = n && n != 0xFFFFFFFFu ? u32_alloc(n) : NULL;
  DWORD got = 0;
  if (data && (!ReadFile(file, data, n, &got, NULL) || got != n)) { u32_free(data); data = NULL; }
  CloseHandle(file);
  if (data) *size = n;
  return data;
}
/* An icon or cursor from a module's resources by the name of its group (the image that fits best). */
static HANDLE icon_from_resource(HANDLE module, const WCHAR *name, BOOL icon, int cx, int cy, UINT flags) {
  HANDLE group = FindResourceW(module, name, (LPCWSTR)(ULONG_PTR)(icon ? RT_GROUP_ICON_ : RT_GROUP_CURSOR_));
  if (!group) return NULL;
  HANDLE loaded = LoadResource(module, group);
  const BYTE *data = loaded ? LockResource(loaded) : NULL;
  const DWORD size = SizeofResource(module, group);
  if (!data || size < sizeof(IconDir_)) return NULL;
  const IconDir_ *dir = (const IconDir_ *)data;
  const GroupEntry_ *entries = (const GroupEntry_ *)(dir + 1);
  int w, h, best = -1, best_score = 0x7FFFFFFF;
  icon_wanted_size(icon, cx, cy, flags, &w, &h);
  if (sizeof *dir + (SIZE_T)dir->count * sizeof(GroupEntry_) > size) return NULL;
  for (int i = 0; i < dir->count; ++i) {
    const int score = icon_entry_score(entries[i].width, entries[i].height, w, h, icon ? entries[i].bits : 1);
    if (score < best_score) { best = i; best_score = score; }
  }
  if (best < 0) return NULL;
  HANDLE image = FindResourceW(module, (LPCWSTR)(ULONG_PTR)entries[best].id, (LPCWSTR)(ULONG_PTR)(icon ? RT_ICON_ : RT_CURSOR_));
  HANDLE image_loaded = image ? LoadResource(module, image) : NULL;
  BYTE *bits = image_loaded ? LockResource(image_loaded) : NULL;
  if (!bits) return NULL;
  return CreateIconFromResourceEx(bits, SizeofResource(module, image), icon, 0x30000, cx, cy, flags);
}
static int is_int_resource(const WCHAR *name, int *id) { if (((ULONG_PTR)name >> 16) == 0) { *id = (int)(ULONG_PTR)name; return 1; } return 0; }

EXPORT HANDLE WINAPI LoadImageW(HANDLE instance, const WCHAR *name, UINT type, int cx, int cy, UINT flags) {
  int id = 0;
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (flags & LR_LOADFROMFILE_) {
    SIZE_T size = 0;
    BYTE *data = read_whole_file(name, &size);
    HANDLE result = NULL;
    if (!data) { SetLastError(ERROR_FILE_NOT_FOUND); return NULL; }
    if (type == 0 /* IMAGE_BITMAP */) { if (size > 14 && data[0] == 'B' && data[1] == 'M') result = bitmap_from_dib(data + 14, size - 14); }
    else result = icon_from_file_data(data, size, type == 1, cx, cy, flags);
    u32_free(data);
    if (!result) SetLastError(ERROR_INVALID_PARAMETER);
    return result;
  }
  if (type == 0) {
    HANDLE module = instance ? instance : GetModuleHandleW(L"user32.dll");
    HANDLE res = FindResourceW(module, name, (LPCWSTR)(ULONG_PTR)RT_BITMAP_);
    HANDLE loaded = res ? LoadResource(module, res) : NULL;
    const BYTE *data = loaded ? LockResource(loaded) : NULL;
    return data ? bitmap_from_dib(data, SizeofResource(module, res)) : NULL;
  }
  if (type != 1 && type != 2) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (!instance && is_int_resource(name, &id)) {      /* a system image */
    UNICODE_STRING us = {0, 0, (PWSTR)(ULONG_PTR)id};
    HANDLE h = (HANDLE)NtUserFindExistingCursorIcon(0, (ULONG_PTR)&us, type == 2 ? 1 : 2);
    if (h) return h;
  }
  return icon_from_resource(instance, name, type == 1, cx, cy, flags);
}
EXPORT HANDLE WINAPI LoadImageA(HANDLE instance, const char *name, UINT type, int cx, int cy, UINT flags) {
  WCHAR *w = wide_of(name);
  HANDLE h = LoadImageW(instance, w, type, cx, cy, flags);
  wide_free(w, name);
  return h;
}
EXPORT HANDLE WINAPI LoadIconW(HANDLE instance, const WCHAR *name) { return LoadImageW(instance, name, 1, 0, 0, LR_SHARED_ | LR_DEFAULTSIZE_); }
EXPORT HANDLE WINAPI LoadIconA(HANDLE instance, const char *name) { return LoadImageA(instance, name, 1, 0, 0, LR_SHARED_ | LR_DEFAULTSIZE_); }
EXPORT HANDLE WINAPI LoadCursorW(HANDLE instance, const WCHAR *name) { return LoadImageW(instance, name, 2, 0, 0, LR_SHARED_ | LR_DEFAULTSIZE_); }
EXPORT HANDLE WINAPI LoadCursorA(HANDLE instance, const char *name) { return LoadImageA(instance, name, 2, 0, 0, LR_SHARED_ | LR_DEFAULTSIZE_); }
EXPORT HANDLE WINAPI LoadCursorFromFileW(const WCHAR *path) { return LoadImageW(NULL, path, 2, 0, 0, LR_LOADFROMFILE_ | LR_DEFAULTSIZE_); }
EXPORT HANDLE WINAPI LoadCursorFromFileA(const char *path) { return LoadImageA(NULL, path, 2, 0, 0, LR_LOADFROMFILE_ | LR_DEFAULTSIZE_); }
EXPORT HANDLE WINAPI LoadBitmapW(HANDLE instance, const WCHAR *name) { return LoadImageW(instance, name, 0, 0, 0, 0); }
EXPORT HANDLE WINAPI LoadBitmapA(HANDLE instance, const char *name) { return LoadImageA(instance, name, 0, 0, 0, 0); }
EXPORT HANDLE WINAPI CopyImage(HANDLE image, UINT type, int cx, int cy, UINT flags) {
  (void)cx; (void)cy; (void)flags;
  if (type == 0) return copy_bitmap(image);
  return CopyIcon(image);
}

/* ---- drawing -------------------------------------------------------------------------------------------------------------------------------------------------------- */
#define DI_MASK_ 1
#define DI_IMAGE_ 2
#define DI_NORMAL_ 3
EXPORT BOOL WINAPI DrawIconEx(HANDLE hdc, int x, int y, HANDLE icon, int cx, int cy, UINT step, HANDLE background, UINT flags) {
  IconInfo_ info;
  Bitmap_ bm;
  (void)step;
  if (!hdc || !GetIconInfo(icon, &info)) return FALSE;
  if (GetObjectW(info.color ? info.color : info.mask, sizeof bm, &bm) == 0) return FALSE;
  const int sw = bm.width, sh = info.color ? bm.height : bm.height / 2;
  if (cx == 0 || (flags & 0x4 /* DI_DEFAULTSIZE */)) cx = sw;
  if (cy == 0 || (flags & 0x4)) cy = sh;
  HANDLE mem = CreateCompatibleDC(hdc);
  if (background) { RECT_ r = {x, y, x + cx, y + cy}; FillRect(hdc, &r, background); }
  if (mem) {
    HANDLE old = SelectObject(mem, info.mask);
    if (flags & DI_MASK_) StretchBlt(hdc, x, y, cx, cy, mem, 0, 0, sw, sh, info.color ? SRCAND_ : SRCCOPY_);
    if ((flags & DI_IMAGE_) && info.color) { SelectObject(mem, info.color); StretchBlt(hdc, x, y, cx, cy, mem, 0, 0, sw, sh, SRCINVERT_); }
    else if ((flags & DI_IMAGE_) && !info.color) StretchBlt(hdc, x, y, cx, cy, mem, 0, sh, sw, sh, SRCINVERT_);
    SelectObject(mem, old);
    DeleteDC(mem);
  }
  DeleteObject(info.mask);
  if (info.color) DeleteObject(info.color);
  return TRUE;
}
EXPORT BOOL WINAPI DrawIcon(HANDLE hdc, int x, int y, HANDLE icon) { return DrawIconEx(hdc, x, y, icon, 0, 0, 0, NULL, DI_NORMAL_ | 0x4); }
