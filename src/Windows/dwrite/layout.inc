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

/* dwrite.dll: the text layout. Text is split into clusters (a code point with the combining marks after it), each gets the face of its attributes (or the fallback face when
 * the font lacks the character), an advance from the font (and kerning from its kern table), then the lines are broken and positioned. Included by dwrite.c.
 * Left to right only: a right-to-left reading direction mirrors the alignment but the runs are not reordered. */

#define CL_SPACE 1          /* whitespace (it hangs at the end of a line) */
#define CL_NEWLINE 2        /* ends a line */
#define CL_TAB 4            /* advances to the next tab stop */
#define CL_BREAK_AFTER 8    /* a line may break after it */
#define CL_INLINE 16        /* an inline object */
#define CL_NOGLYPH 32       /* has no glyph (controls, zero-width characters) */
#define CL_SOFT_HYPHEN 64
#define CL_NOBREAK_SPACE 128

typedef struct CharAttr {
  UINT16 family, locale;      /* indices into the layout's name tables (0: the format's) */
  UINT16 weight;
  BYTE style, stretch, flags;      /* flags: 1 underline, 2 strikethrough */
  float size;
  IUnknown *effect;
  IDWriteInlineObject *inline_object;
  IDWriteTypography *typography;
  IDWriteFontCollection *collection;
} CharAttr;

typedef struct Cluster {
  UINT32 pos;      /* UTF-16 offset */
  UINT16 length;
  BYTE flags;
  UINT16 glyph;
  UINT32 face;
  float size, advance, ascent, descent, gap;
  float x;      /* inside its line, set when the line is laid out */
} Cluster;

typedef struct Line {
  UINT32 first, count;      /* clusters */
  UINT32 pos, length;      /* text units, including the trailing whitespace and the newline */
  UINT32 trailing_ws, newline;      /* units */
  float width, width_ws;      /* without / with the trailing whitespace */
  float x, y, height, baseline;
  float justify;      /* extra width added to each space of a justified line */
  UINT32 trim_at;      /* clusters from here on are replaced by the trimming sign (first + count: none) */
  float trim_x, sign_width;
  int trimmed;
} Line;

typedef struct Layout {
  const IDWriteTextLayoutVtbl *lpVtbl;
  LONG ref;
  FormatProps p;      /* (starts like a TextFormat: the IDWriteTextFormat methods work on it) */
  WCHAR *text;
  UINT32 length;
  float max_width, max_height;
  CharAttr *attrs;
  WCHAR **families, **locales;      /* name tables; entry 0 is unused (the format's) */
  UINT32 family_count, locale_count;
  IUnknown **pool;      /* references held for the attributes */
  UINT32 pool_count, pool_capacity;
  int built;
  Cluster *clusters;
  UINT32 cluster_count;
  Line *lines;
  UINT32 line_count;
  float top;      /* paragraph alignment offset */
  float metrics_width, metrics_width_ws, metrics_height, metrics_left;
  float min_width;
  float tab_stop;
  float sign_width;
  IDWriteInlineObject *sign;
} Layout;

static void layout_free_built(Layout *L) {
  mem_free(L->clusters);
  mem_free(L->lines);
  L->clusters = NULL;
  L->lines = NULL;
  L->cluster_count = L->line_count = 0;
  release(L->sign);
  L->sign = NULL;
  L->built = 0;
}
static void layout_free(Layout *L) {
  layout_free_built(L);
  for (UINT32 i = 0; i < L->pool_count; ++i) release(L->pool[i]);
  mem_free(L->pool);
  for (UINT32 i = 1; i < L->family_count; ++i) mem_free(L->families[i]);
  for (UINT32 i = 1; i < L->locale_count; ++i) mem_free(L->locales[i]);
  mem_free(L->families);
  mem_free(L->locales);
  mem_free(L->attrs);
  mem_free(L->text);
  props_release(&L->p);
  mem_free(L);
}
REF_METHODS(layout, Layout, IDWriteTextLayout, layout_free)
static HRESULT WINAPI layout_QueryInterface(IDWriteTextLayout *This, REFIID riid, void **out) {
  return query3(This, riid, out, &IID_IDWriteTextLayout, &IID_IDWriteTextFormat, NULL);
}

/* ---- classification of characters */
static int cp_newline(UINT32 c) { return c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029; }
static int cp_break_space(UINT32 c) { return c == ' ' || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) || (c >= 0x2008 && c <= 0x200A) || c == 0x205F || c == 0x3000; }
static int cp_nobreak_space(UINT32 c) { return c == 0xA0 || c == 0x2007 || c == 0x202F; }
static int cp_zero_width(UINT32 c) { return c == 0x200B || c == 0x200C || c == 0x200D || c == 0x200E || c == 0x200F || c == 0x2060 || c == 0xFEFF || c == 0xAD || (c >= 0x202A && c <= 0x202E); }
static int cp_combining(UINT32 c) { return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200D; }
static int cp_ideograph(UINT32 c) {
  return (c >= 0x2E80 && c <= 0x9FFF && !(c >= 0x3001 && c <= 0x3002)) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF21 && c <= 0xFF5A) || (c >= 0x20000 && c <= 0x2FFFF);
}
static int cp_no_break_before(UINT32 c) { return c == 0x3001 || c == 0x3002 || c == 0xFF0C || c == 0xFF0E || c == 0xFF09 || c == 0x300D || c == 0x300F || c == 0xFF01 || c == 0xFF1F; }
static int cp_alnum(UINT32 c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0xC0; }

/* The break opportunities (line break classes, simplified): after spaces, hyphens and dashes, zero width spaces, soft hyphens, and between ideographs. `next` is the code point
 * after the cluster (0 at the end). */
static int cp_break_after(UINT32 c, UINT32 next, UINT32 prev) {
  if (cp_break_space(c) || c == 0x200B || c == 0xAD || c == '\t') return 1;
  if (c == '-' || c == 0x2010 || c == 0x2012 || c == 0x2013 || c == 0x2014) return next && !cp_break_space(next) && next != '-' && (c != '-' || cp_alnum(prev)) && !(next >= '0' && next <= '9' && c == '-' && !cp_alnum(prev));
  if (cp_ideograph(c) && next && !cp_no_break_before(next)) return 1;
  return 0;
}

/* The code point at `pos` of UTF-16 text and the units it takes. */
static UINT32 decode16(const WCHAR *t, UINT32 length, UINT32 pos, UINT32 *units) {
  const UINT32 c = t[pos];
  *units = 1;
  if (c >= 0xD800 && c < 0xDC00 && pos + 1 < length && t[pos + 1] >= 0xDC00 && t[pos + 1] < 0xE000) { *units = 2; return 0x10000 + ((c - 0xD800) << 10) + (t[pos + 1] - 0xDC00); }
  if (c >= 0xD800 && c < 0xE000) return 0xFFFD;
  return c;
}

/* ---- attributes */
static CharAttr attr_default(const FormatProps *p) {
  CharAttr a;
  memset(&a, 0, sizeof(a));
  a.weight = (UINT16)p->weight;
  a.style = (BYTE)p->style;
  a.stretch = (BYTE)p->stretch;
  a.size = p->size;
  return a;
}
static const WCHAR *layout_family(const Layout *L, const CharAttr *a) { return a->family ? L->families[a->family] : L->p.family; }
static const WCHAR *layout_locale(const Layout *L, const CharAttr *a) { return a->locale ? L->locales[a->locale] : L->p.locale; }

static HRESULT layout_pool_add(Layout *L, IUnknown *object) {
  if (!object) return S_OK;
  if (L->pool_count == L->pool_capacity) {
    IUnknown **bigger = mem_grow(L->pool, (L->pool_capacity ? L->pool_capacity * 2 : 8) * sizeof(*bigger));
    if (!bigger) return E_OUTOFMEMORY;
    L->pool = bigger;
    L->pool_capacity = L->pool_capacity ? L->pool_capacity * 2 : 8;
  }
  add_ref(object);
  L->pool[L->pool_count++] = object;
  return S_OK;
}
static HRESULT layout_name_add(WCHAR ***table, UINT32 *count, const WCHAR *name, UINT16 *index) {
  for (UINT32 i = 1; i < *count; ++i)
    if (wieq((*table)[i], name)) { *index = (UINT16)i; return S_OK; }
  if (*count >= 0xFFFF) return E_OUTOFMEMORY;
  WCHAR **bigger = mem_grow(*table, (*count + 1) * sizeof(WCHAR *));
  if (!bigger) return E_OUTOFMEMORY;
  *table = bigger;
  if (*count == 0) { (*table)[0] = NULL; *count = 1; }
  (*table)[*count] = wdup(name);
  if (!(*table)[*count]) return E_OUTOFMEMORY;
  *index = (UINT16)*count;
  ++*count;
  return S_OK;
}
/* Clamps a program's text range to the text; false when it lies past the end (nothing to do). */
static int range_clamp(const Layout *L, DWRITE_TEXT_RANGE r, UINT32 *begin, UINT32 *end) {
  if (r.startPosition >= L->length) return 0;
  const UINT64 e = (UINT64)r.startPosition + r.length;
  *begin = r.startPosition;
  *end = e > L->length ? L->length : (UINT32)e;
  return 1;
}
static HRESULT layout_set_range_check(Layout *L, DWRITE_TEXT_RANGE range, UINT32 *b, UINT32 *e) {
  if (!range_clamp(L, range, b, e)) return S_OK;      /* S_OK: nothing to change */
  return S_FALSE;      /* S_FALSE: go on */
}

static HRESULT WINAPI layout_SetMaxWidth(IDWriteTextLayout *This, FLOAT v) {
  Layout *L = (Layout *)This;
  if (!(v >= 0) || v != v) return E_INVALIDARG;
  L->max_width = v; L->p.dirty = 1; return S_OK;
}
static HRESULT WINAPI layout_SetMaxHeight(IDWriteTextLayout *This, FLOAT v) {
  Layout *L = (Layout *)This;
  if (!(v >= 0) || v != v) return E_INVALIDARG;
  L->max_height = v; L->p.dirty = 1; return S_OK;
}
static HRESULT WINAPI layout_SetFontCollection(IDWriteTextLayout *This, IDWriteFontCollection *c, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  HRESULT hr = layout_pool_add(L, (IUnknown *)c);
  if (FAILED(hr)) return hr;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].collection = c;
  L->p.dirty = 1;
  return S_OK;
}
static HRESULT WINAPI layout_SetFontFamilyName(IDWriteTextLayout *This, const WCHAR *name, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  UINT16 index = 0;
  if (!name) return E_INVALIDARG;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  HRESULT hr = layout_name_add(&L->families, &L->family_count, name, &index);
  if (FAILED(hr)) return hr;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].family = index;
  L->p.dirty = 1;
  return S_OK;
}
static HRESULT WINAPI layout_SetFontWeight(IDWriteTextLayout *This, DWRITE_FONT_WEIGHT v, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  if (!weight_ok(v)) return E_INVALIDARG;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].weight = (UINT16)v;
  L->p.dirty = 1;
  return S_OK;
}
static HRESULT WINAPI layout_SetFontStyle(IDWriteTextLayout *This, DWRITE_FONT_STYLE v, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  if (!style_ok(v)) return E_INVALIDARG;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].style = (BYTE)v;
  L->p.dirty = 1;
  return S_OK;
}
static HRESULT WINAPI layout_SetFontStretch(IDWriteTextLayout *This, DWRITE_FONT_STRETCH v, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  if (!stretch_ok(v) || v == 0) return E_INVALIDARG;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].stretch = (BYTE)v;
  L->p.dirty = 1;
  return S_OK;
}
static HRESULT WINAPI layout_SetFontSize(IDWriteTextLayout *This, FLOAT v, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  if (!(v > 0) || !f_finite(v)) return E_INVALIDARG;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].size = v;
  L->p.dirty = 1;
  return S_OK;
}
static HRESULT layout_set_flag(Layout *L, BOOL on, BYTE flag, DWRITE_TEXT_RANGE range) {
  UINT32 b, e;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].flags = on ? (BYTE)(L->attrs[i].flags | flag) : (BYTE)(L->attrs[i].flags & ~flag);
  L->p.dirty = 1;
  return S_OK;
}
static HRESULT WINAPI layout_SetUnderline(IDWriteTextLayout *This, BOOL v, DWRITE_TEXT_RANGE range) { return layout_set_flag((Layout *)This, v, 1, range); }
static HRESULT WINAPI layout_SetStrikethrough(IDWriteTextLayout *This, BOOL v, DWRITE_TEXT_RANGE range) { return layout_set_flag((Layout *)This, v, 2, range); }
static HRESULT WINAPI layout_SetDrawingEffect(IDWriteTextLayout *This, IUnknown *effect, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  HRESULT hr = layout_pool_add(L, effect);
  if (FAILED(hr)) return hr;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].effect = effect;
  return S_OK;
}
static HRESULT WINAPI layout_SetInlineObject(IDWriteTextLayout *This, IDWriteInlineObject *object, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  HRESULT hr = layout_pool_add(L, (IUnknown *)object);
  if (FAILED(hr)) return hr;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].inline_object = object;
  L->p.dirty = 1;
  return S_OK;
}
static HRESULT WINAPI layout_SetTypography(IDWriteTextLayout *This, IDWriteTypography *typography, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  HRESULT hr = layout_pool_add(L, (IUnknown *)typography);
  if (FAILED(hr)) return hr;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].typography = typography;
  return S_OK;
}
static HRESULT WINAPI layout_SetLocaleName(IDWriteTextLayout *This, const WCHAR *name, DWRITE_TEXT_RANGE range) {
  Layout *L = (Layout *)This;
  UINT32 b, e;
  UINT16 index = 0;
  if (!name) return E_INVALIDARG;
  if (layout_set_range_check(L, range, &b, &e) == S_OK) return S_OK;
  HRESULT hr = layout_name_add(&L->locales, &L->locale_count, name, &index);
  if (FAILED(hr)) return hr;
  for (UINT32 i = b; i < e; ++i) L->attrs[i].locale = index;
  return S_OK;
}
static FLOAT WINAPI layout_GetMaxWidth(IDWriteTextLayout *This) { return ((Layout *)This)->max_width; }
static FLOAT WINAPI layout_GetMaxHeight(IDWriteTextLayout *This) { return ((Layout *)This)->max_height; }

/* The getters: the value at `pos` and the range around it that has the same value. A position past the text gets the format's value and a range from the end of the text. */
typedef int (*AttrSame)(const Layout *L, const CharAttr *a, const CharAttr *b);
static void attr_range(const Layout *L, UINT32 pos, AttrSame same, DWRITE_TEXT_RANGE *range) {
  if (pos >= L->length) { range->startPosition = L->length; range->length = 0xFFFFFFFFu - L->length; return; }
  UINT32 b = pos, e = pos + 1;
  while (b > 0 && same(L, &L->attrs[b - 1], &L->attrs[pos])) --b;
  while (e < L->length && same(L, &L->attrs[e], &L->attrs[pos])) ++e;
  range->startPosition = b;
  range->length = e - b;
}
static CharAttr attr_at(const Layout *L, UINT32 pos) { return pos < L->length ? L->attrs[pos] : attr_default(&L->p); }
static int same_family(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->family == b->family; }
static int same_weight(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->weight == b->weight; }
static int same_style(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->style == b->style; }
static int same_stretch(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->stretch == b->stretch; }
static int same_size(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->size == b->size; }
static int same_underline(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return (a->flags & 1) == (b->flags & 1); }
static int same_strike(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return (a->flags & 2) == (b->flags & 2); }
static int same_effect(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->effect == b->effect; }
static int same_inline(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->inline_object == b->inline_object; }
static int same_typography(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->typography == b->typography; }
static int same_collection(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->collection == b->collection; }
static int same_locale(const Layout *L, const CharAttr *a, const CharAttr *b) { (void)L; return a->locale == b->locale; }

static HRESULT WINAPI layout_GetFontCollection_(IDWriteTextLayout *This, UINT32 pos, IDWriteFontCollection **c, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  const CharAttr a = attr_at(L, pos);
  *c = a.collection ? a.collection : L->p.collection ? L->p.collection : (IDWriteFontCollection *)system_collection();
  add_ref(*c);
  if (range) attr_range(L, pos, same_collection, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetFontFamilyNameLength_(IDWriteTextLayout *This, UINT32 pos, UINT32 *length, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  const CharAttr a = attr_at(L, pos);
  *length = (UINT32)wlen(layout_family(L, &a));
  if (range) attr_range(L, pos, same_family, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetFontFamilyName_(IDWriteTextLayout *This, UINT32 pos, WCHAR *name, UINT32 size, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  const CharAttr a = attr_at(L, pos);
  if (range) attr_range(L, pos, same_family, range);
  return copy_wide(layout_family(L, &a), name, size);
}
static HRESULT WINAPI layout_GetFontWeight_(IDWriteTextLayout *This, UINT32 pos, DWRITE_FONT_WEIGHT *v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = (DWRITE_FONT_WEIGHT)attr_at(L, pos).weight;
  if (range) attr_range(L, pos, same_weight, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetFontStyle_(IDWriteTextLayout *This, UINT32 pos, DWRITE_FONT_STYLE *v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = (DWRITE_FONT_STYLE)attr_at(L, pos).style;
  if (range) attr_range(L, pos, same_style, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetFontStretch_(IDWriteTextLayout *This, UINT32 pos, DWRITE_FONT_STRETCH *v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = (DWRITE_FONT_STRETCH)attr_at(L, pos).stretch;
  if (range) attr_range(L, pos, same_stretch, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetFontSize_(IDWriteTextLayout *This, UINT32 pos, FLOAT *v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = attr_at(L, pos).size;
  if (range) attr_range(L, pos, same_size, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetUnderline(IDWriteTextLayout *This, UINT32 pos, BOOL *v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = (attr_at(L, pos).flags & 1) != 0;
  if (range) attr_range(L, pos, same_underline, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetStrikethrough(IDWriteTextLayout *This, UINT32 pos, BOOL *v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = (attr_at(L, pos).flags & 2) != 0;
  if (range) attr_range(L, pos, same_strike, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetDrawingEffect(IDWriteTextLayout *This, UINT32 pos, IUnknown **v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = attr_at(L, pos).effect;
  add_ref(*v);
  if (range) attr_range(L, pos, same_effect, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetInlineObject(IDWriteTextLayout *This, UINT32 pos, IDWriteInlineObject **v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = attr_at(L, pos).inline_object;
  add_ref(*v);
  if (range) attr_range(L, pos, same_inline, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetTypography(IDWriteTextLayout *This, UINT32 pos, IDWriteTypography **v, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  *v = attr_at(L, pos).typography;
  add_ref(*v);
  if (range) attr_range(L, pos, same_typography, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetLocaleNameLength_(IDWriteTextLayout *This, UINT32 pos, UINT32 *length, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  const CharAttr a = attr_at(L, pos);
  *length = (UINT32)wlen(layout_locale(L, &a));
  if (range) attr_range(L, pos, same_locale, range);
  return S_OK;
}
static HRESULT WINAPI layout_GetLocaleName_(IDWriteTextLayout *This, UINT32 pos, WCHAR *name, UINT32 size, DWRITE_TEXT_RANGE *range) {
  Layout *L = (Layout *)This;
  const CharAttr a = attr_at(L, pos);
  if (range) attr_range(L, pos, same_locale, range);
  return copy_wide(layout_locale(L, &a), name, size);
}

/* ---- building: clusters, faces, advances */
typedef struct FaceKey { UINT32 family_ptr_hash; const WCHAR *family; IDWriteFontCollection *collection; UINT16 weight; BYTE style, stretch; UINT32 face; int valid; } FaceKey;

static HRESULT layout_shape(Layout *L) {
  /* 1. clusters */
  Cluster *cl = mem_array((SIZE_T)L->length + 1, sizeof(Cluster));
  UINT32 *cps = mem_array((SIZE_T)L->length + 1, sizeof(UINT32));
  UINT16 *glyphs = mem_array((SIZE_T)L->length + 1, sizeof(UINT16));
  if (!cl || !cps || !glyphs) { mem_free(cl); mem_free(cps); mem_free(glyphs); return E_OUTOFMEMORY; }
  UINT32 n = 0;
  for (UINT32 pos = 0; pos < L->length;) {
    UINT32 units;
    const UINT32 c = decode16(L->text, L->length, pos, &units);
    if (n > 0 && cp_combining(c) && !(cl[n - 1].flags & (CL_NEWLINE | CL_INLINE))) {      /* a mark joins the cluster before it */
      cl[n - 1].length = (UINT16)(cl[n - 1].length + units);
      pos += units;
      continue;
    }
    Cluster *k = &cl[n];
    k->pos = pos;
    k->length = (UINT16)units;
    cps[n] = c;
    if (cp_newline(c)) {
      k->flags |= CL_NEWLINE | CL_NOGLYPH;
      if (c == '\r' && pos + 1 < L->length && L->text[pos + 1] == '\n') k->length = 2;
    } else if (c == '\t') k->flags |= CL_TAB | CL_SPACE | CL_NOGLYPH;
    else if (cp_break_space(c)) k->flags |= CL_SPACE;
    else if (cp_nobreak_space(c)) k->flags |= CL_SPACE | CL_NOBREAK_SPACE;
    else if (cp_zero_width(c)) k->flags |= CL_NOGLYPH | (c == 0xAD ? CL_SOFT_HYPHEN : 0);
    if (L->attrs[pos].inline_object) k->flags |= CL_INLINE;
    pos += k->length;
    ++n;
  }
  /* 2. faces: resolved per run of equal attributes */
  FaceKey key;
  memset(&key, 0, sizeof(key));
  for (UINT32 i = 0; i < n; ++i) {
    const CharAttr *a = &L->attrs[cl[i].pos];
    const WCHAR *family = layout_family(L, a);
    cl[i].size = a->size;
    if (cl[i].flags & CL_INLINE) continue;
    if (!(key.valid && key.family == family && key.collection == a->collection && key.weight == a->weight && key.style == a->style && key.stretch == a->stretch)) {
      IDWriteFontCollection *coll = a->collection ? a->collection : L->p.collection;
      key.valid = resolve_face(collection_of(coll), family, a->weight, a->stretch, (DWRITE_FONT_STYLE)a->style, &key.face);
      key.family = family;
      key.collection = a->collection;
      key.weight = a->weight;
      key.style = a->style;
      key.stretch = a->stretch;
    }
    if (!key.valid) { mem_free(cl); mem_free(cps); mem_free(glyphs); return E_FAIL; }
    cl[i].face = key.face;
  }
  /* 3. glyphs, a run of the same face at a time */
  for (UINT32 i = 0; i < n;) {
    UINT32 j = i;
    while (j < n && cl[j].face == cl[i].face && !(cl[i].flags & CL_INLINE) && !(cl[j].flags & CL_INLINE)) ++j;
    if (j == i) { ++i; continue; }
    face_map(cl[i].face, cps + i, j - i, glyphs + i);
    for (UINT32 k = i; k < j; ++k) cl[k].glyph = (cl[k].flags & CL_NOGLYPH) ? 0 : glyphs[k];
    i = j;
  }
  UINT32 fallback = 0;
  const int have_fallback = fallback_face(&fallback);
  for (UINT32 i = 0; i < n; ++i) {      /* what the font lacks comes from the fallback face */
    if (cl[i].glyph || (cl[i].flags & (CL_NOGLYPH | CL_INLINE)) || !have_fallback || cl[i].face == fallback) continue;
    UINT16 g = 0;
    face_map(fallback, &cps[i], 1, &g);
    if (g) { cl[i].face = fallback; cl[i].glyph = g; }
  }
  mem_free(glyphs);
  /* 4. advances, metrics, break opportunities */
  for (UINT32 i = 0; i < n; ++i) {
    Cluster *k = &cl[i];
    if (k->flags & CL_INLINE) {
      DWRITE_INLINE_OBJECT_METRICS m;
      memset(&m, 0, sizeof(m));
      L->attrs[k->pos].inline_object->lpVtbl->GetMetrics(L->attrs[k->pos].inline_object, &m);
      k->advance = m.width;
      k->ascent = m.baseline;
      k->descent = m.height - m.baseline;
      continue;
    }
    DWRITE_FONT_METRICS fm;
    if (!face_info(k->face, NULL, &fm) || !fm.designUnitsPerEm) continue;
    const float scale = k->size / (float)fm.designUnitsPerEm;
    k->ascent = (float)fm.ascent * scale;
    k->descent = (float)fm.descent * scale;
    k->gap = (float)fm.lineGap * scale;
    if (k->flags & (CL_NOGLYPH | CL_NEWLINE)) continue;
    if (k->glyph == 0 && (cps[i] == 0xFFFD || cps[i] == 0)) { }
    UINT16 g = k->glyph;
    TcosGlyphBox box;
    if (!face_boxes(k->face, &g, 1, &box)) continue;
    k->advance = (float)box.advance * scale;
    if (i > 0 && cl[i - 1].face == k->face && cl[i - 1].glyph && k->glyph && !(cl[i - 1].flags & (CL_INLINE | CL_NOGLYPH)) && cl[i - 1].size == k->size)
      cl[i - 1].advance += (float)face_kerning(k->face, cl[i - 1].glyph, k->glyph) * scale;
  }
  for (UINT32 i = 0; i < n; ++i) {
    Cluster *k = &cl[i];
    const UINT32 next = i + 1 < n ? cps[i + 1] : 0, prev = i > 0 ? cps[i - 1] : 0;
    if (!(k->flags & CL_NOBREAK_SPACE) && cp_break_after(cps[i], next, prev)) k->flags |= CL_BREAK_AFTER;
    if ((k->flags & CL_INLINE)) {
      IDWriteInlineObject *o = L->attrs[k->pos].inline_object;
      DWRITE_BREAK_CONDITION before = DWRITE_BREAK_CONDITION_NEUTRAL, after = DWRITE_BREAK_CONDITION_NEUTRAL;
      o->lpVtbl->GetBreakConditions(o, &before, &after);
      if (after == DWRITE_BREAK_CONDITION_CAN_BREAK) k->flags |= CL_BREAK_AFTER;
      else if (after == DWRITE_BREAK_CONDITION_MAY_NOT_BREAK) k->flags &= ~CL_BREAK_AFTER;
    }
  }
  mem_free(cps);
  L->clusters = cl;
  L->cluster_count = n;
  return S_OK;
}

static float cluster_tab_advance(const Layout *L, float x) {
  const float stop = L->tab_stop > 0 ? L->tab_stop : 32.0f;
  const float next = (f_floor(x / stop) + 1.0f) * stop;
  return next - x;
}

/* The width of the ellipsis for trimming (the program's sign or the default one made from the format). */
static void layout_prepare_sign(Layout *L) {
  L->sign = NULL;
  L->sign_width = 0;
  if (L->p.trimming.granularity == DWRITE_TRIMMING_GRANULARITY_NONE) return;
  IDWriteInlineObject *sign = L->p.trim_sign;
  if (sign) add_ref(sign);
  else if (FAILED(sign_create(&L->p, &sign))) return;
  DWRITE_INLINE_OBJECT_METRICS m;
  memset(&m, 0, sizeof(m));
  sign->lpVtbl->GetMetrics(sign, &m);
  L->sign = sign;
  L->sign_width = m.width;
}

static HRESULT layout_lines(Layout *L) {
  const UINT32 n = L->cluster_count;
  UINT32 capacity = 4;
  Line *lines = mem_array(capacity, sizeof(Line));
  if (!lines) return E_OUTOFMEMORY;
  UINT32 count = 0;
  const int wraps = L->p.wrapping != DWRITE_WORD_WRAPPING_NO_WRAP && L->max_width < BIG_WIDTH;
  const float avail = wraps || L->p.trimming.granularity != DWRITE_TRIMMING_GRANULARITY_NONE ? L->max_width : BIG_WIDTH;
  L->tab_stop = L->p.tab_stop;
  layout_prepare_sign(L);
  UINT32 s = 0;
  do {
    float x = 0;
    int last_break = -1;
    UINT32 i = s;
    int mandatory = 0;
    UINT32 end = s;      /* one past the last cluster of this line */
    for (; i < n; ++i) {
      Cluster *k = &L->clusters[i];
      if (k->flags & CL_TAB) k->advance = cluster_tab_advance(L, x);
      if (k->flags & CL_NEWLINE) { k->x = x; end = i + 1; mandatory = 1; break; }
      const int hangs = (k->flags & CL_SPACE) != 0;
      if (wraps && !hangs && i > s && x + k->advance > avail + 0.001f) {
        if (L->p.wrapping == DWRITE_WORD_WRAPPING_CHARACTER || last_break < 0) {
          if (L->p.wrapping != DWRITE_WORD_WRAPPING_WHOLE_WORD || last_break >= 0) { end = i; break; }
        } else { end = (UINT32)last_break + 1; break; }
      }
      k->x = x;
      x += k->advance;
      if (k->flags & CL_BREAK_AFTER) last_break = (int)i;
      end = i + 1;
    }
    if (end == s && s < n) end = s + 1;      /* (always make progress) */
    if (i == n && !mandatory) end = n;
    if (count == capacity) {
      Line *bigger = mem_grow(lines, capacity * 2 * sizeof(Line));
      if (!bigger) { mem_free(lines); return E_OUTOFMEMORY; }
      lines = bigger;
      capacity *= 2;
    }
    Line *ln = &lines[count++];
    memset(ln, 0, sizeof(*ln));
    ln->first = s;
    ln->count = end - s;
    ln->trim_at = end;
    /* positions inside the line, from the clusters kept */
    float cx = 0;
    UINT32 trailing = 0;
    float ascent = 0, descent = 0, gap = 0;
    for (UINT32 j = s; j < end; ++j) {
      Cluster *k = &L->clusters[j];
      if (k->flags & CL_TAB) k->advance = cluster_tab_advance(L, cx);
      k->x = cx;
      if (!(k->flags & CL_NEWLINE)) cx += k->advance;
      if (k->ascent > ascent) ascent = k->ascent;
      if (k->descent > descent) descent = k->descent;
      if (k->gap > gap) gap = k->gap;
    }
    UINT32 newline_units = 0;
    UINT32 last = end;
    if (end > s && (L->clusters[end - 1].flags & CL_NEWLINE)) { newline_units = L->clusters[end - 1].length; --last; }
    float width_ws = 0, width = 0;
    if (last > s) {
      const Cluster *lk = &L->clusters[last - 1];
      width_ws = lk->x + lk->advance;
    }
    UINT32 w_end = last;
    while (w_end > s && (L->clusters[w_end - 1].flags & CL_SPACE)) { trailing += L->clusters[w_end - 1].length; --w_end; }
    if (w_end > s) { const Cluster *lk = &L->clusters[w_end - 1]; width = lk->x + lk->advance; }
    ln->pos = end > s ? L->clusters[s].pos : 0;
    ln->length = 0;
    for (UINT32 j = s; j < end; ++j) ln->length += L->clusters[j].length;
    ln->trailing_ws = trailing;
    ln->newline = newline_units;
    ln->width = width;
    ln->width_ws = width_ws;
    /* trimming of a line that does not fit (only when it does not wrap) */
    if (!wraps && L->p.trimming.granularity != DWRITE_TRIMMING_GRANULARITY_NONE && L->sign && width > avail + 0.001f) {
      UINT32 cut = last;
      while (cut > s && L->clusters[cut - 1].x + L->clusters[cut - 1].advance + L->sign_width > avail) --cut;
      if (L->p.trimming.granularity == DWRITE_TRIMMING_GRANULARITY_WORD) {
        UINT32 c = cut;
        while (c > s && !(L->clusters[c - 1].flags & CL_BREAK_AFTER)) --c;
        if (c > s) cut = c;
      }
      while (cut > s && (L->clusters[cut - 1].flags & CL_SPACE)) --cut;
      ln->trim_at = cut;
      ln->trim_x = cut > s ? L->clusters[cut - 1].x + L->clusters[cut - 1].advance : 0;
      ln->sign_width = L->sign_width;
      ln->trimmed = 1;
      ln->width = ln->width_ws = ln->trim_x + L->sign_width;
    }
    /* line height */
    if (end == s || (ascent == 0 && descent == 0)) {      /* an empty line has the height of the default font */
      UINT32 face;
      if (resolve_face(collection_of(L->p.collection), L->p.family, L->p.weight, L->p.stretch, L->p.style, &face)) {
        DWRITE_FONT_METRICS fm;
        if (face_info(face, NULL, &fm) && fm.designUnitsPerEm) {
          ascent = (float)fm.ascent * L->p.size / (float)fm.designUnitsPerEm;
          descent = (float)fm.descent * L->p.size / (float)fm.designUnitsPerEm;
          gap = (float)fm.lineGap * L->p.size / (float)fm.designUnitsPerEm;
        }
      }
    }
    ln->height = ascent + descent + gap;
    ln->baseline = ascent + gap;
    if (L->p.spacing_method == DWRITE_LINE_SPACING_METHOD_UNIFORM) {
      ln->height = L->p.line_spacing;
      ln->baseline = L->p.baseline;
    }
    /* justification: a wrapped line spreads over the width */
    if (L->p.alignment == DWRITE_TEXT_ALIGNMENT_JUSTIFIED && !mandatory && end < n && wraps && width < avail) {
      UINT32 spaces = 0;
      for (UINT32 j = s; j < w_end; ++j) if ((L->clusters[j].flags & CL_SPACE) && !(L->clusters[j].flags & CL_NOBREAK_SPACE)) ++spaces;
      if (spaces) ln->justify = (avail - width) / (float)spaces;
    }
    s = end;
  } while (s < n);
  if (count == 0) { /* unreachable: the loop runs at least once */ }
  L->lines = lines;
  L->line_count = count;
  return S_OK;
}

/* Positions the lines: alignment inside the layout box, vertical offsets, and the numbers GetMetrics reports. */
static void layout_place(Layout *L) {
  float y = 0, total = 0, widest = 0, widest_ws = 0, left = BIG_WIDTH;
  for (UINT32 i = 0; i < L->line_count; ++i) total += L->lines[i].height;
  const int bounded_w = L->max_width < BIG_WIDTH;
  DWRITE_TEXT_ALIGNMENT a = L->p.alignment;
  if (L->p.reading == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT) a = a == DWRITE_TEXT_ALIGNMENT_LEADING ? DWRITE_TEXT_ALIGNMENT_TRAILING : a == DWRITE_TEXT_ALIGNMENT_TRAILING ? DWRITE_TEXT_ALIGNMENT_LEADING : a;
  L->top = 0;
  if (L->max_height < BIG_WIDTH) {
    if (L->p.paragraph == DWRITE_PARAGRAPH_ALIGNMENT_FAR) L->top = L->max_height - total;
    else if (L->p.paragraph == DWRITE_PARAGRAPH_ALIGNMENT_CENTER) L->top = (L->max_height - total) / 2;
  }
  for (UINT32 i = 0; i < L->line_count; ++i) {
    Line *ln = &L->lines[i];
    ln->y = y;
    y += ln->height;
    float lw = ln->width + ln->justify * 0;      /* (a justified line fills the box) */
    if (ln->justify > 0) lw = L->max_width;
    ln->x = 0;
    if (bounded_w && a == DWRITE_TEXT_ALIGNMENT_TRAILING) ln->x = L->max_width - lw;
    else if (bounded_w && a == DWRITE_TEXT_ALIGNMENT_CENTER) ln->x = (L->max_width - lw) / 2;
    if (ln->justify > 0) { ln->width = ln->width_ws = lw; }
    if (ln->width > widest) widest = ln->width;
    if (ln->width_ws > widest_ws) widest_ws = ln->width_ws;
    if (ln->x < left) left = ln->x;
  }
  L->metrics_left = L->line_count ? left : 0;
  L->metrics_width = widest;
  L->metrics_width_ws = widest_ws;
  L->metrics_height = total;
  /* the narrowest the layout can be: the longest piece between two break opportunities */
  float piece = 0, longest = 0;
  for (UINT32 i = 0; i < L->cluster_count; ++i) {
    const Cluster *k = &L->clusters[i];
    if (k->flags & CL_NEWLINE) { piece = 0; continue; }
    if (!(k->flags & CL_SPACE)) piece += k->advance;
    if (k->flags & CL_BREAK_AFTER) { if (piece > longest) longest = piece; piece = 0; }
  }
  if (piece > longest) longest = piece;
  L->min_width = longest;
}

static HRESULT layout_ensure(Layout *L) {
  if (L->built && !L->p.dirty) return S_OK;
  layout_free_built(L);
  L->p.dirty = 0;
  HRESULT hr = layout_shape(L);
  if (SUCCEEDED(hr)) hr = layout_lines(L);
  if (FAILED(hr)) { layout_free_built(L); return hr; }
  layout_place(L);
  L->built = 1;
  return S_OK;
}

/* ---- queries on the built layout */
static HRESULT WINAPI layout_GetLineMetrics(IDWriteTextLayout *This, DWRITE_LINE_METRICS *metrics, UINT32 max, UINT32 *actual) {
  Layout *L = (Layout *)This;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  *actual = L->line_count;
  if (!metrics || max < L->line_count) return E_NOT_SUFFICIENT_BUFFER;
  for (UINT32 i = 0; i < L->line_count; ++i) {
    const Line *ln = &L->lines[i];
    metrics[i].length = ln->length;
    metrics[i].trailingWhitespaceLength = ln->trailing_ws;
    metrics[i].newlineLength = ln->newline;
    metrics[i].height = ln->height;
    metrics[i].baseline = ln->baseline;
    metrics[i].isTrimmed = ln->trimmed;
  }
  return S_OK;
}
static HRESULT WINAPI layout_GetMetrics(IDWriteTextLayout *This, DWRITE_TEXT_METRICS *m) {
  Layout *L = (Layout *)This;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  m->left = L->metrics_left;
  m->top = L->top;
  m->width = L->metrics_width;
  m->widthIncludingTrailingWhitespace = L->metrics_width_ws;
  m->height = L->metrics_height;
  m->layoutWidth = L->max_width;
  m->layoutHeight = L->max_height;
  m->maxBidiReorderingDepth = 1;
  m->lineCount = L->line_count;
  return S_OK;
}
static HRESULT WINAPI layout_GetOverhangMetrics(IDWriteTextLayout *This, DWRITE_OVERHANG_METRICS *o) {
  Layout *L = (Layout *)This;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  /* the ink of the text compared with the layout box: the glyph boxes of the clusters */
  float left = 0, top = 0, right = 0, bottom = 0;
  for (UINT32 li = 0; li < L->line_count; ++li) {
    const Line *ln = &L->lines[li];
    for (UINT32 j = ln->first; j < ln->first + ln->count && j < ln->trim_at; ++j) {
      const Cluster *k = &L->clusters[j];
      if (k->flags & (CL_NOGLYPH | CL_INLINE | CL_SPACE)) continue;
      UINT16 g = k->glyph;
      TcosGlyphBox b;
      DWRITE_FONT_METRICS fm;
      if (!face_boxes(k->face, &g, 1, &b) || !face_info(k->face, NULL, &fm)) continue;
      const float s = k->size / (float)fm.designUnitsPerEm;
      const float gx0 = ln->x + k->x + (float)b.x_min * s, gx1 = ln->x + k->x + (float)b.x_max * s;
      const float gy0 = L->top + ln->y + ln->baseline - (float)b.y_max * s, gy1 = L->top + ln->y + ln->baseline - (float)b.y_min * s;
      if (-gx0 > left) left = -gx0;
      if (gx1 - L->max_width > right && L->max_width < BIG_WIDTH) right = gx1 - L->max_width;
      if (-gy0 > top) top = -gy0;
      if (gy1 - L->max_height > bottom && L->max_height < BIG_WIDTH) bottom = gy1 - L->max_height;
    }
  }
  o->left = left;
  o->top = top;
  o->right = right;
  o->bottom = bottom;
  return S_OK;
}
static HRESULT WINAPI layout_GetClusterMetrics(IDWriteTextLayout *This, DWRITE_CLUSTER_METRICS *metrics, UINT32 max, UINT32 *actual) {
  Layout *L = (Layout *)This;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  *actual = L->cluster_count;
  if (!metrics || max < L->cluster_count) return E_NOT_SUFFICIENT_BUFFER;
  for (UINT32 i = 0; i < L->cluster_count; ++i) {
    const Cluster *k = &L->clusters[i];
    memset(&metrics[i], 0, sizeof(metrics[i]));
    metrics[i].width = (k->flags & CL_NEWLINE) ? 0 : k->advance;
    metrics[i].length = k->length;
    metrics[i].canWrapLineAfter = (k->flags & CL_BREAK_AFTER) != 0;
    metrics[i].isWhitespace = (k->flags & CL_SPACE) != 0;
    metrics[i].isNewline = (k->flags & CL_NEWLINE) != 0;
    metrics[i].isSoftHyphen = (k->flags & CL_SOFT_HYPHEN) != 0;
  }
  return S_OK;
}
static HRESULT WINAPI layout_DetermineMinWidth(IDWriteTextLayout *This, FLOAT *min_width) {
  Layout *L = (Layout *)This;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  *min_width = L->min_width;
  return S_OK;
}

/* The line a text position is in (the last line for a position at or past the end). */
static UINT32 layout_line_of(const Layout *L, UINT32 pos) {
  for (UINT32 i = 0; i < L->line_count; ++i)
    if (pos < L->lines[i].pos + L->lines[i].length) return i;
  return L->line_count ? L->line_count - 1 : 0;
}
static void hit_metrics(const Layout *L, const Line *ln, const Cluster *k, DWRITE_HIT_TEST_METRICS *m) {
  memset(m, 0, sizeof(*m));
  m->textPosition = k->pos;
  m->length = k->length;
  m->left = ln->x + k->x;
  m->top = L->top + ln->y;
  m->width = (k->flags & CL_NEWLINE) ? 0 : k->advance;
  m->height = ln->height;
  m->isText = TRUE;
}
static HRESULT WINAPI layout_HitTestPoint(IDWriteTextLayout *This, FLOAT x, FLOAT y, BOOL *trailing, BOOL *inside, DWRITE_HIT_TEST_METRICS *m) {
  Layout *L = (Layout *)This;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  memset(m, 0, sizeof(*m));
  *trailing = FALSE;
  *inside = FALSE;
  if (L->cluster_count == 0) {
    m->height = L->line_count ? L->lines[0].height : 0;
    m->top = L->top;
    return S_OK;
  }
  UINT32 li = 0;
  for (; li + 1 < L->line_count; ++li)
    if (y < L->top + L->lines[li].y + L->lines[li].height) break;
  const Line *ln = &L->lines[li];
  *inside = x >= ln->x && x < ln->x + ln->width_ws && y >= L->top + ln->y && y < L->top + ln->y + ln->height;
  UINT32 last = ln->first + ln->count;
  if (last > ln->first && (L->clusters[last - 1].flags & CL_NEWLINE)) --last;
  if (last == ln->first) { hit_metrics(L, ln, &L->clusters[ln->first], m); m->width = 0; *inside = FALSE; return S_OK; }
  UINT32 pick = ln->first;
  const float lx = x - ln->x;
  for (UINT32 j = ln->first; j < last; ++j) {
    pick = j;
    if (lx < L->clusters[j].x + L->clusters[j].advance) break;
  }
  const Cluster *k = &L->clusters[pick];
  hit_metrics(L, ln, k, m);
  *trailing = lx >= k->x + k->advance / 2;
  return S_OK;
}
static HRESULT WINAPI layout_HitTestTextPosition(IDWriteTextLayout *This, UINT32 pos, BOOL trailing, FLOAT *px, FLOAT *py, DWRITE_HIT_TEST_METRICS *m) {
  Layout *L = (Layout *)This;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  memset(m, 0, sizeof(*m));
  *px = 0;
  *py = L->top;
  if (L->cluster_count == 0) { m->height = L->line_count ? L->lines[0].height : 0; m->top = L->top; return S_OK; }
  if (pos >= L->length) {      /* the end of the text: after its last character */
    const Line *ln = &L->lines[L->line_count - 1];
    UINT32 last = ln->first + ln->count;
    const Cluster *k = &L->clusters[last - 1];
    if (k->flags & CL_NEWLINE) {      /* (after a final newline the caret is at the start of an empty line) */
      *px = ln->x; *py = L->top + ln->y + ln->height;
      m->textPosition = L->length; m->top = *py; m->height = ln->height;
      return S_OK;
    }
    hit_metrics(L, ln, k, m);
    m->textPosition = L->length;
    m->length = 0;
    m->left = ln->x + k->x + k->advance;
    m->width = 0;
    *px = m->left;
    *py = m->top;
    return S_OK;
  }
  UINT32 ci = 0;
  while (ci + 1 < L->cluster_count && L->clusters[ci + 1].pos <= pos) ++ci;
  const Line *ln = &L->lines[layout_line_of(L, pos)];
  const Cluster *k = &L->clusters[ci];
  hit_metrics(L, ln, k, m);
  *px = m->left + (trailing ? m->width : 0);
  *py = m->top;
  return S_OK;
}
static HRESULT WINAPI layout_HitTestTextRange(IDWriteTextLayout *This, UINT32 pos, UINT32 length, FLOAT ox, FLOAT oy, DWRITE_HIT_TEST_METRICS *metrics, UINT32 max, UINT32 *actual) {
  Layout *L = (Layout *)This;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  *actual = 0;
  if (pos >= L->length) return S_OK;
  const UINT64 end64 = (UINT64)pos + length;
  const UINT32 end = end64 > L->length ? L->length : (UINT32)end64;
  UINT32 needed = 0;
  for (UINT32 li = 0; li < L->line_count; ++li) {
    const Line *ln = &L->lines[li];
    float x0 = 0, x1 = 0;
    UINT32 first_pos = 0, last_pos = 0;
    int any = 0;
    for (UINT32 j = ln->first; j < ln->first + ln->count; ++j) {
      const Cluster *k = &L->clusters[j];
      if (k->pos + k->length <= pos || k->pos >= end) continue;
      const float w = (k->flags & CL_NEWLINE) ? 0 : k->advance;
      if (!any) { x0 = k->x; first_pos = k->pos; any = 1; }
      x1 = k->x + w;
      last_pos = k->pos + k->length;
    }
    if (!any) continue;
    if (metrics && needed < max) {
      DWRITE_HIT_TEST_METRICS *m = &metrics[needed];
      memset(m, 0, sizeof(*m));
      m->textPosition = first_pos;
      m->length = last_pos - first_pos;
      m->left = ox + ln->x + x0;
      m->top = oy + L->top + ln->y;
      m->width = x1 - x0;
      m->height = ln->height;
      m->isText = TRUE;
    }
    ++needed;
  }
  *actual = needed;
  return metrics && needed <= max ? S_OK : E_NOT_SUFFICIENT_BUFFER;
}

static HRESULT WINAPI layout_Draw(IDWriteTextLayout *This, void *context, IDWriteTextRenderer *renderer, FLOAT ox, FLOAT oy);

static const IDWriteTextLayoutVtbl layout_vtbl = {
  layout_QueryInterface, layout_AddRef, layout_Release,
  (void *)tf_SetTextAlignment, (void *)tf_SetParagraphAlignment, (void *)tf_SetWordWrapping, (void *)tf_SetReadingDirection, (void *)tf_SetFlowDirection,
  (void *)tf_SetIncrementalTabStop, (void *)tf_SetTrimming, (void *)tf_SetLineSpacing, (void *)tf_GetTextAlignment, (void *)tf_GetParagraphAlignment, (void *)tf_GetWordWrapping,
  (void *)tf_GetReadingDirection, (void *)tf_GetFlowDirection, (void *)tf_GetIncrementalTabStop, (void *)tf_GetTrimming, (void *)tf_GetLineSpacing, (void *)tf_GetFontCollection,
  (void *)tf_GetFontFamilyNameLength, (void *)tf_GetFontFamilyName, (void *)tf_GetFontWeight, (void *)tf_GetFontStyle, (void *)tf_GetFontStretch, (void *)tf_GetFontSize,
  (void *)tf_GetLocaleNameLength, (void *)tf_GetLocaleName,
  layout_SetMaxWidth, layout_SetMaxHeight, layout_SetFontCollection, layout_SetFontFamilyName, layout_SetFontWeight, layout_SetFontStyle, layout_SetFontStretch, layout_SetFontSize,
  layout_SetUnderline, layout_SetStrikethrough, layout_SetDrawingEffect, layout_SetInlineObject, layout_SetTypography, layout_SetLocaleName, layout_GetMaxWidth, layout_GetMaxHeight,
  layout_GetFontCollection_, layout_GetFontFamilyNameLength_, layout_GetFontFamilyName_, layout_GetFontWeight_, layout_GetFontStyle_, layout_GetFontStretch_, layout_GetFontSize_,
  layout_GetUnderline, layout_GetStrikethrough, layout_GetDrawingEffect, layout_GetInlineObject, layout_GetTypography, layout_GetLocaleNameLength_, layout_GetLocaleName_,
  layout_Draw, layout_GetLineMetrics, layout_GetMetrics, layout_GetOverhangMetrics, layout_GetClusterMetrics, layout_DetermineMinWidth, layout_HitTestPoint,
  layout_HitTestTextPosition, layout_HitTestTextRange,
};

static HRESULT layout_create(const WCHAR *text, UINT32 length, IDWriteTextFormat *format, float max_width, float max_height, IDWriteTextLayout **out) {
  *out = NULL;
  if ((length && !text) || !format || !(max_width >= 0) || !(max_height >= 0)) return E_INVALIDARG;
  Layout *L = OBJECT_NEW(Layout);
  if (!L) return E_OUTOFMEMORY;
  /* the format's properties, through its interface */
  IDWriteTextFormat *f = format;
  UINT32 fn = f->lpVtbl->GetFontFamilyNameLength(f), ln = f->lpVtbl->GetLocaleNameLength(f);
  WCHAR *family = mem_array((SIZE_T)fn + 1, sizeof(WCHAR)), *locale = mem_array((SIZE_T)ln + 1, sizeof(WCHAR));
  IDWriteFontCollection *collection = NULL;
  if (!family || !locale || FAILED(f->lpVtbl->GetFontFamilyName(f, family, fn + 1)) || FAILED(f->lpVtbl->GetLocaleName(f, locale, ln + 1))) {
    mem_free(family); mem_free(locale); mem_free(L);
    return E_OUTOFMEMORY;
  }
  f->lpVtbl->GetFontCollection(f, &collection);
  HRESULT hr = props_init(&L->p, family, collection, f->lpVtbl->GetFontWeight(f), f->lpVtbl->GetFontStyle(f), f->lpVtbl->GetFontStretch(f), f->lpVtbl->GetFontSize(f), locale);
  release(collection);      /* (props_init took its own reference) */
  mem_free(family);
  mem_free(locale);
  if (FAILED(hr)) { mem_free(L); return hr; }
  L->p.alignment = f->lpVtbl->GetTextAlignment(f);
  L->p.paragraph = f->lpVtbl->GetParagraphAlignment(f);
  L->p.wrapping = f->lpVtbl->GetWordWrapping(f);
  L->p.reading = f->lpVtbl->GetReadingDirection(f);
  L->p.flow = f->lpVtbl->GetFlowDirection(f);
  L->p.tab_stop = f->lpVtbl->GetIncrementalTabStop(f);
  f->lpVtbl->GetTrimming(f, &L->p.trimming, &L->p.trim_sign);
  f->lpVtbl->GetLineSpacing(f, &L->p.spacing_method, &L->p.line_spacing, &L->p.baseline);
  L->lpVtbl = &layout_vtbl;
  L->ref = 1;
  L->length = length;
  L->text = mem_array((SIZE_T)length + 1, sizeof(WCHAR));
  L->attrs = mem_array((SIZE_T)length + 1, sizeof(CharAttr));
  if (!L->text || !L->attrs) { layout_free(L); return E_OUTOFMEMORY; }
  if (length) memcpy(L->text, text, length * sizeof(WCHAR));
  const CharAttr def = attr_default(&L->p);
  for (UINT32 i = 0; i < length; ++i) L->attrs[i] = def;
  L->families = mem_array(1, sizeof(WCHAR *));
  L->locales = mem_array(1, sizeof(WCHAR *));
  L->family_count = L->locale_count = 1;
  L->max_width = max_width;
  L->max_height = max_height;
  L->p.dirty = 1;
  *out = (IDWriteTextLayout *)L;
  return S_OK;
}
