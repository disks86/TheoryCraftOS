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

/* dwrite.dll: the factory and DWriteCreateFactory. Included by dwrite.c. */

typedef struct Factory {
  const IDWriteFactoryVtbl *lpVtbl;
  LONG ref;
  int shared;
  IDWriteFontFileLoader **file_loaders;
  UINT32 file_loader_count, file_loader_capacity;
  IDWriteFontCollectionLoader **collection_loaders;
  UINT32 collection_loader_count, collection_loader_capacity;
  IDWriteGdiInterop *gdi;
} Factory;

static void factory_free(Factory *f) {
  for (UINT32 i = 0; i < f->file_loader_count; ++i) release(f->file_loaders[i]);
  for (UINT32 i = 0; i < f->collection_loader_count; ++i) release(f->collection_loaders[i]);
  mem_free(f->file_loaders);
  mem_free(f->collection_loaders);
  release(f->gdi);
  mem_free(f);
}
static ULONG WINAPI factory_AddRef(IDWriteFactory *This) { return (ULONG)InterlockedIncrement(&((Factory *)This)->ref); }
static ULONG WINAPI factory_Release(IDWriteFactory *This) {
  Factory *f = (Factory *)This;
  const LONG n = InterlockedDecrement(&f->ref);
  if (n == 0 && !f->shared) factory_free(f);
  return (ULONG)n;
}
static HRESULT WINAPI factory_QueryInterface(IDWriteFactory *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteFactory, NULL, NULL); }

static HRESULT WINAPI factory_GetSystemFontCollection(IDWriteFactory *This, IDWriteFontCollection **c, BOOL check) {
  (void)This; (void)check;
  *c = (IDWriteFontCollection *)system_collection();
  if (!*c) return E_FAIL;
  add_ref(*c);
  return S_OK;
}

/* A growing list of interface pointers (registered loaders), each holding a reference. */
static HRESULT list_add(void ***list, UINT32 *count, UINT32 *capacity, void *item) {
  if (*count == *capacity) {
    void **bigger = mem_grow(*list, (*capacity ? *capacity * 2 : 4) * sizeof(void *));
    if (!bigger) return E_OUTOFMEMORY;
    *list = bigger;
    *capacity = *capacity ? *capacity * 2 : 4;
  }
  add_ref(item);
  (*list)[(*count)++] = item;
  return S_OK;
}
static int list_find(void **list, UINT32 count, void *item) {
  for (UINT32 i = 0; i < count; ++i) if (list[i] == item) return (int)i;
  return -1;
}
static HRESULT list_remove(void **list, UINT32 *count, void *item) {
  const int at = list_find(list, *count, item);
  if (at < 0) return E_INVALIDARG;
  release(list[at]);
  list[at] = list[--*count];
  return S_OK;
}

static HRESULT WINAPI factory_RegisterFontCollectionLoader(IDWriteFactory *This, IDWriteFontCollectionLoader *loader) {
  Factory *f = (Factory *)This;
  if (!loader) return E_INVALIDARG;
  if (list_find((void **)f->collection_loaders, f->collection_loader_count, loader) >= 0) return (HRESULT)0x88985006;      /* DWRITE_E_ALREADYREGISTERED */
  return list_add((void ***)&f->collection_loaders, &f->collection_loader_count, &f->collection_loader_capacity, loader);
}
static HRESULT WINAPI factory_UnregisterFontCollectionLoader(IDWriteFactory *This, IDWriteFontCollectionLoader *loader) {
  Factory *f = (Factory *)This;
  return loader ? list_remove((void **)f->collection_loaders, &f->collection_loader_count, loader) : E_INVALIDARG;
}
static HRESULT WINAPI factory_RegisterFontFileLoader(IDWriteFactory *This, IDWriteFontFileLoader *loader) {
  Factory *f = (Factory *)This;
  if (!loader) return E_INVALIDARG;
  if (list_find((void **)f->file_loaders, f->file_loader_count, loader) >= 0) return (HRESULT)0x88985006;      /* DWRITE_E_ALREADYREGISTERED */
  return list_add((void ***)&f->file_loaders, &f->file_loader_count, &f->file_loader_capacity, loader);
}
static HRESULT WINAPI factory_UnregisterFontFileLoader(IDWriteFactory *This, IDWriteFontFileLoader *loader) {
  Factory *f = (Factory *)This;
  return loader ? list_remove((void **)f->file_loaders, &f->file_loader_count, loader) : E_INVALIDARG;
}

static HRESULT WINAPI factory_CreateFontFileReference(IDWriteFactory *This, const WCHAR *path, const FILETIME *time, IDWriteFontFile **file) {
  (void)This;
  WCHAR full[MAX_PATH * 2];
  *file = NULL;
  if (!path) return E_INVALIDARG;
  const DWORD n = GetFullPathNameW(path, MAX_PATH * 2, full, NULL);
  if (n == 0 || n >= MAX_PATH * 2) return HRESULT_FROM_WIN32(n ? ERROR_FILENAME_EXCED_RANGE : GetLastError());
  if (!time && GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return DWRITE_E_FILENOTFOUND_;      /* (with a time stamp the file need not exist yet) */
  FontFile *f = file_new((IDWriteFontFileLoader *)&g_local_loader, full, (UINT32)((wlen(full) + 1) * sizeof(WCHAR)), 0);
  if (!f) return E_OUTOFMEMORY;
  *file = (IDWriteFontFile *)f;
  return S_OK;
}
static HRESULT WINAPI factory_CreateCustomFontFileReference(IDWriteFactory *This, const void *key, UINT32 key_size, IDWriteFontFileLoader *loader, IDWriteFontFile **file) {
  Factory *fa = (Factory *)This;
  *file = NULL;
  const int builtin = loader == (IDWriteFontFileLoader *)&g_local_loader || loader == SYSTEM_LOADER;      /* (the system's own loaders need no registration) */
  if (!loader || (key_size && !key) || (!builtin && list_find((void **)fa->file_loaders, fa->file_loader_count, loader) < 0)) return E_INVALIDARG;
  FontFile *f = file_new(loader, key, key_size, !builtin);
  if (!f) return E_OUTOFMEMORY;
  *file = (IDWriteFontFile *)f;
  return S_OK;
}
static HRESULT WINAPI factory_CreateFontFace(IDWriteFactory *This, DWRITE_FONT_FACE_TYPE type, UINT32 count, IDWriteFontFile *const *files, UINT32 index, DWRITE_FONT_SIMULATIONS sim,
                                             IDWriteFontFace **face) {
  (void)This;
  *face = NULL;
  if (count != 1 || !files || !files[0] || (UINT32)type > DWRITE_FONT_FACE_TYPE_RAW_CFF || (sim & ~(DWRITE_FONT_SIMULATIONS_BOLD | DWRITE_FONT_SIMULATIONS_OBLIQUE))) return E_INVALIDARG;
  if (type == DWRITE_FONT_FACE_TYPE_TYPE1 || type == DWRITE_FONT_FACE_TYPE_VECTOR || type == DWRITE_FONT_FACE_TYPE_BITMAP) return E_INVALIDARG;
  if (type == DWRITE_FONT_FACE_TYPE_RAW_CFF) return (HRESULT)0x88985009;      /* DWRITE_E_UNSUPPORTEDOPERATION */
  if (files[0]->lpVtbl != &file_vtbl) return E_INVALIDARG;
  if (type == DWRITE_FONT_FACE_TYPE_CFF || type == DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION) return DWRITE_E_FILEFORMAT;      /* (the fonts are TrueType, not collections) */
  UINT32 id;
  HRESULT hr = file_face((FontFile *)files[0], &id);
  if (FAILED(hr)) return hr;
  (void)index;
  return face_create(id, sim, face);
}
static HRESULT WINAPI factory_CreateRenderingParams(IDWriteFactory *This, IDWriteRenderingParams **p) {
  (void)This;
  return params_create(1.8f, 0.5f, 1.0f, DWRITE_PIXEL_GEOMETRY_RGB, DWRITE_RENDERING_MODE_DEFAULT, p);
}
static HRESULT WINAPI factory_CreateMonitorRenderingParams(IDWriteFactory *This, HMONITOR monitor, IDWriteRenderingParams **p) {
  (void)monitor;
  return factory_CreateRenderingParams(This, p);
}
static HRESULT WINAPI factory_CreateCustomRenderingParams(IDWriteFactory *This, FLOAT gamma, FLOAT contrast, FLOAT cleartype, DWRITE_PIXEL_GEOMETRY geometry,
                                                          DWRITE_RENDERING_MODE mode, IDWriteRenderingParams **p) {
  (void)This;
  return params_create(gamma, contrast, cleartype, geometry, mode, p);
}
static HRESULT WINAPI factory_CreateTextFormat(IDWriteFactory *This, const WCHAR *family, IDWriteFontCollection *c, DWRITE_FONT_WEIGHT w, DWRITE_FONT_STYLE s, DWRITE_FONT_STRETCH st,
                                               FLOAT size, const WCHAR *locale, IDWriteTextFormat **out) {
  (void)This;
  return format_create(family, c, w, s, st, size, locale, out);
}
static HRESULT WINAPI factory_CreateTypography(IDWriteFactory *This, IDWriteTypography **t) { (void)This; *t = NULL; return typography_create(t); }
static HRESULT WINAPI factory_GetGdiInterop(IDWriteFactory *This, IDWriteGdiInterop **out) {
  Factory *f = (Factory *)This;
  *out = NULL;
  if (!f->gdi) {
    GdiInterop *g = OBJECT_NEW(GdiInterop);
    if (!g) return E_OUTOFMEMORY;
    g->lpVtbl = &gdi_vtbl;
    g->ref = 1;
    if (InterlockedCompareExchangePointer((void *volatile *)&f->gdi, g, NULL) != NULL) mem_free(g);
  }
  *out = f->gdi;
  add_ref(*out);
  return S_OK;
}
static HRESULT WINAPI factory_CreateTextLayout(IDWriteFactory *This, const WCHAR *text, UINT32 length, IDWriteTextFormat *format, FLOAT w, FLOAT h, IDWriteTextLayout **out) {
  (void)This;
  return layout_create(text, length, format, w, h, out);
}
static HRESULT WINAPI factory_CreateGdiCompatibleTextLayout(IDWriteFactory *This, const WCHAR *text, UINT32 length, IDWriteTextFormat *format, FLOAT w, FLOAT h, FLOAT ppd,
                                                            const DWRITE_MATRIX *transform, BOOL natural, IDWriteTextLayout **out) {
  (void)This; (void)transform; (void)natural;
  *out = NULL;
  if (!(ppd > 0)) return E_INVALIDARG;
  return layout_create(text, length, format, w, h, out);
}
static HRESULT WINAPI factory_CreateEllipsisTrimmingSign(IDWriteFactory *This, IDWriteTextFormat *format, IDWriteInlineObject **out) {
  (void)This;
  *out = NULL;
  if (!format) return E_INVALIDARG;
  IDWriteTextLayout *tmp = NULL;
  HRESULT hr = layout_create(NULL, 0, format, BIG_WIDTH, BIG_WIDTH, &tmp);
  if (FAILED(hr)) return hr;
  hr = sign_create(&((Layout *)tmp)->p, out);
  release(tmp);
  return hr;
}
static HRESULT WINAPI factory_CreateTextAnalyzer(IDWriteFactory *This, IDWriteTextAnalyzer **out) { (void)This; *out = NULL; return analyzer_create(out); }
/* A BCP 47 name: a language of 2 or 3 letters, then any subtags of 1 to 8 letters or digits ("en-US"). */
static int locale_name_ok(const WCHAR *name) {
  UINT32 i = 0, run = 0;
  if (!name) return 0;
  for (; name[i] && name[i] != '-'; ++i, ++run)
    if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z'))) return 0;
  if (run < 2 || run > 3) return 0;
  while (name[i] == '-') {
    ++i;
    run = 0;
    for (; name[i] && name[i] != '-'; ++i, ++run)
      if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9'))) return 0;
    if (run < 1 || run > 8) return 0;
  }
  return 1;
}
static HRESULT WINAPI factory_CreateNumberSubstitution(IDWriteFactory *This, DWRITE_NUMBER_SUBSTITUTION_METHOD method, const WCHAR *locale, BOOL ignore, IDWriteNumberSubstitution **out) {
  (void)This; (void)ignore;
  *out = NULL;
  if ((UINT32)method > DWRITE_NUMBER_SUBSTITUTION_METHOD_TRADITIONAL || (method != DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE && !locale_name_ok(locale))) return E_INVALIDARG;
  return numsub_create(out);
}
static HRESULT WINAPI factory_CreateGlyphRunAnalysis(IDWriteFactory *This, const DWRITE_GLYPH_RUN *run, FLOAT ppd, const DWRITE_MATRIX *transform, DWRITE_RENDERING_MODE mode,
                                                     DWRITE_MEASURING_MODE measuring, FLOAT bx, FLOAT by, IDWriteGlyphRunAnalysis **out) {
  (void)This; (void)measuring;
  return analysis_create(run, ppd, transform, mode, bx, by, out);
}

/* A collection of the families in the font files a program's collection loader enumerates. */
static HRESULT WINAPI factory_CreateCustomFontCollection(IDWriteFactory *This, IDWriteFontCollectionLoader *loader, const void *key, UINT32 key_size, IDWriteFontCollection **out) {
  Factory *f = (Factory *)This;
  *out = NULL;
  if (!loader || (key_size && !key) || list_find((void **)f->collection_loaders, f->collection_loader_count, loader) < 0) return E_INVALIDARG;
  IDWriteFontFileEnumerator *en = NULL;
  HRESULT hr = loader->lpVtbl->CreateEnumeratorFromKey(loader, This, key, key_size, &en);
  if (FAILED(hr)) return hr;
  Collection *c = OBJECT_NEW(Collection);
  if (!c) { en->lpVtbl->Release(en); return E_OUTOFMEMORY; }
  c->lpVtbl = &collection_vtbl;
  c->ref = 1;
  for (;;) {
    BOOL has = FALSE;
    hr = en->lpVtbl->MoveNext(en, &has);
    if (FAILED(hr) || !has) break;
    IDWriteFontFile *file = NULL;
    hr = en->lpVtbl->GetCurrentFontFile(en, &file);
    if (FAILED(hr)) break;
    UINT32 face = 0;
    hr = file->lpVtbl == &file_vtbl ? file_face((FontFile *)file, &face) : E_INVALIDARG;
    release(file);
    if (FAILED(hr)) break;
    WCHAR *family = face_name(face, 0);
    if (!family) { hr = E_OUTOFMEMORY; break; }
    UINT32 at = c->count, i;
    for (i = 0; i < c->count; ++i) if (wieq(c->families[i].name, family)) { at = i; break; }
    if (at == c->count) {
      FamilyEntry *bigger = mem_grow(c->families, (c->count + 1) * sizeof(FamilyEntry));
      if (!bigger) { mem_free(family); hr = E_OUTOFMEMORY; break; }
      c->families = bigger;
      memset(&c->families[at], 0, sizeof(FamilyEntry));
      c->families[at].name = family;
      ++c->count;
    } else mem_free(family);
    FamilyEntry *e = &c->families[at];
    UINT32 *faces = mem_array(e->count + 1, sizeof(UINT32));
    if (!faces) { hr = E_OUTOFMEMORY; break; }
    if (e->count) memcpy(faces, e->faces, e->count * sizeof(UINT32));
    faces[e->count] = face;
    mem_free(e->faces);
    e->faces = faces;
    ++e->count;
  }
  en->lpVtbl->Release(en);
  if (FAILED(hr)) { collection_free(c); return hr; }
  for (UINT32 i = 1; i < c->count; ++i)      /* (families are sorted by name) */
    for (UINT32 j = i; j > 0; --j) {
      const WCHAR *a = c->families[j - 1].name, *b = c->families[j].name;
      UINT32 k = 0;
      while (a[k] && wlower(a[k]) == wlower(b[k])) ++k;
      if (wlower(a[k]) <= wlower(b[k])) break;
      FamilyEntry t = c->families[j];
      c->families[j] = c->families[j - 1];
      c->families[j - 1] = t;
    }
  *out = (IDWriteFontCollection *)c;
  return S_OK;
}

static const IDWriteFactoryVtbl factory_vtbl = {
  factory_QueryInterface, factory_AddRef, factory_Release, factory_GetSystemFontCollection, factory_CreateCustomFontCollection, factory_RegisterFontCollectionLoader,
  factory_UnregisterFontCollectionLoader, factory_CreateFontFileReference, factory_CreateCustomFontFileReference, factory_CreateFontFace, factory_CreateRenderingParams,
  factory_CreateMonitorRenderingParams, factory_CreateCustomRenderingParams, factory_RegisterFontFileLoader, factory_UnregisterFontFileLoader, factory_CreateTextFormat,
  factory_CreateTypography, factory_GetGdiInterop, factory_CreateTextLayout, factory_CreateGdiCompatibleTextLayout, factory_CreateEllipsisTrimmingSign, factory_CreateTextAnalyzer,
  factory_CreateNumberSubstitution, factory_CreateGlyphRunAnalysis,
};

static Factory g_shared_factory;

EXPORT HRESULT WINAPI DWriteCreateFactory(DWRITE_FACTORY_TYPE type, REFIID iid, IUnknown **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!iid || (UINT32)type > DWRITE_FACTORY_TYPE_ISOLATED) return E_INVALIDARG;
  if (!guid_eq(iid, &IID_IDWriteFactory) && !guid_eq(iid, &IID_IUnknown)) return E_NOINTERFACE;
  if (type == DWRITE_FACTORY_TYPE_SHARED) {
    if (!g_shared_factory.lpVtbl) {
      g_shared_factory.shared = 1;
      g_shared_factory.ref = 1;      /* (the DLL's own reference: the shared factory is never freed) */
      InterlockedExchangePointer((void *volatile *)&g_shared_factory.lpVtbl, (void *)&factory_vtbl);
    }
    InterlockedIncrement(&g_shared_factory.ref);
    *out = (IUnknown *)&g_shared_factory;
    return S_OK;
  }
  Factory *f = OBJECT_NEW(Factory);
  if (!f) return E_OUTOFMEMORY;
  f->lpVtbl = &factory_vtbl;
  f->ref = 1;
  *out = (IUnknown *)f;
  return S_OK;
}
