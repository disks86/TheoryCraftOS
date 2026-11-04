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

/* Test of dwrite.dll on TheoryCraftOS (src/Windows/dwrite): the factory, the system fonts, faces and tables, font files, text formats and layouts (metrics, wrapping,
 * hit testing, drawing through a text renderer, trimming), the text analyzer, glyph run analysis, outlines, GDI interop and the bitmap render target. It prints Wine-style
 * "Test failed" lines so that scripts/winetest.sh counts them: build it as build/winetests/dwrite_own.exe
 * (x86_64-w64-mingw32-gcc -O1 -o build/winetests/dwrite_own.exe tests/windows/dwritetest.c -ldwrite) and run `scripts/winetest.sh dwrite own`. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
#define check(cond, msg) do { ++g_checks; if (!(cond)) { ++g_failed; printf("dwritetest.c:%d: Test failed: %s\n", __LINE__, msg); } } while (0)
#define check_hr(hr, expected, msg) do { ++g_checks; const HRESULT h_ = (hr); if (h_ != (expected)) { ++g_failed; printf("dwritetest.c:%d: Test failed: %s (hr %08lx, expected %08lx)\n", __LINE__, msg, (unsigned long)h_, (unsigned long)(expected)); } } while (0)

static IDWriteFactory *g_factory;
static IDWriteFontCollection *g_system;
static IDWriteFontFace *g_face;

/* ---- a text renderer that counts */
typedef struct Renderer { IDWriteTextRenderer iface; int runs, glyphs, underlines, strikes, inlines; float last_x, last_y; } Renderer;
static HRESULT WINAPI r_qi(IDWriteTextRenderer *This, REFIID riid, void **out) { (void)riid; *out = This; return S_OK; }
static ULONG WINAPI r_ref(IDWriteTextRenderer *This) { (void)This; return 2; }
static HRESULT WINAPI r_snap(IDWriteTextRenderer *This, void *c, BOOL *d) { (void)This; (void)c; *d = FALSE; return S_OK; }
static HRESULT WINAPI r_xform(IDWriteTextRenderer *This, void *c, DWRITE_MATRIX *m) { (void)This; (void)c; m->m11 = 1; m->m12 = 0; m->m21 = 0; m->m22 = 1; m->dx = 0; m->dy = 0; return S_OK; }
static HRESULT WINAPI r_ppd(IDWriteTextRenderer *This, void *c, FLOAT *p) { (void)This; (void)c; *p = 1.0f; return S_OK; }
static HRESULT WINAPI r_glyphs(IDWriteTextRenderer *This, void *c, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN *run, const DWRITE_GLYPH_RUN_DESCRIPTION *d, IUnknown *e) {
  Renderer *r = (Renderer *)This;
  (void)c; (void)mode; (void)e;
  ++r->runs;
  r->glyphs += run->glyphCount;
  r->last_x = x;
  r->last_y = y;
  check(run->fontFace != NULL && run->fontEmSize > 0 && (!d || d->stringLength >= run->glyphCount), "a glyph run is complete");
  return S_OK;
}
static HRESULT WINAPI r_under(IDWriteTextRenderer *This, void *c, FLOAT x, FLOAT y, const DWRITE_UNDERLINE *u, IUnknown *e) {
  (void)c; (void)x; (void)y; (void)e;
  ++((Renderer *)This)->underlines;
  check(u->width > 0 && u->thickness > 0, "an underline has a size");
  return S_OK;
}
static HRESULT WINAPI r_strike(IDWriteTextRenderer *This, void *c, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH *s, IUnknown *e) { (void)c; (void)x; (void)y; (void)s; (void)e; ++((Renderer *)This)->strikes; return S_OK; }
static HRESULT WINAPI r_inline(IDWriteTextRenderer *This, void *c, FLOAT x, FLOAT y, IDWriteInlineObject *o, BOOL sw, BOOL rtl, IUnknown *e) {
  (void)e;
  ++((Renderer *)This)->inlines;
  return IDWriteInlineObject_Draw(o, c, This, x, y, sw, rtl, NULL);      /* (the object draws itself through the renderer) */
}
static const IDWriteTextRendererVtbl r_vtbl = { r_qi, r_ref, r_ref, r_snap, r_xform, r_ppd, r_glyphs, r_under, r_strike, r_inline };
static void renderer_init(Renderer *r) { memset(r, 0, sizeof(*r)); r->iface.lpVtbl = &r_vtbl; }

static void test_factory(void) {
  IDWriteFactory *a = NULL, *b = NULL, *iso = NULL;
  IUnknown *u = NULL;
  check_hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&a), S_OK, "shared factory");
  check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&b)) && a == b, "the shared factory is one object");
  check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, &IID_IDWriteFactory, (IUnknown **)&iso)) && iso != a, "an isolated factory is its own");
  IDWriteFactory_Release(iso);
  IDWriteFactory_Release(b);
  check_hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFontFace, &u), E_NOINTERFACE, "a wrong IID");
  check_hr(DWriteCreateFactory((DWRITE_FACTORY_TYPE)7, &IID_IDWriteFactory, &u), E_INVALIDARG, "a wrong type");
  g_factory = a;
}

static void test_collection(void) {
  UINT32 index = 0;
  BOOL exists = FALSE;
  WCHAR prev[64] = L"", name[64];
  int sorted = 1;
  check_hr(IDWriteFactory_GetSystemFontCollection(g_factory, &g_system, FALSE), S_OK, "the system collection");
  const UINT32 count = IDWriteFontCollection_GetFontFamilyCount(g_system);
  check(count >= 10, "the system has families");
  check(IDWriteFontCollection_FindFamilyName(g_system, L"Arial", &index, &exists) == S_OK && exists && index < count, "Arial exists");
  check(IDWriteFontCollection_FindFamilyName(g_system, L"aRIAL", &index, &exists) == S_OK && exists, "family names are case insensitive");
  check(IDWriteFontCollection_FindFamilyName(g_system, L"No Such Family", &index, &exists) == S_OK && !exists && index == (UINT32)-1, "an unknown family");
  for (UINT32 i = 0; i < count; ++i) {
    IDWriteFontFamily *fam = NULL;
    IDWriteLocalizedStrings *names = NULL;
    check_hr(IDWriteFontCollection_GetFontFamily(g_system, i, &fam), S_OK, "GetFontFamily");
    check_hr(IDWriteFontFamily_GetFamilyNames(fam, &names), S_OK, "GetFamilyNames");
    check_hr(IDWriteLocalizedStrings_GetString(names, 0, name, 64), S_OK, "GetString");
    if (prev[0] && lstrcmpiW(prev, name) > 0) sorted = 0;
    lstrcpyW(prev, name);
    IDWriteLocalizedStrings_Release(names);
    IDWriteFontFamily_Release(fam);
  }
  check(sorted, "families are sorted");
  IDWriteFontFamily *fam = NULL;
  check_hr(IDWriteFontCollection_GetFontFamily(g_system, count, &fam), E_INVALIDARG, "a family index past the end");
  check(fam == NULL, "no family");
}

static void test_family_and_font(void) {
  UINT32 index;
  BOOL exists, has = FALSE;
  IDWriteFontFamily *fam = NULL, *fam2 = NULL;
  IDWriteFont *font = NULL, *bold = NULL, *italic = NULL, *first = NULL, *again = NULL;
  IDWriteFontList *list = NULL;
  IDWriteLocalizedStrings *names = NULL;
  DWRITE_FONT_METRICS m;
  IDWriteFontCollection_FindFamilyName(g_system, L"Arial", &index, &exists);
  check_hr(IDWriteFontCollection_GetFontFamily(g_system, index, &fam), S_OK, "the Arial family");
  check(IDWriteFontFamily_GetFontCount(fam) == 4, "four faces");
  check_hr(IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font), S_OK, "regular");
  check(IDWriteFont_GetWeight(font) == DWRITE_FONT_WEIGHT_NORMAL && IDWriteFont_GetStyle(font) == DWRITE_FONT_STYLE_NORMAL && IDWriteFont_GetStretch(font) == DWRITE_FONT_STRETCH_NORMAL,
        "regular properties");
  check(IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &bold) == S_OK && IDWriteFont_GetWeight(bold) == DWRITE_FONT_WEIGHT_BOLD, "bold");
  check(IDWriteFontFamily_GetFirstMatchingFont(fam, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_ITALIC, &italic) == S_OK &&
            IDWriteFont_GetStyle(italic) == DWRITE_FONT_STYLE_ITALIC && IDWriteFont_GetWeight(italic) == DWRITE_FONT_WEIGHT_NORMAL, "italic");
  check(IDWriteFontFamily_GetMatchingFonts(fam, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &list) == S_OK && IDWriteFontList_GetFontCount(list) == 4, "the matching fonts");
  check(IDWriteFontList_GetFont(list, 0, &first) == S_OK && IDWriteFont_GetWeight(first) == DWRITE_FONT_WEIGHT_BOLD, "the best match comes first");
  IDWriteFont_Release(first);
  IDWriteFontList_Release(list);

  IDWriteFont_GetMetrics(font, &m);
  check(m.designUnitsPerEm == 2048 && m.ascent > 1500 && m.descent > 300 && m.xHeight > 800 && m.capHeight > 1100, "font metrics");
  check(m.underlineThickness > 0 && m.underlinePosition < 0 && m.strikethroughThickness > 0 && m.strikethroughPosition > 0, "decoration metrics");
  check(IDWriteFont_HasCharacter(font, 'A', &has) == S_OK && has, "has A");
  check(IDWriteFont_HasCharacter(font, 0x10FFFF, &has) == S_OK && !has, "has not U+10FFFF");
  check(IDWriteFont_GetInformationalStrings(font, DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &names, &has) == S_OK && has && names, "the family name string");
  if (names) IDWriteLocalizedStrings_Release(names);
  names = NULL;
  check(IDWriteFont_GetInformationalStrings(font, DWRITE_INFORMATIONAL_STRING_NONE, &names, &has) == S_OK && !has && names == NULL, "no string for id none");
  check_hr(IDWriteFont_GetFaceNames(bold, &names), S_OK, "face names");
  if (names) {
    UINT32 found = 7;
    check(IDWriteLocalizedStrings_FindLocaleName(names, L"en-US", &found, &has) == S_OK && has && found == 0, "the en-us locale");
    IDWriteLocalizedStrings_Release(names);
  }
  check_hr(IDWriteFont_CreateFontFace(font, &g_face), S_OK, "CreateFontFace");
  check(IDWriteFont_GetFontFamily(font, &fam2) == S_OK, "font -> family");
  if (fam2) {
    IDWriteFontCollection *c = NULL;
    check(IDWriteFontFamily_GetFontCollection(fam2, &c) == S_OK && c == g_system, "family -> collection");
    IDWriteFontCollection_Release(c);
    IDWriteFontFamily_Release(fam2);
  }
  check(IDWriteFontCollection_GetFontFromFontFace(g_system, g_face, &again) == S_OK && IDWriteFont_GetWeight(again) == DWRITE_FONT_WEIGHT_NORMAL, "GetFontFromFontFace");
  IDWriteFont_Release(again);
  IDWriteFont_Release(italic);
  IDWriteFont_Release(bold);
  IDWriteFont_Release(font);
  IDWriteFontFamily_Release(fam);
}

static void test_face(void) {
  IDWriteFontFace *f = g_face;
  const UINT32 cps[4] = {'A', 'B', ' ', 0x10FFFF};
  UINT16 g[4] = {9, 9, 9, 9};
  DWRITE_GLYPH_METRICS gm[3];
  const void *table = NULL;
  void *ctx = NULL;
  UINT32 size = 0, n = 0;
  BOOL exists = FALSE;
  IDWriteFontFile *file = NULL;
  check(IDWriteFontFace_GetType(f) == DWRITE_FONT_FACE_TYPE_TRUETYPE && IDWriteFontFace_GetIndex(f) == 0 && IDWriteFontFace_GetSimulations(f) == 0, "face basics");
  check(IDWriteFontFace_GetGlyphCount(f) > 200, "the glyph count");
  check(IDWriteFontFace_GetGlyphIndices(f, cps, 4, g) == S_OK && g[0] && g[1] && g[0] != g[1] && g[2] && g[3] == 0, "glyph indices");
  check(IDWriteFontFace_GetDesignGlyphMetrics(f, g, 3, gm, FALSE) == S_OK && gm[0].advanceWidth > 1000 && gm[0].advanceWidth < 2500 && gm[2].advanceWidth > 0, "design glyph metrics");
  check(gm[0].verticalOriginY > 0 && gm[0].leftSideBearing + (INT32)gm[0].advanceWidth - gm[0].rightSideBearing > 0, "glyph bearings");
  check(IDWriteFontFace_TryGetFontTable(f, DWRITE_MAKE_OPENTYPE_TAG('h', 'e', 'a', 'd'), &table, &size, &ctx, &exists) == S_OK && exists && size == 54 && table, "the head table");
  if (exists && size >= 16) {
    const BYTE *t = table;
    check(t[12] == 0x5F && t[13] == 0x0F && t[14] == 0x3C && t[15] == 0xF5, "the head magic number");
  }
  IDWriteFontFace_ReleaseFontTable(f, ctx);
  check(IDWriteFontFace_TryGetFontTable(f, DWRITE_MAKE_OPENTYPE_TAG('Z', 'Z', 'Z', 'Z'), &table, &size, &ctx, &exists) == S_OK && !exists && table == NULL && size == 0, "a missing table");
  check(IDWriteFontFace_GetFiles(f, &n, NULL) == S_OK && n == 1, "one file");
  check(IDWriteFontFace_GetFiles(f, &n, &file) == S_OK && file, "the file");
  if (file) {
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE ft;
    DWRITE_FONT_FACE_TYPE ct;
    UINT32 faces = 0, key_size = 0;
    const void *key = NULL, *frag = NULL;
    void *fctx = NULL;
    IDWriteFontFileLoader *loader = NULL;
    IDWriteFontFileStream *stream = NULL;
    UINT64 total = 0;
    check(IDWriteFontFile_Analyze(file, &supported, &ft, &ct, &faces) == S_OK && supported && ft == DWRITE_FONT_FILE_TYPE_TRUETYPE && ct == DWRITE_FONT_FACE_TYPE_TRUETYPE && faces == 1, "analyzing the system font file");
    check(IDWriteFontFile_GetReferenceKey(file, &key, &key_size) == S_OK && key_size > 0, "the reference key");
    check(IDWriteFontFile_GetLoader(file, &loader) == S_OK && loader, "the loader");
    check(IDWriteFontFileLoader_CreateStreamFromKey(loader, key, key_size, &stream) == S_OK && stream, "the stream");
    if (stream) {
      IDWriteFontFileStream_GetFileSize(stream, &total);
      check(total > 10000, "the font file size");
      check(IDWriteFontFileStream_ReadFileFragment(stream, &frag, 0, total, &fctx) == S_OK && frag, "reading the whole file");
      CreateDirectoryW(L"C:\\Temp", NULL);
      HANDLE h = CreateFileW(L"C:\\Temp\\dwtest.ttf", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      DWORD w = 0;
      check(h != INVALID_HANDLE_VALUE && WriteFile(h, frag, (DWORD)total, &w, NULL) && w == total, "writing the font file out");
      CloseHandle(h);
      IDWriteFontFileStream_ReleaseFileFragment(stream, fctx);
      check(FAILED(IDWriteFontFileStream_ReadFileFragment(stream, &frag, total, 1, &fctx)), "reading past the end");
      IDWriteFontFileStream_Release(stream);
    }
    IDWriteFontFileLoader_Release(loader);
    IDWriteFontFile_Release(file);
  }
  DWRITE_RENDERING_MODE mode = DWRITE_RENDERING_MODE_DEFAULT;
  check(IDWriteFontFace_GetRecommendedRenderingMode(f, 12.0f, 1.0f, DWRITE_MEASURING_MODE_NATURAL, NULL, &mode) == S_OK && mode != DWRITE_RENDERING_MODE_DEFAULT, "the recommended rendering mode");
  DWRITE_FONT_METRICS gdi, nat;
  IDWriteFontFace_GetMetrics(f, &nat);
  check(IDWriteFontFace_GetGdiCompatibleMetrics(f, 13.0f, 1.0f, NULL, &gdi) == S_OK && gdi.designUnitsPerEm == nat.designUnitsPerEm && gdi.ascent > 0, "GDI compatible metrics");
  check_hr(IDWriteFontFace_GetGdiCompatibleMetrics(f, 0.0f, 1.0f, NULL, &gdi), E_INVALIDARG, "size zero");
}

static void test_custom_file(void) {
  IDWriteFontFile *file = NULL, *bad = NULL;
  IDWriteFontFace *face = NULL, *face2 = NULL;
  BOOL supported = FALSE;
  DWRITE_FONT_FILE_TYPE ft;
  DWRITE_FONT_FACE_TYPE ct;
  UINT32 faces;
  DWORD w;
  check(FAILED(IDWriteFactory_CreateFontFileReference(g_factory, L"C:\\Temp\\no_such_font.ttf", NULL, &file)) && !file, "a missing file");
  check_hr(IDWriteFactory_CreateFontFileReference(g_factory, L"C:\\Temp\\dwtest.ttf", NULL, &file), S_OK, "a file reference");
  if (!file) return;
  check(IDWriteFontFile_Analyze(file, &supported, &ft, &ct, &faces) == S_OK && supported && ft == DWRITE_FONT_FILE_TYPE_TRUETYPE, "analyzing the file");
  check_hr(IDWriteFactory_CreateFontFace(g_factory, DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &face), S_OK, "a face of the file");
  if (face) {
    const UINT32 cps[2] = {'H', 'g'};
    UINT16 a[2], b[2];
    IDWriteFontFace_GetGlyphIndices(face, cps, 2, a);
    IDWriteFontFace_GetGlyphIndices(g_face, cps, 2, b);
    check(a[0] && a[0] == b[0] && a[1] == b[1], "the added font has the glyphs of the original");
    check(IDWriteFactory_CreateFontFace(g_factory, DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &face2) == S_OK && face2, "a second face of the same file");
    if (face2) IDWriteFontFace_Release(face2);
    IDWriteFontFace_Release(face);
  }
  HANDLE h = CreateFileW(L"C:\\Temp\\dwbad.ttf", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  WriteFile(h, "this is not a font file, only text.......", 40, &w, NULL);
  CloseHandle(h);
  check_hr(IDWriteFactory_CreateFontFileReference(g_factory, L"C:\\Temp\\dwbad.ttf", NULL, &bad), S_OK, "a reference to a bad file");
  supported = TRUE;
  check(IDWriteFontFile_Analyze(bad, &supported, &ft, &ct, &faces) == S_OK && !supported, "a bad file is not supported");
  face = NULL;
  check(FAILED(IDWriteFactory_CreateFontFace(g_factory, DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, &bad, 0, DWRITE_FONT_SIMULATIONS_NONE, &face)) && !face, "no face from a bad file");
  IDWriteFontFile_Release(bad);
  IDWriteFontFile_Release(file);
  DeleteFileW(L"C:\\Temp\\dwbad.ttf");
}

static IDWriteTextFormat *make_format(float size, DWRITE_FONT_WEIGHT weight) {
  IDWriteTextFormat *f = NULL;
  IDWriteFactory_CreateTextFormat(g_factory, L"Arial", NULL, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
  return f;
}
static IDWriteTextLayout *make_layout(const WCHAR *text, IDWriteTextFormat *f, float w, float h) {
  IDWriteTextLayout *l = NULL;
  check_hr(IDWriteFactory_CreateTextLayout(g_factory, text, lstrlenW(text), f, w, h, &l), S_OK, "a text layout");
  return l;
}

static void test_text_format(void) {
  IDWriteTextFormat *f = make_format(16.0f, DWRITE_FONT_WEIGHT_NORMAL);
  WCHAR name[32];
  DWRITE_LINE_SPACING_METHOD method;
  float spacing, baseline;
  check(f != NULL, "a text format");
  if (!f) return;
  check(IDWriteTextFormat_GetFontSize(f) == 16.0f && IDWriteTextFormat_GetFontWeight(f) == DWRITE_FONT_WEIGHT_NORMAL, "format properties");
  check(IDWriteTextFormat_GetFontFamilyName(f, name, 32) == S_OK && !lstrcmpW(name, L"Arial") && IDWriteTextFormat_GetFontFamilyNameLength(f) == 5, "the family name");
  check_hr(IDWriteTextFormat_GetFontFamilyName(f, name, 5), E_NOT_SUFFICIENT_BUFFER, "a short buffer");
  check(IDWriteTextFormat_GetLocaleName(f, name, 32) == S_OK && !lstrcmpW(name, L"en-us"), "the locale name");
  check(IDWriteTextFormat_GetTextAlignment(f) == DWRITE_TEXT_ALIGNMENT_LEADING && IDWriteTextFormat_GetParagraphAlignment(f) == DWRITE_PARAGRAPH_ALIGNMENT_NEAR &&
            IDWriteTextFormat_GetWordWrapping(f) == DWRITE_WORD_WRAPPING_WRAP && IDWriteTextFormat_GetReadingDirection(f) == DWRITE_READING_DIRECTION_LEFT_TO_RIGHT, "defaults");
  check(IDWriteTextFormat_SetTextAlignment(f, DWRITE_TEXT_ALIGNMENT_CENTER) == S_OK && IDWriteTextFormat_GetTextAlignment(f) == DWRITE_TEXT_ALIGNMENT_CENTER, "setting the alignment");
  check_hr(IDWriteTextFormat_SetTextAlignment(f, (DWRITE_TEXT_ALIGNMENT)9), E_INVALIDARG, "a bad alignment");
  check_hr(IDWriteTextFormat_SetLineSpacing(f, DWRITE_LINE_SPACING_METHOD_UNIFORM, 20.0f, 15.0f), S_OK, "line spacing");
  IDWriteTextFormat_GetLineSpacing(f, &method, &spacing, &baseline);
  check(method == DWRITE_LINE_SPACING_METHOD_UNIFORM && spacing == 20.0f && baseline == 15.0f, "line spacing values");
  check_hr(IDWriteTextFormat_SetIncrementalTabStop(f, -1.0f), E_INVALIDARG, "a bad tab stop");
  IDWriteTextFormat_Release(f);
  f = NULL;
  check(FAILED(IDWriteFactory_CreateTextFormat(g_factory, L"Arial", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 0.0f, L"en-us", &f)) && !f, "size zero");
  check(FAILED(IDWriteFactory_CreateTextFormat(g_factory, NULL, NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &f)), "no family name");
}

static void test_layout(void) {
  IDWriteTextFormat *f = make_format(16.0f, DWRITE_FONT_WEIGHT_NORMAL), *big = make_format(32.0f, DWRITE_FONT_WEIGHT_NORMAL), *bold = make_format(16.0f, DWRITE_FONT_WEIGHT_BOLD);
  DWRITE_TEXT_METRICS m, m2, m3;
  DWRITE_LINE_METRICS lm[4];
  DWRITE_CLUSTER_METRICS cm[16];
  UINT32 n = 0, total = 0;
  HRESULT hr;
  IDWriteTextLayout *l = make_layout(L"Hello World", f, 500.0f, 200.0f), *lb = make_layout(L"Hello World", big, 500.0f, 200.0f), *lbold = make_layout(L"Hello World", bold, 500.0f, 200.0f);
  check_hr(IDWriteTextLayout_GetMetrics(l, &m), S_OK, "layout metrics");
  check(m.lineCount == 1 && m.width > 50 && m.width < 120 && m.height > 15 && m.height < 25 && m.layoutWidth == 500.0f && m.left == 0 && m.top == 0, "single line metrics");
  IDWriteTextLayout_GetMetrics(lb, &m2);
  check(m2.width > m.width * 1.9f && m2.width < m.width * 2.1f && m2.height > m.height * 1.9f, "twice the size, twice the extent");
  IDWriteTextLayout_GetMetrics(lbold, &m3);
  check(m3.width > m.width, "bold is wider");
  hr = IDWriteTextLayout_GetLineMetrics(l, lm, 4, &n);
  check(hr == S_OK && n == 1 && lm[0].length == 11 && lm[0].newlineLength == 0 && lm[0].trailingWhitespaceLength == 0 && lm[0].baseline > 10 && lm[0].baseline < lm[0].height, "line metrics");
  check(IDWriteTextLayout_GetLineMetrics(l, NULL, 0, &n) == E_NOT_SUFFICIENT_BUFFER && n == 1, "the line count");
  hr = IDWriteTextLayout_GetClusterMetrics(l, cm, 16, &n);
  check(hr == S_OK && n == 11 && cm[5].isWhitespace && cm[5].canWrapLineAfter && !cm[0].isWhitespace, "cluster metrics");
  float sum = 0;
  for (UINT32 i = 0; i < n; ++i) sum += cm[i].width;
  check(sum > m.width - 0.01f && sum < m.widthIncludingTrailingWhitespace + 0.01f, "the clusters add up to the width");

  IDWriteTextLayout *w = make_layout(L"Hello World again and again", f, 80.0f, 200.0f);
  IDWriteTextLayout_GetMetrics(w, &m);
  hr = IDWriteTextLayout_GetLineMetrics(w, lm, 4, &n);
  check(m.lineCount >= 3 && m.width <= 80.5f && n >= 3, "wrapped into lines");
  for (UINT32 i = 0; i < n && i < 4; ++i) total += lm[i].length;
  check(n > 4 || total == 27, "the lines cover the text");
  check(lm[0].trailingWhitespaceLength == 1, "a wrapped line ends with its space");
  IDWriteTextLayout_Release(w);
  w = make_layout(L"Hello World", f, 80.0f, 200.0f);
  IDWriteTextLayout_SetWordWrapping(w, DWRITE_WORD_WRAPPING_NO_WRAP);
  IDWriteTextLayout_GetMetrics(w, &m);
  check(m.lineCount == 1, "no wrap: one line");
  IDWriteTextLayout_SetWordWrapping(w, DWRITE_WORD_WRAPPING_WRAP);
  IDWriteTextLayout_SetMaxWidth(w, 30.0f);
  IDWriteTextLayout_GetMetrics(w, &m);
  check(m.lineCount >= 3, "a word that does not fit is broken");
  IDWriteTextLayout_Release(w);

  IDWriteTextLayout *nl = make_layout(L"one\ntwo\r\nthree", f, 500.0f, 500.0f);
  IDWriteTextLayout_GetMetrics(nl, &m);
  IDWriteTextLayout_GetLineMetrics(nl, lm, 4, &n);
  check(m.lineCount == 3 && n == 3 && lm[0].newlineLength == 1 && lm[1].newlineLength == 2 && lm[2].newlineLength == 0, "explicit newlines");
  IDWriteTextLayout *empty = make_layout(L"", f, 100.0f, 100.0f);
  IDWriteTextLayout_GetMetrics(empty, &m);
  check(m.lineCount == 1 && m.width == 0 && m.height > 10, "empty text has one empty line");

  IDWriteTextLayout_SetTextAlignment(l, DWRITE_TEXT_ALIGNMENT_TRAILING);
  IDWriteTextLayout_GetMetrics(l, &m);
  check(m.left > 400.0f, "trailing alignment");
  IDWriteTextLayout_SetTextAlignment(l, DWRITE_TEXT_ALIGNMENT_CENTER);
  IDWriteTextLayout_GetMetrics(l, &m);
  check(m.left > 200.0f && m.left < 250.0f, "centered");
  IDWriteTextLayout_SetTextAlignment(l, DWRITE_TEXT_ALIGNMENT_LEADING);
  IDWriteTextLayout_SetParagraphAlignment(l, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
  IDWriteTextLayout_GetMetrics(l, &m);
  check(m.top > 150.0f, "paragraph alignment far");
  IDWriteTextLayout_SetParagraphAlignment(l, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

  BOOL trailing, inside;
  DWRITE_HIT_TEST_METRICS hm, ranges[4];
  float px, py, px2, minw;
  hr = IDWriteTextLayout_HitTestPoint(l, 1.0f, 5.0f, &trailing, &inside, &hm);
  check(hr == S_OK && inside && !trailing && hm.textPosition == 0 && hm.length == 1 && hm.width > 4, "hit testing the first character");
  hr = IDWriteTextLayout_HitTestPoint(l, 400.0f, 5.0f, &trailing, &inside, &hm);
  check(hr == S_OK && !inside && trailing && hm.textPosition == 10, "hit testing past the end");
  hr = IDWriteTextLayout_HitTestTextPosition(l, 5, FALSE, &px, &py, &hm);
  check(hr == S_OK && px > 30 && px < 50 && py == 0 && hm.textPosition == 5, "the caret position");
  hr = IDWriteTextLayout_HitTestTextPosition(l, 5, TRUE, &px2, &py, &hm);
  check(hr == S_OK && px2 > px, "the trailing edge is right of the leading edge");
  hr = IDWriteTextLayout_HitTestTextRange(l, 0, 5, 10.0f, 20.0f, ranges, 4, &n);
  check(hr == S_OK && n == 1 && ranges[0].textPosition == 0 && ranges[0].length == 5 && ranges[0].left == 10.0f && ranges[0].top == 20.0f && ranges[0].width > 30, "a text range");
  IDWriteTextLayout_GetMetrics(l, &m);
  check(IDWriteTextLayout_DetermineMinWidth(l, &minw) == S_OK && minw > 20 && minw < m.width, "the minimum width");

  DWRITE_TEXT_RANGE r1 = {6, 5}, r;
  DWRITE_FONT_WEIGHT wt;
  float size;
  WCHAR fam[40];
  check_hr(IDWriteTextLayout_SetFontWeight(l, DWRITE_FONT_WEIGHT_BOLD, r1), S_OK, "weight on a range");
  check(IDWriteTextLayout_GetFontWeight(l, 7, &wt, &r) == S_OK && wt == DWRITE_FONT_WEIGHT_BOLD && r.startPosition == 6 && r.length == 5, "the weight and its range");
  check(IDWriteTextLayout_GetFontWeight(l, 0, &wt, &r) == S_OK && wt == DWRITE_FONT_WEIGHT_NORMAL && r.startPosition == 0 && r.length == 6, "the range before");
  IDWriteTextLayout_GetMetrics(l, &m2);
  IDWriteTextLayout_SetFontWeight(l, DWRITE_FONT_WEIGHT_NORMAL, r1);
  IDWriteTextLayout_GetMetrics(l, &m);
  check(m2.width > m.width, "a bold range is wider");
  check_hr(IDWriteTextLayout_SetFontSize(l, 24.0f, r1), S_OK, "size on a range");
  IDWriteTextLayout_GetFontSize(l, 8, &size, &r);
  check(size == 24.0f && r.startPosition == 6, "the size of a range");
  check_hr(IDWriteTextLayout_SetFontFamilyName(l, L"Times New Roman", r1), S_OK, "family on a range");
  IDWriteTextLayout_GetFontFamilyName(l, 8, fam, 40, &r);
  check(!lstrcmpW(fam, L"Times New Roman"), "the family of a range");
  check_hr(IDWriteTextLayout_SetFontWeight(l, (DWRITE_FONT_WEIGHT)0, r1), E_INVALIDARG, "weight zero");
  check(IDWriteTextLayout_GetFontWeight(l, 50, &wt, &r) == S_OK && r.startPosition == 11, "a position past the text");

  Renderer rd;
  renderer_init(&rd);
  IDWriteTextLayout *dl = make_layout(L"Draw this text", f, 500.0f, 100.0f);
  DWRITE_TEXT_RANGE under = {0, 4}, strike = {5, 4};
  IDWriteTextLayout_SetUnderline(dl, TRUE, under);
  IDWriteTextLayout_SetStrikethrough(dl, TRUE, strike);
  check_hr(IDWriteTextLayout_Draw(dl, NULL, &rd.iface, 10.0f, 20.0f), S_OK, "drawing");
  check(rd.runs >= 3 && rd.glyphs == 14, "draw: glyph runs");
  check(rd.underlines == 1 && rd.strikes == 1, "draw: decorations");
  check(rd.last_y > 30.0f && rd.last_x >= 10.0f, "draw: the baseline origin");

  IDWriteTextFormat *tf = make_format(16.0f, DWRITE_FONT_WEIGHT_NORMAL);
  DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}, got;
  IDWriteInlineObject *sign = NULL, *got_sign = NULL;
  DWRITE_INLINE_OBJECT_METRICS im;
  IDWriteTextFormat_SetWordWrapping(tf, DWRITE_WORD_WRAPPING_NO_WRAP);
  check_hr(IDWriteFactory_CreateEllipsisTrimmingSign(g_factory, tf, &sign), S_OK, "the ellipsis sign");
  IDWriteInlineObject_GetMetrics(sign, &im);
  check(im.width > 5 && im.height > 10 && im.baseline > 8, "the sign's metrics");
  IDWriteTextFormat_SetTrimming(tf, &trim, sign);
  IDWriteTextLayout *tl = make_layout(L"A long line of text that is cut", tf, 100.0f, 50.0f);
  IDWriteTextLayout_GetMetrics(tl, &m);
  IDWriteTextLayout_GetLineMetrics(tl, lm, 4, &n);
  check(m.lineCount == 1 && lm[0].isTrimmed && m.width <= 100.5f, "a trimmed line");
  renderer_init(&rd);
  check(IDWriteTextLayout_Draw(tl, NULL, &rd.iface, 0, 0) == S_OK && rd.glyphs >= 8, "drawing a trimmed line");
  IDWriteTextFormat_GetTrimming(tf, &got, &got_sign);
  check(got.granularity == DWRITE_TRIMMING_GRANULARITY_CHARACTER && got_sign == sign, "the trimming getter");
  if (got_sign) IDWriteInlineObject_Release(got_sign);
  IDWriteTextLayout_Release(tl);
  IDWriteInlineObject_Release(sign);
  IDWriteTextFormat_Release(tf);
  IDWriteTextLayout_Release(dl);
  IDWriteTextLayout_Release(empty);
  IDWriteTextLayout_Release(nl);
  IDWriteTextLayout_Release(lbold);
  IDWriteTextLayout_Release(lb);
  IDWriteTextLayout_Release(l);
  IDWriteTextFormat_Release(bold);
  IDWriteTextFormat_Release(big);
  l = make_layout(L"a\x0301" L"b\xD83D\xDE00", f, 500.0f, 100.0f);
  hr = IDWriteTextLayout_GetClusterMetrics(l, cm, 16, &n);
  check(hr == S_OK && n == 3 && cm[0].length == 2 && cm[1].length == 1 && cm[2].length == 2, "clusters: a combining mark and a surrogate pair");
  IDWriteTextLayout_Release(l);
  IDWriteTextFormat_Release(f);
}

/* ---- the text analyzer */
typedef struct Source { IDWriteTextAnalysisSource iface; const WCHAR *text; UINT32 length; } Source;
static HRESULT WINAPI s_qi(IDWriteTextAnalysisSource *This, REFIID r, void **o) { (void)r; *o = This; return S_OK; }
static ULONG WINAPI s_ref(IDWriteTextAnalysisSource *This) { (void)This; return 2; }
static HRESULT WINAPI s_at(IDWriteTextAnalysisSource *This, UINT32 pos, const WCHAR **t, UINT32 *n) { Source *s = (Source *)This; *t = s->text + pos; *n = s->length - pos; return S_OK; }
static HRESULT WINAPI s_before(IDWriteTextAnalysisSource *This, UINT32 pos, const WCHAR **t, UINT32 *n) { Source *s = (Source *)This; *t = s->text; *n = pos; return S_OK; }
static DWRITE_READING_DIRECTION WINAPI s_dir(IDWriteTextAnalysisSource *This) { (void)This; return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
static HRESULT WINAPI s_loc(IDWriteTextAnalysisSource *This, UINT32 pos, UINT32 *n, const WCHAR **l) { Source *s = (Source *)This; *n = s->length - pos; *l = L"en-us"; return S_OK; }
static HRESULT WINAPI s_num(IDWriteTextAnalysisSource *This, UINT32 pos, UINT32 *n, IDWriteNumberSubstitution **sub) { Source *s = (Source *)This; *n = s->length - pos; *sub = NULL; return S_OK; }
static const IDWriteTextAnalysisSourceVtbl s_vtbl = { s_qi, s_ref, s_ref, s_at, s_before, s_dir, s_loc, s_num };
typedef struct Sink { IDWriteTextAnalysisSink iface; DWRITE_LINE_BREAKPOINT bp[32]; UINT32 bp_count, scripts, bidi; } Sink;
static HRESULT WINAPI k_qi(IDWriteTextAnalysisSink *This, REFIID r, void **o) { (void)r; *o = This; return S_OK; }
static ULONG WINAPI k_ref(IDWriteTextAnalysisSink *This) { (void)This; return 2; }
static HRESULT WINAPI k_script(IDWriteTextAnalysisSink *This, UINT32 p, UINT32 n, const DWRITE_SCRIPT_ANALYSIS *a) { (void)p; (void)n; (void)a; ++((Sink *)This)->scripts; return S_OK; }
static HRESULT WINAPI k_break(IDWriteTextAnalysisSink *This, UINT32 p, UINT32 n, const DWRITE_LINE_BREAKPOINT *b) {
  Sink *s = (Sink *)This;
  for (UINT32 i = 0; i < n && p + i < 32; ++i) s->bp[p + i] = b[i];
  s->bp_count = p + n;
  return S_OK;
}
static HRESULT WINAPI k_bidi(IDWriteTextAnalysisSink *This, UINT32 p, UINT32 n, UINT8 e, UINT8 r) { (void)p; (void)n; (void)e; (void)r; ++((Sink *)This)->bidi; return S_OK; }
static HRESULT WINAPI k_num(IDWriteTextAnalysisSink *This, UINT32 p, UINT32 n, IDWriteNumberSubstitution *s) { (void)This; (void)p; (void)n; (void)s; return S_OK; }
static const IDWriteTextAnalysisSinkVtbl k_vtbl = { k_qi, k_ref, k_ref, k_script, k_break, k_bidi, k_num };

static void test_analyzer(void) {
  IDWriteTextAnalyzer *an = NULL;
  static const WCHAR text[] = L"ab cd-ef\n";
  static const WCHAR word[] = L"Hi \xD83D\xDE00";
  Source src = {{&s_vtbl}, text, 9};
  Sink sink;
  UINT16 cmap[8] = {0}, glyphs[8] = {0};
  DWRITE_SHAPING_TEXT_PROPERTIES tp[8];
  DWRITE_SHAPING_GLYPH_PROPERTIES gp[8];
  DWRITE_SCRIPT_ANALYSIS sa = {0, DWRITE_SCRIPT_SHAPES_DEFAULT};
  UINT32 count = 0;
  float adv[8];
  DWRITE_GLYPH_OFFSET off[8];
  check_hr(IDWriteFactory_CreateTextAnalyzer(g_factory, &an), S_OK, "the analyzer");
  if (!an) return;
  memset(&sink, 0, sizeof(sink));
  sink.iface.lpVtbl = &k_vtbl;
  check(IDWriteTextAnalyzer_AnalyzeLineBreakpoints(an, &src.iface, 0, 9, &sink.iface) == S_OK && sink.bp_count == 9, "line breakpoints");
  check(sink.bp[1].breakConditionAfter == DWRITE_BREAK_CONDITION_MAY_NOT_BREAK && sink.bp[2].breakConditionAfter == DWRITE_BREAK_CONDITION_CAN_BREAK && sink.bp[2].isWhitespace, "a break after the space");
  check(sink.bp[5].breakConditionAfter == DWRITE_BREAK_CONDITION_CAN_BREAK, "a break after the hyphen");
  check(IDWriteTextAnalyzer_AnalyzeScript(an, &src.iface, 0, 9, &sink.iface) == S_OK && sink.scripts == 1, "script analysis");
  check(IDWriteTextAnalyzer_AnalyzeBidi(an, &src.iface, 0, 9, &sink.iface) == S_OK && sink.bidi == 1, "bidi analysis");
  check(IDWriteTextAnalyzer_GetGlyphs(an, word, 5, g_face, FALSE, FALSE, &sa, L"en-us", NULL, NULL, NULL, 0, 8, cmap, tp, glyphs, gp, &count) == S_OK && count == 4 && cmap[0] == 0 &&
            cmap[1] == 1 && cmap[3] == 3 && cmap[4] == 3 && glyphs[0] != 0 && glyphs[1] != 0 && glyphs[3] == 0, "GetGlyphs");
  check_hr(IDWriteTextAnalyzer_GetGlyphs(an, word, 5, g_face, FALSE, FALSE, &sa, L"en-us", NULL, NULL, NULL, 0, 2, cmap, tp, glyphs, gp, &count), E_NOT_SUFFICIENT_BUFFER, "too few glyphs");
  IDWriteTextAnalyzer_GetGlyphs(an, word, 5, g_face, FALSE, FALSE, &sa, L"en-us", NULL, NULL, NULL, 0, 8, cmap, tp, glyphs, gp, &count);
  check(IDWriteTextAnalyzer_GetGlyphPlacements(an, word, cmap, tp, 5, glyphs, gp, 3, g_face, 16.0f, FALSE, FALSE, &sa, L"en-us", NULL, NULL, 0, adv, off) == S_OK && adv[0] > 8 &&
            adv[0] < 14 && adv[1] > 2 && adv[1] < 6 && off[0].advanceOffset == 0, "GetGlyphPlacements");
  check(IDWriteTextAnalyzer_GetGdiCompatibleGlyphPlacements(an, word, cmap, tp, 5, glyphs, gp, 3, g_face, 16.0f, 1.0f, NULL, FALSE, FALSE, FALSE, &sa, L"en-us", NULL, NULL, 0, adv, off) == S_OK &&
            adv[0] == (float)(int)adv[0] && adv[1] == (float)(int)adv[1], "GDI compatible placements are whole pixels");
  IDWriteTextAnalyzer_Release(an);
}

/* ---- glyph run analysis, outlines, the bitmap render target */
typedef struct Sink2 { ID2D1SimplifiedGeometrySink iface; int figures, lines, beziers, closed; } Sink2;
static HRESULT WINAPI g_qi(IUnknown *This, REFIID r, void **o) { (void)r; *o = This; return S_OK; }
static ULONG WINAPI g_ref(IUnknown *This) { (void)This; return 2; }
static void WINAPI g_fill(ID2D1SimplifiedGeometrySink *This, D2D1_FILL_MODE m) { (void)This; (void)m; }
static void WINAPI g_flags(ID2D1SimplifiedGeometrySink *This, D2D1_PATH_SEGMENT f) { (void)This; (void)f; }
static void WINAPI g_begin(ID2D1SimplifiedGeometrySink *This, D2D1_POINT_2F p, D2D1_FIGURE_BEGIN b) { (void)p; (void)b; ++((Sink2 *)This)->figures; }
static void WINAPI g_lines(ID2D1SimplifiedGeometrySink *This, const D2D1_POINT_2F *p, UINT32 n) { (void)p; ((Sink2 *)This)->lines += n; }
static void WINAPI g_bez(ID2D1SimplifiedGeometrySink *This, const D2D1_BEZIER_SEGMENT *b, UINT32 n) { (void)b; ((Sink2 *)This)->beziers += n; }
static void WINAPI g_end(ID2D1SimplifiedGeometrySink *This, D2D1_FIGURE_END e) { if (e == D2D1_FIGURE_END_CLOSED) ++((Sink2 *)This)->closed; }
static HRESULT WINAPI g_close(ID2D1SimplifiedGeometrySink *This) { (void)This; return S_OK; }
static const ID2D1SimplifiedGeometrySinkVtbl g_vtbl = { g_qi, g_ref, g_ref, g_fill, g_flags, g_begin, g_lines, g_bez, g_end, g_close };

static void test_glyph_run(void) {
  const UINT32 cps[3] = {'H', 'o', 'e'};
  UINT16 g[3];
  float adv[3] = {12.0f, 10.0f, 9.0f};
  DWRITE_GLYPH_RUN run;
  IDWriteGlyphRunAnalysis *an = NULL;
  RECT b;
  BYTE tex[40 * 20 * 3];
  Sink2 s;
  IDWriteFontFace_GetGlyphIndices(g_face, cps, 3, g);
  memset(&run, 0, sizeof(run));
  run.fontFace = g_face;
  run.fontEmSize = 16.0f;
  run.glyphCount = 3;
  run.glyphIndices = g;
  run.glyphAdvances = adv;
  check_hr(IDWriteFactory_CreateGlyphRunAnalysis(g_factory, &run, 1.0f, NULL, DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL, DWRITE_MEASURING_MODE_NATURAL, 5.0f, 20.0f, &an), S_OK, "a glyph run analysis");
  if (an) {
    check_hr(IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(an, DWRITE_TEXTURE_CLEARTYPE_3x1, &b), S_OK, "the texture bounds");
    const int w = b.right - b.left, h = b.bottom - b.top;
    check(w > 15 && w < 40 && h > 8 && h < 20 && b.left >= 3 && b.bottom <= 22, "plausible bounds");
    memset(tex, 0, sizeof(tex));
    int ink = 0, mono = 1;
    check_hr(IDWriteGlyphRunAnalysis_CreateAlphaTexture(an, DWRITE_TEXTURE_CLEARTYPE_3x1, &b, tex, sizeof(tex)), S_OK, "the texture");
    for (int i = 0; i < w * h * 3; ++i) ink += tex[i] > 128;
    check(ink > 30, "the texture has ink");
    check_hr(IDWriteGlyphRunAnalysis_CreateAlphaTexture(an, DWRITE_TEXTURE_ALIASED_1x1, &b, tex, 2), E_INVALIDARG, "a buffer that is too small");
    check_hr(IDWriteGlyphRunAnalysis_CreateAlphaTexture(an, DWRITE_TEXTURE_ALIASED_1x1, &b, tex, sizeof(tex)), S_OK, "the aliased texture");
    for (int i = 0; i < w * h; ++i) if (tex[i] != 0 && tex[i] != 255) mono = 0;
    check(mono, "an aliased texture is black and white");
    IDWriteGlyphRunAnalysis_Release(an);
  }
  an = NULL;
  check(FAILED(IDWriteFactory_CreateGlyphRunAnalysis(g_factory, &run, 1.0f, NULL, DWRITE_RENDERING_MODE_OUTLINE, DWRITE_MEASURING_MODE_NATURAL, 0, 0, &an)) && !an, "outline mode is refused");
  memset(&s, 0, sizeof(s));
  s.iface.lpVtbl = &g_vtbl;
  check(IDWriteFontFace_GetGlyphRunOutline(g_face, 16.0f, g, NULL, NULL, 1, FALSE, FALSE, &s.iface) == S_OK && s.figures == 1 && s.closed == 1 && s.lines >= 10 && s.beziers == 0, "the outline of H");
  memset(&s, 0, sizeof(s));
  s.iface.lpVtbl = &g_vtbl;
  check(IDWriteFontFace_GetGlyphRunOutline(g_face, 16.0f, g + 1, NULL, NULL, 1, FALSE, FALSE, &s.iface) == S_OK && s.figures == 2 && s.beziers >= 8, "the outline of o");
}

static void test_misc(void) {
  IDWriteRenderingParams *p = NULL;
  IDWriteTypography *t = NULL;
  IDWriteNumberSubstitution *ns = NULL;
  IDWriteGdiInterop *gdi = NULL;
  IDWriteFont *font = NULL;
  IDWriteFontCollection *c = NULL;
  LOGFONTW lf, out;
  DWRITE_FONT_FEATURE feat = {DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES, 1}, got;
  BOOL sys = FALSE;
  check(IDWriteFactory_CreateRenderingParams(g_factory, &p) == S_OK && IDWriteRenderingParams_GetGamma(p) > 1.0f && IDWriteRenderingParams_GetRenderingMode(p) == DWRITE_RENDERING_MODE_DEFAULT, "rendering parameters");
  IDWriteRenderingParams_Release(p);
  check(IDWriteFactory_CreateCustomRenderingParams(g_factory, 2.2f, 0.5f, 1.0f, DWRITE_PIXEL_GEOMETRY_BGR, DWRITE_RENDERING_MODE_ALIASED, &p) == S_OK &&
            IDWriteRenderingParams_GetPixelGeometry(p) == DWRITE_PIXEL_GEOMETRY_BGR && IDWriteRenderingParams_GetRenderingMode(p) == DWRITE_RENDERING_MODE_ALIASED, "custom parameters");
  IDWriteRenderingParams_Release(p);
  p = NULL;
  check_hr(IDWriteFactory_CreateCustomRenderingParams(g_factory, -1.0f, 0.5f, 1.0f, DWRITE_PIXEL_GEOMETRY_BGR, DWRITE_RENDERING_MODE_ALIASED, &p), E_INVALIDARG, "a negative gamma");
  check(IDWriteFactory_CreateTypography(g_factory, &t) == S_OK && IDWriteTypography_AddFontFeature(t, feat) == S_OK && IDWriteTypography_GetFontFeatureCount(t) == 1 &&
            IDWriteTypography_GetFontFeature(t, 0, &got) == S_OK && got.nameTag == feat.nameTag && IDWriteTypography_GetFontFeature(t, 1, &got) == E_INVALIDARG, "typography");
  IDWriteTypography_Release(t);
  check(IDWriteFactory_CreateNumberSubstitution(g_factory, DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE, L"en-us", FALSE, &ns) == S_OK && ns, "number substitution");
  if (ns) IDWriteNumberSubstitution_Release(ns);
  check_hr(IDWriteFactory_CreateNumberSubstitution(g_factory, DWRITE_NUMBER_SUBSTITUTION_METHOD_TRADITIONAL, L"dummy", FALSE, &ns), E_INVALIDARG, "a bad locale");

  check_hr(IDWriteFactory_GetGdiInterop(g_factory, &gdi), S_OK, "GDI interop");
  memset(&lf, 0, sizeof(lf));
  lstrcpyW(lf.lfFaceName, L"Arial");
  lf.lfWeight = FW_BOLD;
  check(IDWriteGdiInterop_CreateFontFromLOGFONT(gdi, &lf, &font) == S_OK && font && IDWriteFont_GetWeight(font) == DWRITE_FONT_WEIGHT_BOLD, "a font from a LOGFONT");
  if (font) {
    check(IDWriteGdiInterop_ConvertFontToLOGFONT(gdi, font, &out, &sys) == S_OK && sys && out.lfWeight == FW_BOLD && !lstrcmpW(out.lfFaceName, L"Arial") && !out.lfItalic, "a LOGFONT from a font");
    IDWriteFont_Release(font);
  }
  font = NULL;
  lstrcpyW(lf.lfFaceName, L"No Such Face");
  check(IDWriteGdiInterop_CreateFontFromLOGFONT(gdi, &lf, &font) == (HRESULT)0x88985002 && !font, "an unknown LOGFONT face");
  check(IDWriteGdiInterop_ConvertFontFaceToLOGFONT(gdi, g_face, &out) == S_OK && out.lfFaceName[0], "a LOGFONT from a face");

  IDWriteBitmapRenderTarget *target = NULL;
  SIZE size;
  check_hr(IDWriteGdiInterop_CreateBitmapRenderTarget(gdi, NULL, 64, 32, &target), S_OK, "a bitmap render target");
  if (target) {
    HDC dc = IDWriteBitmapRenderTarget_GetMemoryDC(target);
    check(dc != NULL, "its memory DC");
    check(IDWriteBitmapRenderTarget_GetSize(target, &size) == S_OK && size.cx == 64 && size.cy == 32, "its size");
    check(IDWriteBitmapRenderTarget_GetPixelsPerDip(target) == 1.0f, "pixels per dip");
    check_hr(IDWriteBitmapRenderTarget_Resize(target, 80, 40), S_OK, "resizing");
    check(IDWriteBitmapRenderTarget_GetSize(target, &size) == S_OK && size.cx == 80 && size.cy == 40, "the new size");
    const UINT32 cp = 'W';
    UINT16 g;
    DWRITE_GLYPH_RUN run;
    RECT box;
    float adv = 14.0f;
    IDWriteRenderingParams *rp = NULL;
    IDWriteFactory_CreateRenderingParams(g_factory, &rp);
    IDWriteFontFace_GetGlyphIndices(g_face, &cp, 1, &g);
    memset(&run, 0, sizeof(run));
    run.fontFace = g_face;
    run.fontEmSize = 20.0f;
    run.glyphCount = 1;
    run.glyphIndices = &g;
    run.glyphAdvances = &adv;
    check(IDWriteBitmapRenderTarget_DrawGlyphRun(target, 10.0f, 30.0f, DWRITE_MEASURING_MODE_NATURAL, &run, rp, RGB(255, 0, 0), &box) == S_OK && box.right - box.left > 8 && box.bottom - box.top > 8, "drawing a glyph run");
    check_hr(IDWriteBitmapRenderTarget_DrawGlyphRun(target, 10.0f, 30.0f, DWRITE_MEASURING_MODE_NATURAL, &run, NULL, RGB(255, 0, 0), &box), E_INVALIDARG, "drawing without parameters");
    IDWriteRenderingParams_Release(rp);
    IDWriteBitmapRenderTarget_Release(target);
  }
  check(IDWriteGdiInterop_CreateBitmapRenderTarget(gdi, NULL, 0, 0, &target) == S_OK && target, "a render target of size zero");
  if (target) IDWriteBitmapRenderTarget_Release(target);
  IDWriteGdiInterop_Release(gdi);
  check_hr(IDWriteFactory_CreateCustomFontCollection(g_factory, NULL, NULL, 0, &c), E_INVALIDARG, "no collection loader");
  check_hr(IDWriteFactory_RegisterFontFileLoader(g_factory, NULL), E_INVALIDARG, "registering nothing");
}

int main(void) {
  test_factory();
  if (!g_factory) { printf("dwritetest: no factory\n"); return 1; }
  test_collection();
  if (!g_system) return 1;
  test_family_and_font();
  if (!g_face) return 1;
  test_face();
  test_custom_file();
  test_text_format();
  test_layout();
  test_analyzer();
  test_glyph_run();
  test_misc();
  DeleteFileW(L"C:\\Temp\\dwtest.ttf");
  printf("dwritetest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed != 0;
}
