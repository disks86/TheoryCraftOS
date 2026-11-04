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

/* dwrite.dll: rendering parameters, typography, number substitution, the text format (and the properties a text layout shares with it), the ellipsis trimming sign, and
 * choosing the face for a family name. Included by dwrite.c. */

/* ================================================================ finding the face of a (family, weight, stretch, style) */
static Collection *collection_of(IDWriteFontCollection *c) { return c ? (Collection *)c : system_collection(); }

/* The family of that name in `c`, else in the system collection; else the system's default (Segoe UI, Arial). */
static int find_family(Collection *c, const WCHAR *name, Collection **found, UINT32 *index) {
  Collection *sys = system_collection();
  Collection *order[2] = {c ? c : sys, c ? sys : NULL};
  for (UINT32 k = 0; k < 2; ++k) {
    if (!order[k] || !name) continue;
    for (UINT32 i = 0; i < order[k]->count; ++i)
      if (wieq(order[k]->families[i].name, name)) { *found = order[k]; *index = i; return 1; }
  }
  return 0;
}
static int resolve_face(Collection *c, const WCHAR *name, UINT32 weight, UINT32 stretch, DWRITE_FONT_STYLE style, UINT32 *face) {
  Collection *found = NULL;
  UINT32 index = 0, order[16];
  if (!find_family(c, name, &found, &index) && !find_family(NULL, L"Segoe UI", &found, &index) && !find_family(NULL, L"Arial", &found, &index)) return 0;
  const FamilyEntry *e = &found->families[index];
  if (!e->count) return 0;
  match_order(e, weight, stretch, style, order);
  *face = e->faces[order[0]];
  return 1;
}
/* The face that stands in for characters a font does not have: the wide-coverage fallback face of the kernel. */
static int fallback_face(UINT32 *face) {
  Collection *sys = system_collection(), *found = NULL;
  UINT32 index = 0;
  if (!sys || !find_family(sys, L"TCOS Fallback", &found, &index) || !found->families[index].count) return 0;
  *face = found->families[index].faces[0];
  return 1;
}

/* ================================================================ rendering parameters */
typedef struct RenderParams { const IDWriteRenderingParamsVtbl *lpVtbl; LONG ref; float gamma, contrast, cleartype; DWRITE_PIXEL_GEOMETRY geometry; DWRITE_RENDERING_MODE mode; } RenderParams;
static void params_free(RenderParams *p) { mem_free(p); }
REF_METHODS(params, RenderParams, IDWriteRenderingParams, params_free)
static HRESULT WINAPI params_QueryInterface(IDWriteRenderingParams *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteRenderingParams, NULL, NULL); }
static FLOAT WINAPI params_GetGamma(IDWriteRenderingParams *This) { return ((RenderParams *)This)->gamma; }
static FLOAT WINAPI params_GetEnhancedContrast(IDWriteRenderingParams *This) { return ((RenderParams *)This)->contrast; }
static FLOAT WINAPI params_GetClearTypeLevel(IDWriteRenderingParams *This) { return ((RenderParams *)This)->cleartype; }
static DWRITE_PIXEL_GEOMETRY WINAPI params_GetPixelGeometry(IDWriteRenderingParams *This) { return ((RenderParams *)This)->geometry; }
static DWRITE_RENDERING_MODE WINAPI params_GetRenderingMode(IDWriteRenderingParams *This) { return ((RenderParams *)This)->mode; }
static const IDWriteRenderingParamsVtbl params_vtbl = {
  params_QueryInterface, params_AddRef, params_Release, params_GetGamma, params_GetEnhancedContrast, params_GetClearTypeLevel, params_GetPixelGeometry, params_GetRenderingMode,
};
static HRESULT params_create(float gamma, float contrast, float cleartype, DWRITE_PIXEL_GEOMETRY geometry, DWRITE_RENDERING_MODE mode, IDWriteRenderingParams **out) {
  *out = NULL;
  if (!(gamma > 0) || !(contrast >= 0) || !(cleartype >= 0 && cleartype <= 1.0f) || (UINT32)geometry > DWRITE_PIXEL_GEOMETRY_BGR || (UINT32)mode > DWRITE_RENDERING_MODE_OUTLINE)
    return E_INVALIDARG;
  RenderParams *p = OBJECT_NEW(RenderParams);
  if (!p) return E_OUTOFMEMORY;
  p->lpVtbl = &params_vtbl;
  p->ref = 1;
  p->gamma = gamma;
  p->contrast = contrast;
  p->cleartype = cleartype;
  p->geometry = geometry;
  p->mode = mode;
  *out = (IDWriteRenderingParams *)p;
  return S_OK;
}

/* ================================================================ typography */
typedef struct Typography { const IDWriteTypographyVtbl *lpVtbl; LONG ref; DWRITE_FONT_FEATURE *features; UINT32 count, capacity; } Typography;
static void typography_free(Typography *t) { mem_free(t->features); mem_free(t); }
REF_METHODS(typography, Typography, IDWriteTypography, typography_free)
static HRESULT WINAPI typography_QueryInterface(IDWriteTypography *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteTypography, NULL, NULL); }
static HRESULT WINAPI typography_AddFontFeature(IDWriteTypography *This, DWRITE_FONT_FEATURE feature) {
  Typography *t = (Typography *)This;
  if (t->count == t->capacity) {
    DWRITE_FONT_FEATURE *bigger = mem_grow(t->features, (t->capacity ? t->capacity * 2 : 4) * sizeof(*bigger));
    if (!bigger) return E_OUTOFMEMORY;
    t->features = bigger;
    t->capacity = t->capacity ? t->capacity * 2 : 4;
  }
  t->features[t->count++] = feature;
  return S_OK;
}
static UINT32 WINAPI typography_GetFontFeatureCount(IDWriteTypography *This) { return ((Typography *)This)->count; }
static HRESULT WINAPI typography_GetFontFeature(IDWriteTypography *This, UINT32 index, DWRITE_FONT_FEATURE *feature) {
  Typography *t = (Typography *)This;
  if (index >= t->count) return E_INVALIDARG;
  *feature = t->features[index];
  return S_OK;
}
static const IDWriteTypographyVtbl typography_vtbl = {
  typography_QueryInterface, typography_AddRef, typography_Release, typography_AddFontFeature, typography_GetFontFeatureCount, typography_GetFontFeature,
};
static HRESULT typography_create(IDWriteTypography **out) {
  Typography *t = OBJECT_NEW(Typography);
  if (!t) return E_OUTOFMEMORY;
  t->lpVtbl = &typography_vtbl;
  t->ref = 1;
  *out = (IDWriteTypography *)t;
  return S_OK;
}

/* ================================================================ number substitution */
typedef struct NumberSub { const IDWriteNumberSubstitutionVtbl *lpVtbl; LONG ref; } NumberSub;
static void numsub_free(NumberSub *n) { mem_free(n); }
REF_METHODS(numsub, NumberSub, IDWriteNumberSubstitution, numsub_free)
static HRESULT WINAPI numsub_QueryInterface(IDWriteNumberSubstitution *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteNumberSubstitution, NULL, NULL); }
static const IDWriteNumberSubstitutionVtbl numsub_vtbl = { numsub_QueryInterface, numsub_AddRef, numsub_Release };
static HRESULT numsub_create(IDWriteNumberSubstitution **out) {
  NumberSub *n = OBJECT_NEW(NumberSub);
  if (!n) return E_OUTOFMEMORY;
  n->lpVtbl = &numsub_vtbl;
  n->ref = 1;
  *out = (IDWriteNumberSubstitution *)n;
  return S_OK;
}

/* ================================================================ text format */
/* The properties of a text format. A text layout has the same (it is a text format too), so the IDWriteTextFormat methods below work on any object that starts like
 * TextFormat: the vtable pointer, the reference count and the properties. */
typedef struct FormatProps {
  WCHAR *family, *locale;
  IDWriteFontCollection *collection;      /* referenced; NULL = the system's */
  DWRITE_FONT_WEIGHT weight;
  DWRITE_FONT_STYLE style;
  DWRITE_FONT_STRETCH stretch;
  float size;
  DWRITE_TEXT_ALIGNMENT alignment;
  DWRITE_PARAGRAPH_ALIGNMENT paragraph;
  DWRITE_WORD_WRAPPING wrapping;
  DWRITE_READING_DIRECTION reading;
  DWRITE_FLOW_DIRECTION flow;
  float tab_stop;      /* 0: four spaces */
  DWRITE_TRIMMING trimming;
  IDWriteInlineObject *trim_sign;
  DWRITE_LINE_SPACING_METHOD spacing_method;
  float line_spacing, baseline;
  int dirty;      /* a layout rebuilds its lines when this is set */
} FormatProps;
typedef struct TextFormat { const IDWriteTextFormatVtbl *lpVtbl; LONG ref; FormatProps p; } TextFormat;
typedef struct FormatHeader { const void *vtbl; LONG ref; FormatProps p; } FormatHeader;
#define PROPS(This) (&((FormatHeader *)(This))->p)

static void props_release(FormatProps *p) {
  mem_free(p->family);
  mem_free(p->locale);
  release(p->collection);
  release(p->trim_sign);
}
static void format_free(TextFormat *f) { props_release(&f->p); mem_free(f); }
REF_METHODS(format, TextFormat, IDWriteTextFormat, format_free)
static HRESULT WINAPI format_QueryInterface(IDWriteTextFormat *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteTextFormat, NULL, NULL); }

static HRESULT WINAPI tf_SetTextAlignment(IDWriteTextFormat *This, DWRITE_TEXT_ALIGNMENT v) {
  if ((UINT32)v > DWRITE_TEXT_ALIGNMENT_JUSTIFIED) return E_INVALIDARG;
  PROPS(This)->alignment = v; PROPS(This)->dirty = 1; return S_OK;
}
static HRESULT WINAPI tf_SetParagraphAlignment(IDWriteTextFormat *This, DWRITE_PARAGRAPH_ALIGNMENT v) {
  if ((UINT32)v > DWRITE_PARAGRAPH_ALIGNMENT_CENTER) return E_INVALIDARG;
  PROPS(This)->paragraph = v; PROPS(This)->dirty = 1; return S_OK;
}
static HRESULT WINAPI tf_SetWordWrapping(IDWriteTextFormat *This, DWRITE_WORD_WRAPPING v) {
  if ((UINT32)v > DWRITE_WORD_WRAPPING_NO_WRAP) return E_INVALIDARG;
  PROPS(This)->wrapping = v; PROPS(This)->dirty = 1; return S_OK;
}
static HRESULT WINAPI tf_SetReadingDirection(IDWriteTextFormat *This, DWRITE_READING_DIRECTION v) {
  if ((UINT32)v > DWRITE_READING_DIRECTION_RIGHT_TO_LEFT) return E_INVALIDARG;
  PROPS(This)->reading = v; PROPS(This)->dirty = 1; return S_OK;
}
static HRESULT WINAPI tf_SetFlowDirection(IDWriteTextFormat *This, DWRITE_FLOW_DIRECTION v) {
  if ((UINT32)v > DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM) return E_INVALIDARG;
  PROPS(This)->flow = v; PROPS(This)->dirty = 1; return S_OK;
}
static HRESULT WINAPI tf_SetIncrementalTabStop(IDWriteTextFormat *This, FLOAT v) {
  if (!(v > 0)) return E_INVALIDARG;
  PROPS(This)->tab_stop = v; PROPS(This)->dirty = 1; return S_OK;
}
static HRESULT WINAPI tf_SetTrimming(IDWriteTextFormat *This, const DWRITE_TRIMMING *t, IDWriteInlineObject *sign) {
  FormatProps *p = PROPS(This);
  if (!t || (UINT32)t->granularity > DWRITE_TRIMMING_GRANULARITY_WORD) return E_INVALIDARG;
  add_ref(sign);
  release(p->trim_sign);
  p->trim_sign = sign;
  p->trimming = *t;
  p->dirty = 1;
  return S_OK;
}
static HRESULT WINAPI tf_SetLineSpacing(IDWriteTextFormat *This, DWRITE_LINE_SPACING_METHOD method, FLOAT spacing, FLOAT baseline) {
  FormatProps *p = PROPS(This);
  if ((UINT32)method > DWRITE_LINE_SPACING_METHOD_UNIFORM || spacing < 0 || baseline < 0 || !f_finite(spacing) || !f_finite(baseline)) return E_INVALIDARG;
  p->spacing_method = method;
  p->line_spacing = spacing;
  p->baseline = baseline;
  p->dirty = 1;
  return S_OK;
}
static DWRITE_TEXT_ALIGNMENT WINAPI tf_GetTextAlignment(IDWriteTextFormat *This) { return PROPS(This)->alignment; }
static DWRITE_PARAGRAPH_ALIGNMENT WINAPI tf_GetParagraphAlignment(IDWriteTextFormat *This) { return PROPS(This)->paragraph; }
static DWRITE_WORD_WRAPPING WINAPI tf_GetWordWrapping(IDWriteTextFormat *This) { return PROPS(This)->wrapping; }
static DWRITE_READING_DIRECTION WINAPI tf_GetReadingDirection(IDWriteTextFormat *This) { return PROPS(This)->reading; }
static DWRITE_FLOW_DIRECTION WINAPI tf_GetFlowDirection(IDWriteTextFormat *This) { return PROPS(This)->flow; }
static FLOAT WINAPI tf_GetIncrementalTabStop(IDWriteTextFormat *This) { return PROPS(This)->tab_stop; }
static HRESULT WINAPI tf_GetTrimming(IDWriteTextFormat *This, DWRITE_TRIMMING *t, IDWriteInlineObject **sign) {
  FormatProps *p = PROPS(This);
  *t = p->trimming;
  *sign = p->trim_sign;
  add_ref(*sign);
  return S_OK;
}
static HRESULT WINAPI tf_GetLineSpacing(IDWriteTextFormat *This, DWRITE_LINE_SPACING_METHOD *method, FLOAT *spacing, FLOAT *baseline) {
  FormatProps *p = PROPS(This);
  *method = p->spacing_method;
  *spacing = p->line_spacing;
  *baseline = p->baseline;
  return S_OK;
}
static HRESULT WINAPI tf_GetFontCollection(IDWriteTextFormat *This, IDWriteFontCollection **c) {
  FormatProps *p = PROPS(This);
  *c = p->collection ? p->collection : (IDWriteFontCollection *)system_collection();
  add_ref(*c);
  return S_OK;
}
static UINT32 WINAPI tf_GetFontFamilyNameLength(IDWriteTextFormat *This) { return (UINT32)wlen(PROPS(This)->family); }
static HRESULT copy_wide(const WCHAR *from, WCHAR *to, UINT32 size) {
  if (!to) return E_INVALIDARG;
  if (size < wlen(from) + 1) { if (size) to[0] = 0; return E_NOT_SUFFICIENT_BUFFER; }
  memcpy(to, from, (wlen(from) + 1) * sizeof(WCHAR));
  return S_OK;
}
static HRESULT WINAPI tf_GetFontFamilyName(IDWriteTextFormat *This, WCHAR *name, UINT32 size) { return copy_wide(PROPS(This)->family, name, size); }
static DWRITE_FONT_WEIGHT WINAPI tf_GetFontWeight(IDWriteTextFormat *This) { return PROPS(This)->weight; }
static DWRITE_FONT_STYLE WINAPI tf_GetFontStyle(IDWriteTextFormat *This) { return PROPS(This)->style; }
static DWRITE_FONT_STRETCH WINAPI tf_GetFontStretch(IDWriteTextFormat *This) { return PROPS(This)->stretch; }
static FLOAT WINAPI tf_GetFontSize(IDWriteTextFormat *This) { return PROPS(This)->size; }
static UINT32 WINAPI tf_GetLocaleNameLength(IDWriteTextFormat *This) { return (UINT32)wlen(PROPS(This)->locale); }
static HRESULT WINAPI tf_GetLocaleName(IDWriteTextFormat *This, WCHAR *name, UINT32 size) { return copy_wide(PROPS(This)->locale, name, size); }

static const IDWriteTextFormatVtbl format_vtbl = {
  format_QueryInterface, format_AddRef, format_Release, tf_SetTextAlignment, tf_SetParagraphAlignment, tf_SetWordWrapping, tf_SetReadingDirection, tf_SetFlowDirection,
  tf_SetIncrementalTabStop, tf_SetTrimming, tf_SetLineSpacing, tf_GetTextAlignment, tf_GetParagraphAlignment, tf_GetWordWrapping, tf_GetReadingDirection, tf_GetFlowDirection,
  tf_GetIncrementalTabStop, tf_GetTrimming, tf_GetLineSpacing, tf_GetFontCollection, tf_GetFontFamilyNameLength, tf_GetFontFamilyName, tf_GetFontWeight, tf_GetFontStyle,
  tf_GetFontStretch, tf_GetFontSize, tf_GetLocaleNameLength, tf_GetLocaleName,
};

static int weight_ok(UINT32 w) { return w >= 1 && w <= 999; }
static int style_ok(UINT32 s) { return s <= DWRITE_FONT_STYLE_ITALIC; }
static int stretch_ok(UINT32 s) { return s <= DWRITE_FONT_STRETCH_ULTRA_EXPANDED; }

/* Fills the properties (everything copied; the collection is referenced). */
static HRESULT props_init(FormatProps *p, const WCHAR *family, IDWriteFontCollection *collection, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch,
                          float size, const WCHAR *locale) {
  memset(p, 0, sizeof(*p));
  p->family = wdup(family);
  p->locale = wdup(locale);
  if (!p->family || !p->locale) { props_release(p); return E_OUTOFMEMORY; }
  p->collection = collection;
  add_ref(collection);
  p->weight = weight;
  p->style = style;
  p->stretch = stretch;
  p->size = size;
  p->tab_stop = size * 4.0f;
  p->trimming.granularity = DWRITE_TRIMMING_GRANULARITY_NONE;
  return S_OK;
}
static HRESULT format_create(const WCHAR *family, IDWriteFontCollection *collection, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, float size,
                             const WCHAR *locale, IDWriteTextFormat **out) {
  *out = NULL;
  if (!family || !locale || !(size > 0) || !f_finite(size) || !weight_ok(weight) || !style_ok(style) || !stretch_ok(stretch) || stretch == 0) return E_INVALIDARG;
  TextFormat *f = OBJECT_NEW(TextFormat);
  if (!f) return E_OUTOFMEMORY;
  const HRESULT hr = props_init(&f->p, family, collection, weight, style, stretch, size, locale);
  if (FAILED(hr)) { mem_free(f); return hr; }
  f->lpVtbl = &format_vtbl;
  f->ref = 1;
  *out = (IDWriteTextFormat *)f;
  return S_OK;
}

/* ================================================================ the ellipsis trimming sign (an inline object that draws "…") */
typedef struct EllipsisSign { const IDWriteInlineObjectVtbl *lpVtbl; LONG ref; UINT32 face; UINT16 glyph; float size; float width, ascent; } EllipsisSign;
static void sign_free(EllipsisSign *s) { mem_free(s); }
REF_METHODS(sign, EllipsisSign, IDWriteInlineObject, sign_free)
static HRESULT WINAPI sign_QueryInterface(IDWriteInlineObject *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteInlineObject, NULL, NULL); }
static HRESULT WINAPI sign_Draw(IDWriteInlineObject *This, void *context, IDWriteTextRenderer *renderer, FLOAT x, FLOAT y, BOOL sideways, BOOL rtl, IUnknown *effect) {
  EllipsisSign *s = (EllipsisSign *)This;
  IDWriteFontFace *face = NULL;
  HRESULT hr = face_create(s->face, DWRITE_FONT_SIMULATIONS_NONE, &face);
  if (FAILED(hr)) return hr;
  const UINT16 glyph = s->glyph;
  const FLOAT advance = s->width;
  DWRITE_GLYPH_RUN run;
  memset(&run, 0, sizeof(run));
  run.fontFace = face;
  run.fontEmSize = s->size;
  run.glyphCount = 1;
  run.glyphIndices = &glyph;
  run.glyphAdvances = &advance;
  run.isSideways = sideways;
  run.bidiLevel = rtl ? 1 : 0;
  hr = renderer->lpVtbl->DrawGlyphRun(renderer, context, x, y + s->ascent, DWRITE_MEASURING_MODE_NATURAL, &run, NULL, effect);
  release(face);
  return hr;
}
static HRESULT WINAPI sign_GetMetrics(IDWriteInlineObject *This, DWRITE_INLINE_OBJECT_METRICS *m) {
  EllipsisSign *s = (EllipsisSign *)This;
  DWRITE_FONT_METRICS fm;
  memset(&fm, 0, sizeof(fm));
  fm.designUnitsPerEm = 1;
  face_info(s->face, NULL, &fm);
  m->width = s->width;
  m->height = (float)(fm.ascent + fm.descent) * s->size / (float)fm.designUnitsPerEm;
  m->baseline = s->ascent;
  m->supportsSideways = FALSE;
  return S_OK;
}
static HRESULT WINAPI sign_GetOverhangMetrics(IDWriteInlineObject *This, DWRITE_OVERHANG_METRICS *o) { (void)This; memset(o, 0, sizeof(*o)); return S_OK; }
static HRESULT WINAPI sign_GetBreakConditions(IDWriteInlineObject *This, DWRITE_BREAK_CONDITION *before, DWRITE_BREAK_CONDITION *after) {
  (void)This;
  *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
  return S_OK;
}
static const IDWriteInlineObjectVtbl sign_vtbl = { sign_QueryInterface, sign_AddRef, sign_Release, sign_Draw, sign_GetMetrics, sign_GetOverhangMetrics, sign_GetBreakConditions };

static HRESULT sign_create(const FormatProps *p, IDWriteInlineObject **out) {
  *out = NULL;
  UINT32 face = 0, cp = 0x2026;
  UINT16 glyph = 0;
  if (!resolve_face(collection_of(p->collection), p->family, p->weight, p->stretch, p->style, &face)) return E_FAIL;
  face_map(face, &cp, 1, &glyph);
  if (!glyph && fallback_face(&face)) face_map(face, &cp, 1, &glyph);
  EllipsisSign *s = OBJECT_NEW(EllipsisSign);
  if (!s) return E_OUTOFMEMORY;
  TcosGlyphBox box;
  DWRITE_FONT_METRICS fm;
  memset(&box, 0, sizeof(box));
  memset(&fm, 0, sizeof(fm));
  fm.designUnitsPerEm = 1;
  face_boxes(face, &glyph, 1, &box);
  face_info(face, NULL, &fm);
  s->lpVtbl = &sign_vtbl;
  s->ref = 1;
  s->face = face;
  s->glyph = glyph;
  s->size = p->size;
  s->width = (float)box.advance * p->size / (float)fm.designUnitsPerEm;
  s->ascent = (float)fm.ascent * p->size / (float)fm.designUnitsPerEm;
  *out = (IDWriteInlineObject *)s;
  return S_OK;
}
