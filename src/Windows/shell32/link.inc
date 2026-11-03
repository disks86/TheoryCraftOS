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

/* Shortcuts (.lnk files, [MS-SHLLINK]): the ShellLink object with IShellLinkW, IShellLinkA, IPersistFile and IPersistStream, and the class factories shell32.dll serves
 * (DllGetClassObject). Files this writes have the header, a target list, the string data (Unicode) and no LinkInfo; files with a LinkInfo are read (the local base path is
 * the target when the target list does not name a file). Included by shell32.c. */

#define LNK_HAS_IDLIST 0x01
#define LNK_HAS_LINKINFO 0x02
#define LNK_HAS_NAME 0x04
#define LNK_HAS_RELATIVE_PATH 0x08
#define LNK_HAS_WORKING_DIR 0x10
#define LNK_HAS_ARGUMENTS 0x20
#define LNK_HAS_ICON 0x40
#define LNK_UNICODE 0x80
#define LNK_FORCE_NO_LINKINFO 0x100

typedef struct {
  const IShellLinkWVtbl *w;
  const IShellLinkAVtbl *a;
  const IPersistFileVtbl *file;
  const IPersistStreamVtbl *stream;
  const IShellLinkDataListVtbl *data_list;
  DWORD data_flags;
  LONG ref;
  LPITEMIDLIST pidl;
  WCHAR *target;      /* the path as set, when the target list is not kept */
  WCHAR *description, *working_dir, *arguments, *icon_path, *relative_path, *current_file;
  int icon_index, show;
  WORD hotkey;
  BOOL dirty;
} LinkObject;
#define LINK_FROM_A(p) ((LinkObject *)((BYTE *)(p) - offsetof(LinkObject, a)))
#define LINK_FROM_FILE(p) ((LinkObject *)((BYTE *)(p) - offsetof(LinkObject, file)))
#define LINK_FROM_DATA(p) ((LinkObject *)((BYTE *)(p) - offsetof(LinkObject, data_list)))
#define LINK_FROM_STREAM(p) ((LinkObject *)((BYTE *)(p) - offsetof(LinkObject, stream)))
#define LINK_W(p) ((LinkObject *)(p))

static const GUID CLSID_ShellLink_ = {0x00021401, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID CLSID_ShellItem_ = {0x9AC9FBE1, 0xE0A2, 0x4AD6, {0xB4, 0xEE, 0xE2, 0x12, 0x01, 0x3E, 0xA9, 0x17}};
static HRESULT known_folder_manager_create(REFIID riid, void **out);
static const GUID CLSID_KnownFolderManager_ = {0x4DF0C730, 0xDF9D, 0x4AE3, {0x91, 0x53, 0xAA, 0x6B, 0x82, 0xE9, 0x79, 0x5A}};

static void link_set(WCHAR **field, const WCHAR *value) {
  WCHAR *copy = value ? w_dup(value) : NULL;
  mem_free(*field);
  *field = copy;
}

static HRESULT STDMETHODCALLTYPE lw_query(IShellLinkW *self, REFIID riid, void **out) {
  LinkObject *l = LINK_W(self);
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IShellLinkW)) *out = &l->w;
  else if (IsEqualIID(riid, &IID_IShellLinkA)) *out = &l->a;
  else if (IsEqualIID(riid, &IID_IPersistFile) || IsEqualIID(riid, &IID_IPersist)) *out = &l->file;
  else if (IsEqualIID(riid, &IID_IPersistStream)) *out = &l->stream;
  else if (IsEqualIID(riid, &IID_IShellLinkDataList)) *out = &l->data_list;
  else return E_NOINTERFACE;
  InterlockedIncrement(&l->ref);
  return S_OK;
}
static ULONG STDMETHODCALLTYPE lw_addref(IShellLinkW *self) { return (ULONG)InterlockedIncrement(&LINK_W(self)->ref); }
static ULONG STDMETHODCALLTYPE lw_release(IShellLinkW *self) {
  LinkObject *l = LINK_W(self);
  const LONG n = InterlockedDecrement(&l->ref);
  if (!n) {
    SHFree(l->pidl);
    mem_free(l->target); mem_free(l->description); mem_free(l->working_dir); mem_free(l->arguments); mem_free(l->icon_path); mem_free(l->relative_path); mem_free(l->current_file);
    mem_free(l);
    objects_remove();
  }
  return (ULONG)n;
}

/* The target of a link as a path (from the list, or as set). */
static BOOL link_target_path(const LinkObject *l, WCHAR *out, size_t capacity) {
  if (l->pidl && pidl_to_path(l->pidl, out, capacity)) return TRUE;
  if (l->target) return w_copy(out, capacity, l->target);
  out[0] = 0;
  return FALSE;
}
static HRESULT STDMETHODCALLTYPE lw_get_path(IShellLinkW *self, LPWSTR file, int capacity, WIN32_FIND_DATAW *find, DWORD flags) {
  LinkObject *l = LINK_W(self);
  WCHAR path[MAX_PATH_EX];
  (void)flags;
  if (find) ZeroMemory(find, sizeof(*find));
  if (!file || capacity <= 0) return E_INVALIDARG;
  file[0] = 0;
  if (!link_target_path(l, path, ARRAY_LEN(path))) return S_FALSE;
  if (find) {
    HANDLE h = FindFirstFileW(path, find);
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    else ZeroMemory(find, sizeof(*find));
  }
  if (!w_copy(file, (size_t)capacity, path)) return S_FALSE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE lw_get_idlist(IShellLinkW *self, LPITEMIDLIST *pidl) {
  LinkObject *l = LINK_W(self);
  if (!pidl) return E_POINTER;
  *pidl = NULL;
  if (l->pidl) { *pidl = ILClone(l->pidl); return *pidl ? S_OK : E_OUTOFMEMORY; }
  if (l->target) { *pidl = SHSimpleIDListFromPath(l->target); return *pidl ? S_OK : S_FALSE; }
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE lw_set_idlist(IShellLinkW *self, LPCITEMIDLIST pidl) {
  LinkObject *l = LINK_W(self);
  LPITEMIDLIST copy = pidl ? ILClone(pidl) : NULL;
  SHFree(l->pidl);
  l->pidl = copy;
  link_set(&l->target, NULL);
  l->dirty = TRUE;
  return S_OK;
}
#define LINK_STRING_ACCESSORS(name, field) \
  static HRESULT STDMETHODCALLTYPE lw_get_##name(IShellLinkW *self, LPWSTR out, int capacity) { \
    LinkObject *l = LINK_W(self); \
    if (!out || capacity <= 0) return E_INVALIDARG; \
    if (!w_copy(out, (size_t)capacity, l->field ? l->field : L"")) return S_FALSE; \
    return S_OK; \
  } \
  static HRESULT STDMETHODCALLTYPE lw_set_##name(IShellLinkW *self, LPCWSTR value) { \
    LinkObject *l = LINK_W(self); \
    link_set(&l->field, value); \
    l->dirty = TRUE; \
    return S_OK; \
  }
LINK_STRING_ACCESSORS(description, description)
LINK_STRING_ACCESSORS(working_dir, working_dir)
LINK_STRING_ACCESSORS(arguments, arguments)
static HRESULT STDMETHODCALLTYPE lw_get_hotkey(IShellLinkW *self, WORD *key) { if (!key) return E_POINTER; *key = LINK_W(self)->hotkey; return S_OK; }
static HRESULT STDMETHODCALLTYPE lw_set_hotkey(IShellLinkW *self, WORD key) { LINK_W(self)->hotkey = key; LINK_W(self)->dirty = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE lw_get_show(IShellLinkW *self, int *show) { if (!show) return E_POINTER; *show = LINK_W(self)->show ? LINK_W(self)->show : SW_SHOWNORMAL; return S_OK; }
static HRESULT STDMETHODCALLTYPE lw_set_show(IShellLinkW *self, int show) { LINK_W(self)->show = show; LINK_W(self)->dirty = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE lw_get_icon(IShellLinkW *self, LPWSTR out, int capacity, int *index) {
  LinkObject *l = LINK_W(self);
  if (!out || capacity <= 0 || !index) return E_INVALIDARG;
  *index = l->icon_index;
  if (!w_copy(out, (size_t)capacity, l->icon_path ? l->icon_path : L"")) return S_FALSE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE lw_set_icon(IShellLinkW *self, LPCWSTR path, int index) {
  LinkObject *l = LINK_W(self);
  link_set(&l->icon_path, path);
  l->icon_index = index;
  l->dirty = TRUE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE lw_set_relative(IShellLinkW *self, LPCWSTR path, DWORD reserved) {
  (void)reserved;
  link_set(&LINK_W(self)->relative_path, path);
  LINK_W(self)->dirty = TRUE;
  return S_OK;
}
/* Checks that the target is there (a missing one is an error: there is no search for moved files and no dialog). */
static HRESULT STDMETHODCALLTYPE lw_resolve(IShellLinkW *self, HWND owner, DWORD flags) {
  LinkObject *l = LINK_W(self);
  WCHAR path[MAX_PATH_EX];
  (void)owner; (void)flags;
  if (!link_target_path(l, path, ARRAY_LEN(path))) return l->pidl ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  if (path_exists(path)) return S_OK;
  return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}
static HRESULT STDMETHODCALLTYPE lw_set_path(IShellLinkW *self, LPCWSTR path) {
  LinkObject *l = LINK_W(self);
  if (!path) return E_INVALIDARG;
  SHFree(l->pidl);
  l->pidl = NULL;
  WCHAR expanded[MAX_PATH_EX];
  const DWORD n = ExpandEnvironmentStringsW(path, expanded, ARRAY_LEN(expanded));
  const WCHAR *use = n && n <= ARRAY_LEN(expanded) ? expanded : path;
  WCHAR found[MAX_PATH_EX];
  if (*use) {
    /* a program given without its extension is the file that has one */
    WCHAR full_try[MAX_PATH_EX];
    if (path_full(use, full_try, ARRAY_LEN(full_try)) && !path_exists(full_try) && !*path_extension(full_try)) {
      static const WCHAR *const kExt[] = {L".exe", L".com", L".bat", L".cmd"};
      for (size_t i = 0; i < ARRAY_LEN(kExt); ++i) {
        w_copy(found, ARRAY_LEN(found), full_try);
        w_cat(found, ARRAY_LEN(found), kExt[i]);
        if (path_exists(found)) { use = found; break; }
      }
    }
  }
  PIDLIST_ABSOLUTE pidl = NULL;
  WCHAR full[MAX_PATH_EX];
  if (!*use) { link_set(&l->target, NULL); l->dirty = TRUE; return S_OK; }
  if (path_full(use, full, ARRAY_LEN(full)) && SUCCEEDED(path_to_pidl(full, FALSE, &pidl, NULL))) { l->pidl = pidl; link_set(&l->target, NULL); }
  else link_set(&l->target, use);
  l->dirty = TRUE;
  return S_OK;
}
static const IShellLinkWVtbl g_link_w_vtbl = {
  lw_query, lw_addref, lw_release, lw_get_path, lw_get_idlist, lw_set_idlist, lw_get_description, lw_set_description, lw_get_working_dir, lw_set_working_dir,
  lw_get_arguments, lw_set_arguments, lw_get_hotkey, lw_set_hotkey, lw_get_show, lw_set_show, lw_get_icon, lw_set_icon, lw_set_relative, lw_resolve, lw_set_path,
};

/* the ANSI interface on the same object */
static HRESULT STDMETHODCALLTYPE la_query(IShellLinkA *self, REFIID riid, void **out) { return lw_query((IShellLinkW *)LINK_FROM_A(self), riid, out); }
static ULONG STDMETHODCALLTYPE la_addref(IShellLinkA *self) { return lw_addref((IShellLinkW *)LINK_FROM_A(self)); }
static ULONG STDMETHODCALLTYPE la_release(IShellLinkA *self) { return lw_release((IShellLinkW *)LINK_FROM_A(self)); }
static HRESULT STDMETHODCALLTYPE la_get_path(IShellLinkA *self, LPSTR file, int capacity, WIN32_FIND_DATAA *find, DWORD flags) {
  WCHAR path[MAX_PATH_EX];
  WIN32_FIND_DATAW fw;
  if (find) ZeroMemory(find, sizeof(*find));
  if (!file || capacity <= 0) return E_INVALIDARG;
  file[0] = 0;
  const HRESULT hr = lw_get_path((IShellLinkW *)LINK_FROM_A(self), path, ARRAY_LEN(path), &fw, flags);
  if (hr != S_OK) return hr;
  if (!w_to_a(path, file, capacity)) return S_FALSE;
  if (find) {
    find->dwFileAttributes = fw.dwFileAttributes; find->ftCreationTime = fw.ftCreationTime; find->ftLastAccessTime = fw.ftLastAccessTime; find->ftLastWriteTime = fw.ftLastWriteTime;
    find->nFileSizeHigh = fw.nFileSizeHigh; find->nFileSizeLow = fw.nFileSizeLow;
    w_to_a(fw.cFileName, find->cFileName, sizeof(find->cFileName));
  }
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE la_get_idlist(IShellLinkA *self, LPITEMIDLIST *pidl) { return lw_get_idlist((IShellLinkW *)LINK_FROM_A(self), pidl); }
static HRESULT STDMETHODCALLTYPE la_set_idlist(IShellLinkA *self, LPCITEMIDLIST pidl) { return lw_set_idlist((IShellLinkW *)LINK_FROM_A(self), pidl); }
#define LINK_A_STRING(name) \
  static HRESULT STDMETHODCALLTYPE la_get_##name(IShellLinkA *self, LPSTR out, int capacity) { \
    WCHAR w[MAX_PATH_EX]; \
    if (!out || capacity <= 0) return E_INVALIDARG; \
    const HRESULT hr = lw_get_##name((IShellLinkW *)LINK_FROM_A(self), w, ARRAY_LEN(w)); \
    if (FAILED(hr)) return hr; \
    return w_to_a(w, out, capacity) ? S_OK : S_FALSE; \
  } \
  static HRESULT STDMETHODCALLTYPE la_set_##name(IShellLinkA *self, LPCSTR value) { \
    WCHAR *w = a_to_w(value); \
    const HRESULT hr = w || !value ? lw_set_##name((IShellLinkW *)LINK_FROM_A(self), w) : E_OUTOFMEMORY; \
    mem_free(w); \
    return hr; \
  }
LINK_A_STRING(description)
LINK_A_STRING(working_dir)
LINK_A_STRING(arguments)
static HRESULT STDMETHODCALLTYPE la_get_hotkey(IShellLinkA *self, WORD *key) { return lw_get_hotkey((IShellLinkW *)LINK_FROM_A(self), key); }
static HRESULT STDMETHODCALLTYPE la_set_hotkey(IShellLinkA *self, WORD key) { return lw_set_hotkey((IShellLinkW *)LINK_FROM_A(self), key); }
static HRESULT STDMETHODCALLTYPE la_get_show(IShellLinkA *self, int *show) { return lw_get_show((IShellLinkW *)LINK_FROM_A(self), show); }
static HRESULT STDMETHODCALLTYPE la_set_show(IShellLinkA *self, int show) { return lw_set_show((IShellLinkW *)LINK_FROM_A(self), show); }
static HRESULT STDMETHODCALLTYPE la_get_icon(IShellLinkA *self, LPSTR out, int capacity, int *index) {
  WCHAR w[MAX_PATH_EX];
  if (!out || capacity <= 0 || !index) return E_INVALIDARG;
  const HRESULT hr = lw_get_icon((IShellLinkW *)LINK_FROM_A(self), w, ARRAY_LEN(w), index);
  if (FAILED(hr)) return hr;
  return w_to_a(w, out, capacity) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE la_set_icon(IShellLinkA *self, LPCSTR path, int index) {
  WCHAR *w = a_to_w(path);
  const HRESULT hr = lw_set_icon((IShellLinkW *)LINK_FROM_A(self), w, index);
  mem_free(w);
  return hr;
}
static HRESULT STDMETHODCALLTYPE la_set_relative(IShellLinkA *self, LPCSTR path, DWORD reserved) {
  WCHAR *w = a_to_w(path);
  const HRESULT hr = lw_set_relative((IShellLinkW *)LINK_FROM_A(self), w, reserved);
  mem_free(w);
  return hr;
}
static HRESULT STDMETHODCALLTYPE la_resolve(IShellLinkA *self, HWND owner, DWORD flags) { return lw_resolve((IShellLinkW *)LINK_FROM_A(self), owner, flags); }
static HRESULT STDMETHODCALLTYPE la_set_path(IShellLinkA *self, LPCSTR path) {
  if (!path) return E_INVALIDARG;
  WCHAR *w = a_to_w(path);
  const HRESULT hr = w ? lw_set_path((IShellLinkW *)LINK_FROM_A(self), w) : E_OUTOFMEMORY;
  mem_free(w);
  return hr;
}
static const IShellLinkAVtbl g_link_a_vtbl = {
  la_query, la_addref, la_release, la_get_path, la_get_idlist, la_set_idlist, la_get_description, la_set_description, la_get_working_dir, la_set_working_dir,
  la_get_arguments, la_set_arguments, la_get_hotkey, la_set_hotkey, la_get_show, la_set_show, la_get_icon, la_set_icon, la_set_relative, la_resolve, la_set_path,
};

/* ---- the file format ----------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { BYTE *data; size_t size, capacity; BOOL failed; } Bytes;
static void bytes_put(Bytes *b, const void *p, size_t n) {
  if (b->failed) return;
  if (n > 0x7FFFFFFF - b->size) { b->failed = TRUE; return; }
  if (b->size + n > b->capacity) {
    size_t want = b->capacity ? b->capacity * 2 : 512;
    while (want < b->size + n) want *= 2;
    BYTE *grown = mem_realloc(b->data, want);
    if (!grown) { b->failed = TRUE; return; }
    b->data = grown;
    b->capacity = want;
  }
  CopyMemory(b->data + b->size, p, n);
  b->size += n;
}
static void bytes_put32(Bytes *b, DWORD v) { bytes_put(b, &v, 4); }
static void bytes_put16(Bytes *b, WORD v) { bytes_put(b, &v, 2); }
/* A string with its 16-bit character count. */
static void bytes_put_string(Bytes *b, const WCHAR *s) {
  const size_t n = wlen(s);
  bytes_put16(b, (WORD)(n > 0xFFFF ? 0xFFFF : n));
  bytes_put(b, s, (n > 0xFFFF ? 0xFFFF : n) * sizeof(WCHAR));
}

static BOOL link_serialize(const LinkObject *l, Bytes *out) {
  DWORD flags = LNK_UNICODE | LNK_FORCE_NO_LINKINFO;
  DWORD attributes = 0, size = 0;
  FILETIME created = {0}, accessed = {0}, written = {0};
  WCHAR path[MAX_PATH_EX];
  LPITEMIDLIST pidl = l->pidl ? ILClone(l->pidl) : (l->target ? SHSimpleIDListFromPath(l->target) : NULL);
  if (pidl) flags |= LNK_HAS_IDLIST;
  if (link_target_path(l, path, ARRAY_LEN(path))) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &data)) { attributes = data.dwFileAttributes; created = data.ftCreationTime; accessed = data.ftLastAccessTime; written = data.ftLastWriteTime; size = data.nFileSizeLow; }
  }
  if (l->description && *l->description) flags |= LNK_HAS_NAME;
  if (l->relative_path && *l->relative_path) flags |= LNK_HAS_RELATIVE_PATH;
  if (l->working_dir && *l->working_dir) flags |= LNK_HAS_WORKING_DIR;
  if (l->arguments && *l->arguments) flags |= LNK_HAS_ARGUMENTS;
  if (l->icon_path && *l->icon_path) flags |= LNK_HAS_ICON;
  bytes_put32(out, 0x4C);
  bytes_put(out, &CLSID_ShellLink_, 16);
  bytes_put32(out, flags);
  bytes_put32(out, attributes);
  bytes_put(out, &created, 8);
  bytes_put(out, &accessed, 8);
  bytes_put(out, &written, 8);
  bytes_put32(out, size);
  bytes_put32(out, (DWORD)l->icon_index);
  bytes_put32(out, (DWORD)(l->show ? l->show : SW_SHOWNORMAL));
  bytes_put16(out, l->hotkey);
  bytes_put16(out, 0);
  bytes_put32(out, 0);
  bytes_put32(out, 0);
  if (pidl) { const UINT n = ILGetSize(pidl); bytes_put16(out, (WORD)(n > 0xFFFF ? 0xFFFF : n)); bytes_put(out, pidl, n); }
  if (flags & LNK_HAS_NAME) bytes_put_string(out, l->description);
  if (flags & LNK_HAS_RELATIVE_PATH) bytes_put_string(out, l->relative_path);
  if (flags & LNK_HAS_WORKING_DIR) bytes_put_string(out, l->working_dir);
  if (flags & LNK_HAS_ARGUMENTS) bytes_put_string(out, l->arguments);
  if (flags & LNK_HAS_ICON) bytes_put_string(out, l->icon_path);
  bytes_put32(out, 0);      /* the end of the extra data */
  SHFree(pidl);
  return !out->failed;
}

/* A reader that never goes past the end. */
typedef struct { const BYTE *data; size_t size, position; BOOL failed; } Reader;
static const BYTE *reader_take(Reader *r, size_t n) {
  if (r->failed || n > r->size - r->position) { r->failed = TRUE; return NULL; }
  const BYTE *p = r->data + r->position;
  r->position += n;
  return p;
}
static DWORD reader_u32(Reader *r) { DWORD v = 0; const BYTE *p = reader_take(r, 4); if (p) CopyMemory(&v, p, 4); return v; }
static WORD reader_u16(Reader *r) { WORD v = 0; const BYTE *p = reader_take(r, 2); if (p) CopyMemory(&v, p, 2); return v; }
static WCHAR *reader_string(Reader *r, BOOL unicode) {
  const WORD n = reader_u16(r);
  const BYTE *p = reader_take(r, (size_t)n * (unicode ? 2 : 1));
  if (!p) return NULL;
  WCHAR *s = mem_alloc(((size_t)n + 1) * sizeof(WCHAR));
  if (!s) { r->failed = TRUE; return NULL; }
  if (unicode) CopyMemory(s, p, (size_t)n * 2);
  else MultiByteToWideChar(CP_ACP, 0, (const char *)p, n, s, n);
  return s;
}
/* Is the list well formed (items inside its bytes, ended by 0)? */
static BOOL idlist_valid(const BYTE *p, size_t size) {
  size_t at = 0;
  while (at + 2 <= size) {
    USHORT cb;
    CopyMemory(&cb, p + at, 2);
    if (!cb) return TRUE;
    if (cb < 2 || cb > size - at) return FALSE;
    at += cb;
  }
  return FALSE;
}
static HRESULT link_parse(LinkObject *l, const BYTE *data, size_t size) {
  Reader r = {data, size, 0, FALSE};
  if (reader_u32(&r) != 0x4C) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  const BYTE *clsid = reader_take(&r, 16);
  if (!clsid || memcmp(clsid, &CLSID_ShellLink_, 16)) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  const DWORD flags = reader_u32(&r);
  reader_take(&r, 4 + 24 + 4);      /* attributes, the three times, the size */
  const int icon_index = (int)reader_u32(&r);
  const int show = (int)reader_u32(&r);
  const WORD hotkey = reader_u16(&r);
  reader_take(&r, 2 + 4 + 4);
  if (r.failed) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  LPITEMIDLIST pidl = NULL;
  WCHAR *base_path = NULL;
  if (flags & LNK_HAS_IDLIST) {
    const WORD n = reader_u16(&r);
    const BYTE *p = reader_take(&r, n);
    if (!p || !idlist_valid(p, n)) return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    pidl = il_new((size_t)n + 2);
    if (!pidl) return E_OUTOFMEMORY;
    CopyMemory(pidl, p, n);
    ((BYTE *)pidl)[n] = ((BYTE *)pidl)[n + 1] = 0;
  }
  if (flags & LNK_HAS_LINKINFO) {
    const size_t start = r.position;
    const DWORD info_size = reader_u32(&r);
    const BYTE *info = (info_size >= 28 && info_size <= r.size - start) ? r.data + start : NULL;
    if (!info) { SHFree(pidl); return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT); }
    DWORD info_flags, local_offset;
    CopyMemory(&info_flags, info + 8, 4);
    CopyMemory(&local_offset, info + 16, 4);
    if ((info_flags & 1) && local_offset < info_size) {
      const char *a = (const char *)info + local_offset;
      size_t n = 0;
      while (local_offset + n < info_size && a[n]) ++n;
      base_path = mem_alloc((n + 1) * sizeof(WCHAR));
      if (base_path) MultiByteToWideChar(CP_ACP, 0, a, (int)n, base_path, (int)n);
    }
    r.position = start + info_size;
  }
  const BOOL unicode = (flags & LNK_UNICODE) != 0;
  WCHAR *name = NULL, *relative = NULL, *dir = NULL, *args = NULL, *icon = NULL;
  if (flags & LNK_HAS_NAME) name = reader_string(&r, unicode);
  if (flags & LNK_HAS_RELATIVE_PATH) relative = reader_string(&r, unicode);
  if (flags & LNK_HAS_WORKING_DIR) dir = reader_string(&r, unicode);
  if (flags & LNK_HAS_ARGUMENTS) args = reader_string(&r, unicode);
  if (flags & LNK_HAS_ICON) icon = reader_string(&r, unicode);
  if (r.failed) {
    SHFree(pidl); mem_free(base_path); mem_free(name); mem_free(relative); mem_free(dir); mem_free(args); mem_free(icon);
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  }
  SHFree(l->pidl);
  l->pidl = pidl;
  link_set(&l->target, NULL);
  WCHAR path[MAX_PATH_EX];
  if (base_path && !(pidl && pidl_to_path(pidl, path, ARRAY_LEN(path)))) { l->target = base_path; base_path = NULL; }
  mem_free(base_path);
  mem_free(l->description); l->description = name;
  mem_free(l->relative_path); l->relative_path = relative;
  mem_free(l->working_dir); l->working_dir = dir;
  mem_free(l->arguments); l->arguments = args;
  mem_free(l->icon_path); l->icon_path = icon;
  l->icon_index = icon_index;
  l->show = show;
  l->hotkey = hotkey;
  l->dirty = FALSE;
  return S_OK;
}

/* ---- IPersistFile ----------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE lf_query(IPersistFile *self, REFIID riid, void **out) { return lw_query((IShellLinkW *)LINK_FROM_FILE(self), riid, out); }
static ULONG STDMETHODCALLTYPE lf_addref(IPersistFile *self) { return lw_addref((IShellLinkW *)LINK_FROM_FILE(self)); }
static ULONG STDMETHODCALLTYPE lf_release(IPersistFile *self) { return lw_release((IShellLinkW *)LINK_FROM_FILE(self)); }
static HRESULT STDMETHODCALLTYPE lf_class_id(IPersistFile *self, CLSID *id) { (void)self; if (!id) return E_POINTER; *id = CLSID_ShellLink_; return S_OK; }
static HRESULT STDMETHODCALLTYPE lf_is_dirty(IPersistFile *self) { return LINK_FROM_FILE(self)->dirty ? S_OK : S_FALSE; }
static HRESULT STDMETHODCALLTYPE lf_load(IPersistFile *self, LPCOLESTR file, DWORD mode) {
  LinkObject *l = LINK_FROM_FILE(self);
  (void)mode;
  if (!file) return E_INVALIDARG;
  HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return hr_last_error();
  LARGE_INTEGER size;
  HRESULT hr = E_FAIL;
  if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= 1024 * 1024) {
    BYTE *data = mem_alloc((size_t)size.QuadPart);
    DWORD got = 0;
    if (data && ReadFile(h, data, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart) hr = link_parse(l, data, got);
    else if (!data) hr = E_OUTOFMEMORY;
    mem_free(data);
  } else hr = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
  CloseHandle(h);
  if (SUCCEEDED(hr)) link_set(&l->current_file, file);
  return hr;
}
static HRESULT STDMETHODCALLTYPE lf_save(IPersistFile *self, LPCOLESTR file, BOOL remember) {
  LinkObject *l = LINK_FROM_FILE(self);
  const WCHAR *target_file = file ? file : l->current_file;
  if (!target_file) return E_INVALIDARG;
  Bytes b = {0};
  if (!link_serialize(l, &b)) { mem_free(b.data); return E_OUTOFMEMORY; }
  HANDLE h = CreateFileW(target_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) { mem_free(b.data); return hr_last_error(); }
  DWORD done = 0;
  const BOOL ok = WriteFile(h, b.data, (DWORD)b.size, &done, NULL) && done == b.size;
  CloseHandle(h);
  mem_free(b.data);
  if (!ok) return hr_last_error();
  if (remember || !file) { link_set(&l->current_file, target_file); l->dirty = FALSE; }
  change_notify(SHCNE_CREATE, target_file, NULL);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE lf_save_completed(IPersistFile *self, LPCOLESTR file) { (void)self; (void)file; return S_OK; }
static HRESULT STDMETHODCALLTYPE lf_cur_file(IPersistFile *self, LPOLESTR *file) {
  LinkObject *l = LINK_FROM_FILE(self);
  if (!file) return E_POINTER;
  *file = NULL;
  if (!l->current_file) return S_FALSE;
  *file = w_task_dup(l->current_file);
  return *file ? S_OK : E_OUTOFMEMORY;
}
static const IPersistFileVtbl g_link_file_vtbl = {lf_query, lf_addref, lf_release, lf_class_id, lf_is_dirty, lf_load, lf_save, lf_save_completed, lf_cur_file};

/* ---- IPersistStream --------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE ls_query(IPersistStream *self, REFIID riid, void **out) { return lw_query((IShellLinkW *)LINK_FROM_STREAM(self), riid, out); }
static ULONG STDMETHODCALLTYPE ls_addref(IPersistStream *self) { return lw_addref((IShellLinkW *)LINK_FROM_STREAM(self)); }
static ULONG STDMETHODCALLTYPE ls_release(IPersistStream *self) { return lw_release((IShellLinkW *)LINK_FROM_STREAM(self)); }
static HRESULT STDMETHODCALLTYPE ls_class_id(IPersistStream *self, CLSID *id) { (void)self; if (!id) return E_POINTER; *id = CLSID_ShellLink_; return S_OK; }
static HRESULT STDMETHODCALLTYPE ls_is_dirty(IPersistStream *self) { return LINK_FROM_STREAM(self)->dirty ? S_OK : S_FALSE; }
static HRESULT STDMETHODCALLTYPE ls_load(IPersistStream *self, IStream *stream) {
  LinkObject *l = LINK_FROM_STREAM(self);
  Bytes b = {0};
  BYTE chunk[512];
  ULONG got = 0;
  if (!stream) return E_INVALIDARG;
  do {
    const HRESULT hr = IStream_Read(stream, chunk, sizeof(chunk), &got);
    if (FAILED(hr)) { mem_free(b.data); return hr; }
    bytes_put(&b, chunk, got);
    if (b.size > 1024 * 1024) break;
  } while (got == sizeof(chunk) && !b.failed);
  const HRESULT hr = b.failed ? E_OUTOFMEMORY : link_parse(l, b.data, b.size);
  mem_free(b.data);
  return hr;
}
static HRESULT STDMETHODCALLTYPE ls_save(IPersistStream *self, IStream *stream, BOOL clear_dirty) {
  LinkObject *l = LINK_FROM_STREAM(self);
  Bytes b = {0};
  if (!stream) return E_INVALIDARG;
  if (!link_serialize(l, &b)) { mem_free(b.data); return E_OUTOFMEMORY; }
  ULONG done = 0;
  const HRESULT hr = IStream_Write(stream, b.data, (ULONG)b.size, &done);
  mem_free(b.data);
  if (SUCCEEDED(hr) && clear_dirty) l->dirty = FALSE;
  return hr;
}
static HRESULT STDMETHODCALLTYPE ls_size_max(IPersistStream *self, ULARGE_INTEGER *size) {
  Bytes b = {0};
  if (!size) return E_POINTER;
  if (!link_serialize(LINK_FROM_STREAM(self), &b)) { mem_free(b.data); return E_OUTOFMEMORY; }
  size->QuadPart = b.size;
  mem_free(b.data);
  return S_OK;
}
static const IPersistStreamVtbl g_link_stream_vtbl = {ls_query, ls_addref, ls_release, ls_class_id, ls_is_dirty, ls_load, ls_save, ls_size_max};

/* the data blocks of a shortcut are not kept; the flags are */
static HRESULT STDMETHODCALLTYPE ld_query(IShellLinkDataList *self, REFIID riid, void **out) { return lw_query((IShellLinkW *)LINK_FROM_DATA(self), riid, out); }
static ULONG STDMETHODCALLTYPE ld_addref(IShellLinkDataList *self) { return lw_addref((IShellLinkW *)LINK_FROM_DATA(self)); }
static ULONG STDMETHODCALLTYPE ld_release(IShellLinkDataList *self) { return lw_release((IShellLinkW *)LINK_FROM_DATA(self)); }
static HRESULT STDMETHODCALLTYPE ld_add(IShellLinkDataList *self, void *block) { (void)self; (void)block; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE ld_copy(IShellLinkDataList *self, DWORD signature, void **block) { (void)self; (void)signature; if (block) *block = NULL; return E_FAIL; }
static HRESULT STDMETHODCALLTYPE ld_remove(IShellLinkDataList *self, DWORD signature) { (void)self; (void)signature; return E_FAIL; }
static HRESULT STDMETHODCALLTYPE ld_get_flags(IShellLinkDataList *self, DWORD *flags) { if (!flags) return E_INVALIDARG; *flags = LINK_FROM_DATA(self)->data_flags; return S_OK; }
static HRESULT STDMETHODCALLTYPE ld_set_flags(IShellLinkDataList *self, DWORD flags) { LINK_FROM_DATA(self)->data_flags = flags; LINK_FROM_DATA(self)->dirty = TRUE; return S_OK; }
static const IShellLinkDataListVtbl g_link_data_vtbl = {ld_query, ld_addref, ld_release, ld_add, ld_copy, ld_remove, ld_get_flags, ld_set_flags};

static HRESULT link_create(REFIID riid, void **out) {
  LinkObject *l = mem_alloc(sizeof(*l));
  if (!l) return E_OUTOFMEMORY;
  l->w = &g_link_w_vtbl;
  l->a = &g_link_a_vtbl;
  l->file = &g_link_file_vtbl;
  l->stream = &g_link_stream_vtbl;
  l->data_list = &g_link_data_vtbl;
  l->ref = 1;
  l->show = SW_SHOWNORMAL;
  objects_add();
  const HRESULT hr = lw_query((IShellLinkW *)l, riid, out);
  lw_release((IShellLinkW *)l);
  return hr;
}

/* ---- class factories (DllGetClassObject) ------------------------------------------------------------------------------------------------------------------------- */

typedef struct { const IClassFactoryVtbl *vtbl; LONG ref; GUID clsid; } Factory;
static HRESULT STDMETHODCALLTYPE cf_query(IClassFactory *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) { *out = self; InterlockedIncrement(&((Factory *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(IClassFactory *self) { return (ULONG)InterlockedIncrement(&((Factory *)self)->ref); }
static ULONG STDMETHODCALLTYPE cf_release(IClassFactory *self) {
  Factory *f = (Factory *)self;
  const LONG n = InterlockedDecrement(&f->ref);
  if (!n) { mem_free(f); objects_remove(); }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE cf_create(IClassFactory *self, IUnknown *outer, REFIID riid, void **out) {
  Factory *f = (Factory *)self;
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  if (IsEqualGUID(&f->clsid, &CLSID_ShellLink_)) return link_create(riid, out);
  if (IsEqualGUID(&f->clsid, &CLSID_KnownFolderManager_)) return known_folder_manager_create(riid, out);
  if (IsEqualGUID(&f->clsid, &CLSID_ShellItem_)) {
    LPITEMIDLIST empty = il_new(sizeof(USHORT));
    if (!empty) return E_OUTOFMEMORY;
    const HRESULT hr = shell_item_new(empty, riid, out);
    SHFree(empty);
    return hr;
  }
  if (IsEqualGUID(&f->clsid, &CLSID_ShellDesktop_)) return folder_create(FOLDER_DESKTOP, NULL, NULL, riid, out);
  if (IsEqualGUID(&f->clsid, &CLSID_MyComputer_)) {
    LPITEMIDLIST pidl = il_guid(&CLSID_MyComputer_);
    const HRESULT hr = folder_create(FOLDER_COMPUTER, pidl, NULL, riid, out);
    SHFree(pidl);
    return hr;
  }
  if (IsEqualGUID(&f->clsid, &CLSID_ShellFSFolder_)) return folder_create(FOLDER_FILES, NULL, NULL, riid, out);
  return CLASS_E_CLASSNOTAVAILABLE;
}
static HRESULT STDMETHODCALLTYPE cf_lock(IClassFactory *self, BOOL lock) { (void)self; if (lock) objects_add(); else objects_remove(); return S_OK; }
static const IClassFactoryVtbl g_factory_vtbl = {cf_query, cf_addref, cf_release, cf_create, cf_lock};

EXPORT HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!IsEqualGUID(clsid, &CLSID_ShellLink_) && !IsEqualGUID(clsid, &CLSID_ShellItem_) && !IsEqualGUID(clsid, &CLSID_ShellDesktop_) && !IsEqualGUID(clsid, &CLSID_MyComputer_) &&
      !IsEqualGUID(clsid, &CLSID_ShellFSFolder_) && !IsEqualGUID(clsid, &CLSID_KnownFolderManager_))
    return CLASS_E_CLASSNOTAVAILABLE;
  Factory *f = mem_alloc(sizeof(*f));
  if (!f) return E_OUTOFMEMORY;
  f->vtbl = &g_factory_vtbl;
  f->ref = 1;
  f->clsid = *clsid;
  objects_add();
  const HRESULT hr = cf_query((IClassFactory *)f, riid, out);
  cf_release((IClassFactory *)f);
  return hr;
}
