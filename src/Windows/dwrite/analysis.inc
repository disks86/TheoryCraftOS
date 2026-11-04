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

/* dwrite.dll: glyph run analysis (anti-aliased alpha textures from the kernel's rasteriser), the text analyzer (line breakpoints, glyphs and placements by character map and kern
 * table) and the GDI interop. Included by dwrite.c. */

/* The kernel face number of one of our font face objects; 0 when the object is somebody else's (a program's own IDWriteFontFace is not supported). */
static int face_id_of(IDWriteFontFace *f, UINT32 *id) {
  if (!f || f->lpVtbl != &face_vtbl) return 0;
  *id = ((FontFace *)f)->face;
  return 1;
}

static float f_sqrt(float v) {
  if (!(v > 0)) return 0;
  float x = v > 1 ? v : 1;
  for (int i = 0; i < 20; ++i) x = 0.5f * (x + v / x);
  return x;
}

/* ================================================================ glyph run analysis */
typedef struct GlyphBitmap { INT32 x, y; UINT32 width, height; BYTE *pixels; } GlyphBitmap;
typedef struct RunAnalysis {
  const IDWriteGlyphRunAnalysisVtbl *lpVtbl;
  LONG ref;
  GlyphBitmap *bitmaps;
  UINT32 count;
  RECT bounds;
  DWRITE_RENDERING_MODE mode;
} RunAnalysis;
static void analysis_free(RunAnalysis *a) {
  for (UINT32 i = 0; i < a->count; ++i) mem_free(a->bitmaps[i].pixels);
  mem_free(a->bitmaps);
  mem_free(a);
}
REF_METHODS(analysis, RunAnalysis, IDWriteGlyphRunAnalysis, analysis_free)
static HRESULT WINAPI analysis_QueryInterface(IDWriteGlyphRunAnalysis *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteGlyphRunAnalysis, NULL, NULL); }
static HRESULT WINAPI analysis_GetAlphaTextureBounds(IDWriteGlyphRunAnalysis *This, DWRITE_TEXTURE_TYPE type, RECT *bounds) {
  RunAnalysis *a = (RunAnalysis *)This;
  if ((UINT32)type > DWRITE_TEXTURE_CLEARTYPE_3x1) { memset(bounds, 0, sizeof(*bounds)); return E_INVALIDARG; }
  *bounds = a->bounds;
  return S_OK;
}
static HRESULT WINAPI analysis_CreateAlphaTexture(IDWriteGlyphRunAnalysis *This, DWRITE_TEXTURE_TYPE type, const RECT *bounds, BYTE *out, UINT32 size) {
  RunAnalysis *a = (RunAnalysis *)This;
  if ((UINT32)type > DWRITE_TEXTURE_CLEARTYPE_3x1 || !bounds || !out || bounds->right < bounds->left || bounds->bottom < bounds->top) return E_INVALIDARG;
  const UINT32 w = (UINT32)(bounds->right - bounds->left), h = (UINT32)(bounds->bottom - bounds->top), bpp = type == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 3 : 1;
  if ((UINT64)w * h * bpp > size) return E_INVALIDARG;
  memset(out, 0, (SIZE_T)w * h * bpp);
  for (UINT32 i = 0; i < a->count; ++i) {
    const GlyphBitmap *g = &a->bitmaps[i];
    for (UINT32 row = 0; row < g->height; ++row) {
      const INT32 y = g->y + (INT32)row - bounds->top;
      if (y < 0 || y >= (INT32)h) continue;
      for (UINT32 col = 0; col < g->width; ++col) {
        const INT32 x = g->x + (INT32)col - bounds->left;
        if (x < 0 || x >= (INT32)w) continue;
        BYTE v = g->pixels[row * g->width + col];
        if (type == DWRITE_TEXTURE_ALIASED_1x1) v = v >= 128 ? 255 : 0;
        BYTE *d = out + ((SIZE_T)y * w + x) * bpp;
        for (UINT32 c = 0; c < bpp; ++c) if (v > d[c]) d[c] = v;
      }
    }
  }
  return S_OK;
}
static HRESULT WINAPI analysis_GetAlphaBlendParams(IDWriteGlyphRunAnalysis *This, IDWriteRenderingParams *params, FLOAT *gamma, FLOAT *contrast, FLOAT *cleartype) {
  (void)This;
  *gamma = params ? params->lpVtbl->GetGamma(params) : 1.8f;
  *contrast = params ? params->lpVtbl->GetEnhancedContrast(params) : 0.5f;
  *cleartype = params ? params->lpVtbl->GetClearTypeLevel(params) : 1.0f;
  return S_OK;
}
static const IDWriteGlyphRunAnalysisVtbl analysis_vtbl = {
  analysis_QueryInterface, analysis_AddRef, analysis_Release, analysis_GetAlphaTextureBounds, analysis_CreateAlphaTexture, analysis_GetAlphaBlendParams,
};

static HRESULT analysis_create(const DWRITE_GLYPH_RUN *run, float ppd, const DWRITE_MATRIX *m, DWRITE_RENDERING_MODE mode, float bx, float by, IDWriteGlyphRunAnalysis **out) {
  UINT32 face;
  *out = NULL;
  if (!run || !face_id_of(run->fontFace, &face) || !(run->fontEmSize > 0) || !(ppd > 0) || (run->glyphCount && !run->glyphIndices) || mode == DWRITE_RENDERING_MODE_OUTLINE ||
      (UINT32)mode > DWRITE_RENDERING_MODE_OUTLINE)
    return E_INVALIDARG;
  DWRITE_MATRIX id = {1, 0, 0, 1, 0, 0};
  if (!m) m = &id;
  const float sx = f_sqrt(m->m11 * m->m11 + m->m12 * m->m12), sy = f_sqrt(m->m21 * m->m21 + m->m22 * m->m22), scale = (sx + sy) / 2;
  const float size_px = run->fontEmSize * ppd * (scale > 0 ? scale : 1.0f);
  DWRITE_FONT_METRICS fm;
  if (!face_info(face, NULL, &fm)) return E_FAIL;
  RunAnalysis *a = OBJECT_NEW(RunAnalysis);
  if (!a) return E_OUTOFMEMORY;
  a->lpVtbl = &analysis_vtbl;
  a->ref = 1;
  a->mode = mode;
  a->bitmaps = mem_array(run->glyphCount ? run->glyphCount : 1, sizeof(GlyphBitmap));
  if (!a->bitmaps) { mem_free(a); return E_OUTOFMEMORY; }
  TcosGlyphBox *boxes = mem_array(run->glyphCount ? run->glyphCount : 1, sizeof(TcosGlyphBox));
  if (!boxes) { analysis_free(a); return E_OUTOFMEMORY; }
  face_boxes(face, run->glyphIndices, run->glyphCount, boxes);
  /* the baseline origin in pixels */
  float pen_x = bx * ppd * m->m11 + by * ppd * m->m21 + m->dx, pen_y = bx * ppd * m->m12 + by * ppd * m->m22 + m->dy;
  const float dir = run->bidiLevel & 1 ? -1.0f : 1.0f;
  const INT32 size26 = (INT32)(size_px * 64.0f + 0.5f);
  int any = 0;
  for (UINT32 i = 0; i < run->glyphCount; ++i) {
    const float adv_px = (run->glyphAdvances ? run->glyphAdvances[i] * ppd * (scale > 0 ? scale : 1.0f) : (float)boxes[i].advance * size_px / (float)fm.designUnitsPerEm);
    if (dir < 0) pen_x -= adv_px;
    float gx = pen_x + (run->glyphOffsets ? run->glyphOffsets[i].advanceOffset * ppd * scale : 0);
    float gy = pen_y - (run->glyphOffsets ? run->glyphOffsets[i].ascenderOffset * ppd * scale : 0);
    if (dir > 0) pen_x += adv_px;
    const float ix = f_floor(gx);
    const UINT32 sub = (UINT32)f_floor((gx - ix) * 4.0f) & 3;
    const INT32 iy = (INT32)f_round(gy);
    TcosFontRequest r;
    if (size26 <= 0 || font_call(OP_RASTERIZE, face, run->glyphIndices[i], sub, (ULONG)size26, NULL, NULL, 0, &r) != 0 || r.r2 == 0 || r.r3 == 0) continue;
    GlyphBitmap *g = &a->bitmaps[a->count];
    g->pixels = mem_alloc((SIZE_T)r.r2 * r.r3);
    if (!g->pixels || font_call(OP_RASTERIZE, face, run->glyphIndices[i], sub, (ULONG)size26, NULL, g->pixels, r.r2 * r.r3, NULL) != 0) { mem_free(g->pixels); g->pixels = NULL; continue; }
    g->x = (INT32)ix + (INT32)r.r0;
    g->y = iy - (INT32)r.r1;
    g->width = r.r2;
    g->height = r.r3;
    ++a->count;
    if (!any) { a->bounds.left = g->x; a->bounds.top = g->y; a->bounds.right = g->x + (INT32)g->width; a->bounds.bottom = g->y + (INT32)g->height; any = 1; }
    else {
      if (g->x < a->bounds.left) a->bounds.left = g->x;
      if (g->y < a->bounds.top) a->bounds.top = g->y;
      if (g->x + (INT32)g->width > a->bounds.right) a->bounds.right = g->x + (INT32)g->width;
      if (g->y + (INT32)g->height > a->bounds.bottom) a->bounds.bottom = g->y + (INT32)g->height;
    }
  }
  mem_free(boxes);
  *out = (IDWriteGlyphRunAnalysis *)a;
  return S_OK;
}

/* ================================================================ text analyzer */
typedef struct Analyzer { const IDWriteTextAnalyzerVtbl *lpVtbl; LONG ref; } Analyzer;
static void analyzer_free(Analyzer *a) { mem_free(a); }
REF_METHODS(analyzer, Analyzer, IDWriteTextAnalyzer, analyzer_free)
static HRESULT WINAPI analyzer_QueryInterface(IDWriteTextAnalyzer *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteTextAnalyzer, NULL, NULL); }

static HRESULT WINAPI analyzer_AnalyzeScript(IDWriteTextAnalyzer *This, IDWriteTextAnalysisSource *src, UINT32 pos, UINT32 length, IDWriteTextAnalysisSink *sink) {
  (void)This; (void)src;
  DWRITE_SCRIPT_ANALYSIS a;
  a.script = 0;
  a.shapes = DWRITE_SCRIPT_SHAPES_DEFAULT;
  if (!sink) return E_INVALIDARG;
  return length ? sink->lpVtbl->SetScriptAnalysis(sink, pos, length, &a) : S_OK;
}
static HRESULT WINAPI analyzer_AnalyzeBidi(IDWriteTextAnalyzer *This, IDWriteTextAnalysisSource *src, UINT32 pos, UINT32 length, IDWriteTextAnalysisSink *sink) {
  (void)This;
  if (!sink || !src) return E_INVALIDARG;
  const BYTE level = src->lpVtbl->GetParagraphReadingDirection(src) == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT ? 1 : 0;
  return length ? sink->lpVtbl->SetBidiLevel(sink, pos, length, level, level) : S_OK;
}
static HRESULT WINAPI analyzer_AnalyzeNumberSubstitution(IDWriteTextAnalyzer *This, IDWriteTextAnalysisSource *src, UINT32 pos, UINT32 length, IDWriteTextAnalysisSink *sink) {
  (void)This; (void)src; (void)pos; (void)length;
  return sink ? S_OK : E_INVALIDARG;
}
/* The text [pos, pos + length) of an analysis source, in one new buffer. */
static HRESULT source_text(IDWriteTextAnalysisSource *src, UINT32 pos, UINT32 length, WCHAR **out) {
  WCHAR *text = mem_array((SIZE_T)length + 1, sizeof(WCHAR));
  *out = NULL;
  if (!text) return E_OUTOFMEMORY;
  for (UINT32 done = 0; done < length;) {
    const WCHAR *chunk = NULL;
    UINT32 n = 0;
    HRESULT hr = src->lpVtbl->GetTextAtPosition(src, pos + done, &chunk, &n);
    if (FAILED(hr) || !chunk || n == 0) { mem_free(text); return FAILED(hr) ? hr : E_FAIL; }
    if (n > length - done) n = length - done;
    memcpy(text + done, chunk, n * sizeof(WCHAR));
    done += n;
  }
  *out = text;
  return S_OK;
}
static HRESULT WINAPI analyzer_AnalyzeLineBreakpoints(IDWriteTextAnalyzer *This, IDWriteTextAnalysisSource *src, UINT32 pos, UINT32 length, IDWriteTextAnalysisSink *sink) {
  (void)This;
  if (!src || !sink) return E_INVALIDARG;
  if (length == 0) return S_OK;
  WCHAR *text;
  HRESULT hr = source_text(src, pos, length, &text);
  if (FAILED(hr)) return hr;
  DWRITE_LINE_BREAKPOINT *bp = mem_array(length, sizeof(*bp));
  if (!bp) { mem_free(text); return E_OUTOFMEMORY; }
  for (UINT32 i = 0; i < length;) {
    UINT32 units, next_units;
    const UINT32 c = decode16(text, length, i, &units);
    const UINT32 next = i + units < length ? decode16(text, length, i + units, &next_units) : 0;
    const UINT32 prev = i > 0 ? text[i - 1] : 0;
    for (UINT32 u = 0; u < units; ++u) {
      bp[i + u].breakConditionBefore = DWRITE_BREAK_CONDITION_NEUTRAL;
      bp[i + u].breakConditionAfter = DWRITE_BREAK_CONDITION_NEUTRAL;
      bp[i + u].isWhitespace = cp_break_space(c) || cp_nobreak_space(c) || c == '\t' || cp_newline(c);
      bp[i + u].isSoftHyphen = c == 0xAD;
    }
    /* a break may happen after the last unit of a character */
    bp[i + units - 1].breakConditionAfter = (cp_newline(c) || cp_break_after(c, next, prev)) ? DWRITE_BREAK_CONDITION_CAN_BREAK : DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
    if (i + units < length) bp[i + units].breakConditionBefore = bp[i + units - 1].breakConditionAfter;
    i += units;
  }
  hr = sink->lpVtbl->SetLineBreakpoints(sink, pos, length, bp);
  mem_free(bp);
  mem_free(text);
  return hr;
}
static HRESULT WINAPI analyzer_GetGlyphs(IDWriteTextAnalyzer *This, const WCHAR *text, UINT32 length, IDWriteFontFace *font_face, BOOL sideways, BOOL rtl,
                                         const DWRITE_SCRIPT_ANALYSIS *analysis, const WCHAR *locale, IDWriteNumberSubstitution *substitution,
                                         const DWRITE_TYPOGRAPHIC_FEATURES **features, const UINT32 *feature_range_lengths, UINT32 feature_ranges, UINT32 max_glyphs, UINT16 *cluster_map,
                                         DWRITE_SHAPING_TEXT_PROPERTIES *text_props, UINT16 *glyph_indices, DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_props, UINT32 *actual) {
  (void)This; (void)sideways; (void)rtl; (void)analysis; (void)locale; (void)substitution; (void)features; (void)feature_range_lengths; (void)feature_ranges;
  UINT32 face;
  if (actual) *actual = 0;
  if (!text || length == 0 || !analysis || !cluster_map || !text_props || !glyph_indices || !glyph_props || !actual || !face_id_of(font_face, &face)) return E_INVALIDARG;
  UINT32 *cps = mem_array(length, sizeof(UINT32));
  UINT16 *glyphs = mem_array(length, sizeof(UINT16));
  UINT32 n = 0;
  if (!cps || !glyphs) { mem_free(cps); mem_free(glyphs); return E_OUTOFMEMORY; }
  for (UINT32 i = 0; i < length;) {
    UINT32 units;
    cps[n] = decode16(text, length, i, &units);
    for (UINT32 u = 0; u < units; ++u) cluster_map[i + u] = (UINT16)n;
    i += units;
    ++n;
  }
  if (n > max_glyphs) { mem_free(cps); mem_free(glyphs); return E_NOT_SUFFICIENT_BUFFER; }
  face_map(face, cps, n, glyphs);
  for (UINT32 i = 0; i < n; ++i) {
    glyph_indices[i] = glyphs[i];
    memset(&glyph_props[i], 0, sizeof(glyph_props[i]));
    glyph_props[i].isClusterStart = 1;
    glyph_props[i].isZeroWidthSpace = cps[i] == 0x200B;
    glyph_props[i].justification = (cp_break_space(cps[i]) || cps[i] == '\t') ? 4 /* SCRIPT_JUSTIFY_BLANK */ : (cp_newline(cps[i]) || cp_zero_width(cps[i])) ? 0 : 2 /* SCRIPT_JUSTIFY_CHARACTER */;
  }
  for (UINT32 i = 0; i < length; ++i) { text_props[i].isShapedAlone = 0; text_props[i].reserved = 0; }
  *actual = n;
  mem_free(cps);
  mem_free(glyphs);
  return S_OK;
}
/* The advances of glyphs (kerned) in DIPs; `ppd` > 0 rounds each to whole pixels. */
static HRESULT placements(IDWriteFontFace *font_face, const UINT16 *glyphs, UINT32 count, float size, float ppd, FLOAT *advances, DWRITE_GLYPH_OFFSET *offsets) {
  UINT32 face;
  DWRITE_FONT_METRICS fm;
  if (count == 0) return S_OK;
  if (!advances || !offsets || !glyphs || !(size >= 0) || !face_id_of(font_face, &face) || !face_info(face, NULL, &fm)) return E_INVALIDARG;
  TcosGlyphBox *boxes = mem_array(count ? count : 1, sizeof(TcosGlyphBox));
  if (!boxes) return E_OUTOFMEMORY;
  face_boxes(face, glyphs, count, boxes);
  const float scale = size / (float)fm.designUnitsPerEm;
  for (UINT32 i = 0; i < count; ++i) {
    float adv = (float)boxes[i].advance * scale;
    if (i + 1 < count && glyphs[i] && glyphs[i + 1]) adv += (float)face_kerning(face, glyphs[i], glyphs[i + 1]) * scale;
    if (ppd > 0) adv = f_round(adv * ppd) / ppd;
    advances[i] = adv;
    offsets[i].advanceOffset = 0;
    offsets[i].ascenderOffset = 0;
  }
  mem_free(boxes);
  return S_OK;
}
static HRESULT WINAPI analyzer_GetGlyphPlacements(IDWriteTextAnalyzer *This, const WCHAR *text, const UINT16 *cluster_map, DWRITE_SHAPING_TEXT_PROPERTIES *props, UINT32 text_length,
                                                  const UINT16 *glyphs, const DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_props, UINT32 glyph_count, IDWriteFontFace *face, FLOAT size,
                                                  BOOL sideways, BOOL rtl, const DWRITE_SCRIPT_ANALYSIS *analysis, const WCHAR *locale, const DWRITE_TYPOGRAPHIC_FEATURES **features,
                                                  const UINT32 *feature_lengths, UINT32 feature_ranges, FLOAT *advances, DWRITE_GLYPH_OFFSET *offsets) {
  (void)This; (void)text; (void)cluster_map; (void)props; (void)text_length; (void)glyph_props; (void)sideways; (void)rtl; (void)analysis; (void)locale; (void)features;
  (void)feature_lengths; (void)feature_ranges;
  return placements(face, glyphs, glyph_count, size, 0, advances, offsets);
}
static HRESULT WINAPI analyzer_GetGdiCompatibleGlyphPlacements(IDWriteTextAnalyzer *This, const WCHAR *text, const UINT16 *cluster_map, DWRITE_SHAPING_TEXT_PROPERTIES *props,
                                                               UINT32 text_length, const UINT16 *glyphs, const DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_props, UINT32 glyph_count,
                                                               IDWriteFontFace *face, FLOAT size, FLOAT ppd, const DWRITE_MATRIX *transform, BOOL natural, BOOL sideways, BOOL rtl,
                                                               const DWRITE_SCRIPT_ANALYSIS *analysis, const WCHAR *locale, const DWRITE_TYPOGRAPHIC_FEATURES **features,
                                                               const UINT32 *feature_lengths, UINT32 feature_ranges, FLOAT *advances, DWRITE_GLYPH_OFFSET *offsets) {
  (void)This; (void)text; (void)cluster_map; (void)props; (void)text_length; (void)glyph_props; (void)transform; (void)natural; (void)sideways; (void)rtl; (void)analysis;
  (void)locale; (void)features; (void)feature_lengths; (void)feature_ranges;
  if (!(ppd > 0)) return E_INVALIDARG;
  return placements(face, glyphs, glyph_count, size, ppd, advances, offsets);
}
static const IDWriteTextAnalyzerVtbl analyzer_vtbl = {
  analyzer_QueryInterface, analyzer_AddRef, analyzer_Release, analyzer_AnalyzeScript, analyzer_AnalyzeBidi, analyzer_AnalyzeNumberSubstitution, analyzer_AnalyzeLineBreakpoints,
  analyzer_GetGlyphs, analyzer_GetGlyphPlacements, analyzer_GetGdiCompatibleGlyphPlacements,
};
static HRESULT analyzer_create(IDWriteTextAnalyzer **out) {
  Analyzer *a = OBJECT_NEW(Analyzer);
  if (!a) return E_OUTOFMEMORY;
  a->lpVtbl = &analyzer_vtbl;
  a->ref = 1;
  *out = (IDWriteTextAnalyzer *)a;
  return S_OK;
}


/* ================================================================ bitmap render target (GDI memory DC and DIB section; gdi32.dll is looked up when first needed) */
typedef struct GdiApi {
  HDC (WINAPI *CreateCompatibleDC)(HDC);
  HBITMAP (WINAPI *CreateDIBSection)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
  HGDIOBJ (WINAPI *SelectObject)(HDC, HGDIOBJ);
  BOOL (WINAPI *DeleteObject)(HGDIOBJ);
  BOOL (WINAPI *DeleteDC)(HDC);
  HGDIOBJ (WINAPI *GetCurrentObject)(HDC, UINT);
  int (WINAPI *GetObjectW)(HANDLE, int, LPVOID);
  int (WINAPI *SetGraphicsMode)(HDC, int);
} GdiApi;
static GdiApi g_gdi_api;
static int gdi_api(void) {
  if (g_gdi_api.GetObjectW) return 1;
  HMODULE m = LoadLibraryW(L"gdi32.dll");
  if (!m) return 0;
  GdiApi a;
  a.CreateCompatibleDC = (void *)GetProcAddress(m, "CreateCompatibleDC");
  a.CreateDIBSection = (void *)GetProcAddress(m, "CreateDIBSection");
  a.SelectObject = (void *)GetProcAddress(m, "SelectObject");
  a.DeleteObject = (void *)GetProcAddress(m, "DeleteObject");
  a.DeleteDC = (void *)GetProcAddress(m, "DeleteDC");
  a.GetCurrentObject = (void *)GetProcAddress(m, "GetCurrentObject");
  a.GetObjectW = (void *)GetProcAddress(m, "GetObjectW");
  a.SetGraphicsMode = (void *)GetProcAddress(m, "SetGraphicsMode");
  if (!a.CreateCompatibleDC || !a.CreateDIBSection || !a.SelectObject || !a.DeleteObject || !a.DeleteDC || !a.GetCurrentObject || !a.GetObjectW) return 0;
  g_gdi_api = a;
  return 1;
}

typedef struct RenderTarget {
  const IDWriteBitmapRenderTargetVtbl *lpVtbl;
  LONG ref;
  HDC dc;
  HBITMAP bitmap, old;
  BYTE *bits;
  UINT32 width, height, stride;      /* the size the program asked for; the bitmap is at least 1 x 1 */
  float ppd;
  DWRITE_MATRIX transform;
} RenderTarget;
static void target_release_bitmap(RenderTarget *t) {
  if (t->dc && t->old) g_gdi_api.SelectObject(t->dc, t->old);
  if (t->bitmap) g_gdi_api.DeleteObject(t->bitmap);
  t->bitmap = NULL;
  t->old = NULL;
  t->bits = NULL;
}
static void target_free(RenderTarget *t) {
  target_release_bitmap(t);
  if (t->dc) g_gdi_api.DeleteDC(t->dc);
  mem_free(t);
}
REF_METHODS(target, RenderTarget, IDWriteBitmapRenderTarget, target_free)
static HRESULT WINAPI target_QueryInterface(IDWriteBitmapRenderTarget *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteBitmapRenderTarget, NULL, NULL); }
static HRESULT target_make_bitmap(RenderTarget *t, UINT32 w, UINT32 h) {
  if (w > 32767 || h > 32767) return E_INVALIDARG;
  BITMAPINFO bi;
  void *bits = NULL;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w ? (LONG)w : 1;
  bi.bmiHeader.biHeight = -(h ? (LONG)h : 1);      /* top down */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  HBITMAP bitmap = g_gdi_api.CreateDIBSection(t->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (!bitmap || !bits) { if (bitmap) g_gdi_api.DeleteObject(bitmap); return E_OUTOFMEMORY; }
  target_release_bitmap(t);
  t->bitmap = bitmap;
  t->old = g_gdi_api.SelectObject(t->dc, bitmap);
  t->bits = bits;
  t->width = w;
  t->height = h;
  t->stride = (w ? w : 1) * 4;
  return S_OK;
}
static HRESULT WINAPI target_DrawGlyphRun(IDWriteBitmapRenderTarget *This, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN *run, IDWriteRenderingParams *params,
                                          COLORREF color, RECT *box) {
  RenderTarget *t = (RenderTarget *)This;
  if (box) memset(box, 0, sizeof(*box));
  if (!params || (UINT32)mode > DWRITE_MEASURING_MODE_GDI_NATURAL || !run) return E_INVALIDARG;
  IDWriteGlyphRunAnalysis *ga = NULL;
  /* (the target's transform and DPI scale the run exactly as they scale a glyph run analysis) */
  HRESULT hr = analysis_create(run, t->ppd, &t->transform, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL, x, y, &ga);
  if (FAILED(hr)) return hr;
  RunAnalysis *a = (RunAnalysis *)ga;
  const BYTE cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
  RECT drawn = {0, 0, 0, 0};
  for (UINT32 i = 0; i < a->count; ++i) {
    const GlyphBitmap *g = &a->bitmaps[i];
    for (UINT32 row = 0; row < g->height; ++row) {
      const INT32 py = g->y + (INT32)row;
      if (py < 0 || py >= (INT32)t->height) continue;
      for (UINT32 col = 0; col < g->width; ++col) {
        const INT32 px = g->x + (INT32)col;
        const UINT32 cov = g->pixels[row * g->width + col];
        if (px < 0 || px >= (INT32)t->width || cov == 0) continue;
        BYTE *d = t->bits + (SIZE_T)py * t->stride + (SIZE_T)px * 4;
        d[0] = (BYTE)(d[0] + ((int)cb - d[0]) * (int)cov / 255);
        d[1] = (BYTE)(d[1] + ((int)cg - d[1]) * (int)cov / 255);
        d[2] = (BYTE)(d[2] + ((int)cr - d[2]) * (int)cov / 255);
        if (drawn.right == drawn.left) { drawn.left = px; drawn.right = px + 1; drawn.top = py; drawn.bottom = py + 1; }
        else {
          if (px < drawn.left) drawn.left = px;
          if (px + 1 > drawn.right) drawn.right = px + 1;
          if (py < drawn.top) drawn.top = py;
          if (py + 1 > drawn.bottom) drawn.bottom = py + 1;
        }
      }
    }
  }
  (void)drawn;
  if (box) *box = a->bounds;      /* (the bounds of the run, also where it falls outside the bitmap) */
  release(ga);
  return S_OK;
}
static HDC WINAPI target_GetMemoryDC(IDWriteBitmapRenderTarget *This) { return ((RenderTarget *)This)->dc; }
static FLOAT WINAPI target_GetPixelsPerDip(IDWriteBitmapRenderTarget *This) { return ((RenderTarget *)This)->ppd; }
static HRESULT WINAPI target_SetPixelsPerDip(IDWriteBitmapRenderTarget *This, FLOAT ppd) {
  if (!(ppd > 0)) return E_INVALIDARG;
  ((RenderTarget *)This)->ppd = ppd;
  return S_OK;
}
static HRESULT WINAPI target_GetCurrentTransform(IDWriteBitmapRenderTarget *This, DWRITE_MATRIX *m) { *m = ((RenderTarget *)This)->transform; return S_OK; }
static HRESULT WINAPI target_SetCurrentTransform(IDWriteBitmapRenderTarget *This, const DWRITE_MATRIX *m) {
  RenderTarget *t = (RenderTarget *)This;
  if (m) t->transform = *m;
  else { const DWRITE_MATRIX id = {1, 0, 0, 1, 0, 0}; t->transform = id; }
  return S_OK;
}
static HRESULT WINAPI target_GetSize(IDWriteBitmapRenderTarget *This, SIZE *size) {
  RenderTarget *t = (RenderTarget *)This;
  size->cx = (LONG)t->width;
  size->cy = (LONG)t->height;
  return S_OK;
}
static HRESULT WINAPI target_Resize(IDWriteBitmapRenderTarget *This, UINT32 w, UINT32 h) {
  RenderTarget *t = (RenderTarget *)This;
  if (w == t->width && h == t->height) return S_OK;
  return target_make_bitmap(t, w, h);
}
static const IDWriteBitmapRenderTargetVtbl target_vtbl = {
  target_QueryInterface, target_AddRef, target_Release, target_DrawGlyphRun, target_GetMemoryDC, target_GetPixelsPerDip, target_SetPixelsPerDip, target_GetCurrentTransform,
  target_SetCurrentTransform, target_GetSize, target_Resize,
};
static HRESULT target_create(HDC hdc, UINT32 w, UINT32 h, IDWriteBitmapRenderTarget **out) {
  *out = NULL;
  if (!gdi_api()) return E_FAIL;
  RenderTarget *t = OBJECT_NEW(RenderTarget);
  if (!t) return E_OUTOFMEMORY;
  t->lpVtbl = &target_vtbl;
  t->ref = 1;
  t->ppd = 1.0f;
  t->transform.m11 = t->transform.m22 = 1.0f;
  t->dc = g_gdi_api.CreateCompatibleDC(hdc);
  if (t->dc && g_gdi_api.SetGraphicsMode) g_gdi_api.SetGraphicsMode(t->dc, GM_ADVANCED);
  HRESULT hr = t->dc ? target_make_bitmap(t, w, h) : E_OUTOFMEMORY;
  if (FAILED(hr)) { target_free(t); return hr; }
  *out = (IDWriteBitmapRenderTarget *)t;
  return S_OK;
}

/* ================================================================ GDI interop */
typedef struct GdiInterop { const IDWriteGdiInteropVtbl *lpVtbl; LONG ref; } GdiInterop;
static void gdi_free(GdiInterop *g) { mem_free(g); }
REF_METHODS(gdi, GdiInterop, IDWriteGdiInterop, gdi_free)
static HRESULT WINAPI gdi_QueryInterface(IDWriteGdiInterop *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteGdiInterop, NULL, NULL); }
static HRESULT WINAPI gdi_CreateFontFromLOGFONT(IDWriteGdiInterop *This, const LOGFONTW *lf, IDWriteFont **font) {
  (void)This;
  *font = NULL;
  if (!lf) return E_INVALIDARG;
  Collection *sys = system_collection();
  Collection *found = NULL;
  UINT32 index = 0, order[16];
  WCHAR name[LF_FACESIZE + 1];
  memcpy(name, lf->lfFaceName, LF_FACESIZE * sizeof(WCHAR));
  name[LF_FACESIZE] = 0;
  if (!sys) return E_FAIL;
  if (!name[0]) memcpy(name, L"Arial", 6 * sizeof(WCHAR));
  if (!find_family(sys, name, &found, &index) || found != sys) return DWRITE_E_NOFONT_;
  const UINT32 weight = lf->lfWeight <= 0 ? 400 : lf->lfWeight > 999 ? 999 : (UINT32)lf->lfWeight;
  match_order(&sys->families[index], weight, DWRITE_FONT_STRETCH_NORMAL, lf->lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, order);
  return font_create(sys, index, sys->families[index].faces[order[0]], font);
}
static void logfont_fill(LOGFONTW *lf, const WCHAR *family, UINT32 face) {
  TcosFaceInfo info;
  memset(lf, 0, sizeof(*lf));
  if (face_info(face, &info, NULL)) { lf->lfWeight = info.weight; lf->lfItalic = (info.flags & 1) ? 1 : 0; }
  lf->lfCharSet = DEFAULT_CHARSET;
  lf->lfOutPrecision = OUT_TT_PRECIS;
  lf->lfClipPrecision = CLIP_DEFAULT_PRECIS;
  lf->lfQuality = DEFAULT_QUALITY;
  for (UINT32 i = 0; family[i] && i < LF_FACESIZE - 1; ++i) lf->lfFaceName[i] = family[i];
}
static HRESULT WINAPI gdi_ConvertFontToLOGFONT(IDWriteGdiInterop *This, IDWriteFont *font, LOGFONTW *lf, BOOL *is_system) {
  (void)This;
  if (!font || !lf || font->lpVtbl != &font_vtbl) return E_INVALIDARG;
  Font *f = (Font *)font;
  logfont_fill(lf, f->collection->families[f->family].name, f->face);
  if (is_system) *is_system = f->collection->is_system;
  return S_OK;
}
static HRESULT WINAPI gdi_ConvertFontFaceToLOGFONT(IDWriteGdiInterop *This, IDWriteFontFace *font_face, LOGFONTW *lf) {
  (void)This;
  UINT32 face;
  if (!lf || !face_id_of(font_face, &face)) return E_INVALIDARG;
  Collection *sys = system_collection();
  const WCHAR *family = NULL;
  WCHAR *own = NULL;
  for (UINT32 i = 0; sys && i < sys->count && !family; ++i)
    for (UINT32 j = 0; j < sys->families[i].count; ++j)
      if (sys->families[i].faces[j] == face) { family = sys->families[i].name; break; }
  if (!family) { own = face_name(face, 0); family = own; }
  if (!family) return E_FAIL;
  logfont_fill(lf, family, face);
  mem_free(own);
  return S_OK;
}
static HRESULT WINAPI gdi_CreateFontFaceFromHdc(IDWriteGdiInterop *This, HDC hdc, IDWriteFontFace **face) {
  *face = NULL;
  if (!hdc) return E_INVALIDARG;
  if (!gdi_api()) return E_FAIL;
  LOGFONTW lf;
  HGDIOBJ font = g_gdi_api.GetCurrentObject(hdc, OBJ_FONT);
  if (!font || g_gdi_api.GetObjectW(font, sizeof(lf), &lf) != sizeof(lf)) return E_FAIL;
  IDWriteFont *f = NULL;
  HRESULT hr = gdi_CreateFontFromLOGFONT(This, &lf, &f);
  if (FAILED(hr)) return hr;
  hr = f->lpVtbl->CreateFontFace(f, face);
  release(f);
  return hr;
}
static HRESULT WINAPI gdi_CreateBitmapRenderTarget(IDWriteGdiInterop *This, HDC hdc, UINT32 width, UINT32 height, IDWriteBitmapRenderTarget **target) {
  (void)This;
  return target_create(hdc, width, height, target);
}
static const IDWriteGdiInteropVtbl gdi_vtbl = {
  gdi_QueryInterface, gdi_AddRef, gdi_Release, gdi_CreateFontFromLOGFONT, gdi_ConvertFontToLOGFONT, gdi_ConvertFontFaceToLOGFONT, gdi_CreateFontFaceFromHdc,
  gdi_CreateBitmapRenderTarget,
};
