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
/* gdi32.dll: objects (stock objects, GetObject, DeleteObject, SelectObject), pens, brushes, palettes, regions, clipping. Included by gdi32.c. */

static int object_is_valid(HGDIOBJ h);

/* ---- stock objects, GetObject, DeleteObject ------------------------------------------------------------------------------------------------------------------- */

/* The stock objects have fixed handles in the kernel (src/Process/Win32k/Win32kGdi.cpp, init_stock): generation 1, the stock flag, the type and the slot, where the slots follow the
 * order the objects are made in (the brushes 0-5, the pens 6-8, the fonts 10-14, 16, 17, then DC_BRUSH, DC_PEN, the default palette). Win32kGdiTest checks this table. */
EXPORT HGDIOBJ WINAPI GetStockObject(int index) {
  static const BYTE slot[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10, 11, 12, 13, 14, 19, 15, 16, 17, 18};      /* by index; 0: none */
  UINT type;
  switch (index) {
    case WHITE_BRUSH: case LTGRAY_BRUSH: case GRAY_BRUSH: case DKGRAY_BRUSH: case BLACK_BRUSH: case NULL_BRUSH: case DC_BRUSH: type = NTGDI_OBJ_BRUSH; break;
    case WHITE_PEN: case BLACK_PEN: case NULL_PEN: case DC_PEN: type = NTGDI_OBJ_PEN; break;
    case OEM_FIXED_FONT: case ANSI_FIXED_FONT: case ANSI_VAR_FONT: case SYSTEM_FONT: case DEVICE_DEFAULT_FONT: case SYSTEM_FIXED_FONT: case DEFAULT_GUI_FONT: type = NTGDI_OBJ_FONT; break;
    case DEFAULT_PALETTE: type = NTGDI_OBJ_PAL; break;
    default: return NULL;
  }
  return (HGDIOBJ)(UINT_PTR)(0x01000000u | type | NTGDI_HANDLE_STOCK_OBJECT | slot[index]);
}

static int object_is_valid(HGDIOBJ h) {
  switch (gdi_handle_type(h)) {
    case NTGDI_OBJ_DC: { DWORD r; return NtGdiGetDCDword((HDC)h, NtGdiGetBkMode, &r); }
    case NTGDI_OBJ_REGION: { RECT rc; return NtGdiGetRgnBox((HRGN)h, &rc) != 0; }
    case NTGDI_OBJ_PAL: case NTGDI_OBJ_BITMAP: case NTGDI_OBJ_FONT: case NTGDI_OBJ_BRUSH: case NTGDI_OBJ_PEN: case NTGDI_OBJ_EXTPEN: return NtGdiExtGetObjectW(h, 0, NULL) > 0;
    case NTGDI_OBJ_METADC: case NTGDI_OBJ_ENHMETADC: case NTGDI_OBJ_METAFILE: case NTGDI_OBJ_ENHMETAFILE: return meta_handle_valid(h);
    default: return 0;
  }
}

EXPORT DWORD WINAPI GetObjectType(HGDIOBJ h) {
  DWORD type;
  switch (gdi_handle_type(h)) {
    case NTGDI_OBJ_PEN: type = OBJ_PEN; break;
    case NTGDI_OBJ_BRUSH: type = OBJ_BRUSH; break;
    case NTGDI_OBJ_DC: { DWORD mem = 0; type = NtGdiGetDCDword((HDC)h, NtGdiIsMemDC, &mem) && mem ? OBJ_MEMDC : OBJ_DC; break; }
    case NTGDI_OBJ_PAL: type = OBJ_PAL; break;
    case NTGDI_OBJ_FONT: type = OBJ_FONT; break;
    case NTGDI_OBJ_BITMAP: type = OBJ_BITMAP; break;
    case NTGDI_OBJ_REGION: type = OBJ_REGION; break;
    case NTGDI_OBJ_METAFILE: type = OBJ_METAFILE; break;
    case NTGDI_OBJ_ENHMETAFILE: type = OBJ_ENHMETAFILE; break;
    case NTGDI_OBJ_EXTPEN: type = OBJ_EXTPEN; break;
    case NTGDI_OBJ_METADC: type = OBJ_METADC; break;
    case NTGDI_OBJ_ENHMETADC: type = OBJ_ENHMETADC; break;
    default: type = 0; break;
  }
  if (!type || !object_is_valid(h)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
  return type;
}

EXPORT int WINAPI GetObjectW(HANDLE h, int count, LPVOID buffer) { return NtGdiExtGetObjectW(h, count, buffer); }

static void logfont_WtoA(const LOGFONTW *w, LOGFONTA *a) {
  memcpy(a, w, FIELD_OFFSET(LOGFONTW, lfFaceName));
  WideCharToMultiByte(CP_ACP, 0, w->lfFaceName, -1, a->lfFaceName, LF_FACESIZE, NULL, NULL);
  a->lfFaceName[LF_FACESIZE - 1] = 0;
}
static void logfont_AtoW(const LOGFONTA *a, LOGFONTW *w) {
  memcpy(w, a, FIELD_OFFSET(LOGFONTA, lfFaceName));
  MultiByteToWideChar(CP_ACP, 0, a->lfFaceName, -1, w->lfFaceName, LF_FACESIZE);
  w->lfFaceName[LF_FACESIZE - 1] = 0;
}

EXPORT int WINAPI GetObjectA(HANDLE h, int count, LPVOID buffer) {
  if (gdi_handle_type(h) == NTGDI_OBJ_FONT) {
    LOGFONTW lw;
    LOGFONTA la;
    if (!NtGdiExtGetObjectW(h, sizeof(lw), &lw)) return 0;
    if (!buffer) return sizeof(LOGFONTA);
    if (count <= 0) return 0;
    logfont_WtoA(&lw, &la);
    if (count > (int)sizeof(la)) count = sizeof(la);
    memcpy(buffer, &la, count);
    return count;
  }
  return NtGdiExtGetObjectW(h, count, buffer);
}

EXPORT BOOL WINAPI DeleteObject(HGDIOBJ h) {
  const BOOL r = NtGdiDeleteObjectApp(h);
  if (r) meta_object_deleted(h);
  return r;
}
EXPORT BOOL WINAPI DeleteDC(HDC hdc) { if (is_meta_dc(hdc)) return meta_delete_dc(hdc);
  if (is_meta_dc(hdc)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }      /* (metafile DCs are not made by this gdi32 yet) */
  const BOOL r = NtGdiDeleteObjectApp(hdc);
  return r;
}
EXPORT BOOL WINAPI InternalDeleteObject(HGDIOBJ h) { return DeleteObject(h); }
EXPORT BOOL WINAPI UnrealizeObject(HGDIOBJ h) { return h ? NtGdiUnrealizeObject(h) : FALSE; }

EXPORT HGDIOBJ WINAPI GetCurrentObject(HDC hdc, UINT type) {
  switch (type) {
    case OBJ_EXTPEN: type = OBJ_PEN; /* fall through */
    case OBJ_PEN: case OBJ_BRUSH: case OBJ_PAL: case OBJ_FONT: case OBJ_BITMAP: return NtGdiGetDCObject(RD(hdc), type);
    default: SetLastError(ERROR_INVALID_PARAMETER); return NULL;
  }
}

EXPORT int WINAPI ExtSelectClipRgn(HDC hdc, HRGN rgn, int mode) { if (is_meta_dc(hdc)) meta_regionop(hdc, EMR_EXTSELECTCLIPRGN, rgn, NULL, 0, 0, mode, 1); return NtGdiExtSelectClipRgn(RD(hdc), rgn, mode); }
EXPORT int WINAPI SelectClipRgn(HDC hdc, HRGN rgn) { return ExtSelectClipRgn(hdc, rgn, RGN_COPY); }

EXPORT HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ obj) { if (is_meta_dc(hdc)) return meta_select(hdc, obj);
  if (!obj) return NULL;
  switch (gdi_handle_type(obj)) {
    case NTGDI_OBJ_BITMAP: return NtGdiSelectBitmap(RD(hdc), obj);
    case NTGDI_OBJ_BRUSH: return NtGdiSelectBrush(RD(hdc), obj);
    case NTGDI_OBJ_PEN: case NTGDI_OBJ_EXTPEN: return NtGdiSelectPen(RD(hdc), obj);
    case NTGDI_OBJ_FONT: return NtGdiSelectFont(RD(hdc), obj);
    case NTGDI_OBJ_REGION: return (HGDIOBJ)(UINT_PTR)(UINT)NtGdiExtSelectClipRgn(RD(hdc), obj, RGN_COPY);
    default: SetLastError(ERROR_INVALID_HANDLE); return NULL;
  }
}
EXPORT HGDIOBJ WINAPI SelectBrushLocal(HDC hdc, HGDIOBJ obj) { return NtGdiSelectBrush(RD(hdc), obj); }
EXPORT HGDIOBJ WINAPI SelectFontLocal(HDC hdc, HGDIOBJ obj) { return NtGdiSelectFont(RD(hdc), obj); }

/* ---- pens ------------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HPEN WINAPI CreatePen(int style, int width, COLORREF color) {
  if (style == PS_NULL) { HPEN null_pen = GetStockObject(NULL_PEN); if (null_pen) return null_pen; }
  return NtGdiCreatePen(style, width, color, NULL);
}
EXPORT HPEN WINAPI CreatePenIndirect(const LOGPEN *pen) {
  if (!pen) return NULL;
  return CreatePen(pen->lopnStyle, pen->lopnWidth.x, pen->lopnColor);
}

/* ---- brushes ---------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HBRUSH WINAPI CreateSolidBrush(COLORREF color) { return NtGdiCreateSolidBrush(color, NULL); }
EXPORT HBRUSH WINAPI CreateHatchBrush(int style, COLORREF color) {
  if (style < HS_HORIZONTAL || style >= HS_API_MAX) return NULL;
  return NtGdiCreateHatchBrushInternal(style, color, FALSE);
}
EXPORT HBRUSH WINAPI CreatePatternBrush(HBITMAP bitmap) { return NtGdiCreatePatternBrushInternal(bitmap, FALSE, FALSE); }

static DWORD dib_packed_size(const BITMAPINFO *bmi, UINT usage);      /* header + colours + pixels (dib.inc); 0 if the header is bad */

static HBRUSH make_dib_brush(const void *data, UINT usage, BOOL is_8x8, BOOL pen) {
  const DWORD size = dib_packed_size((const BITMAPINFO *)data, usage);
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  return NtGdiCreateDIBBrush(data, usage, size, is_8x8, pen, data);
}
EXPORT HBRUSH WINAPI CreateDIBPatternBrushPt(const void *data, UINT usage) {
  if (!data) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  return make_dib_brush(data, usage, FALSE, FALSE);
}
EXPORT HBRUSH WINAPI CreateDIBPatternBrush(HGLOBAL global, UINT usage) {
  void *info = GlobalLock(global);
  if (!info) return NULL;
  HBRUSH r = CreateDIBPatternBrushPt(info, usage);
  GlobalUnlock(global);
  return r;
}
EXPORT HBRUSH WINAPI CreateBrushIndirect(const LOGBRUSH *brush) {
  if (!brush) return NULL;
  switch (brush->lbStyle) {
    case BS_SOLID: return CreateSolidBrush(brush->lbColor);
    case BS_HATCHED: return CreateHatchBrush((int)brush->lbHatch, brush->lbColor);
    case BS_NULL: return GetStockObject(NULL_BRUSH);
    case BS_PATTERN: return NtGdiCreatePatternBrushInternal((HBITMAP)brush->lbHatch, FALSE, FALSE);
    case BS_PATTERN8X8: return NtGdiCreatePatternBrushInternal((HBITMAP)brush->lbHatch, FALSE, TRUE);
    case BS_DIBPATTERN: return CreateDIBPatternBrush((HGLOBAL)brush->lbHatch, brush->lbColor);
    case BS_DIBPATTERNPT: return CreateDIBPatternBrushPt((const void *)brush->lbHatch, brush->lbColor);
    case BS_DIBPATTERN8X8: if (!brush->lbHatch) return NULL; return make_dib_brush((const void *)brush->lbHatch, brush->lbColor, TRUE, FALSE);
    default: SetLastError(ERROR_INVALID_PARAMETER); return NULL;
  }
}

EXPORT HPEN WINAPI ExtCreatePen(DWORD style, DWORD width, const LOGBRUSH *brush, DWORD style_count, const DWORD *style_bits) {
  HBRUSH hbrush = NULL;
  ULONG_PTR hatch;
  void *locked = NULL;
  DWORD dib_size = 0;
  HPEN pen;
  if (!brush) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if ((style & PS_STYLE_MASK) == PS_USERSTYLE) {
    if ((INT)style_count <= 0) return NULL;
    if (!style_bits) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  } else if (style_count || style_bits) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  hatch = brush->lbHatch;
  switch (brush->lbStyle) {
    case BS_PATTERN8X8: case BS_PATTERN:
      hbrush = NtGdiCreatePatternBrushInternal((HBITMAP)hatch, TRUE, brush->lbStyle == BS_PATTERN8X8);
      if (!hbrush) return NULL;
      break;
    case BS_DIBPATTERN: locked = GlobalLock((HGLOBAL)hatch); if (!locked) return NULL; hatch = (ULONG_PTR)locked; /* fall through */
    case BS_DIBPATTERN8X8: case BS_DIBPATTERNPT:
      dib_size = dib_packed_size((const BITMAPINFO *)hatch, brush->lbColor);
      if (!dib_size) { if (locked) GlobalUnlock((HGLOBAL)brush->lbHatch); SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
      hbrush = NtGdiCreateDIBBrush((const void *)hatch, brush->lbColor, dib_size, brush->lbStyle == BS_DIBPATTERN8X8, TRUE, (const void *)hatch);
      if (locked) GlobalUnlock((HGLOBAL)brush->lbHatch);
      if (!hbrush) return NULL;
      break;
    default: break;
  }
  if ((style & PS_TYPE_MASK) == PS_COSMETIC && (style & PS_STYLE_MASK) == PS_NULL && !hbrush) {
    if (width != 1 && brush->lbStyle != BS_SOLID) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    return GetStockObject(NULL_PEN);
  }
  pen = NtGdiExtCreatePen(style, width, brush->lbStyle, brush->lbColor, brush->lbHatch, hatch, style_count, style_bits, dib_size, FALSE, hbrush);
  if (!pen) { if (hbrush) DeleteObject(hbrush); SetLastError(ERROR_INVALID_PARAMETER); }
  return pen;
}

/* ---- palettes ----------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HPALETTE WINAPI CreatePalette(const LOGPALETTE *palette) {
  if (!palette) return NULL;
  return NtGdiCreatePaletteInternal(palette, palette->palNumEntries);
}
EXPORT HPALETTE WINAPI CreateHalftonePalette(HDC hdc) { return NtGdiCreateHalftonePalette(hdc); }
EXPORT UINT WINAPI GetPaletteEntries(HPALETTE pal, UINT start, UINT count, PALETTEENTRY *entries) { return NtGdiDoPalette(pal, (WORD)start, (WORD)count, entries, NtGdiGetPaletteEntries, FALSE); }
EXPORT UINT WINAPI SetPaletteEntries(HPALETTE pal, UINT start, UINT count, const PALETTEENTRY *entries) { return NtGdiDoPalette(pal, (WORD)start, (WORD)count, (void *)entries, NtGdiSetPaletteEntries, TRUE); }
EXPORT BOOL WINAPI AnimatePalette(HPALETTE pal, UINT start, UINT count, const PALETTEENTRY *entries) { return NtGdiDoPalette(pal, (WORD)start, (WORD)count, (void *)entries, NtGdiAnimatePalette, TRUE); }
EXPORT UINT WINAPI GetSystemPaletteEntries(HDC hdc, UINT start, UINT count, PALETTEENTRY *entries) { return NtGdiDoPalette(RD(hdc), (WORD)start, (WORD)count, entries, NtGdiGetSystemPaletteEntries, FALSE); }
EXPORT UINT WINAPI GetNearestPaletteIndex(HPALETTE pal, COLORREF color) { return NtGdiGetNearestPaletteIndex(pal, color); }
EXPORT BOOL WINAPI ResizePalette(HPALETTE pal, UINT count) { return NtGdiResizePalette(pal, count); }
EXPORT UINT WINAPI GetSystemPaletteUse(HDC hdc) { return NtGdiGetSystemPaletteUse(hdc); }
EXPORT UINT WINAPI SetSystemPaletteUse(HDC hdc, UINT use) { return NtGdiSetSystemPaletteUse(RD(hdc), use); }
EXPORT BOOL WINAPI UpdateColors(HDC hdc) { return NtGdiUpdateColors(hdc); }
EXPORT HPALETTE WINAPI SelectPalette(HDC hdc, HPALETTE pal, BOOL background) { if (is_meta_dc(hdc)) return meta_select_palette(hdc, pal); return NtUserSelectPalette(RD(hdc), pal, (WORD)background); }
EXPORT UINT WINAPI RealizePalette(HDC hdc) { MREC(hdc, EMR_REALIZEPALETTE, 0x0035, 0, 0); return NtUserRealizePalette(hdc); }
EXPORT COLORREF WINAPI GetNearestColor(HDC hdc, COLORREF color) { return NtGdiGetNearestColor(RD(hdc), color); }

/* ---- regions ------------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT HRGN WINAPI CreateRectRgn(int l, int t, int r, int b) { return NtGdiCreateRectRgn(l, t, r, b); }
EXPORT HRGN WINAPI CreateRectRgnIndirect(const RECT *rc) { return rc ? NtGdiCreateRectRgn(rc->left, rc->top, rc->right, rc->bottom) : NULL; }
EXPORT HRGN WINAPI CreateEllipticRgn(int l, int t, int r, int b) { return NtGdiCreateEllipticRgn(l, t, r, b); }
EXPORT HRGN WINAPI CreateEllipticRgnIndirect(const RECT *rc) { return rc ? NtGdiCreateEllipticRgn(rc->left, rc->top, rc->right, rc->bottom) : NULL; }
EXPORT HRGN WINAPI CreateRoundRectRgn(int l, int t, int r, int b, int w, int h) { return NtGdiCreateRoundRectRgn(l, t, r, b, w, h); }
EXPORT HRGN WINAPI CreatePolyPolygonRgn(const POINT *points, const INT *counts, int polygons, int mode) {
  return (HRGN)(UINT_PTR)NtGdiPolyPolyDraw((HDC)(UINT_PTR)mode, points, (const ULONG *)counts, polygons, NtGdiPolyPolygonRgn);
}
EXPORT HRGN WINAPI CreatePolygonRgn(const POINT *points, int count, int mode) { INT c = count; return CreatePolyPolygonRgn(points, &c, 1, mode); }
EXPORT BOOL WINAPI SetRectRgn(HRGN rgn, int l, int t, int r, int b) { return NtGdiSetRectRgn(rgn, l, t, r, b); }
EXPORT int WINAPI CombineRgn(HRGN dest, HRGN a, HRGN b, int mode) { return NtGdiCombineRgn(dest, a, b, mode); }
EXPORT BOOL WINAPI EqualRgn(HRGN a, HRGN b) { return NtGdiEqualRgn(a, b); }
EXPORT int WINAPI GetRgnBox(HRGN rgn, RECT *rc) { return NtGdiGetRgnBox(rgn, rc); }
EXPORT int WINAPI OffsetRgn(HRGN rgn, int x, int y) { return NtGdiOffsetRgn(rgn, x, y); }
EXPORT BOOL WINAPI PtInRegion(HRGN rgn, int x, int y) { return NtGdiPtInRegion(rgn, x, y); }
EXPORT BOOL WINAPI RectInRegion(HRGN rgn, const RECT *rc) { return NtGdiRectInRegion(rgn, rc); }
EXPORT HRGN WINAPI ExtCreateRegion(const XFORM *xform, DWORD count, const RGNDATA *data) { return NtGdiExtCreateRegion(xform, count, data); }
EXPORT DWORD WINAPI GetRegionData(HRGN rgn, DWORD count, RGNDATA *data) { return NtGdiGetRegionData(rgn, count, data); }
EXPORT BOOL WINAPI FillRgn(HDC hdc, HRGN rgn, HBRUSH brush) { if (is_meta_dc(hdc)) return meta_regionop(hdc, EMR_FILLRGN, rgn, brush, 0, 0, 0, 0); return NtGdiFillRgn(RD(hdc), rgn, brush); }
EXPORT BOOL WINAPI FrameRgn(HDC hdc, HRGN rgn, HBRUSH brush, int w, int h) { if (is_meta_dc(hdc)) return meta_regionop(hdc, EMR_FRAMERGN, rgn, brush, w, h, 0, 0); return NtGdiFrameRgn(RD(hdc), rgn, brush, w, h); }
EXPORT BOOL WINAPI InvertRgn(HDC hdc, HRGN rgn) { if (is_meta_dc(hdc)) return meta_regionop(hdc, EMR_INVERTRGN, rgn, NULL, 0, 0, 0, 0); return NtGdiInvertRgn(RD(hdc), rgn); }
EXPORT BOOL WINAPI PaintRgn(HDC hdc, HRGN rgn) { if (is_meta_dc(hdc)) return meta_regionop(hdc, EMR_PAINTRGN, rgn, NULL, 0, 0, 0, 0); return NtGdiFillRgn(RD(hdc), rgn, GetCurrentObject(RD(hdc), OBJ_BRUSH)); }
EXPORT int WINAPI GetClipRgn(HDC hdc, HRGN rgn) { return NtGdiGetRandomRgn(RD(hdc), rgn, 1); }
EXPORT int WINAPI GetMetaRgn(HDC hdc, HRGN rgn) { return NtGdiGetRandomRgn(RD(hdc), rgn, 2); }
EXPORT int WINAPI SetMetaRgn(HDC hdc) { MREC(hdc, EMR_SETMETARGN, -1, 0, 0); return NtGdiSetMetaRgn(hdc); }
EXPORT int WINAPI GetRandomRgn(HDC hdc, HRGN rgn, INT code) { return NtGdiGetRandomRgn(RD(hdc), rgn, code); }
EXPORT int WINAPI GetClipBox(HDC hdc, RECT *rc) { return NtGdiGetAppClipBox(RD(hdc), rc); }
EXPORT int WINAPI IntersectClipRect(HDC hdc, int l, int t, int r, int b) { MREC(hdc, EMR_INTERSECTCLIPRECT, 0x0416, 0, 4, l, t, r, b); return NtGdiIntersectClipRect(RD(hdc), l, t, r, b); }
EXPORT int WINAPI ExcludeClipRect(HDC hdc, int l, int t, int r, int b) { MREC(hdc, EMR_EXCLUDECLIPRECT, 0x0415, 0, 4, l, t, r, b); return NtGdiExcludeClipRect(RD(hdc), l, t, r, b); }
EXPORT int WINAPI OffsetClipRgn(HDC hdc, int x, int y) { MREC(hdc, EMR_OFFSETCLIPRGN, 0x0220, 0, 2, x, y); return NtGdiOffsetClipRgn(RD(hdc), x, y); }
EXPORT BOOL WINAPI PtVisible(HDC hdc, int x, int y) { return NtGdiPtVisible(RD(hdc), x, y); }
EXPORT BOOL WINAPI RectVisible(HDC hdc, const RECT *rc) { return NtGdiRectVisible(RD(hdc), rc); }
