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

/* The clipboard of user32.dll (included by window.inc before the controls). The data is win32k's (src/Process/Win32k/Win32kUserMiscClipboard.cpp), which the system keeps equal to the
 * clipboard of the desktop's own programs (src/Kernel/Clipboard.hpp): text put here is on the desktop's clipboard when the program closes the clipboard, and what a desktop program
 * copies is here at the next call. This file turns the handles of Windows (HGLOBAL, HBITMAP) into the bytes win32k keeps and back: formats that hold memory are copied (GlobalSize
 * bytes); a bitmap is kept as the CF_DIB bytes of its image next to the handle; the handle of a format the program set, or that was made for it by a read, is remembered per format
 * until the clipboard changes, so GetClipboardData answers the same handle while the clipboard is the same. Not done: metafiles and palettes (only the handle is kept: they work
 * inside one process), CF_DIBV5 conversion. */
SVC NtUserOpenClipboard(ULONG_PTR hwnd, ULONG_PTR unknown);
SVC NtUserCloseClipboard(void);
SVC NtUserEmptyClipboard(void);
SVC NtUserSetClipboardData(ULONG_PTR format, ULONG_PTR handle, ULONG_PTR params);
SVC NtUserGetClipboardData(ULONG_PTR format, ULONG_PTR params);
SVC NtUserEnumClipboardFormats(ULONG_PTR format);
SVC NtUserIsClipboardFormatAvailable(ULONG_PTR format);
SVC NtUserCountClipboardFormats(void);
SVC NtUserGetClipboardOwner(void);
SVC NtUserGetOpenClipboardWindow(void);
SVC NtUserGetClipboardViewer(void);
SVC NtUserSetClipboardViewer(ULONG_PTR hwnd);
SVC NtUserChangeClipboardChain(ULONG_PTR hwnd, ULONG_PTR next);
SVC NtUserAddClipboardFormatListener(ULONG_PTR hwnd);
SVC NtUserRemoveClipboardFormatListener(ULONG_PTR hwnd);
SVC NtUserGetClipboardSequenceNumber(void);
SVC NtUserGetUpdatedClipboardFormats(ULONG_PTR formats, ULONG_PTR count, ULONG_PTR out_count);
SVC NtUserGetPriorityClipboardFormat(ULONG_PTR list, ULONG_PTR count);
IMPORT int WINAPI GetDIBits(HANDLE hdc, HANDLE bitmap, UINT start, UINT lines, void *bits, void *info, UINT usage);
#define CF_BITMAP_ 2
#define CF_METAFILEPICT_ 3
#define CF_PALETTE_ 9
#define CF_DIB_ 8
#define CF_ENHMETAFILE_ 14
#define CF_UNICODETEXT_ 13
#define GMEM_MOVEABLE_ 2
#define ERROR_CLIPBOARD_NOT_OPEN_ 1418

typedef struct { UINT format; HANDLE handle; UINT sequence; BOOL owned; } ClipEntry;
#define CLIP_CACHE 32
static ClipEntry g_clip_cache[CLIP_CACHE];
static BOOL clip_is_gdi_format(UINT f) { return f == CF_BITMAP_ || f == CF_PALETTE_ || f == CF_ENHMETAFILE_ || (f >= 0x0082 && f <= 0x008E && f != 0x0083); }
static void clip_release(ClipEntry *e) {
  if (e->handle) {
    if (e->format == CF_BITMAP_ || e->format == CF_PALETTE_) { if (!e->owned) DeleteObject(e->handle); }
    else if (!clip_is_gdi_format(e->format) && !e->owned) GlobalFree(e->handle);
  }
  e->handle = NULL; e->format = 0;
}
static void clip_forget(BOOL all, UINT sequence) {      /* entries made for an older clipboard are dropped (all: whatever the sequence) */
  for (int i = 0; i < CLIP_CACHE; ++i) if (g_clip_cache[i].handle && (all || g_clip_cache[i].sequence != sequence)) clip_release(&g_clip_cache[i]);
}
static ClipEntry *clip_find(UINT format, UINT sequence) {
  for (int i = 0; i < CLIP_CACHE; ++i) if (g_clip_cache[i].handle && g_clip_cache[i].format == format && g_clip_cache[i].sequence == sequence) return &g_clip_cache[i];
  return NULL;
}
static void clip_remember(UINT format, HANDLE handle, UINT sequence, BOOL owned) {
  ClipEntry *slot = NULL;
  for (int i = 0; i < CLIP_CACHE; ++i) { if (g_clip_cache[i].handle && g_clip_cache[i].format == format) { clip_release(&g_clip_cache[i]); slot = &g_clip_cache[i]; break; } if (!g_clip_cache[i].handle && !slot) slot = &g_clip_cache[i]; }
  if (!slot) slot = &g_clip_cache[0];
  slot->format = format; slot->handle = handle; slot->sequence = sequence; slot->owned = owned;
}

EXPORT BOOL WINAPI OpenClipboard(HANDLE hwnd) { return BOOL_OF(NtUserOpenClipboard((ULONG_PTR)hwnd, 0)); }
EXPORT BOOL WINAPI CloseClipboard(void) { return BOOL_OF(NtUserCloseClipboard()); }
EXPORT BOOL WINAPI EmptyClipboard(void) {
  const BOOL ok = BOOL_OF(NtUserEmptyClipboard());
  if (ok) clip_forget(TRUE, 0);
  return ok;
}
EXPORT int WINAPI CountClipboardFormats(void) { return (int)NtUserCountClipboardFormats(); }
EXPORT UINT WINAPI EnumClipboardFormats(UINT format) { return (UINT)NtUserEnumClipboardFormats(format); }
EXPORT BOOL WINAPI IsClipboardFormatAvailable(UINT format) { return BOOL_OF(NtUserIsClipboardFormatAvailable(format)); }
EXPORT HANDLE WINAPI GetClipboardOwner(void) { return (HANDLE)NtUserGetClipboardOwner(); }
EXPORT HANDLE WINAPI GetOpenClipboardWindow(void) { return (HANDLE)NtUserGetOpenClipboardWindow(); }
EXPORT HANDLE WINAPI GetClipboardViewer(void) { return (HANDLE)NtUserGetClipboardViewer(); }
EXPORT HANDLE WINAPI SetClipboardViewer(HANDLE hwnd) { return (HANDLE)NtUserSetClipboardViewer((ULONG_PTR)hwnd); }
EXPORT BOOL WINAPI ChangeClipboardChain(HANDLE hwnd, HANDLE next) { return BOOL_OF(NtUserChangeClipboardChain((ULONG_PTR)hwnd, (ULONG_PTR)next)); }
EXPORT BOOL WINAPI AddClipboardFormatListener(HANDLE hwnd) { return BOOL_OF(NtUserAddClipboardFormatListener((ULONG_PTR)hwnd)); }
EXPORT BOOL WINAPI RemoveClipboardFormatListener(HANDLE hwnd) { return BOOL_OF(NtUserRemoveClipboardFormatListener((ULONG_PTR)hwnd)); }
EXPORT DWORD WINAPI GetClipboardSequenceNumber(void) { return (DWORD)NtUserGetClipboardSequenceNumber(); }
EXPORT BOOL WINAPI GetUpdatedClipboardFormats(UINT *formats, UINT count, UINT *out_count) { return BOOL_OF(NtUserGetUpdatedClipboardFormats((ULONG_PTR)formats, count, (ULONG_PTR)out_count)); }
EXPORT int WINAPI GetPriorityClipboardFormat(UINT *list, int count) { return (int)(LONG)NtUserGetPriorityClipboardFormat((ULONG_PTR)list, (ULONG_PTR)(LONG_PTR)count); }

/* The CF_DIB bytes of a bitmap (a 32-bit bottom-up image); the buffer is a heap block (u32_free). */
static BYTE *clip_dib_of_bitmap(HANDLE bitmap, SIZE_T *size) {
  Bitmap_ bm;
  if (GetObjectW(bitmap, sizeof bm, &bm) == 0 || bm.width <= 0 || bm.height <= 0) return NULL;
  const SIZE_T bytes = (SIZE_T)bm.width * bm.height * 4, total = 40 + bytes;
  BYTE *dib = u32_alloc(total);
  if (!dib) return NULL;
  BmiHeader_ *h = (BmiHeader_ *)dib;
  *h = (BmiHeader_){40, bm.width, bm.height, 1, 32, 0, (DWORD)bytes, 0, 0, 0, 0};
  HANDLE dc = GetDC(NULL);
  const int lines = GetDIBits(dc, bitmap, 0, (UINT)bm.height, dib + 40, dib, 0 /* DIB_RGB_COLORS */);
  ReleaseDC(NULL, dc);
  if (!lines) { u32_free(dib); return NULL; }
  *size = total;
  return dib;
}
EXPORT HANDLE WINAPI SetClipboardData(UINT format, HANDLE handle) {
  struct { ULONG_PTR data, size; BYTE cache_only, pad[3]; DWORD sequence; } params = {0, 0, 0, {0}, 0};
  BYTE *owned_dib = NULL;
  void *locked = NULL;
  if (handle) {
    if (format == CF_BITMAP_) {
      SIZE_T size = 0;
      owned_dib = clip_dib_of_bitmap(handle, &size);
      params.data = (ULONG_PTR)owned_dib; params.size = owned_dib ? size : 0;
    } else if (!clip_is_gdi_format(format) && format != CF_METAFILEPICT_) {
      locked = GlobalLock(handle);
      if (!locked) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
      params.data = (ULONG_PTR)locked; params.size = GlobalSize(handle);
    }
  }
  const LONG status = (LONG)NtUserSetClipboardData(format, (ULONG_PTR)handle, (ULONG_PTR)&params);
  if (locked) GlobalUnlock(handle);
  if (status < 0) { u32_free(owned_dib); return NULL; }
  if (format == CF_BITMAP_ && owned_dib) {      /* the picture is also a CF_DIB, for the readers of that format (and the desktop's programs) */
    struct { ULONG_PTR data, size; BYTE cache_only, pad[3]; DWORD sequence; } dib_params = {params.data, params.size, 0, {0}, 0};
    NtUserSetClipboardData(CF_DIB_, 0, (ULONG_PTR)&dib_params);
    params.sequence = dib_params.sequence;
  }
  u32_free(owned_dib);
  if (handle) clip_remember(format, handle, params.sequence, TRUE);
  return handle ? handle : (HANDLE)1;
}
EXPORT HANDLE WINAPI GetClipboardData(UINT format) {
  struct { ULONG_PTR data, capacity, size, sequence; } params = {0, 0, 0, 0};
  HANDLE got = (HANDLE)NtUserGetClipboardData(format, (ULONG_PTR)&params);
  const UINT sequence = (UINT)params.sequence;
  clip_forget(FALSE, sequence);
  ClipEntry *cached = clip_find(format, sequence);
  if (cached) return cached->handle;
  UINT source = format;
  if (!got && format == CF_BITMAP_) {      /* only a CF_DIB is there (a picture of another program or the desktop) */
    source = CF_DIB_;
    params.size = 0;
    got = (HANDLE)NtUserGetClipboardData(source, (ULONG_PTR)&params);
  }
  if (!got) return NULL;
  const SIZE_T size = (SIZE_T)params.size;
  if (format == CF_ENHMETAFILE_ || format == CF_PALETTE_ || format == CF_METAFILEPICT_) return NULL;      /* (only the owner's own handle is known: the cache above) */
  BYTE *bytes = u32_alloc(size + 4);
  if (!bytes) return NULL;
  for (SIZE_T i = 0; i < size + 4; ++i) bytes[i] = 0;
  struct { ULONG_PTR data, capacity, size, sequence; } read = {(ULONG_PTR)bytes, size, 0, 0};
  NtUserGetClipboardData(source, (ULONG_PTR)&read);
  HANDLE result = NULL;
  if (format == CF_BITMAP_) { result = bitmap_from_dib(bytes, size); u32_free(bytes); }
  else {
    result = GlobalAlloc(GMEM_MOVEABLE_, size + 4);
    void *dst = result ? GlobalLock(result) : NULL;
    if (dst) { for (SIZE_T i = 0; i < size + 4; ++i) ((BYTE *)dst)[i] = bytes[i]; GlobalUnlock(result); }
    else if (result) { GlobalFree(result); result = NULL; }
    u32_free(bytes);
  }
  if (result) clip_remember(format, result, sequence, FALSE);
  return result;
}
