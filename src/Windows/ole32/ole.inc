/* ole32: the OLE helpers: OleInitialize, the clipboard, storage media, drag and drop, advise holders, the in-place helpers and the data cache.
 *
 * There is no window server in user space here (user32.dll and gdi32.dll are stubs), so everything that Windows keeps in a window or in the system clipboard is kept in
 * this process: the OLE clipboard is one object of the process, drag and drop registrations are a table keyed by the window handle, and the GDI/USER functions that are
 * needed for the formats that carry GDI objects are looked up with GetProcAddress when they are used (the DLL does not import them). */

/* ---- GDI and USER, looked up when needed ----------------------------------------------------------------------------------------------------------------------- */

static HMODULE g_gdi_module, g_user_module;
static void *dyn_gdi(const char *name) {
  HMODULE m = g_gdi_module;
  if (!m) { m = LoadLibraryW(W('g','d','i','3','2','.','d','l','l')); g_gdi_module = m; }
  return m ? (void *)GetProcAddress(m, name) : NULL;
}
static void *dyn_user(const char *name) {
  HMODULE m = g_user_module;
  if (!m) { m = LoadLibraryW(W('u','s','e','r','3','2','.','d','l','l')); g_user_module = m; }
  return m ? (void *)GetProcAddress(m, name) : NULL;
}

typedef BOOL (WINAPI *HandleBoolFn)(HANDLE);
typedef HANDLE (WINAPI *HandleCopyFn)(HANDLE, LPCWSTR);
typedef HANDLE (WINAPI *CopyImageFn)(HANDLE, UINT, int, int, UINT);
typedef UINT (WINAPI *MetaBitsGetFn)(HANDLE, UINT, void *);
typedef HANDLE (WINAPI *MetaBitsSetFn)(UINT, const BYTE *);
typedef SHORT (WINAPI *KeyStateFn)(int);

static BOOL gdi_delete(const char *name, HANDLE h) {
  HandleBoolFn f = h ? (HandleBoolFn)dyn_gdi(name) : NULL;
  return f ? f(h) : FALSE;
}
static HANDLE gdi_copy(const char *name, HANDLE h) {
  HandleCopyFn f = h ? (HandleCopyFn)dyn_gdi(name) : NULL;
  return f ? f(h, NULL) : NULL;
}
static BOOL key_down(const char *fn, int vk) {
  KeyStateFn f = (KeyStateFn)dyn_user(fn);
  return f && (f(vk) & 0x8000) != 0;
}

/* METAFILEPICT (wingdi.h is not included here) */
typedef struct OleMfPict { LONG mm, xExt, yExt; HANDLE hMF; } OleMfPict;

#define OLE_MAX_BLOB 0x10000000u    /* the largest data block that is copied (a bound on what a hostile size can ask for) */

/* ---- OleInitialize ----------------------------------------------------------------------------------------------------------------------------------------------- */

static LONG g_ole_window_class;      /* OLE has been initialized in the process and the last OleUninitialize has not come yet (Windows: the registered window class) */

static void clip_thread_gone(ThreadData *td);
static void dragdrop_thread_gone(DWORD tid);

EXPORT HRESULT WINAPI OleInitialize(LPVOID reserved) {
  HRESULT hr;
  ThreadData *td;
  (void)reserved;
  hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) return hr;
  td = thread_data(TRUE);
  if (!td) { CoUninitialize(); return E_OUTOFMEMORY; }
  if (td->ole_init_count++ > 0) return S_FALSE;
  InterlockedIncrement(&g_ole_window_class);
  return S_OK;
}
EXPORT HRESULT WINAPI OleInitializeWOW(DWORD a, DWORD b) { (void)a; (void)b; return OleInitialize(NULL); }

EXPORT void WINAPI OleUninitialize(void) {
  ThreadData *td = thread_data(FALSE);
  if (!td || td->ole_init_count == 0) return;
  if (--td->ole_init_count == 0) {
    clip_thread_gone(td);
    dragdrop_thread_gone(GetCurrentThreadId());
    InterlockedDecrement(&g_ole_window_class);
  }
  CoUninitialize();
}

/* the apartment of the thread ends (CoUninitialize / thread exit): what OLE still holds for it goes */
static void ole_release_thread(ThreadData *td) {
  if (td->ole_init_count) {
    td->ole_init_count = 0;
    clip_thread_gone(td);
  }
  dragdrop_thread_gone(GetCurrentThreadId());
}

/* ---- storage media ---------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT void WINAPI ReleaseStgMedium(STGMEDIUM *m) {
  if (!m) return;
  switch (m->tymed) {
    case TYMED_HGLOBAL:
      if (!m->pUnkForRelease && m->hGlobal) GlobalFree(m->hGlobal);
      break;
    case TYMED_FILE:
      if (m->lpszFileName) {
        if (!m->pUnkForRelease) DeleteFileW(m->lpszFileName);
        CoTaskMemFree(m->lpszFileName);
      }
      break;
    case TYMED_ISTREAM:
      if (m->pstm) IStream_Release(m->pstm);
      break;
    case TYMED_ISTORAGE:
      if (m->pstg) IStorage_Release(m->pstg);
      break;
    case TYMED_GDI:
      if (!m->pUnkForRelease) gdi_delete("DeleteObject", m->hBitmap);
      break;
    case TYMED_MFPICT:
      if (!m->pUnkForRelease && m->hMetaFilePict) {
        OleMfPict *p = GlobalLock(m->hMetaFilePict);
        if (p) { gdi_delete("DeleteMetaFile", p->hMF); GlobalUnlock(m->hMetaFilePict); }
        GlobalFree(m->hMetaFilePict);
      }
      break;
    case TYMED_ENHMF:
      if (!m->pUnkForRelease) gdi_delete("DeleteEnhMetaFile", m->hEnhMetaFile);
      break;
    default:
      break;
  }
  if (m->pUnkForRelease) {
    IUnknown *u = m->pUnkForRelease;
    m->pUnkForRelease = NULL;
    IUnknown_Release(u);
  }
}

EXPORT HANDLE WINAPI OleDuplicateData(HANDLE src, CLIPFORMAT cf, UINT flags) {
  if (!src) return NULL;
  switch (cf) {
    case CF_ENHMETAFILE:
      return gdi_copy("CopyEnhMetaFileW", src);
    case CF_METAFILEPICT: {
      HGLOBAL d = GlobalAlloc(flags ? flags : GMEM_MOVEABLE, sizeof(OleMfPict));
      OleMfPict *dp, *sp;
      HANDLE mf;
      if (!d) return NULL;
      dp = GlobalLock(d);
      sp = GlobalLock(src);
      if (!dp || !sp) { if (sp) GlobalUnlock(src); if (dp) GlobalUnlock(d); GlobalFree(d); return NULL; }
      *dp = *sp;
      mf = gdi_copy("CopyMetaFileW", sp->hMF);
      GlobalUnlock(src);
      if (!mf) { GlobalUnlock(d); GlobalFree(d); return NULL; }
      dp->hMF = mf;
      GlobalUnlock(d);
      return d;
    }
    case CF_BITMAP: {
      CopyImageFn f = (CopyImageFn)dyn_user("CopyImage");
      return f ? f(src, 0 /* IMAGE_BITMAP */, 0, 0, 0) : NULL;
    }
    case CF_PALETTE:
      return NULL;
    default: {
      SIZE_T n = GlobalSize(src);
      HGLOBAL d;
      void *dp, *sp;
      if (!n || n > OLE_MAX_BLOB) return NULL;
      d = GlobalAlloc(flags ? flags : GMEM_MOVEABLE, n);
      if (!d) return NULL;
      dp = GlobalLock(d);
      sp = GlobalLock(src);
      if (!dp || !sp) { if (sp) GlobalUnlock(src); if (dp) GlobalUnlock(d); GlobalFree(d); return NULL; }
      mem_copy(dp, sp, n);
      GlobalUnlock(src);
      GlobalUnlock(d);
      return d;
    }
  }
}

/* A copy of a STGMEDIUM the caller keeps (HGLOBAL by value, GDI handles by OleDuplicateData); the copy has no pUnkForRelease. Streams and storages are shared (AddRef). */
static HRESULT medium_copy(const STGMEDIUM *src, CLIPFORMAT cf, STGMEDIUM *dst) {
  HANDLE h = NULL;
  mem_zero(dst, sizeof(*dst));
  dst->tymed = src->tymed;
  switch (src->tymed) {
    case TYMED_HGLOBAL: h = OleDuplicateData(src->hGlobal, CF_TEXT, 0); dst->hGlobal = h; break;
    case TYMED_GDI: h = OleDuplicateData(src->hBitmap, CF_BITMAP, 0); dst->hBitmap = h; break;
    case TYMED_MFPICT: h = OleDuplicateData(src->hMetaFilePict, CF_METAFILEPICT, 0); dst->hMetaFilePict = h; break;
    case TYMED_ENHMF: h = OleDuplicateData(src->hEnhMetaFile, CF_ENHMETAFILE, 0); dst->hEnhMetaFile = h; break;
    case TYMED_ISTREAM: dst->pstm = src->pstm; if (dst->pstm) IStream_AddRef(dst->pstm); return S_OK;
    case TYMED_ISTORAGE: dst->pstg = src->pstg; if (dst->pstg) IStorage_AddRef(dst->pstg); return S_OK;
    default: dst->tymed = TYMED_NULL; return DV_E_TYMED;
  }
  (void)cf;
  if (!h) { dst->tymed = TYMED_NULL; return E_OUTOFMEMORY; }
  return S_OK;
}

/* ---- format and advise enumerators ---------------------------------------------------------------------------------------------------------------------------- */

/* a DVTARGETDEVICE copy (the size is checked: it comes from another component) */
static DVTARGETDEVICE *ptd_copy(const DVTARGETDEVICE *p, BOOL task) {
  DVTARGETDEVICE *r;
  DWORD n;
  if (!p) return NULL;
  n = p->tdSize;
  if (n < 12 || n > 0x100000) return NULL;
  r = task ? CoTaskMemAlloc(n) : mem_alloc(n);
  if (r) mem_copy(r, p, n);
  return r;
}

typedef struct EnumFormat {
  IEnumFORMATETC iface;
  LONG refs;
  FORMATETC *items;
  ULONG count, pos;
} EnumFormat;

static const IEnumFORMATETCVtbl g_enum_format_vtbl;
static EnumFormat *enum_format_of(IEnumFORMATETC *e) { return (EnumFormat *)e; }

static HRESULT WINAPI efmt_qi(IEnumFORMATETC *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumFORMATETC)) { *out = me; IEnumFORMATETC_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI efmt_addref(IEnumFORMATETC *me) { return (ULONG)InterlockedIncrement(&enum_format_of(me)->refs); }
static ULONG WINAPI efmt_release(IEnumFORMATETC *me) {
  EnumFormat *e = enum_format_of(me);
  LONG n = InterlockedDecrement(&e->refs);
  if (n == 0) {
    for (ULONG i = 0; i < e->count; i++) mem_free(e->items[i].ptd);
    mem_free(e->items);
    mem_free(e);
  }
  return (ULONG)n;
}
static HRESULT WINAPI efmt_next(IEnumFORMATETC *me, ULONG celt, FORMATETC *out, ULONG *fetched) {
  EnumFormat *e = enum_format_of(me);
  ULONG got = 0;
  if (fetched) *fetched = 0;
  if (celt && !out) return E_INVALIDARG;
  while (got < celt && e->pos < e->count) {
    out[got] = e->items[e->pos];
    out[got].ptd = ptd_copy(e->items[e->pos].ptd, TRUE);
    got++;
    e->pos++;
  }
  if (fetched) *fetched = got;
  return got == celt ? S_OK : S_FALSE;
}
static HRESULT WINAPI efmt_skip(IEnumFORMATETC *me, ULONG celt) {
  EnumFormat *e = enum_format_of(me);
  if (celt > e->count - e->pos) { e->pos = e->count; return S_FALSE; }
  e->pos += celt;
  return S_OK;
}
static HRESULT WINAPI efmt_reset(IEnumFORMATETC *me) { enum_format_of(me)->pos = 0; return S_OK; }
static HRESULT enum_formatetc_create(const FORMATETC *formats, ULONG count, IEnumFORMATETC **out);
static HRESULT WINAPI efmt_clone(IEnumFORMATETC *me, IEnumFORMATETC **out) {
  EnumFormat *e = enum_format_of(me);
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  hr = enum_formatetc_create(e->items, e->count, out);
  if (SUCCEEDED(hr)) enum_format_of(*out)->pos = e->pos;
  return hr;
}
static const IEnumFORMATETCVtbl g_enum_format_vtbl = {efmt_qi, efmt_addref, efmt_release, efmt_next, efmt_skip, efmt_reset, efmt_clone};

/* An IEnumFORMATETC over a copy of the array (the target devices are copied too). Used by the clipboard, the data cache and OleRegEnumFormatEtc. */
static HRESULT enum_formatetc_create(const FORMATETC *formats, ULONG count, IEnumFORMATETC **out) {
  EnumFormat *e;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (count > 0x100000) return E_OUTOFMEMORY;
  e = mem_alloc(sizeof(*e));
  if (!e) return E_OUTOFMEMORY;
  e->iface.lpVtbl = (IEnumFORMATETCVtbl *)&g_enum_format_vtbl;
  e->refs = 1;
  if (count) {
    e->items = mem_alloc((SIZE_T)count * sizeof(FORMATETC));
    if (!e->items) { mem_free(e); return E_OUTOFMEMORY; }
    for (ULONG i = 0; i < count; i++) {
      e->items[i] = formats[i];
      e->items[i].ptd = ptd_copy(formats[i].ptd, FALSE);
    }
  }
  e->count = count;
  *out = &e->iface;
  return S_OK;
}

typedef struct EnumStat {
  IEnumSTATDATA iface;
  LONG refs;
  STATDATA *items;
  ULONG count, pos;
} EnumStat;

static const IEnumSTATDATAVtbl g_enum_stat_vtbl;
static EnumStat *enum_stat_of(IEnumSTATDATA *e) { return (EnumStat *)e; }
static HRESULT WINAPI estat_qi(IEnumSTATDATA *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumSTATDATA)) { *out = me; IEnumSTATDATA_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI estat_addref(IEnumSTATDATA *me) { return (ULONG)InterlockedIncrement(&enum_stat_of(me)->refs); }
static ULONG WINAPI estat_release(IEnumSTATDATA *me) {
  EnumStat *e = enum_stat_of(me);
  LONG n = InterlockedDecrement(&e->refs);
  if (n == 0) {
    for (ULONG i = 0; i < e->count; i++) {
      mem_free(e->items[i].formatetc.ptd);
      if (e->items[i].pAdvSink) IAdviseSink_Release(e->items[i].pAdvSink);
    }
    mem_free(e->items);
    mem_free(e);
  }
  return (ULONG)n;
}
static HRESULT WINAPI estat_next(IEnumSTATDATA *me, ULONG celt, STATDATA *out, ULONG *fetched) {
  EnumStat *e = enum_stat_of(me);
  ULONG got = 0;
  if (fetched) *fetched = 0;
  if (celt && !out) return E_INVALIDARG;
  while (got < celt && e->pos < e->count) {
    out[got] = e->items[e->pos];
    out[got].formatetc.ptd = ptd_copy(e->items[e->pos].formatetc.ptd, TRUE);
    if (out[got].pAdvSink) IAdviseSink_AddRef(out[got].pAdvSink);
    got++;
    e->pos++;
  }
  if (fetched) *fetched = got;
  return got == celt ? S_OK : S_FALSE;
}
static HRESULT WINAPI estat_skip(IEnumSTATDATA *me, ULONG celt) {
  EnumStat *e = enum_stat_of(me);
  if (celt > e->count - e->pos) { e->pos = e->count; return S_FALSE; }
  e->pos += celt;
  return S_OK;
}
static HRESULT WINAPI estat_reset(IEnumSTATDATA *me) { enum_stat_of(me)->pos = 0; return S_OK; }
static HRESULT enum_statdata_create(const STATDATA *items, ULONG count, IEnumSTATDATA **out);
static HRESULT WINAPI estat_clone(IEnumSTATDATA *me, IEnumSTATDATA **out) {
  EnumStat *e = enum_stat_of(me);
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  hr = enum_statdata_create(e->items, e->count, out);
  if (SUCCEEDED(hr)) enum_stat_of(*out)->pos = e->pos;
  return hr;
}
static const IEnumSTATDATAVtbl g_enum_stat_vtbl = {estat_qi, estat_addref, estat_release, estat_next, estat_skip, estat_reset, estat_clone};

/* An IEnumSTATDATA over a copy of the array (the sinks get a reference, the target devices are copied). */
static HRESULT enum_statdata_create(const STATDATA *items, ULONG count, IEnumSTATDATA **out) {
  EnumStat *e;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (count > 0x100000) return E_OUTOFMEMORY;
  e = mem_alloc(sizeof(*e));
  if (!e) return E_OUTOFMEMORY;
  e->iface.lpVtbl = (IEnumSTATDATAVtbl *)&g_enum_stat_vtbl;
  e->refs = 1;
  if (count) {
    e->items = mem_alloc((SIZE_T)count * sizeof(STATDATA));
    if (!e->items) { mem_free(e); return E_OUTOFMEMORY; }
    for (ULONG i = 0; i < count; i++) {
      e->items[i] = items[i];
      e->items[i].formatetc.ptd = ptd_copy(items[i].formatetc.ptd, FALSE);
      if (e->items[i].pAdvSink) IAdviseSink_AddRef(e->items[i].pAdvSink);
    }
  }
  e->count = count;
  *out = &e->iface;
  return S_OK;
}

/* ---- data blocks: what the clipboard and the cache keep of a medium that holds bytes ----------------------------------------------------------------------------- */

typedef struct Blob { BYTE *data; SIZE_T size; } Blob;
static void blob_free(Blob *b) { mem_free(b->data); b->data = NULL; b->size = 0; }
static BOOL blob_set(Blob *b, const void *p, SIZE_T n) {
  BYTE *d;
  if (n > OLE_MAX_BLOB) return FALSE;
  d = mem_alloc(n);
  if (!d) return FALSE;
  if (n) mem_copy(d, p, n);
  mem_free(b->data);
  b->data = d;
  b->size = n;
  return TRUE;
}
#define BLOB_TYMEDS (TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE)

/* the bytes of a stream from its start */
static HRESULT stream_to_blob(IStream *s, Blob *b) {
  STATSTG st;
  HRESULT hr;
  BYTE *d;
  ULONG got = 0;
  hr = IStream_Stat(s, &st, STATFLAG_NONAME);
  if (FAILED(hr)) return hr;
  if (st.cbSize.QuadPart > OLE_MAX_BLOB) return E_OUTOFMEMORY;
  d = mem_alloc((SIZE_T)st.cbSize.QuadPart);
  if (!d) return E_OUTOFMEMORY;
  hr = stream_seek_to(s, 0);
  if (SUCCEEDED(hr) && st.cbSize.QuadPart) hr = IStream_Read(s, d, (ULONG)st.cbSize.QuadPart, &got);
  if (FAILED(hr)) { mem_free(d); return hr; }
  mem_free(b->data);
  b->data = d;
  b->size = got;
  return S_OK;
}

/* a storage as the bytes of its compound file */
static HRESULT storage_to_blob(IStorage *src, Blob *b) {
  ILockBytes *lb = NULL;
  IStorage *dst = NULL;
  HRESULT hr = CreateILockBytesOnHGlobal(NULL, TRUE, &lb);
  STATSTG st;
  HGLOBAL h = NULL;
  if (FAILED(hr)) return hr;
  hr = StgCreateDocfileOnILockBytes(lb, STGM_CREATE | STGM_SHARE_EXCLUSIVE | STGM_READWRITE, 0, &dst);
  if (SUCCEEDED(hr)) hr = IStorage_CopyTo(src, 0, NULL, NULL, dst);
  if (SUCCEEDED(hr)) hr = IStorage_Commit(dst, STGC_DEFAULT);
  if (dst) IStorage_Release(dst);
  if (SUCCEEDED(hr)) hr = ILockBytes_Stat(lb, &st, STATFLAG_NONAME);
  if (SUCCEEDED(hr)) hr = GetHGlobalFromILockBytes(lb, &h);
  if (SUCCEEDED(hr)) {
    const void *p = h ? GlobalLock(h) : NULL;
    if (!p || st.cbSize.QuadPart > GlobalSize(h) || !blob_set(b, p, (SIZE_T)st.cbSize.QuadPart)) hr = E_FAIL;
    if (p) GlobalUnlock(h);
  }
  ILockBytes_Release(lb);
  return hr;
}

/* the bytes of a medium of one of the three byte kinds */
static HRESULT medium_to_blob(const STGMEDIUM *m, Blob *b) {
  switch (m->tymed) {
    case TYMED_HGLOBAL: {
      SIZE_T n;
      const void *p;
      if (!m->hGlobal) return E_FAIL;
      n = GlobalSize(m->hGlobal);
      p = GlobalLock(m->hGlobal);
      if (!p) return E_FAIL;
      if (!blob_set(b, p, n)) { GlobalUnlock(m->hGlobal); return E_OUTOFMEMORY; }
      GlobalUnlock(m->hGlobal);
      return S_OK;
    }
    case TYMED_ISTREAM: return m->pstm ? stream_to_blob(m->pstm, b) : E_FAIL;
    case TYMED_ISTORAGE: return m->pstg ? storage_to_blob(m->pstg, b) : E_FAIL;
    default: return E_FAIL;
  }
}

/* the blob as a compound file (E_FAIL when it is not one) */
static HRESULT blob_open_storage(const Blob *b, IStorage **out) {
  HGLOBAL h;
  ILockBytes *lb = NULL;
  void *p;
  HRESULT hr;
  *out = NULL;
  h = GlobalAlloc(GMEM_MOVEABLE, b->size ? b->size : 1);
  if (!h) return E_OUTOFMEMORY;
  p = GlobalLock(h);
  if (!p) { GlobalFree(h); return E_OUTOFMEMORY; }
  if (b->size) mem_copy(p, b->data, b->size);
  GlobalUnlock(h);
  hr = CreateILockBytesOnHGlobal(h, TRUE, &lb);
  if (FAILED(hr)) { GlobalFree(h); return hr; }
  hr = StgOpenStorageOnILockBytes(lb, NULL, STGM_SHARE_EXCLUSIVE | STGM_READWRITE, NULL, 0, out);
  ILockBytes_Release(lb);
  if (FAILED(hr)) { *out = NULL; return E_FAIL; }
  return S_OK;
}

/* a new medium of the wanted kind from the bytes */
static HRESULT blob_to_medium(const Blob *b, DWORD kind, STGMEDIUM *out) {
  mem_zero(out, sizeof(*out));
  switch (kind) {
    case TYMED_HGLOBAL: case TYMED_ISTREAM: {
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, b->size ? b->size : 1);
      void *p;
      if (!h) return E_OUTOFMEMORY;
      p = GlobalLock(h);
      if (!p) { GlobalFree(h); return E_OUTOFMEMORY; }
      if (b->size) mem_copy(p, b->data, b->size);
      GlobalUnlock(h);
      if (kind == TYMED_HGLOBAL) { out->tymed = TYMED_HGLOBAL; out->hGlobal = h; return S_OK; }
      /* a stream whose length is the size of the data */
      {
        IStream *s = NULL;
        HRESULT hr = CreateStreamOnHGlobal(h, TRUE, &s);
        ULARGE_INTEGER len;
        if (FAILED(hr)) { GlobalFree(h); return hr; }
        len.QuadPart = b->size;
        IStream_SetSize(s, len);
        out->tymed = TYMED_ISTREAM;
        out->pstm = s;
        return S_OK;
      }
    }
    case TYMED_ISTORAGE: {
      IStorage *s;
      HRESULT hr = blob_open_storage(b, &s);
      if (FAILED(hr)) return hr;
      out->tymed = TYMED_ISTORAGE;
      out->pstg = s;
      return S_OK;
    }
    default: return DV_E_TYMED;
  }
}

/* the bytes into a medium the caller made (GetDataHere) */
static HRESULT blob_to_here(const Blob *b, STGMEDIUM *dst) {
  switch (dst->tymed) {
    case TYMED_HGLOBAL: {
      void *p;
      if (!dst->hGlobal) return E_INVALIDARG;
      if (GlobalSize(dst->hGlobal) < b->size) return E_FAIL;
      p = GlobalLock(dst->hGlobal);
      if (!p) return E_FAIL;
      if (b->size) mem_copy(p, b->data, b->size);
      GlobalUnlock(dst->hGlobal);
      return S_OK;
    }
    case TYMED_ISTREAM:
      if (!dst->pstm) return E_INVALIDARG;
      return b->size > 0xFFFFFFFFu ? E_FAIL : stream_write_all(dst->pstm, b->data, (ULONG)b->size);
    case TYMED_ISTORAGE: {
      IStorage *s;
      HRESULT hr;
      if (!dst->pstg) return E_INVALIDARG;
      hr = blob_open_storage(b, &s);
      if (FAILED(hr)) return E_FAIL;
      hr = IStorage_CopyTo(s, 0, NULL, NULL, dst->pstg);
      IStorage_Release(s);
      return FAILED(hr) ? E_FAIL : S_OK;
    }
    default: return DV_E_TYMED;
  }
}

/* ---- the OLE clipboard ------------------------------------------------------------------------------------------------------------------------------------------- */

/* The clipboard of the process. A data object set with OleSetClipboard stays the live source; OleFlushClipboard renders every format into a private copy and lets go of the
 * source. What OleGetClipboard returns is one object per clipboard state; it binds to the state that is current at its first use (so a source set after OleGetClipboard is
 * the one that is asked) and keeps a reference to the source it bound to. */

typedef struct ClipEntry {
  FORMATETC fmt;               /* ptd is always NULL here */
  DWORD kind;                  /* the TYMED_ kind of the rendered data */
  Blob blob;                   /* the bytes (TYMED_HGLOBAL / ISTREAM / ISTORAGE) */
  HANDLE gdi;                  /* the handle (TYMED_GDI / MFPICT / ENHMF) */
} ClipEntry;

typedef struct ClipData {      /* immutable once published */
  LONG refs;
  FORMATETC *formats;          /* what the source offers (ptd on the heap) */
  ULONG nformats;
  ClipEntry *entries;          /* rendered data after a flush */
  ULONG nentries;
} ClipData;

typedef struct HereCache { CLIPFORMAT cf; Blob blob; } HereCache;

typedef struct ClipConsumer {
  IDataObject iface;
  LONG refs;
  BOOL bound;
  ClipData *data;
  IDataObject *source;
  HereCache *here;             /* data fetched for GetDataHere, per format */
  ULONG nhere;
} ClipConsumer;

static struct {
  IDataObject *source;
  ClipData *data;
  DWORD owner;                 /* thread that set the source */
  ClipConsumer *consumer;      /* the object handed out for the current state (no reference of its own) */
} g_clip;

static const IDataObjectVtbl g_clip_vtbl;

static FORMATETC *formats_dup(const FORMATETC *f, ULONG n) {
  FORMATETC *r;
  if (!n) return NULL;
  r = mem_alloc((SIZE_T)n * sizeof(FORMATETC));
  if (!r) return NULL;
  for (ULONG i = 0; i < n; i++) { r[i] = f[i]; r[i].ptd = ptd_copy(f[i].ptd, FALSE); }
  return r;
}

static void clipentry_free(ClipEntry *e) {
  blob_free(&e->blob);
  if (e->gdi) {
    STGMEDIUM m;
    mem_zero(&m, sizeof(m));
    m.tymed = e->kind;
    m.hGlobal = e->gdi;
    ReleaseStgMedium(&m);
    e->gdi = NULL;
  }
}
static void clipdata_release(ClipData *d) {
  if (!d || InterlockedDecrement(&d->refs) != 0) return;
  for (ULONG i = 0; i < d->nformats; i++) mem_free(d->formats[i].ptd);
  mem_free(d->formats);
  for (ULONG i = 0; i < d->nentries; i++) clipentry_free(&d->entries[i]);
  mem_free(d->entries);
  mem_free(d);
}

/* the formats a data object offers (the target devices are copied) */
static HRESULT clip_snapshot(IDataObject *src, ClipData **out) {
  IEnumFORMATETC *en = NULL;
  HRESULT hr;
  FORMATETC *arr = NULL, f;
  ULONG n = 0, cap = 0;
  ClipData *d;
  *out = NULL;
  hr = IDataObject_EnumFormatEtc(src, DATADIR_GET, &en);
  if (FAILED(hr)) return hr;
  if (!en) return E_FAIL;
  while (n < 4096 && IEnumFORMATETC_Next(en, 1, &f, NULL) == S_OK) {
    if (n == cap) {
      FORMATETC *bigger = mem_realloc(arr, (SIZE_T)(cap ? cap * 2 : 16) * sizeof(FORMATETC));
      if (!bigger) { CoTaskMemFree(f.ptd); break; }
      arr = bigger;
      cap = cap ? cap * 2 : 16;
    }
    arr[n] = f;
    arr[n].ptd = ptd_copy(f.ptd, FALSE);
    CoTaskMemFree(f.ptd);
    n++;
  }
  IEnumFORMATETC_Release(en);
  d = mem_alloc(sizeof(*d));
  if (!d) { for (ULONG i = 0; i < n; i++) mem_free(arr[i].ptd); mem_free(arr); return E_OUTOFMEMORY; }
  d->refs = 1;
  d->formats = arr;
  d->nformats = n;
  *out = d;
  return S_OK;
}

/* does the clipboard offer the format; which kinds of medium */
static BOOL clip_find(const ClipData *d, CLIPFORMAT cf, DWORD *tymeds) {
  BOOL found = FALSE;
  DWORD t = 0;
  if (d) for (ULONG i = 0; i < d->nformats; i++) if (d->formats[i].cfFormat == cf) { found = TRUE; t |= d->formats[i].tymed; }
  if (!found && d) {
    /* the system makes a metafile from an enhanced metafile and the other way round */
    CLIPFORMAT other = cf == CF_ENHMETAFILE ? CF_METAFILEPICT : cf == CF_METAFILEPICT ? CF_ENHMETAFILE : 0;
    if (other) for (ULONG i = 0; i < d->nformats; i++) if (d->formats[i].cfFormat == other) { found = TRUE; t = cf == CF_ENHMETAFILE ? TYMED_ENHMF : TYMED_MFPICT; }
  }
  if (tymeds) *tymeds = t;
  return found;
}

static const DWORD kBlobOrder[3] = {TYMED_HGLOBAL, TYMED_ISTREAM, TYMED_ISTORAGE};

/* a medium for the request from bytes: the kind the data came in first, then global memory, stream, storage */
static HRESULT clip_blob_to_request(const Blob *b, DWORD own_kind, DWORD mask, STGMEDIUM *out) {
  DWORD order[4];
  order[0] = own_kind;
  for (int i = 0; i < 3; i++) order[i + 1] = kBlobOrder[i];
  if (!(mask & BLOB_TYMEDS)) return DV_E_TYMED;
  for (int i = 0; i < 4; i++) if (mask & order[i]) return blob_to_medium(b, order[i], out);
  return DV_E_TYMED;
}

static CLIPFORMAT clip_cf_of_kind(DWORD kind) { return kind == TYMED_MFPICT ? CF_METAFILEPICT : kind == TYMED_ENHMF ? CF_ENHMETAFILE : kind == TYMED_GDI ? CF_BITMAP : CF_TEXT; }

static ClipConsumer *consumer_of(IDataObject *d) { return (ClipConsumer *)d; }

static void consumer_bind(ClipConsumer *c) {
  lock();
  if (!c->bound) {
    c->bound = TRUE;
    if (g_clip.data) { InterlockedIncrement(&g_clip.data->refs); c->data = g_clip.data; }
    /* (a reference is taken while the lock is held: the source cannot go away in between, and AddRef is a leaf call) */
    if (g_clip.source) { c->source = g_clip.source; IDataObject_AddRef(c->source); }
  }
  unlock();
}

static HRESULT WINAPI cc_qi(IDataObject *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDataObject)) { *out = me; IDataObject_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI cc_addref(IDataObject *me) { return (ULONG)InterlockedIncrement(&consumer_of(me)->refs); }
static ULONG WINAPI cc_release(IDataObject *me) {
  ClipConsumer *c = consumer_of(me);
  LONG n;
  lock();
  n = InterlockedDecrement(&c->refs);
  if (n == 0 && g_clip.consumer == c) g_clip.consumer = NULL;
  unlock();
  if (n == 0) {
    if (c->source) IDataObject_Release(c->source);
    clipdata_release(c->data);
    for (ULONG i = 0; i < c->nhere; i++) blob_free(&c->here[i].blob);
    mem_free(c->here);
    mem_free(c);
  }
  return (ULONG)n;
}

/* GetData answered by converting between the kinds of byte medium (a stream from global memory ...) */
static HRESULT clip_get_converted(ClipConsumer *c, FORMATETC *fmt, STGMEDIUM *med, HRESULT source_hr) {
  DWORD offered = 0;
  if (!clip_find(c->data, fmt->cfFormat, &offered)) return DV_E_FORMATETC;
  if (offered & fmt->tymed) return source_hr;
  if ((fmt->tymed & BLOB_TYMEDS) && (offered & BLOB_TYMEDS)) {
    for (int i = 0; i < 3; i++) {
      FORMATETC f = *fmt;
      STGMEDIUM m;
      if (!(offered & kBlobOrder[i])) continue;
      f.tymed = kBlobOrder[i];
      mem_zero(&m, sizeof(m));
      if (IDataObject_GetData(c->source, &f, &m) == S_OK) {
        Blob b = {NULL, 0};
        HRESULT hr = medium_to_blob(&m, &b);
        ReleaseStgMedium(&m);
        if (SUCCEEDED(hr)) { hr = clip_blob_to_request(&b, kBlobOrder[i], fmt->tymed, med); blob_free(&b); return hr; }
      }
    }
    return source_hr;
  }
  return DV_E_TYMED;
}

static ClipEntry *clip_entry_for(const ClipData *d, CLIPFORMAT cf) {
  if (d) for (ULONG i = 0; i < d->nentries; i++) if (d->entries[i].fmt.cfFormat == cf) return &d->entries[i];
  return NULL;
}

static HRESULT clip_get_flushed(ClipConsumer *c, FORMATETC *fmt, STGMEDIUM *med) {
  ClipEntry *e = clip_entry_for(c->data, fmt->cfFormat);
  if (!e) return DV_E_FORMATETC;
  if (e->kind & BLOB_TYMEDS) return clip_blob_to_request(&e->blob, e->kind, fmt->tymed, med);
  if (!(fmt->tymed & e->kind)) return DV_E_TYMED;
  {
    HANDLE h = OleDuplicateData(e->gdi, clip_cf_of_kind(e->kind), 0);
    if (!h) return E_OUTOFMEMORY;
    med->tymed = e->kind;
    med->hGlobal = h;
    return S_OK;
  }
}

static HRESULT WINAPI cc_getdata(IDataObject *me, FORMATETC *fmt, STGMEDIUM *med) {
  ClipConsumer *c = consumer_of(me);
  HRESULT hr;
  if (!fmt || !med) return E_INVALIDARG;
  mem_zero(med, sizeof(*med));
  consumer_bind(c);
  if (!c->source) return clip_get_flushed(c, fmt, med);
  hr = IDataObject_GetData(c->source, fmt, med);
  if (hr == S_OK) return hr;
  mem_zero(med, sizeof(*med));
  return clip_get_converted(c, fmt, med, hr);
}

static Blob *here_find(ClipConsumer *c, CLIPFORMAT cf) {
  for (ULONG i = 0; i < c->nhere; i++) if (c->here[i].cf == cf) return &c->here[i].blob;
  return NULL;
}

/* a medium of the kind to hand to a source's GetDataHere that is not ours */
static BOOL here_medium_make(DWORD kind, STGMEDIUM *m) {
  mem_zero(m, sizeof(*m));
  m->tymed = kind;
  if (kind == TYMED_HGLOBAL) { m->hGlobal = GlobalAlloc(GMEM_MOVEABLE, 4096); return m->hGlobal != NULL; }
  if (kind == TYMED_ISTREAM) return SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &m->pstm));
  if (kind == TYMED_ISTORAGE) return SUCCEEDED(StgCreateDocfile(NULL, STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_DELETEONRELEASE, 0, &m->pstg));
  return FALSE;
}

static HRESULT WINAPI cc_getdatahere(IDataObject *me, FORMATETC *fmt, STGMEDIUM *med) {
  ClipConsumer *c = consumer_of(me);
  FORMATETC f;
  Blob *b;
  HRESULT hr;
  if (!fmt || !med) return E_INVALIDARG;
  consumer_bind(c);
  if (!(med->tymed & BLOB_TYMEDS)) return DV_E_TYMED;
  if (!c->source) {
    ClipEntry *e = clip_entry_for(c->data, fmt->cfFormat);
    if (!e) return DV_E_FORMATETC;
    if (!(e->kind & BLOB_TYMEDS)) return DV_E_TYMED;
    return blob_to_here(&e->blob, med);
  }
  /* first the source is asked to fill the caller's medium */
  f = *fmt;
  f.tymed = med->tymed;
  hr = IDataObject_GetDataHere(c->source, &f, med);
  if (hr == S_OK) return hr;
  b = here_find(c, fmt->cfFormat);
  if (!b) {
    DWORD offered = 0, kind = 0;
    STGMEDIUM m;
    Blob nb = {NULL, 0};
    if (!clip_find(c->data, fmt->cfFormat, &offered)) return DV_E_FORMATETC;
    for (int i = 0; i < 3 && !kind; i++) if (offered & kBlobOrder[i]) kind = kBlobOrder[i];
    if (!kind) return DV_E_TYMED;
    f = *fmt;
    f.tymed = kind;
    /* then in the kind the source offers it, then the data itself */
    if (kind != med->tymed && here_medium_make(kind, &m)) {
      if (IDataObject_GetDataHere(c->source, &f, &m) == S_OK) medium_to_blob(&m, &nb);
      ReleaseStgMedium(&m);
    }
    if (!nb.data) {
      mem_zero(&m, sizeof(m));
      hr = IDataObject_GetData(c->source, &f, &m);
      if (hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
      hr = medium_to_blob(&m, &nb);
      ReleaseStgMedium(&m);
      if (FAILED(hr)) return hr;
    }
    {
      HereCache *bigger;
      lock();
      if (c->nhere >= 256) { unlock(); blob_free(&nb); return E_OUTOFMEMORY; }
      bigger = mem_realloc(c->here, (SIZE_T)(c->nhere + 1) * sizeof(HereCache));
      if (!bigger) { unlock(); blob_free(&nb); return E_OUTOFMEMORY; }
      c->here = bigger;
      c->here[c->nhere].cf = fmt->cfFormat;
      c->here[c->nhere].blob = nb;
      c->nhere++;
      unlock();
    }
    b = here_find(c, fmt->cfFormat);
  }
  return blob_to_here(b, med);
}

static HRESULT WINAPI cc_querygetdata(IDataObject *me, FORMATETC *fmt) {
  ClipConsumer *c = consumer_of(me);
  if (!fmt) return E_INVALIDARG;
  consumer_bind(c);
  if (fmt->lindex != -1 || fmt->dwAspect != DVASPECT_CONTENT) return DV_E_FORMATETC;
  if (!clip_find(c->data, fmt->cfFormat, NULL)) return DV_E_CLIPFORMAT;
  return S_OK;
}
static HRESULT WINAPI cc_canonical(IDataObject *me, FORMATETC *in, FORMATETC *out) { (void)me; (void)in; if (out) out->ptd = NULL; return E_NOTIMPL; }
static HRESULT WINAPI cc_setdata(IDataObject *me, FORMATETC *fmt, STGMEDIUM *med, BOOL release) { (void)me; (void)fmt; (void)med; (void)release; return E_NOTIMPL; }
static HRESULT WINAPI cc_enumformatetc(IDataObject *me, DWORD dir, IEnumFORMATETC **out) {
  ClipConsumer *c = consumer_of(me);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (dir == DATADIR_SET) return E_NOTIMPL;
  if (dir != DATADIR_GET) return E_INVALIDARG;
  consumer_bind(c);
  return enum_formatetc_create(c->data ? c->data->formats : NULL, c->data ? c->data->nformats : 0, out);
}
static HRESULT WINAPI cc_dadvise(IDataObject *me, FORMATETC *f, DWORD advf, IAdviseSink *sink, DWORD *conn) { (void)me; (void)f; (void)advf; (void)sink; if (conn) *conn = 0; return E_NOTIMPL; }
static HRESULT WINAPI cc_dunadvise(IDataObject *me, DWORD conn) { (void)me; (void)conn; return E_NOTIMPL; }
static HRESULT WINAPI cc_enumdadvise(IDataObject *me, IEnumSTATDATA **out) { (void)me; if (out) *out = NULL; return E_NOTIMPL; }
static const IDataObjectVtbl g_clip_vtbl = {cc_qi, cc_addref, cc_release, cc_getdata, cc_getdatahere, cc_querygetdata, cc_canonical, cc_setdata, cc_enumformatetc, cc_dadvise, cc_dunadvise, cc_enumdadvise};

static BOOL ole_initialized_here(void) {
  ThreadData *td = thread_data(FALSE);
  return td && td->ole_init_count > 0;
}

EXPORT HRESULT WINAPI OleGetClipboard(IDataObject **out) {
  ClipConsumer *c, *fresh;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  fresh = mem_alloc(sizeof(*fresh));
  if (!fresh) return E_OUTOFMEMORY;
  fresh->iface.lpVtbl = (IDataObjectVtbl *)&g_clip_vtbl;
  fresh->refs = 1;
  lock();
  c = g_clip.consumer;
  if (c) InterlockedIncrement(&c->refs);
  else g_clip.consumer = c = fresh;
  unlock();
  if (c != fresh) mem_free(fresh);
  *out = &c->iface;
  return S_OK;
}

EXPORT HRESULT WINAPI OleGetClipboardWithEnterpriseInfo(IDataObject **data, PWSTR *enterprise_id, PWSTR *source, PWSTR *target, PWSTR *description) {
  if (enterprise_id) *enterprise_id = NULL;
  if (source) *source = NULL;
  if (target) *target = NULL;
  if (description) *description = NULL;
  return OleGetClipboard(data);
}

EXPORT HRESULT WINAPI OleGetPackageClipboardOwner(PWSTR *package_family_name) {
  if (package_family_name) *package_family_name = NULL;
  return E_NOTIMPL;
}

EXPORT HRESULT WINAPI OleSetClipboard(IDataObject *data) {
  ClipData *snap = NULL, *old_data;
  IDataObject *old_source;
  HRESULT hr;
  if (!ole_initialized_here()) return CO_E_NOTINITIALIZED;
  if (data) {
    hr = clip_snapshot(data, &snap);
    if (FAILED(hr)) return hr;
    IDataObject_AddRef(data);
  }
  lock();
  old_source = g_clip.source;
  old_data = g_clip.data;
  g_clip.source = data;
  g_clip.data = snap;
  g_clip.owner = GetCurrentThreadId();
  g_clip.consumer = NULL;
  unlock();
  if (old_source) IDataObject_Release(old_source);
  clipdata_release(old_data);
  return S_OK;
}

EXPORT HRESULT WINAPI OleIsCurrentClipboard(IDataObject *data) {
  BOOL same;
  if (!data) return S_FALSE;
  lock();
  same = g_clip.source == data;
  unlock();
  return same ? S_OK : S_FALSE;
}

/* every format of the source into a private copy */
static ClipData *clip_render(IDataObject *src, const ClipData *snap) {
  static const DWORD kinds[6] = {TYMED_HGLOBAL, TYMED_ISTREAM, TYMED_ISTORAGE, TYMED_MFPICT, TYMED_ENHMF, TYMED_GDI};
  ClipData *nd = mem_alloc(sizeof(*nd));
  if (!nd) return NULL;
  nd->refs = 1;
  nd->nformats = snap->nformats;
  nd->formats = formats_dup(snap->formats, snap->nformats);
  if (snap->nformats && !nd->formats) { nd->nformats = 0; clipdata_release(nd); return NULL; }
  if (snap->nformats) nd->entries = mem_alloc((SIZE_T)snap->nformats * sizeof(ClipEntry));
  for (ULONG i = 0; nd->entries && i < snap->nformats; i++) {
    const FORMATETC *f = &snap->formats[i];
    BOOL have = FALSE;
    for (ULONG j = 0; j < nd->nentries; j++) if (nd->entries[j].fmt.cfFormat == f->cfFormat) have = TRUE;
    for (int k = 0; k < 6 && !have; k++) {
      FORMATETC rf = *f;
      STGMEDIUM m;
      ClipEntry *e = &nd->entries[nd->nentries];
      if (!(f->tymed & kinds[k])) continue;
      rf.tymed = kinds[k];
      mem_zero(&m, sizeof(m));
      if (IDataObject_GetData(src, &rf, &m) != S_OK) continue;
      e->fmt = *f;
      e->fmt.ptd = NULL;
      e->fmt.tymed = kinds[k];
      e->kind = kinds[k];
      if (kinds[k] & BLOB_TYMEDS) {
        have = SUCCEEDED(medium_to_blob(&m, &e->blob)) && m.tymed == kinds[k];
      } else if (m.tymed == kinds[k]) {
        e->gdi = OleDuplicateData(m.hGlobal, clip_cf_of_kind(kinds[k]), 0);
        have = e->gdi != NULL;
      }
      ReleaseStgMedium(&m);
      if (have) nd->nentries++; else clipentry_free(e);
    }
  }
  return nd;
}

/* release_source FALSE: the owner thread is going away; Windows does not release the source then either */
static HRESULT clip_flush_impl(BOOL release_source, DWORD only_owner) {
  IDataObject *src;
  ClipData *snap, *nd, *old = NULL;
  BOOL swapped = FALSE;
  lock();
  src = g_clip.source;
  snap = g_clip.data;
  if (!src || !snap || (only_owner && g_clip.owner != only_owner)) { unlock(); return S_OK; }
  IDataObject_AddRef(src);
  InterlockedIncrement(&snap->refs);
  unlock();
  nd = clip_render(src, snap);
  if (nd) {
    lock();
    if (g_clip.source == src) {
      old = g_clip.data;
      g_clip.data = nd;
      g_clip.source = NULL;
      g_clip.consumer = NULL;
      swapped = TRUE;
    }
    unlock();
  }
  if (swapped) {
    clipdata_release(old);
    if (release_source) IDataObject_Release(src);
  } else {
    clipdata_release(nd);
  }
  clipdata_release(snap);
  IDataObject_Release(src);
  return nd ? S_OK : E_OUTOFMEMORY;
}

EXPORT HRESULT WINAPI OleFlushClipboard(void) {
  if (!ole_initialized_here()) return E_FAIL;
  return clip_flush_impl(TRUE, 0);
}

static void clip_thread_gone(ThreadData *td) {
  (void)td;
  clip_flush_impl(FALSE, GetCurrentThreadId());
}

/* ---- drag and drop ------------------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct DropReg { struct DropReg *next; HWND hwnd; IDropTarget *target; DWORD tid; } DropReg;
static DropReg *g_drop;

EXPORT HRESULT WINAPI RegisterDragDrop(HWND hwnd, IDropTarget *target) {
  ThreadData *td = thread_data(FALSE);
  DropReg *r;
  if (!td || (td->ole_init_count == 0 && td->apartment_kind != APT_MTA)) return g_ole_window_class ? CO_E_NOTINITIALIZED : E_OUTOFMEMORY;
  if (!target) return E_INVALIDARG;
  if (!hwnd) return DRAGDROP_E_INVALIDHWND;
  r = mem_alloc(sizeof(*r));
  if (!r) return E_OUTOFMEMORY;
  r->hwnd = hwnd;
  r->target = target;
  r->tid = GetCurrentThreadId();
  lock();
  for (DropReg *p = g_drop; p; p = p->next) if (p->hwnd == hwnd) { unlock(); mem_free(r); return DRAGDROP_E_ALREADYREGISTERED; }
  r->next = g_drop;
  g_drop = r;
  unlock();
  IDropTarget_AddRef(target);
  return S_OK;
}

EXPORT HRESULT WINAPI RevokeDragDrop(HWND hwnd) {
  DropReg **pp, *r = NULL;
  if (!hwnd) return DRAGDROP_E_INVALIDHWND;
  lock();
  for (pp = &g_drop; *pp; pp = &(*pp)->next) if ((*pp)->hwnd == hwnd) { r = *pp; *pp = r->next; break; }
  unlock();
  if (!r) return DRAGDROP_E_NOTREGISTERED;
  IDropTarget_Release(r->target);
  mem_free(r);
  return S_OK;
}

/* the registrations of a thread end with its OLE */
static void dragdrop_thread_gone(DWORD tid) {
  for (;;) {
    DropReg **pp, *r = NULL;
    lock();
    for (pp = &g_drop; *pp; pp = &(*pp)->next) if ((*pp)->tid == tid) { r = *pp; *pp = r->next; break; }
    unlock();
    if (!r) return;
    IDropTarget_Release(r->target);
    mem_free(r);
  }
}

/* Without a window server there is no pointer and no window under it: the drag runs as Windows runs it when the pointer is over nothing. The source decides (by
 * QueryContinueDrag) when the drag ends; it is asked a bounded number of times so that a source that waits for the user cannot hold the thread forever. */
EXPORT HRESULT WINAPI DoDragDrop(IDataObject *data, IDropSource *source, DWORD ok_effects, DWORD *effect) {
  ThreadData *td = thread_data(FALSE);
  IEnumFORMATETC *en = NULL;
  FORMATETC f, first;
  BOOL have_first = FALSE;
  HRESULT hr, result = DRAGDROP_S_CANCEL;
  (void)ok_effects;
  if (!td || td->ole_init_count == 0) return CO_E_NOTINITIALIZED;
  if (!data || !source || !effect) return E_INVALIDARG;
  *effect = DROPEFFECT_NONE;
  /* the data object is asked for its formats and for the first one */
  hr = IDataObject_EnumFormatEtc(data, DATADIR_GET, &en);
  if (SUCCEEDED(hr) && en) {
    while (IEnumFORMATETC_Next(en, 1, &f, NULL) == S_OK) CoTaskMemFree(f.ptd);
    IEnumFORMATETC_Reset(en);
    if (IEnumFORMATETC_Next(en, 1, &first, NULL) == S_OK) {
      have_first = TRUE;
      while (IEnumFORMATETC_Next(en, 1, &f, NULL) == S_OK) CoTaskMemFree(f.ptd);
    }
    IEnumFORMATETC_Release(en);
    if (have_first) { IDataObject_QueryGetData(data, &first); CoTaskMemFree(first.ptd); }
  }
  for (int i = 0; i < 100; i++) {
    DWORD keys = key_down("GetAsyncKeyState", 1 /* VK_LBUTTON */) ? MK_LBUTTON : 0;
    BOOL esc = key_down("GetAsyncKeyState", 0x1B /* VK_ESCAPE */);
    hr = IDropSource_QueryContinueDrag(source, esc, keys);
    if (hr == DRAGDROP_S_DROP || hr == DRAGDROP_S_CANCEL) { result = hr; break; }
    if (FAILED(hr)) { result = hr; break; }
    IDropSource_GiveFeedback(source, DROPEFFECT_NONE);
  }
  return result;
}

/* (undocumented export) */
EXPORT HRESULT WINAPI DragDropSetFDT(HWND hwnd, DWORD flags) { (void)hwnd; (void)flags; return S_OK; }

/* ---- the data advise holder ---------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct DahConn { FORMATETC fmt; DWORD advf; IAdviseSink *sink; DWORD id; } DahConn;
typedef struct DataAdviseHolder {
  IDataAdviseHolder iface;
  LONG refs;
  DahConn *c;
  ULONG n;
  DWORD next_id;
} DataAdviseHolder;

static const IDataAdviseHolderVtbl g_dah_vtbl;
static DataAdviseHolder *dah_of(IDataAdviseHolder *h) { return (DataAdviseHolder *)h; }

static HRESULT WINAPI dah_qi(IDataAdviseHolder *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDataAdviseHolder)) { *out = me; IDataAdviseHolder_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI dah_addref(IDataAdviseHolder *me) { return (ULONG)InterlockedIncrement(&dah_of(me)->refs); }
static ULONG WINAPI dah_release(IDataAdviseHolder *me) {
  DataAdviseHolder *h = dah_of(me);
  LONG n = InterlockedDecrement(&h->refs);
  if (n == 0) {
    for (ULONG i = 0; i < h->n; i++) { mem_free(h->c[i].fmt.ptd); if (h->c[i].sink) IAdviseSink_Release(h->c[i].sink); }
    mem_free(h->c);
    mem_free(h);
  }
  return (ULONG)n;
}

/* a connection leaves the table (its sink is released outside the lock) */
static BOOL dah_remove(DataAdviseHolder *h, DWORD id) {
  IAdviseSink *sink = NULL;
  BOOL found = FALSE;
  lock();
  for (ULONG i = 0; i < h->n; i++) {
    if (h->c[i].id != id) continue;
    sink = h->c[i].sink;
    mem_free(h->c[i].fmt.ptd);
    for (ULONG j = i + 1; j < h->n; j++) h->c[j - 1] = h->c[j];
    h->n--;
    found = TRUE;
    break;
  }
  unlock();
  if (sink) IAdviseSink_Release(sink);
  return found;
}

/* the sinks (all of them, or the one connection) are told about a change */
static void dah_notify(DataAdviseHolder *h, IDataObject *obj, DWORD advf, DWORD only_id) {
  DahConn *copy = NULL;
  ULONG n;
  lock();
  n = h->n;
  if (n) copy = mem_alloc((SIZE_T)n * sizeof(DahConn));
  if (copy) {
    for (ULONG i = 0; i < n; i++) {
      copy[i] = h->c[i];
      copy[i].fmt.ptd = ptd_copy(h->c[i].fmt.ptd, FALSE);
      if (copy[i].sink) IAdviseSink_AddRef(copy[i].sink);      /* (a leaf call, made under the lock so that the sink cannot go in between) */
    }
  } else {
    n = 0;
  }
  unlock();
  for (ULONG i = 0; i < n; i++) {
    DahConn *c = &copy[i];
    STGMEDIUM stg;
    BOOL has = FALSE, send = TRUE;
    mem_zero(&stg, sizeof(stg));
    if (only_id && c->id != only_id) send = FALSE;
    if (send && !(c->advf & ADVF_NODATA) && !(advf & ADVF_NODATA)) {
      if (obj && IDataObject_GetData(obj, &c->fmt, &stg) == S_OK) has = TRUE; else send = FALSE;
    }
    if (send && c->sink) IAdviseSink_OnDataChange(c->sink, &c->fmt, &stg);
    if (has) ReleaseStgMedium(&stg);
    if (send && (c->advf & ADVF_ONLYONCE)) dah_remove(h, c->id);
  }
  for (ULONG i = 0; i < n; i++) { mem_free(copy[i].fmt.ptd); if (copy[i].sink) IAdviseSink_Release(copy[i].sink); }
  mem_free(copy);
}

static HRESULT WINAPI dah_advise(IDataAdviseHolder *me, IDataObject *obj, FORMATETC *fmt, DWORD advf, IAdviseSink *sink, DWORD *conn) {
  DataAdviseHolder *h = dah_of(me);
  DahConn *bigger;
  DWORD id;
  if (!conn) return E_INVALIDARG;
  *conn = 0;
  if (!fmt || !sink) return E_INVALIDARG;
  lock();
  if (h->n >= 0x10000) { unlock(); return E_OUTOFMEMORY; }
  bigger = mem_realloc(h->c, (SIZE_T)(h->n + 1) * sizeof(DahConn));
  if (!bigger) { unlock(); return E_OUTOFMEMORY; }
  h->c = bigger;
  id = ++h->next_id;
  h->c[h->n].fmt = *fmt;
  h->c[h->n].fmt.ptd = ptd_copy(fmt->ptd, FALSE);
  h->c[h->n].advf = advf;
  h->c[h->n].sink = sink;
  h->c[h->n].id = id;
  h->n++;
  IAdviseSink_AddRef(sink);
  unlock();
  *conn = id;
  if ((advf & ADVF_PRIMEFIRST) && obj) dah_notify(h, obj, 0, id);
  return S_OK;
}
static HRESULT WINAPI dah_unadvise(IDataAdviseHolder *me, DWORD conn) { return dah_remove(dah_of(me), conn) ? S_OK : OLE_E_NOCONNECTION; }
static HRESULT WINAPI dah_enumadvise(IDataAdviseHolder *me, IEnumSTATDATA **out) {
  DataAdviseHolder *h = dah_of(me);
  STATDATA *items = NULL;
  ULONG n;
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  lock();
  n = h->n;
  if (n) items = mem_alloc((SIZE_T)n * sizeof(STATDATA));
  if (n && !items) { unlock(); return E_OUTOFMEMORY; }
  for (ULONG i = 0; i < n; i++) {
    items[i].formatetc = h->c[i].fmt;
    items[i].advf = h->c[i].advf;
    items[i].pAdvSink = h->c[i].sink;
    items[i].dwConnection = h->c[i].id;
  }
  hr = enum_statdata_create(items, n, out);        /* (copies the formats and takes its own references on the sinks) */
  unlock();
  mem_free(items);
  return hr;
}
static HRESULT WINAPI dah_senddatachange(IDataAdviseHolder *me, IDataObject *obj, DWORD reserved, DWORD advf) {
  (void)reserved;
  dah_notify(dah_of(me), obj, advf, 0);
  return S_OK;
}
static const IDataAdviseHolderVtbl g_dah_vtbl = {dah_qi, dah_addref, dah_release, dah_advise, dah_unadvise, dah_enumadvise, dah_senddatachange};

EXPORT HRESULT WINAPI CreateDataAdviseHolder(IDataAdviseHolder **out) {
  DataAdviseHolder *h;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  h = mem_alloc(sizeof(*h));
  if (!h) return E_OUTOFMEMORY;
  h->iface.lpVtbl = (IDataAdviseHolderVtbl *)&g_dah_vtbl;
  h->refs = 1;
  *out = &h->iface;
  return S_OK;
}

/* ---- the OLE advise holder -------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct OahConn { IAdviseSink *sink; DWORD id; } OahConn;
typedef struct OleAdviseHolder {
  IOleAdviseHolder iface;
  LONG refs;
  OahConn *c;
  ULONG n;
  DWORD next_id;
} OleAdviseHolder;

static const IOleAdviseHolderVtbl g_oah_vtbl;
static OleAdviseHolder *oah_of(IOleAdviseHolder *h) { return (OleAdviseHolder *)h; }

static HRESULT WINAPI oah_qi(IOleAdviseHolder *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IOleAdviseHolder)) { *out = me; IOleAdviseHolder_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI oah_addref(IOleAdviseHolder *me) { return (ULONG)InterlockedIncrement(&oah_of(me)->refs); }
static ULONG WINAPI oah_release(IOleAdviseHolder *me) {
  OleAdviseHolder *h = oah_of(me);
  LONG n = InterlockedDecrement(&h->refs);
  if (n == 0) {
    for (ULONG i = 0; i < h->n; i++) if (h->c[i].sink) IAdviseSink_Release(h->c[i].sink);
    mem_free(h->c);
    mem_free(h);
  }
  return (ULONG)n;
}
static HRESULT WINAPI oah_advise(IOleAdviseHolder *me, IAdviseSink *sink, DWORD *conn) {
  OleAdviseHolder *h = oah_of(me);
  OahConn *bigger;
  if (!conn) return E_INVALIDARG;
  *conn = 0;
  if (!sink) return E_INVALIDARG;
  lock();
  if (h->n >= 0x10000) { unlock(); return E_OUTOFMEMORY; }
  bigger = mem_realloc(h->c, (SIZE_T)(h->n + 1) * sizeof(OahConn));
  if (!bigger) { unlock(); return E_OUTOFMEMORY; }
  h->c = bigger;
  h->c[h->n].sink = sink;
  h->c[h->n].id = ++h->next_id;
  *conn = h->c[h->n].id;
  h->n++;
  IAdviseSink_AddRef(sink);
  unlock();
  return S_OK;
}
static HRESULT WINAPI oah_unadvise(IOleAdviseHolder *me, DWORD conn) {
  OleAdviseHolder *h = oah_of(me);
  IAdviseSink *sink = NULL;
  BOOL found = FALSE;
  lock();
  for (ULONG i = 0; i < h->n; i++) {
    if (h->c[i].id != conn) continue;
    sink = h->c[i].sink;
    for (ULONG j = i + 1; j < h->n; j++) h->c[j - 1] = h->c[j];
    h->n--;
    found = TRUE;
    break;
  }
  unlock();
  if (sink) IAdviseSink_Release(sink);
  return found ? S_OK : OLE_E_NOCONNECTION;
}
static HRESULT WINAPI oah_enumadvise(IOleAdviseHolder *me, IEnumSTATDATA **out) {
  OleAdviseHolder *h = oah_of(me);
  STATDATA *items = NULL;
  ULONG n;
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  lock();
  n = h->n;
  if (n) items = mem_alloc((SIZE_T)n * sizeof(STATDATA));
  if (n && !items) { unlock(); return E_OUTOFMEMORY; }
  for (ULONG i = 0; i < n; i++) { items[i].pAdvSink = h->c[i].sink; items[i].dwConnection = h->c[i].id; }
  hr = enum_statdata_create(items, n, out);
  unlock();
  mem_free(items);
  return hr;
}

/* which notification goes out */
enum { OAH_RENAME, OAH_SAVE, OAH_CLOSE };
static HRESULT oah_send(OleAdviseHolder *h, int what, IMoniker *mk) {
  IAdviseSink **sinks = NULL;
  ULONG n;
  lock();
  n = h->n;
  if (n) sinks = mem_alloc((SIZE_T)n * sizeof(*sinks));
  if (!sinks) n = 0;
  for (ULONG i = 0; i < n; i++) { sinks[i] = h->c[i].sink; if (sinks[i]) IAdviseSink_AddRef(sinks[i]); }
  unlock();
  for (ULONG i = 0; i < n; i++) {
    if (!sinks[i]) continue;
    if (what == OAH_RENAME) IAdviseSink_OnRename(sinks[i], mk);
    else if (what == OAH_SAVE) IAdviseSink_OnSave(sinks[i]);
    else IAdviseSink_OnClose(sinks[i]);
    IAdviseSink_Release(sinks[i]);
  }
  mem_free(sinks);
  return S_OK;
}
static HRESULT WINAPI oah_onrename(IOleAdviseHolder *me, IMoniker *mk) { return oah_send(oah_of(me), OAH_RENAME, mk); }
static HRESULT WINAPI oah_onsave(IOleAdviseHolder *me) { return oah_send(oah_of(me), OAH_SAVE, NULL); }
static HRESULT WINAPI oah_onclose(IOleAdviseHolder *me) { return oah_send(oah_of(me), OAH_CLOSE, NULL); }
static const IOleAdviseHolderVtbl g_oah_vtbl = {oah_qi, oah_addref, oah_release, oah_advise, oah_unadvise, oah_enumadvise, oah_onrename, oah_onsave, oah_onclose};

EXPORT HRESULT WINAPI CreateOleAdviseHolder(IOleAdviseHolder **out) {
  OleAdviseHolder *h;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  h = mem_alloc(sizeof(*h));
  if (!h) return E_OUTOFMEMORY;
  h->iface.lpVtbl = (IOleAdviseHolderVtbl *)&g_oah_vtbl;
  h->refs = 1;
  *out = &h->iface;
  return S_OK;
}

/* ---- in-place activation helpers ---------------------------------------------------------------------------------------------------------------------------------- */

typedef struct MenuDescriptor { HMENU combined; OLEMENUGROUPWIDTHS widths; HWND frame, active; } MenuDescriptor;

EXPORT HOLEMENU WINAPI OleCreateMenuDescriptor(HMENU combined, LPOLEMENUGROUPWIDTHS widths) {
  HGLOBAL h;
  MenuDescriptor *d;
  if (!combined || !widths) return NULL;
  h = GlobalAlloc(GMEM_MOVEABLE, sizeof(MenuDescriptor));
  if (!h) return NULL;
  d = GlobalLock(h);
  if (!d) { GlobalFree(h); return NULL; }
  mem_zero(d, sizeof(*d));
  d->combined = combined;
  d->widths = *widths;
  GlobalUnlock(h);
  return (HOLEMENU)h;
}

EXPORT HRESULT WINAPI OleSetMenuDescriptor(HOLEMENU menu, HWND frame, HWND active, LPOLEINPLACEFRAME frame_obj, LPOLEINPLACEACTIVEOBJECT active_obj) {
  (void)frame_obj;
  (void)active_obj;
  if (!frame) return E_INVALIDARG;
  if (menu) {
    MenuDescriptor *d = GlobalLock((HGLOBAL)menu);
    if (!d) return E_INVALIDARG;
    d->frame = frame;
    d->active = active;
    GlobalUnlock((HGLOBAL)menu);
  }
  return S_OK;
}

EXPORT HRESULT WINAPI OleDestroyMenuDescriptor(HOLEMENU menu) {
  if (menu) GlobalFree((HGLOBAL)menu);
  return S_OK;
}

typedef int (WINAPI *CopyAccelFn)(HACCEL, ACCEL *, int);

EXPORT BOOL WINAPI IsAccelerator(HACCEL table, int count, LPMSG msg, WORD *cmd) {
  CopyAccelFn copy = (CopyAccelFn)dyn_user("CopyAcceleratorTableW");
  ACCEL *entries;
  int n;
  BOOL hit = FALSE;
  if (!msg || !table || count <= 0 || !copy) return FALSE;
  if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN && msg->message != WM_CHAR && msg->message != WM_SYSCHAR) return FALSE;
  n = copy(table, NULL, 0);
  if (n <= 0 || n > 0x10000) return FALSE;
  if (n > count) n = count;
  entries = mem_alloc((SIZE_T)n * sizeof(ACCEL));
  if (!entries) return FALSE;
  n = copy(table, entries, n);
  for (int i = 0; i < n && !hit; i++) {
    const ACCEL *a = &entries[i];
    if (a->key != (WORD)msg->wParam) continue;
    if (msg->message == WM_CHAR || msg->message == WM_SYSCHAR) {
      if (!(a->fVirt & FVIRTKEY) && !!(a->fVirt & FALT) == (msg->message == WM_SYSCHAR)) hit = TRUE;
    } else if (a->fVirt & FVIRTKEY) {
      BYTE mask = 0;
      if (key_down("GetKeyState", 0x10 /* VK_SHIFT */)) mask |= FSHIFT;
      if (key_down("GetKeyState", 0x11 /* VK_CONTROL */)) mask |= FCONTROL;
      if (key_down("GetKeyState", 0x12 /* VK_MENU */)) mask |= FALT;
      if ((a->fVirt & (FSHIFT | FCONTROL | FALT)) == mask) hit = TRUE;
    }
    if (hit && cmd) *cmd = entries[i].cmd;
  }
  mem_free(entries);
  return hit;
}

EXPORT HRESULT WINAPI OleTranslateAccelerator(LPOLEINPLACEFRAME frame, LPOLEINPLACEFRAMEINFO info, LPMSG msg) {
  WORD cmd = 0;
  if (!frame || !info || !msg) return E_INVALIDARG;
  if (IsAccelerator(info->haccel, info->cAccelEntries, msg, &cmd)) return IOleInPlaceFrame_TranslateAccelerator(frame, msg, cmd);
  return S_FALSE;
}

EXPORT HRESULT WINAPI OleNoteObjectVisible(LPUNKNOWN unk, BOOL visible) { return CoLockObjectExternal(unk, visible, TRUE); }

EXPORT HRESULT WINAPI OleDraw(LPUNKNOWN unk, DWORD aspect, HDC hdc, LPCRECT rect) {
  IViewObject *view = NULL;
  RECTL r;
  HRESULT hr;
  if (!unk) return E_INVALIDARG;
  hr = IUnknown_QueryInterface(unk, &IID_IViewObject, (void **)&view);
  if (FAILED(hr)) return hr;
  if (rect) { r.left = rect->left; r.top = rect->top; r.right = rect->right; r.bottom = rect->bottom; }
  hr = IViewObject_Draw(view, aspect, -1, NULL, NULL, NULL, hdc, rect ? &r : NULL, NULL, NULL, 0);
  IViewObject_Release(view);
  return hr;
}

/* The icon of a class or file and the metafile that carries an icon with its label need GDI metafiles, which this system does not have yet: they fail the way they fail
 * when the icon cannot be made. */
EXPORT HGLOBAL WINAPI OleGetIconOfClass(REFCLSID clsid, LPOLESTR label, BOOL use_type_as_label) { (void)clsid; (void)label; (void)use_type_as_label; return NULL; }
EXPORT HGLOBAL WINAPI OleGetIconOfFile(LPOLESTR path, BOOL use_file_as_label) { (void)path; (void)use_file_as_label; return NULL; }
EXPORT HGLOBAL WINAPI OleMetafilePictFromIconAndLabel(HICON icon, LPOLESTR label, LPOLESTR source, UINT index) { (void)icon; (void)label; (void)source; (void)index; return NULL; }

/* ---- the data cache ------------------------------------------------------------------------------------------------------------------------------------------------ */

/* CreateDataCache: IOleCache2, IOleCacheControl, IPersistStorage, IDataObject and IViewObject2 over a list of cached formats. The entries are kept in this process; the
 * presentations are saved in the "\002OlePres%03d" streams of the object's storage (and, for the static picture classes, in "CONTENTS"). The pictures that need GDI are
 * kept as bytes when GDI cannot make the object, so that a load and a save keep them. */

static const GUID CLSID_Ole32NoClass = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
static const GUID CLSID_Ole32PicMetafile = {0x00000315, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID CLSID_Ole32PicDib = {0x00000316, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID CLSID_Ole32PicEnhMetafile = {0x00000319, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

#define DC_MAX_ENTRIES 4096

typedef struct CacheEntry {
  FORMATETC fmt;               /* ptd is always NULL; a view cache has cfFormat 0 until data tells the format */
  DWORD advf;
  DWORD id;                    /* the connection id: a DIB and its bitmap share one */
  BOOL derived;                /* the twin that came with the entry the caller asked for */
  BOOL is_view;                /* made by a view cache request: saved as a view cache while it has no data */
  BOOL has_data;
  STGMEDIUM med;               /* HGLOBAL / GDI / MFPICT / ENHMF, owned (no pUnkForRelease) */
  BOOL shared_out;             /* a handle of med went out to a caller without a copy */
  Blob raw;                    /* presentation bytes read from a stream that could not be made into a medium */
  DWORD sink_conn;             /* connection at the running data object */
  BOOL dirty;
} CacheEntry;

typedef struct DataCache {
  IUnknown inner;
  IDataObject data;
  IViewObject2 view;
  IOleCache2 cache;
  IOleCacheControl control;
  IPersistStorage persist;
  IAdviseSink sink;
  LONG refs;
  IUnknown *outer;             /* NULL: not aggregated */
  CLSID clsid;
  CacheEntry *e;
  ULONG n, cap;
  DWORD next_id;
  IStorage *stg;
  BOOL loaded;                 /* the entries hold what the storage has (a discard clears this) */
  BOOL dirty;
  IAdviseSink *view_sink;
  DWORD view_aspects, view_advf;
  IDataObject *running;
  DWORD freeze_next;
} DataCache;

#define DC_FROM(field, p) ((DataCache *)((char *)(p) - __builtin_offsetof(DataCache, field)))

static const IUnknownVtbl g_dc_inner_vtbl;
static const IDataObjectVtbl g_dc_data_vtbl;
static const IViewObject2Vtbl g_dc_view_vtbl;
static const IOleCache2Vtbl g_dc_cache_vtbl;
static const IOleCacheControlVtbl g_dc_control_vtbl;
static const IPersistStorageVtbl g_dc_persist_vtbl;
static const IAdviseSinkVtbl g_dc_sink_vtbl;

static IUnknown *dc_outer(DataCache *dc) { return dc->outer ? dc->outer : &dc->inner; }

static int dc_static_kind(const GUID *clsid) {
  if (IsEqualGUID(clsid, &CLSID_Ole32PicDib)) return 1;
  if (IsEqualGUID(clsid, &CLSID_Ole32PicMetafile)) return 2;
  if (IsEqualGUID(clsid, &CLSID_Ole32PicEnhMetafile)) return 3;
  return 0;
}

static DWORD dc_tymed_of(int cf) {
  switch (cf) {
    case CF_DIB: return TYMED_HGLOBAL;
    case CF_BITMAP: return TYMED_GDI;
    case CF_METAFILEPICT: return TYMED_MFPICT;
    case CF_ENHMETAFILE: return TYMED_ENHMF;
    default: return 0;
  }
}

/* a view cache of the icon aspect is a metafile cache */
static void dc_normalize(FORMATETC *f) {
  f->ptd = NULL;
  if (f->cfFormat == 0 && f->dwAspect == DVASPECT_ICON) { f->cfFormat = CF_METAFILEPICT; f->tymed = TYMED_MFPICT; }
}

static CacheEntry *dc_find_locked(DataCache *dc, const FORMATETC *f) {
  for (ULONG i = 0; i < dc->n; i++) {
    CacheEntry *e = &dc->e[i];
    if (e->fmt.cfFormat == f->cfFormat && e->fmt.dwAspect == f->dwAspect && e->fmt.lindex == f->lindex) return e;
  }
  return NULL;
}

static CacheEntry *dc_add_locked(DataCache *dc, const FORMATETC *f, DWORD advf, DWORD id, BOOL derived, BOOL is_view) {
  CacheEntry *e;
  if (dc->n >= DC_MAX_ENTRIES) return NULL;
  if (dc->n == dc->cap) {
    ULONG cap = dc->cap ? dc->cap * 2 : 8;
    CacheEntry *bigger = mem_realloc(dc->e, (SIZE_T)cap * sizeof(CacheEntry));
    if (!bigger) return NULL;
    dc->e = bigger;
    dc->cap = cap;
  }
  e = &dc->e[dc->n++];
  mem_zero(e, sizeof(*e));
  e->fmt = *f;
  e->fmt.ptd = NULL;
  e->fmt.lindex = -1;
  e->advf = advf;
  e->id = id;
  e->derived = derived;
  e->is_view = is_view;
  return e;
}

/* the entry the caller asked for and, for a DIB or a bitmap, the one the cache makes with it (always listed DIB first) */
static void dc_new_entries_locked(DataCache *dc, const FORMATETC *f, DWORD advf, DWORD id, BOOL is_view) {
  if (f->cfFormat == CF_DIB || f->cfFormat == CF_BITMAP) {
    FORMATETC d = *f, b = *f;
    d.cfFormat = CF_DIB; d.tymed = TYMED_HGLOBAL;
    b.cfFormat = CF_BITMAP; b.tymed = TYMED_GDI;
    dc_add_locked(dc, &d, advf, id, f->cfFormat != CF_DIB, FALSE);
    dc_add_locked(dc, &b, advf, id, f->cfFormat != CF_BITMAP, FALSE);
  } else {
    dc_add_locked(dc, f, advf, id, FALSE, is_view);
  }
}

static void ce_clear(CacheEntry *e) {
  if (e->has_data) {
    if (!(e->shared_out && e->med.tymed != TYMED_HGLOBAL)) ReleaseStgMedium(&e->med);      /* (a handle that went out is not freed under its holder) */
    mem_zero(&e->med, sizeof(e->med));
    e->has_data = FALSE;
  }
  blob_free(&e->raw);
}

/* the entries of a static picture class */
static void dc_add_auto_locked(DataCache *dc) {
  FORMATETC f;
  int kind = dc_static_kind(&dc->clsid);
  mem_zero(&f, sizeof(f));
  f.dwAspect = DVASPECT_CONTENT;
  f.lindex = -1;
  if (kind == 1) f.cfFormat = CF_DIB, f.tymed = TYMED_HGLOBAL;
  else if (kind == 2) f.cfFormat = CF_METAFILEPICT, f.tymed = TYMED_MFPICT;
  else if (kind == 3) f.cfFormat = CF_ENHMETAFILE, f.tymed = TYMED_ENHMF;
  else return;
  dc_new_entries_locked(dc, &f, 0, 1, FALSE);
}

/* the cache becomes the cache of another class: the entries of the old class's picture move to a connection id of their own */
static void dc_set_class_locked(DataCache *dc, const GUID *clsid) {
  DWORD moved = 0;
  if (IsEqualGUID(&dc->clsid, clsid)) return;
  for (ULONG i = 0; i < dc->n; i++) {
    if (dc->e[i].id != 1) continue;
    if (!moved) moved = dc->next_id++;
    dc->e[i].id = moved;
  }
  dc->clsid = *clsid;
  dc_add_auto_locked(dc);
}

static void dc_free_entries(DataCache *dc) {
  lock();
  for (ULONG i = 0; i < dc->n; i++) ce_clear(&dc->e[i]);
  mem_free(dc->e);
  dc->e = NULL;
  dc->n = dc->cap = 0;
  unlock();
}

typedef struct EntryInfo { FORMATETC fmt; DWORD advf, id, sink_conn; BOOL has_data, has_pres, derived, is_view, dirty; } EntryInfo;
static BOOL dc_info(DataCache *dc, ULONG i, EntryInfo *out) {
  lock();
  if (i >= dc->n) { unlock(); return FALSE; }
  out->fmt = dc->e[i].fmt;
  out->advf = dc->e[i].advf;
  out->id = dc->e[i].id;
  out->sink_conn = dc->e[i].sink_conn;
  out->has_data = dc->e[i].has_data;
  out->has_pres = dc->e[i].has_data || dc->e[i].raw.data != NULL;
  out->derived = dc->e[i].derived;
  out->is_view = dc->e[i].is_view;
  out->dirty = dc->e[i].dirty;
  unlock();
  return TRUE;
}

/* ---- notifications ---- */

static void dc_fire_view_change(DataCache *dc, DWORD aspect) {
  IAdviseSink *sink;
  DWORD mask;
  lock();
  sink = dc->view_sink;
  mask = dc->view_aspects;
  if (sink) IAdviseSink_AddRef(sink);      /* (a leaf call under the lock: the sink cannot go in between) */
  unlock();
  if (!sink) return;
  if (mask & aspect) IAdviseSink_OnViewChange(sink, aspect, -1);
  IAdviseSink_Release(sink);
}

/* a medium to keep: the caller's own (release) or a copy */
static HRESULT dc_own_medium(STGMEDIUM *src, BOOL release, STGMEDIUM *out) {
  HRESULT hr;
  if (src->tymed != TYMED_HGLOBAL && src->tymed != TYMED_GDI && src->tymed != TYMED_MFPICT && src->tymed != TYMED_ENHMF) return DV_E_TYMED;
  if (release && !src->pUnkForRelease) { *out = *src; return S_OK; }
  hr = medium_copy(src, 0, out);
  if (release) ReleaseStgMedium(src);
  return hr;
}

/* the data of the entry is replaced */
static BOOL dc_set_medium(DataCache *dc, const FORMATETC *id, STGMEDIUM *owned, BOOL mark_dirty) {
  CacheEntry *e;
  lock();
  e = dc_find_locked(dc, id);
  if (!e) { unlock(); return FALSE; }
  ce_clear(e);
  e->med = *owned;
  e->has_data = TRUE;
  e->shared_out = FALSE;
  if (mark_dirty) { e->dirty = TRUE; dc->dirty = TRUE; }
  if (id->cfFormat == CF_DIB || id->cfFormat == CF_BITMAP) {      /* the twin is made from the other format and is stale now */
    FORMATETC t = *id;
    CacheEntry *twin;
    t.cfFormat = id->cfFormat == CF_DIB ? CF_BITMAP : CF_DIB;
    twin = dc_find_locked(dc, &t);
    if (twin) ce_clear(twin);
  }
  unlock();
  return TRUE;
}

/* a view cache entry gets a format */
static void dc_materialize_locked(DataCache *dc, CacheEntry *e, int cf) {
  FORMATETC f;
  DWORD advf = e->advf, id = e->id;
  f = e->fmt;
  f.cfFormat = (CLIPFORMAT)cf;
  f.tymed = dc_tymed_of(cf);
  e->fmt.cfFormat = (CLIPFORMAT)cf;
  e->fmt.tymed = f.tymed;
  e->is_view = FALSE;
  if (cf == CF_DIB || cf == CF_BITMAP) {
    FORMATETC t = f;
    t.cfFormat = cf == CF_DIB ? CF_BITMAP : CF_DIB;
    t.tymed = dc_tymed_of(t.cfFormat);
    dc_add_locked(dc, &t, advf, id, TRUE, FALSE);      /* (e may move: it is not used again) */
  }
}

/* the bytes a stream read as presentation: global memory for a DIB and for the formats that are bytes anyway */
static void dc_load_pres(DataCache *dc, DWORD cf_raw, int cf, DWORD aspect, DWORD advf, const BYTE *data, SIZE_T n);

static void dc_ensure_loaded(DataCache *dc);

/* ---- IUnknown of the object and of its interfaces ---- */

static void dc_destroy(DataCache *dc);

static HRESULT WINAPI dc_inner_qi(IUnknown *me, REFIID riid, void **out) {
  DataCache *dc = DC_FROM(inner, me);
  void *r = NULL;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown)) r = &dc->inner;
  else if (IsEqualGUID(riid, &IID_IDataObject)) r = &dc->data;
  else if (IsEqualGUID(riid, &IID_IViewObject) || IsEqualGUID(riid, &IID_IViewObject2)) r = &dc->view;
  else if (IsEqualGUID(riid, &IID_IOleCache) || IsEqualGUID(riid, &IID_IOleCache2)) r = &dc->cache;
  else if (IsEqualGUID(riid, &IID_IOleCacheControl)) r = &dc->control;
  else if (IsEqualGUID(riid, &IID_IPersist) || IsEqualGUID(riid, &IID_IPersistStorage)) r = &dc->persist;
  else return E_NOINTERFACE;
  IUnknown_AddRef((IUnknown *)r);
  *out = r;
  return S_OK;
}
static ULONG WINAPI dc_inner_addref(IUnknown *me) { return (ULONG)InterlockedIncrement(&DC_FROM(inner, me)->refs); }
static ULONG WINAPI dc_inner_release(IUnknown *me) {
  DataCache *dc = DC_FROM(inner, me);
  LONG n = InterlockedDecrement(&dc->refs);
  if (n == 0) {
    dc->refs = 1000;          /* (the object is used while it goes) */
    dc_destroy(dc);
  }
  return (ULONG)n;
}
static const IUnknownVtbl g_dc_inner_vtbl = {dc_inner_qi, dc_inner_addref, dc_inner_release};

/* each interface hands IUnknown on to the controlling object */
#define DC_UNKNOWN_METHODS(prefix, field, Itype) \
  static HRESULT WINAPI prefix##_qi(Itype *me, REFIID riid, void **out) { return IUnknown_QueryInterface(dc_outer(DC_FROM(field, me)), riid, out); } \
  static ULONG WINAPI prefix##_addref(Itype *me) { return IUnknown_AddRef(dc_outer(DC_FROM(field, me))); } \
  static ULONG WINAPI prefix##_release(Itype *me) { return IUnknown_Release(dc_outer(DC_FROM(field, me))); }
DC_UNKNOWN_METHODS(dd, data, IDataObject)
DC_UNKNOWN_METHODS(dv, view, IViewObject2)
DC_UNKNOWN_METHODS(dcache, cache, IOleCache2)
DC_UNKNOWN_METHODS(dctl, control, IOleCacheControl)
DC_UNKNOWN_METHODS(dp, persist, IPersistStorage)
DC_UNKNOWN_METHODS(dsink, sink, IAdviseSink)

/* ---- the running data object ---- */

static void dc_advise_entries(DataCache *dc) {
  IDataObject *run;
  lock();
  run = dc->running;
  unlock();
  if (!run) return;
  for (ULONG i = 0;; i++) {
    EntryInfo in;
    DWORD conn = 0;
    FORMATETC f;
    if (!dc_info(dc, i, &in)) break;
    if (in.derived || in.sink_conn || in.fmt.cfFormat == 0) continue;
    f = in.fmt;
    if (SUCCEEDED(IDataObject_DAdvise(run, &f, in.advf, &dc->sink, &conn)) && conn) {
      CacheEntry *e;
      lock();
      e = dc_find_locked(dc, &in.fmt);
      if (e) e->sink_conn = conn;
      unlock();
      if (!e) IDataObject_DUnadvise(run, conn);
    }
  }
}

static void dc_stop(DataCache *dc) {
  IDataObject *run;
  DWORD *conns = NULL;
  ULONG n = 0;
  lock();
  run = dc->running;
  dc->running = NULL;
  if (dc->n) conns = mem_alloc((SIZE_T)dc->n * sizeof(DWORD));
  for (ULONG i = 0; i < dc->n; i++) if (dc->e[i].sink_conn) { if (conns) conns[n++] = dc->e[i].sink_conn; dc->e[i].sink_conn = 0; }
  unlock();
  if (run) {
    for (ULONG i = 0; i < n; i++) IDataObject_DUnadvise(run, conns[i]);
    IDataObject_Release(run);
  }
  mem_free(conns);
}

static HRESULT WINAPI dctl_onrun(IOleCacheControl *me, IDataObject *obj) {
  DataCache *dc = DC_FROM(control, me);
  IDataObject *old;
  if (!obj) return E_INVALIDARG;
  IDataObject_AddRef(obj);
  lock();
  old = dc->running;
  dc->running = obj;
  unlock();
  if (old) IDataObject_Release(old);
  dc_advise_entries(dc);
  return S_OK;
}
static HRESULT WINAPI dctl_onstop(IOleCacheControl *me) { dc_stop(DC_FROM(control, me)); return S_OK; }
static const IOleCacheControlVtbl g_dc_control_vtbl = {dctl_qi, dctl_addref, dctl_release, dctl_onrun, dctl_onstop};

/* ---- IOleCache / IOleCache2 ---- */

static BOOL dc_static_allows(DataCache *dc, const FORMATETC *f) {
  int kind = dc_static_kind(&dc->clsid);
  if (!kind) return TRUE;
  if (f->dwAspect == DVASPECT_ICON) return TRUE;
  if (f->dwAspect != DVASPECT_CONTENT) return FALSE;
  if (kind == 1) return f->cfFormat == CF_DIB || f->cfFormat == CF_BITMAP;
  if (kind == 2) return f->cfFormat == CF_METAFILEPICT;
  return f->cfFormat == CF_ENHMETAFILE;
}

static HRESULT WINAPI dcache_cache(IOleCache2 *me, FORMATETC *pf, DWORD advf, DWORD *conn) {
  DataCache *dc = DC_FROM(cache, me);
  FORMATETC f, advise_fmt;
  HRESULT result = S_OK;
  BOOL is_view = FALSE, added;
  CacheEntry *e;
  DWORD id;
  if (!pf || !conn) return E_INVALIDARG;
  *conn = 0;
  f = *pf;
  f.ptd = NULL;
  f.lindex = -1;
  if (!f.tymed) return DV_E_TYMED;
  if (f.dwAspect == DVASPECT_ICON) {
    if (f.cfFormat != 0 && f.cfFormat != CF_METAFILEPICT) return DV_E_FORMATETC;
    is_view = pf->cfFormat == 0;
    dc_normalize(&f);
    f.cfFormat = CF_METAFILEPICT;
    f.tymed = TYMED_MFPICT;
  } else if (f.cfFormat == 0) {
    is_view = TRUE;
  } else {
    DWORD allowed = dc_tymed_of(f.cfFormat);
    if (f.tymed != allowed) {
      if (f.tymed == TYMED_HGLOBAL) result = CACHE_S_FORMATETC_NOTSUPPORTED;
      else return DV_E_TYMED;
    }
  }
  lock();
  if (!dc_static_allows(dc, &f)) { unlock(); return DV_E_FORMATETC; }
  e = dc_find_locked(dc, &f);
  if (e) { *conn = e->id; unlock(); return CACHE_S_SAMECACHE; }
  id = dc->next_id++;
  dc_new_entries_locked(dc, &f, advf, id, is_view);
  dc->dirty = TRUE;
  added = dc_find_locked(dc, &f) != NULL;
  advise_fmt = f;
  unlock();
  if (!added) return E_OUTOFMEMORY;
  *conn = id;
  /* the running data object is asked to tell about changes */
  {
    IDataObject *run;
    lock();
    run = dc->running;
    unlock();
    if (run && f.cfFormat) dc_advise_entries(dc);
  }
  (void)advise_fmt;
  return result;
}

static HRESULT WINAPI dcache_uncache(IOleCache2 *me, DWORD conn) {
  DataCache *dc = DC_FROM(cache, me);
  IDataObject *run = NULL;
  DWORD conns[8];
  ULONG nc = 0;
  BOOL found = FALSE;
  lock();
  for (ULONG i = 0; i < dc->n;) {
    if (dc->e[i].id != conn) { i++; continue; }
    found = TRUE;
    if (dc->e[i].sink_conn && nc < 8) conns[nc++] = dc->e[i].sink_conn;
    ce_clear(&dc->e[i]);
    for (ULONG j = i + 1; j < dc->n; j++) dc->e[j - 1] = dc->e[j];
    dc->n--;
  }
  if (found) { dc->dirty = TRUE; run = dc->running; if (run) IDataObject_AddRef(run); }
  unlock();
  if (run) {
    for (ULONG i = 0; i < nc; i++) IDataObject_DUnadvise(run, conns[i]);
    IDataObject_Release(run);
  }
  return found ? S_OK : OLE_E_NOCONNECTION;
}

static HRESULT WINAPI dcache_enumcache(IOleCache2 *me, IEnumSTATDATA **out) {
  DataCache *dc = DC_FROM(cache, me);
  STATDATA *items = NULL;
  ULONG n;
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  lock();
  n = dc->n;
  if (n) items = mem_alloc((SIZE_T)n * sizeof(STATDATA));
  if (n && !items) { unlock(); return E_OUTOFMEMORY; }
  for (ULONG i = 0; i < n; i++) {
    items[i].formatetc = dc->e[i].fmt;
    items[i].advf = dc->e[i].advf;
    items[i].dwConnection = dc->e[i].id;
  }
  unlock();
  /* by connection id; entries of one id stay in the order they were made */
  for (ULONG i = 1; i < n; i++) {
    STATDATA key = items[i];
    ULONG j = i;
    while (j > 0 && items[j - 1].dwConnection > key.dwConnection) { items[j] = items[j - 1]; j--; }
    items[j] = key;
  }
  hr = enum_statdata_create(items, n, out);
  mem_free(items);
  return hr;
}

static HRESULT dc_update(DataCache *dc, IDataObject *obj, DWORD flags);

static HRESULT WINAPI dcache_initcache(IOleCache2 *me, IDataObject *obj) {
  if (!obj) return E_INVALIDARG;
  return dc_update(DC_FROM(cache, me), obj, UPDFCACHE_IFBLANK | UPDFCACHE_NORMALCACHE | UPDFCACHE_NODATACACHE);
}

static HRESULT WINAPI dcache_setdata(IOleCache2 *me, FORMATETC *pf, STGMEDIUM *med, BOOL release) {
  DataCache *dc = DC_FROM(cache, me);
  FORMATETC f;
  STGMEDIUM owned;
  CacheEntry *e;
  DWORD want = 0;
  HRESULT hr;
  if (!pf || !med) return E_INVALIDARG;
  f = *pf;
  dc_normalize(&f);
  dc_ensure_loaded(dc);
  lock();
  e = dc_find_locked(dc, &f);
  if (!e) {
    /* a view cache of the aspect takes the format of the data */
    FORMATETC v = f;
    v.cfFormat = 0;
    e = dc_find_locked(dc, &v);
    if (e && f.cfFormat) { dc_materialize_locked(dc, e, f.cfFormat); e = dc_find_locked(dc, &f); }
    else e = NULL;
  }
  if (e) want = e->fmt.tymed;
  unlock();
  if (!e) return OLE_E_BLANK;
  if (med->tymed != want) return DV_E_TYMED;
  hr = dc_own_medium(med, release, &owned);
  if (FAILED(hr)) return hr;
  if (!dc_set_medium(dc, &f, &owned, TRUE)) { ReleaseStgMedium(&owned); return OLE_E_BLANK; }
  dc_fire_view_change(dc, f.dwAspect);
  return S_OK;
}

/* UpdateCache: the entries selected by the flags are asked of the data object. A view cache takes the first format the data object has. */
static HRESULT dc_update(DataCache *dc, IDataObject *obj, DWORD flags) {
  static const int views[4] = {CF_ENHMETAFILE, CF_METAFILEPICT, CF_DIB, CF_BITMAP};
  ULONG attempted = 0, updated = 0;
  BOOL everything = (flags & 0x7FFFFFFFu) == 0x7FFFFFFFu;
  dc_ensure_loaded(dc);
  for (ULONG i = 0;; i++) {
    EntryInfo in;
    BOOL want;
    if (!dc_info(dc, i, &in)) break;
    if (in.derived) continue;
    if (in.advf & ADVF_NODATA) want = (flags & UPDFCACHE_NODATACACHE) != 0;
    else want = (flags & (UPDFCACHE_NORMALCACHE | UPDFCACHE_IFBLANK)) != 0;
    if (want && !everything && (flags & (UPDFCACHE_IFBLANK | UPDFCACHE_ONLYIFBLANK)) && in.has_pres) want = FALSE;
    if (!want) continue;
    attempted++;
    for (int k = 0; in.fmt.cfFormat == 0 && k < 4; k++) {
      FORMATETC f = in.fmt;
      STGMEDIUM m, owned;
      if (in.fmt.cfFormat == 0) { f.cfFormat = (CLIPFORMAT)views[k]; f.tymed = dc_tymed_of(views[k]); }
      mem_zero(&m, sizeof(m));
      if (IDataObject_GetData(obj, &f, &m) != S_OK) continue;
      if (m.tymed != f.tymed || FAILED(dc_own_medium(&m, TRUE, &owned))) { ReleaseStgMedium(&m); continue; }
      if (in.fmt.cfFormat == 0) {
        CacheEntry *e;
        lock();
        e = dc_find_locked(dc, &in.fmt);
        if (e) dc_materialize_locked(dc, e, views[k]);
        unlock();
      }
      if (dc_set_medium(dc, &f, &owned, TRUE)) { updated++; dc_fire_view_change(dc, f.dwAspect); }
      else ReleaseStgMedium(&owned);
      break;
    }
    if (in.fmt.cfFormat != 0) {
      FORMATETC f = in.fmt;
      STGMEDIUM m, owned;
      mem_zero(&m, sizeof(m));
      if (IDataObject_GetData(obj, &f, &m) == S_OK) {
        if (m.tymed == f.tymed && SUCCEEDED(dc_own_medium(&m, TRUE, &owned))) {
          if (dc_set_medium(dc, &f, &owned, TRUE)) { updated++; dc_fire_view_change(dc, f.dwAspect); }
          else ReleaseStgMedium(&owned);
        } else {
          ReleaseStgMedium(&m);
        }
      }
    }
  }
  return (attempted && !updated) ? CACHE_E_NOCACHE_UPDATED : S_OK;
}

static HRESULT WINAPI dcache_updatecache(IOleCache2 *me, IDataObject *obj, DWORD flags, void *reserved) {
  (void)reserved;
  if (!obj) return E_INVALIDARG;
  return dc_update(DC_FROM(cache, me), obj, flags);
}

static HRESULT dc_save(DataCache *dc, IStorage *stg);

static HRESULT WINAPI dcache_discardcache(IOleCache2 *me, DWORD options) {
  DataCache *dc = DC_FROM(cache, me);
  IStorage *stg = NULL;
  if (options == DISCARDCACHE_SAVEIFDIRTY) {
    lock();
    if (dc->dirty && dc->stg) { stg = dc->stg; IStorage_AddRef(stg); }
    unlock();
    if (stg) { dc_save(dc, stg); IStorage_Release(stg); }
  }
  lock();
  for (ULONG i = 0; i < dc->n; i++) ce_clear(&dc->e[i]);
  dc->loaded = FALSE;
  unlock();
  return S_OK;
}
static const IOleCache2Vtbl g_dc_cache_vtbl = {dcache_qi, dcache_addref, dcache_release, dcache_cache, dcache_uncache, dcache_enumcache, dcache_initcache, dcache_setdata,
                                              dcache_updatecache, dcache_discardcache};

/* ---- IDataObject ---- */

static HRESULT WINAPI dd_getdata(IDataObject *me, FORMATETC *pf, STGMEDIUM *out) {
  DataCache *dc = DC_FROM(data, me);
  FORMATETC f;
  CacheEntry *e;
  HRESULT hr = S_OK;
  if (!pf || !out) return E_INVALIDARG;
  mem_zero(out, sizeof(*out));
  f = *pf;
  dc_normalize(&f);
  dc_ensure_loaded(dc);
  lock();
  e = dc_find_locked(dc, &f);
  if (!e) hr = f.cfFormat == 0 ? DV_E_CLIPFORMAT : OLE_E_BLANK;
  else if (!e->has_data) hr = OLE_E_BLANK;
  else if (!(f.tymed & e->med.tymed)) hr = DV_E_TYMED;
  else if (e->med.tymed == TYMED_HGLOBAL) {
    out->tymed = TYMED_HGLOBAL;
    out->hGlobal = OleDuplicateData(e->med.hGlobal, CF_DIB, 0);
    if (!out->hGlobal) { out->tymed = TYMED_NULL; hr = E_OUTOFMEMORY; }
  } else {
    /* the handle is the cache's own: the medium holds a reference to the cache that keeps it alive, and the entry never frees the handle while it is out */
    *out = e->med;
    out->pUnkForRelease = &dc->inner;
    IUnknown_AddRef(&dc->inner);
    e->shared_out = TRUE;
  }
  unlock();
  return hr;
}
static HRESULT WINAPI dd_getdatahere(IDataObject *me, FORMATETC *pf, STGMEDIUM *med) {
  DataCache *dc = DC_FROM(data, me);
  FORMATETC f;
  CacheEntry *e;
  HRESULT hr = OLE_E_BLANK;
  if (!pf || !med) return E_INVALIDARG;
  f = *pf;
  dc_normalize(&f);
  dc_ensure_loaded(dc);
  lock();
  e = dc_find_locked(dc, &f);
  if (e && e->has_data && e->med.tymed == TYMED_HGLOBAL && med->tymed == TYMED_HGLOBAL) {
    Blob b = {NULL, 0};
    if (SUCCEEDED(medium_to_blob(&e->med, &b))) { hr = blob_to_here(&b, med); blob_free(&b); }
    else hr = E_OUTOFMEMORY;
  } else if (e && e->has_data) {
    hr = DV_E_TYMED;
  }
  unlock();
  return hr;
}
static HRESULT WINAPI dd_querygetdata(IDataObject *me, FORMATETC *pf) {
  DataCache *dc = DC_FROM(data, me);
  FORMATETC f;
  CacheEntry *e;
  HRESULT hr;
  if (!pf) return E_INVALIDARG;
  f = *pf;
  dc_normalize(&f);
  dc_ensure_loaded(dc);
  lock();
  e = dc_find_locked(dc, &f);
  if (!e) hr = f.cfFormat ? DV_E_FORMATETC : DV_E_CLIPFORMAT;
  else if (!e->has_data) hr = OLE_E_BLANK;
  else hr = (f.tymed & e->med.tymed) ? S_OK : DV_E_TYMED;
  unlock();
  return hr;
}
static HRESULT WINAPI dd_canonical(IDataObject *me, FORMATETC *in, FORMATETC *out) { (void)me; (void)in; if (out) out->ptd = NULL; return E_NOTIMPL; }
static HRESULT WINAPI dd_setdata(IDataObject *me, FORMATETC *pf, STGMEDIUM *med, BOOL release) { return IOleCache2_SetData(&DC_FROM(data, me)->cache, pf, med, release); }
static HRESULT WINAPI dd_enumformatetc(IDataObject *me, DWORD dir, IEnumFORMATETC **out) { (void)me; (void)dir; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT WINAPI dd_dadvise(IDataObject *me, FORMATETC *f, DWORD advf, IAdviseSink *sink, DWORD *conn) { (void)me; (void)f; (void)advf; (void)sink; if (conn) *conn = 0; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT WINAPI dd_dunadvise(IDataObject *me, DWORD conn) { (void)me; (void)conn; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT WINAPI dd_enumdadvise(IDataObject *me, IEnumSTATDATA **out) { (void)me; if (out) *out = NULL; return OLE_E_ADVISENOTSUPPORTED; }
static const IDataObjectVtbl g_dc_data_vtbl = {dd_qi, dd_addref, dd_release, dd_getdata, dd_getdatahere, dd_querygetdata, dd_canonical, dd_setdata, dd_enumformatetc, dd_dadvise, dd_dunadvise,
                                              dd_enumdadvise};

/* ---- IAdviseSink (what the running data object tells the cache) ---- */

static void WINAPI dsink_ondatachange(IAdviseSink *me, FORMATETC *pf, STGMEDIUM *med) {
  DataCache *dc = DC_FROM(sink, me);
  FORMATETC f;
  STGMEDIUM owned;
  if (!pf || !med) return;
  f = *pf;
  dc_normalize(&f);
  if (med->tymed == TYMED_NULL) return;
  if (SUCCEEDED(dc_own_medium(med, FALSE, &owned))) {
    if (dc_set_medium(dc, &f, &owned, TRUE)) dc_fire_view_change(dc, f.dwAspect);
    else ReleaseStgMedium(&owned);
  }
}
static void WINAPI dsink_onviewchange(IAdviseSink *me, DWORD aspect, LONG lindex) { (void)lindex; dc_fire_view_change(DC_FROM(sink, me), aspect); }
static void WINAPI dsink_onrename(IAdviseSink *me, IMoniker *mk) { (void)me; (void)mk; }
static void WINAPI dsink_onsave(IAdviseSink *me) { (void)me; }
static void WINAPI dsink_onclose(IAdviseSink *me) { (void)me; }
static const IAdviseSinkVtbl g_dc_sink_vtbl = {dsink_qi, dsink_addref, dsink_release, dsink_ondatachange, dsink_onviewchange, dsink_onrename, dsink_onsave, dsink_onclose};

/* ---- IViewObject2 ---- */

typedef BOOL (WINAPI *DrawContinueFn)(ULONG_PTR);

/* has the aspect something to show */
static BOOL dc_has_aspect(DataCache *dc, DWORD aspect) {
  BOOL r = FALSE;
  lock();
  for (ULONG i = 0; i < dc->n; i++) if (dc->e[i].fmt.dwAspect == aspect && (dc->e[i].has_data || dc->e[i].raw.data)) r = TRUE;
  unlock();
  return r;
}

static HRESULT WINAPI dv_draw(IViewObject2 *me, DWORD aspect, LONG lindex, void *pv, DVTARGETDEVICE *ptd, HDC target, HDC hdc, LPCRECTL bounds, LPCRECTL wbounds,
                              DrawContinueFn cont, ULONG_PTR cdata) {
  DataCache *dc = DC_FROM(view, me);
  (void)lindex; (void)pv; (void)ptd; (void)target; (void)hdc; (void)bounds; (void)wbounds;
  dc_ensure_loaded(dc);
  if (!dc_has_aspect(dc, aspect)) return OLE_E_BLANK;
  /* (the picture itself needs GDI: the draw is only the protocol of the continue callback) */
  if (cont && !cont(cdata)) return E_ABORT;
  return S_OK;
}
static HRESULT WINAPI dv_getcolorset(IViewObject2 *me, DWORD aspect, LONG lindex, void *pv, DVTARGETDEVICE *ptd, HDC target, LOGPALETTE **set) {
  (void)me; (void)aspect; (void)lindex; (void)pv; (void)ptd; (void)target;
  if (set) *set = NULL;
  return E_NOTIMPL;
}
static HRESULT WINAPI dv_freeze(IViewObject2 *me, DWORD aspect, LONG lindex, void *pv, DWORD *freeze) {
  DataCache *dc = DC_FROM(view, me);
  (void)lindex; (void)pv;
  if (!freeze) return E_INVALIDARG;
  *freeze = 0;
  dc_ensure_loaded(dc);
  if (!dc_has_aspect(dc, aspect)) return OLE_E_BLANK;
  lock();
  *freeze = ++dc->freeze_next;
  unlock();
  return S_OK;
}
static HRESULT WINAPI dv_unfreeze(IViewObject2 *me, DWORD freeze) { (void)me; (void)freeze; return S_OK; }
static HRESULT WINAPI dv_setadvise(IViewObject2 *me, DWORD aspects, DWORD advf, IAdviseSink *sink) {
  DataCache *dc = DC_FROM(view, me);
  IAdviseSink *old;
  if (sink) IAdviseSink_AddRef(sink);
  lock();
  old = dc->view_sink;
  dc->view_sink = sink;
  dc->view_aspects = sink ? aspects : 0;
  dc->view_advf = advf;
  unlock();
  if (old) IAdviseSink_Release(old);
  if (sink && (advf & ADVF_PRIMEFIRST)) dc_fire_view_change(dc, aspects);
  return S_OK;
}
static HRESULT WINAPI dv_getadvise(IViewObject2 *me, DWORD *aspects, DWORD *advf, IAdviseSink **sink) {
  DataCache *dc = DC_FROM(view, me);
  lock();
  if (aspects) *aspects = dc->view_aspects;
  if (advf) *advf = dc->view_advf;
  if (sink) { *sink = dc->view_sink; if (*sink) IAdviseSink_AddRef(*sink); }
  unlock();
  return S_OK;
}

static LONG le32_at(const BYTE *p) { return (LONG)((DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24); }

static HRESULT WINAPI dv_getextent(IViewObject2 *me, DWORD aspect, LONG lindex, DVTARGETDEVICE *ptd, LPSIZEL size) {
  DataCache *dc = DC_FROM(view, me);
  HRESULT hr = OLE_E_BLANK;
  (void)lindex; (void)ptd;
  if (!size) return E_INVALIDARG;
  dc_ensure_loaded(dc);
  lock();
  for (ULONG i = 0; i < dc->n && hr == OLE_E_BLANK; i++) {
    CacheEntry *e = &dc->e[i];
    if (e->fmt.dwAspect != aspect || !e->has_data) continue;
    if (e->fmt.cfFormat == CF_DIB && e->med.tymed == TYMED_HGLOBAL) {
      /* size in 0.01 mm from the pixels and the pixels per meter of the header (96 dpi when it has none) */
      const BYTE *p = GlobalLock(e->med.hGlobal);
      if (p && GlobalSize(e->med.hGlobal) >= 40) {
        LONGLONG w = le32_at(p + 4), h = le32_at(p + 8), xp = le32_at(p + 24), yp = le32_at(p + 28);
        if (h < 0) h = -h;
        if (w < 0) w = -w;
        if (xp <= 0) xp = 3779;
        if (yp <= 0) yp = 3779;
        size->cx = (LONG)(w * 100000 / xp);
        size->cy = (LONG)(h * 100000 / yp);
        hr = S_OK;
      }
      if (p) GlobalUnlock(e->med.hGlobal);
    } else if (e->fmt.cfFormat == CF_METAFILEPICT && e->med.tymed == TYMED_MFPICT) {
      OleMfPict *mf = GlobalLock(e->med.hMetaFilePict);
      if (mf && mf->xExt > 0 && mf->yExt > 0) { size->cx = mf->xExt; size->cy = mf->yExt; hr = S_OK; }
      if (mf) GlobalUnlock(e->med.hMetaFilePict);
    }
  }
  unlock();
  return hr;
}
static const IViewObject2Vtbl g_dc_view_vtbl = {dv_qi, dv_addref, dv_release, dv_draw, dv_getcolorset, dv_freeze, dv_unfreeze, dv_setadvise, dv_getadvise, dv_getextent};

/* ---- IPersistStorage ---- */

static const WCHAR kOlePresFmt[] = {2, 'O', 'l', 'e', 'P', 'r', 'e', 's', 0};
static void olepres_name(WCHAR *out, int num) {
  int i = 0;
  while (kOlePresFmt[i]) { out[i] = kOlePresFmt[i]; i++; }
  out[i++] = (WCHAR)('0' + num / 100 % 10);
  out[i++] = (WCHAR)('0' + num / 10 % 10);
  out[i++] = (WCHAR)('0' + num % 10);
  out[i] = 0;
}

static CLSID storage_class(IStorage *stg) {
  STATSTG st;
  CLSID c;
  mem_zero(&c, sizeof(c));
  if (SUCCEEDED(IStorage_Stat(stg, &st, STATFLAG_NONAME))) c = st.clsid;
  return c;
}

static HRESULT WINAPI dp_getclassid(IPersistStorage *me, CLSID *out) {
  DataCache *dc = DC_FROM(persist, me);
  if (!out) return E_INVALIDARG;
  lock();
  *out = dc->clsid;
  unlock();
  return S_OK;
}
static HRESULT WINAPI dp_isdirty(IPersistStorage *me) {
  DataCache *dc = DC_FROM(persist, me);
  BOOL d;
  lock();
  d = dc->dirty;
  unlock();
  return d ? S_OK : S_FALSE;
}
static HRESULT WINAPI dp_initnew(IPersistStorage *me, IStorage *stg) {
  DataCache *dc = DC_FROM(persist, me);
  CLSID c;
  if (!stg) return E_POINTER;
  lock();
  if (dc->stg) { unlock(); return CO_E_ALREADYINITIALIZED; }
  unlock();
  c = storage_class(stg);
  IStorage_AddRef(stg);
  lock();
  dc_set_class_locked(dc, &c);
  dc->stg = stg;
  dc->loaded = TRUE;
  dc->dirty = TRUE;
  unlock();
  return S_OK;
}

static void dc_load_streams(DataCache *dc, IStorage *stg);

static HRESULT WINAPI dp_load(IPersistStorage *me, IStorage *stg) {
  DataCache *dc = DC_FROM(persist, me);
  CLSID c;
  if (!stg) return E_INVALIDARG;
  lock();
  if (dc->stg) { unlock(); return CO_E_ALREADYINITIALIZED; }
  unlock();
  dc_stop(dc);
  dc_free_entries(dc);
  c = storage_class(stg);
  IStorage_AddRef(stg);
  lock();
  mem_zero(&dc->clsid, sizeof(dc->clsid));
  dc_set_class_locked(dc, &c);
  dc->stg = stg;
  dc->loaded = TRUE;
  unlock();
  dc_load_streams(dc, stg);
  lock();
  dc->dirty = FALSE;
  unlock();
  return S_OK;
}
static HRESULT WINAPI dp_save(IPersistStorage *me, IStorage *stg, BOOL same_as_load) {
  DataCache *dc = DC_FROM(persist, me);
  HRESULT hr;
  if (!stg) return E_INVALIDARG;
  hr = dc_save(dc, stg);
  if (SUCCEEDED(hr) && same_as_load) {
    lock();
    dc->dirty = FALSE;
    for (ULONG i = 0; i < dc->n; i++) dc->e[i].dirty = FALSE;
    unlock();
  }
  return hr;
}
static HRESULT WINAPI dp_savecompleted(IPersistStorage *me, IStorage *stg) {
  DataCache *dc = DC_FROM(persist, me);
  IStorage *old = NULL;
  if (stg) {
    IStorage_AddRef(stg);
    lock();
    old = dc->stg;
    dc->stg = stg;
    unlock();
    if (old) IStorage_Release(old);
  }
  return S_OK;
}
static HRESULT WINAPI dp_handsoff(IPersistStorage *me) {
  DataCache *dc = DC_FROM(persist, me);
  IStorage *old;
  dc_ensure_loaded(dc);
  lock();
  old = dc->stg;
  dc->stg = NULL;
  unlock();
  if (old) IStorage_Release(old);
  return S_OK;
}
static const IPersistStorageVtbl g_dc_persist_vtbl = {dp_qi, dp_addref, dp_release, dp_getclassid, dp_isdirty, dp_initnew, dp_load, dp_save, dp_savecompleted, dp_handsoff};

/* ---- the presentation streams ---- */

static BOOL rd_u32(const Blob *b, SIZE_T *pos, DWORD *v) {
  if (*pos > b->size || b->size - *pos < 4) return FALSE;
  *v = (DWORD)le32_at(b->data + *pos);
  *pos += 4;
  return TRUE;
}

/* a metafile (placeable or not) as a medium, when GDI can make it */
static BOOL mf_medium(const BYTE *bits, SIZE_T n, LONG xext, LONG yext, STGMEDIUM *out) {
  MetaBitsSetFn set = (MetaBitsSetFn)dyn_gdi("SetMetaFileBitsEx");
  HANDLE mf;
  HGLOBAL h;
  OleMfPict *p;
  if (!set || !n || n > OLE_MAX_BLOB) return FALSE;
  mf = set((UINT)n, bits);
  if (!mf) return FALSE;
  h = GlobalAlloc(GMEM_MOVEABLE, sizeof(OleMfPict));
  p = h ? GlobalLock(h) : NULL;
  if (!p) { if (h) GlobalFree(h); gdi_delete("DeleteMetaFile", mf); return FALSE; }
  p->mm = 8 /* MM_ANISOTROPIC */;
  p->xExt = xext;
  p->yExt = yext;
  p->hMF = mf;
  GlobalUnlock(h);
  mem_zero(out, sizeof(*out));
  out->tymed = TYMED_MFPICT;
  out->hMetaFilePict = h;
  return TRUE;
}
static BOOL emf_medium(const BYTE *bits, SIZE_T n, STGMEDIUM *out) {
  MetaBitsSetFn set = (MetaBitsSetFn)dyn_gdi("SetEnhMetaFileBits");
  HANDLE h;
  if (!set || !n || n > OLE_MAX_BLOB) return FALSE;
  h = set((UINT)n, bits);
  if (!h) return FALSE;
  mem_zero(out, sizeof(*out));
  out->tymed = TYMED_ENHMF;
  out->hEnhMetaFile = h;
  return TRUE;
}

/* The data of one presentation goes into the entry for it (made when there is none). cf: 0 view cache, otherwise the clipboard format; -2 unknown format (ignored). */
static void dc_load_pres(DataCache *dc, DWORD cf_raw, int cf, DWORD aspect, DWORD advf, const BYTE *data, SIZE_T n) {
  FORMATETC f;
  STGMEDIUM m;
  BOOL have_medium = FALSE, view = cf == 0;
  CacheEntry *e;
  (void)cf_raw;
  if (cf == -2) return;
  mem_zero(&f, sizeof(f));
  f.dwAspect = aspect;
  f.lindex = -1;
  if (view && aspect == DVASPECT_ICON) cf = CF_METAFILEPICT;
  f.cfFormat = (CLIPFORMAT)(view && aspect != DVASPECT_ICON ? 0 : cf);
  f.tymed = dc_tymed_of(f.cfFormat) ? dc_tymed_of(f.cfFormat) : TYMED_HGLOBAL;
  mem_zero(&m, sizeof(m));
  if (n && !view) {
    if (f.cfFormat == CF_METAFILEPICT) have_medium = mf_medium(data, n, 0, 0, &m);
    else if (f.cfFormat == CF_ENHMETAFILE) have_medium = emf_medium(data, n, &m);
    else if (f.tymed == TYMED_HGLOBAL) {
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
      void *p = h ? GlobalLock(h) : NULL;
      if (p) { mem_copy(p, data, n); GlobalUnlock(h); m.tymed = TYMED_HGLOBAL; m.hGlobal = h; have_medium = TRUE; }
      else if (h) GlobalFree(h);
    }
  }
  lock();
  e = dc_find_locked(dc, &f);
  if (!e) {
    DWORD id = dc->next_id++;
    dc_new_entries_locked(dc, &f, advf, id, view);
    e = dc_find_locked(dc, &f);
  }
  if (e) {
    e->advf = advf;
    e->dirty = FALSE;
    ce_clear(e);
    if (have_medium) { e->med = m; e->has_data = TRUE; have_medium = FALSE; }
    else if (n && !view) blob_set(&e->raw, data, n);
  }
  unlock();
  if (have_medium) ReleaseStgMedium(&m);
}

static HRESULT open_named(IStorage *stg, const WCHAR *name, IStream **out) {
  return IStorage_OpenStream(stg, name, NULL, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, out);
}

/* the picture of a static class from its "CONTENTS" stream */
static void dc_load_contents(DataCache *dc, IStorage *stg) {
  IStream *s = NULL;
  Blob b = {NULL, 0};
  int kind = dc_static_kind(&dc->clsid);
  FORMATETC f;
  if (FAILED(open_named(stg, W('C', 'O', 'N', 'T', 'E', 'N', 'T', 'S'), &s)) && FAILED(open_named(stg, W('C', 'o', 'n', 't', 'e', 'n', 't', 's'), &s))) return;
  if (SUCCEEDED(stream_to_blob(s, &b)) && b.size) {
    mem_zero(&f, sizeof(f));
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = -1;
    if (kind == 1) {
      /* a bitmap file: the DIB follows the 14 byte file header */
      if (b.size > 14 && b.data[0] == 'B' && b.data[1] == 'M') {
        SIZE_T n = b.size - 14;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
        BYTE *p = h ? GlobalLock(h) : NULL;
        if (p) {
          mem_copy(p, b.data + 14, n);
          if (n >= 40) {      /* no resolution in the file: 96 dpi */
            for (int off = 24; off <= 28; off += 4) if (!le32_at(p + off)) { p[off] = 0xC3; p[off + 1] = 0x0E; p[off + 2] = 0; p[off + 3] = 0; }
          }
          GlobalUnlock(h);
          dc_load_pres(dc, 0, CF_DIB, DVASPECT_CONTENT, 0, NULL, 0);
          f.cfFormat = CF_DIB; f.tymed = TYMED_HGLOBAL;
          {
            STGMEDIUM m;
            mem_zero(&m, sizeof(m));
            m.tymed = TYMED_HGLOBAL;
            m.hGlobal = h;
            if (!dc_set_medium(dc, &f, &m, FALSE)) GlobalFree(h);
          }
        } else if (h) {
          GlobalFree(h);
        }
      }
    } else if (kind == 2) {
      SIZE_T skip = (b.size > 22 && le32_at(b.data) == (LONG)0x9AC6CDD7) ? 22 : 0;
      dc_load_pres(dc, 0, CF_METAFILEPICT, DVASPECT_CONTENT, 0, b.data + skip, b.size - skip);
    } else if (kind == 3) {
      DWORD hdr = b.size > 4 ? (DWORD)le32_at(b.data) : 0;
      if (hdr <= b.size - 4 && b.size > 4) dc_load_pres(dc, 0, CF_ENHMETAFILE, DVASPECT_CONTENT, 0, b.data + 4 + hdr, b.size - 4 - hdr);
    }
  }
  blob_free(&b);
  IStream_Release(s);
}

static void dc_load_streams(DataCache *dc, IStorage *stg) {
  int kind = dc_static_kind(&dc->clsid);
  if (kind) dc_load_contents(dc, stg);
  for (int num = 0; num < 1000; num++) {
    WCHAR name[16];
    IStream *s = NULL;
    Blob b = {NULL, 0};
    SIZE_T pos = 0;
    DWORD len, v, tdsize, aspect, lindex, advf, size = 0;
    int cf = -2;
    olepres_name(name, num);
    if (FAILED(open_named(stg, name, &s))) break;
    if (FAILED(stream_to_blob(s, &b))) { IStream_Release(s); break; }
    IStream_Release(s);
    if (!rd_u32(&b, &pos, &len)) { blob_free(&b); continue; }
    if (len == 0) cf = 0;
    else if (len == 0xFFFFFFFFu) { if (rd_u32(&b, &pos, &v)) cf = (int)v; }
    else if (len <= b.size - pos) pos += len;           /* a named format: not known without the window system */
    if (cf != -2 && rd_u32(&b, &pos, &tdsize) && tdsize >= 4 && tdsize - 4 <= b.size - pos) {
      pos += tdsize - 4;
      if (rd_u32(&b, &pos, &aspect) && rd_u32(&b, &pos, &lindex) && rd_u32(&b, &pos, &advf)) {
        DWORD ex, ey;
        BOOL ok = TRUE;
        if (cf != 0) ok = rd_u32(&b, &pos, &v) && rd_u32(&b, &pos, &ex) && rd_u32(&b, &pos, &ey) && rd_u32(&b, &pos, &size) && size <= b.size - pos;
        if (ok && (!kind || aspect == DVASPECT_ICON)) dc_load_pres(dc, 0, cf, aspect, advf, b.data + pos, cf ? size : 0);
      }
    }
    blob_free(&b);
  }
}

static void dc_ensure_loaded(DataCache *dc) {
  IStorage *stg = NULL;
  lock();
  if (!dc->loaded && dc->stg) { dc->loaded = TRUE; stg = dc->stg; IStorage_AddRef(stg); }
  unlock();
  if (stg) { dc_load_streams(dc, stg); IStorage_Release(stg); }
}

/* ---- saving ---- */

typedef struct SaveItem { FORMATETC fmt; DWORD advf, id; BOOL is_view, has, dirty; Blob bytes; } SaveItem;

static void put_u32(BYTE *buf, SIZE_T *pos, DWORD v) { buf[*pos] = (BYTE)v; buf[*pos + 1] = (BYTE)(v >> 8); buf[*pos + 2] = (BYTE)(v >> 16); buf[*pos + 3] = (BYTE)(v >> 24); *pos += 4; }

static HRESULT put_stream(IStorage *stg, const WCHAR *name, const BYTE *data, SIZE_T n) {
  IStream *s = NULL;
  HRESULT hr;
  if (n > 0xFFFFFFFFu) return E_OUTOFMEMORY;
  hr = IStorage_CreateStream(stg, name, STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE, 0, 0, &s);
  if (FAILED(hr)) return hr;
  hr = n ? stream_write_all(s, data, (ULONG)n) : S_OK;
  IStream_Release(s);
  return hr;
}

/* the bytes of the data of an entry as a presentation stream holds them */
static void save_bytes(const CacheEntry *e, Blob *out) {
  if (e->has_data) {
    if (e->med.tymed == TYMED_HGLOBAL) { medium_to_blob(&e->med, out); return; }
    if (e->med.tymed == TYMED_MFPICT) {
      OleMfPict *p = GlobalLock(e->med.hMetaFilePict);
      MetaBitsGetFn get = (MetaBitsGetFn)dyn_gdi("GetMetaFileBitsEx");
      if (p && get) {
        UINT n = get(p->hMF, 0, NULL);
        BYTE *buf = n && n <= OLE_MAX_BLOB ? mem_alloc(n) : NULL;
        if (buf && get(p->hMF, n, buf) == n) { mem_free(out->data); out->data = buf; out->size = n; buf = NULL; }
        mem_free(buf);
      }
      if (p) GlobalUnlock(e->med.hMetaFilePict);
      if (out->data) return;
    }
  }
  if (e->raw.data) blob_set(out, e->raw.data, e->raw.size);
}

static HRESULT dc_save(DataCache *dc, IStorage *stg) {
  SaveItem *items = NULL;
  ULONG n = 0;
  int kind = dc_static_kind(&dc->clsid), num = 0;
  HRESULT hr = S_OK;
  dc_ensure_loaded(dc);
  lock();
  if (dc->n) items = mem_alloc((SIZE_T)dc->n * sizeof(SaveItem));
  if (dc->n && !items) { unlock(); return E_OUTOFMEMORY; }
  for (ULONG i = 0; i < dc->n; i++) {
    CacheEntry *e = &dc->e[i];
    SaveItem it;
    if (e->derived) continue;
    mem_zero(&it, sizeof(it));
    it.fmt = e->fmt;
    it.advf = e->advf;
    it.id = e->id;
    it.is_view = e->is_view;
    it.has = e->has_data || e->raw.data != NULL;
    it.dirty = e->dirty;
    save_bytes(e, &it.bytes);
    /* by connection id, in the order they were made */
    {
      ULONG j = n;
      while (j > 0 && items[j - 1].id > it.id) { items[j] = items[j - 1]; j--; }
      items[j] = it;
      n++;
    }
  }
  unlock();
  /* what an earlier save left is replaced */
  for (int i = 0; i < 1000; i++) {
    WCHAR name[16];
    olepres_name(name, i);
    if (FAILED(IStorage_DestroyElement(stg, name))) break;
  }
  for (ULONG i = 0; i < n && SUCCEEDED(hr); i++) {
    SaveItem *it = &items[i];
    BYTE *buf;
    SIZE_T pos = 0, cap;
    BOOL as_view = it->fmt.cfFormat == 0 || (it->is_view && !it->has);
    WCHAR name[16];
    if (kind && it->fmt.dwAspect != DVASPECT_ICON) {
      /* the picture of a static class is the "CONTENTS" stream, and only when it was changed */
      if (!it->dirty || !it->has) continue;
      if (kind == 1 && it->bytes.size >= 40) {
        SIZE_T total = 14 + it->bytes.size;
        DWORD bisize = (DWORD)le32_at(it->bytes.data), clr = (DWORD)le32_at(it->bytes.data + 32), bits = (DWORD)(it->bytes.data[14] | it->bytes.data[15] << 8);
        DWORD off = 14 + bisize;
        BYTE *file;
        if (bisize >= 40 && bisize <= it->bytes.size) {
          if (bits <= 8) off += (clr ? clr : (1u << bits)) * 4;
          else if (le32_at(it->bytes.data + 16) == 3 && bisize == 40) off += 12;
          file = mem_alloc(total);
          if (!file) { hr = E_OUTOFMEMORY; break; }
          file[0] = 'B'; file[1] = 'M';
          pos = 2;
          put_u32(file, &pos, (DWORD)total);
          put_u32(file, &pos, 0);
          put_u32(file, &pos, off);
          mem_copy(file + 14, it->bytes.data, it->bytes.size);
          IStorage_DestroyElement(stg, W('C', 'O', 'N', 'T', 'E', 'N', 'T', 'S'));
          hr = put_stream(stg, W('C', 'O', 'N', 'T', 'E', 'N', 'T', 'S'), file, total);
          mem_free(file);
        }
      } else {
        IStorage_DestroyElement(stg, W('C', 'O', 'N', 'T', 'E', 'N', 'T', 'S'));
        hr = put_stream(stg, W('C', 'O', 'N', 'T', 'E', 'N', 'T', 'S'), it->bytes.data, it->bytes.size);
      }
      continue;
    }
    cap = 8 + 16 + 16 + it->bytes.size;
    buf = mem_alloc(cap);
    if (!buf) { hr = E_OUTOFMEMORY; break; }
    if (as_view) {
      put_u32(buf, &pos, 0);
    } else {
      put_u32(buf, &pos, 0xFFFFFFFFu);
      put_u32(buf, &pos, it->fmt.cfFormat);
    }
    put_u32(buf, &pos, 4);                          /* no target device */
    put_u32(buf, &pos, it->fmt.dwAspect);
    put_u32(buf, &pos, (DWORD)it->fmt.lindex);
    put_u32(buf, &pos, it->advf);
    if (!as_view) {
      put_u32(buf, &pos, 0);
      put_u32(buf, &pos, 0);
      put_u32(buf, &pos, 0);
      put_u32(buf, &pos, (DWORD)it->bytes.size);
      if (it->bytes.size) { mem_copy(buf + pos, it->bytes.data, it->bytes.size); pos += it->bytes.size; }
    }
    olepres_name(name, num++);
    hr = put_stream(stg, name, buf, pos);
    mem_free(buf);
  }
  for (ULONG i = 0; i < n; i++) blob_free(&items[i].bytes);
  mem_free(items);
  return hr;
}

/* ---- creation and destruction ---- */

static void dc_destroy(DataCache *dc) {
  IAdviseSink *vs;
  IStorage *stg;
  dc_stop(dc);
  lock();
  vs = dc->view_sink;
  stg = dc->stg;
  dc->view_sink = NULL;
  dc->stg = NULL;
  unlock();
  if (vs) IAdviseSink_Release(vs);
  if (stg) IStorage_Release(stg);
  dc_free_entries(dc);
  mem_free(dc);
}

EXPORT HRESULT WINAPI CreateDataCache(LPUNKNOWN outer, REFCLSID clsid, REFIID riid, LPVOID *out) {
  DataCache *dc;
  HRESULT hr;
  if (!out || !riid || !clsid) return E_INVALIDARG;
  *out = NULL;
  if (outer && !IsEqualGUID(riid, &IID_IUnknown)) return E_INVALIDARG;
  dc = mem_alloc(sizeof(*dc));
  if (!dc) return E_OUTOFMEMORY;
  dc->inner.lpVtbl = (IUnknownVtbl *)&g_dc_inner_vtbl;
  dc->data.lpVtbl = (IDataObjectVtbl *)&g_dc_data_vtbl;
  dc->view.lpVtbl = (IViewObject2Vtbl *)&g_dc_view_vtbl;
  dc->cache.lpVtbl = (IOleCache2Vtbl *)&g_dc_cache_vtbl;
  dc->control.lpVtbl = (IOleCacheControlVtbl *)&g_dc_control_vtbl;
  dc->persist.lpVtbl = (IPersistStorageVtbl *)&g_dc_persist_vtbl;
  dc->sink.lpVtbl = (IAdviseSinkVtbl *)&g_dc_sink_vtbl;
  dc->refs = 1;
  dc->outer = outer;
  dc->next_id = 2;
  dc->loaded = TRUE;
  lock();
  dc_set_class_locked(dc, clsid);
  unlock();
  hr = IUnknown_QueryInterface(&dc->inner, riid, out);
  IUnknown_Release(&dc->inner);
  if (FAILED(hr)) *out = NULL;
  return hr;
}

static HRESULT datacache_create_instance(IUnknown *outer, REFIID riid, void **out) { return CreateDataCache(outer, &CLSID_Ole32NoClass, riid, out); }
static HRESULT picture_dib_create_instance(IUnknown *outer, REFIID riid, void **out) { return CreateDataCache(outer, &CLSID_Ole32PicDib, riid, out); }
static HRESULT picture_metafile_create_instance(IUnknown *outer, REFIID riid, void **out) { return CreateDataCache(outer, &CLSID_Ole32PicMetafile, riid, out); }
static HRESULT picture_enhmetafile_create_instance(IUnknown *outer, REFIID riid, void **out) { return CreateDataCache(outer, &CLSID_Ole32PicEnhMetafile, riid, out); }

/* The classes of this file: the picture classes are caches of a static kind. */
#define BUILTIN_CLASSES_OLE(X) X(CLSID_Ole32PicDib, picture_dib_create_instance) X(CLSID_Ole32PicMetafile, picture_metafile_create_instance) \
  X(CLSID_Ole32PicEnhMetafile, picture_enhmetafile_create_instance)
