/* ole32: marshaling of interfaces. Every object of the process is reachable from every thread through its own pointer, so the standard marshaler keeps the object in a table
 * of this process and the OBJREF in the stream (the Windows format: the signature "MEOW", STDOBJREF with the OXID of the process, the OID of the object and an IPID per
 * marshaled interface) names the entry. Unmarshaling finds the entry again and hands back the object itself. */

static const GUID kClsidStdMarshal = {0x00000017, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID kClsidInProcFreeMarshaler = {0x0000033A, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID kClsidGlobalInterfaceTable = {0x00000323, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

#define OBJREF_SIGNATURE_ 0x574F454Du      /* "MEOW" */
#define OBJREF_FLAG_STANDARD 1
#define OBJREF_FLAG_HANDLER 2
#define OBJREF_FLAG_CUSTOM 4
#define OBJREF_FLAG_EXTENDED 8

#define OBJREF_CUSTOM_HEADER (4 + 4 + 16 + 16 + 4 + 4)
#define OBJREF_STD_SIZE (4 + 4 + 16 + 40 + 4)

/* ---- the table of marshaled objects ------------------------------------------------------------------------------------------------------------------------------ */

typedef struct StdEntry {
  struct StdEntry *next;
  GUID ipid;
  ULONGLONG oid;
  IUnknown *unk;           /* the identity of the object */
  LONG normal_refs;        /* marshaled with MSHLFLAGS_NORMAL, not yet unmarshaled or released */
  BOOL table_strong;       /* holds a reference */
  BOOL table;              /* stays after an unmarshal */
} StdEntry;

static StdEntry *g_std_entries;
static ULONGLONG g_oxid;
static ULONGLONG g_next_oid = 0x1000;

static ULONGLONG process_oxid(void) {
  if (!g_oxid) {
    GUID g;
    CoCreateGuid(&g);
    mem_copy(&g_oxid, &g, 8);
    if (!g_oxid) g_oxid = 1;
  }
  return g_oxid;
}

/* ---- the standard marshaler ----------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct StdMarshal {
  IMarshal iface;
  LONG refs;
  IUnknown *unk;           /* the object this marshaler was made for (NULL: for unmarshaling only) */
  GUID iid;
} StdMarshal;

static const IMarshalVtbl g_std_marshal_vtbl;

static HRESULT WINAPI stdm_qi(IMarshal *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMarshal)) { *out = me; IMarshal_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI stdm_addref(IMarshal *me) { return (ULONG)InterlockedIncrement(&((StdMarshal *)me)->refs); }
static ULONG WINAPI stdm_release(IMarshal *me) {
  StdMarshal *m = (StdMarshal *)me;
  LONG refs = InterlockedDecrement(&m->refs);
  if (refs == 0) {
    if (m->unk) IUnknown_Release(m->unk);
    mem_free(m);
  }
  return (ULONG)refs;
}
static HRESULT WINAPI stdm_unmarshal_class(IMarshal *me, REFIID riid, void *pv, DWORD ctx, void *ctxptr, DWORD flags, CLSID *clsid) {
  (void)me; (void)riid; (void)pv; (void)ctx; (void)ctxptr; (void)flags;
  if (!clsid) return E_INVALIDARG;
  *clsid = kClsidStdMarshal;
  return S_OK;
}
static HRESULT WINAPI stdm_size_max(IMarshal *me, REFIID riid, void *pv, DWORD ctx, void *ctxptr, DWORD flags, DWORD *size) {
  (void)me; (void)riid; (void)pv; (void)ctx; (void)ctxptr; (void)flags;
  if (!size) return E_INVALIDARG;
  *size = OBJREF_STD_SIZE;
  return S_OK;
}

static HRESULT WINAPI stdm_marshal(IMarshal *me, IStream *stream, REFIID riid, void *pv, DWORD ctx, void *ctxptr, DWORD flags) {
  StdMarshal *m = (StdMarshal *)me;
  IUnknown *identity = NULL;
  BYTE buffer[OBJREF_STD_SIZE];
  StdEntry *e;
  HRESULT hr;
  (void)ctx; (void)ctxptr;
  if (!stream) return E_INVALIDARG;
  if (pv) hr = IUnknown_QueryInterface((IUnknown *)pv, &IID_IUnknown, (void **)&identity);
  else if (m->unk) hr = IUnknown_QueryInterface(m->unk, &IID_IUnknown, (void **)&identity);
  else return E_INVALIDARG;
  if (FAILED(hr)) return hr;
  e = mem_alloc(sizeof(*e));
  if (!e) { IUnknown_Release(identity); return E_OUTOFMEMORY; }
  CoCreateGuid(&e->ipid);
  e->unk = identity;                  /* (the reference of the QueryInterface above is the entry's) */
  e->table = (flags & (MSHLFLAGS_TABLESTRONG | MSHLFLAGS_TABLEWEAK)) != 0;
  if (flags & MSHLFLAGS_TABLEWEAK) { IUnknown_Release(identity); }
  else if (flags & MSHLFLAGS_TABLESTRONG) e->table_strong = TRUE;
  else e->normal_refs = 1;
  lock();
  /* one OID per object: the entries of the same object share it */
  e->oid = 0;
  for (StdEntry *o = g_std_entries; o; o = o->next) if (o->unk == identity) { e->oid = o->oid; break; }
  if (!e->oid) e->oid = g_next_oid++;
  e->next = g_std_entries;
  g_std_entries = e;
  ULONGLONG oxid = process_oxid();
  unlock();
  mem_zero(buffer, sizeof(buffer));
  *(DWORD *)(buffer + 0) = OBJREF_SIGNATURE_;
  *(DWORD *)(buffer + 4) = OBJREF_FLAG_STANDARD;
  mem_copy(buffer + 8, riid, 16);
  *(DWORD *)(buffer + 24) = 0;                                         /* STDOBJREF.flags */
  *(DWORD *)(buffer + 28) = (flags & MSHLFLAGS_TABLEWEAK) ? 0 : (flags & MSHLFLAGS_TABLESTRONG) ? 1 : 5;      /* cPublicRefs */
  mem_copy(buffer + 32, &oxid, 8);
  mem_copy(buffer + 40, &e->oid, 8);
  mem_copy(buffer + 48, &e->ipid, 16);
  /* DUALSTRINGARRAY: no entries */
  hr = stream_write_all(stream, buffer, OBJREF_STD_SIZE);
  if (FAILED(hr)) {
    lock();
    for (StdEntry **link = &g_std_entries, *x; (x = *link) != NULL; link = &x->next) if (x == e) { *link = x->next; break; }
    unlock();
    if (!(flags & MSHLFLAGS_TABLEWEAK)) IUnknown_Release(identity);
    mem_free(e);
  }
  return hr;
}

/* Reads the STDOBJREF part (the stream is after the 24 bytes of signature, flags, iid) and finds the entry. `consume`: unmarshal (QueryInterface), else only release. */
static HRESULT std_take(IStream *stream, REFIID riid, void **out, BOOL consume) {
  BYTE body[40 + 4];
  GUID ipid;
  StdEntry *e = NULL;
  HRESULT hr = stream_read_all(stream, body, sizeof(body));
  if (FAILED(hr)) return hr;
  mem_copy(&ipid, body + 24, 16);
  USHORT dsa_entries = *(USHORT *)(body + 40);
  if (dsa_entries) {
    /* the string bindings (wNumEntries 16-bit words) of a server: skip them */
    LARGE_INTEGER skip;
    skip.QuadPart = (LONGLONG)dsa_entries * 2;
    IStream_Seek(stream, skip, STREAM_SEEK_CUR, NULL);
  }
  lock();
  for (e = g_std_entries; e; e = e->next) if (IsEqualGUID(&e->ipid, &ipid)) break;
  if (!e) { unlock(); return CO_E_OBJNOTCONNECTED; }
  IUnknown *unk = e->unk;
  BOOL release_hold = FALSE, free_entry = FALSE;
  if (e->table) {
    if (!consume) { free_entry = TRUE; release_hold = e->table_strong; }
  } else {
    if (e->normal_refs > 0) e->normal_refs--;
    if (e->normal_refs == 0) { free_entry = TRUE; release_hold = TRUE; }
  }
  if (!consume || !e->table) { /* the entry may go */ }
  if (free_entry) {
    for (StdEntry **link = &g_std_entries, *x; (x = *link) != NULL; link = &x->next) if (x == e) { *link = x->next; break; }
  }
  if (consume && unk) IUnknown_AddRef(unk);
  unlock();
  if (free_entry) mem_free(e);
  hr = S_OK;
  if (consume) {
    hr = unk ? IUnknown_QueryInterface(unk, riid, out) : E_NOINTERFACE;
    if (unk) IUnknown_Release(unk);
  }
  if (release_hold && unk) IUnknown_Release(unk);
  return hr;
}

static HRESULT WINAPI stdm_unmarshal(IMarshal *me, IStream *stream, REFIID riid, void **out) {
  BYTE head[24];
  (void)me;
  if (!stream || !out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = stream_read_all(stream, head, sizeof(head));
  if (FAILED(hr)) return hr;
  if (*(DWORD *)head != OBJREF_SIGNATURE_) return RPC_E_INVALID_OBJREF;
  if (*(DWORD *)(head + 4) != OBJREF_FLAG_STANDARD) return RPC_E_INVALID_OBJREF;
  return std_take(stream, riid, out, TRUE);
}
static HRESULT WINAPI stdm_release_data(IMarshal *me, IStream *stream) {
  BYTE head[24];
  (void)me;
  if (!stream) return E_INVALIDARG;
  HRESULT hr = stream_read_all(stream, head, sizeof(head));
  if (FAILED(hr)) return hr;
  if (*(DWORD *)head != OBJREF_SIGNATURE_ || *(DWORD *)(head + 4) != OBJREF_FLAG_STANDARD) return RPC_E_INVALID_OBJREF;
  return std_take(stream, &IID_IUnknown, NULL, FALSE);
}

static void std_disconnect_object(IUnknown *identity) {
  for (;;) {
    StdEntry *e = NULL;
    lock();
    for (StdEntry **link = &g_std_entries, *x; (x = *link) != NULL; link = &x->next) {
      if (x->unk == identity) { *link = x->next; e = x; break; }
    }
    unlock();
    if (!e) return;
    if (e->normal_refs > 0 || e->table_strong) IUnknown_Release(e->unk);
    mem_free(e);
  }
}
static HRESULT WINAPI stdm_disconnect(IMarshal *me, DWORD reserved) {
  StdMarshal *m = (StdMarshal *)me;
  (void)reserved;
  if (m->unk) std_disconnect_object(m->unk);
  return S_OK;
}
static const IMarshalVtbl g_std_marshal_vtbl = {stdm_qi, stdm_addref, stdm_release, stdm_unmarshal_class, stdm_size_max, stdm_marshal, stdm_unmarshal, stdm_release_data, stdm_disconnect};

static HRESULT std_marshal_create(IUnknown *unk, REFIID riid, IMarshal **out) {
  StdMarshal *m = mem_alloc(sizeof(*m));
  if (!m) return E_OUTOFMEMORY;
  m->iface.lpVtbl = (IMarshalVtbl *)&g_std_marshal_vtbl;
  m->refs = 1;
  if (unk) { m->unk = unk; IUnknown_AddRef(unk); }
  if (riid) m->iid = *riid;
  *out = &m->iface;
  return S_OK;
}
static HRESULT std_marshal_create_instance(IUnknown *outer, REFIID riid, void **out) {
  IMarshal *m;
  (void)outer;
  HRESULT hr = std_marshal_create(NULL, NULL, &m);
  if (FAILED(hr)) return hr;
  hr = IMarshal_QueryInterface(m, riid, out);
  IMarshal_Release(m);
  return hr;
}

EXPORT HRESULT WINAPI CoGetStandardMarshal(REFIID riid, IUnknown *unk, DWORD ctx, LPVOID ctxptr, DWORD flags, LPMARSHAL *out) {
  (void)ctx; (void)ctxptr; (void)flags;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (unk) {
    IUnknown *identity;
    HRESULT hr = IUnknown_QueryInterface(unk, &IID_IUnknown, (void **)&identity);
    if (FAILED(hr)) return hr;
    hr = std_marshal_create(identity, riid, out);
    IUnknown_Release(identity);
    return hr;
  }
  return std_marshal_create(NULL, riid, out);
}
EXPORT HRESULT WINAPI CoGetStdMarshalEx(LPUNKNOWN outer, DWORD smexflags, LPUNKNOWN *inner) {
  (void)smexflags;
  if (!inner) return E_INVALIDARG;
  *inner = NULL;
  if (!outer) return E_INVALIDARG;
  IMarshal *m;
  HRESULT hr = std_marshal_create(outer, NULL, &m);
  if (SUCCEEDED(hr)) { hr = IMarshal_QueryInterface(m, &IID_IUnknown, (void **)inner); IMarshal_Release(m); }
  return hr;
}

/* ---- CoMarshalInterface and friends --------------------------------------------------------------------------------------------------------------------------- */

/* the marshaler of an object: its own IMarshal, or the standard one */
static HRESULT get_marshaler(REFIID riid, IUnknown *unk, DWORD ctx, void *ctxptr, DWORD flags, IMarshal **out, CLSID *clsid) {
  HRESULT hr = IUnknown_QueryInterface(unk, &IID_IMarshal, (void **)out);
  if (FAILED(hr)) {
    hr = CoGetStandardMarshal(riid, unk, ctx, ctxptr, flags, out);
    if (FAILED(hr)) return hr;
  }
  hr = IMarshal_GetUnmarshalClass(*out, riid, unk, ctx, ctxptr, flags, clsid);
  if (FAILED(hr)) { IMarshal_Release(*out); *out = NULL; }
  return hr;
}

EXPORT HRESULT WINAPI CoMarshalInterface(LPSTREAM stream, REFIID riid, LPUNKNOWN unk, DWORD ctx, LPVOID ctxptr, DWORD flags) {
  IMarshal *marshal;
  CLSID clsid;
  if (!stream || !riid) return E_INVALIDARG;
  if (!unk) return E_INVALIDARG;
  HRESULT hr = get_marshaler(riid, unk, ctx, ctxptr, flags, &marshal, &clsid);
  if (FAILED(hr)) return hr;
  if (!IsEqualGUID(&clsid, &kClsidStdMarshal)) {
    BYTE header[OBJREF_CUSTOM_HEADER];
    mem_zero(header, sizeof(header));
    *(DWORD *)header = OBJREF_SIGNATURE_;
    *(DWORD *)(header + 4) = OBJREF_FLAG_CUSTOM;
    mem_copy(header + 8, riid, 16);
    mem_copy(header + 24, &clsid, 16);
    hr = stream_write_all(stream, header, sizeof(header));
    if (FAILED(hr)) { IMarshal_Release(marshal); return hr; }
  }
  hr = IMarshal_MarshalInterface(marshal, stream, riid, unk, ctx, ctxptr, flags);
  IMarshal_Release(marshal);
  return hr;
}

EXPORT HRESULT WINAPI CoUnmarshalInterface(LPSTREAM stream, REFIID riid, LPVOID *out) {
  BYTE head[24];
  ULARGE_INTEGER start;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!stream) return E_INVALIDARG;
  HRESULT hr = stream_tell(stream, &start);
  if (FAILED(hr)) return hr;
  hr = stream_read_all(stream, head, sizeof(head));
  if (FAILED(hr)) return hr;
  if (*(DWORD *)head != OBJREF_SIGNATURE_) return RPC_E_INVALID_OBJREF;
  DWORD flags = *(DWORD *)(head + 4);
  if (flags == OBJREF_FLAG_STANDARD) {
    return std_take(stream, riid, out, TRUE);
  } else if (flags == OBJREF_FLAG_CUSTOM) {
    BYTE rest[16 + 4 + 4];
    CLSID clsid;
    hr = stream_read_all(stream, rest, sizeof(rest));
    if (FAILED(hr)) return hr;
    mem_copy(&clsid, rest, 16);
    IMarshal *marshal;
    DWORD extension = *(DWORD *)(rest + 16);
    if (extension) {
      LARGE_INTEGER skip;
      skip.QuadPart = extension;
      IStream_Seek(stream, skip, STREAM_SEEK_CUR, NULL);
    }
    if (IsEqualGUID(&clsid, &kClsidStdMarshal)) hr = std_marshal_create(NULL, NULL, &marshal);
    else hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMarshal, (void **)&marshal);
    if (FAILED(hr)) return hr;
    hr = IMarshal_UnmarshalInterface(marshal, stream, riid, out);
    IMarshal_Release(marshal);
    return hr;
  } else if (flags == OBJREF_FLAG_HANDLER) {
    /* the handler's class follows the STDOBJREF; it unmarshals from the start of the OBJREF */
    BYTE std[40];
    CLSID clsid;
    hr = stream_read_all(stream, std, sizeof(std));
    if (SUCCEEDED(hr)) hr = stream_read_all(stream, &clsid, sizeof(clsid));
    if (FAILED(hr)) return hr;
    IMarshal *marshal;
    hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMarshal, (void **)&marshal);
    if (FAILED(hr)) return hr;
    stream_seek_to(stream, start.QuadPart);
    hr = IMarshal_UnmarshalInterface(marshal, stream, riid, out);
    IMarshal_Release(marshal);
    return hr;
  }
  return RPC_E_INVALID_OBJREF;
}

EXPORT HRESULT WINAPI CoReleaseMarshalData(LPSTREAM stream) {
  BYTE head[24];
  ULARGE_INTEGER start;
  if (!stream) return E_INVALIDARG;
  HRESULT hr = stream_tell(stream, &start);
  if (FAILED(hr)) return hr;
  hr = stream_read_all(stream, head, sizeof(head));
  if (FAILED(hr)) return hr;
  if (*(DWORD *)head != OBJREF_SIGNATURE_) return RPC_E_INVALID_OBJREF;
  DWORD flags = *(DWORD *)(head + 4);
  if (flags == OBJREF_FLAG_STANDARD) return std_take(stream, &IID_IUnknown, NULL, FALSE);
  if (flags == OBJREF_FLAG_CUSTOM) {
    BYTE rest[16 + 4 + 4];
    CLSID clsid;
    hr = stream_read_all(stream, rest, sizeof(rest));
    if (FAILED(hr)) return hr;
    mem_copy(&clsid, rest, 16);
    IMarshal *marshal;
    DWORD extension = *(DWORD *)(rest + 16);
    if (extension) { LARGE_INTEGER skip; skip.QuadPart = extension; IStream_Seek(stream, skip, STREAM_SEEK_CUR, NULL); }
    if (IsEqualGUID(&clsid, &kClsidStdMarshal)) hr = std_marshal_create(NULL, NULL, &marshal);
    else hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IMarshal, (void **)&marshal);
    if (FAILED(hr)) return hr;
    hr = IMarshal_ReleaseMarshalData(marshal, stream);
    IMarshal_Release(marshal);
    return hr;
  }
  return RPC_E_INVALID_OBJREF;
}

EXPORT HRESULT WINAPI CoGetMarshalSizeMax(ULONG *size, REFIID riid, LPUNKNOWN unk, DWORD ctx, LPVOID ctxptr, DWORD flags) {
  IMarshal *marshal;
  CLSID clsid;
  DWORD inner = 0;
  if (!size || !unk) return E_INVALIDARG;
  HRESULT hr = get_marshaler(riid, unk, ctx, ctxptr, flags, &marshal, &clsid);
  if (FAILED(hr)) return hr;
  hr = IMarshal_GetMarshalSizeMax(marshal, riid, unk, ctx, ctxptr, flags, &inner);
  IMarshal_Release(marshal);
  if (FAILED(hr)) return hr;
  *size = inner + (IsEqualGUID(&clsid, &kClsidStdMarshal) ? 0 : 96);
  return S_OK;
}

EXPORT HRESULT WINAPI CoMarshalInterThreadInterfaceInStream(REFIID riid, LPUNKNOWN unk, LPSTREAM *out) {
  IStream *stream;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
  if (FAILED(hr)) return hr;
  hr = CoMarshalInterface(stream, riid, unk, MSHCTX_INPROC, NULL, MSHLFLAGS_NORMAL);
  if (FAILED(hr)) { IStream_Release(stream); return hr; }
  stream_seek_to(stream, 0);
  *out = stream;
  return S_OK;
}
EXPORT HRESULT WINAPI CoGetInterfaceAndReleaseStream(LPSTREAM stream, REFIID riid, LPVOID *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!stream) return E_INVALIDARG;
  HRESULT hr = CoUnmarshalInterface(stream, riid, out);
  IStream_Release(stream);
  return hr;
}

EXPORT HRESULT WINAPI CoMarshalHresult(LPSTREAM stream, HRESULT hr) { return stream ? stream_write_all(stream, &hr, sizeof(hr)) : E_INVALIDARG; }
EXPORT HRESULT WINAPI CoUnmarshalHresult(LPSTREAM stream, HRESULT *hr) { return stream && hr ? stream_read_all(stream, hr, sizeof(*hr)) : E_INVALIDARG; }

EXPORT HRESULT WINAPI CoDisconnectObject(LPUNKNOWN unk, DWORD reserved) {
  IMarshal *marshal;
  IUnknown *identity;
  if (!unk) return E_INVALIDARG;
  if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IMarshal, (void **)&marshal))) {
    IMarshal_DisconnectObject(marshal, reserved);
    IMarshal_Release(marshal);
  }
  if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IUnknown, (void **)&identity))) {
    std_disconnect_object(identity);
    IUnknown_Release(identity);
  }
  return S_OK;
}

/* ---- external locks ---------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct ExternalLock { struct ExternalLock *next; IUnknown *unk; LONG count; } ExternalLock;
static ExternalLock *g_external_locks;

EXPORT HRESULT WINAPI CoLockObjectExternal(LPUNKNOWN unk, BOOL lock_it, BOOL last_unlock_releases) {
  IUnknown *identity;
  ExternalLock *l;
  HRESULT hr;
  if (!unk) return E_INVALIDARG;
  hr = IUnknown_QueryInterface(unk, &IID_IUnknown, (void **)&identity);
  if (FAILED(hr)) return hr;
  lock();
  for (l = g_external_locks; l; l = l->next) if (l->unk == identity) break;
  if (lock_it) {
    if (!l) {
      l = mem_alloc(sizeof(*l));
      if (!l) { unlock(); IUnknown_Release(identity); return E_OUTOFMEMORY; }
      l->unk = identity;
      l->next = g_external_locks;
      g_external_locks = l;
      IUnknown_AddRef(identity);
    }
    l->count++;
    unlock();
    IUnknown_Release(identity);
    return S_OK;
  }
  if (!l) { unlock(); IUnknown_Release(identity); return S_OK; }
  BOOL gone = FALSE;
  if (--l->count <= 0) {
    for (ExternalLock **link = &g_external_locks, *x; (x = *link) != NULL; link = &x->next) if (x == l) { *link = x->next; break; }
    gone = TRUE;
  }
  unlock();
  if (gone) {
    mem_free(l);
    IUnknown_Release(identity);       /* the lock's reference */
    if (last_unlock_releases) CoDisconnectObject(identity, 0);
  }
  IUnknown_Release(identity);
  return S_OK;
}

/* ---- the free-threaded marshaler ----------------------------------------------------------------------------------------------------------------------------------- */

typedef struct FreeMarshaler {
  IUnknown inner;
  IMarshal marshal;
  LONG refs;
  IUnknown *outer;
} FreeMarshaler;
#define FTM_FROM_INNER(me) ((FreeMarshaler *)((BYTE *)(me) - offsetof(FreeMarshaler, inner)))
#define FTM_FROM_MARSHAL(me) ((FreeMarshaler *)((BYTE *)(me) - offsetof(FreeMarshaler, marshal)))

static HRESULT WINAPI ftm_inner_qi(IUnknown *me, REFIID riid, void **out) {
  FreeMarshaler *f = FTM_FROM_INNER(me);
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown)) *out = &f->inner;
  else if (IsEqualGUID(riid, &IID_IMarshal)) *out = &f->marshal;
  else return E_NOINTERFACE;
  IUnknown_AddRef(&f->inner);
  return S_OK;
}
static ULONG WINAPI ftm_inner_addref(IUnknown *me) { return (ULONG)InterlockedIncrement(&FTM_FROM_INNER(me)->refs); }
static ULONG WINAPI ftm_inner_release(IUnknown *me) {
  FreeMarshaler *f = FTM_FROM_INNER(me);
  LONG refs = InterlockedDecrement(&f->refs);
  if (refs == 0) mem_free(f);
  return (ULONG)refs;
}
static const IUnknownVtbl g_ftm_inner_vtbl = {ftm_inner_qi, ftm_inner_addref, ftm_inner_release};

static HRESULT WINAPI ftm_qi(IMarshal *me, REFIID riid, void **out) {
  FreeMarshaler *f = FTM_FROM_MARSHAL(me);
  return IUnknown_QueryInterface(f->outer ? f->outer : &f->inner, riid, out);
}
static ULONG WINAPI ftm_addref(IMarshal *me) { FreeMarshaler *f = FTM_FROM_MARSHAL(me); return IUnknown_AddRef(f->outer ? f->outer : &f->inner); }
static ULONG WINAPI ftm_release(IMarshal *me) { FreeMarshaler *f = FTM_FROM_MARSHAL(me); return IUnknown_Release(f->outer ? f->outer : &f->inner); }
static HRESULT WINAPI ftm_unmarshal_class(IMarshal *me, REFIID riid, void *pv, DWORD ctx, void *ctxptr, DWORD flags, CLSID *clsid) {
  (void)me; (void)riid; (void)pv; (void)ctxptr; (void)flags;
  if (!clsid) return E_INVALIDARG;
  *clsid = (ctx == MSHCTX_INPROC) ? kClsidInProcFreeMarshaler : kClsidStdMarshal;
  return S_OK;
}
static HRESULT WINAPI ftm_size_max(IMarshal *me, REFIID riid, void *pv, DWORD ctx, void *ctxptr, DWORD flags, DWORD *size) {
  if (!size) return E_INVALIDARG;
  if (ctx == MSHCTX_INPROC) { *size = sizeof(DWORD) + sizeof(void *) + sizeof(DWORD) + sizeof(GUID); return S_OK; }
  IMarshal *std;
  HRESULT hr = CoGetStandardMarshal(riid, (IUnknown *)pv, ctx, ctxptr, flags, &std);
  (void)me;
  if (FAILED(hr)) return hr;
  hr = IMarshal_GetMarshalSizeMax(std, riid, pv, ctx, ctxptr, flags, size);
  IMarshal_Release(std);
  return hr;
}
static HRESULT WINAPI ftm_marshal(IMarshal *me, IStream *stream, REFIID riid, void *pv, DWORD ctx, void *ctxptr, DWORD flags) {
  (void)me;
  if (ctx == MSHCTX_INPROC) {
    DWORD zero = 0;
    HRESULT hr = stream_write_all(stream, &flags, sizeof(flags));
    if (SUCCEEDED(hr)) hr = stream_write_all(stream, &pv, sizeof(pv));
    if (SUCCEEDED(hr)) hr = stream_write_all(stream, &zero, sizeof(zero));
    if (SUCCEEDED(hr)) hr = stream_write_all(stream, &kClsidInProcFreeMarshaler, sizeof(GUID));
    if (SUCCEEDED(hr) && pv && !(flags & MSHLFLAGS_TABLEWEAK)) IUnknown_AddRef((IUnknown *)pv);
    return hr;
  }
  IMarshal *std;
  HRESULT hr = CoGetStandardMarshal(riid, (IUnknown *)pv, ctx, ctxptr, flags, &std);
  if (FAILED(hr)) return hr;
  hr = IMarshal_MarshalInterface(std, stream, riid, pv, ctx, ctxptr, flags);
  IMarshal_Release(std);
  return hr;
}
static HRESULT WINAPI ftm_unmarshal(IMarshal *me, IStream *stream, REFIID riid, void **out) {
  DWORD flags, zero;
  void *pv;
  GUID id;
  (void)me;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  HRESULT hr = stream_read_all(stream, &flags, sizeof(flags));
  if (SUCCEEDED(hr)) hr = stream_read_all(stream, &pv, sizeof(pv));
  if (SUCCEEDED(hr)) hr = stream_read_all(stream, &zero, sizeof(zero));
  if (SUCCEEDED(hr)) hr = stream_read_all(stream, &id, sizeof(id));
  if (FAILED(hr)) return hr;
  if (!pv) return E_INVALIDARG;
  hr = IUnknown_QueryInterface((IUnknown *)pv, riid, out);
  if (!(flags & (MSHLFLAGS_TABLEWEAK | MSHLFLAGS_TABLESTRONG))) IUnknown_Release((IUnknown *)pv);
  return hr;
}
static HRESULT WINAPI ftm_release_data(IMarshal *me, IStream *stream) {
  DWORD flags, zero;
  void *pv;
  GUID id;
  (void)me;
  HRESULT hr = stream_read_all(stream, &flags, sizeof(flags));
  if (SUCCEEDED(hr)) hr = stream_read_all(stream, &pv, sizeof(pv));
  if (SUCCEEDED(hr)) hr = stream_read_all(stream, &zero, sizeof(zero));
  if (SUCCEEDED(hr)) hr = stream_read_all(stream, &id, sizeof(id));
  if (FAILED(hr)) return hr;
  if (pv && !(flags & MSHLFLAGS_TABLEWEAK)) IUnknown_Release((IUnknown *)pv);
  return S_OK;
}
static HRESULT WINAPI ftm_disconnect(IMarshal *me, DWORD reserved) { (void)me; (void)reserved; return S_OK; }
static const IMarshalVtbl g_ftm_marshal_vtbl = {ftm_qi, ftm_addref, ftm_release, ftm_unmarshal_class, ftm_size_max, ftm_marshal, ftm_unmarshal, ftm_release_data, ftm_disconnect};

EXPORT HRESULT WINAPI CoCreateFreeThreadedMarshaler(LPUNKNOWN outer, LPUNKNOWN *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  FreeMarshaler *f = mem_alloc(sizeof(*f));
  if (!f) return E_OUTOFMEMORY;
  f->inner.lpVtbl = (IUnknownVtbl *)&g_ftm_inner_vtbl;
  f->marshal.lpVtbl = (IMarshalVtbl *)&g_ftm_marshal_vtbl;
  f->refs = 1;
  f->outer = outer;
  *out = &f->inner;
  return S_OK;
}
static HRESULT ftm_create_instance(IUnknown *outer, REFIID riid, void **out) {
  IUnknown *inner;
  (void)outer;
  HRESULT hr = CoCreateFreeThreadedMarshaler(NULL, &inner);
  if (FAILED(hr)) return hr;
  hr = IUnknown_QueryInterface(inner, riid, out);
  IUnknown_Release(inner);
  return hr;
}

/* ---- the global interface table ---------------------------------------------------------------------------------------------------------------------------------- */

typedef struct GitEntry { struct GitEntry *next; DWORD cookie; IStream *stream; GUID iid; } GitEntry;
static GitEntry *g_git_entries;
static DWORD g_git_next_cookie = 0x100;

typedef struct GlobalTable { IGlobalInterfaceTable iface; } GlobalTable;

static HRESULT WINAPI git_qi(IGlobalInterfaceTable *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IGlobalInterfaceTable)) { *out = me; return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI git_addref(IGlobalInterfaceTable *me) { (void)me; return 2; }
static ULONG WINAPI git_release(IGlobalInterfaceTable *me) { (void)me; return 1; }
static HRESULT WINAPI git_register(IGlobalInterfaceTable *me, IUnknown *unk, REFIID riid, DWORD *cookie) {
  IStream *stream;
  GitEntry *e;
  (void)me;
  if (!unk || !riid || !cookie) return E_INVALIDARG;
  HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
  if (FAILED(hr)) return hr;
  hr = CoMarshalInterface(stream, riid, unk, MSHCTX_INPROC, NULL, MSHLFLAGS_TABLESTRONG);
  if (FAILED(hr)) { IStream_Release(stream); return hr; }
  e = mem_alloc(sizeof(*e));
  if (!e) { stream_seek_to(stream, 0); CoReleaseMarshalData(stream); IStream_Release(stream); return E_OUTOFMEMORY; }
  e->stream = stream;
  e->iid = *riid;
  lock();
  e->cookie = g_git_next_cookie;
  g_git_next_cookie += 4;
  e->next = g_git_entries;
  g_git_entries = e;
  unlock();
  *cookie = e->cookie;
  return S_OK;
}
static HRESULT WINAPI git_revoke(IGlobalInterfaceTable *me, DWORD cookie) {
  GitEntry *e = NULL;
  (void)me;
  lock();
  for (GitEntry **link = &g_git_entries, *x; (x = *link) != NULL; link = &x->next) if (x->cookie == cookie) { *link = x->next; e = x; break; }
  unlock();
  if (!e) return E_INVALIDARG;
  stream_seek_to(e->stream, 0);
  CoReleaseMarshalData(e->stream);
  IStream_Release(e->stream);
  mem_free(e);
  return S_OK;
}
static HRESULT WINAPI git_get(IGlobalInterfaceTable *me, DWORD cookie, REFIID riid, void **out) {
  IStream *stream = NULL, *clone;
  (void)me;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  lock();
  for (GitEntry *e = g_git_entries; e; e = e->next) if (e->cookie == cookie) { stream = e->stream; IStream_AddRef(stream); break; }
  unlock();
  if (!stream) return E_INVALIDARG;
  HRESULT hr = IStream_Clone(stream, &clone);
  IStream_Release(stream);
  if (FAILED(hr)) return hr;
  stream_seek_to(clone, 0);
  hr = CoUnmarshalInterface(clone, riid, out);
  IStream_Release(clone);
  return hr;
}
static const IGlobalInterfaceTableVtbl g_git_vtbl = {git_qi, git_addref, git_release, git_register, git_revoke, git_get};
static GlobalTable g_git = {{(IGlobalInterfaceTableVtbl *)&g_git_vtbl}};

static HRESULT git_create_instance(IUnknown *outer, REFIID riid, void **out) {
  (void)outer;
  return git_qi(&g_git.iface, riid, out);
}

/* the classes of this file that can be created by class id */
#define BUILTIN_CLASSES_MARSHAL(X) X(kClsidStdMarshal, std_marshal_create_instance) X(kClsidInProcFreeMarshaler, ftm_create_instance) X(kClsidGlobalInterfaceTable, git_create_instance)
