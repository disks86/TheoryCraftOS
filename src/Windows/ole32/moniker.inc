/* ole32: monikers (file, item, anti, generic composite, pointer, objref, class), bind contexts, the running object table and the display-name parser.
 *
 * All the system monikers share one C type (MonikerObj) and one IMoniker vtable: the kind selects the behaviour. A generic composite keeps its components as a flat array
 * (never a composite inside a composite), so walking it needs no recursion; Load and unmarshaling of nested data from a stream are bounded by a per-thread depth count.
 * The running object table is process wide; it identifies a moniker by its comparison data (IROTData, or the class and display name as Windows does), never by calling
 * IsEqual or Hash on the caller's moniker. Nothing calls out to an object while lock() is held: ROT entries carry their own reference count so a lookup can leave the lock
 * before it touches the registered object. */

static const CLSID MK_CLSID_File = {0x00000303, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const CLSID MK_CLSID_Item = {0x00000304, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const CLSID MK_CLSID_Anti = {0x00000305, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const CLSID MK_CLSID_Pointer = {0x00000306, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const CLSID MK_CLSID_Composite = {0x00000309, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const CLSID MK_CLSID_Class = {0x0000031a, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const CLSID MK_CLSID_Objref = {0x00000327, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

#define MK_MAX_BYTES 0x100000u          /* the largest string or blob a saved moniker may carry */
#define MK_MAX_COMPONENTS 4096u         /* components of a composite read from a stream */
#define MK_MAX_DEPTH 32                 /* nesting of composites while loading or unmarshaling */
#define MK_MAX_ANTI 0xfffffu            /* the largest anti moniker count Windows accepts */

static const WCHAR g_mk_empty[] = {0};
static const WCHAR *nz(const WCHAR *s) { return s ? s : g_mk_empty; }

/* ---- small helpers --------------------------------------------------------------------------------------------------------------------------------------------------- */

static WCHAR *heap_wdup_n(const WCHAR *s, SIZE_T n) {
  if (n > MK_MAX_BYTES) return NULL;
  WCHAR *r = mem_alloc((n + 1) * sizeof(WCHAR));
  if (r && n) mem_copy(r, s, n * sizeof(WCHAR));
  return r;
}
static WCHAR *task_wdup_n(const WCHAR *s, SIZE_T n) {
  if (n > ((SIZE_T)-1) / sizeof(WCHAR) - 1) return NULL;
  WCHAR *r = CoTaskMemAlloc((n + 1) * sizeof(WCHAR));
  if (!r) return NULL;
  if (n) mem_copy(r, s, n * sizeof(WCHAR));
  r[n] = 0;
  return r;
}

/* a growable wide string on the heap */
typedef struct { WCHAR *p; SIZE_T len, cap; BOOL failed; } WBuf;
static void wb_add_n(WBuf *b, const WCHAR *s, SIZE_T n) {
  if (b->failed) return;
  if (n > ((SIZE_T)-1) / sizeof(WCHAR) / 2 - b->len - 1) { b->failed = TRUE; return; }
  if (b->len + n + 1 > b->cap) {
    SIZE_T cap = b->cap ? b->cap * 2 : 32;
    while (cap < b->len + n + 1) cap *= 2;
    WCHAR *p = mem_realloc(b->p, cap * sizeof(WCHAR));
    if (!p) { b->failed = TRUE; return; }
    b->p = p;
    b->cap = cap;
  }
  if (n) mem_copy(b->p + b->len, s, n * sizeof(WCHAR));
  b->len += n;
  b->p[b->len] = 0;
}
static void wb_add(WBuf *b, const WCHAR *s) { wb_add_n(b, s, wlen(s)); }
static void wb_add_ascii(WBuf *b, const char *s) { while (*s) { WCHAR c = (WCHAR)(BYTE)*s++; wb_add_n(b, &c, 1); } }
/* the text as CoTaskMem memory (the buffer is released either way) */
static HRESULT wb_finish_task(WBuf *b, LPOLESTR *out) {
  *out = NULL;
  if (b->failed) { mem_free(b->p); return E_OUTOFMEMORY; }
  *out = task_wdup_n(b->p ? b->p : g_mk_empty, b->len);
  mem_free(b->p);
  b->p = NULL;
  return *out ? S_OK : E_OUTOFMEMORY;
}
static WCHAR *wb_finish_heap(WBuf *b) {
  if (b->failed) { mem_free(b->p); return NULL; }
  if (!b->p) return heap_wdup_n(g_mk_empty, 0);
  return b->p;
}

/* nesting depth of composite loads on this thread (a TLS slot of this module) */
static DWORD g_mk_tls = TLS_OUT_OF_INDEXES;
static BOOL mk_depth_enter(void) {
  DWORD slot = *(volatile DWORD *)&g_mk_tls;
  if (slot == TLS_OUT_OF_INDEXES) {
    DWORD fresh = TlsAlloc();
    if (fresh == TLS_OUT_OF_INDEXES) return FALSE;
    if ((DWORD)InterlockedCompareExchange((LONG *)&g_mk_tls, (LONG)fresh, (LONG)TLS_OUT_OF_INDEXES) != TLS_OUT_OF_INDEXES) TlsFree(fresh);
    slot = *(volatile DWORD *)&g_mk_tls;
  }
  ULONG_PTR depth = (ULONG_PTR)TlsGetValue(slot);
  if (depth >= MK_MAX_DEPTH) return FALSE;
  TlsSetValue(slot, (void *)(depth + 1));
  return TRUE;
}
static void mk_depth_leave(void) {
  DWORD slot = *(volatile DWORD *)&g_mk_tls;
  if (slot == TLS_OUT_OF_INDEXES) return;
  ULONG_PTR depth = (ULONG_PTR)TlsGetValue(slot);
  if (depth) TlsSetValue(slot, (void *)(depth - 1));
}

/* base64 (the objref moniker's display name) */
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(WBuf *b, const BYTE *p, ULONG n) {
  for (ULONG i = 0; i < n; i += 3) {
    ULONG v = (ULONG)p[i] << 16 | (i + 1 < n ? (ULONG)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    WCHAR q[4];
    q[0] = (WCHAR)kB64[v >> 18 & 63];
    q[1] = (WCHAR)kB64[v >> 12 & 63];
    q[2] = i + 1 < n ? (WCHAR)kB64[v >> 6 & 63] : '=';
    q[3] = i + 2 < n ? (WCHAR)kB64[v & 63] : '=';
    wb_add_n(b, q, 4);
  }
}
static int b64_value(WCHAR c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
/* n characters of base64 (padding allowed at the end only); NULL when the text is not base64 */
static BYTE *b64_decode(const WCHAR *s, SIZE_T n, ULONG *size) {
  while (n && s[n - 1] == '=') n--;
  if (n > MK_MAX_BYTES) return NULL;
  BYTE *r = mem_alloc(n * 3 / 4 + 3);
  if (!r) return NULL;
  ULONG acc = 0, bits = 0, k = 0;
  for (SIZE_T i = 0; i < n; i++) {
    int v = b64_value(s[i]);
    if (v < 0) { mem_free(r); return NULL; }
    acc = acc << 6 | (ULONG)v;
    bits += 6;
    if (bits >= 8) { bits -= 8; r[k++] = (BYTE)(acc >> bits); }
  }
  *size = k;
  return r;
}

/* ---- the moniker object ---------------------------------------------------------------------------------------------------------------------------------------------- */

enum { MK_FILE, MK_ITEM, MK_ANTI, MK_COMPOSITE, MK_POINTER, MK_OBJREF, MK_CLASS };

typedef struct MonikerObj {
  IMoniker iface;
  IROTData rotdata;
  IMarshal marshal;
  LONG refs;
  int kind;
  WCHAR *path;                 /* file: the path */
  WCHAR *delim, *name;         /* item: the delimiter (may be NULL) and the item name */
  DWORD anti;                  /* anti: how many levels it removes */
  IMoniker **comps;            /* composite: the components, left to right, none of them a composite of this module */
  ULONG ncomps;
  IUnknown *obj;               /* pointer and objref: the object */
  BYTE *objref;                /* objref: the marshaled object (made once, for the display name) */
  ULONG objref_size;
  CLSID clsid;                 /* class: the class and the optional text after it (raw bytes as saved) */
  WCHAR *extra;
  DWORD extra_bytes;
} MonikerObj;

static const IMonikerVtbl g_mk_vtbl;
static const IROTDataVtbl g_mk_rot_vtbl;
static const IMarshalVtbl g_mk_marshal_vtbl;

static MonikerObj *mk_impl(IMoniker *m) { return m && m->lpVtbl == &g_mk_vtbl ? (MonikerObj *)m : NULL; }
static MonikerObj *mk_from_rot(IROTData *r) { return (MonikerObj *)((BYTE *)r - offsetof(MonikerObj, rotdata)); }
static MonikerObj *mk_from_marshal(IMarshal *r) { return (MonikerObj *)((BYTE *)r - offsetof(MonikerObj, marshal)); }

static const CLSID *mk_kind_clsid(int kind) {
  switch (kind) {
    case MK_FILE: return &MK_CLSID_File;
    case MK_ITEM: return &MK_CLSID_Item;
    case MK_ANTI: return &MK_CLSID_Anti;
    case MK_COMPOSITE: return &MK_CLSID_Composite;
    case MK_POINTER: return &MK_CLSID_Pointer;
    case MK_OBJREF: return &MK_CLSID_Objref;
    default: return &MK_CLSID_Class;
  }
}
static int mk_kind_from_clsid(REFCLSID clsid) {
  for (int k = MK_FILE; k <= MK_CLASS; k++) if (IsEqualGUID(clsid, mk_kind_clsid(k))) return k;
  return -1;
}

static MonikerObj *mk_new(int kind) {
  MonikerObj *m = mem_alloc(sizeof(*m));
  if (!m) return NULL;
  m->iface.lpVtbl = &g_mk_vtbl;
  m->rotdata.lpVtbl = &g_mk_rot_vtbl;
  m->marshal.lpVtbl = &g_mk_marshal_vtbl;
  m->refs = 1;
  m->kind = kind;
  m->anti = kind == MK_ANTI ? 1 : 0;
  return m;
}
/* the state a moniker has when it is created by its class (before IPersistStream::Load) */
static MonikerObj *mk_new_default(int kind) {
  MonikerObj *m = mk_new(kind);
  if (!m) return NULL;
  if (kind == MK_FILE) m->path = heap_wdup_n(g_mk_empty, 0);
  if (kind == MK_ITEM) { m->delim = heap_wdup_n(g_mk_empty, 0); m->name = heap_wdup_n(g_mk_empty, 0); }
  if ((kind == MK_FILE && !m->path) || (kind == MK_ITEM && (!m->delim || !m->name))) { mem_free(m->path); mem_free(m->delim); mem_free(m->name); mem_free(m); return NULL; }
  return m;
}
static void mk_release_comps(IMoniker **comps, ULONG n) {
  for (ULONG i = 0; i < n; i++) if (comps[i]) IMoniker_Release(comps[i]);
  mem_free(comps);
}
static void mk_destroy(MonikerObj *m) {
  mem_free(m->path);
  mem_free(m->delim);
  mem_free(m->name);
  mem_free(m->extra);
  mem_free(m->objref);
  if (m->comps) mk_release_comps(m->comps, m->ncomps);
  if (m->obj) IUnknown_Release(m->obj);
  mem_free(m);
}

/* ---- lists of monikers ----------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { IMoniker **v; ULONG n, cap; BOOL failed; } MkList;
/* append (the list takes the reference passed in) */
static void ml_push(MkList *l, IMoniker *m) {
  if (l->failed) { IMoniker_Release(m); return; }
  if (l->n == l->cap) {
    ULONG cap = l->cap ? l->cap * 2 : 8;
    if (cap < l->cap || cap > 0x10000000u) { l->failed = TRUE; IMoniker_Release(m); return; }
    IMoniker **v = mem_realloc(l->v, cap * sizeof(*v));
    if (!v) { l->failed = TRUE; IMoniker_Release(m); return; }
    l->v = v;
    l->cap = cap;
  }
  l->v[l->n++] = m;
}
/* append a moniker, or the components of a composite of this module (each with a new reference) */
static void ml_push_flat(MkList *l, IMoniker *m) {
  MonikerObj *c = mk_impl(m);
  if (c && c->kind == MK_COMPOSITE) {
    for (ULONG i = 0; i < c->ncomps; i++) { IMoniker_AddRef(c->comps[i]); ml_push(l, c->comps[i]); }
    return;
  }
  IMoniker_AddRef(m);
  ml_push(l, m);
}
static void ml_free(MkList *l) { mk_release_comps(l->v, l->n); l->v = NULL; l->n = l->cap = 0; }

/* a moniker of the n monikers in v (the references move to it): NULL for none, the moniker itself for one, else a new composite (no simplification) */
static HRESULT mk_from_parts(IMoniker **v, ULONG n, IMoniker **out) {
  *out = NULL;
  if (!n) return S_OK;
  if (n == 1) { *out = v[0]; v[0] = NULL; return S_OK; }
  MonikerObj *c = mk_new(MK_COMPOSITE);
  IMoniker **copy = mem_alloc((SIZE_T)n * sizeof(*copy));
  if (!c || !copy) { mem_free(c); mem_free(copy); return E_OUTOFMEMORY; }
  for (ULONG i = 0; i < n; i++) { copy[i] = v[i]; v[i] = NULL; }
  c->comps = copy;
  c->ncomps = n;
  *out = &c->iface;
  return S_OK;
}
/* the first k components of a composite as a moniker (new references) */
static HRESULT mk_prefix_of(MonikerObj *c, ULONG k, IMoniker **out) {
  MkList l = {0};
  for (ULONG i = 0; i < k && i < c->ncomps; i++) { IMoniker_AddRef(c->comps[i]); ml_push(&l, c->comps[i]); }
  if (l.failed) { ml_free(&l); *out = NULL; return E_OUTOFMEMORY; }
  HRESULT hr = mk_from_parts(l.v, l.n, out);
  ml_free(&l);
  return hr;
}

static HRESULT mk_create_anti(DWORD count, IMoniker **out) {
  MonikerObj *m = mk_new(MK_ANTI);
  *out = NULL;
  if (!m) return E_OUTOFMEMORY;
  m->anti = count;
  *out = &m->iface;
  return S_OK;
}

/* ---- IUnknown / IPersistStream ---------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI mk_qi(IMoniker *me, REFIID riid, void **out) {
  MonikerObj *m = (MonikerObj *)me;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  BOOL pointerish = m->kind == MK_POINTER || m->kind == MK_OBJREF;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IPersistStream) || IsEqualGUID(riid, &IID_IMoniker)) *out = me;
  else if (IsEqualGUID(riid, &IID_IPersist) && m->kind == MK_CLASS) *out = me;
  else if (IsEqualGUID(riid, &IID_IROTData) && !pointerish) *out = &m->rotdata;
  else if (IsEqualGUID(riid, &IID_IMarshal)) *out = &m->marshal;
  else if (IsEqualGUID(riid, mk_kind_clsid(m->kind))) *out = me;           /* the moniker's own class names its implementation */
  else if (m->kind == MK_OBJREF && IsEqualGUID(riid, &MK_CLSID_Pointer)) *out = me;
  else return E_NOINTERFACE;
  IUnknown_AddRef((IUnknown *)*out);
  return S_OK;
}
static ULONG WINAPI mk_addref(IMoniker *me) { return (ULONG)InterlockedIncrement(&((MonikerObj *)me)->refs); }
static ULONG WINAPI mk_release(IMoniker *me) {
  MonikerObj *m = (MonikerObj *)me;
  LONG r = InterlockedDecrement(&m->refs);
  if (!r) mk_destroy(m);
  return (ULONG)r;
}
static HRESULT WINAPI mk_getclassid(IMoniker *me, CLSID *clsid) {
  if (!clsid) return E_POINTER;
  *clsid = *mk_kind_clsid(((MonikerObj *)me)->kind);
  return S_OK;
}
static HRESULT WINAPI mk_isdirty(IMoniker *me) { (void)me; return S_FALSE; }

/* -- strings in item moniker records: DWORD byte count, an ANSI string with its terminator, optionally followed by the same text as UTF-16 (no terminator) -- */
static BOOL needs_wide(const WCHAR *s) { for (; *s; s++) if (*s > 0x7f) return TRUE; return FALSE; }

static HRESULT write_ansi_record(IStream *s, const WCHAR *str) {
  DWORD n = 0;
  if (!str) return stream_write_all(s, &n, sizeof(n));
  SIZE_T chars = wlen(str);
  if (chars > MK_MAX_BYTES / 4) return E_INVALIDARG;
  int ansi = WideCharToMultiByte(CP_ACP, 0, str, -1, NULL, 0, NULL, NULL);
  if (ansi <= 0) return E_FAIL;
  BOOL wide = needs_wide(str);
  n = (DWORD)ansi + (wide ? (DWORD)(chars * sizeof(WCHAR)) : 0);
  char *a = mem_alloc((SIZE_T)ansi);
  if (!a) return E_OUTOFMEMORY;
  WideCharToMultiByte(CP_ACP, 0, str, -1, a, ansi, NULL, NULL);
  HRESULT hr = stream_write_all(s, &n, sizeof(n));
  if (SUCCEEDED(hr)) hr = stream_write_all(s, a, (ULONG)ansi);
  if (SUCCEEDED(hr) && wide) hr = stream_write_all(s, str, (ULONG)(chars * sizeof(WCHAR)));
  mem_free(a);
  return hr;
}
/* *out is NULL for an empty record */
static HRESULT read_ansi_record(IStream *s, WCHAR **out) {
  DWORD n;
  *out = NULL;
  HRESULT hr = stream_read_all(s, &n, sizeof(n));
  if (FAILED(hr)) return hr;
  if (!n) return S_OK;
  if (n > MK_MAX_BYTES) return E_OUTOFMEMORY;
  BYTE *buf = mem_alloc(n);
  if (!buf) return E_OUTOFMEMORY;
  hr = stream_read_all(s, buf, n);
  if (FAILED(hr)) { mem_free(buf); return hr; }
  DWORD ansi = 0;
  while (ansi < n && buf[ansi]) ansi++;
  if (ansi == n) { mem_free(buf); return E_FAIL; }       /* the ANSI part must be terminated */
  DWORD rest = n - (ansi + 1);
  WCHAR *r;
  if (rest) {
    if (rest % sizeof(WCHAR)) { mem_free(buf); return E_FAIL; }
    r = mem_alloc(rest + sizeof(WCHAR));
    if (r) mem_copy(r, buf + ansi + 1, rest);
  } else {
    int wn = MultiByteToWideChar(CP_ACP, 0, (const char *)buf, -1, NULL, 0);
    r = wn > 0 ? mem_alloc((SIZE_T)wn * sizeof(WCHAR)) : NULL;
    if (r) MultiByteToWideChar(CP_ACP, 0, (const char *)buf, -1, r, wn);
    else if (wn <= 0) r = heap_wdup_n(g_mk_empty, 0);
  }
  mem_free(buf);
  if (!r) return E_OUTOFMEMORY;
  *out = r;
  return S_OK;
}

/* -- file monikers: WORD cAnti, DWORD ansi bytes, ANSI path, WORD 0xFFFF, WORD 0xDEAD, 20 bytes zero, DWORD unicode size (bytes + 6), [DWORD bytes, WORD 3, UTF-16 path] -- */
static HRESULT file_save(MonikerObj *m, IStream *s) {
  const WCHAR *path = nz(m->path);
  SIZE_T chars = wlen(path);
  if (chars > MK_MAX_BYTES / 4) return E_INVALIDARG;
  int ansi = WideCharToMultiByte(CP_ACP, 0, path, -1, NULL, 0, NULL, NULL);
  if (ansi <= 0) return E_FAIL;
  char *a = mem_alloc((SIZE_T)ansi);
  if (!a) return E_OUTOFMEMORY;
  WideCharToMultiByte(CP_ACP, 0, path, -1, a, ansi, NULL, NULL);
  WORD zero_word = 0, ffff = 0xFFFF, dead = 0xDEAD, three = 3;
  DWORD na = (DWORD)ansi;
  BYTE reserved[20];
  mem_zero(reserved, sizeof(reserved));
  HRESULT hr = stream_write_all(s, &zero_word, sizeof(WORD));
  if (SUCCEEDED(hr)) hr = stream_write_all(s, &na, sizeof(DWORD));
  if (SUCCEEDED(hr)) hr = stream_write_all(s, a, na);
  if (SUCCEEDED(hr)) hr = stream_write_all(s, &ffff, sizeof(WORD));
  if (SUCCEEDED(hr)) hr = stream_write_all(s, &dead, sizeof(WORD));
  if (SUCCEEDED(hr)) hr = stream_write_all(s, reserved, sizeof(reserved));
  if (SUCCEEDED(hr)) {
    if (!needs_wide(path)) {
      DWORD none = 0;
      hr = stream_write_all(s, &none, sizeof(DWORD));
    } else {
      DWORD bytes = (DWORD)(chars * sizeof(WCHAR)), total = bytes + 6;
      hr = stream_write_all(s, &total, sizeof(DWORD));
      if (SUCCEEDED(hr)) hr = stream_write_all(s, &bytes, sizeof(DWORD));
      if (SUCCEEDED(hr)) hr = stream_write_all(s, &three, sizeof(WORD));
      if (SUCCEEDED(hr)) hr = stream_write_all(s, path, bytes);
    }
  }
  mem_free(a);
  return hr;
}
static ULONGLONG file_save_size(MonikerObj *m) {
  const WCHAR *path = nz(m->path);
  SIZE_T chars = wlen(path);
  int ansi = WideCharToMultiByte(CP_ACP, 0, path, -1, NULL, 0, NULL, NULL);
  ULONGLONG n = 2 + 4 + (ULONGLONG)(ansi > 0 ? ansi : 1) + 2 + 2 + 20 + 4;
  if (needs_wide(path)) n += 4 + 2 + (ULONGLONG)chars * sizeof(WCHAR);
  return n;
}
static HRESULT file_load(MonikerObj *m, IStream *s) {
  WORD anti, w;
  DWORD na, wsize;
  BYTE reserved[20];
  HRESULT hr = stream_read_all(s, &anti, sizeof(WORD));
  if (SUCCEEDED(hr)) hr = stream_read_all(s, &na, sizeof(DWORD));
  if (FAILED(hr)) return hr;
  if (na > MK_MAX_BYTES) return E_OUTOFMEMORY;
  char *a = mem_alloc((SIZE_T)na + 1);
  if (!a) return E_OUTOFMEMORY;
  hr = stream_read_all(s, a, na);
  if (SUCCEEDED(hr)) hr = stream_read_all(s, &w, sizeof(WORD));          /* end of server (0xFFFF) */
  if (SUCCEEDED(hr)) hr = stream_read_all(s, &w, sizeof(WORD));          /* version (0xDEAD) */
  if (SUCCEEDED(hr)) hr = stream_read_all(s, reserved, sizeof(reserved));
  if (SUCCEEDED(hr)) hr = stream_read_all(s, &wsize, sizeof(DWORD));
  if (FAILED(hr)) { mem_free(a); return hr; }
  WCHAR *path = NULL;
  if (!wsize) {
    int wn = MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);           /* a is terminated: the extra byte is zero */
    if (wn > 0 && (path = mem_alloc((SIZE_T)wn * sizeof(WCHAR)))) MultiByteToWideChar(CP_ACP, 0, a, -1, path, wn);
  } else {
    DWORD bytes;
    WORD key;
    if (wsize < 6 || wsize - 6 > MK_MAX_BYTES) { mem_free(a); return E_FAIL; }
    hr = stream_read_all(s, &bytes, sizeof(DWORD));
    if (SUCCEEDED(hr)) hr = stream_read_all(s, &key, sizeof(WORD));
    if (SUCCEEDED(hr) && (bytes != wsize - 6 || bytes % sizeof(WCHAR))) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
      path = mem_alloc((SIZE_T)bytes + sizeof(WCHAR));
      if (!path) hr = E_OUTOFMEMORY;
      else if (FAILED(hr = stream_read_all(s, path, bytes))) { mem_free(path); path = NULL; }
    }
    if (FAILED(hr)) { mem_free(a); return hr; }
  }
  mem_free(a);
  if (!path) return E_OUTOFMEMORY;
  if (anti) {                   /* the parent-directory steps Windows keeps apart from the path */
    WBuf b = {0};
    for (WORD i = 0; i < anti; i++) wb_add_ascii(&b, "..\\");
    wb_add(&b, path);
    mem_free(path);
    if (!(path = wb_finish_heap(&b))) return E_OUTOFMEMORY;
  }
  mem_free(m->path);
  m->path = path;
  return S_OK;
}

/* -- class monikers: CLSID, DWORD byte count, the bytes (text after the CLSID in the display name) -- */
static HRESULT class_load(MonikerObj *m, IStream *s) {
  CLSID clsid;
  DWORD n;
  HRESULT hr = stream_read_all(s, &clsid, sizeof(clsid));
  if (SUCCEEDED(hr)) hr = stream_read_all(s, &n, sizeof(n));
  if (FAILED(hr)) return hr;
  if (n > MK_MAX_BYTES) return E_OUTOFMEMORY;
  WCHAR *extra = NULL;
  if (n) {
    extra = mem_alloc(((SIZE_T)n / sizeof(WCHAR) + 2) * sizeof(WCHAR));      /* always terminated as a wide string */
    if (!extra) return E_OUTOFMEMORY;
    if (FAILED(hr = stream_read_all(s, extra, n))) { mem_free(extra); return hr; }
  }
  mem_free(m->extra);
  m->clsid = clsid;
  m->extra = extra;
  m->extra_bytes = n;
  return S_OK;
}
static HRESULT class_save(MonikerObj *m, IStream *s) {
  HRESULT hr = stream_write_all(s, &m->clsid, sizeof(CLSID));
  if (SUCCEEDED(hr)) hr = stream_write_all(s, &m->extra_bytes, sizeof(DWORD));
  if (SUCCEEDED(hr) && m->extra_bytes) hr = stream_write_all(s, m->extra, m->extra_bytes);
  return hr;
}

/* -- generic composites: DWORD count, then each component as CLSID + its own saved data -- */
static HRESULT mk_create_by_clsid(REFCLSID clsid, IMoniker **out) {
  int kind = mk_kind_from_clsid(clsid);
  *out = NULL;
  if (kind >= 0) {
    MonikerObj *m = mk_new_default(kind);
    if (!m) return E_OUTOFMEMORY;
    *out = &m->iface;
    return S_OK;
  }
  return CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMoniker, (void **)out);
}
static HRESULT mk_load_component(IStream *s, IMoniker **out) {
  CLSID clsid;
  *out = NULL;
  HRESULT hr = ReadClassStm(s, &clsid);
  if (FAILED(hr)) return hr;
  IMoniker *m;
  if (FAILED(hr = mk_create_by_clsid(&clsid, &m))) return hr;
  if (FAILED(hr = IMoniker_Load(m, s))) { IMoniker_Release(m); return hr; }
  *out = m;
  return S_OK;
}
static HRESULT mk_save_component(IStream *s, IMoniker *m, BOOL clear) {
  CLSID clsid;
  HRESULT hr = IMoniker_GetClassID(m, &clsid);
  if (SUCCEEDED(hr)) hr = WriteClassStm(s, &clsid);
  if (SUCCEEDED(hr)) hr = IMoniker_Save(m, s, clear);
  return hr;
}
static HRESULT composite_load(MonikerObj *m, IStream *s) {
  DWORD n;
  HRESULT hr = stream_read_all(s, &n, sizeof(n));
  if (FAILED(hr)) return hr;
  if (n > MK_MAX_COMPONENTS) return E_FAIL;
  if (!mk_depth_enter()) return E_FAIL;
  MkList l = {0};
  for (DWORD i = 0; i < n && SUCCEEDED(hr); i++) {
    IMoniker *c;
    if (SUCCEEDED(hr = mk_load_component(s, &c))) { ml_push_flat(&l, c); IMoniker_Release(c); }
  }
  mk_depth_leave();
  if (SUCCEEDED(hr) && (l.failed || l.n > MK_MAX_COMPONENTS)) hr = E_OUTOFMEMORY;
  if (FAILED(hr)) { ml_free(&l); return hr; }
  IMoniker **old = m->comps;
  ULONG oldn = m->ncomps;
  m->comps = l.v;
  m->ncomps = l.n;
  if (old) mk_release_comps(old, oldn);
  return S_OK;
}

/* the objref moniker's object marshaled into a stream: for another machine as Windows does, else for this process */
static HRESULT objref_marshal_into(IStream *s, IUnknown *obj) {
  ULARGE_INTEGER pos;
  HRESULT hr = stream_tell(s, &pos);
  if (FAILED(hr)) return hr;
  hr = CoMarshalInterface(s, &IID_IUnknown, obj, MSHCTX_DIFFERENTMACHINE, NULL, MSHLFLAGS_NORMAL);
  if (FAILED(hr) && SUCCEEDED(stream_seek_to(s, pos.QuadPart))) hr = CoMarshalInterface(s, &IID_IUnknown, obj, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL);
  return hr;
}
/* the objref moniker's marshaled object (an OBJREF), made once for its display name */
static HRESULT objref_bytes(MonikerObj *m, const BYTE **bytes, ULONG *size) {
  if (!m->obj) return E_UNEXPECTED;
  BYTE *cached = *(BYTE *volatile *)&m->objref;
  if (cached) { *bytes = cached; *size = m->objref_size; return S_OK; }
  IStream *s;
  HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &s);
  if (FAILED(hr)) return hr;
  hr = objref_marshal_into(s, m->obj);
  ULARGE_INTEGER pos;
  BYTE *buf = NULL;
  if (SUCCEEDED(hr)) hr = stream_tell(s, &pos);
  if (SUCCEEDED(hr) && (pos.QuadPart > MK_MAX_BYTES || !pos.QuadPart)) hr = E_FAIL;
  if (SUCCEEDED(hr) && !(buf = mem_alloc((SIZE_T)pos.QuadPart))) hr = E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) hr = stream_seek_to(s, 0);
  if (SUCCEEDED(hr)) hr = stream_read_all(s, buf, (ULONG)pos.QuadPart);
  IStream_Release(s);
  if (FAILED(hr)) { mem_free(buf); return hr; }
  m->objref_size = (ULONG)pos.QuadPart;
  if (InterlockedCompareExchangePointer((void **)&m->objref, buf, NULL) != NULL) {
    mem_free(buf);              /* another thread made it first (its marshal data stays, as with any normal marshal) */
  }
  *bytes = m->objref;
  *size = m->objref_size;
  return S_OK;
}

static HRESULT WINAPI mk_load(IMoniker *me, IStream *s) {
  MonikerObj *m = (MonikerObj *)me;
  if (!s) return E_INVALIDARG;
  switch (m->kind) {
    case MK_FILE: return file_load(m, s);
    case MK_ITEM: {
      WCHAR *delim, *name;
      HRESULT hr = read_ansi_record(s, &delim);
      if (FAILED(hr)) return hr;
      if (FAILED(hr = read_ansi_record(s, &name))) { mem_free(delim); return hr; }
      mem_free(m->delim);
      mem_free(m->name);
      m->delim = delim;
      m->name = name;
      return S_OK;
    }
    case MK_ANTI: {
      DWORD n;
      HRESULT hr = stream_read_all(s, &n, sizeof(n));
      if (FAILED(hr)) return hr;
      if (n > MK_MAX_ANTI) return E_INVALIDARG;
      m->anti = n;
      return S_OK;
    }
    case MK_COMPOSITE: return composite_load(m, s);
    case MK_POINTER: return E_NOTIMPL;
    case MK_OBJREF: {
      IUnknown *obj;
      HRESULT hr = CoUnmarshalInterface(s, &IID_IUnknown, (void **)&obj);
      if (FAILED(hr)) return hr;
      if (m->obj) IUnknown_Release(m->obj);
      m->obj = obj;
      return S_OK;
    }
    default: return class_load(m, s);
  }
}
static HRESULT WINAPI mk_save(IMoniker *me, IStream *s, BOOL clear) {
  MonikerObj *m = (MonikerObj *)me;
  if (!s) return E_INVALIDARG;
  switch (m->kind) {
    case MK_FILE: return file_save(m, s);
    case MK_ITEM: {
      HRESULT hr = write_ansi_record(s, m->delim);
      if (SUCCEEDED(hr)) hr = write_ansi_record(s, nz(m->name));
      return hr;
    }
    case MK_ANTI: return stream_write_all(s, &m->anti, sizeof(DWORD));
    case MK_COMPOSITE: {
      if (!m->ncomps) return E_OUTOFMEMORY;           /* (what Windows answers for an empty composite) */
      DWORD n = m->ncomps;
      HRESULT hr = stream_write_all(s, &n, sizeof(n));
      for (ULONG i = 0; i < m->ncomps && SUCCEEDED(hr); i++) hr = mk_save_component(s, m->comps[i], clear);
      return hr;
    }
    case MK_POINTER: return E_NOTIMPL;
    case MK_OBJREF:
      if (!m->obj) return E_UNEXPECTED;
      return objref_marshal_into(s, m->obj);
    default: return class_save(m, s);
  }
}
static HRESULT WINAPI mk_getsizemax(IMoniker *me, ULARGE_INTEGER *size) {
  MonikerObj *m = (MonikerObj *)me;
  if (!size) return E_POINTER;
  size->QuadPart = 0;
  switch (m->kind) {
    case MK_FILE: size->QuadPart = file_save_size(m); return S_OK;
    case MK_ITEM:
      size->QuadPart = 26 + (m->delim ? ((ULONGLONG)wlen(m->delim) + 1) * 4 : 0) + ((ULONGLONG)wlen(nz(m->name)) + 1) * 4;
      return S_OK;
    case MK_ANTI: size->QuadPart = sizeof(CLSID) + sizeof(DWORD); return S_OK;
    case MK_COMPOSITE: {
      if (!m->ncomps) return E_UNEXPECTED;
      ULONGLONG total = sizeof(CLSID) + sizeof(DWORD);
      for (ULONG i = 0; i < m->ncomps; i++) {
        ULARGE_INTEGER one;
        HRESULT hr = IMoniker_GetSizeMax(m->comps[i], &one);
        if (FAILED(hr)) return hr;
        total += sizeof(CLSID) + one.QuadPart;
      }
      size->QuadPart = total;
      return S_OK;
    }
    case MK_POINTER: return E_NOTIMPL;
    case MK_OBJREF: {
      ULONG n;
      if (!m->obj) return E_UNEXPECTED;
      HRESULT hr = CoGetMarshalSizeMax(&n, &IID_IUnknown, m->obj, MSHCTX_DIFFERENTMACHINE, NULL, MSHLFLAGS_NORMAL);
      if (FAILED(hr)) hr = CoGetMarshalSizeMax(&n, &IID_IUnknown, m->obj, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL);
      if (SUCCEEDED(hr)) size->QuadPart = n;
      return hr;
    }
    default: size->QuadPart = sizeof(CLSID) + sizeof(DWORD) + m->extra_bytes; return S_OK;
  }
}

/* ---- paths of file monikers ------------------------------------------------------------------------------------------------------------------------------------------- */

/* a path as Windows' file moniker splits it: every backslash is a token of its own, the text between them another */
typedef struct { const WCHAR *s; SIZE_T n; } PathTok;
static PathTok *path_tokens(const WCHAR *p, ULONG *count) {
  SIZE_T len = wlen(p);
  *count = 0;
  if (len > MK_MAX_BYTES) return NULL;
  PathTok *t = mem_alloc((len + 1) * sizeof(PathTok));
  if (!t) return NULL;
  ULONG n = 0;
  for (SIZE_T i = 0; i < len;) {
    SIZE_T j = i + 1;
    if (p[i] != '\\') while (j < len && p[j] != '\\') j++;
    t[n].s = p + i;
    t[n].n = j - i;
    n++;
    i = j;
  }
  *count = n;
  return t;
}
static BOOL tok_is(const PathTok *t, const char *lit) {
  SIZE_T i = 0;
  for (; lit[i]; i++) if (i >= t->n || t->s[i] != (WCHAR)(BYTE)lit[i]) return FALSE;
  return i == t->n;
}
static BOOL tok_ieq(const PathTok *a, const PathTok *b) { return a->n == b->n && !wnicmp(a->s, b->s, a->n); }

/* the path a new file moniker keeps: a relative path made only of ".." steps gets a final backslash, a doubled backslash in it loses one */
static WCHAR *file_normalize(const WCHAR *path) {
  ULONG nb;
  PathTok *t = path_tokens(path, &nb);
  if (!t) return NULL;
  BOOL add = FALSE;
  ULONG skip = (ULONG)-1;
  if (nb && tok_is(&t[0], "..")) {
    add = TRUE;
    for (ULONG i = 0; i < nb; i++) {
      if (!tok_is(&t[i], "..") && !tok_is(&t[i], "\\")) { add = FALSE; break; }
      if (tok_is(&t[i], "\\") && i + 1 < nb && tok_is(&t[i + 1], "\\")) { skip = i; add = FALSE; break; }
    }
  }
  if (nb && tok_is(&t[nb - 1], "\\")) add = FALSE;
  WBuf b = {0};
  for (ULONG i = 0; i < nb; i++) if (i != skip) wb_add_n(&b, t[i].s, t[i].n);
  if (add) wb_add_ascii(&b, "\\");
  mem_free(t);
  return wb_finish_heap(&b);
}
static HRESULT mk_create_file(const WCHAR *path, IMoniker **out) {
  *out = NULL;
  MonikerObj *m = mk_new(MK_FILE);
  if (!m) return E_OUTOFMEMORY;
  if (!(m->path = file_normalize(path))) { mem_free(m); return E_OUTOFMEMORY; }
  *out = &m->iface;
  return S_OK;
}
/* the path of any file moniker (for one of this module without a call); the caller frees *own */
static HRESULT file_path_of(IMoniker *mk, const WCHAR **path, WCHAR **own) {
  MonikerObj *m = mk_impl(mk);
  *own = NULL;
  if (m && m->kind == MK_FILE) { *path = nz(m->path); return S_OK; }
  IBindCtx *bc;
  HRESULT hr = CreateBindCtx(0, &bc);
  if (FAILED(hr)) return hr;
  hr = IMoniker_GetDisplayName(mk, bc, NULL, own);
  IBindCtx_Release(bc);
  if (FAILED(hr)) return hr;
  *path = *own;
  return S_OK;
}

/* file + file: the right path appended to the left one, each leading ".." of the right one removing the last part of the left one */
static HRESULT file_compose(MonikerObj *m, IMoniker *right, IMoniker **out) {
  const WCHAR *rp;
  WCHAR *own;
  HRESULT hr = file_path_of(right, &rp, &own);
  if (FAILED(hr)) return hr;
  const WCHAR *lp = nz(m->path), *rest = rp;
  SIZE_T base = wlen(lp);
  while (rest[0] == '.' && rest[1] == '.' && (rest[2] == '\\' || !rest[2])) {
    SIZE_T e = base;
    if (e && lp[e - 1] == '\\') e--;
    if (!e) break;
    SIZE_T st = e;
    while (st && lp[st - 1] != '\\') st--;
    if (e - st == 2 && (lp[st + 1] == ':' || (lp[st] == '.' && lp[st + 1] == '.'))) break;     /* a drive or a ".." stays */
    base = st;
    rest += rest[2] ? 3 : 2;
  }
  WBuf b = {0};
  wb_add_n(&b, lp, base);
  if (base && lp[base - 1] != '\\' && rest[0] && rest[0] != '\\') wb_add_ascii(&b, "\\");
  wb_add(&b, rest);
  CoTaskMemFree(own);
  WCHAR *joined = wb_finish_heap(&b);
  if (!joined) return E_OUTOFMEMORY;
  hr = mk_create_file(joined, out);
  mem_free(joined);
  return hr;
}

/* ---- composition ----------------------------------------------------------------------------------------------------------------------------------------------------- */

static DWORD mk_kind_sys(int kind) {
  switch (kind) {
    case MK_FILE: return MKSYS_FILEMONIKER;
    case MK_ITEM: return MKSYS_ITEMMONIKER;
    case MK_ANTI: return MKSYS_ANTIMONIKER;
    case MK_COMPOSITE: return MKSYS_GENERICCOMPOSITE;
    case MK_POINTER: return MKSYS_POINTERMONIKER;
    case MK_OBJREF: return MKSYS_OBJREFMONIKER;
    default: return MKSYS_CLASSMONIKER;
  }
}
static DWORD mk_sys_of(IMoniker *mk) {
  MonikerObj *m = mk_impl(mk);
  DWORD sys = MKSYS_NONE;
  if (m) return mk_kind_sys(m->kind);
  if (FAILED(IMoniker_IsSystemMoniker(mk, &sys))) sys = MKSYS_NONE;
  return sys;
}

/* left + right through the generic composite rules: the rightmost part of the left side is composed (non-generically) with the leftmost part of the right side while
 * that succeeds, the result standing in for the right part; what is left is joined into a composite */
static HRESULT mk_compose(IMoniker *left, IMoniker *right, IMoniker **out) {
  MkList L = {0}, R = {0};
  *out = NULL;
  ml_push_flat(&L, left);
  ml_push_flat(&R, right);
  HRESULT hr = (L.failed || R.failed) ? E_OUTOFMEMORY : S_OK;
  ULONG r0 = 0;
  while (SUCCEEDED(hr) && L.n && r0 < R.n) {
    IMoniker *c = NULL;
    hr = IMoniker_ComposeWith(L.v[L.n - 1], R.v[r0], TRUE, &c);
    if (hr == MK_E_NEEDGENERIC) { if (c) IMoniker_Release(c); hr = S_OK; break; }
    if (FAILED(hr)) break;
    L.n--;
    IMoniker_Release(L.v[L.n]);
    L.v[L.n] = NULL;
    IMoniker_Release(R.v[r0]);
    R.v[r0] = NULL;
    MonikerObj *cc = mk_impl(c);
    if (!c) r0++;
    else if (cc && cc->kind == MK_COMPOSITE) {
      MkList N = {0};
      ml_push_flat(&N, c);
      IMoniker_Release(c);
      for (ULONG i = r0 + 1; i < R.n; i++) { ml_push(&N, R.v[i]); R.v[i] = NULL; }
      ml_free(&R);
      R = N;
      r0 = 0;
      if (R.failed) hr = E_OUTOFMEMORY;
    } else R.v[r0] = c;
  }
  if (SUCCEEDED(hr)) {
    for (ULONG i = r0; i < R.n; i++) { ml_push(&L, R.v[i]); R.v[i] = NULL; }
    hr = L.failed ? E_OUTOFMEMORY : mk_from_parts(L.v, L.n, out);
  }
  ml_free(&L);
  ml_free(&R);
  return hr;
}

EXPORT HRESULT WINAPI CreateGenericComposite(LPMONIKER left, LPMONIKER right, LPMONIKER *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!left && !right) return S_OK;
  if (!left || !right) { *out = left ? left : right; IMoniker_AddRef(*out); return S_OK; }
  return mk_compose(left, right, out);
}

/* the composition of a moniker that is not a composite with another one */
static HRESULT mk_compose_simple(MonikerObj *m, IMoniker *right, BOOL only_if_not_generic, IMoniker **out) {
  if (!right) return E_INVALIDARG;
  MonikerObj *r = mk_impl(right);
  DWORD sys = mk_sys_of(right);
  if (m->kind != MK_ANTI && sys == MKSYS_ANTIMONIKER) {
    DWORD count = r ? r->anti : 1;
    if (m->kind == MK_CLASS || count <= 1) return S_OK;       /* the anti moniker cancels this one */
    return mk_create_anti(count - 1, out);
  }
  if (m->kind == MK_FILE && sys == MKSYS_FILEMONIKER) return file_compose(m, right, out);
  if (only_if_not_generic) return MK_E_NEEDGENERIC;
  return CreateGenericComposite(&m->iface, right, out);
}

/* ---- the enumerator of monikers ---------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct MonikerEnum {
  IEnumMoniker iface;
  LONG refs;
  IMoniker **items;
  ULONG count, pos;
  BOOL can_clone;
} MonikerEnum;
static const IEnumMonikerVtbl g_mkenum_vtbl;

/* an enumerator of the n monikers in v (it takes the references and the array) */
static HRESULT mkenum_create(IMoniker **v, ULONG n, BOOL can_clone, IEnumMoniker **out) {
  MonikerEnum *e = mem_alloc(sizeof(*e));
  *out = NULL;
  if (!e) { mk_release_comps(v, n); return E_OUTOFMEMORY; }
  e->iface.lpVtbl = &g_mkenum_vtbl;
  e->refs = 1;
  e->items = v;
  e->count = n;
  e->can_clone = can_clone;
  *out = &e->iface;
  return S_OK;
}
static HRESULT WINAPI mkenum_qi(IEnumMoniker *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumMoniker)) { *out = me; IEnumMoniker_AddRef(me); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI mkenum_addref(IEnumMoniker *me) { return (ULONG)InterlockedIncrement(&((MonikerEnum *)me)->refs); }
static ULONG WINAPI mkenum_release(IEnumMoniker *me) {
  MonikerEnum *e = (MonikerEnum *)me;
  LONG r = InterlockedDecrement(&e->refs);
  if (!r) { mk_release_comps(e->items, e->count); mem_free(e); }
  return (ULONG)r;
}
static HRESULT WINAPI mkenum_next(IEnumMoniker *me, ULONG celt, IMoniker **out, ULONG *fetched) {
  MonikerEnum *e = (MonikerEnum *)me;
  if (!out) return E_INVALIDARG;
  if (!celt) { *out = NULL; if (fetched) *fetched = 0; return S_OK; }
  if (celt > 1 && !fetched) return E_INVALIDARG;
  ULONG got = 0;
  while (got < celt && e->pos < e->count) {
    out[got] = e->items[e->pos++];
    IMoniker_AddRef(out[got]);
    got++;
  }
  if (fetched) *fetched = got;
  return got == celt ? S_OK : S_FALSE;
}
static HRESULT WINAPI mkenum_skip(IEnumMoniker *me, ULONG celt) {
  MonikerEnum *e = (MonikerEnum *)me;
  if (celt > e->count - e->pos) { e->pos = e->count; return S_FALSE; }
  e->pos += celt;
  return S_OK;
}
static HRESULT WINAPI mkenum_reset(IEnumMoniker *me) { ((MonikerEnum *)me)->pos = 0; return S_OK; }
static HRESULT WINAPI mkenum_clone(IEnumMoniker *me, IEnumMoniker **out) {
  MonikerEnum *e = (MonikerEnum *)me;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!e->can_clone) return E_NOTIMPL;           /* (the enumerator of a composite cannot be cloned, as on Windows) */
  IMoniker **copy = mem_alloc(((SIZE_T)e->count + 1) * sizeof(*copy));
  if (!copy) return E_OUTOFMEMORY;
  for (ULONG i = 0; i < e->count; i++) { copy[i] = e->items[i]; IMoniker_AddRef(copy[i]); }
  HRESULT hr = mkenum_create(copy, e->count, TRUE, out);
  if (SUCCEEDED(hr)) ((MonikerEnum *)*out)->pos = e->pos;
  return hr;
}
static const IEnumMonikerVtbl g_mkenum_vtbl = {mkenum_qi, mkenum_addref, mkenum_release, mkenum_next, mkenum_skip, mkenum_reset, mkenum_clone};

/* ---- IMoniker --------------------------------------------------------------------------------------------------------------------------------------------------------- */

static DWORD mk_bind_speed(IBindCtx *pbc) {
  BIND_OPTS o;
  o.cbStruct = sizeof(o);
  if (!pbc || FAILED(IBindCtx_GetBindOptions(pbc, &o)) || !o.dwTickCountDeadline) return BINDSPEED_INDEFINITE;
  return o.dwTickCountDeadline < 2500 ? BINDSPEED_IMMEDIATE : BINDSPEED_MODERATE;
}
static void mk_bind_options(IBindCtx *pbc, BIND_OPTS2 *o) {
  mem_zero(o, sizeof(*o));
  o->cbStruct = sizeof(*o);
  if (!pbc || FAILED(IBindCtx_GetBindOptions(pbc, (BIND_OPTS *)o))) {
    o->grfMode = STGM_READWRITE;
    o->dwClassContext = CLSCTX_SERVER;
  }
}
static HRESULT mk_rot_of(IBindCtx *pbc, IRunningObjectTable **rot) {
  *rot = NULL;
  return pbc ? IBindCtx_GetRunningObjectTable(pbc, rot) : GetRunningObjectTable(0, rot);
}

/* the item container that the moniker to the left binds to */
static HRESULT item_container(IBindCtx *pbc, IMoniker *left, IOleItemContainer **out) {
  *out = NULL;
  return IMoniker_BindToObject(left, pbc, NULL, &IID_IOleItemContainer, (void **)out);
}

static HRESULT file_bind_object(MonikerObj *m, IBindCtx *pbc, IMoniker *left, REFIID riid, void **out) {
  BIND_OPTS2 o;
  IUnknown *obj = NULL;
  IPersistFile *pf = NULL;
  HRESULT hr;
  mk_bind_options(pbc, &o);
  if (!left) {
    IRunningObjectTable *rot;
    if (FAILED(hr = mk_rot_of(pbc, &rot))) return hr;
    hr = IRunningObjectTable_GetObject(rot, &m->iface, &obj);
    IRunningObjectTable_Release(rot);
    if (hr != S_OK) {
      CLSID clsid;
      obj = NULL;
      if (FAILED(hr = GetClassFile(nz(m->path), &clsid))) return hr;
      if (FAILED(hr = CoCreateInstance(&clsid, NULL, o.dwClassContext ? o.dwClassContext : CLSCTX_SERVER, &IID_IPersistFile, (void **)&pf))) return hr;
    }
  } else {
    IClassFactory *cf = NULL;
    hr = IMoniker_BindToObject(left, pbc, NULL, &IID_IClassFactory, (void **)&cf);
    if (FAILED(hr)) return hr == E_NOINTERFACE ? MK_E_INTERMEDIATEINTERFACENOTSUPPORTED : hr;
    hr = IClassFactory_CreateInstance(cf, NULL, &IID_IPersistFile, (void **)&pf);
    IClassFactory_Release(cf);
    if (FAILED(hr)) return hr;
  }
  if (pf) {
    hr = IPersistFile_Load(pf, nz(m->path), o.grfMode & 0x70 ? o.grfMode : STGM_READ);
    if (FAILED(hr)) { IPersistFile_Release(pf); return hr; }
    obj = (IUnknown *)pf;
  }
  hr = IUnknown_QueryInterface(obj, riid, out);
  IUnknown_Release(obj);
  if (SUCCEEDED(hr) && pbc) IBindCtx_RegisterObjectBound(pbc, (IUnknown *)*out);
  return hr;
}

static HRESULT WINAPI mk_bind_object(IMoniker *me, IBindCtx *pbc, IMoniker *left, REFIID riid, void **out) {
  MonikerObj *m = (MonikerObj *)me;
  HRESULT hr;
  if (!out) return E_POINTER;
  *out = NULL;
  switch (m->kind) {
    case MK_FILE:
      if (!pbc) return E_INVALIDARG;
      return file_bind_object(m, pbc, left, riid, out);
    case MK_ITEM: {
      IOleItemContainer *c;
      if (!left) return E_INVALIDARG;
      if (FAILED(hr = item_container(pbc, left, &c))) return hr;
      hr = IOleItemContainer_GetObject(c, (LPOLESTR)nz(m->name), mk_bind_speed(pbc), pbc, riid, out);
      IOleItemContainer_Release(c);
      return hr;
    }
    case MK_ANTI: return E_NOTIMPL;
    case MK_COMPOSITE: {
      IMoniker *rest, *whole = NULL;
      if (!pbc) return E_INVALIDARG;
      if (!m->ncomps) return E_UNEXPECTED;
      if (!left) {
        IRunningObjectTable *rot;
        IUnknown *obj = NULL;
        if (SUCCEEDED(mk_rot_of(pbc, &rot))) {
          hr = IRunningObjectTable_GetObject(rot, me, &obj);
          IRunningObjectTable_Release(rot);
          if (hr == S_OK && obj) { hr = IUnknown_QueryInterface(obj, riid, out); IUnknown_Release(obj); return hr; }
        }
      }
      if (FAILED(hr = mk_prefix_of(m, m->ncomps - 1, &rest))) return hr;
      if (left && FAILED(hr = CreateGenericComposite(left, rest, &whole))) { if (rest) IMoniker_Release(rest); return hr; }
      hr = IMoniker_BindToObject(m->comps[m->ncomps - 1], pbc, left ? whole : rest, riid, out);
      if (whole) IMoniker_Release(whole);
      if (rest) IMoniker_Release(rest);
      return hr;
    }
    case MK_POINTER:
    case MK_OBJREF:
      if (!m->obj) return E_UNEXPECTED;
      return IUnknown_QueryInterface(m->obj, riid, out);
    default: {
      BIND_OPTS2 o;
      mk_bind_options(pbc, &o);
      if (!left) return CoGetClassObject(&m->clsid, o.dwClassContext ? o.dwClassContext : CLSCTX_SERVER, o.pServerInfo, riid, out);
      IClassActivator *act;
      if (FAILED(hr = IMoniker_BindToObject(left, pbc, NULL, &IID_IClassActivator, (void **)&act))) return hr;
      hr = IClassActivator_GetClassObject(act, &m->clsid, o.dwClassContext ? o.dwClassContext : CLSCTX_SERVER, o.locale, riid, out);
      IClassActivator_Release(act);
      return hr;
    }
  }
}

static HRESULT WINAPI mk_bind_storage(IMoniker *me, IBindCtx *pbc, IMoniker *left, REFIID riid, void **out) {
  MonikerObj *m = (MonikerObj *)me;
  HRESULT hr;
  if (!out) return E_POINTER;
  *out = NULL;
  switch (m->kind) {
    case MK_FILE: {
      if (!pbc) return E_INVALIDARG;
      if (left) return E_NOTIMPL;
      if (IsEqualGUID(riid, &IID_IStorage)) {
        BIND_OPTS2 o;
        IStorage *stg;
        mk_bind_options(pbc, &o);
        hr = StgIsStorageFile(nz(m->path));
        if (hr != S_OK) return hr;
        if (FAILED(hr = StgOpenStorage(nz(m->path), NULL, o.grfMode, NULL, 0, &stg))) return hr;
        *out = stg;
        return hr;
      }
      if (IsEqualGUID(riid, &IID_IStream) || IsEqualGUID(riid, &IID_ILockBytes)) return E_FAIL;
      return E_NOINTERFACE;
    }
    case MK_ITEM: {
      IOleItemContainer *c;
      if (!left) return E_INVALIDARG;
      if (FAILED(hr = item_container(pbc, left, &c))) return hr;
      hr = IOleItemContainer_GetObjectStorage(c, (LPOLESTR)nz(m->name), pbc, riid, out);
      IOleItemContainer_Release(c);
      return hr;
    }
    case MK_ANTI: return E_NOTIMPL;
    case MK_COMPOSITE: {
      IMoniker *rest, *whole = NULL;
      if (!pbc) return E_INVALIDARG;
      if (!m->ncomps) return E_UNEXPECTED;
      if (FAILED(hr = mk_prefix_of(m, m->ncomps - 1, &rest))) return hr;
      if (left && FAILED(hr = CreateGenericComposite(left, rest, &whole))) { if (rest) IMoniker_Release(rest); return hr; }
      hr = IMoniker_BindToStorage(m->comps[m->ncomps - 1], pbc, left ? whole : rest, riid, out);
      if (whole) IMoniker_Release(whole);
      if (rest) IMoniker_Release(rest);
      return hr;
    }
    case MK_POINTER:
    case MK_OBJREF:
      if (!m->obj) return E_UNEXPECTED;
      return IUnknown_QueryInterface(m->obj, riid, out);
    default: return mk_bind_object(me, pbc, left, riid, out);
  }
}

static HRESULT WINAPI mk_reduce(IMoniker *me, IBindCtx *pbc, DWORD how, IMoniker **left, IMoniker **reduced) {
  MonikerObj *m = (MonikerObj *)me;
  (void)left;
  if (m->kind == MK_FILE || m->kind == MK_COMPOSITE) { if (!pbc || !reduced) return E_INVALIDARG; }
  else if (!reduced) return E_INVALIDARG;
  *reduced = NULL;
  if (m->kind == MK_COMPOSITE && m->ncomps) {
    MkList l = {0};
    BOOL changed = FALSE;
    for (ULONG i = 0; i < m->ncomps; i++) {
      IMoniker *r = NULL;
      HRESULT hr = IMoniker_Reduce(m->comps[i], pbc, how, NULL, &r);
      if (FAILED(hr) || !r) { r = m->comps[i]; IMoniker_AddRef(r); }
      else if (r != m->comps[i]) changed = TRUE;
      ml_push(&l, r);
    }
    if (l.failed) { ml_free(&l); return E_OUTOFMEMORY; }
    if (changed) {
      IMoniker *acc = NULL;
      HRESULT hr = S_OK;
      for (ULONG i = 0; i < l.n && SUCCEEDED(hr); i++) {
        IMoniker *next = NULL;
        hr = CreateGenericComposite(acc, l.v[i], &next);
        if (acc) IMoniker_Release(acc);
        acc = next;
      }
      ml_free(&l);
      if (FAILED(hr)) { if (acc) IMoniker_Release(acc); return hr; }
      *reduced = acc;
      return S_OK;
    }
    ml_free(&l);
  }
  *reduced = me;
  IMoniker_AddRef(me);
  return MK_S_REDUCED_TO_SELF;
}

static HRESULT WINAPI mk_compose_with(IMoniker *me, IMoniker *right, BOOL only_if_not_generic, IMoniker **out) {
  MonikerObj *m = (MonikerObj *)me;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (m->kind == MK_COMPOSITE) {
    if (!right) {
      if (only_if_not_generic) return MK_E_NEEDGENERIC;
      *out = me;
      IMoniker_AddRef(me);
      return S_OK;
    }
    if (only_if_not_generic) return MK_E_NEEDGENERIC;
    return CreateGenericComposite(me, right, out);
  }
  if (m->kind == MK_ANTI) {
    if (!right) return E_INVALIDARG;
    if (only_if_not_generic) return MK_E_NEEDGENERIC;
    return CreateGenericComposite(me, right, out);
  }
  return mk_compose_simple(m, right, only_if_not_generic, out);
}

static HRESULT WINAPI mk_enum(IMoniker *me, BOOL forward, IEnumMoniker **out) {
  MonikerObj *m = (MonikerObj *)me;
  if (m->kind == MK_POINTER) { if (out) *out = NULL; return E_NOTIMPL; }
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (m->kind != MK_COMPOSITE) return S_OK;
  if (!m->ncomps) return E_UNEXPECTED;
  IMoniker **v = mem_alloc((SIZE_T)m->ncomps * sizeof(*v));
  if (!v) return E_OUTOFMEMORY;
  for (ULONG i = 0; i < m->ncomps; i++) {
    v[i] = m->comps[forward ? i : m->ncomps - 1 - i];
    IMoniker_AddRef(v[i]);
  }
  return mkenum_create(v, m->ncomps, FALSE, out);
}

static HRESULT WINAPI mk_isequal(IMoniker *me, IMoniker *other) {
  MonikerObj *m = (MonikerObj *)me, *o = mk_impl(other);
  if (!other) return E_INVALIDARG;
  if (other == me) return S_OK;
  if (!o || o->kind != m->kind) return S_FALSE;
  switch (m->kind) {
    case MK_FILE: return !wicmp(nz(m->path), nz(o->path)) ? S_OK : S_FALSE;
    case MK_ITEM: return !wicmp(nz(m->name), nz(o->name)) ? S_OK : S_FALSE;
    case MK_ANTI: return m->anti == o->anti ? S_OK : S_FALSE;
    case MK_COMPOSITE:
      if (!m->ncomps || m->ncomps != o->ncomps) return S_FALSE;
      for (ULONG i = 0; i < m->ncomps; i++) if (IMoniker_IsEqual(m->comps[i], o->comps[i]) != S_OK) return S_FALSE;
      return S_OK;
    case MK_POINTER:
    case MK_OBJREF: return m->obj == o->obj ? S_OK : S_FALSE;
    default: return IsEqualGUID(&m->clsid, &o->clsid) ? S_OK : S_FALSE;
  }
}

static HRESULT WINAPI mk_hash(IMoniker *me, DWORD *out) {
  MonikerObj *m = (MonikerObj *)me;
  DWORD h = 0;
  if (!out) return E_POINTER;
  switch (m->kind) {
    case MK_FILE: {
      const WCHAR *p = nz(m->path);
      SIZE_T len = wlen(p);
      /* case-insensitive (like IsEqual): characters are folded through upper case, ASCII letters hash as lower case like Windows */
#define MK_FOLD(c) ((DWORD)(wup(c) >= 'A' && wup(c) <= 'Z' ? wup(c) + 32 : wup(c)))
      if (len < 16) for (SIZE_T i = 0; i < len; i++) h = h * 37 + MK_FOLD(p[i]);
      else { SIZE_T skip = len / 8; for (SIZE_T i = 0; i < len; i += skip) h = h * 39 + MK_FOLD(p[i]); }
#undef MK_FOLD
      break;
    }
    case MK_ITEM: for (const WCHAR *p = nz(m->name); *p; p++) h = (h * 3) ^ wup(*p); break;
    case MK_ANTI: h = 0x80000000u | m->anti; break;
    case MK_COMPOSITE:
      if (!m->ncomps) return E_UNEXPECTED;
      for (ULONG i = 0; i < m->ncomps; i++) {
        DWORD one;
        HRESULT hr = IMoniker_Hash(m->comps[i], &one);
        if (FAILED(hr)) return hr;
        h ^= one;
      }
      break;
    case MK_POINTER:
    case MK_OBJREF: h = PtrToUlong(m->obj); break;
    default: h = m->clsid.Data1; break;
  }
  *out = h;
  return S_OK;
}

static HRESULT WINAPI mk_isrunning(IMoniker *me, IBindCtx *pbc, IMoniker *left, IMoniker *newly) {
  MonikerObj *m = (MonikerObj *)me;
  IRunningObjectTable *rot;
  HRESULT hr;
  switch (m->kind) {
    case MK_FILE:
      if (newly && (newly == me || IMoniker_IsEqual(newly, me) == S_OK)) return S_OK;
      if (!pbc) return E_POINTER;
      if (FAILED(hr = IBindCtx_GetRunningObjectTable(pbc, &rot))) return hr;
      hr = IRunningObjectTable_IsRunning(rot, me);
      IRunningObjectTable_Release(rot);
      return hr;
    case MK_ITEM:
      if (!pbc) return E_INVALIDARG;
      if (!left) {
        if (newly) return IMoniker_IsEqual(me, newly) == S_OK ? S_OK : S_FALSE;
        if (FAILED(hr = IBindCtx_GetRunningObjectTable(pbc, &rot))) return hr;
        hr = IRunningObjectTable_IsRunning(rot, me);
        IRunningObjectTable_Release(rot);
        return hr;
      } else {
        IOleItemContainer *c;
        if ((hr = IMoniker_IsRunning(left, pbc, NULL, NULL)) != S_OK) return hr;      /* the container must run */
        if (FAILED(hr = item_container(pbc, left, &c))) return hr;
        hr = IOleItemContainer_IsRunning(c, (LPOLESTR)nz(m->name));
        IOleItemContainer_Release(c);
        return hr;
      }
    case MK_ANTI: return S_FALSE;
    case MK_COMPOSITE: {
      if (!pbc) return E_INVALIDARG;
      if (!m->ncomps) return E_UNEXPECTED;
      if (left) {
        IMoniker *whole;
        if (FAILED(hr = CreateGenericComposite(left, me, &whole))) return hr;
        if (!whole) return S_FALSE;
        hr = IMoniker_IsRunning(whole, pbc, NULL, newly);
        IMoniker_Release(whole);
        return hr;
      }
      if (newly) return IMoniker_IsEqual(me, newly) == S_OK ? S_OK : S_FALSE;
      if (FAILED(hr = IBindCtx_GetRunningObjectTable(pbc, &rot))) return hr;
      hr = IRunningObjectTable_IsRunning(rot, me);
      IRunningObjectTable_Release(rot);
      if (hr == S_OK) return S_OK;
      IMoniker *rest;
      if (FAILED(hr = mk_prefix_of(m, m->ncomps - 1, &rest))) return hr;
      hr = IMoniker_IsRunning(m->comps[m->ncomps - 1], pbc, rest, NULL);
      if (rest) IMoniker_Release(rest);
      return hr;
    }
    case MK_POINTER:
    case MK_OBJREF: return S_OK;
    default: return E_NOTIMPL;
  }
}

static HRESULT rot_time_of(IBindCtx *pbc, IMoniker *mk, FILETIME *ft) {
  IRunningObjectTable *rot;
  HRESULT hr = mk_rot_of(pbc, &rot);
  if (FAILED(hr)) return hr;
  hr = IRunningObjectTable_GetTimeOfLastChange(rot, mk, ft);
  IRunningObjectTable_Release(rot);
  return hr;
}

static HRESULT WINAPI mk_gettime(IMoniker *me, IBindCtx *pbc, IMoniker *left, FILETIME *ft) {
  MonikerObj *m = (MonikerObj *)me;
  HRESULT hr;
  switch (m->kind) {
    case MK_FILE: {
      WIN32_FILE_ATTRIBUTE_DATA info;
      if (!ft) return E_POINTER;
      if (left || !pbc) return E_INVALIDARG;
      if (rot_time_of(pbc, me, ft) == S_OK) return S_OK;
      if (!GetFileAttributesExW(nz(m->path), GetFileExInfoStandard, &info)) return MK_E_NOOBJECT;
      *ft = info.ftLastWriteTime;
      return S_OK;
    }
    case MK_ITEM: {
      IMoniker *whole;
      if (!pbc || !ft) return E_INVALIDARG;
      if (!left) return MK_E_NOTBINDABLE;
      if (FAILED(hr = CreateGenericComposite(left, me, &whole))) return hr;
      hr = whole ? rot_time_of(pbc, whole, ft) : MK_E_UNAVAILABLE;
      if (whole) IMoniker_Release(whole);
      if (hr == S_OK) return S_OK;
      return IMoniker_GetTimeOfLastChange(left, pbc, NULL, ft);
    }
    case MK_ANTI:
    case MK_POINTER: return E_NOTIMPL;
    case MK_COMPOSITE: {
      IMoniker *whole = me;
      if (!pbc || !ft) return E_INVALIDARG;
      if (!m->ncomps) return E_UNEXPECTED;
      if (left) {
        if (FAILED(hr = CreateGenericComposite(left, me, &whole))) return hr;
        if (!whole) return MK_E_NOTBINDABLE;
      } else IMoniker_AddRef(whole);
      hr = rot_time_of(pbc, whole, ft);
      if (hr != S_OK) {
        MonikerObj *w = mk_impl(whole);
        if (w && w->kind == MK_COMPOSITE && w->ncomps) {
          IMoniker *rest;
          if (SUCCEEDED(hr = mk_prefix_of(w, w->ncomps - 1, &rest))) {
            hr = IMoniker_GetTimeOfLastChange(w->comps[w->ncomps - 1], pbc, rest, ft);
            if (rest) IMoniker_Release(rest);
          }
        } else hr = IMoniker_GetTimeOfLastChange(whole, pbc, NULL, ft);
      }
      IMoniker_Release(whole);
      return hr;
    }
    default: return MK_E_UNAVAILABLE;       /* class and objref monikers */
  }
}

static HRESULT WINAPI mk_inverse(IMoniker *me, IMoniker **out) {
  MonikerObj *m = (MonikerObj *)me;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (m->kind == MK_ANTI) return MK_E_NOINVERSE;
  if (m->kind != MK_COMPOSITE) return mk_create_anti(1, out);
  if (!m->ncomps) return E_UNEXPECTED;
  IMoniker *acc = NULL;
  for (ULONG i = m->ncomps; i-- > 0;) {
    IMoniker *inv, *next = NULL;
    HRESULT hr = IMoniker_Inverse(m->comps[i], &inv);
    if (FAILED(hr)) { if (acc) IMoniker_Release(acc); return hr; }
    hr = CreateGenericComposite(acc, inv, &next);
    if (acc) IMoniker_Release(acc);
    if (inv) IMoniker_Release(inv);
    if (FAILED(hr)) return hr;
    acc = next;
  }
  *out = acc;
  return S_OK;
}

/* the common prefix of monikers of which at least one is a composite (MonikerCommonPrefixWith); for two monikers that are not composites there is none */
static HRESULT mk_common_prefix(IMoniker *a, IMoniker *b, IMoniker **prefix) {
  *prefix = NULL;
  if (!a || !b) return MK_E_NOPREFIX;
  MonikerObj *ca = mk_impl(a), *cb = mk_impl(b);
  BOOL comp_a = ca && ca->kind == MK_COMPOSITE, comp_b = cb && cb->kind == MK_COMPOSITE;
  if (!comp_a && !comp_b) return MK_E_NOPREFIX;
  MkList A = {0}, B = {0};
  ml_push_flat(&A, a);
  ml_push_flat(&B, b);
  HRESULT hr = (A.failed || B.failed || !A.n || !B.n) ? E_OUTOFMEMORY : S_OK;
  if (SUCCEEDED(hr)) {
    ULONG i = 0;
    while (i < A.n && i < B.n && IMoniker_IsEqual(A.v[i], B.v[i]) == S_OK) i++;
    if (!i) {
      if (comp_a && comp_b) hr = S_OK;
      else {
        hr = IMoniker_CommonPrefixWith(A.v[0], B.v[0], prefix);
        /* the result is about the first components only: it is all of a moniker only when that moniker has no other component */
        if (hr == MK_S_US) hr = !comp_a ? MK_S_ME : (!comp_b ? MK_S_HIM : S_OK);
        else if (hr == MK_S_ME && comp_a) hr = S_OK;
        else if (hr == MK_S_HIM && comp_b) hr = S_OK;
      }
    } else if (i == A.n && i == B.n) {
      hr = mk_from_parts(A.v, A.n, prefix);      /* a new moniker equal to both (the references move to it) */
      if (SUCCEEDED(hr)) hr = MK_S_US;
    } else if (i == A.n) { *prefix = a; IMoniker_AddRef(a); hr = MK_S_ME; }
    else if (i == B.n) { *prefix = b; IMoniker_AddRef(b); hr = MK_S_HIM; }
    else {
      hr = mk_from_parts(A.v, i, prefix);
      if (SUCCEEDED(hr)) hr = S_OK;
    }
  }
  ml_free(&A);
  ml_free(&B);
  return hr;
}

static HRESULT WINAPI mk_common_prefix_with(IMoniker *me, IMoniker *other, IMoniker **prefix) {
  MonikerObj *m = (MonikerObj *)me, *o = mk_impl(other);
  HRESULT hr;
  if (!prefix) return E_INVALIDARG;
  *prefix = NULL;
  if (!other) return E_INVALIDARG;
  BOOL other_composite = o ? o->kind == MK_COMPOSITE : mk_sys_of(other) == MKSYS_GENERICCOMPOSITE;
  if (m->kind == MK_COMPOSITE) {
    if (!m->ncomps) return E_UNEXPECTED;
    if (!o || o->kind != MK_COMPOSITE) {
      if (IMoniker_IsEqual(m->comps[0], other) == S_OK) { *prefix = m->comps[0]; IMoniker_AddRef(*prefix); return MK_S_HIM; }
    }
    return mk_common_prefix(me, other, prefix);
  }
  if (other_composite) {
    /* ask the composite and turn its answer around */
    hr = IMoniker_CommonPrefixWith(other, me, prefix);
    if (hr == MK_S_ME) hr = MK_S_HIM;
    else if (hr == MK_S_HIM) hr = MK_S_ME;
    return hr;
  }
  if (m->kind == MK_ANTI && o && o->kind == MK_ANTI) {
    if (m->anti == o->anti) { *prefix = me; IMoniker_AddRef(me); return MK_S_US; }
    if (m->anti < o->anti) { *prefix = me; IMoniker_AddRef(me); return MK_S_ME; }
    *prefix = other;
    IMoniker_AddRef(other);
    return MK_S_HIM;
  }
  if (m->kind == MK_FILE && mk_sys_of(other) == MKSYS_FILEMONIKER) {
    const WCHAR *op;
    WCHAR *own;
    ULONG na, nb;
    if (FAILED(hr = file_path_of(other, &op, &own))) return hr;
    PathTok *ta = path_tokens(nz(m->path), &na), *tb = path_tokens(op, &nb);
    hr = (ta && tb) ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) {
      ULONG i = 0;
      while (i < na && i < nb && tok_ieq(&ta[i], &tb[i])) i++;
      if (!i) hr = MK_E_NOPREFIX;
      else if (i == na && i == nb) { *prefix = me; IMoniker_AddRef(me); hr = MK_S_US; }
      else if (i == na) { *prefix = me; IMoniker_AddRef(me); hr = MK_S_ME; }
      else if (i == nb) { *prefix = other; IMoniker_AddRef(other); hr = MK_S_HIM; }
      else {
        WBuf b = {0};
        for (ULONG k = 0; k < i; k++) wb_add_n(&b, ta[k].s, ta[k].n);
        WCHAR *common = wb_finish_heap(&b);
        hr = common ? mk_create_file(common, prefix) : E_OUTOFMEMORY;
        mem_free(common);
      }
    }
    mem_free(ta);
    mem_free(tb);
    CoTaskMemFree(own);
    return hr;
  }
  if (IMoniker_IsEqual(me, other) == S_OK) { *prefix = me; IMoniker_AddRef(me); return MK_S_US; }
  return MK_E_NOPREFIX;
}

static HRESULT WINAPI mk_relative_path_to(IMoniker *me, IMoniker *other, IMoniker **out) {
  MonikerObj *m = (MonikerObj *)me;
  HRESULT hr;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!other) return E_INVALIDARG;
  switch (m->kind) {
    case MK_ANTI: *out = other; IMoniker_AddRef(other); return MK_S_HIM;
    case MK_POINTER:
    case MK_OBJREF: return E_NOTIMPL;
    case MK_FILE: {
      if (mk_sys_of(other) != MKSYS_FILEMONIKER) { *out = other; IMoniker_AddRef(other); return MK_S_HIM; }
      const WCHAR *op;
      WCHAR *own;
      ULONG na, nb;
      if (FAILED(hr = file_path_of(other, &op, &own))) return hr;
      PathTok *ta = path_tokens(nz(m->path), &na), *tb = path_tokens(op, &nb);
      hr = (ta && tb) ? S_OK : E_OUTOFMEMORY;
      if (SUCCEEDED(hr)) {
        ULONG i = 0;
        while (i < na && i < nb && tok_ieq(&ta[i], &tb[i])) i++;
        while (i && !tok_is(&ta[i - 1], "\\")) i--;            /* back to the last directory both share */
        if (!i) { *out = other; IMoniker_AddRef(other); hr = MK_S_HIM; }
        else {
          WBuf b = {0};
          ULONG words = 0;
          for (ULONG k = i; k < na; k++) if (!tok_is(&ta[k], "\\")) words++;
          for (ULONG k = 1; k < words; k++) wb_add_ascii(&b, "..\\");       /* the last word of the path is the file itself */
          for (ULONG k = i; k < nb; k++) wb_add_n(&b, tb[k].s, tb[k].n);
          WCHAR *rel = wb_finish_heap(&b);
          hr = rel ? mk_create_file(rel, out) : E_OUTOFMEMORY;
          mem_free(rel);
        }
      }
      mem_free(ta);
      mem_free(tb);
      CoTaskMemFree(own);
      return hr;
    }
    case MK_COMPOSITE: {
      if (!m->ncomps) return E_UNEXPECTED;
      MkList B = {0};
      ml_push_flat(&B, other);
      if (B.failed || !B.n) { ml_free(&B); return E_OUTOFMEMORY; }
      ULONG i = 0;
      while (i < m->ncomps && i < B.n && IMoniker_IsEqual(m->comps[i], B.v[i]) == S_OK) i++;
      if (!i) { ml_free(&B); return IMoniker_RelativePathTo(m->comps[0], other, out); }
      /* up out of the rest of this moniker, then down into the rest of the other one */
      IMoniker *up = NULL, *down = NULL;
      hr = S_OK;
      if (i < m->ncomps) {
        IMoniker **tail = m->comps + i;
        ULONG n = m->ncomps - i;
        for (ULONG k = n; k-- > 0 && SUCCEEDED(hr);) {
          IMoniker *inv, *next = NULL;
          if (FAILED(hr = IMoniker_Inverse(tail[k], &inv))) break;
          hr = CreateGenericComposite(up, inv, &next);
          if (up) IMoniker_Release(up);
          if (inv) IMoniker_Release(inv);
          up = next;
        }
      }
      if (SUCCEEDED(hr)) hr = mk_from_parts(B.v + i, B.n - i, &down);
      if (SUCCEEDED(hr)) hr = CreateGenericComposite(up, down, out);
      if (up) IMoniker_Release(up);
      if (down) IMoniker_Release(down);
      ml_free(&B);
      return FAILED(hr) ? hr : S_OK;
    }
    default: return MK_E_NOTBINDABLE;       /* item and class monikers */
  }
}

static HRESULT WINAPI mk_display_name(IMoniker *me, IBindCtx *pbc, IMoniker *left, LPOLESTR *out) {
  MonikerObj *m = (MonikerObj *)me;
  WBuf b = {0};
  WCHAR text[40];
  (void)left;
  if (m->kind == MK_POINTER || m->kind == MK_COMPOSITE) {
    if (out) *out = NULL;
    if (!pbc || !out) return E_INVALIDARG;
  }
  if (!out) return E_POINTER;
  *out = NULL;
  switch (m->kind) {
    case MK_FILE: wb_add(&b, nz(m->path)); break;
    case MK_ITEM: if (m->delim) wb_add(&b, m->delim); wb_add(&b, nz(m->name)); break;
    case MK_ANTI: for (DWORD i = 0; i < m->anti && !b.failed; i++) wb_add_ascii(&b, "\\.."); break;
    case MK_COMPOSITE:
      if (!m->ncomps) return E_INVALIDARG;
      for (ULONG i = 0; i < m->ncomps; i++) {
        LPOLESTR part;
        HRESULT hr = IMoniker_GetDisplayName(m->comps[i], pbc, NULL, &part);
        if (FAILED(hr)) { mem_free(b.p); return hr; }
        wb_add(&b, nz(part));
        CoTaskMemFree(part);
      }
      break;
    case MK_POINTER: return E_NOTIMPL;
    case MK_OBJREF: {
      const BYTE *bytes;
      ULONG size;
      HRESULT hr = objref_bytes(m, &bytes, &size);
      if (FAILED(hr)) return hr;
      wb_add_ascii(&b, "objref:");
      b64_encode(&b, bytes, size);
      wb_add_ascii(&b, ":");
      break;
    }
    default:
      guid_to_text(&m->clsid, text);
      text[37] = 0;
      wb_add_ascii(&b, "clsid:");
      wb_add(&b, text + 1);
      if (m->extra) wb_add(&b, m->extra);
      wb_add_ascii(&b, ":");
      break;
  }
  return wb_finish_task(&b, out);
}

/* binds the moniker to its object's IParseDisplayName and lets that parse the rest of a display name */
static HRESULT mk_parse_through_object(IMoniker *me, IBindCtx *pbc, IMoniker *left, LPOLESTR name, ULONG *eaten, IMoniker **out) {
  IParseDisplayName *pdn;
  HRESULT hr = IMoniker_BindToObject(me, pbc, left, &IID_IParseDisplayName, (void **)&pdn);
  if (FAILED(hr)) return hr;
  hr = IParseDisplayName_ParseDisplayName(pdn, pbc, name, eaten, out);
  IParseDisplayName_Release(pdn);
  return hr;
}

static HRESULT WINAPI mk_parse_display_name(IMoniker *me, IBindCtx *pbc, IMoniker *left, LPOLESTR name, ULONG *eaten, IMoniker **out) {
  MonikerObj *m = (MonikerObj *)me;
  HRESULT hr;
  if (!out || !eaten || !name) return E_INVALIDARG;
  *out = NULL;
  switch (m->kind) {
    case MK_ITEM: {
      IOleItemContainer *c;
      IParseDisplayName *pdn = NULL;
      if (!left) return MK_E_SYNTAX;
      if (FAILED(hr = item_container(pbc, left, &c))) return hr;
      hr = IOleItemContainer_GetObject(c, (LPOLESTR)nz(m->name), mk_bind_speed(pbc), pbc, &IID_IParseDisplayName, (void **)&pdn);
      if (SUCCEEDED(hr) && pdn) {
        hr = IParseDisplayName_ParseDisplayName(pdn, pbc, name, eaten, out);
        IParseDisplayName_Release(pdn);
      }
      IOleItemContainer_Release(c);
      return hr;
    }
    case MK_ANTI: return E_NOTIMPL;
    case MK_COMPOSITE: {
      IMoniker *rest, *whole = NULL;
      if (!m->ncomps) return E_UNEXPECTED;
      if (FAILED(hr = mk_prefix_of(m, m->ncomps - 1, &rest))) return hr;
      if (left && FAILED(hr = CreateGenericComposite(left, rest, &whole))) { if (rest) IMoniker_Release(rest); return hr; }
      hr = IMoniker_ParseDisplayName(m->comps[m->ncomps - 1], pbc, left ? whole : rest, name, eaten, out);
      if (whole) IMoniker_Release(whole);
      if (rest) IMoniker_Release(rest);
      return hr;
    }
    case MK_POINTER:
    case MK_OBJREF: {
      IParseDisplayName *pdn;
      if (left) return MK_E_SYNTAX;
      if (!m->obj) return E_UNEXPECTED;
      if (FAILED(hr = IUnknown_QueryInterface(m->obj, &IID_IParseDisplayName, (void **)&pdn))) return hr;
      hr = IParseDisplayName_ParseDisplayName(pdn, pbc, name, eaten, out);
      IParseDisplayName_Release(pdn);
      return hr;
    }
    default: return mk_parse_through_object(me, pbc, left, name, eaten, out);      /* file and class monikers */
  }
}

static HRESULT WINAPI mk_issystem(IMoniker *me, DWORD *sys) {
  if (!sys) return E_POINTER;
  *sys = mk_kind_sys(((MonikerObj *)me)->kind);
  return S_OK;
}

static const IMonikerVtbl g_mk_vtbl = {mk_qi, mk_addref, mk_release, mk_getclassid, mk_isdirty, mk_load, mk_save, mk_getsizemax, mk_bind_object, mk_bind_storage,
                                       mk_reduce, mk_compose_with, mk_enum, mk_isequal, mk_hash, mk_isrunning, mk_gettime, mk_inverse, mk_common_prefix_with,
                                       mk_relative_path_to, mk_display_name, mk_parse_display_name, mk_issystem};

/* ---- IROTData --------------------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT WINAPI mkrot_qi(IROTData *me, REFIID riid, void **out) { return mk_qi(&mk_from_rot(me)->iface, riid, out); }
static ULONG WINAPI mkrot_addref(IROTData *me) { return mk_addref(&mk_from_rot(me)->iface); }
static ULONG WINAPI mkrot_release(IROTData *me) { return mk_release(&mk_from_rot(me)->iface); }

/* CLSID followed by the upper-case text and its terminator */
static HRESULT rotdata_text(const CLSID *clsid, const WCHAR *a, const WCHAR *b, BYTE *data, ULONG max, ULONG *size) {
  SIZE_T la = wlen(a), lb = wlen(b);
  if (la + lb > MK_MAX_BYTES) return E_OUTOFMEMORY;
  ULONG need = (ULONG)(sizeof(CLSID) + (la + lb + 1) * sizeof(WCHAR));
  *size = need;
  if (!data || max < need) return E_OUTOFMEMORY;
  mem_copy(data, clsid, sizeof(CLSID));
  BYTE *p = data + sizeof(CLSID);
  for (SIZE_T i = 0; i <= la + lb; i++) {
    WCHAR c = i < la ? wup(a[i]) : (i < la + lb ? wup(b[i - la]) : 0);
    mem_copy(p + i * sizeof(WCHAR), &c, sizeof(WCHAR));
  }
  return S_OK;
}

static HRESULT WINAPI mkrot_data(IROTData *me, BYTE *data, ULONG max, ULONG *size) {
  MonikerObj *m = mk_from_rot(me);
  if (!size) return E_POINTER;
  *size = 0;
  switch (m->kind) {
    case MK_FILE: return rotdata_text(&MK_CLSID_File, nz(m->path), g_mk_empty, data, max, size);
    case MK_ITEM: return rotdata_text(&MK_CLSID_Item, nz(m->delim), nz(m->name), data, max, size);
    case MK_ANTI:
      *size = sizeof(CLSID) + sizeof(DWORD);
      if (!data || max < *size) return E_OUTOFMEMORY;
      mem_copy(data, &MK_CLSID_Anti, sizeof(CLSID));
      mem_copy(data + sizeof(CLSID), &m->anti, sizeof(DWORD));
      return S_OK;
    case MK_CLASS:
      *size = 2 * sizeof(CLSID);
      if (!data || max < *size) return E_OUTOFMEMORY;
      mem_copy(data, &MK_CLSID_Class, sizeof(CLSID));
      mem_copy(data + sizeof(CLSID), &m->clsid, sizeof(CLSID));
      return S_OK;
    case MK_COMPOSITE: {
      if (!m->ncomps) return E_UNEXPECTED;
      if (!data || max < sizeof(CLSID)) return E_OUTOFMEMORY;
      mem_copy(data, &MK_CLSID_Composite, sizeof(CLSID));
      ULONG off = sizeof(CLSID);
      for (ULONG i = 0; i < m->ncomps; i++) {
        IROTData *rd;
        ULONG got = 0;
        if (FAILED(IMoniker_QueryInterface(m->comps[i], &IID_IROTData, (void **)&rd))) return E_NOTIMPL;
        HRESULT hr = IROTData_GetComparisonData(rd, data + off, max - off, &got);
        IROTData_Release(rd);
        if (FAILED(hr)) return hr;
        if (got > max - off) return E_OUTOFMEMORY;
        off += got;
      }
      *size = off;
      return S_OK;
    }
    default: return E_NOTIMPL;
  }
}
static const IROTDataVtbl g_mk_rot_vtbl = {mkrot_qi, mkrot_addref, mkrot_release, mkrot_data};

/* ---- IMarshal: monikers travel by value (their saved data); a composite as its two halves, a pointer moniker as its object ----------------------------------------- */

static HRESULT WINAPI mkm_qi(IMarshal *me, REFIID riid, void **out) { return mk_qi(&mk_from_marshal(me)->iface, riid, out); }
static ULONG WINAPI mkm_addref(IMarshal *me) { return mk_addref(&mk_from_marshal(me)->iface); }
static ULONG WINAPI mkm_release(IMarshal *me) { return mk_release(&mk_from_marshal(me)->iface); }
static BOOL mk_by_value(int kind) { return kind == MK_FILE || kind == MK_ITEM || kind == MK_ANTI || kind == MK_CLASS; }

static HRESULT WINAPI mkm_unmarshal_class(IMarshal *me, REFIID riid, void *pv, DWORD ctx, void *pvctx, DWORD flags, CLSID *clsid) {
  (void)riid; (void)pv; (void)ctx; (void)pvctx; (void)flags;
  if (!clsid) return E_POINTER;
  *clsid = *mk_kind_clsid(mk_from_marshal(me)->kind);
  return S_OK;
}
static HRESULT WINAPI mkm_size_max(IMarshal *me, REFIID riid, void *pv, DWORD ctx, void *pvctx, DWORD flags, DWORD *size) {
  MonikerObj *m = mk_from_marshal(me);
  HRESULT hr;
  (void)riid; (void)pv;
  if (!size) return E_POINTER;
  *size = 0;
  if (mk_by_value(m->kind)) {
    ULARGE_INTEGER n;
    if (FAILED(hr = IMoniker_GetSizeMax(&m->iface, &n))) return hr;
    if (n.QuadPart > MAXDWORD) return E_FAIL;
    *size = n.LowPart;
    return S_OK;
  }
  if (m->kind == MK_COMPOSITE) {
    IMoniker *left;
    ULONG a = 0, b = 0;
    if (!m->ncomps) return E_UNEXPECTED;
    if (FAILED(hr = mk_prefix_of(m, m->ncomps - 1, &left))) return hr;
    hr = left ? CoGetMarshalSizeMax(&a, &IID_IMoniker, (IUnknown *)left, ctx, pvctx, flags) : S_OK;
    if (left) IMoniker_Release(left);
    if (SUCCEEDED(hr)) hr = CoGetMarshalSizeMax(&b, &IID_IMoniker, (IUnknown *)m->comps[m->ncomps - 1], ctx, pvctx, flags);
    if (SUCCEEDED(hr) && a > MAXDWORD - b) hr = E_FAIL;
    if (SUCCEEDED(hr)) *size = a + b;
    return hr;
  }
  if (!m->obj) return E_UNEXPECTED;
  ULONG n = 0;
  hr = CoGetMarshalSizeMax(&n, &IID_IUnknown, m->obj, ctx, pvctx, flags);
  if (SUCCEEDED(hr)) *size = n;
  return hr;
}
static HRESULT WINAPI mkm_marshal(IMarshal *me, IStream *s, REFIID riid, void *pv, DWORD ctx, void *pvctx, DWORD flags) {
  MonikerObj *m = mk_from_marshal(me);
  HRESULT hr;
  (void)riid; (void)pv;
  if (mk_by_value(m->kind)) return IMoniker_Save(&m->iface, s, FALSE);
  if (m->kind == MK_COMPOSITE) {
    IMoniker *left;
    if (!m->ncomps) return E_UNEXPECTED;
    if (FAILED(hr = mk_prefix_of(m, m->ncomps - 1, &left))) return hr;
    hr = left ? CoMarshalInterface(s, &IID_IMoniker, (IUnknown *)left, ctx, pvctx, flags) : E_UNEXPECTED;
    if (left) IMoniker_Release(left);
    if (SUCCEEDED(hr)) hr = CoMarshalInterface(s, &IID_IMoniker, (IUnknown *)m->comps[m->ncomps - 1], ctx, pvctx, flags);
    return hr;
  }
  if (!m->obj) return E_UNEXPECTED;
  return CoMarshalInterface(s, &IID_IUnknown, m->obj, ctx, pvctx, flags);
}
static HRESULT WINAPI mkm_unmarshal(IMarshal *me, IStream *s, REFIID riid, void **out) {
  MonikerObj *m = mk_from_marshal(me);
  HRESULT hr;
  if (!out) return E_POINTER;
  *out = NULL;
  if (mk_by_value(m->kind)) hr = IMoniker_Load(&m->iface, s);
  else if (m->kind == MK_COMPOSITE) {
    IMoniker *l = NULL, *r = NULL;
    if (!mk_depth_enter()) return E_FAIL;
    hr = CoUnmarshalInterface(s, &IID_IMoniker, (void **)&l);
    if (SUCCEEDED(hr)) hr = CoUnmarshalInterface(s, &IID_IMoniker, (void **)&r);
    mk_depth_leave();
    if (SUCCEEDED(hr) && (!l || !r)) hr = E_UNEXPECTED;
    if (SUCCEEDED(hr)) {
      MkList list = {0};
      ml_push_flat(&list, l);
      ml_push_flat(&list, r);
      if (list.failed) { ml_free(&list); hr = E_OUTOFMEMORY; }
      else {
        IMoniker **old = m->comps;
        ULONG oldn = m->ncomps;
        m->comps = list.v;
        m->ncomps = list.n;
        if (old) mk_release_comps(old, oldn);
      }
    }
    if (l) IMoniker_Release(l);
    if (r) IMoniker_Release(r);
  } else {
    IUnknown *obj = NULL;
    hr = CoUnmarshalInterface(s, &IID_IUnknown, (void **)&obj);
    if (SUCCEEDED(hr)) {
      if (m->obj) IUnknown_Release(m->obj);
      m->obj = obj;
    }
  }
  if (FAILED(hr)) return hr;
  return mk_qi(&m->iface, riid, out);
}
static HRESULT WINAPI mkm_release_data(IMarshal *me, IStream *s) {
  MonikerObj *m = mk_from_marshal(me);
  if (mk_by_value(m->kind)) {           /* read the data past */
    MonikerObj *tmp = mk_new_default(m->kind);
    if (!tmp) return E_OUTOFMEMORY;
    HRESULT hr = IMoniker_Load(&tmp->iface, s);
    IMoniker_Release(&tmp->iface);
    return hr;
  }
  if (m->kind == MK_COMPOSITE) {
    if (!mk_depth_enter()) return E_FAIL;
    HRESULT hr = CoReleaseMarshalData(s);
    if (SUCCEEDED(hr)) hr = CoReleaseMarshalData(s);
    mk_depth_leave();
    return hr;
  }
  return CoReleaseMarshalData(s);
}
static HRESULT WINAPI mkm_disconnect(IMarshal *me, DWORD reserved) { (void)me; (void)reserved; return S_OK; }
static const IMarshalVtbl g_mk_marshal_vtbl = {mkm_qi, mkm_addref, mkm_release, mkm_unmarshal_class, mkm_size_max, mkm_marshal, mkm_unmarshal, mkm_release_data, mkm_disconnect};

/* ---- creation ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI CreateFileMoniker(LPCOLESTR path, LPMONIKER *out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (!path) return MK_E_SYNTAX;
  if (wlen(path) > MK_MAX_BYTES / sizeof(WCHAR)) return E_OUTOFMEMORY;
  return mk_create_file(path, out);
}
EXPORT HRESULT WINAPI CreateItemMoniker(LPCOLESTR delim, LPCOLESTR item, LPMONIKER *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  MonikerObj *m = mk_new(MK_ITEM);
  if (!m) return E_OUTOFMEMORY;
  m->delim = delim ? heap_wdup_n(delim, wlen(delim)) : NULL;
  m->name = heap_wdup_n(nz(item), wlen(nz(item)));
  if ((delim && !m->delim) || !m->name) { mk_destroy(m); return E_OUTOFMEMORY; }
  *out = &m->iface;
  return S_OK;
}
EXPORT HRESULT WINAPI CreateAntiMoniker(LPMONIKER *out) {
  if (!out) return E_INVALIDARG;
  return mk_create_anti(1, out);
}
static HRESULT mk_create_with_object(int kind, IUnknown *obj, IMoniker **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  MonikerObj *m = mk_new(kind);
  if (!m) return E_OUTOFMEMORY;
  m->obj = obj;
  if (obj) IUnknown_AddRef(obj);
  *out = &m->iface;
  return S_OK;
}
EXPORT HRESULT WINAPI CreatePointerMoniker(LPUNKNOWN obj, LPMONIKER *out) { return mk_create_with_object(MK_POINTER, obj, out); }
EXPORT HRESULT WINAPI CreateObjrefMoniker(LPUNKNOWN obj, LPMONIKER *out) { return mk_create_with_object(MK_OBJREF, obj, out); }

/* a class moniker with the text that followed the CLSID in its display name (n characters, none for 0) */
static HRESULT class_create(REFCLSID clsid, const WCHAR *extra, SIZE_T n, IMoniker **out) {
  *out = NULL;
  if (n > MK_MAX_BYTES / sizeof(WCHAR) - 1) return E_OUTOFMEMORY;
  MonikerObj *m = mk_new(MK_CLASS);
  if (!m) return E_OUTOFMEMORY;
  m->clsid = *clsid;
  if (n) {
    if (!(m->extra = heap_wdup_n(extra, n))) { mem_free(m); return E_OUTOFMEMORY; }
    m->extra_bytes = (DWORD)((n + 1) * sizeof(WCHAR));
  }
  *out = &m->iface;
  return S_OK;
}
EXPORT HRESULT WINAPI CreateClassMoniker(REFCLSID clsid, LPMONIKER *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!clsid) return E_INVALIDARG;
  return class_create(clsid, NULL, 0, out);
}

/* the class objects (CoCreateInstance gives an empty moniker, to be loaded) */
static HRESULT mk_create_instance(int kind, IUnknown *outer, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (outer) return CLASS_E_NOAGGREGATION;
  MonikerObj *m = mk_new_default(kind);
  if (!m) return E_OUTOFMEMORY;
  HRESULT hr = mk_qi(&m->iface, riid, out);
  IMoniker_Release(&m->iface);
  return hr;
}
static HRESULT file_moniker_create_instance(IUnknown *outer, REFIID riid, void **out) { return mk_create_instance(MK_FILE, outer, riid, out); }
static HRESULT item_moniker_create_instance(IUnknown *outer, REFIID riid, void **out) { return mk_create_instance(MK_ITEM, outer, riid, out); }
static HRESULT anti_moniker_create_instance(IUnknown *outer, REFIID riid, void **out) { return mk_create_instance(MK_ANTI, outer, riid, out); }
static HRESULT composite_moniker_create_instance(IUnknown *outer, REFIID riid, void **out) { return mk_create_instance(MK_COMPOSITE, outer, riid, out); }
static HRESULT pointer_moniker_create_instance(IUnknown *outer, REFIID riid, void **out) { return mk_create_instance(MK_POINTER, outer, riid, out); }
static HRESULT class_moniker_create_instance(IUnknown *outer, REFIID riid, void **out) { return mk_create_instance(MK_CLASS, outer, riid, out); }
static HRESULT objref_moniker_create_instance(IUnknown *outer, REFIID riid, void **out) { return mk_create_instance(MK_OBJREF, outer, riid, out); }

/* ---- bind contexts ----------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { WCHAR *key; IUnknown *obj; } BcParam;
typedef struct BindCtxObj {
  IBindCtx iface;
  LONG refs;
  CRITICAL_SECTION cs;         /* this context's own lock: held for its lists (an AddRef of a listed object is the only call made under it) */
  BIND_OPTS3 opts;
  IUnknown **bound;
  ULONG nbound, capbound;
  BcParam *params;
  ULONG nparams, capparams;
} BindCtxObj;
static const IBindCtxVtbl g_bc_vtbl;
static BindCtxObj *bc_of(IBindCtx *b) { return (BindCtxObj *)b; }

static HRESULT WINAPI bc_qi(IBindCtx *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IBindCtx)) { *out = me; IBindCtx_AddRef(me); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI bc_addref(IBindCtx *me) { return (ULONG)InterlockedIncrement(&bc_of(me)->refs); }
static HRESULT WINAPI bc_release_bound(IBindCtx *me);
static ULONG WINAPI bc_release(IBindCtx *me) {
  BindCtxObj *b = bc_of(me);
  LONG r = InterlockedDecrement(&b->refs);
  if (r) return (ULONG)r;
  bc_release_bound(me);
  for (ULONG i = 0; i < b->nparams; i++) { IUnknown_Release(b->params[i].obj); mem_free(b->params[i].key); }
  mem_free(b->params);
  DeleteCriticalSection(&b->cs);
  mem_free(b);
  return 0;
}
static HRESULT WINAPI bc_register_bound(IBindCtx *me, IUnknown *obj) {
  BindCtxObj *b = bc_of(me);
  if (!obj) return S_OK;
  IUnknown_AddRef(obj);
  EnterCriticalSection(&b->cs);
  if (b->nbound == b->capbound) {
    ULONG cap = b->capbound ? b->capbound * 2 : 4;
    IUnknown **v = cap > b->capbound && cap < 0x10000000u ? mem_realloc(b->bound, cap * sizeof(*v)) : NULL;
    if (!v) { LeaveCriticalSection(&b->cs); IUnknown_Release(obj); return E_OUTOFMEMORY; }
    b->bound = v;
    b->capbound = cap;
  }
  b->bound[b->nbound++] = obj;
  LeaveCriticalSection(&b->cs);
  return S_OK;
}
static HRESULT WINAPI bc_revoke_bound(IBindCtx *me, IUnknown *obj) {
  BindCtxObj *b = bc_of(me);
  IUnknown *found = NULL;
  if (!obj) return E_INVALIDARG;
  EnterCriticalSection(&b->cs);
  for (ULONG i = 0; i < b->nbound; i++) {
    if (b->bound[i] != obj) continue;
    found = obj;
    b->bound[i] = b->bound[--b->nbound];
    break;
  }
  LeaveCriticalSection(&b->cs);
  if (!found) return MK_E_NOTBOUND;
  IUnknown_Release(found);
  return S_OK;
}
static HRESULT WINAPI bc_release_bound(IBindCtx *me) {
  BindCtxObj *b = bc_of(me);
  EnterCriticalSection(&b->cs);
  IUnknown **v = b->bound;
  ULONG n = b->nbound;
  b->bound = NULL;
  b->nbound = b->capbound = 0;
  LeaveCriticalSection(&b->cs);
  for (ULONG i = 0; i < n; i++) IUnknown_Release(v[i]);
  mem_free(v);
  return S_OK;
}
static HRESULT WINAPI bc_set_options(IBindCtx *me, BIND_OPTS *o) {
  BindCtxObj *b = bc_of(me);
  if (!o) return E_INVALIDARG;
  if (o->cbStruct < sizeof(BIND_OPTS) || o->cbStruct > sizeof(BIND_OPTS3)) return E_INVALIDARG;
  EnterCriticalSection(&b->cs);
  mem_copy(&b->opts, o, o->cbStruct);
  b->opts.cbStruct = sizeof(BIND_OPTS3);
  LeaveCriticalSection(&b->cs);
  return S_OK;
}
static HRESULT WINAPI bc_get_options(IBindCtx *me, BIND_OPTS *o) {
  BindCtxObj *b = bc_of(me);
  if (!o) return E_POINTER;
  DWORD n = o->cbStruct;
  if (n < sizeof(BIND_OPTS)) return E_INVALIDARG;
  if (n > sizeof(BIND_OPTS3)) n = sizeof(BIND_OPTS3);
  EnterCriticalSection(&b->cs);
  mem_copy(o, &b->opts, n);
  LeaveCriticalSection(&b->cs);
  o->cbStruct = n;
  return S_OK;
}
static HRESULT WINAPI bc_get_rot(IBindCtx *me, IRunningObjectTable **out) {
  (void)me;
  if (!out) return E_INVALIDARG;
  return GetRunningObjectTable(0, out);
}
static HRESULT WINAPI bc_register_param(IBindCtx *me, LPOLESTR key, IUnknown *obj) {
  BindCtxObj *b = bc_of(me);
  IUnknown *old = NULL;
  if (!key || !obj) return E_INVALIDARG;
  SIZE_T n = wlen(key);
  WCHAR *dup = heap_wdup_n(key, n);
  if (!dup) return E_OUTOFMEMORY;
  IUnknown_AddRef(obj);
  EnterCriticalSection(&b->cs);
  for (ULONG i = 0; i < b->nparams; i++) {
    if (wcmp(b->params[i].key, key)) continue;
    old = b->params[i].obj;
    b->params[i].obj = obj;
    LeaveCriticalSection(&b->cs);
    mem_free(dup);
    IUnknown_Release(old);
    return S_OK;
  }
  if (b->nparams == b->capparams) {
    ULONG cap = b->capparams ? b->capparams * 2 : 4;
    BcParam *v = cap > b->capparams && cap < 0x10000000u ? mem_realloc(b->params, cap * sizeof(*v)) : NULL;
    if (!v) { LeaveCriticalSection(&b->cs); mem_free(dup); IUnknown_Release(obj); return E_OUTOFMEMORY; }
    b->params = v;
    b->capparams = cap;
  }
  b->params[b->nparams].key = dup;
  b->params[b->nparams].obj = obj;
  b->nparams++;
  LeaveCriticalSection(&b->cs);
  return S_OK;
}
static HRESULT WINAPI bc_get_param(IBindCtx *me, LPOLESTR key, IUnknown **out) {
  BindCtxObj *b = bc_of(me);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!key) return E_INVALIDARG;
  EnterCriticalSection(&b->cs);
  for (ULONG i = 0; i < b->nparams; i++) {
    if (wcmp(b->params[i].key, key)) continue;
    *out = b->params[i].obj;
    IUnknown_AddRef(*out);
    break;
  }
  LeaveCriticalSection(&b->cs);
  return *out ? S_OK : E_FAIL;
}
static HRESULT WINAPI bc_enum_params(IBindCtx *me, IEnumString **out) {
  (void)me;
  if (out) *out = NULL;
  return E_NOTIMPL;
}
static HRESULT WINAPI bc_revoke_param(IBindCtx *me, LPOLESTR key) {
  BindCtxObj *b = bc_of(me);
  BcParam found = {NULL, NULL};
  if (!key) return E_INVALIDARG;
  EnterCriticalSection(&b->cs);
  for (ULONG i = 0; i < b->nparams; i++) {
    if (wcmp(b->params[i].key, key)) continue;
    found = b->params[i];
    b->params[i] = b->params[--b->nparams];
    break;
  }
  LeaveCriticalSection(&b->cs);
  if (!found.obj) return E_FAIL;
  IUnknown_Release(found.obj);
  mem_free(found.key);
  return S_OK;
}
static const IBindCtxVtbl g_bc_vtbl = {bc_qi, bc_addref, bc_release, bc_register_bound, bc_revoke_bound, bc_release_bound, bc_set_options, bc_get_options,
                                       bc_get_rot, bc_register_param, bc_get_param, bc_enum_params, bc_revoke_param};

EXPORT HRESULT WINAPI CreateBindCtx(DWORD reserved, LPBC *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (reserved) return E_INVALIDARG;
  BindCtxObj *b = mem_alloc(sizeof(*b));
  if (!b) return E_OUTOFMEMORY;
  b->iface.lpVtbl = &g_bc_vtbl;
  b->refs = 1;
  InitializeCriticalSection(&b->cs);
  b->opts.cbStruct = sizeof(BIND_OPTS3);
  b->opts.grfFlags = 0;
  b->opts.grfMode = STGM_READWRITE;
  b->opts.dwTickCountDeadline = 0;
  b->opts.dwTrackFlags = 0;
  b->opts.dwClassContext = CLSCTX_SERVER;
  b->opts.locale = GetThreadLocale();
  b->opts.pServerInfo = NULL;
  b->opts.hwnd = NULL;
  *out = &b->iface;
  return S_OK;
}

/* ---- the running object table ------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct RotEntry {
  LONG refs;                   /* the table's reference and those of lookups in progress */
  DWORD cookie;
  IUnknown *obj;
  IMoniker *moniker;           /* the reduced moniker it was registered with (EnumRunning) */
  IExternalConnection *conn;   /* a strong connection, for ROTFLAGS_REGISTRATIONKEEPSALIVE */
  BYTE *data;                  /* the moniker's identity */
  ULONG size;
  FILETIME time;
} RotEntry;

static RotEntry **g_rot;       /* under lock() */
static ULONG g_rot_count, g_rot_cap;
static DWORD g_rot_next_cookie = 1;

static void rot_entry_release(RotEntry *e) {
  if (InterlockedDecrement(&e->refs)) return;
  if (e->conn) {
    IExternalConnection_ReleaseConnection(e->conn, EXTCONN_STRONG, 0, TRUE);
    IExternalConnection_Release(e->conn);
  }
  IUnknown_Release(e->obj);
  if (e->moniker) IMoniker_Release(e->moniker);
  mem_free(e->data);
  mem_free(e);
}
/* (under lock) */
static RotEntry *rot_find_locked(const BYTE *data, ULONG size) {
  for (ULONG i = 0; i < g_rot_count; i++) if (g_rot[i]->size == size && !mem_cmp(g_rot[i]->data, data, size)) return g_rot[i];
  return NULL;
}

/* the reduced form of a moniker (a reference) */
static HRESULT rot_reduce(IMoniker *mk, IBindCtx *pbc, IMoniker **out) {
  IMoniker *r = NULL;
  *out = NULL;
  HRESULT hr = IMoniker_Reduce(mk, pbc, MKRREDUCE_ALL, NULL, &r);
  if (FAILED(hr)) return hr;
  if (!r) { r = mk; IMoniker_AddRef(r); }
  *out = r;
  return S_OK;
}
/* how the table tells monikers apart: the comparison data of IROTData, else the class and the display name */
static HRESULT rot_identity(IMoniker *mk, IBindCtx *pbc, BYTE **data, ULONG *size) {
  IROTData *rd;
  *data = NULL;
  *size = 0;
  if (SUCCEEDED(IMoniker_QueryInterface(mk, &IID_IROTData, (void **)&rd))) {
    ULONG cap = ROT_COMPARE_MAX, got = 0;
    BYTE *buf = mem_alloc(cap);
    HRESULT hr = buf ? IROTData_GetComparisonData(rd, buf, cap, &got) : E_OUTOFMEMORY;
    if (hr == E_OUTOFMEMORY && buf && got > cap && got <= MK_MAX_BYTES) {
      BYTE *bigger = mem_realloc(buf, got);
      if (bigger) { buf = bigger; cap = got; hr = IROTData_GetComparisonData(rd, buf, cap, &got); }
    }
    IROTData_Release(rd);
    if (SUCCEEDED(hr) && got <= cap) { *data = buf; *size = got; return S_OK; }
    /* the moniker cannot describe itself (a composite with a pointer moniker in it): it stands for itself */
    if (!buf && !(buf = mem_alloc(sizeof(CLSID) + sizeof(void *)))) return E_OUTOFMEMORY;
    mem_zero(buf, sizeof(CLSID));
    void *self = mk;
    mem_copy(buf + sizeof(CLSID), &self, sizeof(self));
    *data = buf;
    *size = sizeof(CLSID) + sizeof(void *);
    return S_OK;
  }
  LPOLESTR name;
  CLSID clsid;
  HRESULT hr = IMoniker_GetDisplayName(mk, pbc, NULL, &name);
  if (FAILED(hr)) return hr;
  if (FAILED(hr = IMoniker_GetClassID(mk, &clsid))) { CoTaskMemFree(name); return hr; }
  SIZE_T n = wlen(name);
  if (n > MK_MAX_BYTES) { CoTaskMemFree(name); return E_OUTOFMEMORY; }
  ULONG total = (ULONG)(sizeof(CLSID) + (n + 1) * sizeof(WCHAR));
  BYTE *buf = mem_alloc(total);
  if (!buf) { CoTaskMemFree(name); return E_OUTOFMEMORY; }
  mem_copy(buf, &clsid, sizeof(CLSID));
  mem_copy(buf + sizeof(CLSID), name, (n + 1) * sizeof(WCHAR));
  CoTaskMemFree(name);
  *data = buf;
  *size = total;
  return S_OK;
}
/* the identity of a moniker as the table sees it, reduced first */
static HRESULT rot_key(IMoniker *mk, BYTE **data, ULONG *size) {
  IBindCtx *pbc;
  IMoniker *reduced;
  HRESULT hr = CreateBindCtx(0, &pbc);
  if (FAILED(hr)) return hr;
  if (SUCCEEDED(hr = rot_reduce(mk, pbc, &reduced))) {
    hr = rot_identity(reduced, pbc, data, size);
    IMoniker_Release(reduced);
  }
  IBindCtx_Release(pbc);
  return hr;
}
/* a lookup: the entry with that identity, with a reference for the caller (NULL if none) */
static RotEntry *rot_lookup(IMoniker *mk, HRESULT *hr) {
  BYTE *data;
  ULONG size;
  *hr = rot_key(mk, &data, &size);
  if (FAILED(*hr)) return NULL;
  lock();
  RotEntry *e = rot_find_locked(data, size);
  if (e) InterlockedIncrement(&e->refs);
  unlock();
  mem_free(data);
  return e;
}

static HRESULT WINAPI rot_qi(IRunningObjectTable *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IRunningObjectTable)) { *out = me; return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI rot_addref(IRunningObjectTable *me) { (void)me; return 2; }
static ULONG WINAPI rot_release(IRunningObjectTable *me) { (void)me; return 1; }

static HRESULT WINAPI rot_register(IRunningObjectTable *me, DWORD flags, IUnknown *obj, IMoniker *mk, DWORD *cookie) {
  (void)me;
  if (flags & ~(ROTFLAGS_REGISTRATIONKEEPSALIVE | ROTFLAGS_ALLOWANYCLIENT)) return E_INVALIDARG;
  if (!obj || !mk || !cookie) return E_INVALIDARG;
  *cookie = 0;
  if (flags & ROTFLAGS_ALLOWANYCLIENT) return CO_E_WRONG_SERVER_IDENTITY;   /* only for servers started by the SCM with a RunAs identity */
  MonikerObj *pm = mk_impl(mk);
  if (pm && pm->kind == MK_POINTER && !pm->obj) return E_INVALIDARG;

  IBindCtx *pbc;
  IMoniker *reduced;
  HRESULT hr = CreateBindCtx(0, &pbc);
  if (FAILED(hr)) return hr;
  if (FAILED(hr = rot_reduce(mk, pbc, &reduced))) { IBindCtx_Release(pbc); return hr; }
  RotEntry *e = mem_alloc(sizeof(*e));
  if (!e) { IMoniker_Release(reduced); IBindCtx_Release(pbc); return E_OUTOFMEMORY; }
  if (IMoniker_GetTimeOfLastChange(reduced, pbc, NULL, &e->time) != S_OK) CoFileTimeNow(&e->time);
  hr = rot_identity(reduced, pbc, &e->data, &e->size);
  IBindCtx_Release(pbc);
  if (FAILED(hr)) { IMoniker_Release(reduced); mem_free(e); return hr; }
  e->refs = 1;
  e->moniker = reduced;
  e->obj = obj;
  IUnknown_AddRef(obj);
  if (flags & ROTFLAGS_REGISTRATIONKEEPSALIVE) {
    IExternalConnection *conn;
    if (SUCCEEDED(IUnknown_QueryInterface(obj, &IID_IExternalConnection, (void **)&conn))) {
      IExternalConnection_AddConnection(conn, EXTCONN_STRONG, 0);
      e->conn = conn;
    }
  }
  lock();
  BOOL exists = rot_find_locked(e->data, e->size) != NULL;
  if (g_rot_count == g_rot_cap) {
    ULONG cap = g_rot_cap ? g_rot_cap * 2 : 16;
    RotEntry **v = cap > g_rot_cap && cap < 0x10000000u ? mem_realloc(g_rot, cap * sizeof(*v)) : NULL;
    if (!v) { unlock(); rot_entry_release(e); return E_OUTOFMEMORY; }
    g_rot = v;
    g_rot_cap = cap;
  }
  e->cookie = g_rot_next_cookie++;
  if (!g_rot_next_cookie) g_rot_next_cookie = 1;
  g_rot[g_rot_count++] = e;
  *cookie = e->cookie;
  unlock();
  return exists ? MK_S_MONIKERALREADYREGISTERED : S_OK;
}
static HRESULT WINAPI rot_revoke(IRunningObjectTable *me, DWORD cookie) {
  RotEntry *e = NULL;
  (void)me;
  lock();
  for (ULONG i = 0; i < g_rot_count; i++) {
    if (g_rot[i]->cookie != cookie) continue;
    e = g_rot[i];
    mem_copy(&g_rot[i], &g_rot[i + 1], (g_rot_count - i - 1) * sizeof(*g_rot));
    g_rot_count--;
    break;
  }
  unlock();
  if (!e) return E_INVALIDARG;
  rot_entry_release(e);
  return S_OK;
}
static HRESULT WINAPI rot_isrunning(IRunningObjectTable *me, IMoniker *mk) {
  HRESULT hr;
  (void)me;
  if (!mk) return E_INVALIDARG;
  RotEntry *e = rot_lookup(mk, &hr);
  if (FAILED(hr)) return hr;
  if (!e) return S_FALSE;
  rot_entry_release(e);
  return S_OK;
}
static HRESULT WINAPI rot_getobject(IRunningObjectTable *me, IMoniker *mk, IUnknown **out) {
  HRESULT hr;
  (void)me;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!mk) return E_INVALIDARG;
  RotEntry *e = rot_lookup(mk, &hr);
  if (FAILED(hr)) return hr;
  if (!e) return MK_E_UNAVAILABLE;
  *out = e->obj;
  IUnknown_AddRef(*out);
  rot_entry_release(e);
  return S_OK;
}
static HRESULT WINAPI rot_notechange(IRunningObjectTable *me, DWORD cookie, FILETIME *ft) {
  BOOL found = FALSE;
  (void)me;
  if (!ft) return E_INVALIDARG;
  lock();
  for (ULONG i = 0; i < g_rot_count; i++) if (g_rot[i]->cookie == cookie) { g_rot[i]->time = *ft; found = TRUE; break; }
  unlock();
  return found ? S_OK : E_INVALIDARG;
}
static HRESULT WINAPI rot_gettime(IRunningObjectTable *me, IMoniker *mk, FILETIME *ft) {
  HRESULT hr;
  (void)me;
  if (!mk || !ft) return E_INVALIDARG;
  RotEntry *e = rot_lookup(mk, &hr);
  if (FAILED(hr)) return hr;
  if (!e) return MK_E_UNAVAILABLE;
  lock();
  *ft = e->time;
  unlock();
  rot_entry_release(e);
  return S_OK;
}
static HRESULT WINAPI rot_enum(IRunningObjectTable *me, IEnumMoniker **out) {
  (void)me;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  lock();
  ULONG n = g_rot_count;
  RotEntry **snap = mem_alloc(((SIZE_T)n + 1) * sizeof(*snap));
  if (snap) for (ULONG i = 0; i < n; i++) { snap[i] = g_rot[i]; InterlockedIncrement(&snap[i]->refs); }
  unlock();
  if (!snap) return E_OUTOFMEMORY;
  IMoniker **v = mem_alloc(((SIZE_T)n + 1) * sizeof(*v));
  for (ULONG i = 0; i < n; i++) {
    if (v) { v[i] = snap[i]->moniker; IMoniker_AddRef(v[i]); }
    rot_entry_release(snap[i]);
  }
  mem_free(snap);
  if (!v) return E_OUTOFMEMORY;
  return mkenum_create(v, n, TRUE, out);
}
static const IRunningObjectTableVtbl g_rot_vtbl = {rot_qi, rot_addref, rot_release, rot_register, rot_revoke, rot_isrunning, rot_getobject, rot_notechange,
                                                   rot_gettime, rot_enum};
static IRunningObjectTable g_rot_object = {(IRunningObjectTableVtbl *)&g_rot_vtbl};

EXPORT HRESULT WINAPI GetRunningObjectTable(DWORD reserved, LPRUNNINGOBJECTTABLE *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (reserved) return E_UNEXPECTED;
  *out = &g_rot_object;
  return S_OK;
}
EXPORT void WINAPI DestroyRunningObjectTable(void) {
  lock();
  RotEntry **v = g_rot;
  ULONG n = g_rot_count;
  g_rot = NULL;
  g_rot_count = g_rot_cap = 0;
  unlock();
  for (ULONG i = 0; i < n; i++) rot_entry_release(v[i]);
  mem_free(v);
}
EXPORT HRESULT WINAPI CleanROTForApartment(void) { return S_OK; }

EXPORT HRESULT WINAPI GetObjectFromRotByPath(LPCWSTR path, DWORD session, IUnknown **out) {
  IMoniker *mk;
  (void)session;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!path) return E_INVALIDARG;
  HRESULT hr = CreateFileMoniker(path, &mk);
  if (FAILED(hr)) return hr;
  hr = rot_getobject(&g_rot_object, mk, out);
  IMoniker_Release(mk);
  return hr;
}

/* ---- display names ----------------------------------------------------------------------------------------------------------------------------------------------- */

/* "clsid:" [{] 36 characters of CLSID [}] [text up to and including the next ':'] */
static HRESULT parse_class(const WCHAR *name, ULONG *eaten, IMoniker **out) {
  const WCHAR *s = name + 6;
  WCHAR text[40];
  CLSID clsid;
  BOOL brace = *s == '{';
  if (brace) s++;
  text[0] = '{';
  for (int i = 0; i < 36; i++) { if (!s[i]) return MK_E_SYNTAX; text[1 + i] = s[i]; }
  text[37] = '}';
  text[38] = 0;
  if (!guid_from_text(text, &clsid)) return MK_E_SYNTAX;
  s += 36;
  if (brace) { if (*s != '}') return MK_E_SYNTAX; s++; }
  const WCHAR *extra = s;
  while (*s && *s != ':') s++;
  SIZE_T n = (SIZE_T)(s - extra);
  if (*s == ':') s++;
  HRESULT hr = class_create(&clsid, extra, n, out);
  if (SUCCEEDED(hr)) *eaten = (ULONG)(s - name);
  return hr;
}

/* "objref:" base64 ":" */
static HRESULT parse_objref(const WCHAR *name, ULONG *eaten, IMoniker **out) {
  const WCHAR *s = name + 7, *e = s;
  while (*e && *e != ':') e++;
  if (*e != ':' || e == s) return MK_E_SYNTAX;
  ULONG size;
  BYTE *bytes = b64_decode(s, (SIZE_T)(e - s), &size);
  if (!bytes) return MK_E_SYNTAX;
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
  void *p = h ? GlobalLock(h) : NULL;
  if (!p) { if (h) GlobalFree(h); mem_free(bytes); return E_OUTOFMEMORY; }
  mem_copy(p, bytes, size);
  GlobalUnlock(h);
  mem_free(bytes);
  IStream *stm;
  HRESULT hr = CreateStreamOnHGlobal(h, TRUE, &stm);
  if (FAILED(hr)) { GlobalFree(h); return hr; }
  IUnknown *obj;
  hr = CoUnmarshalInterface(stm, &IID_IUnknown, (void **)&obj);
  IStream_Release(stm);
  if (FAILED(hr)) return MK_E_SYNTAX;
  hr = CreateObjrefMoniker(obj, out);
  IUnknown_Release(obj);
  if (SUCCEEDED(hr)) *eaten = (ULONG)(e + 1 - name);
  return hr;
}

/* "progid:..." or "@progid...": the class of the ProgID parses the whole name through its IParseDisplayName */
static HRESULT parse_progid(IBindCtx *pbc, LPCOLESTR name, ULONG *eaten, IMoniker **out) {
  const WCHAR *start = name, *end;
  if (*start == '@') start++;
  for (end = start; *end && *end != ':'; end++) {}
  SIZE_T len = (SIZE_T)(end - start);
  /* it must start with '@' or have a ':', and be longer than a drive letter */
  if ((start == name && !*end) || len <= 1) return MK_E_SYNTAX;
  WCHAR *progid = heap_wdup_n(start, len);
  if (!progid) return E_OUTOFMEMORY;
  CLSID clsid;
  HRESULT hr = CLSIDFromProgID(progid, &clsid);
  mem_free(progid);
  if (FAILED(hr)) return MK_E_SYNTAX;
  IMoniker *cm;
  if (FAILED(hr = CreateClassMoniker(&clsid, &cm))) return hr;
  IParseDisplayName *pdn = NULL;
  hr = IMoniker_BindToObject(cm, pbc, NULL, &IID_IParseDisplayName, (void **)&pdn);
  if (FAILED(hr)) {
    IClassFactory *cf;
    if (SUCCEEDED(hr = IMoniker_BindToObject(cm, pbc, NULL, &IID_IClassFactory, (void **)&cf))) {
      hr = IClassFactory_CreateInstance(cf, NULL, &IID_IParseDisplayName, (void **)&pdn);
      IClassFactory_Release(cf);
    }
  }
  IMoniker_Release(cm);
  if (FAILED(hr)) return hr;
  hr = IParseDisplayName_ParseDisplayName(pdn, pbc, (LPOLESTR)name, eaten, out);
  IParseDisplayName_Release(pdn);
  return hr;
}

/* the longest beginning of the name (cut at ':', '\\', '/' or '!') that is a running file moniker or an existing file */
static BOOL is_path_separator(WCHAR c) { return c == ':' || c == '\\' || c == '/' || c == '!'; }
static HRESULT parse_file(IBindCtx *pbc, LPCOLESTR name, ULONG *eaten, IMoniker **out) {
  SIZE_T end = wlen(name);
  if (end > MK_MAX_BYTES / sizeof(WCHAR)) return MK_E_SYNTAX;
  while (end) {
    WCHAR *part = heap_wdup_n(name, end);
    if (!part) return E_OUTOFMEMORY;
    IMoniker *fm;
    HRESULT hr = mk_create_file(part, &fm);
    if (FAILED(hr)) { mem_free(part); return hr; }
    IRunningObjectTable *rot;
    BOOL found = FALSE;
    if (SUCCEEDED(mk_rot_of(pbc, &rot))) {
      found = IRunningObjectTable_IsRunning(rot, fm) == S_OK;
      IRunningObjectTable_Release(rot);
    }
    if (!found) {
      DWORD n = GetFullPathNameW(part, 0, NULL, NULL);
      WCHAR *full = n && n < 0x8000 ? mem_alloc((SIZE_T)n * sizeof(WCHAR)) : NULL;
      if (full && GetFullPathNameW(part, n, full, NULL) && GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) found = TRUE;
      mem_free(full);
    }
    mem_free(part);
    if (found) { *eaten = (ULONG)end; *out = fm; return S_OK; }
    IMoniker_Release(fm);
    SIZE_T j = end;
    while (j && !is_path_separator(name[j - 1])) j--;
    if (!j) break;
    end = j - 1;          /* the text before that separator */
  }
  return MK_E_SYNTAX;
}

EXPORT HRESULT WINAPI MkParseDisplayName(LPBC pbc, LPCOLESTR name, ULONG *eaten, LPMONIKER *out) {
  if (!pbc || !name || !*name || !eaten || !out) return E_INVALIDARG;
  *eaten = 0;
  *out = NULL;
  IMoniker *mk = NULL;
  ULONG n = 0;
  HRESULT hr;
  if (!wnicmp(name, W('c', 'l', 's', 'i', 'd', ':'), 6)) hr = parse_class(name, &n, &mk);
  else if (!wnicmp(name, W('o', 'b', 'j', 'r', 'e', 'f', ':'), 7)) hr = parse_objref(name, &n, &mk);
  else hr = parse_progid(pbc, name, &n, &mk);
  if (FAILED(hr) && hr != MK_E_SYNTAX) return hr;
  if (FAILED(hr)) hr = parse_file(pbc, name, &n, &mk);
  if (FAILED(hr)) return hr;
  SIZE_T total = wlen(name), done = 0;
  for (;;) {
    if (!n || n > total - done) { if (mk) IMoniker_Release(mk); return MK_E_SYNTAX; }     /* a parser must make progress */
    done += n;
    if (done == total) break;
    IMoniker *next = NULL;
    n = 0;
    hr = mk ? IMoniker_ParseDisplayName(mk, pbc, NULL, (LPOLESTR)name + done, &n, &next) : MK_E_SYNTAX;
    if (FAILED(hr)) { if (mk) IMoniker_Release(mk); return hr; }
    IMoniker *whole = NULL;
    hr = CreateGenericComposite(mk, next, &whole);
    IMoniker_Release(mk);
    if (next) IMoniker_Release(next);
    if (FAILED(hr)) return hr;
    mk = whole;
  }
  *eaten = (ULONG)done;
  *out = mk;
  return S_OK;
}

EXPORT HRESULT WINAPI MonikerCommonPrefixWith(LPMONIKER a, LPMONIKER b, LPMONIKER *common) {
  if (!common) return E_INVALIDARG;
  return mk_common_prefix(a, b, common);
}
EXPORT HRESULT WINAPI MonikerRelativePathTo(LPMONIKER src, LPMONIKER dest, LPMONIKER *rel, BOOL reserved) {
  (void)reserved;
  if (!rel) return E_INVALIDARG;
  *rel = NULL;
  if (!src || !dest) return E_INVALIDARG;
  return IMoniker_RelativePathTo(src, dest, rel);
}

EXPORT HRESULT WINAPI BindMoniker(LPMONIKER mk, DWORD flags, REFIID riid, LPVOID *out) {
  IBindCtx *pbc;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!mk || flags) return E_INVALIDARG;
  HRESULT hr = CreateBindCtx(0, &pbc);
  if (FAILED(hr)) return hr;
  hr = IMoniker_BindToObject(mk, pbc, NULL, riid, out);
  IBindCtx_Release(pbc);
  return hr;
}
EXPORT HRESULT WINAPI CoGetObject(LPCWSTR name, BIND_OPTS *opts, REFIID riid, void **out) {
  IBindCtx *pbc;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = CreateBindCtx(0, &pbc);
  if (FAILED(hr)) return hr;
  if (opts) hr = IBindCtx_SetBindOptions(pbc, opts);
  if (SUCCEEDED(hr)) {
    ULONG eaten;
    IMoniker *mk;
    if (SUCCEEDED(hr = MkParseDisplayName(pbc, name, &eaten, &mk))) {
      hr = IMoniker_BindToObject(mk, pbc, NULL, riid, out);
      IMoniker_Release(mk);
    }
  }
  IBindCtx_Release(pbc);
  return hr;
}
/* type libraries named by monikers are oleaut32's: without it here, there is no library to load */
EXPORT HRESULT WINAPI MonikerLoadTypeLib(LPCOLESTR name, ITypeLib **out) {
  (void)name;
  if (out) *out = NULL;
  return TYPE_E_CANTLOADLIBRARY;
}

#define BUILTIN_CLASSES_MONIKER(X) X(MK_CLSID_File, file_moniker_create_instance) X(MK_CLSID_Item, item_moniker_create_instance) X(MK_CLSID_Anti, anti_moniker_create_instance) X(MK_CLSID_Composite, composite_moniker_create_instance) X(MK_CLSID_Pointer, pointer_moniker_create_instance) X(MK_CLSID_Class, class_moniker_create_instance) X(MK_CLSID_Objref, objref_moniker_create_instance)
