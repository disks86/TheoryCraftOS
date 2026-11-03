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
/* gdi32.dll: fonts and text. Included by gdi32.c. */

/* ---- character sets and code pages ------------------------------------------------------------------------------------------------------------------------- */

static const struct { BYTE charset; UINT codepage; DWORD fs; } charset_table[] = {
  { ANSI_CHARSET, 1252, FS_LATIN1 }, { EASTEUROPE_CHARSET, 1250, FS_LATIN2 }, { RUSSIAN_CHARSET, 1251, FS_CYRILLIC }, { GREEK_CHARSET, 1253, FS_GREEK },
  { TURKISH_CHARSET, 1254, FS_TURKISH }, { HEBREW_CHARSET, 1255, FS_HEBREW }, { ARABIC_CHARSET, 1256, FS_ARABIC }, { BALTIC_CHARSET, 1257, FS_BALTIC },
  { VIETNAMESE_CHARSET, 1258, FS_VIETNAMESE }, { THAI_CHARSET, 874, FS_THAI }, { SHIFTJIS_CHARSET, 932, FS_JISJAPAN }, { GB2312_CHARSET, 936, FS_CHINESESIMP },
  { HANGEUL_CHARSET, 949, FS_WANSUNG }, { CHINESEBIG5_CHARSET, 950, FS_CHINESETRAD }, { JOHAB_CHARSET, 1361, FS_JOHAB }, { MAC_CHARSET, 10000, FS_LATIN1 },
  { OEM_CHARSET, 437, FS_LATIN1 }, { SYMBOL_CHARSET, 42, 0x80000000u },
};
#define CHARSET_COUNT (sizeof(charset_table) / sizeof(charset_table[0]))

EXPORT BOOL WINAPI TranslateCharsetInfo(DWORD *src, CHARSETINFO *info, DWORD flags) {
  if (!info) return FALSE;
  for (unsigned i = 0; i < CHARSET_COUNT; ++i) {
    int match = 0;
    switch (flags) {
      case TCI_SRCFONTSIG: {
        const DWORD *sig = src;
        match = sig && (sig[0] & charset_table[i].fs) && charset_table[i].fs != 0x80000000u;
        if (!match && sig && (sig[0] & 0x80000000u)) match = charset_table[i].charset == SYMBOL_CHARSET;
        break;
      }
      case TCI_SRCCODEPAGE: match = charset_table[i].codepage == (UINT)(ULONG_PTR)src; break;
      case TCI_SRCCHARSET: match = charset_table[i].charset == (BYTE)(ULONG_PTR)src; break;
      default: return FALSE;
    }
    if (!match) continue;
    info->ciCharset = charset_table[i].charset;
    info->ciACP = charset_table[i].charset == OEM_CHARSET ? GetOEMCP() : charset_table[i].codepage;
    memset(&info->fs, 0, sizeof(info->fs));
    info->fs.fsCsb[0] = charset_table[i].fs;
    return TRUE;
  }
  return FALSE;
}

EXPORT int WINAPI GetTextCharsetInfo(HDC hdc, LPFONTSIGNATURE fs, DWORD flags) { return NtGdiGetTextCharsetInfo(RD(hdc), fs, flags); }
EXPORT int WINAPI GetTextCharset(HDC hdc) { return NtGdiGetTextCharsetInfo(RD(hdc), NULL, 0); }
EXPORT UINT WINAPI GdiGetCodePage(HDC hdc) {
  const UINT cs = NtGdiGetTextCharsetInfo(RD(hdc), NULL, 0);
  CHARSETINFO ci;
  if (cs == ANSI_CHARSET || cs == DEFAULT_CHARSET || cs == GDI_ERROR) return GetACP();
  if (TranslateCharsetInfo((DWORD *)(ULONG_PTR)cs, &ci, TCI_SRCCHARSET)) return ci.ciACP;
  return GetACP();
}

/* The wide characters of an ANSI string in the code page of the DC's font. `map` (optional, `count` entries) gets, per byte, the index of the wide character it belongs to. */
static WCHAR *text_AtoW(HDC hdc, const char *s, int count, int *wlen, int *map) {
  const UINT cp = GdiGetCodePage(hdc);
  WCHAR *w = mem_alloc_n((SIZE_T)count + 1, sizeof(WCHAR));
  int n = 0;
  if (!w) return NULL;
  for (int i = 0; i < count; ++i) {
    int len = 1;
    if ((BYTE)s[i] >= 0x80 && i + 1 < count && IsDBCSLeadByteEx(cp, (BYTE)s[i])) len = 2;
    if (map) { map[i] = n; if (len == 2) map[i + 1] = n; }
    if (!MultiByteToWideChar(cp, 0, s + i, len, w + n, 1)) w[n] = 0x30fb;
    ++n;
    i += len - 1;
  }
  *wlen = n;
  return w;
}

/* ---- fonts -------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HFONT WINAPI CreateFontIndirectExW(const ENUMLOGFONTEXDVW *lfex) {
  if (!lfex) return NULL;
  return NtGdiHfontCreate(lfex, sizeof(*lfex), 0, 0, NULL);
}
EXPORT HFONT WINAPI CreateFontIndirectW(const LOGFONTW *lf) {
  ENUMLOGFONTEXDVW ex;
  if (!lf) return NULL;
  memset(&ex, 0, sizeof(ex));
  ex.elfEnumLogfontEx.elfLogFont = *lf;
  return CreateFontIndirectExW(&ex);
}
EXPORT HFONT WINAPI CreateFontIndirectExA(const ENUMLOGFONTEXDVA *lfex) {
  ENUMLOGFONTEXDVW ex;
  if (!lfex) return NULL;
  memset(&ex, 0, sizeof(ex));
  logfont_AtoW(&lfex->elfEnumLogfontEx.elfLogFont, &ex.elfEnumLogfontEx.elfLogFont);
  MultiByteToWideChar(CP_ACP, 0, (const char *)lfex->elfEnumLogfontEx.elfFullName, -1, ex.elfEnumLogfontEx.elfFullName, LF_FULLFACESIZE);
  MultiByteToWideChar(CP_ACP, 0, (const char *)lfex->elfEnumLogfontEx.elfStyle, -1, ex.elfEnumLogfontEx.elfStyle, LF_FACESIZE);
  MultiByteToWideChar(CP_ACP, 0, (const char *)lfex->elfEnumLogfontEx.elfScript, -1, ex.elfEnumLogfontEx.elfScript, LF_FACESIZE);
  ex.elfDesignVector = lfex->elfDesignVector;
  return CreateFontIndirectExW(&ex);
}
EXPORT HFONT WINAPI CreateFontIndirectA(const LOGFONTA *lf) {
  LOGFONTW w;
  if (!lf) return NULL;
  logfont_AtoW(lf, &w);
  return CreateFontIndirectW(&w);
}
EXPORT HFONT WINAPI CreateFontW(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike, DWORD charset, DWORD outp, DWORD clip, DWORD quality, DWORD pitch,
                                LPCWSTR face) {
  LOGFONTW lf;
  memset(&lf, 0, sizeof(lf));
  lf.lfHeight = h; lf.lfWidth = w; lf.lfEscapement = esc; lf.lfOrientation = orient; lf.lfWeight = weight; lf.lfItalic = (BYTE)italic; lf.lfUnderline = (BYTE)underline;
  lf.lfStrikeOut = (BYTE)strike; lf.lfCharSet = (BYTE)charset; lf.lfOutPrecision = (BYTE)outp; lf.lfClipPrecision = (BYTE)clip; lf.lfQuality = (BYTE)quality;
  lf.lfPitchAndFamily = (BYTE)pitch;
  if (face) { int i = 0; for (; i < LF_FACESIZE - 1 && face[i]; ++i) lf.lfFaceName[i] = face[i]; lf.lfFaceName[i] = 0; }
  return CreateFontIndirectW(&lf);
}
EXPORT HFONT WINAPI CreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike, DWORD charset, DWORD outp, DWORD clip, DWORD quality, DWORD pitch,
                                LPCSTR face) {
  WCHAR *wf = dup_AtoW(face, CP_ACP);
  HFONT f = CreateFontW(h, w, esc, orient, weight, italic, underline, strike, charset, outp, clip, quality, pitch, wf);
  mem_free(wf);
  return f;
}

EXPORT int WINAPI AddFontResourceExW(LPCWSTR name, DWORD flags, PVOID reserved) { return name ? NtGdiAddFontResourceW(name, lstrlenW(name), 1, flags, 0, reserved) : 0; }
EXPORT int WINAPI AddFontResourceW(LPCWSTR name) { return AddFontResourceExW(name, 0, NULL); }
EXPORT int WINAPI AddFontResourceExA(LPCSTR name, DWORD flags, PVOID reserved) { WCHAR *w = dup_AtoW(name, CP_ACP); int r = w ? AddFontResourceExW(w, flags, reserved) : 0; mem_free(w); return r; }
EXPORT int WINAPI AddFontResourceA(LPCSTR name) { return AddFontResourceExA(name, 0, NULL); }
EXPORT BOOL WINAPI RemoveFontResourceExW(LPCWSTR name, DWORD flags, PVOID reserved) { return name ? NtGdiRemoveFontResourceW(name, lstrlenW(name), 1, flags, 0, reserved) : FALSE; }
EXPORT BOOL WINAPI RemoveFontResourceW(LPCWSTR name) { return RemoveFontResourceExW(name, 0, NULL); }
EXPORT BOOL WINAPI RemoveFontResourceExA(LPCSTR name, DWORD flags, PVOID reserved) { WCHAR *w = dup_AtoW(name, CP_ACP); BOOL r = w ? RemoveFontResourceExW(w, flags, reserved) : FALSE; mem_free(w); return r; }
EXPORT BOOL WINAPI RemoveFontResourceA(LPCSTR name) { return RemoveFontResourceExA(name, 0, NULL); }
EXPORT HANDLE WINAPI AddFontMemResourceEx(PVOID font, DWORD size, PVOID reserved, DWORD *count) {
  if (!font || !size || !count) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  return NtGdiAddFontMemResourceEx(font, size, NULL, 0, count);
}
EXPORT BOOL WINAPI RemoveFontMemResourceEx(HANDLE handle) { return NtGdiRemoveFontMemResourceEx(handle); }
EXPORT BOOL WINAPI CreateScalableFontResourceW(DWORD hidden, LPCWSTR resource, LPCWSTR font, LPCWSTR path) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT BOOL WINAPI CreateScalableFontResourceA(DWORD hidden, LPCSTR resource, LPCSTR font, LPCSTR path) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }

/* ---- text output -------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI ExtTextOutW(HDC hdc, int x, int y, UINT flags, const RECT *rect, LPCWSTR str, UINT count, const INT *dx) { if (is_meta_dc(hdc)) return meta_exttextout(hdc, x, y, flags, rect, str, count, dx);
  if ((int)count < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return NtGdiExtTextOutW(RD(hdc), x, y, flags, rect, str, count, dx, 0);
}
EXPORT BOOL WINAPI ExtTextOutA(HDC hdc, int x, int y, UINT flags, const RECT *rect, LPCSTR str, UINT count, const INT *dx) {
  int wlen = 0;
  WCHAR *w;
  INT *wdx = NULL;
  BOOL r;
  if ((int)count < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!count || !str) return ExtTextOutW(hdc, x, y, flags, rect, NULL, 0, NULL);
  int *map = dx ? mem_alloc_n(count, sizeof(int)) : NULL;
  w = text_AtoW(hdc, str, (int)count, &wlen, map);
  if (!w) { mem_free(map); return FALSE; }
  if (dx) {
    /* a double-byte character has one advance in the wide string: the one of its first byte (Wine adds the two) */
    wdx = mem_alloc_n(wlen ? wlen : 1, sizeof(INT));
    if (wdx) for (UINT i = 0; i < count; ++i) { wdx[map[i]] += dx[i]; }
  }
  r = ExtTextOutW(hdc, x, y, flags, rect, w, wlen, wdx);
  mem_free(w); mem_free(map); mem_free(wdx);
  return r;
}
EXPORT BOOL WINAPI TextOutW(HDC hdc, int x, int y, LPCWSTR str, int count) { return ExtTextOutW(hdc, x, y, 0, NULL, str, count, NULL); }
EXPORT BOOL WINAPI TextOutA(HDC hdc, int x, int y, LPCSTR str, int count) { return ExtTextOutA(hdc, x, y, 0, NULL, str, count, NULL); }
EXPORT BOOL WINAPI PolyTextOutW(HDC hdc, const POLYTEXTW *pt, int count) {
  for (; count > 0; --count, ++pt) if (!ExtTextOutW(hdc, pt->x, pt->y, pt->uiFlags, &pt->rcl, pt->lpstr, pt->n, pt->pdx)) return FALSE;
  return TRUE;
}
EXPORT BOOL WINAPI PolyTextOutA(HDC hdc, const POLYTEXTA *pt, int count) {
  for (; count > 0; --count, ++pt) if (!ExtTextOutA(hdc, pt->x, pt->y, pt->uiFlags, &pt->rcl, pt->lpstr, pt->n, pt->pdx)) return FALSE;
  return TRUE;
}

/* ---- text extents ------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI GetTextExtentExPointW(HDC hdc, LPCWSTR str, int count, int max_ext, LPINT nfit, LPINT dxs, LPSIZE size) {
  if (count < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return NtGdiGetTextExtentExW(RD(hdc), str, count, max_ext, nfit, dxs, size, 0);
}
EXPORT BOOL WINAPI GetTextExtentExPointI(HDC hdc, LPWORD indices, int count, int max_ext, LPINT nfit, LPINT dxs, LPSIZE size) {
  if (count < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return NtGdiGetTextExtentExW(RD(hdc), (const WCHAR *)indices, count, max_ext, nfit, dxs, size, 1);
}
EXPORT BOOL WINAPI GetTextExtentExPointA(HDC hdc, LPCSTR str, int count, int max_ext, LPINT nfit, LPINT dxs, LPSIZE size) {
  int wlen = 0, fit = 0;
  WCHAR *w;
  INT *wdxs = NULL;
  int *map = NULL;
  BOOL r;
  if (count < 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!str && count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  map = mem_alloc_n((SIZE_T)count + 1, sizeof(int));
  w = text_AtoW(hdc, str, count, &wlen, map);
  if (!w) { mem_free(map); return FALSE; }
  if (dxs) wdxs = mem_alloc_n((SIZE_T)wlen + 1, sizeof(INT));
  r = GetTextExtentExPointW(hdc, w, wlen, max_ext, nfit ? &fit : NULL, wdxs, size);
  if (r) {
    if (nfit) { int bytes = 0; while (bytes < count && map[bytes] < fit) ++bytes; *nfit = bytes; }
    if (dxs && wdxs) for (int i = 0; i < count; ++i) dxs[i] = wdxs[map[i]];
  }
  mem_free(w); mem_free(map); mem_free(wdxs);
  return r;
}
EXPORT BOOL WINAPI GetTextExtentPoint32W(HDC hdc, LPCWSTR str, int count, LPSIZE size) { return GetTextExtentExPointW(hdc, str, count, 0, NULL, NULL, size); }
EXPORT BOOL WINAPI GetTextExtentPoint32A(HDC hdc, LPCSTR str, int count, LPSIZE size) { return GetTextExtentExPointA(hdc, str, count, 0, NULL, NULL, size); }
EXPORT BOOL WINAPI GetTextExtentPointW(HDC hdc, LPCWSTR str, int count, LPSIZE size) { return GetTextExtentPoint32W(hdc, str, count, size); }
EXPORT BOOL WINAPI GetTextExtentPointA(HDC hdc, LPCSTR str, int count, LPSIZE size) { return GetTextExtentPoint32A(hdc, str, count, size); }
EXPORT BOOL WINAPI GetTextExtentPointI(HDC hdc, LPWORD indices, int count, LPSIZE size) { return GetTextExtentExPointI(hdc, indices, count, 0, NULL, NULL, size); }

/* ---- text metrics, face names ------------------------------------------------------------------------------------------------------------------------------ */

static void text_metric_WtoA(const TEXTMETRICW *w, TEXTMETRICA *a, UINT cp) {
  a->tmHeight = w->tmHeight; a->tmAscent = w->tmAscent; a->tmDescent = w->tmDescent; a->tmInternalLeading = w->tmInternalLeading; a->tmExternalLeading = w->tmExternalLeading;
  a->tmAveCharWidth = w->tmAveCharWidth; a->tmMaxCharWidth = w->tmMaxCharWidth; a->tmWeight = w->tmWeight; a->tmOverhang = w->tmOverhang;
  a->tmDigitizedAspectX = w->tmDigitizedAspectX; a->tmDigitizedAspectY = w->tmDigitizedAspectY;
  if (w->tmFirstChar > 255) { a->tmFirstChar = 255; a->tmLastChar = 0; } else { a->tmFirstChar = (BYTE)w->tmFirstChar; a->tmLastChar = w->tmLastChar > 255 ? 255 : (BYTE)w->tmLastChar; }
  WideCharToMultiByte(cp, 0, &w->tmDefaultChar, 1, (char *)&a->tmDefaultChar, 1, NULL, NULL);
  WideCharToMultiByte(cp, 0, &w->tmBreakChar, 1, (char *)&a->tmBreakChar, 1, NULL, NULL);
  a->tmItalic = w->tmItalic; a->tmUnderlined = w->tmUnderlined; a->tmStruckOut = w->tmStruckOut; a->tmPitchAndFamily = w->tmPitchAndFamily; a->tmCharSet = w->tmCharSet;
}
EXPORT BOOL WINAPI GetTextMetricsW(HDC hdc, TEXTMETRICW *tm) { return NtGdiGetTextMetricsW(RD(hdc), tm, sizeof(*tm)); }
EXPORT BOOL WINAPI GetTextMetricsA(HDC hdc, TEXTMETRICA *tm) {
  TEXTMETRICW w;
  if (!NtGdiGetTextMetricsW(RD(hdc), &w, sizeof(w))) return FALSE;
  text_metric_WtoA(&w, tm, GdiGetCodePage(hdc));
  return TRUE;
}
EXPORT int WINAPI GetTextFaceW(HDC hdc, int count, LPWSTR name) { return NtGdiGetTextFaceW(RD(hdc), count, name, FALSE); }
EXPORT int WINAPI GetTextFaceAliasW(HDC hdc, int count, LPWSTR name) { return NtGdiGetTextFaceW(RD(hdc), count, name, TRUE); }
EXPORT int WINAPI GetTextFaceA(HDC hdc, int count, LPSTR name) {
  int res = NtGdiGetTextFaceW(RD(hdc), 0, NULL, FALSE);
  WCHAR *w;
  if (res <= 0) return 0;
  w = mem_alloc_n((SIZE_T)res + 1, sizeof(WCHAR));
  if (!w) return 0;
  NtGdiGetTextFaceW(RD(hdc), res, w, FALSE);
  if (name) {
    if (count > 0) {
      res = WideCharToMultiByte(CP_ACP, 0, w, -1, name, count, NULL, NULL);
      if (res == 0) res = count;
      name[count - 1] = 0;
    } else res = 0;
  } else res = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
  mem_free(w);
  return res;
}
EXPORT UINT WINAPI GetOutlineTextMetricsW(HDC hdc, UINT size, OUTLINETEXTMETRICW *otm) { return NtGdiGetOutlineTextMetricsInternalW(RD(hdc), size, otm, 0); }
EXPORT UINT WINAPI GetOutlineTextMetricsA(HDC hdc, UINT size, OUTLINETEXTMETRICA *out) {
  const UINT wsize = NtGdiGetOutlineTextMetricsInternalW(RD(hdc), 0, NULL, 0);
  OUTLINETEXTMETRICW *w;
  UINT needed = sizeof(OUTLINETEXTMETRICA), result = 0;
  char *ptr;
  const UINT cp = CP_ACP;
  PSTR *fields_a[4];
  PSTR fields_w[4];
  if (!wsize) return 0;
  w = mem_alloc(wsize);
  if (!w) return 0;
  if (!NtGdiGetOutlineTextMetricsInternalW(RD(hdc), wsize, w, 0)) { mem_free(w); return 0; }
  fields_w[0] = w->otmpFamilyName; fields_w[1] = w->otmpFaceName; fields_w[2] = w->otmpStyleName; fields_w[3] = w->otmpFullName;
  for (int i = 0; i < 4; ++i) if (fields_w[i]) needed += WideCharToMultiByte(cp, 0, (const WCHAR *)((char *)w + (ULONG_PTR)fields_w[i]), -1, NULL, 0, NULL, NULL);
  if (!out) { mem_free(w); return needed; }
  if (needed > size) { mem_free(w); return 0; }
  out->otmSize = needed;
  text_metric_WtoA(&w->otmTextMetrics, &out->otmTextMetrics, GdiGetCodePage(hdc));
  out->otmFiller = 0;
  memcpy(&out->otmPanoseNumber, &w->otmPanoseNumber, (char *)&w->otmpFamilyName - (char *)&w->otmPanoseNumber);
  fields_a[0] = &out->otmpFamilyName; fields_a[1] = &out->otmpFaceName; fields_a[2] = &out->otmpStyleName; fields_a[3] = &out->otmpFullName;
  ptr = (char *)(out + 1);
  UINT left = needed - sizeof(*out);
  for (int i = 0; i < 4; ++i) {
    if (!fields_w[i]) { *fields_a[i] = NULL; continue; }
    *fields_a[i] = (PSTR)(ULONG_PTR)(ptr - (char *)out);
    const int len = WideCharToMultiByte(cp, 0, (const WCHAR *)((char *)w + (ULONG_PTR)fields_w[i]), -1, ptr, left, NULL, NULL);
    left -= len; ptr += len;
  }
  result = needed;
  mem_free(w);
  return result;
}

/* ---- character widths --------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI GetCharWidthW(HDC hdc, UINT first, UINT last, INT *buffer) { return NtGdiGetCharWidthW(RD(hdc), first, last, NULL, NTGDI_GETCHARWIDTH_INT, buffer); }
EXPORT BOOL WINAPI GetCharWidth32W(HDC hdc, UINT first, UINT last, INT *buffer) { return NtGdiGetCharWidthW(RD(hdc), first, last, NULL, NTGDI_GETCHARWIDTH_INT, buffer); }
EXPORT BOOL WINAPI GetCharWidthFloatW(HDC hdc, UINT first, UINT last, FLOAT *buffer) { return NtGdiGetCharWidthW(RD(hdc), first, last, NULL, 0, buffer); }
EXPORT BOOL WINAPI GetCharWidthI(HDC hdc, UINT first, UINT count, WORD *glyphs, INT *buffer) {
  return NtGdiGetCharWidthW(RD(hdc), first, count, (WCHAR *)glyphs, NTGDI_GETCHARWIDTH_INT | NTGDI_GETCHARWIDTH_INDICES, buffer);
}
EXPORT BOOL WINAPI GetCharABCWidthsW(HDC hdc, UINT first, UINT last, ABC *abc) { return NtGdiGetCharABCWidthsW(RD(hdc), first, last, NULL, NTGDI_GETCHARABCWIDTHS_INT, abc); }
EXPORT BOOL WINAPI GetCharABCWidthsFloatW(HDC hdc, UINT first, UINT last, ABCFLOAT *abc) { return NtGdiGetCharABCWidthsW(RD(hdc), first, last, NULL, 0, abc); }
EXPORT BOOL WINAPI GetCharABCWidthsI(HDC hdc, UINT first, UINT count, WORD *glyphs, ABC *abc) {
  return NtGdiGetCharABCWidthsW(RD(hdc), first, count, (WCHAR *)glyphs, NTGDI_GETCHARABCWIDTHS_INT | NTGDI_GETCHARABCWIDTHS_INDICES, abc);
}
EXPORT BOOL WINAPI GetCharWidthInfo(HDC hdc, void *info) { return NtGdiGetCharWidthInfo(RD(hdc), info); }

/* The A variants take a range of character codes (a code above 255 is a double-byte character) and one result per code. */
static WCHAR code_to_wide(HDC hdc, UINT code) {
  char bytes[2];
  WCHAR w = 0;
  int n = 0;
  if (code > 0xff) bytes[n++] = (char)(code >> 8);
  bytes[n++] = (char)code;
  if (!MultiByteToWideChar(GdiGetCodePage(hdc), 0, bytes, n, &w, 1)) w = 0x30fb;
  return w;
}
typedef BOOL (*char_info_fn)(HDC, WCHAR, void *);
static BOOL a_range(HDC hdc, UINT first, UINT last, char *buffer, SIZE_T item, char_info_fn fn) {
  if (first > last) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  for (UINT c = first; c <= last; ++c, buffer += item) if (!fn(hdc, code_to_wide(hdc, c), buffer)) return FALSE;
  return TRUE;
}
static BOOL one_width_int(HDC hdc, WCHAR w, void *out) { return GetCharWidth32W(hdc, w, w, out); }
static BOOL one_width_float(HDC hdc, WCHAR w, void *out) { return GetCharWidthFloatW(hdc, w, w, out); }
static BOOL one_abc_int(HDC hdc, WCHAR w, void *out) { return GetCharABCWidthsW(hdc, w, w, out); }
static BOOL one_abc_float(HDC hdc, WCHAR w, void *out) { return GetCharABCWidthsFloatW(hdc, w, w, out); }
EXPORT BOOL WINAPI GetCharWidthA(HDC hdc, UINT first, UINT last, INT *buffer) { return a_range(hdc, first, last, (char *)buffer, sizeof(INT), one_width_int); }
EXPORT BOOL WINAPI GetCharWidth32A(HDC hdc, UINT first, UINT last, INT *buffer) { return a_range(hdc, first, last, (char *)buffer, sizeof(INT), one_width_int); }
EXPORT BOOL WINAPI GetCharWidthFloatA(HDC hdc, UINT first, UINT last, FLOAT *buffer) { return a_range(hdc, first, last, (char *)buffer, sizeof(FLOAT), one_width_float); }
EXPORT BOOL WINAPI GetCharABCWidthsA(HDC hdc, UINT first, UINT last, ABC *abc) { return a_range(hdc, first, last, (char *)abc, sizeof(ABC), one_abc_int); }
EXPORT BOOL WINAPI GetCharABCWidthsFloatA(HDC hdc, UINT first, UINT last, ABCFLOAT *abc) { return a_range(hdc, first, last, (char *)abc, sizeof(ABCFLOAT), one_abc_float); }

EXPORT LONG WINAPI GdiGetCharDimensions(HDC hdc, TEXTMETRICW *lptm, LONG *height) {
  static const WCHAR alphabet[] = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  SIZE sz;
  TEXTMETRICW tm;
  if (!GetTextMetricsW(hdc, &tm)) return 0;
  if (lptm) *lptm = tm;
  if (height) *height = tm.tmHeight;
  if (!(tm.tmPitchAndFamily & TMPF_FIXED_PITCH)) return tm.tmAveCharWidth;
  if (!GetTextExtentPointW(hdc, alphabet, 52, &sz)) return 0;
  return (sz.cx / 26 + 1) / 2;
}

/* ---- glyphs, kerning, font data ----------------------------------------------------------------------------------------------------------------------------- */

EXPORT DWORD WINAPI GetGlyphIndicesW(HDC hdc, LPCWSTR str, int count, LPWORD indices, DWORD flags) { return NtGdiGetGlyphIndicesW(RD(hdc), str, count, indices, flags); }
EXPORT DWORD WINAPI GetGlyphIndicesA(HDC hdc, LPCSTR str, int count, LPWORD indices, DWORD flags) {
  int wlen = 0;
  WCHAR *w;
  DWORD r;
  if (count < 0 || (!str && count)) return GDI_ERROR;
  w = text_AtoW(hdc, str, count, &wlen, NULL);
  if (!w) return GDI_ERROR;
  r = NtGdiGetGlyphIndicesW(RD(hdc), w, wlen, indices, flags);
  mem_free(w);
  return r;
}
EXPORT DWORD WINAPI GetGlyphOutlineW(HDC hdc, UINT ch, UINT format, LPGLYPHMETRICS gm, DWORD size, LPVOID buffer, const MAT2 *mat) { return NtGdiGetGlyphOutline(RD(hdc), ch, format, gm, size, buffer, mat, FALSE); }
EXPORT DWORD WINAPI GetGlyphOutlineA(HDC hdc, UINT ch, UINT format, LPGLYPHMETRICS gm, DWORD size, LPVOID buffer, const MAT2 *mat) {
  if (!(format & GGO_GLYPH_INDEX)) ch = code_to_wide(hdc, ch > 0xff ? ch & 0xffff : ch & 0xff);
  return NtGdiGetGlyphOutline(RD(hdc), ch, format, gm, size, buffer, mat, FALSE);
}
EXPORT DWORD WINAPI GetKerningPairsW(HDC hdc, DWORD count, KERNINGPAIR *pairs) { return NtGdiGetKerningPairs(RD(hdc), count, pairs); }
EXPORT DWORD WINAPI GetKerningPairsA(HDC hdc, DWORD count, KERNINGPAIR *pairs) {
  const UINT cp = GdiGetCodePage(hdc);
  DWORD n = NtGdiGetKerningPairs(RD(hdc), 0, NULL), out = 0;
  KERNINGPAIR *w;
  if (!count || !pairs) return n;
  w = mem_alloc_n(n ? n : 1, sizeof(*w));
  if (!w) return 0;
  n = NtGdiGetKerningPairs(RD(hdc), n, w);
  for (DWORD i = 0; i < n && out < count; ++i) {
    char a, b;
    BOOL d1 = FALSE, d2 = FALSE;
    WCHAR first = w[i].wFirst, second = w[i].wSecond;
    if (!WideCharToMultiByte(cp, 0, &first, 1, &a, 1, NULL, &d1) || d1) continue;
    if (!WideCharToMultiByte(cp, 0, &second, 1, &b, 1, NULL, &d2) || d2) continue;
    pairs[out].wFirst = (BYTE)a; pairs[out].wSecond = (BYTE)b; pairs[out].iKernAmount = w[i].iKernAmount;
    ++out;
  }
  mem_free(w);
  return out;
}
EXPORT DWORD WINAPI GetFontData(HDC hdc, DWORD table, DWORD offset, LPVOID buffer, DWORD length) { return NtGdiGetFontData(RD(hdc), table, offset, buffer, length); }
EXPORT DWORD WINAPI GetFontUnicodeRanges(HDC hdc, LPGLYPHSET gs) { return NtGdiGetFontUnicodeRanges(RD(hdc), gs); }
EXPORT BOOL WINAPI GetRasterizerCaps(LPRASTERIZER_STATUS status, UINT size) { return NtGdiGetRasterizerCaps(status, size); }
EXPORT BOOL WINAPI GetFontRealizationInfo(HDC hdc, void *info) { return NtGdiGetRealizationInfo(RD(hdc), info); }
EXPORT BOOL WINAPI GdiRealizationInfo(HDC hdc, void *info) { return NtGdiGetRealizationInfo(RD(hdc), info); }
EXPORT BOOL WINAPI GetFontFileInfo(DWORD instance, DWORD file, void *info, SIZE_T size, SIZE_T *needed) { return NtGdiGetFontFileInfo(instance, file, info, size, needed); }
EXPORT BOOL WINAPI GetFontFileData(DWORD instance, DWORD file, UINT64 offset, void *buffer, SIZE_T size) { return NtGdiGetFontFileData(instance, file, &offset, buffer, size); }
EXPORT BOOL WINAPI FontIsLinked(HDC hdc) { return NtGdiFontIsLinked(hdc); }

EXPORT DWORD WINAPI GetFontLanguageInfo(HDC hdc) {
  FONTSIGNATURE fs;
  DWORD result = 0;
  memset(&fs, 0, sizeof(fs));
  if (NtGdiGetTextCharsetInfo(RD(hdc), &fs, 0) == GDI_ERROR) return GCP_ERROR;
  if (fs.fsCsb[0] & 0x003F0000) result |= GCP_DBCS;
  if (fs.fsCsb[0] & (FS_ARABIC | FS_HEBREW)) result |= GCP_REORDER | GCP_GLYPHSHAPE;
  if (fs.fsCsb[0] & FS_ARABIC) result |= FLI_GLYPHS;
  if (NtGdiGetKerningPairs(RD(hdc), 0, NULL)) result |= GCP_USEKERNING;
  return result;
}

/* GetCharacterPlacement for left-to-right text: the extent, the per-character advances (GCP_USEKERNING is not applied) and glyph indices when asked. */
EXPORT DWORD WINAPI GetCharacterPlacementW(HDC hdc, LPCWSTR str, int count, int max_extent, LPGCP_RESULTSW result, DWORD flags) {
  SIZE size;
  int fit = 0;
  if (count < 0 || (!str && count)) return 0;
  if (!result) {
    if (!GetTextExtentPoint32W(hdc, str, count, &size)) return 0;
    return (WORD)size.cx | ((DWORD)size.cy << 16);
  }
  if (flags & GCP_MAXEXTENT) { if (!GetTextExtentExPointW(hdc, str, count, max_extent, &fit, result->lpDx, &size)) return 0; if (fit < count) count = fit; }
  else if (!GetTextExtentExPointW(hdc, str, count, 0, NULL, result->lpDx, &size)) return 0;
  if (result->lpOutString) for (int i = 0; i < count && (UINT)i < result->nGlyphs; ++i) result->lpOutString[i] = str[i];
  if (result->lpClass) for (int i = 0; i < count && (UINT)i < result->nGlyphs; ++i) result->lpClass[i] = GCPCLASS_LATIN;
  if (result->lpGlyphs && (flags & GCP_GLYPHSHAPE)) NtGdiGetGlyphIndicesW(RD(hdc), str, count, result->lpGlyphs, 0);
  if (result->lpOrder) for (int i = 0; i < count && (UINT)i < result->nGlyphs; ++i) result->lpOrder[i] = i;
  if (result->lpCaretPos) { int x = 0; for (int i = 0; i < count && (UINT)i < result->nGlyphs; ++i) { result->lpCaretPos[i] = x; if (result->lpDx) x += result->lpDx[i]; } }
  result->nGlyphs = count;
  result->nMaxFit = count;
  return (WORD)size.cx | ((DWORD)size.cy << 16);
}
EXPORT DWORD WINAPI GetCharacterPlacementA(HDC hdc, LPCSTR str, int count, int max_extent, LPGCP_RESULTSA result, DWORD flags) {
  int wlen = 0;
  WCHAR *w;
  DWORD r;
  GCP_RESULTSW wr;
  if (count < 0 || (!str && count)) return 0;
  w = text_AtoW(hdc, str, count, &wlen, NULL);
  if (!w) return 0;
  if (!result) { r = GetCharacterPlacementW(hdc, w, wlen, max_extent, NULL, flags); mem_free(w); return r; }
  memset(&wr, 0, sizeof(wr));
  wr.lStructSize = sizeof(wr); wr.nGlyphs = result->nGlyphs; wr.lpDx = result->lpDx; wr.lpCaretPos = result->lpCaretPos; wr.lpClass = (LPSTR)result->lpClass;
  wr.lpOrder = result->lpOrder; wr.lpGlyphs = result->lpGlyphs;
  WCHAR *out = result->lpOutString ? mem_alloc_n((SIZE_T)wlen + 1, sizeof(WCHAR)) : NULL;
  wr.lpOutString = out;
  r = GetCharacterPlacementW(hdc, w, wlen, max_extent, &wr, flags);
  if (out) { WideCharToMultiByte(GdiGetCodePage(hdc), 0, out, wr.nGlyphs, result->lpOutString, wr.nGlyphs, NULL, NULL); mem_free(out); }
  result->nGlyphs = wr.nGlyphs; result->nMaxFit = wr.nMaxFit;
  mem_free(w);
  return r;
}

/* ---- enumeration -------------------------------------------------------------------------------------------------------------------------------------------- */

static struct font_enum_entry *enum_fonts_get(HDC hdc, const LOGFONTW *lf, ULONG *count) {
  ULONG size = 0, charset = lf ? lf->lfCharSet : DEFAULT_CHARSET;
  const WCHAR *face = lf && lf->lfFaceName[0] ? lf->lfFaceName : NULL;
  const ULONG face_len = face ? (ULONG)lstrlenW(face) : 0;
  struct font_enum_entry *buf;
  *count = 0;
  if (!NtGdiEnumFonts(RD(hdc), 0, 0, face_len, face, charset, &size, NULL) || !size) return NULL;
  buf = mem_alloc(size);
  if (!buf) return NULL;
  if (!NtGdiEnumFonts(RD(hdc), 0, 0, face_len, face, charset, &size, buf)) { mem_free(buf); return NULL; }
  *count = size / sizeof(*buf);
  return buf;
}
static int same_family(const struct font_enum_entry *list, ULONG upto, const struct font_enum_entry *e) {
  for (ULONG i = 0; i < upto; ++i) if (lstrcmpiW(list[i].lf.elfLogFont.lfFaceName, e->lf.elfLogFont.lfFaceName) == 0) return 1;
  return 0;
}
static int enum_fonts_W(HDC hdc, LOGFONTW *lf, FONTENUMPROCW proc, LPARAM param, int families_only) {
  ULONG count, i;
  int ret = 1;
  struct font_enum_entry *buf = enum_fonts_get(hdc, lf, &count);
  for (i = 0; i < count; ++i) {
    if (families_only && same_family(buf, i, &buf[i])) continue;
    ret = proc(&buf[i].lf.elfLogFont, (const TEXTMETRICW *)&buf[i].tm, buf[i].type, param);
    if (!ret) break;
  }
  mem_free(buf);
  return ret;
}
typedef struct EnumA { FONTENUMPROCA proc; LPARAM param; } EnumA;
static int CALLBACK enum_thunk_A(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM param) {
  const EnumA *e = (const EnumA *)param;
  const ENUMLOGFONTEXW *w = (const ENUMLOGFONTEXW *)lf;
  const NEWTEXTMETRICEXW *tw = (const NEWTEXTMETRICEXW *)tm;
  ENUMLOGFONTEXA a;
  NEWTEXTMETRICEXA ta;
  memset(&a, 0, sizeof(a)); memset(&ta, 0, sizeof(ta));
  logfont_WtoA(&w->elfLogFont, &a.elfLogFont);
  WideCharToMultiByte(CP_ACP, 0, w->elfFullName, -1, (char *)a.elfFullName, LF_FULLFACESIZE, NULL, NULL); a.elfFullName[LF_FULLFACESIZE - 1] = 0;
  WideCharToMultiByte(CP_ACP, 0, w->elfStyle, -1, (char *)a.elfStyle, LF_FACESIZE, NULL, NULL); a.elfStyle[LF_FACESIZE - 1] = 0;
  WideCharToMultiByte(CP_ACP, 0, w->elfScript, -1, (char *)a.elfScript, LF_FACESIZE, NULL, NULL); a.elfScript[LF_FACESIZE - 1] = 0;
  text_metric_WtoA((const TEXTMETRICW *)tw, (TEXTMETRICA *)&ta, CP_ACP);
  ta.ntmTm.ntmFlags = tw->ntmTm.ntmFlags; ta.ntmTm.ntmSizeEM = tw->ntmTm.ntmSizeEM; ta.ntmTm.ntmCellHeight = tw->ntmTm.ntmCellHeight; ta.ntmTm.ntmAvgWidth = tw->ntmTm.ntmAvgWidth;
  ta.ntmFontSig = tw->ntmFontSig;
  return e->proc((const LOGFONTA *)&a, (const TEXTMETRICA *)&ta, type, e->param);
}
EXPORT int WINAPI EnumFontFamiliesExW(HDC hdc, LPLOGFONTW lf, FONTENUMPROCW proc, LPARAM param, DWORD flags) { return enum_fonts_W(hdc, lf, proc, param, 0); }
EXPORT int WINAPI EnumFontFamiliesExA(HDC hdc, LPLOGFONTA lf, FONTENUMPROCA proc, LPARAM param, DWORD flags) {
  LOGFONTW w;
  EnumA e = { proc, param };
  if (lf) logfont_AtoW(lf, &w);
  return enum_fonts_W(hdc, lf ? &w : NULL, enum_thunk_A, (LPARAM)&e, 0);
}
EXPORT int WINAPI EnumFontFamiliesW(HDC hdc, LPCWSTR family, FONTENUMPROCW proc, LPARAM param) {
  LOGFONTW lf;
  memset(&lf, 0, sizeof(lf));
  if (family) { if (!*family) return 1; for (int i = 0; i < LF_FACESIZE - 1 && family[i]; ++i) lf.lfFaceName[i] = family[i]; }
  lf.lfCharSet = DEFAULT_CHARSET;
  return enum_fonts_W(hdc, &lf, proc, param, family == NULL);
}
EXPORT int WINAPI EnumFontFamiliesA(HDC hdc, LPCSTR family, FONTENUMPROCA proc, LPARAM param) {
  WCHAR *w = dup_AtoW(family, CP_ACP);
  EnumA e = { proc, param };
  int r;
  if (family && !*family) { mem_free(w); return 1; }
  LOGFONTW lf;
  memset(&lf, 0, sizeof(lf));
  if (w) for (int i = 0; i < LF_FACESIZE - 1 && w[i]; ++i) lf.lfFaceName[i] = w[i];
  lf.lfCharSet = DEFAULT_CHARSET;
  r = enum_fonts_W(hdc, &lf, enum_thunk_A, (LPARAM)&e, family == NULL);
  mem_free(w);
  return r;
}
EXPORT int WINAPI EnumFontsW(HDC hdc, LPCWSTR face, FONTENUMPROCW proc, LPARAM param) { return EnumFontFamiliesW(hdc, face, proc, param); }
EXPORT int WINAPI EnumFontsA(HDC hdc, LPCSTR face, FONTENUMPROCA proc, LPARAM param) { return EnumFontFamiliesA(hdc, face, proc, param); }
