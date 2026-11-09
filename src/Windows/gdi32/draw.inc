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
/* gdi32.dll: lines, shapes, polygons, paths, pixels, blits. Included by gdi32.c. */

EXPORT BOOL WINAPI MoveToEx(HDC hdc, int x, int y, POINT *old) { MREC(hdc, EMR_MOVETOEX, 0x0214, 0, 2, x, y); return NtGdiMoveTo(RD(hdc), x, y, old); }
EXPORT BOOL WINAPI LineTo(HDC hdc, int x, int y) { if (is_meta_dc(hdc)) return meta_lineto(hdc, x, y); return NtGdiLineTo(RD(hdc), x, y); }
EXPORT BOOL WINAPI Rectangle(HDC hdc, int l, int t, int r, int b) { if (is_meta_dc(hdc)) { const int v[4] = { l, t, r, b }; return meta_shape(hdc, EMR_RECTANGLE, 0x041B, 4, v); } return NtGdiRectangle(RD(hdc), l, t, r, b); }
EXPORT BOOL WINAPI RoundRect(HDC hdc, int l, int t, int r, int b, int w, int h) { if (is_meta_dc(hdc)) { const int v[6] = { l, t, r, b, w, h }; return meta_shape(hdc, EMR_ROUNDRECT, 0x061C, 6, v); } return NtGdiRoundRect(RD(hdc), l, t, r, b, w, h); }
EXPORT BOOL WINAPI Ellipse(HDC hdc, int l, int t, int r, int b) { if (is_meta_dc(hdc)) { const int v[4] = { l, t, r, b }; return meta_shape(hdc, EMR_ELLIPSE, 0x0418, 4, v); } return NtGdiEllipse(RD(hdc), l, t, r, b); }
EXPORT BOOL WINAPI Arc(HDC hdc, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { if (is_meta_dc(hdc)) return meta_arc(hdc, 0, l, t, r, b, xs, ys, xe, ye); return NtGdiArcInternal(NtGdiArc, hdc, l, t, r, b, xs, ys, xe, ye); }
EXPORT BOOL WINAPI ArcTo(HDC hdc, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { if (is_meta_dc(hdc)) return meta_arc(hdc, 1, l, t, r, b, xs, ys, xe, ye); return NtGdiArcInternal(NtGdiArcTo, hdc, l, t, r, b, xs, ys, xe, ye); }
EXPORT BOOL WINAPI Chord(HDC hdc, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { if (is_meta_dc(hdc)) return meta_arc(hdc, 2, l, t, r, b, xs, ys, xe, ye); return NtGdiArcInternal(NtGdiChord, hdc, l, t, r, b, xs, ys, xe, ye); }
EXPORT BOOL WINAPI Pie(HDC hdc, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { if (is_meta_dc(hdc)) return meta_arc(hdc, 3, l, t, r, b, xs, ys, xe, ye); return NtGdiArcInternal(NtGdiPie, hdc, l, t, r, b, xs, ys, xe, ye); }
EXPORT BOOL WINAPI AngleArc(HDC hdc, int x, int y, DWORD radius, FLOAT start, FLOAT sweep) { if (is_meta_dc(hdc)) { meta_simple(hdc, EMR_ANGLEARC, -1, 0, 5, x, y, (int)radius, meta_float_bits(start), meta_float_bits(sweep)); return TRUE; }
  DWORD s, w;
  memcpy(&s, &start, 4); memcpy(&w, &sweep, 4);
  return NtGdiAngleArc(RD(hdc), x, y, radius, s, w);
}

EXPORT BOOL WINAPI Polygon(HDC hdc, const POINT *points, int count) { if (is_meta_dc(hdc)) return meta_poly(hdc, 1, points, count); ULONG c = (ULONG)count; return (BOOL)NtGdiPolyPolyDraw(RD(hdc), points, &c, 1, NtGdiPolyPolygon); }
EXPORT BOOL WINAPI Polyline(HDC hdc, const POINT *points, int count) { if (is_meta_dc(hdc)) return meta_poly(hdc, 2, points, count); ULONG c = (ULONG)count; return (BOOL)NtGdiPolyPolyDraw(RD(hdc), points, &c, 1, NtGdiPolyPolyline); }
EXPORT BOOL WINAPI PolylineTo(HDC hdc, const POINT *points, DWORD count) { if (is_meta_dc(hdc)) return meta_poly(hdc, 4, points, count); ULONG c = count; return (BOOL)NtGdiPolyPolyDraw(RD(hdc), points, &c, 1, NtGdiPolylineTo); }
EXPORT BOOL WINAPI PolyBezier(HDC hdc, const POINT *points, DWORD count) { if (is_meta_dc(hdc)) return meta_poly(hdc, 0, points, count); ULONG c = count; return (BOOL)NtGdiPolyPolyDraw(RD(hdc), points, &c, 1, NtGdiPolyBezier); }
EXPORT BOOL WINAPI PolyBezierTo(HDC hdc, const POINT *points, DWORD count) { if (is_meta_dc(hdc)) return meta_poly(hdc, 3, points, count); ULONG c = count; return (BOOL)NtGdiPolyPolyDraw(RD(hdc), points, &c, 1, NtGdiPolyBezierTo); }
EXPORT BOOL WINAPI PolyPolygon(HDC hdc, const POINT *points, const INT *counts, int polygons) { if (is_meta_dc(hdc)) return meta_polypoly(hdc, 1, points, (const DWORD *)counts, polygons); return (BOOL)NtGdiPolyPolyDraw(RD(hdc), points, (const ULONG *)counts, polygons, NtGdiPolyPolygon); }
EXPORT BOOL WINAPI PolyPolyline(HDC hdc, const POINT *points, const DWORD *counts, DWORD lines) { if (is_meta_dc(hdc)) return meta_polypoly(hdc, 0, points, counts, lines); return (BOOL)NtGdiPolyPolyDraw(RD(hdc), points, counts, lines, NtGdiPolyPolyline); }
EXPORT BOOL WINAPI PolyDraw(HDC hdc, const POINT *points, const BYTE *types, int count) { if (is_meta_dc(hdc)) return meta_polydraw(hdc, points, types, count); return NtGdiPolyDraw(RD(hdc), points, types, count); }

EXPORT COLORREF WINAPI SetPixel(HDC hdc, int x, int y, COLORREF color) { MDRAW(hdc, color, EMR_SETPIXELV, 0x041F, 4, 3, x, y, (int)color); return NtGdiSetPixel(RD(hdc), x, y, color); }
EXPORT BOOL WINAPI SetPixelV(HDC hdc, int x, int y, COLORREF color) { MDRAW(hdc, TRUE, EMR_SETPIXELV, 0x041F, 4, 3, x, y, (int)color); return NtGdiSetPixel(RD(hdc), x, y, color) != CLR_INVALID; }
EXPORT COLORREF WINAPI GetPixel(HDC hdc, int x, int y) { return NtGdiGetPixel(RD(hdc), x, y); }
EXPORT BOOL WINAPI ExtFloodFill(HDC hdc, int x, int y, COLORREF color, UINT type) { MDRAW(hdc, TRUE, EMR_EXTFLOODFILL, 0x0548, 4, 4, x, y, (int)color, (int)type); return NtGdiExtFloodFill(RD(hdc), x, y, color, type); }
EXPORT BOOL WINAPI FloodFill(HDC hdc, int x, int y, COLORREF color) { return ExtFloodFill(hdc, x, y, color, FLOODFILLBORDER); }

EXPORT BOOL WINAPI BeginPath(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_BEGINPATH, 1); return NtGdiBeginPath(hdc); }
EXPORT BOOL WINAPI EndPath(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_ENDPATH, 2); return NtGdiEndPath(hdc); }
EXPORT BOOL WINAPI AbortPath(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_ABORTPATH, 2); return NtGdiAbortPath(hdc); }
EXPORT BOOL WINAPI CloseFigure(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_CLOSEFIGURE, 0); return NtGdiCloseFigure(hdc); }
EXPORT BOOL WINAPI FillPath(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_FILLPATH, 0); return NtGdiFillPath(hdc); }
EXPORT BOOL WINAPI StrokePath(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_STROKEPATH, 0); return NtGdiStrokePath(hdc); }
EXPORT BOOL WINAPI StrokeAndFillPath(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_STROKEANDFILLPATH, 0); return NtGdiStrokeAndFillPath(hdc); }
EXPORT BOOL WINAPI FlattenPath(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_FLATTENPATH, 0); return NtGdiFlattenPath(hdc); }
EXPORT BOOL WINAPI WidenPath(HDC hdc) { if (is_meta_dc(hdc)) return meta_path(hdc, EMR_WIDENPATH, 0); return NtGdiWidenPath(hdc); }
EXPORT BOOL WINAPI SelectClipPath(HDC hdc, int mode) { if (is_meta_dc(hdc)) { meta_simple(hdc, EMR_SELECTCLIPPATH, -1, 0, 1, mode); return TRUE; } return NtGdiSelectClipPath(RD(hdc), mode); }
EXPORT HRGN WINAPI PathToRegion(HDC hdc) { return NtGdiPathToRegion(hdc); }
EXPORT int WINAPI GetPath(HDC hdc, POINT *points, BYTE *types, int size) { return NtGdiGetPath(RD(hdc), points, types, size); }

EXPORT BOOL WINAPI PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop) { if (is_meta_dc(hdc)) return meta_blit(hdc, 0, x, y, w, h, NULL, 0, 0, 0, 0, rop); return NtGdiPatBlt(RD(hdc), x, y, w, h, rop); }
EXPORT BOOL WINAPI BitBlt(HDC dst, int xd, int yd, int w, int h, HDC src, int xs, int ys, DWORD rop) { if (is_meta_dc(dst)) return meta_blit(dst, 0, xd, yd, w, h, src, xs, ys, w, h, rop); return NtGdiBitBlt(dst, xd, yd, w, h, src, xs, ys, rop, 0, 0); }
EXPORT BOOL WINAPI StretchBlt(HDC dst, int xd, int yd, int wd, int hd, HDC src, int xs, int ys, int ws, int hs, DWORD rop) { if (is_meta_dc(dst)) return meta_blit(dst, 1, xd, yd, wd, hd, src, xs, ys, ws, hs, rop); return NtGdiStretchBlt(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, rop, 0); }
EXPORT BOOL WINAPI MaskBlt(HDC dst, int xd, int yd, int w, int h, HDC src, int xs, int ys, HBITMAP mask, int xm, int ym, DWORD rop) { if (is_meta_dc(dst)) return TRUE; return NtGdiMaskBlt(dst, xd, yd, w, h, src, xs, ys, mask, xm, ym, rop, 0); }
EXPORT BOOL WINAPI PlgBlt(HDC dst, const POINT *pts, HDC src, int xs, int ys, int w, int h, HBITMAP mask, int xm, int ym) { if (is_meta_dc(dst)) return TRUE; return NtGdiPlgBlt(dst, pts, src, xs, ys, w, h, mask, xm, ym, 0); }
EXPORT BOOL WINAPI TransparentBlt(HDC dst, int xd, int yd, int wd, int hd, HDC src, int xs, int ys, int ws, int hs, UINT color) { if (is_meta_dc(dst)) return meta_blit(dst, 3, xd, yd, wd, hd, src, xs, ys, ws, hs, color); return NtGdiTransparentBlt(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, color); }
EXPORT BOOL WINAPI GdiTransparentBlt(HDC dst, int xd, int yd, int wd, int hd, HDC src, int xs, int ys, int ws, int hs, UINT color) { return TransparentBlt(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, color); }
EXPORT BOOL WINAPI GdiAlphaBlend(HDC dst, int xd, int yd, int wd, int hd, HDC src, int xs, int ys, int ws, int hs, BLENDFUNCTION blend) { if (is_meta_dc(dst)) { DWORD bf; memcpy(&bf, &blend, 4); return meta_blit(dst, 2, xd, yd, wd, hd, src, xs, ys, ws, hs, bf); }
  DWORD bits;
  memcpy(&bits, &blend, 4);
  return NtGdiAlphaBlend(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, bits, NULL);
}
EXPORT BOOL WINAPI AlphaBlend(HDC dst, int xd, int yd, int wd, int hd, HDC src, int xs, int ys, int ws, int hs, BLENDFUNCTION blend) { return GdiAlphaBlend(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, blend); }
EXPORT BOOL WINAPI GradientFill(HDC hdc, PTRIVERTEX vertices, ULONG nvert, PVOID mesh, ULONG nmesh, ULONG mode) { if (is_meta_dc(hdc)) return TRUE; return NtGdiGradientFill(RD(hdc), vertices, nvert, mesh, nmesh, mode); }
EXPORT BOOL WINAPI GdiGradientFill(HDC hdc, PTRIVERTEX vertices, ULONG nvert, PVOID mesh, ULONG nmesh, ULONG mode) { if (is_meta_dc(hdc)) return TRUE; return NtGdiGradientFill(RD(hdc), vertices, nvert, mesh, nmesh, mode); }

EXPORT BOOL WINAPI LineDDA(int x0, int y0, int x1, int y1, LINEDDAPROC proc, LPARAM param) {
  int xadd = 1, yadd = 1, err, erradd, cnt, dx = x1 - x0, dy = y1 - y0;
  if (dx < 0) { xadd = -1; dx = -dx; }
  if (dy < 0) { yadd = -1; dy = -dy; }
  if (dx > dy) {
    err = 2 * dy - dx; erradd = 2 * dy - 2 * dx;
    for (cnt = 0; cnt <= dx; cnt++) { proc(x0, y0, param); if (err > 0) { y0 += yadd; err += erradd; } else err += 2 * dy; x0 += xadd; }
  } else {
    err = 2 * dx - dy; erradd = 2 * dx - 2 * dy;
    for (cnt = 0; cnt <= dy; cnt++) { proc(x0, y0, param); if (err > 0) { x0 += xadd; err += erradd; } else err += 2 * dx; y0 += yadd; }
  }
  return TRUE;
}

/* A window drawn by OpenGL is swapped by opengl32.dll (as Windows' gdi32 hands it to the OpenGL driver): that library is asked when the program has it loaded. */
EXPORT int WINAPI SwapBuffers(HDC hdc) {
  HMODULE gl = GetModuleHandleA("opengl32.dll");
  if (gl) {
    BOOL(WINAPI * swap)(HDC) = (BOOL(WINAPI *)(HDC))GetProcAddress(gl, "wglSwapBuffers");
    if (swap) return swap(hdc);
  }
  return NtGdiSwapBuffers(hdc);
}
EXPORT int WINAPI DescribePixelFormat(HDC hdc, int format, UINT size, PIXELFORMATDESCRIPTOR *descr) { return NtGdiDescribePixelFormat(RD(hdc), format, size, descr); }
/* The pixel format belongs to the window (every device context of it has it) or, for a memory device context, to the DC. The kernel keeps a DC's but has no call to read it
 * back: gdi32 remembers what was set, by window. (user32 is not imported here: it imports gdi32; the window of a DC comes from win32k directly.) */
ULONG_PTR NtUserWindowFromDC(ULONG_PTR hdc);
DXG_SERVICE(NtUserWindowFromDC)
static struct { ULONG_PTR key; int format; } pixel_formats[64];
static ULONG_PTR format_key(HDC hdc) {
  const ULONG_PTR window = NtUserWindowFromDC((ULONG_PTR)hdc);
  return window ? window : (ULONG_PTR)hdc;
}
EXPORT int WINAPI GetPixelFormat(HDC hdc) {
  const ULONG_PTR key = format_key(hdc);
  int r = 0;
  table_lock();
  for (int i = 0; i < 64; ++i) if (pixel_formats[i].key == key) r = pixel_formats[i].format;
  table_unlock();
  return r;
}
EXPORT BOOL WINAPI SetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *descr) {
  const int current = GetPixelFormat(hdc);
  if (current) {      /* (a window's pixel format is set once: the same one again succeeds, another fails) */
    if (current != format) { SetLastError(ERROR_INVALID_PIXEL_FORMAT); return FALSE; }
    return TRUE;
  }
  if (!NtGdiSetPixelFormat(RD(hdc), format)) return FALSE;
  const ULONG_PTR key = format_key(hdc);
  table_lock();
  int i, free_slot = -1;
  for (i = 0; i < 64; ++i) { if (pixel_formats[i].key == key) break; if (!pixel_formats[i].key && free_slot < 0) free_slot = i; }
  if (i == 64) i = free_slot;
  if (i >= 0) { pixel_formats[i].key = key; pixel_formats[i].format = format; }
  table_unlock();
  return TRUE;
}

/* ChoosePixelFormat as Wine has it: the format with the fewest mismatches against the request. */
EXPORT int WINAPI ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *wanted) {
  PIXELFORMATDESCRIPTOR p;
  int best = 0, best_score = 0x7fffffff;
  const int count = NtGdiDescribePixelFormat(RD(hdc), 1, sizeof(p), &p);
  if (!wanted) return 0;
  for (int i = 1; i <= count; ++i) {
    int score = 0;
    if (!NtGdiDescribePixelFormat(RD(hdc), i, sizeof(p), &p)) continue;
    if ((wanted->dwFlags & PFD_DOUBLEBUFFER_DONTCARE) == 0 && (wanted->dwFlags & PFD_DOUBLEBUFFER) != (p.dwFlags & PFD_DOUBLEBUFFER)) continue;
    if ((wanted->dwFlags & PFD_STEREO_DONTCARE) == 0 && (wanted->dwFlags & PFD_STEREO) != (p.dwFlags & PFD_STEREO)) continue;
    if ((wanted->dwFlags & PFD_DRAW_TO_WINDOW) && !(p.dwFlags & PFD_DRAW_TO_WINDOW)) continue;
    if ((wanted->dwFlags & PFD_DRAW_TO_BITMAP) && !(p.dwFlags & PFD_DRAW_TO_BITMAP)) continue;
    if ((wanted->dwFlags & PFD_SUPPORT_OPENGL) && !(p.dwFlags & PFD_SUPPORT_OPENGL)) continue;
    /* (a pixel type that is not colour index asks for RGBA, as on Windows) */
    if ((wanted->iPixelType == PFD_TYPE_COLORINDEX) != (p.iPixelType == PFD_TYPE_COLORINDEX)) continue;
    score += wanted->cColorBits > p.cColorBits ? (wanted->cColorBits - p.cColorBits) * 4 : p.cColorBits - wanted->cColorBits;
    /* PFD_DEPTH_DONTCARE: a format without a depth buffer is best */
    if (wanted->dwFlags & PFD_DEPTH_DONTCARE) score += p.cDepthBits ? 64 : 0;
    else score += wanted->cDepthBits > p.cDepthBits ? (wanted->cDepthBits - p.cDepthBits) * 8 : p.cDepthBits - wanted->cDepthBits;
    score += wanted->cStencilBits > p.cStencilBits ? (wanted->cStencilBits - p.cStencilBits) * 8 : p.cStencilBits - wanted->cStencilBits;
    score += wanted->cAlphaBits > p.cAlphaBits ? (wanted->cAlphaBits - p.cAlphaBits) * 2 : p.cAlphaBits - wanted->cAlphaBits;
    if (score < best_score) { best_score = score; best = i; }
  }
  return best;
}
