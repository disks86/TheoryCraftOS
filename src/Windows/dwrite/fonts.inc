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

/* dwrite.dll: localized strings, font files and their loaders, font collections, families, fonts and font faces. Included by dwrite.c. */

#ifndef DWRITE_E_FILEFORMAT
#define DWRITE_E_FILEFORMAT ((HRESULT)0x88985000)
#endif
#define DWRITE_E_UNEXPECTED_ ((HRESULT)0x88985001)
#define DWRITE_E_NOFONT_ ((HRESULT)0x88985002)
#define DWRITE_E_FILENOTFOUND_ ((HRESULT)0x88985003)
#define DWRITE_E_FILEACCESS_ ((HRESULT)0x88985004)
#define DWRITE_E_NOCOLOR_ ((HRESULT)0x88985006)

/* ================================================================ localized strings (one string, en-us) */
typedef struct Strings { const IDWriteLocalizedStringsVtbl *lpVtbl; LONG ref; WCHAR *text; } Strings;
static const WCHAR kLocale[] = L"en-us";
static void strings_free(Strings *s) { mem_free(s->text); mem_free(s); }
REF_METHODS(strings, Strings, IDWriteLocalizedStrings, strings_free)
static HRESULT WINAPI strings_QueryInterface(IDWriteLocalizedStrings *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteLocalizedStrings, NULL, NULL); }
static UINT32 WINAPI strings_GetCount(IDWriteLocalizedStrings *This) { (void)This; return 1; }
static HRESULT WINAPI strings_FindLocaleName(IDWriteLocalizedStrings *This, const WCHAR *name, UINT32 *index, BOOL *exists) {
  (void)This;
  *index = 0;
  *exists = name && wieq(name, kLocale);
  if (!*exists) *index = (UINT32)-1;
  return S_OK;
}
static HRESULT WINAPI strings_GetLocaleNameLength(IDWriteLocalizedStrings *This, UINT32 index, UINT32 *length) {
  (void)This;
  if (index != 0) { *length = (UINT32)-1; return E_FAIL; }
  *length = (UINT32)wlen(kLocale);
  return S_OK;
}
/* Copies `text` into the caller's buffer: E_FAIL for an index past the end, E_NOT_SUFFICIENT_BUFFER for no buffer at all, E_INVALIDARG for one that is too small. */
static HRESULT strings_copy(const WCHAR *text, UINT32 index, WCHAR *buffer, UINT32 size) {
  if (index != 0) { if (buffer && size) buffer[0] = 0; return E_FAIL; }
  if (!buffer) return size ? E_INVALIDARG : E_NOT_SUFFICIENT_BUFFER;
  if (size < wlen(text) + 1) { if (size) buffer[0] = 0; return E_INVALIDARG; }
  memcpy(buffer, text, (wlen(text) + 1) * sizeof(WCHAR));
  return S_OK;
}
static HRESULT WINAPI strings_GetLocaleName(IDWriteLocalizedStrings *This, UINT32 index, WCHAR *buffer, UINT32 size) { (void)This; return strings_copy(kLocale, index, buffer, size); }
static HRESULT WINAPI strings_GetStringLength(IDWriteLocalizedStrings *This, UINT32 index, UINT32 *length) {
  if (index != 0) { *length = (UINT32)-1; return E_FAIL; }
  *length = (UINT32)wlen(((Strings *)This)->text);
  return S_OK;
}
static HRESULT WINAPI strings_GetString(IDWriteLocalizedStrings *This, UINT32 index, WCHAR *buffer, UINT32 size) { return strings_copy(((Strings *)This)->text, index, buffer, size); }
static const IDWriteLocalizedStringsVtbl strings_vtbl = {
  strings_QueryInterface, strings_AddRef, strings_Release, strings_GetCount, strings_FindLocaleName, strings_GetLocaleNameLength, strings_GetLocaleName,
  strings_GetStringLength, strings_GetString,
};
/* Takes ownership of `text`. */
static HRESULT strings_create(WCHAR *text, IDWriteLocalizedStrings **out) {
  Strings *s = OBJECT_NEW(Strings);
  if (!s || !text) { mem_free(s); mem_free(text); return E_OUTOFMEMORY; }
  s->lpVtbl = &strings_vtbl;
  s->ref = 1;
  s->text = text;
  *out = (IDWriteLocalizedStrings *)s;
  return S_OK;
}

/* ================================================================ font file streams and loaders */
typedef struct MemStream { const IDWriteFontFileStreamVtbl *lpVtbl; LONG ref; BYTE *data; UINT64 size; UINT64 write_time; } MemStream;
static void stream_free(MemStream *s) { mem_free(s->data); mem_free(s); }
REF_METHODS(stream, MemStream, IDWriteFontFileStream, stream_free)
static HRESULT WINAPI stream_QueryInterface(IDWriteFontFileStream *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteFontFileStream, NULL, NULL); }
static HRESULT WINAPI stream_ReadFileFragment(IDWriteFontFileStream *This, const void **start, UINT64 offset, UINT64 size, void **context) {
  MemStream *s = (MemStream *)This;
  *start = NULL;
  *context = NULL;
  if (offset > s->size || size > s->size - offset) return E_FAIL;
  *start = s->data + offset;
  return S_OK;
}
static void WINAPI stream_ReleaseFileFragment(IDWriteFontFileStream *This, void *context) { (void)This; (void)context; }
static HRESULT WINAPI stream_GetFileSize(IDWriteFontFileStream *This, UINT64 *size) { *size = ((MemStream *)This)->size; return S_OK; }
static HRESULT WINAPI stream_GetLastWriteTime(IDWriteFontFileStream *This, UINT64 *time) { *time = ((MemStream *)This)->write_time; return S_OK; }
static const IDWriteFontFileStreamVtbl stream_vtbl = {
  stream_QueryInterface, stream_AddRef, stream_Release, stream_ReadFileFragment, stream_ReleaseFileFragment, stream_GetFileSize, stream_GetLastWriteTime,
};
static MemStream *stream_new(BYTE *data, UINT64 size, UINT64 write_time) {      /* takes the data */
  MemStream *s = OBJECT_NEW(MemStream);
  if (!s) { mem_free(data); return NULL; }
  s->lpVtbl = &stream_vtbl;
  s->ref = 1;
  s->data = data;
  s->size = size;
  s->write_time = write_time;
  return s;
}

/* The loader of the system's faces: the key is the face number; the stream is the sfnt file of the kernel. It is a local font file loader too: programs ask for the path of
 * a system font file, and get the name the file would have in C:\Windows\Fonts (the fonts are the kernel's; there is no file of that name). */
static HRESULT WINAPI sysloader_QueryInterface(IDWriteLocalFontFileLoader *This, REFIID riid, void **out) {
  return query3(This, riid, out, &IID_IDWriteLocalFontFileLoader, &IID_IDWriteFontFileLoader, NULL);
}
static ULONG WINAPI sysloader_AddRef(IDWriteLocalFontFileLoader *This) { (void)This; return 2; }
static ULONG WINAPI sysloader_Release(IDWriteLocalFontFileLoader *This) { (void)This; return 1; }
static HRESULT WINAPI sysloader_CreateStreamFromKey(IDWriteLocalFontFileLoader *This, const void *key, UINT32 key_size, IDWriteFontFileStream **stream) {
  (void)This;
  *stream = NULL;
  if (!key || key_size != 4) return E_INVALIDARG;
  const UINT32 face = *(const UINT32 *)key;
  UINT32 total = 0;
  BYTE one;
  if (!face_exists(face)) return DWRITE_E_FILENOTFOUND_;
  face_read(face, 0, &one, 1, &total);
  BYTE *data = mem_alloc(total);
  if (!data) return E_OUTOFMEMORY;
  for (UINT32 done = 0; done < total;) {
    const UINT32 got = face_read(face, done, data + done, total - done, NULL);
    if (!got) { mem_free(data); return E_FAIL; }
    done += got;
  }
  MemStream *s = stream_new(data, total, 0);
  if (!s) return E_OUTOFMEMORY;
  *stream = (IDWriteFontFileStream *)s;
  return S_OK;
}
static UINT32 sysloader_path(UINT32 face, WCHAR *path) {      /* "C:\\Windows\\Fonts\\tcos-<face>.ttf" */
  static const WCHAR prefix[] = L"C:\\Windows\\Fonts\\tcos-";
  WCHAR digits[12];
  UINT32 n = 0, o = 0, v = face;
  do { digits[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
  for (UINT32 i = 0; prefix[i]; ++i) { if (path) path[o] = prefix[i]; ++o; }
  while (n) { if (path) path[o] = digits[--n]; ++o; }
  for (const WCHAR *e = L".ttf"; *e; ++e) { if (path) path[o] = *e; ++o; }
  if (path) path[o] = 0;
  return o;
}
static HRESULT WINAPI sysloader_GetFilePathLengthFromKey(IDWriteLocalFontFileLoader *This, const void *key, UINT32 key_size, UINT32 *length) {
  (void)This;
  *length = 0;
  if (!key || key_size != 4) return E_INVALIDARG;
  *length = sysloader_path(*(const UINT32 *)key, NULL);
  return S_OK;
}
static HRESULT WINAPI sysloader_GetFilePathFromKey(IDWriteLocalFontFileLoader *This, const void *key, UINT32 key_size, WCHAR *path, UINT32 length) {
  (void)This;
  if (!key || key_size != 4 || !path) return E_INVALIDARG;
  if (length < sysloader_path(*(const UINT32 *)key, NULL) + 1) return E_NOT_SUFFICIENT_BUFFER;
  sysloader_path(*(const UINT32 *)key, path);
  return S_OK;
}
static HRESULT WINAPI sysloader_GetLastWriteTimeFromKey(IDWriteLocalFontFileLoader *This, const void *key, UINT32 key_size, FILETIME *time) {
  (void)This;
  if (!key || key_size != 4) return E_INVALIDARG;
  time->dwLowDateTime = time->dwHighDateTime = 0;
  return S_OK;
}
static const IDWriteLocalFontFileLoaderVtbl sysloader_vtbl = {
  sysloader_QueryInterface, sysloader_AddRef, sysloader_Release, sysloader_CreateStreamFromKey, sysloader_GetFilePathLengthFromKey, sysloader_GetFilePathFromKey,
  sysloader_GetLastWriteTimeFromKey,
};
static IDWriteLocalFontFileLoader g_system_loader_object = { &sysloader_vtbl };
#define SYSTEM_LOADER ((IDWriteFontFileLoader *)&g_system_loader_object)

/* The loader of files by path (CreateFontFileReference): the key is the path, NUL ended, in WCHARs. */
static HRESULT WINAPI locloader_QueryInterface(IDWriteLocalFontFileLoader *This, REFIID riid, void **out) {
  return query3(This, riid, out, &IID_IDWriteLocalFontFileLoader, &IID_IDWriteFontFileLoader, NULL);
}
static ULONG WINAPI locloader_AddRef(IDWriteLocalFontFileLoader *This) { (void)This; return 2; }
static ULONG WINAPI locloader_Release(IDWriteLocalFontFileLoader *This) { (void)This; return 1; }
static int key_path_ok(const void *key, UINT32 size) {
  const WCHAR *p = key;
  return key && size >= 2 && size % 2 == 0 && p[size / 2 - 1] == 0;
}
static HRESULT WINAPI locloader_CreateStreamFromKey(IDWriteLocalFontFileLoader *This, const void *key, UINT32 key_size, IDWriteFontFileStream **stream) {
  (void)This;
  *stream = NULL;
  if (!key_path_ok(key, key_size)) return E_INVALIDARG;
  HANDLE file = CreateFileW(key, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return DWRITE_E_FILENOTFOUND_;
  LARGE_INTEGER size;
  FILETIME time = {0, 0};
  GetFileTime(file, NULL, NULL, &time);
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 0x10000000) { CloseHandle(file); return DWRITE_E_FILEACCESS_; }
  BYTE *data = mem_alloc((SIZE_T)size.QuadPart);
  DWORD got = 0;
  if (!data || (size.QuadPart && (!ReadFile(file, data, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart))) {
    CloseHandle(file);
    mem_free(data);
    return DWRITE_E_FILEACCESS_;
  }
  CloseHandle(file);
  MemStream *s = stream_new(data, (UINT64)size.QuadPart, (UINT64)time.dwLowDateTime | (UINT64)time.dwHighDateTime << 32);
  if (!s) return E_OUTOFMEMORY;
  *stream = (IDWriteFontFileStream *)s;
  return S_OK;
}
static HRESULT WINAPI locloader_GetFilePathLengthFromKey(IDWriteLocalFontFileLoader *This, const void *key, UINT32 key_size, UINT32 *length) {
  (void)This;
  *length = 0;
  if (!key_path_ok(key, key_size)) return E_INVALIDARG;
  *length = key_size / 2 - 1;
  return S_OK;
}
static HRESULT WINAPI locloader_GetFilePathFromKey(IDWriteLocalFontFileLoader *This, const void *key, UINT32 key_size, WCHAR *path, UINT32 length) {
  (void)This;
  if (!key_path_ok(key, key_size) || !path) return E_INVALIDARG;
  if (length < key_size / 2) return E_NOT_SUFFICIENT_BUFFER;
  memcpy(path, key, key_size);
  return S_OK;
}
static HRESULT WINAPI locloader_GetLastWriteTimeFromKey(IDWriteLocalFontFileLoader *This, const void *key, UINT32 key_size, FILETIME *time) {
  (void)This;
  if (!key_path_ok(key, key_size)) return E_INVALIDARG;
  HANDLE file = CreateFileW(key, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return DWRITE_E_FILENOTFOUND_;
  const BOOL ok = GetFileTime(file, NULL, NULL, time);
  CloseHandle(file);
  return ok ? S_OK : DWRITE_E_FILEACCESS_;
}
static const IDWriteLocalFontFileLoaderVtbl locloader_vtbl = {
  locloader_QueryInterface, locloader_AddRef, locloader_Release, locloader_CreateStreamFromKey, locloader_GetFilePathLengthFromKey, locloader_GetFilePathFromKey,
  locloader_GetLastWriteTimeFromKey,
};
static IDWriteLocalFontFileLoader g_local_loader = { (const IDWriteLocalFontFileLoaderVtbl *)&locloader_vtbl };

/* ---- font files */
typedef struct FontFile {
  const IDWriteFontFileVtbl *lpVtbl;
  LONG ref;
  IDWriteFontFileLoader *loader;      /* referenced (the system and local loaders are not counted) */
  BYTE *key;
  UINT32 key_size;
} FontFile;
static void file_free(FontFile *f) { release(f->loader); mem_free(f->key); mem_free(f); }
REF_METHODS(file, FontFile, IDWriteFontFile, file_free)
static HRESULT WINAPI file_QueryInterface(IDWriteFontFile *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteFontFile, NULL, NULL); }
static HRESULT WINAPI file_GetReferenceKey(IDWriteFontFile *This, const void **key, UINT32 *size) {
  FontFile *f = (FontFile *)This;
  *key = f->key;
  *size = f->key_size;
  return S_OK;
}
static HRESULT WINAPI file_GetLoader(IDWriteFontFile *This, IDWriteFontFileLoader **loader) {
  FontFile *f = (FontFile *)This;
  *loader = f->loader;
  add_ref(f->loader);
  return S_OK;
}

/* The whole file of `f` into a new buffer. */
static HRESULT file_read_all(FontFile *f, BYTE **data, UINT64 *size) {
  IDWriteFontFileStream *stream = NULL;
  *data = NULL;
  HRESULT hr = f->loader->lpVtbl->CreateStreamFromKey(f->loader, f->key, f->key_size, &stream);
  if (FAILED(hr)) return hr;
  UINT64 total = 0;
  hr = stream->lpVtbl->GetFileSize(stream, &total);
  if (SUCCEEDED(hr) && (total == 0 || total > 0x10000000)) hr = DWRITE_E_FILEFORMAT;
  if (SUCCEEDED(hr)) {
    const void *fragment = NULL;
    void *context = NULL;
    hr = stream->lpVtbl->ReadFileFragment(stream, &fragment, 0, total, &context);
    if (SUCCEEDED(hr)) {
      *data = mem_alloc((SIZE_T)total);
      if (*data) { memcpy(*data, fragment, (SIZE_T)total); *size = total; } else hr = E_OUTOFMEMORY;
      stream->lpVtbl->ReleaseFileFragment(stream, context);
    }
  }
  stream->lpVtbl->Release(stream);
  return hr;
}

static HRESULT WINAPI file_Analyze(IDWriteFontFile *This, BOOL *supported, DWRITE_FONT_FILE_TYPE *file_type, DWRITE_FONT_FACE_TYPE *face_type, UINT32 *faces) {
  FontFile *f = (FontFile *)This;
  *supported = FALSE;
  *file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
  *face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
  *faces = 0;
  IDWriteFontFileStream *stream = NULL;
  HRESULT hr = f->loader->lpVtbl->CreateStreamFromKey(f->loader, f->key, f->key_size, &stream);
  if (FAILED(hr)) return hr;
  UINT64 size = 0;
  const void *fragment = NULL;
  void *context = NULL;
  stream->lpVtbl->GetFileSize(stream, &size);
  if (size >= 12 && SUCCEEDED(stream->lpVtbl->ReadFileFragment(stream, &fragment, 0, 12, &context))) {
    const UINT32 tag = be32(fragment);
    if (tag == 0x00010000 || tag == 0x74727565) { *supported = TRUE; *file_type = DWRITE_FONT_FILE_TYPE_TRUETYPE; *face_type = DWRITE_FONT_FACE_TYPE_TRUETYPE; *faces = 1; }
    else if (tag == 0x74746366) {
      *supported = TRUE;
      *file_type = DWRITE_FONT_FILE_TYPE_TRUETYPE_COLLECTION;
      *face_type = DWRITE_FONT_FACE_TYPE_TRUETYPE_COLLECTION;
      *faces = be32((const BYTE *)fragment + 8);
    } else if (tag == 0x4F54544F) { *supported = TRUE; *file_type = DWRITE_FONT_FILE_TYPE_CFF; *face_type = DWRITE_FONT_FACE_TYPE_CFF; *faces = 1; }
    stream->lpVtbl->ReleaseFileFragment(stream, context);
  }
  stream->lpVtbl->Release(stream);
  return S_OK;
}
static const IDWriteFontFileVtbl file_vtbl = { file_QueryInterface, file_AddRef, file_Release, file_GetReferenceKey, file_GetLoader, file_Analyze };

static FontFile *file_new(IDWriteFontFileLoader *loader, const void *key, UINT32 key_size, int counted) {
  FontFile *f = OBJECT_NEW(FontFile);
  if (f) f->key = mem_alloc(key_size ? key_size : 1);
  if (!f || !f->key) { if (f) mem_free(f); return NULL; }
  f->lpVtbl = &file_vtbl;
  f->ref = 1;
  f->loader = loader;
  if (counted) add_ref(loader);
  f->key_size = key_size;
  if (key_size) memcpy(f->key, key, key_size);
  return f;
}
static FontFile *file_for_face(UINT32 face) { return file_new(SYSTEM_LOADER, &face, 4, 0); }

/* Files that were added to the kernel's fonts, by (loader, key), so that making a face of the same file twice adds it once. */
typedef struct LoadedFile { IDWriteFontFileLoader *loader; BYTE *key; UINT32 key_size, face; } LoadedFile;
static LoadedFile *g_loaded;
static UINT32 g_loaded_count, g_loaded_capacity;

/* The kernel's face number of a font file; adds the file to the kernel's fonts if it is not there. */
static HRESULT file_face(FontFile *f, UINT32 *face) {
  if (f->loader == SYSTEM_LOADER) {
    if (f->key_size != 4) return E_INVALIDARG;
    *face = *(UINT32 *)f->key;
    return face_exists(*face) ? S_OK : DWRITE_E_NOFONT_;
  }
  EnterCriticalSection(&g_lock);
  for (UINT32 i = 0; i < g_loaded_count; ++i)
    if (g_loaded[i].loader == f->loader && g_loaded[i].key_size == f->key_size && memcmp(g_loaded[i].key, f->key, f->key_size) == 0) {
      *face = g_loaded[i].face;
      LeaveCriticalSection(&g_lock);
      return S_OK;
    }
  LeaveCriticalSection(&g_lock);
  BYTE *data = NULL;
  UINT64 size = 0;
  HRESULT hr = file_read_all(f, &data, &size);
  if (FAILED(hr)) return hr;
  TcosFontRequest r;
  const NTSTATUS status = font_call(OP_ADD_FONT, 0, 0, 0, (ULONG)size, data, NULL, 0, &r);
  mem_free(data);
  if (status != 0) return status == (NTSTATUS)0xC0000017 ? E_OUTOFMEMORY : DWRITE_E_FILEFORMAT;
  *face = r.r0;
  EnterCriticalSection(&g_lock);
  if (g_loaded_count == g_loaded_capacity) {
    LoadedFile *bigger = mem_grow(g_loaded, (g_loaded_capacity ? g_loaded_capacity * 2 : 16) * sizeof(LoadedFile));
    if (bigger) { g_loaded = bigger; g_loaded_capacity = g_loaded_capacity ? g_loaded_capacity * 2 : 16; }
  }
  if (g_loaded_count < g_loaded_capacity) {
    LoadedFile *e = &g_loaded[g_loaded_count];
    e->key = mem_alloc(f->key_size ? f->key_size : 1);
    if (e->key) {
      memcpy(e->key, f->key, f->key_size);
      e->key_size = f->key_size;
      e->loader = f->loader;
      add_ref(f->loader);
      e->face = *face;
      ++g_loaded_count;
    }
  }
  LeaveCriticalSection(&g_lock);
  return S_OK;
}

/* ================================================================ font collections */
typedef struct FamilyEntry { WCHAR *name; UINT32 *faces; UINT32 count; } FamilyEntry;
typedef struct Collection {
  const IDWriteFontCollectionVtbl *lpVtbl;
  LONG ref;
  FamilyEntry *families;
  UINT32 count;
  int is_system;
} Collection;

typedef struct FontFamily { const IDWriteFontFamilyVtbl *lpVtbl; LONG ref; Collection *collection; UINT32 index; } FontFamily;
typedef struct FontList { const IDWriteFontListVtbl *lpVtbl; LONG ref; Collection *collection; UINT32 family; UINT32 *faces; UINT32 count; } FontList;
typedef struct Font { const IDWriteFontVtbl *lpVtbl; LONG ref; Collection *collection; UINT32 family; UINT32 face; } Font;
typedef struct FontFace { const IDWriteFontFaceVtbl *lpVtbl; LONG ref; UINT32 face; DWRITE_FONT_SIMULATIONS simulations; } FontFace;

static void collection_free(Collection *c) {
  if (c->is_system) return;      /* (the system collection lives as long as the DLL) */
  for (UINT32 i = 0; i < c->count; ++i) { mem_free(c->families[i].name); mem_free(c->families[i].faces); }
  mem_free(c->families);
  mem_free(c);
}

/* ---- matching (the CSS Fonts algorithm DirectWrite follows: width, then style, then weight) */
static UINT32 stretch_enum_of(UINT16 percent) {
  static const UINT16 kPercent[9] = {50, 62, 75, 87, 100, 112, 125, 150, 200};
  UINT32 best = 4, distance = 1000;
  for (UINT32 i = 0; i < 9; ++i) {
    const UINT32 d = percent > kPercent[i] ? percent - kPercent[i] : kPercent[i] - percent;
    if (d < distance) { distance = d; best = i; }
  }
  return best + 1;
}
static DWRITE_FONT_STYLE face_style(const TcosFaceInfo *i) { return (i->flags & 1) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL; }

/* The sort key of a face for a request: smaller is better. */
static UINT64 match_key(const TcosFaceInfo *info, UINT32 weight, UINT32 stretch, DWRITE_FONT_STYLE style) {
  const UINT32 s = stretch_enum_of(info->stretch);
  UINT32 stretch_d = s > stretch ? s - stretch : stretch - s;
  const DWRITE_FONT_STYLE fs = face_style(info);
  UINT32 style_d = fs == style ? 0 : (style != DWRITE_FONT_STYLE_NORMAL && fs != DWRITE_FONT_STYLE_NORMAL) ? 1 : 2;
  UINT32 weight_d;
  const UINT32 w = info->weight;
  if (w == weight) weight_d = 0;
  else if (weight <= 400 ? w < weight : w > weight) weight_d = (w > weight ? w - weight : weight - w);      /* the preferred side */
  else weight_d = 1000 + (w > weight ? w - weight : weight - w);
  return (UINT64)stretch_d << 40 | (UINT64)style_d << 32 | weight_d;
}

/* The faces of an entry in order of preference for a request: indices into entry->faces. */
static void match_order(const FamilyEntry *e, UINT32 weight, UINT32 stretch, DWRITE_FONT_STYLE style, UINT32 *order) {
  UINT64 keys[16];
  const UINT32 n = e->count < 16 ? e->count : 16;
  for (UINT32 i = 0; i < n; ++i) {
    TcosFaceInfo info;
    keys[i] = face_info(e->faces[i], &info, NULL) ? match_key(&info, weight, stretch, style) : ~(UINT64)0;
    order[i] = i;
  }
  for (UINT32 i = 1; i < n; ++i)      /* (a stable insertion sort: the file order breaks ties) */
    for (UINT32 j = i; j > 0 && keys[order[j]] < keys[order[j - 1]]; --j) { const UINT32 t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
}

/* ---- the system collection */
static const struct { const WCHAR *name; const WCHAR *kernel; } kSystemFamilies[] = {
  {L"Arial", L"TCOS Sans"}, {L"Consolas", L"TCOS Mono"}, {L"Courier New", L"TCOS Mono"}, {L"Georgia", L"TCOS Serif"}, {L"Liberation Mono", L"TCOS Mono"},
  {L"Liberation Sans", L"TCOS Sans"}, {L"Liberation Serif", L"TCOS Serif"}, {L"Lucida Console", L"TCOS Mono"}, {L"Segoe UI", L"TCOS Sans"}, {L"Tahoma", L"TCOS Sans"},
  {L"TCOS Fallback", L"TCOS Fallback"}, {L"TCOS Mono", L"TCOS Mono"}, {L"TCOS Sans", L"TCOS Sans"}, {L"TCOS Serif", L"TCOS Serif"}, {L"Times New Roman", L"TCOS Serif"},
  {L"Verdana", L"TCOS Sans"},
};
#define SYSTEM_FAMILY_COUNT (sizeof(kSystemFamilies) / sizeof(kSystemFamilies[0]))

static const IDWriteFontCollectionVtbl collection_vtbl;
static const IDWriteFontFamilyVtbl family_vtbl;
static const IDWriteFontListVtbl list_vtbl;
static const IDWriteFontVtbl font_vtbl;
static const IDWriteFontFaceVtbl face_vtbl;

static Collection g_system_collection;
static int g_system_ready;
static UINT32 g_bundled_count;      /* the number of faces the system has of its own (the program's come after them) */

/* Caller holds g_lock. */
static int system_collection_build(void) {
  if (g_system_ready) return 1;
  TcosFontRequest r;
  if (font_call(OP_FACE_COUNT, 0, 0, 0, 0, NULL, NULL, 0, &r) != 0) return 0;
  UINT32 count = r.r0 < MAX_FACES ? r.r0 : MAX_FACES;
  FamilyEntry *entries = mem_array(SYSTEM_FAMILY_COUNT, sizeof(FamilyEntry));
  if (!entries) return 0;
  for (UINT32 f = 0; f < count; ++f) {
    FaceCache *fc = face_get(f);
    if (!fc || !(fc->info.flags & 4)) continue;      /* (a face that is bundled) */
    if (!fc->names[0]) fc->names[0] = face_fetch_name(f, 0);
    for (UINT32 i = 0; i < SYSTEM_FAMILY_COUNT; ++i) {
      if (!fc->names[0] || !wieq(fc->names[0], kSystemFamilies[i].kernel)) continue;
      UINT32 *bigger = mem_array(entries[i].count + 1, sizeof(UINT32));
      if (!bigger) continue;
      if (entries[i].count) memcpy(bigger, entries[i].faces, entries[i].count * sizeof(UINT32));
      mem_free(entries[i].faces);
      entries[i].faces = bigger;
      entries[i].faces[entries[i].count++] = f;
    }
    g_bundled_count = f + 1;
  }
  UINT32 kept = 0;
  for (UINT32 i = 0; i < SYSTEM_FAMILY_COUNT; ++i) {
    if (!entries[i].count) continue;
    entries[kept] = entries[i];
    entries[kept].name = wdup(kSystemFamilies[i].name);
    ++kept;
  }
  g_system_collection.lpVtbl = &collection_vtbl;
  g_system_collection.families = entries;
  g_system_collection.count = kept;
  g_system_collection.is_system = 1;
  g_system_collection.ref = 1;
  g_system_ready = kept != 0;
  return g_system_ready;
}
static Collection *system_collection(void) {
  EnterCriticalSection(&g_lock);
  const int ok = system_collection_build();
  LeaveCriticalSection(&g_lock);
  return ok ? &g_system_collection : NULL;
}

/* ---- objects */

static HRESULT font_create(Collection *c, UINT32 family, UINT32 face, IDWriteFont **out) {
  Font *f = OBJECT_NEW(Font);
  if (!f) return E_OUTOFMEMORY;
  f->lpVtbl = &font_vtbl;
  f->ref = 1;
  f->collection = c;
  add_ref(c);
  f->family = family;
  f->face = face;
  *out = (IDWriteFont *)f;
  return S_OK;
}
static HRESULT face_create(UINT32 face, DWRITE_FONT_SIMULATIONS simulations, IDWriteFontFace **out) {
  FontFace *f = OBJECT_NEW(FontFace);
  if (!f) return E_OUTOFMEMORY;
  f->lpVtbl = &face_vtbl;
  f->ref = 1;
  f->face = face;
  f->simulations = simulations;
  *out = (IDWriteFontFace *)f;
  return S_OK;
}

/* -- collection */
static HRESULT WINAPI collection_QueryInterface(IDWriteFontCollection *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteFontCollection, NULL, NULL); }
static ULONG WINAPI collection_AddRef(IDWriteFontCollection *This) { return (ULONG)InterlockedIncrement(&((Collection *)This)->ref); }
static ULONG WINAPI collection_Release(IDWriteFontCollection *This) {
  Collection *c = (Collection *)This;
  const LONG n = InterlockedDecrement(&c->ref);
  if (n == 0) collection_free(c);
  return (ULONG)(n > 0 ? n : 0);
}
static UINT32 WINAPI collection_GetFontFamilyCount(IDWriteFontCollection *This) { return ((Collection *)This)->count; }
static HRESULT family_create(Collection *c, UINT32 index, IDWriteFontFamily **out) {
  FontFamily *f = OBJECT_NEW(FontFamily);
  if (!f) return E_OUTOFMEMORY;
  f->lpVtbl = &family_vtbl;
  f->ref = 1;
  f->collection = c;
  add_ref(c);
  f->index = index;
  *out = (IDWriteFontFamily *)f;
  return S_OK;
}
static HRESULT WINAPI collection_GetFontFamily(IDWriteFontCollection *This, UINT32 index, IDWriteFontFamily **family) {
  Collection *c = (Collection *)This;
  *family = NULL;
  if (index >= c->count) return E_INVALIDARG;
  return family_create(c, index, family);
}
static HRESULT WINAPI collection_FindFamilyName(IDWriteFontCollection *This, const WCHAR *name, UINT32 *index, BOOL *exists) {
  Collection *c = (Collection *)This;
  *index = (UINT32)-1;
  *exists = FALSE;
  if (!name) return E_INVALIDARG;
  for (UINT32 i = 0; i < c->count; ++i)
    if (wieq(c->families[i].name, name)) { *index = i; *exists = TRUE; break; }
  return S_OK;
}
static HRESULT WINAPI collection_GetFontFromFontFace(IDWriteFontCollection *This, IDWriteFontFace *face, IDWriteFont **font) {
  Collection *c = (Collection *)This;
  *font = NULL;
  if (!face) return E_INVALIDARG;
  const UINT32 id = ((FontFace *)face)->face;
  for (UINT32 i = 0; i < c->count; ++i)
    for (UINT32 j = 0; j < c->families[i].count; ++j)
      if (c->families[i].faces[j] == id) return font_create(c, i, id, font);
  return DWRITE_E_NOFONT_;
}
static const IDWriteFontCollectionVtbl collection_vtbl = {
  collection_QueryInterface, collection_AddRef, collection_Release, collection_GetFontFamilyCount, collection_GetFontFamily, collection_FindFamilyName,
  collection_GetFontFromFontFace,
};

/* -- family */
static void family_free(FontFamily *f) { release(f->collection); mem_free(f); }
static ULONG WINAPI family_AddRef(IDWriteFontFamily *This) { return (ULONG)InterlockedIncrement(&((FontFamily *)This)->ref); }
static ULONG WINAPI family_Release(IDWriteFontFamily *This) {
  FontFamily *o = (FontFamily *)This;
  const LONG n = InterlockedDecrement(&o->ref);
  if (n == 0) family_free(o);
  return (ULONG)n;
}
static HRESULT WINAPI family_QueryInterface(IDWriteFontFamily *This, REFIID riid, void **out) {
  return query3(This, riid, out, &IID_IDWriteFontFamily, &IID_IDWriteFontList, NULL);
}
static HRESULT WINAPI family_GetFontCollection(IDWriteFontFamily *This, IDWriteFontCollection **collection) {
  *collection = (IDWriteFontCollection *)((FontFamily *)This)->collection;
  add_ref(*collection);
  return S_OK;
}
static UINT32 WINAPI family_GetFontCount(IDWriteFontFamily *This) {
  FontFamily *f = (FontFamily *)This;
  return f->collection->families[f->index].count;
}
static HRESULT WINAPI family_GetFont(IDWriteFontFamily *This, UINT32 index, IDWriteFont **font) {
  FontFamily *f = (FontFamily *)This;
  const FamilyEntry *e = &f->collection->families[f->index];
  *font = NULL;
  if (index >= e->count) return E_INVALIDARG;
  return font_create(f->collection, f->index, e->faces[index], font);
}
static HRESULT WINAPI family_GetFamilyNames(IDWriteFontFamily *This, IDWriteLocalizedStrings **names) {
  FontFamily *f = (FontFamily *)This;
  return strings_create(wdup(f->collection->families[f->index].name), names);
}
static HRESULT WINAPI family_GetFirstMatchingFont(IDWriteFontFamily *This, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch, DWRITE_FONT_STYLE style, IDWriteFont **font) {
  FontFamily *f = (FontFamily *)This;
  const FamilyEntry *e = &f->collection->families[f->index];
  UINT32 order[16];
  *font = NULL;
  if (!e->count) return DWRITE_E_NOFONT_;
  match_order(e, weight, stretch, style, order);
  return font_create(f->collection, f->index, e->faces[order[0]], font);
}
static HRESULT WINAPI family_GetMatchingFonts(IDWriteFontFamily *This, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch, DWRITE_FONT_STYLE style, IDWriteFontList **fonts) {
  FontFamily *f = (FontFamily *)This;
  const FamilyEntry *e = &f->collection->families[f->index];
  UINT32 order[16];
  *fonts = NULL;
  FontList *l = OBJECT_NEW(FontList);
  if (l) l->faces = mem_array(e->count ? e->count : 1, sizeof(UINT32));
  if (!l || !l->faces) { if (l) mem_free(l); return E_OUTOFMEMORY; }
  match_order(e, weight, stretch, style, order);
  for (UINT32 i = 0; i < e->count && i < 16; ++i) l->faces[i] = e->faces[order[i]];
  l->count = e->count < 16 ? e->count : 16;
  l->lpVtbl = &list_vtbl;
  l->ref = 1;
  l->collection = f->collection;
  add_ref(l->collection);
  l->family = f->index;
  *fonts = (IDWriteFontList *)l;
  return S_OK;
}
static const IDWriteFontFamilyVtbl family_vtbl = {
  family_QueryInterface, family_AddRef, family_Release, family_GetFontCollection, family_GetFontCount, family_GetFont, family_GetFamilyNames, family_GetFirstMatchingFont,
  family_GetMatchingFonts,
};

/* -- font list (the result of GetMatchingFonts) */
static void list_free(FontList *l) { release(l->collection); mem_free(l->faces); mem_free(l); }
REF_METHODS(list, FontList, IDWriteFontList, list_free)
static HRESULT WINAPI list_QueryInterface(IDWriteFontList *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteFontList, NULL, NULL); }
static HRESULT WINAPI list_GetFontCollection(IDWriteFontList *This, IDWriteFontCollection **collection) {
  *collection = (IDWriteFontCollection *)((FontList *)This)->collection;
  add_ref(*collection);
  return S_OK;
}
static UINT32 WINAPI list_GetFontCount(IDWriteFontList *This) { return ((FontList *)This)->count; }
static HRESULT WINAPI list_GetFont(IDWriteFontList *This, UINT32 index, IDWriteFont **font) {
  FontList *l = (FontList *)This;
  *font = NULL;
  if (index >= l->count) return E_INVALIDARG;
  return font_create(l->collection, l->family, l->faces[index], font);
}
static const IDWriteFontListVtbl list_vtbl = { list_QueryInterface, list_AddRef, list_Release, list_GetFontCollection, list_GetFontCount, list_GetFont };

/* -- font */
static void font_free(Font *f) { release(f->collection); mem_free(f); }
REF_METHODS(font, Font, IDWriteFont, font_free)
static HRESULT WINAPI font_QueryInterface(IDWriteFont *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteFont, NULL, NULL); }
static HRESULT WINAPI font_GetFontFamily(IDWriteFont *This, IDWriteFontFamily **family) {
  Font *f = (Font *)This;
  return family_create(f->collection, f->family, family);
}
static DWRITE_FONT_WEIGHT WINAPI font_GetWeight(IDWriteFont *This) {
  TcosFaceInfo i;
  return face_info(((Font *)This)->face, &i, NULL) ? (DWRITE_FONT_WEIGHT)i.weight : DWRITE_FONT_WEIGHT_NORMAL;
}
static DWRITE_FONT_STRETCH WINAPI font_GetStretch(IDWriteFont *This) {
  TcosFaceInfo i;
  return face_info(((Font *)This)->face, &i, NULL) ? (DWRITE_FONT_STRETCH)stretch_enum_of(i.stretch) : DWRITE_FONT_STRETCH_NORMAL;
}
static DWRITE_FONT_STYLE WINAPI font_GetStyle(IDWriteFont *This) {
  TcosFaceInfo i;
  return face_info(((Font *)This)->face, &i, NULL) ? face_style(&i) : DWRITE_FONT_STYLE_NORMAL;
}
static BOOL WINAPI font_IsSymbolFont(IDWriteFont *This) { (void)This; return FALSE; }
static HRESULT WINAPI font_GetFaceNames(IDWriteFont *This, IDWriteLocalizedStrings **names) { return strings_create(face_name(((Font *)This)->face, 1), names); }
/* The informational strings of a face: the font's own name table has them; the bundled faces (subsets without one) only know the kernel's family name and the style. */
static HRESULT face_informational(UINT32 face, DWRITE_INFORMATIONAL_STRING_ID id, IDWriteLocalizedStrings **strings, BOOL *exists) {
  static const BYTE kNameId[] = {0, 0, 5, 7, 8, 9, 12, 10, 11, 13, 14, 1, 2, 16, 17, 19, 4, 6, 20};      /* by DWRITE_INFORMATIONAL_STRING_ID */
  *strings = NULL;
  *exists = FALSE;
  if ((UINT32)id == 0 || (UINT32)id >= sizeof(kNameId)) return S_OK;
  WCHAR *text = face_name_record(face, kNameId[id]);
  if (!text || !text[0]) {
    mem_free(text);
    text = NULL;
    if (id == DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES || id == DWRITE_INFORMATIONAL_STRING_TYPOGRAPHIC_FAMILY_NAMES) text = face_name(face, 0);
    else if (id == DWRITE_INFORMATIONAL_STRING_WIN32_SUBFAMILY_NAMES || id == DWRITE_INFORMATIONAL_STRING_TYPOGRAPHIC_SUBFAMILY_NAMES) text = face_name(face, 1);
    else if (id == DWRITE_INFORMATIONAL_STRING_FULL_NAME) text = face_name(face, 2);
    else if (id == DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME) text = face_name(face, 3);
  }
  if (!text) return S_OK;
  if (!text[0]) { mem_free(text); return S_OK; }
  *exists = TRUE;
  return strings_create(text, strings);
}
static HRESULT WINAPI font_GetInformationalStrings(IDWriteFont *This, DWRITE_INFORMATIONAL_STRING_ID id, IDWriteLocalizedStrings **strings, BOOL *exists) {
  return face_informational(((Font *)This)->face, id, strings, exists);
}
static DWRITE_FONT_SIMULATIONS WINAPI font_GetSimulations(IDWriteFont *This) { (void)This; return DWRITE_FONT_SIMULATIONS_NONE; }
static void WINAPI font_GetMetrics(IDWriteFont *This, DWRITE_FONT_METRICS *metrics) {
  if (!face_info(((Font *)This)->face, NULL, metrics)) memset(metrics, 0, sizeof(*metrics));
}
static HRESULT WINAPI font_HasCharacter(IDWriteFont *This, UINT32 value, BOOL *exists) {
  UINT16 glyph = 0;
  face_map(((Font *)This)->face, &value, 1, &glyph);
  *exists = glyph != 0;
  return S_OK;
}
static HRESULT WINAPI font_CreateFontFace(IDWriteFont *This, IDWriteFontFace **face) { return face_create(((Font *)This)->face, DWRITE_FONT_SIMULATIONS_NONE, face); }
static const IDWriteFontVtbl font_vtbl = {
  font_QueryInterface, font_AddRef, font_Release, font_GetFontFamily, font_GetWeight, font_GetStretch, font_GetStyle, font_IsSymbolFont, font_GetFaceNames,
  font_GetInformationalStrings, font_GetSimulations, font_GetMetrics, font_HasCharacter, font_CreateFontFace,
};

/* -- font face */
static void fface_free(FontFace *f) { mem_free(f); }
REF_METHODS(fface, FontFace, IDWriteFontFace, fface_free)
static HRESULT WINAPI fface_QueryInterface(IDWriteFontFace *This, REFIID riid, void **out) { return query3(This, riid, out, &IID_IDWriteFontFace, NULL, NULL); }
static DWRITE_FONT_FACE_TYPE WINAPI fface_GetType(IDWriteFontFace *This) { (void)This; return DWRITE_FONT_FACE_TYPE_TRUETYPE; }
static HRESULT WINAPI fface_GetFiles(IDWriteFontFace *This, UINT32 *count, IDWriteFontFile **files) {
  if (!files) { *count = 1; return S_OK; }
  if (*count < 1) return E_INVALIDARG;
  FontFile *f = file_for_face(((FontFace *)This)->face);
  if (!f) return E_OUTOFMEMORY;
  files[0] = (IDWriteFontFile *)f;
  *count = 1;
  return S_OK;
}
static UINT32 WINAPI fface_GetIndex(IDWriteFontFace *This) { (void)This; return 0; }
static DWRITE_FONT_SIMULATIONS WINAPI fface_GetSimulations(IDWriteFontFace *This) { return ((FontFace *)This)->simulations; }
static BOOL WINAPI fface_IsSymbolFont(IDWriteFontFace *This) { (void)This; return FALSE; }
static void WINAPI fface_GetMetrics(IDWriteFontFace *This, DWRITE_FONT_METRICS *metrics) {
  if (!face_info(((FontFace *)This)->face, NULL, metrics)) memset(metrics, 0, sizeof(*metrics));
}
static UINT16 WINAPI fface_GetGlyphCount(IDWriteFontFace *This) {
  TcosFaceInfo i;
  return face_info(((FontFace *)This)->face, &i, NULL) ? (UINT16)(i.glyph_count > 65535 ? 65535 : i.glyph_count) : 0;
}
static HRESULT face_glyph_metrics(UINT32 face, const UINT16 *glyphs, UINT32 count, DWRITE_GLYPH_METRICS *metrics) {
  DWRITE_FONT_METRICS fm;
  if (!face_info(face, NULL, &fm)) return E_FAIL;
  TcosGlyphBox *boxes = mem_array(count ? count : 1, sizeof(TcosGlyphBox));
  if (!boxes) return E_OUTOFMEMORY;
  if (!face_boxes(face, glyphs, count, boxes)) { mem_free(boxes); return E_FAIL; }
  for (UINT32 i = 0; i < count; ++i) {
    const TcosGlyphBox *b = &boxes[i];
    DWRITE_GLYPH_METRICS *m = &metrics[i];
    const INT32 height = (INT32)fm.ascent + fm.descent;
    m->leftSideBearing = b->x_min;
    m->advanceWidth = (UINT32)b->advance;
    m->rightSideBearing = b->advance - b->x_max;
    m->advanceHeight = (UINT32)height;
    m->verticalOriginY = fm.ascent;
    m->topSideBearing = fm.ascent - b->y_max;
    m->bottomSideBearing = height - (fm.ascent - b->y_max) - (b->y_max - b->y_min);
  }
  mem_free(boxes);
  return S_OK;
}
static HRESULT WINAPI fface_GetDesignGlyphMetrics(IDWriteFontFace *This, const UINT16 *glyphs, UINT32 count, DWRITE_GLYPH_METRICS *metrics, BOOL sideways) {
  (void)sideways;
  if (count && (!glyphs || !metrics)) return E_INVALIDARG;
  return face_glyph_metrics(((FontFace *)This)->face, glyphs, count, metrics);
}
static HRESULT WINAPI fface_GetGlyphIndices(IDWriteFontFace *This, const UINT32 *codepoints, UINT32 count, UINT16 *glyphs) {
  if (count && (!codepoints || !glyphs)) return E_INVALIDARG;
  return face_map(((FontFace *)This)->face, codepoints, count, glyphs) ? S_OK : E_FAIL;
}
static HRESULT WINAPI fface_TryGetFontTable(IDWriteFontFace *This, UINT32 tag, const void **data, UINT32 *size, void **context, BOOL *exists) {
  const UINT32 face = ((FontFace *)This)->face;
  UINT32 offset = 0, length = 0;
  *data = NULL;
  *size = 0;
  *context = NULL;
  *exists = FALSE;
  EnterCriticalSection(&g_lock);
  FaceCache *fc = face_get(face);
  const int found = fc && face_find_table(fc, face, tag, &offset, &length);
  LeaveCriticalSection(&g_lock);
  if (!found) return S_OK;
  BYTE *copy = mem_alloc(length ? length : 1);
  if (!copy) return E_OUTOFMEMORY;
  for (UINT32 done = 0; done < length;) {
    const UINT32 got = face_read(face, offset + done, copy + done, length - done, NULL);
    if (!got) { mem_free(copy); return S_OK; }
    done += got;
  }
  *data = copy;
  *size = length;
  *context = copy;
  *exists = TRUE;
  return S_OK;
}
static void WINAPI fface_ReleaseFontTable(IDWriteFontFace *This, void *context) { (void)This; mem_free(context); }

/* The outline of glyphs into a geometry sink: quadratic contours become cubic Béziers; y is flipped to the sink's y-down space. */
static D2D1_POINT_2F pt(float x, float y) { D2D1_POINT_2F p; p.x = x; p.y = y; return p; }
static void quad_to_cubic(D2D1_POINT_2F p0, D2D1_POINT_2F q, D2D1_POINT_2F p2, D2D1_BEZIER_SEGMENT *c) {
  c->point1 = pt(p0.x + 2.0f / 3.0f * (q.x - p0.x), p0.y + 2.0f / 3.0f * (q.y - p0.y));
  c->point2 = pt(p2.x + 2.0f / 3.0f * (q.x - p2.x), p2.y + 2.0f / 3.0f * (q.y - p2.y));
  c->point3 = p2;
}
static HRESULT outline_to_sink(UINT32 face, UINT16 glyph, float scale, float ox, float oy, IDWriteGeometrySink *sink) {
  TcosFontRequest r;
  NTSTATUS status = font_call(OP_OUTLINE, face, glyph, 0, 0, NULL, NULL, 0, &r);
  if (status != STATUS_BUFFER_TOO_SMALL_ && status != 0) return status == (NTSTATUS)0xC000000D ? S_OK : E_FAIL;      /* (a glyph past the end has no outline) */
  const UINT32 size = r.r0;
  if (size < 8 || size > 0x400000) return S_OK;
  BYTE *data = mem_alloc(size);
  if (!data) return E_OUTOFMEMORY;
  if (font_call(OP_OUTLINE, face, glyph, 0, 0, NULL, data, size, NULL) != 0) { mem_free(data); return E_FAIL; }
  const UINT32 contours = *(UINT32 *)data, points = *(UINT32 *)(data + 4);
  if (8 + (UINT64)contours * 4 + (UINT64)points * 12 > size) { mem_free(data); return E_FAIL; }
  const UINT32 *ends = (const UINT32 *)(data + 8);
  const INT32 *p = (const INT32 *)(data + 8 + contours * 4);      /* x, y, on per point */
  UINT32 start = 0;
  for (UINT32 c = 0; c < contours; ++c) {
    const UINT32 end = ends[c];
    if (end < start || end >= points) break;
    const UINT32 n = end - start + 1;
#define PX(i) (ox + (float)p[3 * (start + (i))] * scale)
#define PY(i) (oy - (float)p[3 * (start + (i)) + 1] * scale)
#define ON(i) (p[3 * (start + (i)) + 2] != 0)
    /* the first on-curve point (or the middle of two off-curve ones) starts the figure */
    UINT32 first = 0;
    D2D1_POINT_2F cur;
    UINT32 i;
    if (ON(0)) { cur = pt(PX(0), PY(0)); first = 1; }
    else if (ON(n - 1)) { cur = pt(PX(n - 1), PY(n - 1)); first = 0; }
    else cur = pt((PX(0) + PX(n - 1)) / 2, (PY(0) + PY(n - 1)) / 2);
    const D2D1_POINT_2F begin = cur;
    ID2D1SimplifiedGeometrySink_BeginFigure(sink, begin, D2D1_FIGURE_BEGIN_FILLED);
    const UINT32 count = ON(0) ? n - 1 : ON(n - 1) ? n - 1 : n;      /* the points still to walk */
    D2D1_POINT_2F pending = pt(0, 0);
    int have_off = 0;
    for (i = 0; i < count; ++i) {
      const UINT32 k = (first + i) % n;
      const D2D1_POINT_2F q = pt(PX(k), PY(k));
      if (ON(k)) {
        if (have_off) {
          D2D1_BEZIER_SEGMENT seg;
          quad_to_cubic(cur, pending, q, &seg);
          ID2D1SimplifiedGeometrySink_AddBeziers(sink, &seg, 1);
          have_off = 0;
        } else ID2D1SimplifiedGeometrySink_AddLines(sink, &q, 1);
        cur = q;
      } else {
        if (have_off) {
          const D2D1_POINT_2F mid = pt((pending.x + q.x) / 2, (pending.y + q.y) / 2);
          D2D1_BEZIER_SEGMENT seg;
          quad_to_cubic(cur, pending, mid, &seg);
          ID2D1SimplifiedGeometrySink_AddBeziers(sink, &seg, 1);
          cur = mid;
        }
        pending = q;
        have_off = 1;
      }
    }
    if (have_off) {
      D2D1_BEZIER_SEGMENT seg;
      quad_to_cubic(cur, pending, begin, &seg);
      ID2D1SimplifiedGeometrySink_AddBeziers(sink, &seg, 1);
    }
    ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
#undef PX
#undef PY
#undef ON
    start = end + 1;
  }
  mem_free(data);
  return S_OK;
}
static HRESULT WINAPI fface_GetGlyphRunOutline(IDWriteFontFace *This, FLOAT size, const UINT16 *glyphs, const FLOAT *advances, const DWRITE_GLYPH_OFFSET *offsets, UINT32 count,
                                               BOOL sideways, BOOL rtl, IDWriteGeometrySink *sink) {
  (void)sideways;
  FontFace *f = (FontFace *)This;
  DWRITE_FONT_METRICS m;
  if (!sink || (count && !glyphs) || size <= 0 || !face_info(f->face, NULL, &m)) return E_INVALIDARG;
  const float scale = size / (float)m.designUnitsPerEm;
  TcosGlyphBox *boxes = mem_array(count ? count : 1, sizeof(TcosGlyphBox));
  if (!boxes) return E_OUTOFMEMORY;
  face_boxes(f->face, glyphs, count, boxes);
  float x = 0;
  HRESULT hr = S_OK;
  for (UINT32 i = 0; i < count && SUCCEEDED(hr); ++i) {
    const float advance = advances ? advances[i] : (float)boxes[i].advance * scale;
    if (rtl) x -= advance;
    hr = outline_to_sink(f->face, glyphs[i], scale, x + (offsets ? offsets[i].advanceOffset : 0), -(offsets ? offsets[i].ascenderOffset : 0), sink);
    if (!rtl) x += advance;
  }
  mem_free(boxes);
  if (SUCCEEDED(hr)) ID2D1SimplifiedGeometrySink_Close(sink);
  return hr;
}
static HRESULT WINAPI fface_GetRecommendedRenderingMode(IDWriteFontFace *This, FLOAT size, FLOAT ppd, DWRITE_MEASURING_MODE mode, IDWriteRenderingParams *params, DWRITE_RENDERING_MODE *out) {
  (void)This;
  (void)mode;
  if (size <= 0 || ppd <= 0) { *out = DWRITE_RENDERING_MODE_DEFAULT; return E_INVALIDARG; }
  if (params && params->lpVtbl->GetRenderingMode(params) != DWRITE_RENDERING_MODE_DEFAULT) *out = params->lpVtbl->GetRenderingMode(params);
  else if (size * ppd >= 2048.0f) *out = DWRITE_RENDERING_MODE_OUTLINE;
  else *out = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;
  return S_OK;
}
/* GDI-compatible values are the metrics of the font at the pixel size with every measure rounded to whole pixels, then expressed in design units again. */
static void gdi_metrics(const DWRITE_FONT_METRICS *m, float size_px, DWRITE_FONT_METRICS *out) {
  const float upem = (float)m->designUnitsPerEm;
  float scale = size_px / upem;
#define ROUND_UNITS(v) ((int)(f_round((float)(v) * scale) / scale + (scale > 0 ? 0.5f : 0)))
  *out = *m;
  if (scale <= 0) return;
  out->ascent = (UINT16)ROUND_UNITS(m->ascent);
  out->descent = (UINT16)ROUND_UNITS(m->descent);
  out->lineGap = (INT16)ROUND_UNITS(m->lineGap);
  out->capHeight = (UINT16)ROUND_UNITS(m->capHeight);
  out->xHeight = (UINT16)ROUND_UNITS(m->xHeight);
  out->underlinePosition = (INT16)ROUND_UNITS(m->underlinePosition);
  out->underlineThickness = (UINT16)ROUND_UNITS(m->underlineThickness < 1 ? 1 : m->underlineThickness);
  out->strikethroughPosition = (INT16)ROUND_UNITS(m->strikethroughPosition);
  out->strikethroughThickness = (UINT16)ROUND_UNITS(m->strikethroughThickness < 1 ? 1 : m->strikethroughThickness);
#undef ROUND_UNITS
}
static HRESULT WINAPI fface_GetGdiCompatibleMetrics(IDWriteFontFace *This, FLOAT size, FLOAT ppd, const DWRITE_MATRIX *transform, DWRITE_FONT_METRICS *metrics) {
  (void)transform;
  DWRITE_FONT_METRICS m;
  if (size <= 0 || ppd <= 0 || !face_info(((FontFace *)This)->face, NULL, &m)) return E_INVALIDARG;
  gdi_metrics(&m, size * ppd, metrics);
  return S_OK;
}
static HRESULT WINAPI fface_GetGdiCompatibleGlyphMetrics(IDWriteFontFace *This, FLOAT size, FLOAT ppd, const DWRITE_MATRIX *transform, BOOL natural, const UINT16 *glyphs,
                                                         UINT32 count, DWRITE_GLYPH_METRICS *metrics, BOOL sideways) {
  (void)transform;
  (void)natural;
  (void)sideways;
  DWRITE_FONT_METRICS m;
  if (size <= 0 || ppd <= 0 || (count && (!glyphs || !metrics)) || !face_info(((FontFace *)This)->face, NULL, &m)) return E_INVALIDARG;
  HRESULT hr = face_glyph_metrics(((FontFace *)This)->face, glyphs, count, metrics);
  const float scale = size * ppd / (float)m.designUnitsPerEm;
  for (UINT32 i = 0; SUCCEEDED(hr) && i < count; ++i) {      /* (the advance is a whole number of pixels) */
    const float advance = f_round((float)metrics[i].advanceWidth * scale);
    const INT32 rounded = (INT32)(advance / scale + 0.5f);
    metrics[i].rightSideBearing += rounded - (INT32)metrics[i].advanceWidth;
    metrics[i].advanceWidth = (UINT32)rounded;
  }
  return hr;
}
static const IDWriteFontFaceVtbl face_vtbl = {
  fface_QueryInterface, fface_AddRef, fface_Release, fface_GetType, fface_GetFiles, fface_GetIndex, fface_GetSimulations, fface_IsSymbolFont, fface_GetMetrics, fface_GetGlyphCount,
  fface_GetDesignGlyphMetrics, fface_GetGlyphIndices, fface_TryGetFontTable, fface_ReleaseFontTable, fface_GetGlyphRunOutline, fface_GetRecommendedRenderingMode,
  fface_GetGdiCompatibleMetrics, fface_GetGdiCompatibleGlyphMetrics,
};
