/* ole32: the stream on global memory (CreateStreamOnHGlobal), the byte array on global memory (CreateILockBytesOnHGlobal) and the helpers that write to / read from a stream. */

/* ---- stream on an HGLOBAL --------------------------------------------------------------------------------------------------------------------------------------- */

/* what a stream and its clones share: the memory, its size in use */
typedef struct HGlobalContents {
  LONG refs;
  HGLOBAL handle;
  ULARGE_INTEGER length;
  BOOL delete_on_release;
} HGlobalContents;

typedef struct HGlobalStream {
  IStream iface;
  LONG refs;
  HGlobalContents *contents;
  ULARGE_INTEGER position;
} HGlobalStream;

static const IStreamVtbl g_hglobal_stream_vtbl;
static HGlobalStream *hstream_of(IStream *s) { return (HGlobalStream *)s; }

static HRESULT WINAPI hstream_qi(IStream *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ISequentialStream) || IsEqualGUID(riid, &IID_IStream)) {
    *out = me;
    IStream_AddRef(me);
    return S_OK;
  }
  return E_NOINTERFACE;
}
static ULONG WINAPI hstream_addref(IStream *me) { return (ULONG)InterlockedIncrement(&hstream_of(me)->refs); }
static ULONG WINAPI hstream_release(IStream *me) {
  HGlobalStream *s = hstream_of(me);
  LONG refs = InterlockedDecrement(&s->refs);
  if (refs == 0) {
    HGlobalContents *c = s->contents;
    if (InterlockedDecrement(&c->refs) == 0) {
      if (c->delete_on_release && c->handle) GlobalFree(c->handle);
      mem_free(c);
    }
    mem_free(s);
  }
  return (ULONG)refs;
}

static HRESULT WINAPI hstream_read(IStream *me, void *buffer, ULONG want, ULONG *got) {
  HGlobalStream *s = hstream_of(me);
  ULONG have = 0;
  if (got) *got = 0;
  if (!buffer) return STG_E_INVALIDPOINTER;
  if (s->position.LowPart < s->contents->length.LowPart) {
    ULONG left = s->contents->length.LowPart - s->position.LowPart;
    have = left < want ? left : want;
  }
  if (have) {
    BYTE *base = GlobalLock(s->contents->handle);
    if (!base) have = 0;            /* (the memory was freed by someone else: nothing to read) */
    else {
      mem_copy(buffer, base + s->position.LowPart, have);
      GlobalUnlock(s->contents->handle);
      s->position.LowPart += have;
    }
  }
  if (got) *got = have;
  return S_OK;
}

static HRESULT hstream_grow(HGlobalContents *c, ULONGLONG size) {
  SIZE_T have = GlobalSize(c->handle);
  if (size > 0xFFFFFFFFull) return STG_E_MEDIUMFULL;
  if (size > have) {
    HGLOBAL h = GlobalReAlloc(c->handle, (SIZE_T)size, GMEM_MOVEABLE | GMEM_ZEROINIT);
    if (!h) return E_OUTOFMEMORY;
    c->handle = h;
  }
  return S_OK;
}

static HRESULT WINAPI hstream_write(IStream *me, const void *data, ULONG n, ULONG *done) {
  HGlobalStream *s = hstream_of(me);
  if (done) *done = 0;
  if (!data) return STG_E_INVALIDPOINTER;
  if (!n) return S_OK;
  ULONGLONG end = (ULONGLONG)s->position.LowPart + n;
  if (end > 0xFFFFFFFFull) return STG_E_MEDIUMFULL;
  if (end > s->contents->length.LowPart) {
    HRESULT hr = hstream_grow(s->contents, end);
    if (FAILED(hr)) return hr;
    s->contents->length.QuadPart = end;
  }
  BYTE *base = GlobalLock(s->contents->handle);
  if (!base) return E_OUTOFMEMORY;
  mem_copy(base + s->position.LowPart, data, n);
  GlobalUnlock(s->contents->handle);
  s->position.LowPart = (ULONG)end;
  if (done) *done = n;
  return S_OK;
}

/* The offset is a 32-bit signed number (the high part is ignored) and the position a 32-bit one; a failed seek reports where the stream is. */
static HRESULT WINAPI hstream_seek(IStream *me, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out) {
  HGlobalStream *s = hstream_of(me);
  LONGLONG base, target;
  LONG offset = (LONG)move.LowPart;
  HRESULT hr = S_OK;
  switch (origin) {
    case STREAM_SEEK_SET: base = 0; break;
    case STREAM_SEEK_CUR: base = s->position.LowPart; break;
    case STREAM_SEEK_END: base = s->contents->length.LowPart; break;
    default: base = -1; hr = STG_E_SEEKERROR; break;
  }
  if (SUCCEEDED(hr)) {
    target = base + offset;
    if (target < 0 || target > 0xFFFFFFFFll) hr = STG_E_SEEKERROR;
    else s->position.QuadPart = (ULONGLONG)target;
  }
  if (out) { out->LowPart = s->position.LowPart; out->HighPart = 0; }
  return hr;
}

static HRESULT WINAPI hstream_setsize(IStream *me, ULARGE_INTEGER size) {
  HGlobalStream *s = hstream_of(me);
  HGlobalContents *c = s->contents;
  ULONG want = size.LowPart;             /* (the high part is ignored) */
  if (want == c->length.LowPart) return S_OK;
  if (want > c->length.LowPart) {
    HRESULT hr = hstream_grow(c, want);
    if (FAILED(hr)) return hr;
    BYTE *base = GlobalLock(c->handle);
    if (!base) return E_OUTOFMEMORY;
    mem_zero(base + c->length.LowPart, want - c->length.LowPart);
    GlobalUnlock(c->handle);
  }
  c->length.QuadPart = want;
  return S_OK;
}

/* copies up to `n` bytes from `from` to `to` through a buffer (CopyTo); a destination that takes less than it is given ends the copy without an error */
static HRESULT copy_stream_bytes(IStream *from, IStream *to, ULARGE_INTEGER n, ULARGE_INTEGER *read_total, ULARGE_INTEGER *written_total) {
  BYTE *buffer = mem_alloc(1024);
  ULARGE_INTEGER rt = {{0, 0}}, wt = {{0, 0}};
  HRESULT hr = S_OK;
  if (!buffer) return E_OUTOFMEMORY;
  while (n.QuadPart) {
    ULONG want = n.QuadPart < 1024 ? (ULONG)n.QuadPart : 1024, got = 0, put = 0;
    hr = IStream_Read(from, buffer, want, &got);
    if (FAILED(hr)) break;
    if (!got) { hr = S_OK; break; }
    rt.QuadPart += got;
    hr = IStream_Write(to, buffer, got, &put);
    wt.QuadPart += put;
    if (FAILED(hr)) break;
    n.QuadPart -= got;
    if (put < got || got < want) break;
  }
  mem_free(buffer);
  if (read_total) *read_total = rt;
  if (written_total) *written_total = wt;
  return hr;
}
static HRESULT WINAPI hstream_copyto(IStream *me, IStream *dest, ULARGE_INTEGER n, ULARGE_INTEGER *read_total, ULARGE_INTEGER *written_total) {
  if (!dest) return STG_E_INVALIDPOINTER;
  return copy_stream_bytes(me, dest, n, read_total, written_total);
}
static HRESULT WINAPI hstream_commit(IStream *me, DWORD flags) { (void)me; (void)flags; return S_OK; }
static HRESULT WINAPI hstream_revert(IStream *me) { (void)me; return S_OK; }
static HRESULT WINAPI hstream_lock(IStream *me, ULARGE_INTEGER offset, ULARGE_INTEGER n, DWORD type) { (void)me; (void)offset; (void)n; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI hstream_unlock(IStream *me, ULARGE_INTEGER offset, ULARGE_INTEGER n, DWORD type) { (void)me; (void)offset; (void)n; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI hstream_stat(IStream *me, STATSTG *st, DWORD flags) {
  HGlobalStream *s = hstream_of(me);
  if (!st) return STG_E_INVALIDPOINTER;
  mem_zero(st, sizeof(*st));
  st->type = STGTY_STREAM;
  st->cbSize = s->contents->length;
  st->grfMode = STGM_READWRITE;
  (void)flags;
  return S_OK;
}
static HRESULT WINAPI hstream_clone(IStream *me, IStream **out) {
  HGlobalStream *s = hstream_of(me), *c;
  if (!out) return STG_E_INVALIDPOINTER;
  *out = NULL;
  c = mem_alloc(sizeof(*c));
  if (!c) return E_OUTOFMEMORY;
  c->iface.lpVtbl = (IStreamVtbl *)&g_hglobal_stream_vtbl;
  c->refs = 1;
  c->contents = s->contents;
  InterlockedIncrement(&s->contents->refs);
  c->position = s->position;
  *out = &c->iface;
  return S_OK;
}

static const IStreamVtbl g_hglobal_stream_vtbl = {hstream_qi, hstream_addref, hstream_release, hstream_read, hstream_write, hstream_seek, hstream_setsize, hstream_copyto,
                                                   hstream_commit, hstream_revert, hstream_lock, hstream_unlock, hstream_stat, hstream_clone};

EXPORT HRESULT WINAPI CreateStreamOnHGlobal(HGLOBAL handle, BOOL delete_on_release, LPSTREAM *out) {
  HGlobalStream *s;
  HGlobalContents *c;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  s = mem_alloc(sizeof(*s));
  c = mem_alloc(sizeof(*c));
  if (!s || !c) { mem_free(s); mem_free(c); return E_OUTOFMEMORY; }
  if (!handle) {
    handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_NODISCARD | GMEM_SHARE, 0);
    if (!handle) { mem_free(s); mem_free(c); return E_OUTOFMEMORY; }
    delete_on_release = TRUE;
  }
  c->refs = 1;
  c->handle = handle;
  c->delete_on_release = delete_on_release;
  c->length.QuadPart = GlobalSize(handle);
  s->iface.lpVtbl = (IStreamVtbl *)&g_hglobal_stream_vtbl;
  s->refs = 1;
  s->contents = c;
  *out = &s->iface;
  return S_OK;
}
EXPORT HRESULT WINAPI GetHGlobalFromStream(IStream *stream, HGLOBAL *handle) {
  if (!stream || !handle) return E_INVALIDARG;
  HGlobalStream *s = hstream_of(stream);
  if (s->iface.lpVtbl != (IStreamVtbl *)&g_hglobal_stream_vtbl) return E_INVALIDARG;
  *handle = s->contents->handle;
  return S_OK;
}

/* ---- byte array on an HGLOBAL --------------------------------------------------------------------------------------------------------------------------------- */

typedef struct HGlobalLockBytes {
  ILockBytes iface;
  LONG refs;
  HGLOBAL handle;
  ULARGE_INTEGER length;
  BOOL delete_on_release;
} HGlobalLockBytes;

static const ILockBytesVtbl g_hglobal_lockbytes_vtbl;
static HGlobalLockBytes *hlb_of(ILockBytes *l) { return (HGlobalLockBytes *)l; }

static HRESULT WINAPI hlb_qi(ILockBytes *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ILockBytes)) { *out = me; ILockBytes_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI hlb_addref(ILockBytes *me) { return (ULONG)InterlockedIncrement(&hlb_of(me)->refs); }
static ULONG WINAPI hlb_release(ILockBytes *me) {
  HGlobalLockBytes *l = hlb_of(me);
  LONG refs = InterlockedDecrement(&l->refs);
  if (refs == 0) {
    if (l->delete_on_release && l->handle) GlobalFree(l->handle);
    mem_free(l);
  }
  return (ULONG)refs;
}
static HRESULT WINAPI hlb_readat(ILockBytes *me, ULARGE_INTEGER offset, void *buffer, ULONG n, ULONG *got) {
  HGlobalLockBytes *l = hlb_of(me);
  ULONG have = 0;
  if (got) *got = 0;
  if (!buffer) return E_INVALIDARG;
  if (offset.QuadPart < l->length.QuadPart) {
    ULONGLONG left = l->length.QuadPart - offset.QuadPart;
    have = left < n ? (ULONG)left : n;
  }
  if (have) {
    BYTE *base = GlobalLock(l->handle);
    if (!base) return E_OUTOFMEMORY;
    mem_copy(buffer, base + offset.QuadPart, have);
    GlobalUnlock(l->handle);
  }
  if (got) *got = have;
  return S_OK;
}
static HRESULT WINAPI hlb_writeat(ILockBytes *me, ULARGE_INTEGER offset, const void *data, ULONG n, ULONG *done) {
  HGlobalLockBytes *l = hlb_of(me);
  if (done) *done = 0;
  if (!data) return STG_E_INVALIDPOINTER;
  if (!n) return S_OK;
  if (offset.QuadPart > 0x7FFFFFFFull - n) return STG_E_MEDIUMFULL;
  ULONGLONG end = offset.QuadPart + n;
  if (end > l->length.QuadPart) {
    if (end > GlobalSize(l->handle)) {
      HGLOBAL h = GlobalReAlloc(l->handle, (SIZE_T)end, GMEM_MOVEABLE | GMEM_ZEROINIT);
      if (!h) return STG_E_MEDIUMFULL;
      l->handle = h;
    }
    l->length.QuadPart = end;
  }
  BYTE *base = GlobalLock(l->handle);
  if (!base) return E_OUTOFMEMORY;
  mem_copy(base + offset.QuadPart, data, n);
  GlobalUnlock(l->handle);
  if (done) *done = n;
  return S_OK;
}
static HRESULT WINAPI hlb_flush(ILockBytes *me) { (void)me; return S_OK; }
static HRESULT WINAPI hlb_setsize(ILockBytes *me, ULARGE_INTEGER size) {
  HGlobalLockBytes *l = hlb_of(me);
  if (size.QuadPart > 0x7FFFFFFFull) return STG_E_MEDIUMFULL;
  if (size.QuadPart > GlobalSize(l->handle)) {
    HGLOBAL h = GlobalReAlloc(l->handle, (SIZE_T)size.QuadPart, GMEM_MOVEABLE | GMEM_ZEROINIT);
    if (!h) return STG_E_MEDIUMFULL;
    l->handle = h;
  } else if (size.QuadPart < l->length.QuadPart) {
    BYTE *base = GlobalLock(l->handle);
    if (base) { mem_zero(base + size.QuadPart, (SIZE_T)(l->length.QuadPart - size.QuadPart)); GlobalUnlock(l->handle); }
  }
  l->length = size;
  return S_OK;
}
static HRESULT WINAPI hlb_lock(ILockBytes *me, ULARGE_INTEGER offset, ULARGE_INTEGER n, DWORD type) { (void)me; (void)offset; (void)n; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI hlb_unlock(ILockBytes *me, ULARGE_INTEGER offset, ULARGE_INTEGER n, DWORD type) { (void)me; (void)offset; (void)n; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT WINAPI hlb_stat(ILockBytes *me, STATSTG *st, DWORD flags) {
  HGlobalLockBytes *l = hlb_of(me);
  if (!st) return STG_E_INVALIDPOINTER;
  mem_zero(st, sizeof(*st));
  st->type = STGTY_LOCKBYTES;
  st->cbSize = l->length;
  (void)flags;
  return S_OK;
}
static const ILockBytesVtbl g_hglobal_lockbytes_vtbl = {hlb_qi, hlb_addref, hlb_release, hlb_readat, hlb_writeat, hlb_flush, hlb_setsize, hlb_lock, hlb_unlock, hlb_stat};

EXPORT HRESULT WINAPI CreateILockBytesOnHGlobal(HGLOBAL handle, BOOL delete_on_release, ILockBytes **out) {
  HGlobalLockBytes *l;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  l = mem_alloc(sizeof(*l));
  if (!l) return E_OUTOFMEMORY;
  if (!handle) {
    handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_NODISCARD | GMEM_SHARE, 0);
    if (!handle) { mem_free(l); return E_OUTOFMEMORY; }
    delete_on_release = TRUE;
  }
  l->iface.lpVtbl = (ILockBytesVtbl *)&g_hglobal_lockbytes_vtbl;
  l->refs = 1;
  l->handle = handle;
  l->delete_on_release = delete_on_release;
  l->length.QuadPart = GlobalSize(handle);
  *out = &l->iface;
  return S_OK;
}
EXPORT HRESULT WINAPI GetHGlobalFromILockBytes(ILockBytes *lb, HGLOBAL *handle) {
  if (!lb || !handle) return E_INVALIDARG;
  HGlobalLockBytes *l = hlb_of(lb);
  if (l->iface.lpVtbl != (ILockBytesVtbl *)&g_hglobal_lockbytes_vtbl) {
    /* another kind of byte array: only the one on an HGLOBAL has a handle; ask it to read everything into a new one */
    STATSTG st;
    if (FAILED(ILockBytes_Stat(lb, &st, STATFLAG_NONAME))) return E_INVALIDARG;
    return E_INVALIDARG;
  }
  *handle = l->handle;
  return S_OK;
}

/* ---- helpers on streams ----------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT stream_read_all(IStream *s, void *buffer, ULONG n) {
  ULONG got = 0;
  HRESULT hr = IStream_Read(s, buffer, n, &got);
  if (FAILED(hr)) return hr;
  return got == n ? S_OK : STG_E_READFAULT;
}
static HRESULT stream_write_all(IStream *s, const void *buffer, ULONG n) {
  ULONG put = 0;
  HRESULT hr = IStream_Write(s, buffer, n, &put);
  if (FAILED(hr)) return hr;
  return put == n ? S_OK : STG_E_WRITEFAULT;
}
static HRESULT stream_tell(IStream *s, ULARGE_INTEGER *pos) { LARGE_INTEGER zero = {{0, 0}}; return IStream_Seek(s, zero, STREAM_SEEK_CUR, pos); }
static HRESULT stream_seek_to(IStream *s, ULONGLONG pos) { LARGE_INTEGER p; p.QuadPart = (LONGLONG)pos; return IStream_Seek(s, p, STREAM_SEEK_SET, NULL); }

EXPORT HRESULT WINAPI WriteClassStm(IStream *stm, REFCLSID clsid) {
  if (!stm || !clsid) return E_INVALIDARG;
  return IStream_Write(stm, clsid, sizeof(CLSID), NULL);
}
EXPORT HRESULT WINAPI ReadClassStm(IStream *stm, CLSID *clsid) {
  ULONG got = 0;
  if (!stm || !clsid) return E_INVALIDARG;
  HRESULT hr = IStream_Read(stm, clsid, sizeof(CLSID), &got);
  if (FAILED(hr)) return hr;
  return got == sizeof(CLSID) ? S_OK : STG_E_READFAULT;
}
/* A string in a stream: a length in characters including the end mark, then the characters. */
EXPORT HRESULT WINAPI WriteStringStream(IStream *stm, LPCOLESTR s) {
  DWORD n = s ? (DWORD)wlen(s) + 1 : 0;
  HRESULT hr = stream_write_all(stm, &n, 4);
  if (SUCCEEDED(hr) && n) hr = stream_write_all(stm, s, n * sizeof(WCHAR));
  return hr;
}
EXPORT HRESULT WINAPI ReadStringStream(IStream *stm, LPOLESTR *out) {
  DWORD n;
  *out = NULL;
  HRESULT hr = stream_read_all(stm, &n, 4);
  if (FAILED(hr)) return hr;
  if (!n) return S_OK;
  if (n > 0x100000) return STG_E_INVALIDHEADER;
  WCHAR *text = CoTaskMemAlloc(n * sizeof(WCHAR));
  if (!text) return E_OUTOFMEMORY;
  hr = stream_read_all(stm, text, n * sizeof(WCHAR));
  if (FAILED(hr)) { CoTaskMemFree(text); return hr; }
  text[n - 1] = 0;
  *out = text;
  return S_OK;
}
