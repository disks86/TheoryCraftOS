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
/* gdi32.dll for TheoryCraftOS: the GDI API of wingdi.h as a thin layer over the NtGdi* services of win32k (the design of Wine's dlls/gdi32): argument shaping, the A
 * variants of the string functions, the helpers that need no service. The kernel half is src/Process/Gdi*.cpp; the services are called through local system-call
 * stubs (svc_stubs.inc), numbered as src/Windows/tcos_syscalls.h has them. Every export that is not defined here is still a stub (src/Windows/stubs/gdi32.inc).
 * Metafiles (WMF / EMF recording and playback) are not here yet: every DC-taking wrapper can later start with a hook on is_meta_dc(). */
#define WIN32_LEAN_AND_MEAN
#define _GDI32_
#define NOGDICAPMASKS_UNUSED
#include <windows.h>
#include <wingdi.h>
#include <winternl.h>
#include <winspool.h>
#include <string.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#include "../tcos_syscalls.h"

#define GDI_SERVICE(name)                                                                                                                                     \
  __asm__(".text\n.globl " #name "\n" #name ":\n mov %rcx, %r10\n mov $" GDI_STR(NT_SERVICE_##name) ", %eax\n syscall\n cmp $0xC0000002, %eax\n jne 1f\n xor %eax, %eax\n1: ret\n");
/* The Direct3D thunks (D3DKMT*) pass the status of the kernel on as it is: a service the kernel does not implement is STATUS_NOT_IMPLEMENTED, not success. */
#define DXG_SERVICE(name)                                                                                                                                     \
  __asm__(".text\n.globl " #name "\n" #name ":\n mov %rcx, %r10\n mov $" GDI_STR(NT_SERVICE_##name) ", %eax\n syscall\n ret\n");
#define GDI_STR(x) GDI_STR2(x)
#define GDI_STR2(x) #x

typedef ULONG FLONG_;
#ifndef FLONG
#define FLONG FLONG_
#endif
struct font_enum_entry { DWORD type; ENUMLOGFONTEXW lf; NEWTEXTMETRICEXW tm; };
struct font_realization_info { DWORD size, flags, cache_num, instance_id, file_count; WORD face_index, simulations; };
struct char_width_info { INT lsb, rsb, unk; };
struct font_fileinfo { FILETIME writetime; LARGE_INTEGER size; WCHAR path[1]; };

#include "svc_proto.inc"
#include "svc_stubs.inc"

/* Handles (src/Process/Win32k/Win32kGdi.cpp): generation << 24 | stock << 23 | type << 16 | index. The metafile objects are "client objects" of the kernel (type 0x20); gdi32
 * knows which kind each is, and gdi_handle_type() returns the NTGDI_OBJ_METADC ... values for them (they are not kernel values). */
#define NTGDI_OBJ_DC 0x010000u
#define NTGDI_OBJ_REGION 0x040000u
#define NTGDI_OBJ_BITMAP 0x050000u
#define NTGDI_OBJ_PAL 0x080000u
#define NTGDI_OBJ_COLORSPACE 0x090000u
#define NTGDI_OBJ_FONT 0x0a0000u
#define NTGDI_OBJ_BRUSH 0x100000u
#define NTGDI_OBJ_CLIENT 0x200000u
#define NTGDI_OBJ_PEN 0x300000u
#define NTGDI_OBJ_EXTPEN 0x500000u
#define NTGDI_OBJ_ENHMETADC 0x210000u      /* (user-mode kinds of client objects) */
#define NTGDI_OBJ_METAFILE 0x260000u
#define NTGDI_OBJ_ENHMETAFILE 0x460000u
#define NTGDI_OBJ_METADC 0x660000u
#define NTGDI_OBJ_MEMDC 0x410000u      /* (not a kernel type: GetObjectType asks IsMemDC) */
#define NTGDI_HANDLE_TYPE_MASK 0x007f0000u
#define NTGDI_HANDLE_STOCK_OBJECT 0x00800000u
#define NTGDI_GETCHARWIDTH_INT 0x02
#define NTGDI_GETCHARWIDTH_INDICES 0x08
#define NTGDI_GETCHARABCWIDTHS_INT 0x01
#define NTGDI_GETCHARABCWIDTHS_INDICES 0x02
#define MWT_SET_ 4

/* NtGdiGetAndSetDCDword / NtGdiGetDCDword / NtGdiGetDCPoint methods (ntgdi.h, and the additions of the kernel's GDI module: docs/win32k/Gdi.md). */
enum { NtGdiSetMapMode = 8, NtGdiSetBkColor = 100, NtGdiSetBkMode, NtGdiSetTextColor, NtGdiSetDCBrushColor, NtGdiSetDCPenColor, NtGdiSetGraphicsMode, NtGdiSetROP2, NtGdiSetTextAlign,
       TcosSetPolyFillMode = 108, TcosSetStretchBltMode, TcosSetArcDirection, TcosSetRelAbs, TcosSetTextCharacterExtra,
       TcosSetWindowOrgX = 120, TcosSetWindowOrgY, TcosSetWindowExtX, TcosSetWindowExtY, TcosSetViewportOrgX, TcosSetViewportOrgY, TcosSetViewportExtX, TcosSetViewportExtY };
enum { NtGdiGetArcDirection, NtGdiGetBkColor, NtGdiGetBkMode, NtGdiGetDCBrushColor, NtGdiGetDCPenColor, NtGdiGetGraphicsMode, NtGdiGetLayout, NtGdiGetPolyFillMode, NtGdiGetROP2,
       NtGdiGetTextColor, NtGdiIsMemDC, NtGdiHasOpenGL,
       TcosGetStretchBltMode = 12, TcosGetRelAbs, TcosGetTextCharacterExtra, TcosGetTextAlign, TcosGetMapMode };
enum { NtGdiGetBrushOrgEx, NtGdiGetCurrentPosition, NtGdiGetDCOrg, TcosPointViewportOrg, TcosPointWindowOrg, TcosPointViewportExt, TcosPointWindowExt };
enum { NtGdiAnimatePalette, NtGdiSetPaletteEntries, NtGdiGetPaletteEntries, NtGdiGetSystemPaletteEntries, NtGdiGetDIBColorTable, NtGdiSetDIBColorTable };
enum { NtGdiArc, NtGdiArcTo, NtGdiChord, NtGdiPie };
enum { NtGdiPolyPolygon = 1, NtGdiPolyPolyline, NtGdiPolyBezier, NtGdiPolyBezierTo, NtGdiPolylineTo, NtGdiPolyPolygonRgn };
enum { NtGdiLPtoDP, NtGdiDPtoLP };

/* ---- helpers ------------------------------------------------------------------------------------------------------------------------------------------------ */

static UINT meta_kind(const void *h);      /* meta.inc: the NTGDI_OBJ_METADC ... kind of a client object that is a metafile (DC), 0 if it is none */
static inline UINT gdi_handle_type(const void *h) {
  const UINT t = (UINT)((UINT_PTR)h & NTGDI_HANDLE_TYPE_MASK);
  return t == NTGDI_OBJ_CLIENT ? (meta_kind(h) ? meta_kind(h) : t) : t;
}

/* Metafile hook: when the metafile phase exists, a DC-taking wrapper starts with `if (is_meta_dc(hdc)) return META_Name(...)`. */
static inline int is_meta_dc(HDC hdc) { const UINT t = gdi_handle_type(hdc); return t == NTGDI_OBJ_METADC || t == NTGDI_OBJ_ENHMETADC; }

#ifndef EMR_SETLAYOUT
#define EMR_SETLAYOUT 115
#endif
#ifndef EMR_SETTEXTJUSTIFICATION
#define EMR_SETTEXTJUSTIFICATION 120
#endif
#ifndef EMR_ALPHABLEND
#define EMR_ALPHABLEND 114
#endif
#ifndef EMR_TRANSPARENTBLT
#define EMR_TRANSPARENTBLT 116
#endif
/* Metafile recording (meta.inc): a metafile DC's state lives in a hidden kernel DC, RD() maps the handle to it for every call that is not recorded. */
static HDC meta_real(HDC hdc);
#define RD(h) meta_real(h)
static BOOL meta_simple(HDC hdc, DWORD emr, int wmf, unsigned wflags, int n, ...);
#define MREC(hdc, ...) do { if (is_meta_dc(hdc)) meta_simple((hdc), __VA_ARGS__); } while (0)
#define MDRAW(hdc, ret, ...) do { if (is_meta_dc(hdc)) { meta_simple((hdc), __VA_ARGS__); return (ret); } } while (0)
static HGDIOBJ meta_select(HDC hdc, HGDIOBJ obj);
static HPALETTE meta_select_palette(HDC hdc, HPALETTE pal);
static BOOL meta_poly(HDC hdc, int kind, const POINT *pts, int n);
static BOOL meta_polypoly(HDC hdc, int polygon, const POINT *pts, const DWORD *counts, DWORD n);
static BOOL meta_polydraw(HDC hdc, const POINT *pts, const BYTE *types, int count);
static BOOL meta_shape(HDC hdc, DWORD emr, int wmf, int n, const int *v);
static BOOL meta_arc(HDC hdc, int kind, int l, int t, int r, int b, int xs, int ys, int xe, int ye);
static BOOL meta_exttextout(HDC hdc, int x, int y, UINT flags, const RECT *rect, const WCHAR *str, UINT count, const INT *dx);
static BOOL meta_regionop(HDC hdc, DWORD emr, HRGN rgn, HBRUSH brush, int w, int h, int mode, int has_mode);
static BOOL meta_blit(HDC hdc, int kind, int xd, int yd, int wd, int hd, HDC src, int xs, int ys, int ws, int hs, DWORD extra);
static BOOL meta_dibs(HDC hdc, int kind, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, UINT start, UINT lines, const void *bits, const BITMAPINFO *bmi, UINT usage, DWORD rop);
static BOOL meta_lineto(HDC hdc, int x, int y);
static BOOL meta_path(HDC hdc, DWORD emr, int open);
static BOOL meta_xform(HDC hdc, const XFORM *x, DWORD mode, int has_mode);
static int meta_save(HDC hdc);
static BOOL meta_restore(HDC hdc, int level);
static BOOL meta_comment(HDC hdc, UINT size, const BYTE *data);
static void meta_offset_org(HDC hdc, int window, int x, int y);
static BOOL meta_delete_dc(HDC hdc);
static int meta_is_enh(HDC hdc);
static int meta_float_bits(float f);
static void meta_object_deleted(HGDIOBJ obj);
static int meta_handle_valid(HGDIOBJ h);

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static void *mem_alloc_n(SIZE_T count, SIZE_T size) { if (size && count > (SIZE_T)0x7fffffff / size) return NULL; return mem_alloc(count * size); }

static int lock_state;      /* a tiny spin lock for the DIB section table */
static void table_lock(void) { while (__sync_lock_test_and_set(&lock_state, 1)) { } }
static void table_unlock(void) { __sync_lock_release(&lock_state); }

static WCHAR *dup_AtoW(const char *s, UINT cp) {
  if (!s) return NULL;
  const int n = MultiByteToWideChar(cp, 0, s, -1, NULL, 0);
  WCHAR *w = mem_alloc_n((SIZE_T)n + 1, sizeof(WCHAR));
  if (w && n) MultiByteToWideChar(cp, 0, s, -1, w, n);
  return w;
}

static int is_stock_name(const WCHAR *a, const char *b) {
  for (; *a && *b; ++a, ++b) { WCHAR x = *a, y = (WCHAR)(BYTE)*b; if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; if (x != y) return 0; }
  return !*a && !*b;
}

/* ---- DC attributes and state ---------------------------------------------------------------------------------------------------------------------------------- */

static DWORD get_dc_dword(HDC hdc, UINT method, DWORD failure) { DWORD r = failure; if (!NtGdiGetDCDword(RD(hdc), method, &r)) return failure; return r; }
static DWORD set_dc_dword(HDC hdc, UINT method, DWORD value, DWORD failure) { DWORD r = failure; if (!NtGdiGetAndSetDCDword(RD(hdc), method, value, &r)) return failure; return r; }

EXPORT COLORREF WINAPI GetBkColor(HDC hdc) { return get_dc_dword(RD(hdc), NtGdiGetBkColor, CLR_INVALID); }
EXPORT int WINAPI GetBkMode(HDC hdc) { return (int)get_dc_dword(RD(hdc), NtGdiGetBkMode, 0); }
EXPORT COLORREF WINAPI GetTextColor(HDC hdc) { return get_dc_dword(RD(hdc), NtGdiGetTextColor, CLR_INVALID); }
EXPORT COLORREF WINAPI GetDCBrushColor(HDC hdc) { return get_dc_dword(RD(hdc), NtGdiGetDCBrushColor, CLR_INVALID); }
EXPORT COLORREF WINAPI GetDCPenColor(HDC hdc) { return get_dc_dword(RD(hdc), NtGdiGetDCPenColor, CLR_INVALID); }
EXPORT int WINAPI GetGraphicsMode(HDC hdc) { return (int)get_dc_dword(RD(hdc), NtGdiGetGraphicsMode, 0); }
EXPORT int WINAPI GetROP2(HDC hdc) { return (int)get_dc_dword(RD(hdc), NtGdiGetROP2, 0); }
EXPORT int WINAPI GetArcDirection(HDC hdc) { return (int)get_dc_dword(RD(hdc), NtGdiGetArcDirection, 0); }
EXPORT int WINAPI GetPolyFillMode(HDC hdc) { return (int)get_dc_dword(RD(hdc), NtGdiGetPolyFillMode, 0); }
EXPORT DWORD WINAPI GetLayout(HDC hdc) { return get_dc_dword(RD(hdc), NtGdiGetLayout, GDI_ERROR); }
EXPORT int WINAPI GetStretchBltMode(HDC hdc) { return (int)get_dc_dword(RD(hdc), TcosGetStretchBltMode, 0); }
EXPORT int WINAPI GetMapMode(HDC hdc) { return (int)get_dc_dword(RD(hdc), TcosGetMapMode, 0); }
EXPORT UINT WINAPI GetTextAlign(HDC hdc) { return get_dc_dword(RD(hdc), TcosGetTextAlign, GDI_ERROR); }
EXPORT int WINAPI GetTextCharacterExtra(HDC hdc) { return (int)get_dc_dword(RD(hdc), TcosGetTextCharacterExtra, 0x80000000u); }
EXPORT int WINAPI GetRelAbs(HDC hdc, DWORD reserved) { return (int)get_dc_dword(RD(hdc), TcosGetRelAbs, 0); }

EXPORT COLORREF WINAPI SetBkColor(HDC hdc, COLORREF color) { MREC(hdc, EMR_SETBKCOLOR, 0x0201, 1, 1, (int)color); return set_dc_dword(RD(hdc), NtGdiSetBkColor, color, CLR_INVALID); }
EXPORT int WINAPI SetBkMode(HDC hdc, int mode) { MREC(hdc, EMR_SETBKMODE, 0x0102, 0, 1, mode); return (int)set_dc_dword(RD(hdc), NtGdiSetBkMode, (DWORD)mode, 0); }
EXPORT COLORREF WINAPI SetTextColor(HDC hdc, COLORREF color) { MREC(hdc, EMR_SETTEXTCOLOR, 0x0209, 1, 1, (int)color); return set_dc_dword(RD(hdc), NtGdiSetTextColor, color, CLR_INVALID); }
EXPORT COLORREF WINAPI SetDCBrushColor(HDC hdc, COLORREF color) { return set_dc_dword(RD(hdc), NtGdiSetDCBrushColor, color, CLR_INVALID); }
EXPORT COLORREF WINAPI SetDCPenColor(HDC hdc, COLORREF color) { return set_dc_dword(RD(hdc), NtGdiSetDCPenColor, color, CLR_INVALID); }
EXPORT int WINAPI SetGraphicsMode(HDC hdc, int mode) { return (int)set_dc_dword(RD(hdc), NtGdiSetGraphicsMode, (DWORD)mode, 0); }
EXPORT int WINAPI SetROP2(HDC hdc, int mode) { MREC(hdc, EMR_SETROP2, 0x0104, 0, 1, mode); return (int)set_dc_dword(RD(hdc), NtGdiSetROP2, (DWORD)mode, 0); }
EXPORT UINT WINAPI SetTextAlign(HDC hdc, UINT align) { MREC(hdc, EMR_SETTEXTALIGN, 0x012E, 0, 1, (int)align); return set_dc_dword(RD(hdc), NtGdiSetTextAlign, align, GDI_ERROR); }
EXPORT int WINAPI SetMapMode(HDC hdc, int mode) { MREC(hdc, EMR_SETMAPMODE, 0x0103, 0, 1, mode); return (int)set_dc_dword(RD(hdc), NtGdiSetMapMode, (DWORD)mode, 0); }
EXPORT int WINAPI SetArcDirection(HDC hdc, int dir) { MREC(hdc, EMR_SETARCDIRECTION, -1, 0, 1, dir);
  if (dir != AD_COUNTERCLOCKWISE && dir != AD_CLOCKWISE) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  return (int)set_dc_dword(RD(hdc), TcosSetArcDirection, (DWORD)dir, 0);
}
EXPORT int WINAPI SetPolyFillMode(HDC hdc, int mode) { MREC(hdc, EMR_SETPOLYFILLMODE, 0x0106, 0, 1, mode);
  if (mode != ALTERNATE && mode != WINDING) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  return (int)set_dc_dword(RD(hdc), TcosSetPolyFillMode, (DWORD)mode, 0);
}
EXPORT int WINAPI SetStretchBltMode(HDC hdc, int mode) { MREC(hdc, EMR_SETSTRETCHBLTMODE, 0x0107, 0, 1, mode);
  if (mode <= 0 || mode > 4) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  return (int)set_dc_dword(RD(hdc), TcosSetStretchBltMode, (DWORD)mode, 0);
}
EXPORT int WINAPI SetRelAbs(HDC hdc, int mode) { MREC(hdc, 0, 0x0105, 0, 1, mode); return (int)set_dc_dword(RD(hdc), TcosSetRelAbs, (DWORD)mode, 0); }
static DWORD mapper_flags_value;      /* (the kernel keeps no mapper flags) */
EXPORT DWORD WINAPI SetMapperFlags(HDC hdc, DWORD flags) { MREC(hdc, EMR_SETMAPPERFLAGS, 0x0231, 1, 1, (int)flags); if (!NtGdiGetDCDword(RD(hdc), NtGdiGetBkMode, &(DWORD){0})) return GDI_ERROR; DWORD old = mapper_flags_value; mapper_flags_value = flags; return old; }
EXPORT int WINAPI SetTextCharacterExtra(HDC hdc, int extra) { MREC(hdc, 0, 0x0108, 0, 1, extra);
  if (extra == (int)0x80000000) { SetLastError(ERROR_INVALID_PARAMETER); return (int)0x80000000; }
  return (int)set_dc_dword(RD(hdc), TcosSetTextCharacterExtra, (DWORD)extra, 0x80000000u);
}
EXPORT DWORD WINAPI SetLayout(HDC hdc, DWORD layout) { MREC(hdc, EMR_SETLAYOUT, -1, 0, 1, (int)layout); return NtGdiSetLayout(RD(hdc), -1, layout); }
EXPORT BOOL WINAPI SetTextJustification(HDC hdc, int extra, int breaks) { MREC(hdc, EMR_SETTEXTJUSTIFICATION, 0x020A, 0, 2, extra, breaks); return NtGdiSetTextJustification(RD(hdc), extra, breaks); }

EXPORT BOOL WINAPI GetCurrentPositionEx(HDC hdc, POINT *pt) { if (is_meta_dc(hdc) && !meta_is_enh(hdc)) return FALSE; return pt ? NtGdiGetDCPoint(RD(hdc), NtGdiGetCurrentPosition, pt) : FALSE; }
EXPORT BOOL WINAPI GetBrushOrgEx(HDC hdc, POINT *pt) { return pt ? NtGdiGetDCPoint(RD(hdc), NtGdiGetBrushOrgEx, pt) : FALSE; }
EXPORT BOOL WINAPI GetDCOrgEx(HDC hdc, POINT *pt) { return pt ? NtGdiGetDCPoint(RD(hdc), NtGdiGetDCOrg, pt) : FALSE; }
EXPORT BOOL WINAPI SetBrushOrgEx(HDC hdc, int x, int y, POINT *old) { MREC(hdc, EMR_SETBRUSHORGEX, -1, 0, 2, x, y); return NtGdiSetBrushOrg(RD(hdc), x, y, old); }
EXPORT BOOL WINAPI GetWindowOrgEx(HDC hdc, POINT *pt) { return pt ? NtGdiGetDCPoint(RD(hdc), TcosPointWindowOrg, pt) : FALSE; }
EXPORT BOOL WINAPI GetViewportOrgEx(HDC hdc, POINT *pt) { return pt ? NtGdiGetDCPoint(RD(hdc), TcosPointViewportOrg, pt) : FALSE; }
static BOOL get_extent(HDC hdc, UINT method, SIZE *size) {
  POINT pt;
  if (!size || !NtGdiGetDCPoint(RD(hdc), method, &pt)) return FALSE;
  size->cx = pt.x; size->cy = pt.y;
  return TRUE;
}
EXPORT BOOL WINAPI GetWindowExtEx(HDC hdc, SIZE *size) { return get_extent(RD(hdc), TcosPointWindowExt, size); }
EXPORT BOOL WINAPI GetViewportExtEx(HDC hdc, SIZE *size) { return get_extent(RD(hdc), TcosPointViewportExt, size); }
/* The kernel sets one coordinate per call (GetAndSetDCDword 120..127); the old pair is read first. `get_method` is the TcosPoint* that reads the pair, `first` the Tcos Set*X. */
static BOOL set_pair(HDC hdc, UINT get_method, UINT first, int x, int y, POINT *old) {
  POINT prev;
  DWORD ignored;
  if (!NtGdiGetDCPoint(hdc, get_method, &prev)) return FALSE;
  if (!NtGdiGetAndSetDCDword(hdc, first, (DWORD)x, &ignored) || !NtGdiGetAndSetDCDword(hdc, first + 1, (DWORD)y, &ignored)) return FALSE;
  if (old) *old = prev;
  return TRUE;
}
static BOOL set_point(HDC hdc, UINT method, int x, int y, POINT *old) { return set_pair(hdc, method == TcosSetWindowOrgX ? TcosPointWindowOrg : TcosPointViewportOrg, method, x, y, old); }
static BOOL set_extent(HDC hdc, UINT method, int x, int y, SIZE *old) {
  POINT prev;
  const UINT get_method = method == TcosSetWindowExtX ? TcosPointWindowExt : TcosPointViewportExt;
  const int mode = (int)get_dc_dword(hdc, TcosGetMapMode, 0);
  if (!NtGdiGetDCPoint(hdc, get_method, &prev)) return FALSE;
  if (old) { old->cx = prev.x; old->cy = prev.y; }
  if (mode != MM_ISOTROPIC && mode != MM_ANISOTROPIC) return TRUE;      /* the fixed mapping modes keep their extents */
  if (!x || !y) return FALSE;
  return set_pair(hdc, get_method, method, x, y, NULL);
}
static BOOL offset_point(HDC hdc, UINT get_method, UINT first, int dx, int dy, POINT *old) {
  POINT cur;
  if (!NtGdiGetDCPoint(hdc, get_method, &cur)) return FALSE;
  return set_pair(hdc, get_method, first, cur.x + dx, cur.y + dy, old);
}
EXPORT BOOL WINAPI SetWindowOrgEx(HDC hdc, int x, int y, POINT *old) { MREC(hdc, EMR_SETWINDOWORGEX, 0x020B, 0, 2, x, y); return set_point(RD(hdc), TcosSetWindowOrgX, x, y, old); }
EXPORT BOOL WINAPI SetViewportOrgEx(HDC hdc, int x, int y, POINT *old) { MREC(hdc, EMR_SETVIEWPORTORGEX, 0x020D, 0, 2, x, y); return set_point(RD(hdc), TcosSetViewportOrgX, x, y, old); }
EXPORT BOOL WINAPI OffsetWindowOrgEx(HDC hdc, int x, int y, POINT *old) { BOOL r = offset_point(RD(hdc), TcosPointWindowOrg, TcosSetWindowOrgX, x, y, old); if (r && is_meta_dc(hdc)) meta_offset_org(hdc, 1, x, y); return r; }
EXPORT BOOL WINAPI OffsetViewportOrgEx(HDC hdc, int x, int y, POINT *old) { BOOL r = offset_point(RD(hdc), TcosPointViewportOrg, TcosSetViewportOrgX, x, y, old); if (r && is_meta_dc(hdc)) meta_offset_org(hdc, 0, x, y); return r; }
EXPORT BOOL WINAPI SetWindowExtEx(HDC hdc, int x, int y, SIZE *old) { MREC(hdc, EMR_SETWINDOWEXTEX, 0x020C, 0, 2, x, y); return set_extent(RD(hdc), TcosSetWindowExtX, x, y, old); }
EXPORT BOOL WINAPI SetViewportExtEx(HDC hdc, int x, int y, SIZE *old) { MREC(hdc, EMR_SETVIEWPORTEXTEX, 0x020E, 0, 2, x, y); return set_extent(RD(hdc), TcosSetViewportExtX, x, y, old); }
EXPORT BOOL WINAPI ScaleViewportExtEx(HDC hdc, int xn, int xd, int yn, int yd, SIZE *size) { MREC(hdc, EMR_SCALEVIEWPORTEXTEX, 0x0412, 0, 4, xn, xd, yn, yd); return NtGdiScaleViewportExtEx(RD(hdc), xn, xd, yn, yd, size); }
EXPORT BOOL WINAPI ScaleWindowExtEx(HDC hdc, int xn, int xd, int yn, int yd, SIZE *size) { MREC(hdc, EMR_SCALEWINDOWEXTEX, 0x0410, 0, 4, xn, xd, yn, yd); return NtGdiScaleWindowExtEx(RD(hdc), xn, xd, yn, yd, size); }
EXPORT BOOL WINAPI SetVirtualResolution(HDC hdc, DWORD hr, DWORD vr, DWORD hs, DWORD vs) { return NtGdiSetVirtualResolution(RD(hdc), hr, vr, hs, vs); }
EXPORT BOOL WINAPI LPtoDP(HDC hdc, POINT *points, int count) { return NtGdiTransformPoints(RD(hdc), points, points, count, NtGdiLPtoDP); }
EXPORT BOOL WINAPI DPtoLP(HDC hdc, POINT *points, int count) { return NtGdiTransformPoints(RD(hdc), points, points, count, NtGdiDPtoLP); }

EXPORT BOOL WINAPI GetWorldTransform(HDC hdc, XFORM *xform) { return xform ? NtGdiGetTransform(RD(hdc), 0x0203, xform) : FALSE; }
EXPORT BOOL WINAPI SetWorldTransform(HDC hdc, const XFORM *xform) { if (is_meta_dc(hdc) && xform) meta_xform(hdc, xform, 0, 0); return xform ? NtGdiModifyWorldTransform(RD(hdc), xform, MWT_SET_) : FALSE; }
EXPORT BOOL WINAPI ModifyWorldTransform(HDC hdc, const XFORM *xform, DWORD mode) { if (is_meta_dc(hdc)) { static const XFORM idt = {1,0,0,1,0,0}; meta_xform(hdc, xform ? xform : &idt, mode, 1); }
  if (mode != MWT_IDENTITY && !xform) return FALSE;
  return NtGdiModifyWorldTransform(RD(hdc), xform, mode);
}
EXPORT BOOL WINAPI CombineTransform(XFORM *out, const XFORM *a, const XFORM *b) {
  XFORM t;
  if (!out || !a || !b) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  t.eM11 = a->eM11 * b->eM11 + a->eM12 * b->eM21;
  t.eM12 = a->eM11 * b->eM12 + a->eM12 * b->eM22;
  t.eM21 = a->eM21 * b->eM11 + a->eM22 * b->eM21;
  t.eM22 = a->eM21 * b->eM12 + a->eM22 * b->eM22;
  t.eDx = a->eDx * b->eM11 + a->eDy * b->eM21 + b->eDx;
  t.eDy = a->eDx * b->eM12 + a->eDy * b->eM22 + b->eDy;
  *out = t;
  return TRUE;
}

EXPORT BOOL WINAPI SetMiterLimit(HDC hdc, FLOAT limit, FLOAT *old) { MREC(hdc, EMR_SETMITERLIMIT, -1, 0, 1, meta_float_bits(limit)); DWORD bits; memcpy(&bits, &limit, 4); return NtGdiSetMiterLimit(RD(hdc), bits, old); }
EXPORT BOOL WINAPI GetMiterLimit(HDC hdc, FLOAT *limit) { return limit ? NtGdiGetMiterLimit(RD(hdc), limit) : FALSE; }
EXPORT UINT WINAPI GetBoundsRect(HDC hdc, RECT *rect, UINT flags) { return NtGdiGetBoundsRect(RD(hdc), rect, flags); }
EXPORT UINT WINAPI SetBoundsRect(HDC hdc, const RECT *rect, UINT flags) { return NtGdiSetBoundsRect(RD(hdc), rect, flags); }
EXPORT BOOL WINAPI GetColorAdjustment(HDC hdc, COLORADJUSTMENT *ca) { return ca ? NtGdiGetColorAdjustment(RD(hdc), ca) : FALSE; }
EXPORT BOOL WINAPI SetColorAdjustment(HDC hdc, const COLORADJUSTMENT *ca) { return ca ? NtGdiSetColorAdjustment(RD(hdc), ca) : FALSE; }
EXPORT BOOL WINAPI GetDeviceGammaRamp(HDC hdc, void *ramp) { return NtGdiGetDeviceGammaRamp(RD(hdc), ramp); }
EXPORT BOOL WINAPI SetDeviceGammaRamp(HDC hdc, void *ramp) { return NtGdiSetDeviceGammaRamp(RD(hdc), ramp); }
EXPORT int WINAPI GetDeviceCaps(HDC hdc, int cap) { if (is_meta_dc(hdc) && !meta_is_enh(hdc)) return cap == TECHNOLOGY ? DT_METAFILE : 0; return NtGdiGetDeviceCaps(RD(hdc), cap); }
EXPORT BOOL WINAPI GdiFlush(void) { NtGdiFlush(); return TRUE; }
EXPORT DWORD WINAPI GdiGetBatchLimit(void) { return 1; }
EXPORT DWORD WINAPI GdiSetBatchLimit(DWORD limit) { return 1; }
EXPORT BOOL WINAPI GdiComment(HDC hdc, UINT size, const BYTE *data) { if (is_meta_dc(hdc)) return meta_comment(hdc, size, data); return FALSE; }      /* only an enhanced-metafile DC records comments */
/* ICM: the display has one profile (the sRGB one) and colour management is never on. The mode set by SetICMMode is remembered here per DC (the kernel has no slot). */
static const WCHAR icm_profile[] = L"C:\\Windows\\System32\\spool\\drivers\\color\\sRGB Color Space Profile.icm";
#define ICM_SLOTS 64
static struct { HDC dc; int mode; } icm_modes[ICM_SLOTS];
static int dc_is_valid(HDC hdc) { DWORD r; return hdc && NtGdiGetDCDword(RD(hdc), NtGdiGetBkMode, &r); }
EXPORT int WINAPI SetICMMode(HDC hdc, int mode) {
  int slot = -1, previous = ICM_OFF;
  if (!dc_is_valid(hdc)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  if (mode < ICM_OFF || mode > ICM_DONE_OUTSIDEDC) return 0;
  table_lock();
  for (int i = 0; i < ICM_SLOTS; ++i) { if (icm_modes[i].dc == hdc) { slot = i; break; } if (slot < 0 && !icm_modes[i].dc) slot = i; }
  if (slot >= 0) {
    if (icm_modes[slot].dc == hdc) previous = icm_modes[slot].mode;
    if (mode != ICM_QUERY) {
      if (mode == ICM_OFF) icm_modes[slot].dc = NULL; else { icm_modes[slot].dc = hdc; icm_modes[slot].mode = mode; }
    }
  }
  table_unlock();
  return previous;
}
EXPORT BOOL WINAPI GetICMProfileW(HDC hdc, LPDWORD size, LPWSTR name) {
  const DWORD needed = sizeof(icm_profile) / sizeof(WCHAR);
  if (!hdc || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!dc_is_valid(hdc)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (!name || *size < needed) { *size = needed; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memcpy(name, icm_profile, sizeof(icm_profile));
  *size = needed;
  return TRUE;
}
EXPORT BOOL WINAPI GetICMProfileA(HDC hdc, LPDWORD size, LPSTR name) {
  const DWORD needed = sizeof(icm_profile) / sizeof(WCHAR);
  if (!hdc || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!dc_is_valid(hdc)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (!name || *size < needed) { *size = needed; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  WideCharToMultiByte(CP_ACP, 0, icm_profile, -1, name, needed, NULL, NULL);
  *size = needed;
  return TRUE;
}
EXPORT BOOL WINAPI SetICMProfileW(HDC hdc, LPWSTR name) {
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!dc_is_valid(hdc)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI SetICMProfileA(HDC hdc, LPSTR name) {
  if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!dc_is_valid(hdc)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  return TRUE;
}
EXPORT int WINAPI EnumICMProfilesA(HDC hdc, ICMENUMPROCA proc, LPARAM param) {
  char name[MAX_PATH];
  DWORD size = MAX_PATH;
  if (!proc || !GetICMProfileA(hdc, &size, name)) return -1;
  return proc(name, param);
}
EXPORT int WINAPI EnumICMProfilesW(HDC hdc, ICMENUMPROCW proc, LPARAM param) {
  WCHAR name[MAX_PATH];
  DWORD size = MAX_PATH;
  if (!proc || !GetICMProfileW(hdc, &size, name)) return -1;
  return proc(name, param);
}

/* ---- device contexts ------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT int WINAPI SaveDC(HDC hdc) { if (is_meta_dc(hdc)) return meta_save(hdc); return NtGdiSaveDC(hdc); }
EXPORT BOOL WINAPI RestoreDC(HDC hdc, int level) { if (is_meta_dc(hdc)) return meta_restore(hdc, level); return NtGdiRestoreDC(RD(hdc), level); }
EXPORT HDC WINAPI CreateCompatibleDC(HDC hdc) { return NtGdiCreateCompatibleDC(hdc); }
EXPORT BOOL WINAPI CancelDC(HDC hdc) { return NtGdiCancelDC(hdc); }

/* The display is the only device: a DC for "DISPLAY" (or a \\.\DISPLAYn device) is made by the kernel; a printer or other driver fails. */
static int is_display_device(const WCHAR *driver, const WCHAR *device) {
  if (driver && is_stock_name(driver, "DISPLAY")) return 1;
  if (!driver && device) {
    static const char prefix[] = "\\\\.\\DISPLAY";
    int i = 0;
    for (; prefix[i]; ++i) { WCHAR c = device[i]; if (c >= 'a' && c <= 'z') c -= 32; if (c != (WCHAR)(BYTE)prefix[i]) return 0; }
    return 1;
  }
  return 0;
}
static HDC open_dc(const WCHAR *driver, const WCHAR *device, const WCHAR *output, const DEVMODEW *devmode, ULONG type) {
  UNICODE_STRING dev, out;
  if (!is_display_device(driver, device)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  dev.Buffer = (PWSTR)(device ? device : L""); dev.Length = (USHORT)(device ? lstrlenW(device) * 2 : 0); dev.MaximumLength = (USHORT)(dev.Length + 2);
  out.Buffer = (PWSTR)(output ? output : L""); out.Length = (USHORT)(output ? lstrlenW(output) * 2 : 0); out.MaximumLength = (USHORT)(out.Length + 2);
  return NtGdiOpenDCW(&dev, devmode, &out, type, TRUE, NULL, NULL, NULL);
}
EXPORT HDC WINAPI CreateDCW(LPCWSTR driver, LPCWSTR device, LPCWSTR output, const DEVMODEW *devmode) { return open_dc(driver, device, output, devmode, 0); }
EXPORT HDC WINAPI CreateICW(LPCWSTR driver, LPCWSTR device, LPCWSTR output, const DEVMODEW *devmode) { return open_dc(driver, device, output, devmode, 1); }
EXPORT HDC WINAPI CreateDCExW(LPCWSTR driver, LPCWSTR device, LPCWSTR output, const DEVMODEW *devmode, DWORD flags) { return open_dc(driver, device, output, devmode, 0); }

/* DEVMODEA -> DEVMODEW: the fields before dmFormName keep their layout (dmDeviceName widens), the ones after it too. */
static DEVMODEW *devmode_AtoW(const DEVMODEA *a) {
  if (!a) return NULL;
  const WORD extra = a->dmDriverExtra;
  const SIZE_T head = FIELD_OFFSET(DEVMODEA, dmFormName), tail = FIELD_OFFSET(DEVMODEA, dmLogPixels);
  DEVMODEW *w = mem_alloc(sizeof(DEVMODEW) + extra);
  if (!w) return NULL;
  MultiByteToWideChar(CP_ACP, 0, (const char *)a->dmDeviceName, CCHDEVICENAME, w->dmDeviceName, CCHDEVICENAME);
  w->dmDeviceName[CCHDEVICENAME - 1] = 0;
  memcpy(&w->dmSpecVersion, &a->dmSpecVersion, head - FIELD_OFFSET(DEVMODEA, dmSpecVersion));
  MultiByteToWideChar(CP_ACP, 0, (const char *)a->dmFormName, CCHFORMNAME, w->dmFormName, CCHFORMNAME);
  w->dmFormName[CCHFORMNAME - 1] = 0;
  if (a->dmSize > tail) memcpy(&w->dmLogPixels, &a->dmLogPixels, a->dmSize > sizeof(DEVMODEA) ? sizeof(DEVMODEA) - tail : a->dmSize - tail);
  w->dmSize = sizeof(DEVMODEW);
  w->dmDriverExtra = extra;
  if (extra) memcpy((char *)w + sizeof(DEVMODEW), (const char *)a + a->dmSize, extra);
  return w;
}
static HDC open_dc_A(LPCSTR driver, LPCSTR device, LPCSTR output, const DEVMODEA *devmode, ULONG type) {
  WCHAR *d = dup_AtoW(driver, CP_ACP), *v = dup_AtoW(device, CP_ACP), *o = dup_AtoW(output, CP_ACP);
  DEVMODEW *dm = devmode_AtoW(devmode);
  const HDC hdc = open_dc(d, v, o, dm, type);
  mem_free(d); mem_free(v); mem_free(o); mem_free(dm);
  return hdc;
}
EXPORT HDC WINAPI CreateDCA(LPCSTR driver, LPCSTR device, LPCSTR output, const DEVMODEA *devmode) { return open_dc_A(driver, device, output, devmode, 0); }
EXPORT HDC WINAPI CreateICA(LPCSTR driver, LPCSTR device, LPCSTR output, const DEVMODEA *devmode) { return open_dc_A(driver, device, output, devmode, 1); }
EXPORT HDC WINAPI ResetDCW(HDC hdc, const DEVMODEW *devmode) {
  BOOL banding = FALSE;
  return NtGdiResetDC(RD(hdc), devmode, &banding, NULL, NULL) ? hdc : NULL;
}
EXPORT HDC WINAPI ResetDCA(HDC hdc, const DEVMODEA *devmode) {
  DEVMODEW *dm = devmode_AtoW(devmode);
  const HDC r = ResetDCW(hdc, dm);
  mem_free(dm);
  return r;
}

#include "object.inc"
#include "draw.inc"
#include "dib.inc"
#include "text.inc"
#include "misc.inc"
#include "meta.inc"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) { return TRUE; }
