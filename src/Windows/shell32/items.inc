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

/* Shell items (IShellItem, IShellItemArray), the data object of a set of files (the clipboard formats of drag and drop) and the GetUIObjectOf of the file system folders.
 * Included by shell32.c. */

/* ---- clipboard formats ------------------------------------------------------------------------------------------------------------------------------------ */

/* user32.dll's RegisterClipboardFormat when it works, otherwise ids of our own (the window system is not there to keep the table). */
static CLIPFORMAT register_format(const WCHAR *name) {
  static WCHAR names[16][40];
  static int used;
  CLIPFORMAT f = (CLIPFORMAT)RegisterClipboardFormatW(name);
  if (f) return f;
  EnterCriticalSection(&g_lock);
  int i = 0;
  for (; i < used; ++i) if (!wicmp(names[i], name)) break;
  if (i == used && used < 16) { w_copy(names[used], ARRAY_LEN(names[0]), name); ++used; }
  LeaveCriticalSection(&g_lock);
  return (CLIPFORMAT)(0xC100 + i);
}
static CLIPFORMAT format_shell_idlist(void) { return register_format(L"Shell IDList Array"); }

/* ---- IEnumFORMATETC ---------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { const IEnumFORMATETCVtbl *vtbl; LONG ref; FORMATETC *formats; ULONG count, position; } EnumFormats;

static HRESULT STDMETHODCALLTYPE ef_query(IEnumFORMATETC *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumFORMATETC)) { *out = self; InterlockedIncrement(&((EnumFormats *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ef_addref(IEnumFORMATETC *self) { return (ULONG)InterlockedIncrement(&((EnumFormats *)self)->ref); }
static ULONG STDMETHODCALLTYPE ef_release(IEnumFORMATETC *self) {
  EnumFormats *e = (EnumFormats *)self;
  const LONG n = InterlockedDecrement(&e->ref);
  if (!n) { mem_free(e->formats); mem_free(e); objects_remove(); }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE ef_next(IEnumFORMATETC *self, ULONG wanted, FORMATETC *out, ULONG *fetched) {
  EnumFormats *e = (EnumFormats *)self;
  ULONG got = 0;
  if (!out || (!fetched && wanted != 1)) return E_INVALIDARG;
  while (got < wanted && e->position < e->count) out[got++] = e->formats[e->position++];
  if (fetched) *fetched = got;
  return got == wanted ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE ef_skip(IEnumFORMATETC *self, ULONG n) {
  EnumFormats *e = (EnumFormats *)self;
  if (n > e->count - e->position) { e->position = e->count; return S_FALSE; }
  e->position += n;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ef_reset(IEnumFORMATETC *self) { ((EnumFormats *)self)->position = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE ef_clone(IEnumFORMATETC *self, IEnumFORMATETC **out);
static const IEnumFORMATETCVtbl g_enum_formats_vtbl = {ef_query, ef_addref, ef_release, ef_next, ef_skip, ef_reset, ef_clone};

static HRESULT enum_formats_new(const FORMATETC *formats, ULONG count, ULONG position, IEnumFORMATETC **out) {
  EnumFormats *e = mem_alloc(sizeof(*e));
  if (!e) return E_OUTOFMEMORY;
  e->formats = mem_alloc((count ? count : 1) * sizeof(FORMATETC));
  if (!e->formats) { mem_free(e); return E_OUTOFMEMORY; }
  CopyMemory(e->formats, formats, count * sizeof(FORMATETC));
  e->vtbl = &g_enum_formats_vtbl;
  e->ref = 1;
  e->count = count;
  e->position = position;
  objects_add();
  *out = (IEnumFORMATETC *)e;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ef_clone(IEnumFORMATETC *self, IEnumFORMATETC **out) {
  EnumFormats *e = (EnumFormats *)self;
  return enum_formats_new(e->formats, e->count, e->position, out);
}
EXPORT HRESULT WINAPI SHCreateStdEnumFmtEtc(UINT count, const FORMATETC *formats, IEnumFORMATETC **out) {
  if (!out || (count && !formats)) return E_INVALIDARG;
  return enum_formats_new(formats, count, 0, out);
}

/* ---- the data object of a set of files ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  const IDataObjectVtbl *vtbl;
  LONG ref;
  LPITEMIDLIST *pidls;      /* absolute */
  UINT count;
} FilesDataObject;

static HRESULT STDMETHODCALLTYPE do_query(IDataObject *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) { *out = self; InterlockedIncrement(&((FilesDataObject *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE do_addref(IDataObject *self) { return (ULONG)InterlockedIncrement(&((FilesDataObject *)self)->ref); }
static ULONG STDMETHODCALLTYPE do_release(IDataObject *self) {
  FilesDataObject *d = (FilesDataObject *)self;
  const LONG n = InterlockedDecrement(&d->ref);
  if (!n) {
    for (UINT i = 0; i < d->count; ++i) SHFree(d->pidls[i]);
    mem_free(d->pidls);
    mem_free(d);
    objects_remove();
  }
  return (ULONG)n;
}

/* CF_HDROP: a DROPFILES followed by the paths, each ended by 0, then one more 0. */
static HGLOBAL make_hdrop(LPITEMIDLIST *pidls, UINT count) {
  size_t chars = 1;
  WCHAR path[MAX_PATH_EX];
  for (UINT i = 0; i < count; ++i) if (pidl_to_path(pidls[i], path, ARRAY_LEN(path))) chars += wlen(path) + 1;
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(WCHAR));
  if (!h) return NULL;
  DROPFILES *drop = GlobalLock(h);
  drop->pFiles = sizeof(DROPFILES);
  drop->fWide = TRUE;
  WCHAR *p = (WCHAR *)(drop + 1);
  for (UINT i = 0; i < count; ++i) {
    if (!pidl_to_path(pidls[i], path, ARRAY_LEN(path))) continue;
    const size_t n = wlen(path) + 1;
    CopyMemory(p, path, n * sizeof(WCHAR));
    p += n;
  }
  *p = 0;
  GlobalUnlock(h);
  return h;
}
/* The "Shell IDList Array": the parent folder's list, then the child items. */
static HGLOBAL make_idlist_array(LPITEMIDLIST *pidls, UINT count) {
  if (!count) return NULL;
  LPITEMIDLIST parent = ILClone(pidls[0]);
  if (!parent) return NULL;
  ILRemoveLastID(parent);
  size_t total = sizeof(UINT) * (count + 2) + ILGetSize(parent);
  for (UINT i = 0; i < count; ++i) total += ILGetSize(ILFindLastID(pidls[i]));
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
  if (!h) { SHFree(parent); return NULL; }
  CIDA *cida = GlobalLock(h);
  BYTE *base = (BYTE *)cida;
  cida->cidl = count;
  UINT offset = (UINT)(sizeof(UINT) * (count + 2));
  cida->aoffset[0] = offset;
  CopyMemory(base + offset, parent, ILGetSize(parent));
  offset += ILGetSize(parent);
  for (UINT i = 0; i < count; ++i) {
    LPITEMIDLIST child = ILFindLastID(pidls[i]);
    cida->aoffset[i + 1] = offset;
    CopyMemory(base + offset, child, ILGetSize(child));
    offset += ILGetSize(child);
  }
  GlobalUnlock(h);
  SHFree(parent);
  return h;
}

static HRESULT STDMETHODCALLTYPE do_get_data(IDataObject *self, FORMATETC *fmt, STGMEDIUM *medium) {
  FilesDataObject *d = (FilesDataObject *)self;
  if (!fmt || !medium) return E_INVALIDARG;
  ZeroMemory(medium, sizeof(*medium));
  if (!(fmt->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
  HGLOBAL h = NULL;
  if (fmt->cfFormat == CF_HDROP) h = make_hdrop(d->pidls, d->count);
  else if (fmt->cfFormat == format_shell_idlist()) h = make_idlist_array(d->pidls, d->count);
  else return DV_E_FORMATETC;
  if (!h) return E_OUTOFMEMORY;
  medium->tymed = TYMED_HGLOBAL;
  medium->hGlobal = h;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE do_get_data_here(IDataObject *self, FORMATETC *fmt, STGMEDIUM *medium) { (void)self; (void)fmt; (void)medium; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE do_query_get(IDataObject *self, FORMATETC *fmt) {
  (void)self;
  if (!fmt) return E_INVALIDARG;
  if (fmt->cfFormat != CF_HDROP && fmt->cfFormat != format_shell_idlist()) return DV_E_FORMATETC;
  if (fmt->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
  return (fmt->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
}
static HRESULT STDMETHODCALLTYPE do_canonical(IDataObject *self, FORMATETC *in, FORMATETC *out) { (void)self; if (out) { *out = *in; out->ptd = NULL; } return DATA_S_SAMEFORMATETC; }
static HRESULT STDMETHODCALLTYPE do_set_data(IDataObject *self, FORMATETC *fmt, STGMEDIUM *medium, BOOL release) { (void)self; (void)fmt; (void)medium; (void)release; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE do_enum_formats(IDataObject *self, DWORD direction, IEnumFORMATETC **out) {
  (void)self;
  if (direction != DATADIR_GET) return E_NOTIMPL;
  FORMATETC formats[2];
  ZeroMemory(formats, sizeof(formats));
  formats[0].cfFormat = format_shell_idlist();
  formats[1].cfFormat = CF_HDROP;
  for (int i = 0; i < 2; ++i) { formats[i].dwAspect = DVASPECT_CONTENT; formats[i].lindex = -1; formats[i].tymed = TYMED_HGLOBAL; }
  return enum_formats_new(formats, 2, 0, out);
}
static HRESULT STDMETHODCALLTYPE do_advise(IDataObject *self, FORMATETC *fmt, DWORD flags, IAdviseSink *sink, DWORD *connection) { (void)self; (void)fmt; (void)flags; (void)sink; if (connection) *connection = 0; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE do_unadvise(IDataObject *self, DWORD connection) { (void)self; (void)connection; return OLE_E_NOCONNECTION; }
static HRESULT STDMETHODCALLTYPE do_enum_advise(IDataObject *self, IEnumSTATDATA **out) { (void)self; if (out) *out = NULL; return OLE_E_ADVISENOTSUPPORTED; }
static const IDataObjectVtbl g_data_object_vtbl = {do_query, do_addref, do_release, do_get_data, do_get_data_here, do_query_get, do_canonical, do_set_data, do_enum_formats, do_advise, do_unadvise, do_enum_advise};

/* `pidls` are cloned. */
static HRESULT data_object_new(LPITEMIDLIST *pidls, UINT count, IDataObject **out) {
  FilesDataObject *d = mem_alloc(sizeof(*d));
  if (!d) return E_OUTOFMEMORY;
  d->pidls = mem_alloc((count ? count : 1) * sizeof(LPITEMIDLIST));
  if (!d->pidls) { mem_free(d); return E_OUTOFMEMORY; }
  for (UINT i = 0; i < count; ++i) d->pidls[i] = ILClone(pidls[i]);
  d->vtbl = &g_data_object_vtbl;
  d->ref = 1;
  d->count = count;
  objects_add();
  *out = (IDataObject *)d;
  return S_OK;
}

/* The data object of items of one folder given as children. */
static HRESULT folder_data_object(FolderObject *f, UINT count, PCUITEMID_CHILD_ARRAY items, IDataObject **out) {
  LPITEMIDLIST *abs = mem_alloc((count ? count : 1) * sizeof(LPITEMIDLIST));
  if (!abs) return E_OUTOFMEMORY;
  for (UINT i = 0; i < count; ++i) abs[i] = ILCombine(f->pidl, items[i]);
  const HRESULT hr = data_object_new(abs, count, out);
  for (UINT i = 0; i < count; ++i) SHFree(abs[i]);
  mem_free(abs);
  return hr;
}
static HRESULT STDMETHODCALLTYPE sf_ui_object(IShellFolder2 *self, HWND hwnd, UINT count, PCUITEMID_CHILD_ARRAY items, REFIID riid, UINT *reserved, void **out) {
  FolderObject *f = SELF(self);
  (void)hwnd; (void)reserved;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!count || !items) return E_INVALIDARG;
  if (IsEqualIID(riid, &IID_IDataObject)) return folder_data_object(f, count, items, (IDataObject **)out);
  return E_NOINTERFACE;      /* (no context menus, drop targets or icons without a window system) */
}

EXPORT HRESULT WINAPI CIDLData_CreateFromIDArray(PCIDLIST_ABSOLUTE parent, UINT count, PCUIDLIST_RELATIVE_ARRAY items, IDataObject **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!parent) return E_INVALIDARG;
  LPITEMIDLIST *abs = mem_alloc((count ? count : 1) * sizeof(LPITEMIDLIST));
  if (!abs) return E_OUTOFMEMORY;
  for (UINT i = 0; i < count; ++i) abs[i] = ILCombine(parent, items[i]);
  const HRESULT hr = data_object_new(abs, count, out);
  for (UINT i = 0; i < count; ++i) SHFree(abs[i]);
  mem_free(abs);
  return hr;
}
EXPORT HRESULT WINAPI SHCreateDataObject(PCIDLIST_ABSOLUTE parent, UINT count, PCUITEMID_CHILD_ARRAY items, IDataObject *inner, REFIID riid, void **out) {
  (void)inner;
  IDataObject *obj = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = CIDLData_CreateFromIDArray(parent, count, items, &obj);
  if (FAILED(hr)) return hr;
  hr = IDataObject_QueryInterface(obj, riid, out);
  IDataObject_Release(obj);
  return hr;
}

/* ---- IShellItem ----------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  const IShellItemVtbl *item;
  const IPersistIDListVtbl *persist;
  LONG ref;
  LPITEMIDLIST pidl;      /* absolute */
} ShellItemData;
#define ITEM_FROM_PERSIST(p) ((ShellItemData *)((BYTE *)(p) - offsetof(ShellItemData, persist)))
static HRESULT shell_item_new(LPCITEMIDLIST pidl, REFIID riid, void **out);

static HRESULT STDMETHODCALLTYPE si_query(IShellItem *self, REFIID riid, void **out) {
  ShellItemData *s = (ShellItemData *)self;
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IShellItem)) *out = &s->item;
  else if (IsEqualIID(riid, &IID_IPersistIDList) || IsEqualIID(riid, &IID_IPersist)) *out = &s->persist;
  else return E_NOINTERFACE;
  InterlockedIncrement(&s->ref);
  return S_OK;
}
static ULONG STDMETHODCALLTYPE si_addref(IShellItem *self) { return (ULONG)InterlockedIncrement(&((ShellItemData *)self)->ref); }
static ULONG STDMETHODCALLTYPE si_release(IShellItem *self) {
  ShellItemData *s = (ShellItemData *)self;
  const LONG n = InterlockedDecrement(&s->ref);
  if (!n) { SHFree(s->pidl); mem_free(s); objects_remove(); }
  return (ULONG)n;
}

/* The enumerator of IEnumShellItems: items of a folder as shell items. */
typedef struct { const IEnumShellItemsVtbl *vtbl; LONG ref; LPITEMIDLIST parent; LPITEMIDLIST *items; size_t count, position; } EnumShellItems;
static HRESULT STDMETHODCALLTYPE esi_query(IEnumShellItems *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumShellItems)) { *out = self; InterlockedIncrement(&((EnumShellItems *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE esi_addref(IEnumShellItems *self) { return (ULONG)InterlockedIncrement(&((EnumShellItems *)self)->ref); }
static ULONG STDMETHODCALLTYPE esi_release(IEnumShellItems *self) {
  EnumShellItems *e = (EnumShellItems *)self;
  const LONG n = InterlockedDecrement(&e->ref);
  if (!n) {
    for (size_t i = 0; i < e->count; ++i) SHFree(e->items[i]);
    mem_free(e->items); SHFree(e->parent); mem_free(e); objects_remove();
  }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE esi_next(IEnumShellItems *self, ULONG wanted, IShellItem **out, ULONG *fetched) {
  EnumShellItems *e = (EnumShellItems *)self;
  ULONG got = 0;
  if (!out || (!fetched && wanted != 1)) return E_INVALIDARG;
  while (got < wanted && e->position < e->count) {
    LPITEMIDLIST abs = ILCombine(e->parent, e->items[e->position]);
    const HRESULT hr = abs ? shell_item_new(abs, &IID_IShellItem, (void **)&out[got]) : E_OUTOFMEMORY;
    SHFree(abs);
    if (FAILED(hr)) break;
    ++got; ++e->position;
  }
  if (fetched) *fetched = got;
  return got == wanted ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE esi_skip(IEnumShellItems *self, ULONG n) {
  EnumShellItems *e = (EnumShellItems *)self;
  if (n > e->count - e->position) { e->position = e->count; return S_FALSE; }
  e->position += n;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE esi_reset(IEnumShellItems *self) { ((EnumShellItems *)self)->position = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE esi_clone(IEnumShellItems *self, IEnumShellItems **out);
static const IEnumShellItemsVtbl g_enum_items_vtbl = {esi_query, esi_addref, esi_release, esi_next, esi_skip, esi_reset, esi_clone};
static HRESULT enum_shell_items_new(LPCITEMIDLIST parent, LPITEMIDLIST *items, size_t count, size_t position, BOOL take, IEnumShellItems **out) {
  EnumShellItems *e = mem_alloc(sizeof(*e));
  if (!e) return E_OUTOFMEMORY;
  e->items = mem_alloc((count ? count : 1) * sizeof(LPITEMIDLIST));
  if (!e->items) { mem_free(e); return E_OUTOFMEMORY; }
  for (size_t i = 0; i < count; ++i) e->items[i] = take ? items[i] : ILClone(items[i]);
  e->vtbl = &g_enum_items_vtbl;
  e->ref = 1;
  e->parent = ILClone(parent);
  e->count = count;
  e->position = position;
  objects_add();
  *out = (IEnumShellItems *)e;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE esi_clone(IEnumShellItems *self, IEnumShellItems **out) {
  EnumShellItems *e = (EnumShellItems *)self;
  return enum_shell_items_new(e->parent, e->items, e->count, e->position, FALSE, out);
}

static HRESULT STDMETHODCALLTYPE si_bind(IShellItem *self, IBindCtx *bind, REFGUID handler, REFIID riid, void **out) {
  ShellItemData *s = (ShellItemData *)self;
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualGUID(handler, &BHID_SFObject)) return SHBindToObject(NULL, s->pidl, bind, riid, out);
  if (IsEqualGUID(handler, &BHID_SFUIObject) || IsEqualGUID(handler, &BHID_DataObject)) {
    IShellFolder *parent = NULL;
    PCUITEMID_CHILD last = NULL;
    HRESULT hr = SHBindToParent(s->pidl, &IID_IShellFolder, (void **)&parent, &last);
    if (FAILED(hr)) return hr;
    hr = IShellFolder_GetUIObjectOf(parent, NULL, 1, &last, IsEqualGUID(handler, &BHID_DataObject) ? &IID_IDataObject : riid, NULL, out);
    IShellFolder_Release(parent);
    return hr;
  }
  if (IsEqualGUID(handler, &BHID_EnumItems)) {
    IShellFolder *folder = NULL;
    IEnumIDList *ids = NULL;
    HRESULT hr = SHBindToObject(NULL, s->pidl, bind, &IID_IShellFolder, (void **)&folder);
    if (FAILED(hr)) return hr;
    hr = IShellFolder_EnumObjects(folder, NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &ids);
    IShellFolder_Release(folder);
    if (FAILED(hr)) return hr;
    IdVector v = {0};
    LPITEMIDLIST item;
    ULONG got;
    while (IEnumIDList_Next(ids, 1, &item, &got) == S_OK) idv_push(&v, item);
    IEnumIDList_Release(ids);
    IEnumShellItems *e = NULL;
    hr = enum_shell_items_new(s->pidl, v.items, v.count, 0, TRUE, &e);
    mem_free(v.items);
    if (FAILED(hr)) return hr;
    hr = IEnumShellItems_QueryInterface(e, riid, out);
    IEnumShellItems_Release(e);
    return hr;
  }
  return MK_E_NOOBJECT;
}
static HRESULT STDMETHODCALLTYPE si_parent(IShellItem *self, IShellItem **out) {
  ShellItemData *s = (ShellItemData *)self;
  if (!out) return E_POINTER;
  *out = NULL;
  if (!s->pidl->mkid.cb) return MK_E_NOOBJECT;
  LPITEMIDLIST parent = ILClone(s->pidl);
  if (!parent) return E_OUTOFMEMORY;
  ILRemoveLastID(parent);
  const HRESULT hr = shell_item_new(parent, &IID_IShellItem, (void **)out);
  SHFree(parent);
  return hr;
}
static HRESULT STDMETHODCALLTYPE si_display_name(IShellItem *self, SIGDN kind, LPWSTR *name) {
  return SHGetNameFromIDList(((ShellItemData *)self)->pidl, kind, name);
}
static HRESULT STDMETHODCALLTYPE si_attributes(IShellItem *self, SFGAOF mask, SFGAOF *attributes) {
  ShellItemData *s = (ShellItemData *)self;
  if (!attributes) return E_POINTER;
  SFGAOF bits = mask;
  if (!s->pidl->mkid.cb) { *attributes = mask & (SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_CANLINK | SFGAO_DROPTARGET); return (*attributes == mask) ? S_OK : S_FALSE; }
  IShellFolder *parent = NULL;
  PCUITEMID_CHILD last = NULL;
  HRESULT hr = SHBindToParent(s->pidl, &IID_IShellFolder, (void **)&parent, &last);
  if (FAILED(hr)) return hr;
  hr = IShellFolder_GetAttributesOf(parent, 1, &last, &bits);
  IShellFolder_Release(parent);
  if (FAILED(hr)) return hr;
  *attributes = bits & mask;
  return *attributes == mask ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE si_compare(IShellItem *self, IShellItem *other, SICHINTF hint, int *order) {
  ShellItemData *a = (ShellItemData *)self;
  LPITEMIDLIST b = NULL;
  if (!other || !order) return E_INVALIDARG;
  *order = 0;
  if (FAILED(SHGetIDListFromObject((IUnknown *)other, &b))) return E_FAIL;
  const BOOL same = ILIsEqual(a->pidl, b);
  SHFree(b);
  if (same) return S_OK;
  if ((hint & 0xFFFF) == SICHINT_CANONICAL || (hint & 0xFFFF) == SICHINT_ALLFIELDS) { *order = 1; return S_FALSE; }
  /* by display name */
  WCHAR *na = NULL, *nb = NULL;
  IShellItem_GetDisplayName(self, SIGDN_DESKTOPABSOLUTEPARSING, &na);
  IShellItem_GetDisplayName(other, SIGDN_DESKTOPABSOLUTEPARSING, &nb);
  *order = na && nb ? wicmp(na, nb) : 0;
  SHFree(na); SHFree(nb);
  return *order ? S_FALSE : S_OK;
}
static const IShellItemVtbl g_item_vtbl = {si_query, si_addref, si_release, si_bind, si_parent, si_display_name, si_attributes, si_compare};

static HRESULT STDMETHODCALLTYPE sip_query(IPersistIDList *self, REFIID riid, void **out) { return si_query((IShellItem *)ITEM_FROM_PERSIST(self), riid, out); }
static ULONG STDMETHODCALLTYPE sip_addref(IPersistIDList *self) { return si_addref((IShellItem *)ITEM_FROM_PERSIST(self)); }
static ULONG STDMETHODCALLTYPE sip_release(IPersistIDList *self) { return si_release((IShellItem *)ITEM_FROM_PERSIST(self)); }
static HRESULT STDMETHODCALLTYPE sip_class_id(IPersistIDList *self, CLSID *id) { (void)self; if (id) *id = CLSID_ShellItem; return S_OK; }
static HRESULT STDMETHODCALLTYPE sip_set(IPersistIDList *self, PCIDLIST_ABSOLUTE pidl) {
  ShellItemData *s = ITEM_FROM_PERSIST(self);
  LPITEMIDLIST copy = pidl ? ILClone(pidl) : NULL;
  if (!copy) return E_INVALIDARG;
  SHFree(s->pidl);
  s->pidl = copy;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE sip_get(IPersistIDList *self, PIDLIST_ABSOLUTE *pidl) {
  if (!pidl) return E_POINTER;
  *pidl = ILClone(ITEM_FROM_PERSIST(self)->pidl);
  return *pidl ? S_OK : E_OUTOFMEMORY;
}
static const IPersistIDListVtbl g_item_persist_vtbl = {sip_query, sip_addref, sip_release, sip_class_id, sip_set, sip_get};

static HRESULT shell_item_new(LPCITEMIDLIST pidl, REFIID riid, void **out) {
  ShellItemData *s = mem_alloc(sizeof(*s));
  if (!s) return E_OUTOFMEMORY;
  s->item = &g_item_vtbl;
  s->persist = &g_item_persist_vtbl;
  s->ref = 1;
  s->pidl = ILClone(pidl);
  if (!s->pidl) { mem_free(s); return E_OUTOFMEMORY; }
  objects_add();
  const HRESULT hr = si_query((IShellItem *)s, riid, out);
  si_release((IShellItem *)s);
  return hr;
}

EXPORT HRESULT WINAPI SHCreateItemFromIDList(PCIDLIST_ABSOLUTE pidl, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!pidl) return E_INVALIDARG;
  return shell_item_new(pidl, riid, out);
}
EXPORT HRESULT WINAPI SHCreateItemFromParsingName(PCWSTR name, IBindCtx *bind, REFIID riid, void **out) {
  PIDLIST_ABSOLUTE pidl = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = SHParseDisplayName(name, bind, &pidl, 0, NULL);
  if (FAILED(hr)) return hr;
  hr = shell_item_new(pidl, riid, out);
  SHFree(pidl);
  return hr;
}
EXPORT HRESULT WINAPI SHCreateItemFromRelativeName(IShellItem *parent, PCWSTR name, IBindCtx *bind, REFIID riid, void **out) {
  WCHAR *base = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!parent || !name) return E_INVALIDARG;
  HRESULT hr = IShellItem_GetDisplayName(parent, SIGDN_FILESYSPATH, &base);
  if (FAILED(hr)) return hr;
  WCHAR full[MAX_PATH_EX];
  if (!path_join(full, ARRAY_LEN(full), base, name)) { SHFree(base); return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE); }
  SHFree(base);
  return SHCreateItemFromParsingName(full, bind, riid, out);
}
EXPORT HRESULT WINAPI SHCreateItemWithParent(PCIDLIST_ABSOLUTE parent, IShellFolder *folder, PCUITEMID_CHILD child, REFIID riid, void **out) {
  (void)folder;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!child) return E_INVALIDARG;
  LPITEMIDLIST abs = parent ? ILCombine(parent, child) : NULL;
  if (!abs && !parent) return E_INVALIDARG;
  const HRESULT hr = shell_item_new(abs, riid, out);
  SHFree(abs);
  return hr;
}
EXPORT HRESULT WINAPI SHCreateShellItem(PCIDLIST_ABSOLUTE parent, IShellFolder *folder, PCUITEMID_CHILD child, IShellItem **out) {
  return SHCreateItemWithParent(parent, folder, child, &IID_IShellItem, (void **)out);
}
EXPORT HRESULT WINAPI SHCreateItemInKnownFolder(REFKNOWNFOLDERID id, DWORD flags, PCWSTR name, REFIID riid, void **out) {
  PWSTR folder = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = SHGetKnownFolderPath(id, flags, NULL, &folder);
  if (FAILED(hr)) return hr;
  WCHAR full[MAX_PATH_EX];
  if (name && *name) { if (!path_join(full, ARRAY_LEN(full), folder, name)) { SHFree(folder); return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE); } }
  else w_copy(full, ARRAY_LEN(full), folder);
  SHFree(folder);
  return SHCreateItemFromParsingName(full, NULL, riid, out);
}
EXPORT HRESULT WINAPI SHGetKnownFolderItem(REFKNOWNFOLDERID id, KNOWN_FOLDER_FLAG flags, HANDLE token, REFIID riid, void **out) {
  PIDLIST_ABSOLUTE pidl = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = SHGetKnownFolderIDList(id, flags, token, &pidl);
  if (FAILED(hr)) return hr;
  hr = shell_item_new(pidl, riid, out);
  SHFree(pidl);
  return hr;
}
EXPORT HRESULT WINAPI SHGetIDListFromObject(IUnknown *object, PIDLIST_ABSOLUTE *pidl) {
  void *p = NULL;
  if (!pidl) return E_INVALIDARG;
  *pidl = NULL;
  if (!object) return E_INVALIDARG;
  if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IPersistIDList, &p))) {
    const HRESULT hr = IPersistIDList_GetIDList((IPersistIDList *)p, pidl);
    IPersistIDList_Release((IPersistIDList *)p);
    return hr;
  }
  if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IPersistFolder2, &p))) {
    const HRESULT hr = IPersistFolder2_GetCurFolder((IPersistFolder2 *)p, pidl);
    IPersistFolder2_Release((IPersistFolder2 *)p);
    return hr;
  }
  if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IDataObject, &p))) {
    FORMATETC fmt = {format_shell_idlist(), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m;
    HRESULT hr = IDataObject_GetData((IDataObject *)p, &fmt, &m);
    IDataObject_Release((IDataObject *)p);
    if (FAILED(hr)) return hr;
    CIDA *cida = GlobalLock(m.hGlobal);
    if (cida && cida->cidl >= 1) {
      LPCITEMIDLIST parent = (LPCITEMIDLIST)((BYTE *)cida + cida->aoffset[0]), child = (LPCITEMIDLIST)((BYTE *)cida + cida->aoffset[1]);
      *pidl = ILCombine(parent, child);
      hr = *pidl ? S_OK : E_OUTOFMEMORY;
    } else hr = E_FAIL;
    if (cida) GlobalUnlock(m.hGlobal);
    GlobalFree(m.hGlobal);
    return hr;
  }
  return E_NOINTERFACE;
}
EXPORT HRESULT WINAPI SHGetItemFromObject(IUnknown *object, REFIID riid, void **out) {
  PIDLIST_ABSOLUTE pidl = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = SHGetIDListFromObject(object, &pidl);
  if (FAILED(hr)) return hr;
  hr = shell_item_new(pidl, riid, out);
  SHFree(pidl);
  return hr;
}
EXPORT HRESULT WINAPI SHGetItemFromDataObject(IDataObject *data, DATAOBJ_GET_ITEM_FLAGS flags, REFIID riid, void **out) { (void)flags; return SHGetItemFromObject((IUnknown *)data, riid, out); }

/* ---- IShellItemArray ----------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { const IShellItemArrayVtbl *vtbl; LONG ref; LPITEMIDLIST *pidls; DWORD count; } ShellItemArray;

static HRESULT STDMETHODCALLTYPE sia_query(IShellItemArray *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IShellItemArray)) { *out = self; InterlockedIncrement(&((ShellItemArray *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sia_addref(IShellItemArray *self) { return (ULONG)InterlockedIncrement(&((ShellItemArray *)self)->ref); }
static ULONG STDMETHODCALLTYPE sia_release(IShellItemArray *self) {
  ShellItemArray *a = (ShellItemArray *)self;
  const LONG n = InterlockedDecrement(&a->ref);
  if (!n) { for (DWORD i = 0; i < a->count; ++i) SHFree(a->pidls[i]); mem_free(a->pidls); mem_free(a); objects_remove(); }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE sia_bind(IShellItemArray *self, IBindCtx *bind, REFGUID handler, REFIID riid, void **out) {
  ShellItemArray *a = (ShellItemArray *)self;
  (void)bind;
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualGUID(handler, &BHID_DataObject)) return data_object_new(a->pidls, a->count, (IDataObject **)out);
  return MK_E_NOOBJECT;
}
static HRESULT STDMETHODCALLTYPE sia_property_store(IShellItemArray *self, GETPROPERTYSTOREFLAGS flags, REFIID riid, void **out) { (void)self; (void)flags; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE sia_property_list(IShellItemArray *self, REFPROPERTYKEY key, REFIID riid, void **out) { (void)self; (void)key; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE sia_attributes(IShellItemArray *self, SIATTRIBFLAGS flags, SFGAOF mask, SFGAOF *attributes) {
  ShellItemArray *a = (ShellItemArray *)self;
  if (!attributes) return E_POINTER;
  SFGAOF result = flags == SIATTRIBFLAGS_OR ? 0 : mask;
  for (DWORD i = 0; i < a->count; ++i) {
    IShellItem *item = NULL;
    SFGAOF bits = 0;
    if (FAILED(shell_item_new(a->pidls[i], &IID_IShellItem, (void **)&item))) continue;
    IShellItem_GetAttributes(item, mask, &bits);
    IShellItem_Release(item);
    if (flags == SIATTRIBFLAGS_OR) result |= bits; else result &= bits;
  }
  *attributes = result & mask;
  return *attributes == mask ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE sia_count(IShellItemArray *self, DWORD *count) { if (!count) return E_POINTER; *count = ((ShellItemArray *)self)->count; return S_OK; }
static HRESULT STDMETHODCALLTYPE sia_item_at(IShellItemArray *self, DWORD index, IShellItem **out) {
  ShellItemArray *a = (ShellItemArray *)self;
  if (!out) return E_POINTER;
  *out = NULL;
  if (index >= a->count) return E_INVALIDARG;
  return shell_item_new(a->pidls[index], &IID_IShellItem, (void **)out);
}
static HRESULT STDMETHODCALLTYPE sia_enum(IShellItemArray *self, IEnumShellItems **out) {
  ShellItemArray *a = (ShellItemArray *)self;
  LPITEMIDLIST root = il_new(sizeof(USHORT));
  if (!root) return E_OUTOFMEMORY;
  const HRESULT hr = enum_shell_items_new(root, a->pidls, a->count, 0, FALSE, out);
  SHFree(root);
  return hr;
}
static const IShellItemArrayVtbl g_item_array_vtbl = {sia_query, sia_addref, sia_release, sia_bind, sia_property_store, sia_property_list, sia_attributes, sia_count, sia_item_at, sia_enum};

static HRESULT shell_item_array_new(LPITEMIDLIST *pidls, DWORD count, IShellItemArray **out) {
  ShellItemArray *a = mem_alloc(sizeof(*a));
  if (!a) return E_OUTOFMEMORY;
  a->pidls = mem_alloc((count ? count : 1) * sizeof(LPITEMIDLIST));
  if (!a->pidls) { mem_free(a); return E_OUTOFMEMORY; }
  for (DWORD i = 0; i < count; ++i) a->pidls[i] = ILClone(pidls[i]);
  a->vtbl = &g_item_array_vtbl;
  a->ref = 1;
  a->count = count;
  objects_add();
  *out = (IShellItemArray *)a;
  return S_OK;
}
EXPORT HRESULT WINAPI SHCreateShellItemArray(PCIDLIST_ABSOLUTE parent, IShellFolder *folder, UINT count, PCUITEMID_CHILD_ARRAY children, IShellItemArray **out) {
  (void)folder;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!count || !children) return E_INVALIDARG;
  LPITEMIDLIST *abs = mem_alloc(count * sizeof(LPITEMIDLIST));
  if (!abs) return E_OUTOFMEMORY;
  for (UINT i = 0; i < count; ++i) abs[i] = parent ? ILCombine(parent, children[i]) : ILClone(children[i]);
  const HRESULT hr = shell_item_array_new(abs, count, out);
  for (UINT i = 0; i < count; ++i) SHFree(abs[i]);
  mem_free(abs);
  return hr;
}
EXPORT HRESULT WINAPI SHCreateShellItemArrayFromIDLists(UINT count, PCIDLIST_ABSOLUTE_ARRAY pidls, IShellItemArray **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!count || !pidls) return E_INVALIDARG;
  return shell_item_array_new((LPITEMIDLIST *)pidls, count, out);
}
EXPORT HRESULT WINAPI SHCreateShellItemArrayFromShellItem(IShellItem *item, REFIID riid, void **out) {
  PIDLIST_ABSOLUTE pidl = NULL;
  IShellItemArray *array = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = SHGetIDListFromObject((IUnknown *)item, &pidl);
  if (FAILED(hr)) return hr;
  hr = shell_item_array_new(&pidl, 1, &array);
  SHFree(pidl);
  if (FAILED(hr)) return hr;
  hr = IShellItemArray_QueryInterface(array, riid, out);
  IShellItemArray_Release(array);
  return hr;
}
EXPORT HRESULT WINAPI SHCreateShellItemArrayFromDataObject(IDataObject *data, REFIID riid, void **out) {
  FORMATETC fmt = {format_shell_idlist(), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM m;
  if (!out || !data) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = IDataObject_GetData(data, &fmt, &m);
  if (FAILED(hr)) return hr;
  CIDA *cida = GlobalLock(m.hGlobal);
  if (!cida) { GlobalFree(m.hGlobal); return E_FAIL; }
  LPITEMIDLIST *abs = mem_alloc((cida->cidl ? cida->cidl : 1) * sizeof(LPITEMIDLIST));
  IShellItemArray *array = NULL;
  hr = abs ? S_OK : E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) {
    LPCITEMIDLIST parent = (LPCITEMIDLIST)((BYTE *)cida + cida->aoffset[0]);
    for (UINT i = 0; i < cida->cidl; ++i) abs[i] = ILCombine(parent, (LPCITEMIDLIST)((BYTE *)cida + cida->aoffset[i + 1]));
    hr = shell_item_array_new(abs, cida->cidl, &array);
    for (UINT i = 0; i < cida->cidl; ++i) SHFree(abs[i]);
    mem_free(abs);
  }
  GlobalUnlock(m.hGlobal);
  GlobalFree(m.hGlobal);
  if (FAILED(hr)) return hr;
  hr = IShellItemArray_QueryInterface(array, riid, out);
  IShellItemArray_Release(array);
  return hr;
}
EXPORT HRESULT WINAPI SHGetAttributesFromDataObject(IDataObject *data, DWORD mask, DWORD *attributes, UINT *count) {
  IShellItemArray *array = NULL;
  if (!data) return E_INVALIDARG;
  HRESULT hr = SHCreateShellItemArrayFromDataObject(data, &IID_IShellItemArray, (void **)&array);
  if (FAILED(hr)) return hr;
  if (count) { DWORD n = 0; IShellItemArray_GetCount(array, &n); *count = n; }
  SFGAOF bits = 0;
  hr = IShellItemArray_GetAttributes(array, SIATTRIBFLAGS_AND, mask, &bits);
  if (attributes) *attributes = bits;
  IShellItemArray_Release(array);
  return SUCCEEDED(hr) ? S_OK : hr;
}
