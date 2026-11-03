/* ole32: apartments, class registration and activation, the registry of classes, library loading. */

/* ---- the registry ------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The value `name` (NULL: the default) of HKEY_CLASSES_ROOT\path as text; REG_EXPAND_SZ is expanded. 0 on success. */
static LONG reg_class_string(const WCHAR *path, const WCHAR *name, WCHAR *out, DWORD cap) {
  HKEY key;
  DWORD type = 0, size = (cap - 1) * sizeof(WCHAR);
  LONG r = RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &key);
  if (r != ERROR_SUCCESS) return r;
  r = RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size);
  RegCloseKey(key);
  if (r != ERROR_SUCCESS) return r;
  if (type != REG_SZ && type != REG_EXPAND_SZ) return ERROR_FILE_NOT_FOUND;
  out[size / sizeof(WCHAR)] = 0;
  out[cap - 1] = 0;
  if (type == REG_EXPAND_SZ) {
    WCHAR tmp[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(out, tmp, MAX_PATH * 2);
    if (n && n <= MAX_PATH * 2) wcopy_n(out, tmp, cap);
  }
  return ERROR_SUCCESS;
}

/* "CLSID\{...}\sub" in `out` */
static void clsid_key_path(REFCLSID clsid, const WCHAR *sub, WCHAR *out) {
  WCHAR *p = out;
  const WCHAR *prefix = W('C', 'L', 'S', 'I', 'D', '\\');
  while (*prefix) *p++ = *prefix++;
  guid_to_text(clsid, p);
  p += 38;
  if (sub) { *p++ = '\\'; while (*sub) *p++ = *sub++; }
  *p = 0;
}
static void interface_key_path(REFIID iid, const WCHAR *sub, WCHAR *out) {
  WCHAR *p = out;
  const WCHAR *prefix = W('I', 'n', 't', 'e', 'r', 'f', 'a', 'c', 'e', '\\');
  while (*prefix) *p++ = *prefix++;
  guid_to_text(iid, p);
  p += 38;
  if (sub) { *p++ = '\\'; while (*sub) *p++ = *sub++; }
  *p = 0;
}

/* ---- apartments ----------------------------------------------------------------------------------------------------------------------------------------------------- */

static const GUID g_guid_null = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
#define APT_NONE 0
#define APT_STA 1
#define APT_MTA 2

static LONG g_mta_threads;            /* threads of the process initialized as MTA */
static LONG g_mta_usage;              /* CoIncrementMTAUsage calls not yet undone */
static BOOL g_main_sta_taken;
static DWORD g_main_sta_thread;
static BOOL g_security_initialized;

typedef struct InitSpy { struct InitSpy *next; IInitializeSpy *spy; DWORD id; DWORD thread; } InitSpy;
static InitSpy *g_init_spies;
static DWORD g_next_spy_id = 1;

static BOOL mta_exists(void) { return g_mta_threads > 0 || g_mta_usage > 0; }

static void spy_pre_init(ThreadData *td, DWORD coinit, DWORD *count_out) {
  (void)count_out;
  for (InitSpy *s = g_init_spies; s; s = s->next) if (s->thread == GetCurrentThreadId()) IInitializeSpy_PreInitialize(s->spy, coinit, td->init_count);
}

EXPORT HRESULT WINAPI CoInitializeEx(LPVOID reserved, DWORD coinit) {
  ThreadData *td = thread_data(TRUE);
  HRESULT hr = S_OK;
  DWORD kind;
  (void)reserved;
  if (!td) return E_OUTOFMEMORY;
  if (coinit & ~0xFu) return E_INVALIDARG;
  kind = (coinit & COINIT_APARTMENTTHREADED) ? APT_STA : APT_MTA;
  /* spies of this thread see the call first */
  lock();
  for (InitSpy *s = g_init_spies; s; s = s->next) {
    if (s->thread != GetCurrentThreadId()) continue;
    IInitializeSpy *spy = s->spy;
    unlock();
    hr = IInitializeSpy_PreInitialize(spy, coinit, td->init_count);
    lock();
    break;
  }
  unlock();
  if (FAILED(hr)) return hr;
  hr = S_OK;
  if (td->apartment_kind != APT_NONE) {
    if (td->apartment_kind != kind) {
      hr = RPC_E_CHANGED_MODE;
    } else {
      td->init_count++;
      hr = S_FALSE;
    }
  } else {
    td->apartment_kind = kind;
    td->init_count = 1;
    td->flags = (kind == APT_STA ? 0x80 : 0x100 | 0x40) | ((coinit & COINIT_DISABLE_OLE1DDE) ? 0x40 : 0);
    lock();
    if (kind == APT_MTA) {
      g_mta_threads++;
    } else if (!g_main_sta_taken) {
      g_main_sta_taken = TRUE;
      g_main_sta_thread = GetCurrentThreadId();
      td->main_sta = TRUE;
    }
    unlock();
    hr = S_OK;
  }
  lock();
  for (InitSpy *s = g_init_spies; s; s = s->next) {
    if (s->thread != GetCurrentThreadId()) continue;
    IInitializeSpy *spy = s->spy;
    unlock();
    IInitializeSpy_PostInitialize(spy, hr, coinit, td->init_count);
    lock();
    break;
  }
  unlock();
  return hr;
}
EXPORT HRESULT WINAPI CoInitialize(LPVOID reserved) { return CoInitializeEx(reserved, COINIT_APARTMENTTHREADED); }
EXPORT HRESULT WINAPI CoInitializeWOW(DWORD a, DWORD b) { (void)a; (void)b; return S_OK; }

static void ole_release_thread(ThreadData *td);

EXPORT void WINAPI CoUninitialize(void) {
  ThreadData *td = thread_data(FALSE);
  if (!td || td->apartment_kind == APT_NONE) return;
  lock();
  for (InitSpy *s = g_init_spies; s; s = s->next) {
    if (s->thread != GetCurrentThreadId()) continue;
    IInitializeSpy *spy = s->spy;
    unlock();
    IInitializeSpy_PreUninitialize(spy, td->init_count);
    lock();
    break;
  }
  unlock();
  if (td->init_count > 0) td->init_count--;
  if (td->init_count == 0) {
    ole_release_thread(td);
    lock();
    if (td->apartment_kind == APT_MTA) g_mta_threads--;
    if (td->main_sta) { g_main_sta_taken = FALSE; td->main_sta = FALSE; }
    unlock();
    if (td->error_info) { IErrorInfo_Release(td->error_info); td->error_info = NULL; }
    td->apartment_kind = APT_NONE;
    td->flags &= ~(0x80u | 0x100u | 0x40u);
  }
  lock();
  for (InitSpy *s = g_init_spies; s; s = s->next) {
    if (s->thread != GetCurrentThreadId()) continue;
    IInitializeSpy *spy = s->spy;
    unlock();
    IInitializeSpy_PostUninitialize(spy, td->init_count);
    lock();
    break;
  }
  unlock();
}
EXPORT void WINAPI CoUnloadingWOW(BOOL wow) { (void)wow; }

/* the thread ends without CoUninitialize: the apartment goes with it */
static void thread_detach(void) {
  ThreadData *td = thread_data(FALSE);
  if (!td) return;
  if (td->apartment_kind == APT_NONE) { *teb_ole_slot() = NULL; mem_free(td); return; }
  td->init_count = 1;
  CoUninitialize();
  *teb_ole_slot() = NULL;
  mem_free(td);
}

EXPORT HRESULT WINAPI CoIncrementMTAUsage(CO_MTA_USAGE_COOKIE *cookie) {
  if (!cookie) return E_INVALIDARG;
  lock();
  g_mta_usage++;
  unlock();
  *cookie = (CO_MTA_USAGE_COOKIE)mem_alloc(sizeof(void *));
  return S_OK;
}
EXPORT HRESULT WINAPI CoDecrementMTAUsage(CO_MTA_USAGE_COOKIE cookie) {
  if (!cookie) return E_INVALIDARG;
  mem_free(cookie);
  lock();
  if (g_mta_usage > 0) g_mta_usage--;
  unlock();
  return S_OK;
}

EXPORT HRESULT WINAPI CoGetApartmentType(APTTYPE *type, APTTYPEQUALIFIER *qualifier) {
  ThreadData *td = thread_data(FALSE);
  if (!type || !qualifier) return E_INVALIDARG;
  if (td && td->apartment_kind == APT_STA) { *type = td->main_sta ? APTTYPE_MAINSTA : APTTYPE_STA; *qualifier = APTTYPEQUALIFIER_NONE; return S_OK; }
  if (td && td->apartment_kind == APT_MTA) { *type = APTTYPE_MTA; *qualifier = APTTYPEQUALIFIER_NONE; return S_OK; }
  if (mta_exists()) { *type = APTTYPE_MTA; *qualifier = APTTYPEQUALIFIER_IMPLICIT_MTA; return S_OK; }
  *type = APTTYPE_CURRENT;
  *qualifier = APTTYPEQUALIFIER_NONE;
  return CO_E_NOTINITIALIZED;
}
EXPORT HRESULT WINAPI CoGetApartmentID(DWORD type, ULONGLONG *id) {
  ThreadData *td = thread_data(FALSE);
  if (!id) return E_INVALIDARG;
  if (!td || td->apartment_kind == APT_NONE) { if (!mta_exists()) return CO_E_NOTINITIALIZED; *id = 0x1000; return S_OK; }
  *id = td->apartment_kind == APT_MTA ? 0x1000 : (ULONGLONG)GetCurrentThreadId();
  (void)type;
  return S_OK;
}

static BOOL com_initialized(void) {
  ThreadData *td = thread_data(FALSE);
  return (td && td->apartment_kind != APT_NONE) || mta_exists();
}

/* ---- the context of an apartment ------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct ComContext {
  IUnknown unknown;            /* IUnknown only; the token is the address of this */
  IComThreadingInfo threading;
  IContextCallback callback;
  LONG refs;
  DWORD kind;
  DWORD thread;
} ComContext;

static ComContext *ctx_of_unknown(IUnknown *u) { return (ComContext *)u; }
static HRESULT ctx_qi_common(ComContext *c, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown)) *out = &c->unknown;
  else if (IsEqualGUID(riid, &IID_IComThreadingInfo)) *out = &c->threading;
  else if (IsEqualGUID(riid, &IID_IContextCallback)) *out = &c->callback;
  else return E_NOINTERFACE;
  InterlockedIncrement(&c->refs);
  return S_OK;
}
static HRESULT WINAPI ctx_u_qi(IUnknown *me, REFIID riid, void **out) { return ctx_qi_common((ComContext *)me, riid, out); }
static ULONG WINAPI ctx_u_addref(IUnknown *me) { return (ULONG)InterlockedIncrement(&((ComContext *)me)->refs); }
static ULONG WINAPI ctx_u_release(IUnknown *me) { return (ULONG)InterlockedDecrement(&((ComContext *)me)->refs); }
static const IUnknownVtbl g_ctx_unknown_vtbl = {ctx_u_qi, ctx_u_addref, ctx_u_release};

#define CTX_FROM(field, me) ((ComContext *)((BYTE *)(me) - offsetof(ComContext, field)))
static HRESULT WINAPI ctx_t_qi(IComThreadingInfo *me, REFIID riid, void **out) { return ctx_qi_common(CTX_FROM(threading, me), riid, out); }
static ULONG WINAPI ctx_t_addref(IComThreadingInfo *me) { return (ULONG)InterlockedIncrement(&CTX_FROM(threading, me)->refs); }
static ULONG WINAPI ctx_t_release(IComThreadingInfo *me) { return (ULONG)InterlockedDecrement(&CTX_FROM(threading, me)->refs); }
static HRESULT WINAPI ctx_t_apt(IComThreadingInfo *me, APTTYPE *t) { (void)me; APTTYPEQUALIFIER q; return CoGetApartmentType(t, &q); }
static HRESULT WINAPI ctx_t_thread(IComThreadingInfo *me, THDTYPE *t) {
  ThreadData *td = thread_data(FALSE);
  (void)me;
  if (!t) return E_INVALIDARG;
  *t = td && td->apartment_kind == APT_STA ? THDTYPE_PROCESSMESSAGES : THDTYPE_BLOCKMESSAGES;
  return S_OK;
}
static HRESULT WINAPI ctx_t_logical(IComThreadingInfo *me, GUID *g) { (void)me; return CoCreateGuid(g); }
static HRESULT WINAPI ctx_t_setlogical(IComThreadingInfo *me, REFGUID g) { (void)me; (void)g; return S_OK; }
static const IComThreadingInfoVtbl g_ctx_threading_vtbl = {ctx_t_qi, ctx_t_addref, ctx_t_release, ctx_t_apt, ctx_t_thread, ctx_t_logical, ctx_t_setlogical};

static HRESULT WINAPI ctx_c_qi(IContextCallback *me, REFIID riid, void **out) { return ctx_qi_common(CTX_FROM(callback, me), riid, out); }
static ULONG WINAPI ctx_c_addref(IContextCallback *me) { return (ULONG)InterlockedIncrement(&CTX_FROM(callback, me)->refs); }
static ULONG WINAPI ctx_c_release(IContextCallback *me) { return (ULONG)InterlockedDecrement(&CTX_FROM(callback, me)->refs); }
static HRESULT WINAPI ctx_c_enter(IContextCallback *me, PFNCONTEXTCALL fn, ComCallData *data, REFIID riid, int method, IUnknown *unk) {
  (void)me; (void)riid; (void)method; (void)unk;
  if (!fn) return E_INVALIDARG;
  return fn(data);
}
static const IContextCallbackVtbl g_ctx_callback_vtbl = {ctx_c_qi, ctx_c_addref, ctx_c_release, ctx_c_enter};

static ComContext g_mta_context = {{(IUnknownVtbl *)&g_ctx_unknown_vtbl}, {(IComThreadingInfoVtbl *)&g_ctx_threading_vtbl}, {(IContextCallbackVtbl *)&g_ctx_callback_vtbl}, 1, APT_MTA, 0};

static ComContext *current_context(void) {
  ThreadData *td = thread_data(FALSE);
  if (td && td->apartment_kind == APT_STA) {
    ComContext *c = td->sta_context;
    if (!c) {
      c = mem_alloc(sizeof(*c));
      if (!c) return NULL;
      c->unknown.lpVtbl = (IUnknownVtbl *)&g_ctx_unknown_vtbl;
      c->threading.lpVtbl = (IComThreadingInfoVtbl *)&g_ctx_threading_vtbl;
      c->callback.lpVtbl = (IContextCallbackVtbl *)&g_ctx_callback_vtbl;
      c->refs = 1;
      c->kind = APT_STA;
      c->thread = GetCurrentThreadId();
      td->sta_context = c;
    }
    return c;
  }
  if ((td && td->apartment_kind == APT_MTA) || mta_exists()) return &g_mta_context;
  return NULL;
}
EXPORT HRESULT WINAPI CoGetContextToken(ULONG_PTR *token) {
  if (!token) return E_POINTER;
  *token = 0;
  ComContext *c = current_context();
  if (!c) return CO_E_NOTINITIALIZED;
  *token = (ULONG_PTR)&c->unknown;
  return S_OK;
}
EXPORT HRESULT WINAPI CoGetObjectContext(REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  ComContext *c = current_context();
  if (!c) return CO_E_NOTINITIALIZED;
  return ctx_qi_common(c, riid, out);
}
EXPORT HRESULT WINAPI CoGetDefaultContext(APTTYPE type, REFIID riid, void **out) { (void)type; return CoGetObjectContext(riid, out); }
EXPORT HRESULT WINAPI CoGetCurrentLogicalThreadId(GUID *g) { return CoCreateGuid(g); }
EXPORT HRESULT WINAPI CoDisconnectContext(DWORD timeout) { (void)timeout; return S_OK; }
EXPORT HRESULT WINAPI CoGetCallerTID(DWORD *tid) { (void)tid; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoGetActivationState(GUID guid, DWORD flags, DWORD *out) { (void)guid; (void)flags; (void)out; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoGetComCatalog(REFIID riid, void **out) { (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoGetInterceptor(REFIID iid, IUnknown *outer, REFIID riid, void **out) { (void)iid; (void)outer; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoGetInterceptorFromTypeInfo(REFIID iid, IUnknown *outer, ITypeInfo *info, REFIID riid, void **out) { (void)iid; (void)outer; (void)info; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoGetInterceptorForOle32(REFIID iid, IUnknown *outer, REFIID riid, void **out) { (void)iid; (void)outer; (void)riid; if (out) *out = NULL; return E_NOTIMPL; }

/* ---- state of a thread ------------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT HRESULT WINAPI CoGetState(IUnknown **state) {
  ThreadData *td = thread_data(FALSE);
  if (!state) return E_POINTER;
  *state = td ? (IUnknown *)td->state : NULL;
  if (*state) IUnknown_AddRef(*state);
  return *state ? S_OK : E_FAIL;
}
EXPORT HRESULT WINAPI CoSetState(IUnknown *state) {
  ThreadData *td = thread_data(TRUE);
  if (!td) return E_OUTOFMEMORY;
  if (state) IUnknown_AddRef(state);
  if (td->state) IUnknown_Release((IUnknown *)td->state);
  td->state = state;
  return S_OK;
}

/* ---- initialization spies ------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI CoRegisterInitializeSpy(IInitializeSpy *spy, ULARGE_INTEGER *cookie) {
  InitSpy *s;
  if (!spy || !cookie) return E_INVALIDARG;
  if (FAILED(IInitializeSpy_QueryInterface(spy, &IID_IInitializeSpy, (void **)&spy))) return E_INVALIDARG;
  s = mem_alloc(sizeof(*s));
  if (!s) { IInitializeSpy_Release(spy); return E_OUTOFMEMORY; }
  s->spy = spy;
  s->thread = GetCurrentThreadId();
  lock();
  s->id = g_next_spy_id++;
  s->next = g_init_spies;
  g_init_spies = s;
  unlock();
  cookie->HighPart = s->thread;
  cookie->LowPart = s->id;
  return S_OK;
}
EXPORT HRESULT WINAPI CoRevokeInitializeSpy(ULARGE_INTEGER cookie) {
  InitSpy *s, **link;
  if (cookie.HighPart != GetCurrentThreadId()) return E_INVALIDARG;
  lock();
  for (link = &g_init_spies; (s = *link) != NULL; link = &s->next) {
    if (s->id == cookie.LowPart && s->thread == cookie.HighPart) {
      *link = s->next;
      unlock();
      IInitializeSpy_Release(s->spy);
      mem_free(s);
      return S_OK;
    }
  }
  unlock();
  return E_INVALIDARG;
}

/* ---- class names: ProgIDs and CLSIDs -------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI CLSIDFromProgID(LPCOLESTR progid, LPCLSID clsid) {
  WCHAR path[MAX_PATH], text[80];
  if (!progid || !clsid) return E_INVALIDARG;
  if (wlen(progid) > 200) return CO_E_CLASSSTRING;
  wcopy_n(path, progid, 200);
  SIZE_T n = wlen(path);
  const WCHAR *tail = W('\\', 'C', 'L', 'S', 'I', 'D');
  mem_copy(path + n, tail, 7 * sizeof(WCHAR));
  if (reg_class_string(path, NULL, text, 80) != ERROR_SUCCESS) return CO_E_CLASSSTRING;
  return guid_from_text(text, clsid) ? S_OK : CO_E_CLASSSTRING;
}
EXPORT HRESULT WINAPI CLSIDFromProgIDEx(LPCOLESTR progid, LPCLSID clsid) { return CLSIDFromProgID(progid, clsid); }

EXPORT HRESULT WINAPI CLSIDFromString(LPCOLESTR s, LPCLSID clsid) {
  if (!clsid) return E_INVALIDARG;
  if (!s) { mem_zero(clsid, sizeof(*clsid)); return S_OK; }
  if (s[0] != '{') {
    HRESULT hr = CLSIDFromProgID(s, clsid);
    return hr == S_OK ? S_OK : (hr == E_INVALIDARG ? hr : CO_E_CLASSSTRING);
  }
  GUID g;
  if (!guid_from_text(s, &g)) return CO_E_CLASSSTRING;
  *clsid = g;
  return S_OK;
}

EXPORT HRESULT WINAPI ProgIDFromCLSID(REFCLSID clsid, LPOLESTR *out) {
  WCHAR path[100], text[200];
  if (!out) return E_INVALIDARG;
  *out = NULL;
  clsid_key_path(clsid, W('P', 'r', 'o', 'g', 'I', 'D'), path);
  if (reg_class_string(path, NULL, text, 200) != ERROR_SUCCESS) return REGDB_E_CLASSNOTREG;
  *out = CoTaskMemAlloc((wlen(text) + 1) * sizeof(WCHAR));
  if (!*out) return E_OUTOFMEMORY;
  mem_copy(*out, text, (wlen(text) + 1) * sizeof(WCHAR));
  return S_OK;
}

EXPORT HRESULT WINAPI CLSIDFromOle1Class(LPCOLESTR progid, LPCLSID clsid, BOOL for_run) {
  WCHAR path[MAX_PATH], text[80];
  (void)for_run;
  if (!progid || !clsid) return E_INVALIDARG;
  wcopy_n(path, progid, 200);
  SIZE_T n = wlen(path);
  mem_copy(path + n, W('\\', 'C', 'L', 'S', 'I', 'D'), 7 * sizeof(WCHAR));
  if (reg_class_string(path, NULL, text, 80) != ERROR_SUCCESS || !guid_from_text(text, clsid)) return REGDB_E_CLASSNOTREG;
  return S_OK;
}
EXPORT BOOL WINAPI CoIsOle1Class(REFCLSID clsid) {
  WCHAR path[100], text[80];
  clsid_key_path(clsid, W('O', 'l', 'e', '1', 'C', 'l', 'a', 's', 's'), path);
  return reg_class_string(path, NULL, text, 80) == ERROR_SUCCESS ? TRUE : FALSE;
}

/* ---- TreatAs, proxy/stub registrations ------------------------------------------------------------------------------------------------------------------------------ */

EXPORT HRESULT WINAPI CoGetTreatAsClass(REFCLSID clsid, LPCLSID treat) {
  WCHAR path[100], text[80];
  if (!treat) return E_INVALIDARG;
  *treat = *clsid;
  clsid_key_path(clsid, W('T', 'r', 'e', 'a', 't', 'A', 's'), path);
  if (reg_class_string(path, NULL, text, 80) != ERROR_SUCCESS) return S_FALSE;
  GUID g;
  if (!guid_from_text(text, &g)) return S_FALSE;
  *treat = g;
  return S_OK;
}
EXPORT HRESULT WINAPI CoTreatAsClass(REFCLSID from, REFCLSID to) {
  WCHAR path[100], text[40];
  HKEY key;
  clsid_key_path(from, NULL, path);
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) return REGDB_E_CLASSNOTREG;
  if (IsEqualGUID(to, &g_guid_null) || IsEqualGUID(from, to)) {
    RegDeleteKeyW(key, W('T', 'r', 'e', 'a', 't', 'A', 's'));
    RegCloseKey(key);
    return S_OK;
  }
  HKEY sub;
  guid_to_text(to, text);
  LONG r = RegCreateKeyExW(key, W('T', 'r', 'e', 'a', 't', 'A', 's'), 0, NULL, 0, KEY_WRITE, NULL, &sub, NULL);
  RegCloseKey(key);
  if (r != ERROR_SUCCESS) return REGDB_E_WRITEREGDB;
  r = RegSetValueExW(sub, NULL, 0, REG_SZ, (const BYTE *)text, 39 * sizeof(WCHAR));
  RegCloseKey(sub);
  return r == ERROR_SUCCESS ? S_OK : REGDB_E_WRITEREGDB;
}

typedef struct PsEntry { struct PsEntry *next; IID iid; CLSID clsid; } PsEntry;
static PsEntry *g_ps_entries;

EXPORT HRESULT WINAPI CoGetPSClsid(REFIID iid, CLSID *clsid) {
  WCHAR path[100], text[80];
  if (!clsid) return E_INVALIDARG;
  if (!com_initialized()) return CO_E_NOTINITIALIZED;
  lock();
  for (PsEntry *e = g_ps_entries; e; e = e->next) {
    if (IsEqualGUID(&e->iid, iid)) { *clsid = e->clsid; unlock(); return S_OK; }
  }
  unlock();
  interface_key_path(iid, W('P', 'r', 'o', 'x', 'y', 'S', 't', 'u', 'b', 'C', 'l', 's', 'i', 'd', '3', '2'), path);
  if (reg_class_string(path, NULL, text, 80) != ERROR_SUCCESS) return REGDB_E_IIDNOTREG;
  return guid_from_text(text, clsid) ? S_OK : REGDB_E_IIDNOTREG;
}
EXPORT HRESULT WINAPI CoRegisterPSClsid(REFIID iid, REFCLSID clsid) {
  PsEntry *e;
  if (!com_initialized()) return CO_E_NOTINITIALIZED;
  lock();
  for (e = g_ps_entries; e; e = e->next) if (IsEqualGUID(&e->iid, iid)) break;
  if (!e) {
    e = mem_alloc(sizeof(*e));
    if (!e) { unlock(); return E_OUTOFMEMORY; }
    e->iid = *iid;
    e->next = g_ps_entries;
    g_ps_entries = e;
  }
  e->clsid = *clsid;
  unlock();
  return S_OK;
}
EXPORT HRESULT WINAPI CoGetClassVersion(REFCLSID clsid, DWORD flags, void *version) { (void)clsid; (void)flags; (void)version; return E_NOTIMPL; }

/* ---- loaded libraries ------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct LoadedLibrary { struct LoadedLibrary *next; HMODULE module; LONG uses; BOOL auto_free; } LoadedLibrary;
static LoadedLibrary *g_libraries;

static HMODULE load_server_library(const WCHAR *path) {
  HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!m) return NULL;
  lock();
  LoadedLibrary *l;
  for (l = g_libraries; l; l = l->next) if (l->module == m) { l->uses++; break; }
  if (!l) {
    l = mem_alloc(sizeof(*l));
    if (l) { l->module = m; l->uses = 1; l->auto_free = TRUE; l->next = g_libraries; g_libraries = l; }
  }
  unlock();
  return m;
}
EXPORT HINSTANCE WINAPI CoLoadLibrary(LPOLESTR path, BOOL auto_free) {
  HMODULE m = load_server_library(path);
  (void)auto_free;
  return m;
}
EXPORT void WINAPI CoFreeLibrary(HINSTANCE module) {
  lock();
  for (LoadedLibrary **link = &g_libraries, *l; (l = *link) != NULL; link = &l->next) {
    if (l->module != module) continue;
    if (--l->uses <= 0) { *link = l->next; unlock(); FreeLibrary(module); mem_free(l); return; }
    unlock();
    FreeLibrary(module);
    return;
  }
  unlock();
  FreeLibrary(module);
}
static void free_unused_libraries(BOOL all) {
  typedef HRESULT (WINAPI *CanUnload)(void);
  for (;;) {
    HMODULE victim = NULL;
    lock();
    for (LoadedLibrary *l = g_libraries; l; l = l->next) {
      if (!l->auto_free) continue;
      CanUnload can = (CanUnload)(void *)GetProcAddress(l->module, "DllCanUnloadNow");
      if (all || (can && can() == S_OK)) { victim = l->module; break; }
    }
    unlock();
    if (!victim) return;
    lock();
    for (LoadedLibrary **link = &g_libraries, *l; (l = *link) != NULL; link = &l->next) {
      if (l->module == victim) { *link = l->next; mem_free(l); break; }
    }
    unlock();
    FreeLibrary(victim);
  }
}
EXPORT void WINAPI CoFreeUnusedLibraries(void) { free_unused_libraries(FALSE); }
EXPORT void WINAPI CoFreeUnusedLibrariesEx(DWORD timeout, DWORD reserved) { (void)timeout; (void)reserved; free_unused_libraries(FALSE); }
EXPORT void WINAPI CoFreeAllLibraries(void) { free_unused_libraries(TRUE); }

/* ---- class factories: the classes ole32 implements itself ---------------------------------------------------------------------------------------------------- */

typedef HRESULT (*CreateObjectFn)(IUnknown *outer, REFIID riid, void **out);

typedef struct SimpleFactory {
  IClassFactory iface;
  LONG refs;
  CreateObjectFn create;
} SimpleFactory;

static HRESULT WINAPI factory_qi(IClassFactory *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IClassFactory)) { *out = me; IClassFactory_AddRef(me); return S_OK; }
  return E_NOINTERFACE;
}
static ULONG WINAPI factory_addref(IClassFactory *me) { return (ULONG)InterlockedIncrement(&((SimpleFactory *)me)->refs); }
static ULONG WINAPI factory_release(IClassFactory *me) {
  SimpleFactory *f = (SimpleFactory *)me;
  LONG refs = InterlockedDecrement(&f->refs);
  if (refs == 0) mem_free(f);
  return (ULONG)refs;
}
static HRESULT WINAPI factory_create(IClassFactory *me, IUnknown *outer, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (outer && !IsEqualGUID(riid, &IID_IUnknown)) return CLASS_E_NOAGGREGATION;
  return ((SimpleFactory *)me)->create(outer, riid, out);
}
static HRESULT WINAPI factory_lock(IClassFactory *me, BOOL lock_it) { (void)me; (void)lock_it; return S_OK; }
static const IClassFactoryVtbl g_factory_vtbl = {factory_qi, factory_addref, factory_release, factory_create, factory_lock};

static HRESULT make_factory(CreateObjectFn create, REFIID riid, void **out) {
  SimpleFactory *f = mem_alloc(sizeof(*f));
  if (!f) return E_OUTOFMEMORY;
  f->iface.lpVtbl = (IClassFactoryVtbl *)&g_factory_vtbl;
  f->refs = 1;
  f->create = create;
  HRESULT hr = IClassFactory_QueryInterface(&f->iface, riid, out);
  IClassFactory_Release(&f->iface);
  return hr;
}
static CreateObjectFn find_builtin_class(REFCLSID clsid);      /* (builtin.inc) */

/* ---- registered class objects ------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct ClassEntry {
  struct ClassEntry *next;
  CLSID clsid;
  IUnknown *object;
  DWORD context, flags, cookie;
  DWORD thread;
  BOOL suspended;
} ClassEntry;
static ClassEntry *g_classes;
static DWORD g_next_class_cookie = 1;
static LONG g_server_refs;

EXPORT HRESULT WINAPI CoRegisterClassObject(REFCLSID clsid, LPUNKNOWN object, DWORD context, DWORD flags, LPDWORD cookie) {
  ClassEntry *e;
  if (cookie) *cookie = 0;
  if (!clsid || !object || !cookie) return E_INVALIDARG;
  if (!com_initialized()) return CO_E_NOTINITIALIZED;
  if (!(context & (CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_HANDLER | CLSCTX_REMOTE_SERVER))) return E_INVALIDARG;
  e = mem_alloc(sizeof(*e));
  if (!e) return E_OUTOFMEMORY;
  e->clsid = *clsid;
  IUnknown_AddRef(object);
  e->object = object;
  e->context = context;
  e->flags = flags;
  e->thread = GetCurrentThreadId();
  e->suspended = (flags & REGCLS_SUSPENDED) != 0;
  lock();
  e->cookie = g_next_class_cookie++;
  e->next = g_classes;
  g_classes = e;
  unlock();
  *cookie = e->cookie;
  return S_OK;
}
EXPORT HRESULT WINAPI CoRevokeClassObject(DWORD cookie) {
  lock();
  for (ClassEntry **link = &g_classes, *e; (e = *link) != NULL; link = &e->next) {
    if (e->cookie != cookie) continue;
    *link = e->next;
    unlock();
    IUnknown_Release(e->object);
    mem_free(e);
    return S_OK;
  }
  unlock();
  return E_INVALIDARG;
}
EXPORT HRESULT WINAPI CoResumeClassObjects(void) {
  lock();
  for (ClassEntry *e = g_classes; e; e = e->next) if (e->thread == GetCurrentThreadId()) e->suspended = FALSE;
  unlock();
  return S_OK;
}
EXPORT HRESULT WINAPI CoSuspendClassObjects(void) {
  lock();
  for (ClassEntry *e = g_classes; e; e = e->next) if (e->thread == GetCurrentThreadId()) e->suspended = TRUE;
  unlock();
  return S_OK;
}
EXPORT ULONG WINAPI CoAddRefServerProcess(void) { return (ULONG)InterlockedIncrement(&g_server_refs); }
EXPORT ULONG WINAPI CoReleaseServerProcess(void) {
  LONG n = InterlockedDecrement(&g_server_refs);
  if (n == 0) CoSuspendClassObjects();
  return (ULONG)n;
}
EXPORT HRESULT WINAPI CoRetireServer(DWORD a) { (void)a; return S_OK; }
EXPORT HRESULT WINAPI CoRegisterSurrogate(LPSURROGATE surrogate) { (void)surrogate; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoRegisterSurrogateEx(REFGUID guid, void *reserved) { (void)guid; (void)reserved; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoRegisterActivationFilter(void *filter) { (void)filter; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoRegisterChannelHook(REFGUID extension, IChannelHook *hook) { (void)extension; (void)hook; return S_OK; }

/* the object registered for `clsid` that fits `context`, queried for riid; the single-use ones are revoked */
static HRESULT get_registered_class(REFCLSID clsid, DWORD context, REFIID riid, void **out, BOOL *found) {
  *found = FALSE;
  lock();
  for (ClassEntry *e = g_classes; e; e = e->next) {
    if (!IsEqualGUID(&e->clsid, clsid) || !(e->context & context) || e->suspended) continue;
    IUnknown *object = e->object;
    BOOL single = (e->flags & 0x7) == REGCLS_SINGLEUSE;
    DWORD cookie = e->cookie;
    IUnknown_AddRef(object);
    unlock();
    *found = TRUE;
    HRESULT hr = IUnknown_QueryInterface(object, riid, out);
    IUnknown_Release(object);
    if (single && SUCCEEDED(hr)) CoRevokeClassObject(cookie);
    return hr;
  }
  unlock();
  return REGDB_E_CLASSNOTREG;
}

/* ---- finding and creating objects ----------------------------------------------------------------------------------------------------------------------------------- */

typedef HRESULT (WINAPI *DllGetClassObjectFn)(REFCLSID, REFIID, void **);

/* The classes shell32.dll serves (its DllGetClassObject): shortcuts, shell items, the desktop and file system folders, My Computer, the known-folder manager. */
static BOOL is_shell32_class(REFCLSID clsid) {
  static const GUID classes[] = {
    {0x00021401, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, {0x9AC9FBE1, 0xE0A2, 0x4AD6, {0xB4, 0xEE, 0xE2, 0x12, 0x01, 0x3E, 0xA9, 0x17}},
    {0x00021400, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, {0xF3364BA0, 0x65B9, 0x11CE, {0xA9, 0xBA, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}},
    {0x20D04FE0, 0x3AEA, 0x1069, {0xA2, 0xD8, 0x08, 0x00, 0x2B, 0x30, 0x30, 0x9D}}, {0x4DF0C730, 0xDF9D, 0x4AE3, {0x91, 0x53, 0xAA, 0x6B, 0x82, 0xE9, 0x79, 0x5A}},
  };
  for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) if (IsEqualGUID(&classes[i], clsid)) return TRUE;
  return FALSE;
}

static HRESULT load_class_from_dll(const WCHAR *path, REFCLSID clsid, REFIID riid, void **out) {
  WCHAR full[MAX_PATH];
  HMODULE m;
  DllGetClassObjectFn entry;
  if (!SearchPathW(NULL, path, W('.', 'd', 'l', 'l'), MAX_PATH, full, NULL)) wcopy_n(full, path, MAX_PATH);
  m = load_server_library(full);
  if (!m) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) == 0 ? E_FAIL : CO_E_DLLNOTFOUND;
  entry = (DllGetClassObjectFn)(void *)GetProcAddress(m, "DllGetClassObject");
  if (!entry) { CoFreeLibrary(m); return CO_E_DLLNOTFOUND; }
  HRESULT hr = entry(clsid, riid, out);
  if (FAILED(hr)) CoFreeLibrary(m);
  return hr;
}

EXPORT HRESULT WINAPI CoGetClassObject(REFCLSID rclsid, DWORD context, LPVOID server_info, REFIID riid, LPVOID *out) {
  CLSID clsid;
  WCHAR path[100], dll[MAX_PATH * 2];
  HRESULT hr;
  BOOL found;
  (void)server_info;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!rclsid || !riid) return E_INVALIDARG;
  if (!com_initialized()) return CO_E_NOTINITIALIZED;
  CoGetTreatAsClass(rclsid, &clsid);
  hr = get_registered_class(&clsid, context, riid, out, &found);
  if (found) return hr;
  if (context & (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER)) {
    CreateObjectFn builtin = find_builtin_class(&clsid);
    if (builtin) return make_factory(builtin, riid, out);
    if (context & CLSCTX_INPROC_SERVER) {
      clsid_key_path(&clsid, W('I', 'n', 'p', 'r', 'o', 'c', 'S', 'e', 'r', 'v', 'e', 'r', '3', '2'), path);
      if (reg_class_string(path, NULL, dll, MAX_PATH * 2) == ERROR_SUCCESS && dll[0]) return load_class_from_dll(dll, &clsid, riid, out);
      if (is_shell32_class(&clsid)) return load_class_from_dll(W('s', 'h', 'e', 'l', 'l', '3', '2', '.', 'd', 'l', 'l'), &clsid, riid, out);      /* (a fresh system registers none of these) */
    }
    if (context & CLSCTX_INPROC_HANDLER) {
      clsid_key_path(&clsid, W('I', 'n', 'p', 'r', 'o', 'c', 'H', 'a', 'n', 'd', 'l', 'e', 'r', '3', '2'), path);
      if (reg_class_string(path, NULL, dll, MAX_PATH * 2) == ERROR_SUCCESS && dll[0] && wicmp(dll, W('o', 'l', 'e', '3', '2', '.', 'd', 'l', 'l')) != 0)
        return load_class_from_dll(dll, &clsid, riid, out);
    }
  }
  if (context & CLSCTX_LOCAL_SERVER) {
    clsid_key_path(&clsid, W('L', 'o', 'c', 'a', 'l', 'S', 'e', 'r', 'v', 'e', 'r', '3', '2'), path);
    /* a server in another process cannot be reached: this library has no RPC */
    if (reg_class_string(path, NULL, dll, MAX_PATH * 2) == ERROR_SUCCESS && dll[0]) return CO_E_SERVER_EXEC_FAILURE;
  }
  return REGDB_E_CLASSNOTREG;
}

EXPORT HRESULT WINAPI CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID riid, LPVOID *out) {
  IClassFactory *factory;
  if (!out) return E_POINTER;
  *out = NULL;
  if (!com_initialized()) return CO_E_NOTINITIALIZED;
  HRESULT hr = CoGetClassObject(clsid, context, NULL, &IID_IClassFactory, (void **)&factory);
  if (FAILED(hr)) return hr;
  hr = IClassFactory_CreateInstance(factory, outer, riid, out);
  IClassFactory_Release(factory);
  return hr;
}
EXPORT HRESULT WINAPI CoCreateInstanceEx(REFCLSID clsid, IUnknown *outer, DWORD context, COSERVERINFO *server, ULONG count, MULTI_QI *results) {
  IUnknown *unk;
  ULONG good = 0;
  if (!count || !results) return E_INVALIDARG;
  if (server && server->pwszName && server->pwszName[0]) return E_NOTIMPL;      /* (another machine) */
  for (ULONG i = 0; i < count; i++) { results[i].pItf = NULL; results[i].hr = E_NOINTERFACE; }
  HRESULT hr = CoCreateInstance(clsid, outer, context, &IID_IUnknown, (void **)&unk);
  if (FAILED(hr)) {
    for (ULONG i = 0; i < count; i++) results[i].hr = hr;
    return hr;
  }
  for (ULONG i = 0; i < count; i++) {
    if (outer && !IsEqualGUID(results[i].pIID, &IID_IUnknown)) { continue; }
    results[i].hr = IUnknown_QueryInterface(unk, results[i].pIID, (void **)&results[i].pItf);
    if (SUCCEEDED(results[i].hr)) good++;
  }
  IUnknown_Release(unk);
  return good == count ? S_OK : (good ? CO_S_NOTALLINTERFACES : E_NOINTERFACE);
}
EXPORT HRESULT WINAPI CoCreateInstanceFromApp(REFCLSID clsid, IUnknown *outer, DWORD context, PVOID reserved, ULONG count, MULTI_QI *results) {
  (void)reserved;
  return CoCreateInstanceEx(clsid, outer, context, NULL, count, results);
}
EXPORT HRESULT WINAPI CoCreateObjectInContext(IUnknown *object, void *context, REFIID riid, void **out) {
  (void)context;
  if (!object) return E_INVALIDARG;
  return IUnknown_QueryInterface(object, riid, out);
}

/* an object from a file or a storage: the object is created, then told to load it */
EXPORT HRESULT WINAPI CoGetInstanceFromFile(COSERVERINFO *server, CLSID *clsid, IUnknown *outer, DWORD context, DWORD mode, OLECHAR *name, ULONG count, MULTI_QI *results) {
  CLSID id = {0};
  IPersistFile *pf;
  HRESULT hr;
  if (!count || !results || !name) return E_INVALIDARG;
  if (clsid) id = *clsid;
  else {
    hr = GetClassFile(name, &id);
    if (FAILED(hr)) return hr;
  }
  MULTI_QI first = {&IID_IPersistFile, NULL, S_OK};
  hr = CoCreateInstanceEx(&id, outer, context, server, 1, &first);
  if (FAILED(hr)) return hr;
  pf = (IPersistFile *)first.pItf;
  hr = IPersistFile_Load(pf, name, mode);
  if (SUCCEEDED(hr)) {
    for (ULONG i = 0; i < count; i++) {
      results[i].hr = IPersistFile_QueryInterface(pf, results[i].pIID, (void **)&results[i].pItf);
    }
  }
  IPersistFile_Release(pf);
  return hr;
}
EXPORT HRESULT WINAPI CoGetInstanceFromIStorage(COSERVERINFO *server, CLSID *clsid, IUnknown *outer, DWORD context, IStorage *storage, ULONG count, MULTI_QI *results) {
  CLSID id = {0};
  IPersistStorage *ps;
  HRESULT hr;
  if (!count || !results || !storage) return E_INVALIDARG;
  if (clsid) id = *clsid;
  else {
    STATSTG st;
    hr = IStorage_Stat(storage, &st, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    id = st.clsid;
  }
  MULTI_QI first = {&IID_IPersistStorage, NULL, S_OK};
  hr = CoCreateInstanceEx(&id, outer, context, server, 1, &first);
  if (FAILED(hr)) return hr;
  ps = (IPersistStorage *)first.pItf;
  hr = IPersistStorage_Load(ps, storage);
  if (SUCCEEDED(hr)) {
    for (ULONG i = 0; i < count; i++) results[i].hr = IPersistStorage_QueryInterface(ps, results[i].pIID, (void **)&results[i].pItf);
  }
  IPersistStorage_Release(ps);
  return hr;
}

/* ---- security and the other per-process settings ----------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI CoInitializeSecurity(PSECURITY_DESCRIPTOR sd, LONG count, SOLE_AUTHENTICATION_SERVICE *services, void *reserved1, DWORD level, DWORD impersonation,
                                           void *auth_list, DWORD capabilities, void *reserved3) {
  (void)sd; (void)count; (void)services; (void)reserved1; (void)level; (void)impersonation; (void)auth_list; (void)capabilities; (void)reserved3;
  if (!com_initialized()) return CO_E_NOTINITIALIZED;
  lock();
  g_security_initialized = TRUE;
  unlock();
  return S_OK;
}
EXPORT HRESULT WINAPI CoQueryAuthenticationServices(DWORD *count, SOLE_AUTHENTICATION_SERVICE **services) { if (count) *count = 0; if (services) *services = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoQueryProxyBlanket(IUnknown *proxy, DWORD *authn, DWORD *authz, OLECHAR **server, DWORD *level, DWORD *impersonation, RPC_AUTH_IDENTITY_HANDLE *identity, DWORD *caps) {
  IClientSecurity *cs;
  if (!proxy) return E_INVALIDARG;
  HRESULT hr = IUnknown_QueryInterface(proxy, &IID_IClientSecurity, (void **)&cs);
  if (FAILED(hr)) return hr;
  hr = IClientSecurity_QueryBlanket(cs, proxy, authn, authz, server, level, impersonation, identity, caps);
  IClientSecurity_Release(cs);
  return hr;
}
EXPORT HRESULT WINAPI CoSetProxyBlanket(IUnknown *proxy, DWORD authn, DWORD authz, OLECHAR *server, DWORD level, DWORD impersonation, RPC_AUTH_IDENTITY_HANDLE identity, DWORD caps) {
  IClientSecurity *cs;
  if (!proxy) return E_INVALIDARG;
  HRESULT hr = IUnknown_QueryInterface(proxy, &IID_IClientSecurity, (void **)&cs);
  if (FAILED(hr)) return hr;
  hr = IClientSecurity_SetBlanket(cs, proxy, authn, authz, server, level, impersonation, identity, caps);
  IClientSecurity_Release(cs);
  return hr;
}
EXPORT HRESULT WINAPI CoCopyProxy(IUnknown *proxy, IUnknown **copy) {
  IClientSecurity *cs;
  if (!proxy || !copy) return E_INVALIDARG;
  HRESULT hr = IUnknown_QueryInterface(proxy, &IID_IClientSecurity, (void **)&cs);
  if (FAILED(hr)) return hr;
  hr = IClientSecurity_CopyProxy(cs, proxy, copy);
  IClientSecurity_Release(cs);
  return hr;
}
EXPORT HRESULT WINAPI CoQueryClientBlanket(DWORD *authn, DWORD *authz, OLECHAR **server, DWORD *level, DWORD *impersonation, RPC_AUTHZ_HANDLE *privs, DWORD *caps) {
  (void)authn; (void)authz; (void)server; (void)level; (void)impersonation; (void)privs; (void)caps;
  return RPC_E_CALL_COMPLETE;
}
EXPORT HRESULT WINAPI CoImpersonateClient(void) { return RPC_E_CALL_COMPLETE; }
EXPORT HRESULT WINAPI CoRevertToSelf(void) { return S_OK; }
EXPORT HRESULT WINAPI CoSwitchCallContext(IUnknown *new_context, IUnknown **old_context) {
  ThreadData *td = thread_data(TRUE);
  if (!td || !old_context) return E_INVALIDARG;
  *old_context = td->call_context;
  td->call_context = new_context;
  return S_OK;
}
EXPORT HRESULT WINAPI CoGetCallContext(REFIID riid, void **out) {
  ThreadData *td = thread_data(FALSE);
  if (!out) return E_INVALIDARG;
  if (!td || !td->call_context) { *out = NULL; return RPC_E_CALL_COMPLETE; }
  return IUnknown_QueryInterface(td->call_context, riid, out);
}
EXPORT HRESULT WINAPI CoAllowSetForegroundWindow(IUnknown *unk, LPVOID reserved) { (void)unk; (void)reserved; return S_OK; }
EXPORT HRESULT WINAPI CoAllowUnmarshalerCLSID(REFCLSID clsid) { (void)clsid; return S_OK; }
EXPORT HRESULT WINAPI CoCheckElevationEnabled(void) { return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoAicGetTokenForCOM(void *a, void *b, void *c) { (void)a; (void)b; (void)c; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoInstall(IBindCtx *bind, DWORD flags, uCLSSPEC *spec, QUERYCONTEXT *client, LPWSTR ssp) { (void)bind; (void)flags; (void)spec; (void)client; (void)ssp; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoInvalidateRemoteMachineBindings(LPOLESTR machine) { (void)machine; return S_OK; }
EXPORT HRESULT WINAPI CoGetSystemWow64DirectoryW(LPWSTR buffer, UINT size) { (void)buffer; (void)size; return HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED); }
EXPORT BOOL WINAPI CoIsHandlerConnected(IUnknown *unk) { (void)unk; return TRUE; }
EXPORT DWORD WINAPI CoGetProcessIdentifier(void) { return 0; }
EXPORT DWORD WINAPI CoGetModuleType(void) { return 0; }
EXPORT HRESULT WINAPI CoGetCallState(int a, ULONG *b) { (void)a; (void)b; return E_NOTIMPL; }
EXPORT void WINAPI CoPopServiceDomain(void) {}
EXPORT HRESULT WINAPI CoPushServiceDomain(void) { return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoHandlePriorityEventsFromMessagePump(void) { return S_OK; }
EXPORT HRESULT WINAPI CoSetMessageDispatcher(void *dispatcher) { (void)dispatcher; return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoVrfCheckThreadState(void) { return S_OK; }
EXPORT HRESULT WINAPI CoVrfGetThreadState(void) { return S_OK; }
EXPORT HRESULT WINAPI CoVrfReleaseThreadState(void) { return S_OK; }
EXPORT HRESULT WINAPI CoReactivateObject(void) { return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoDeactivateObject(void) { return E_NOTIMPL; }
EXPORT HRESULT WINAPI CoQueryReleaseObject(IUnknown *unk) { (void)unk; return S_OK; }
EXPORT HRESULT WINAPI UpdateDCOMSettings(void) { return S_OK; }
EXPORT HRESULT WINAPI UpdateProcessTracing(void *a, void *b) { (void)a; (void)b; return S_OK; }

/* the cancellation of calls: calls in this library never leave the thread, so there is nothing to cancel */
EXPORT HRESULT WINAPI CoEnableCallCancellation(LPVOID reserved) {
  ThreadData *td = thread_data(TRUE);
  if (reserved) return E_INVALIDARG;
  if (!td) return E_OUTOFMEMORY;
  td->cancel_count++;
  return S_OK;
}
EXPORT HRESULT WINAPI CoDisableCallCancellation(LPVOID reserved) {
  ThreadData *td = thread_data(TRUE);
  if (reserved) return E_INVALIDARG;
  if (!td) return E_OUTOFMEMORY;
  if (td->cancel_count <= 0) return CO_E_CANCEL_DISABLED;
  td->cancel_count--;
  return S_OK;
}
EXPORT HRESULT WINAPI CoSetCancelObject(IUnknown *unk) { (void)unk; return S_OK; }
EXPORT HRESULT WINAPI CoGetCancelObject(DWORD tid, REFIID riid, void **out) { (void)tid; (void)riid; if (out) *out = NULL; return E_NOINTERFACE; }
EXPORT HRESULT WINAPI CoTestCancel(void) { return S_OK; }
EXPORT HRESULT WINAPI CoCancelCall(DWORD tid, ULONG timeout) { (void)tid; (void)timeout; return RPC_E_CALL_COMPLETE; }

/* message filters (the thread's only; they are not used, calls do not leave the thread) */
EXPORT HRESULT WINAPI CoRegisterMessageFilter(LPMESSAGEFILTER filter, LPMESSAGEFILTER *old) {
  ThreadData *td = thread_data(FALSE);
  if (!td || td->apartment_kind != APT_STA) return CO_E_NOT_SUPPORTED;
  if (old) *old = (LPMESSAGEFILTER)td->message_filter; else if (td->message_filter) IUnknown_Release(td->message_filter);
  if (filter) IMessageFilter_AddRef(filter);
  td->message_filter = (IUnknown *)filter;
  return S_OK;
}

/* waiting: the handles are waited for as they are; there is no message loop here to run in the meantime */
EXPORT HRESULT WINAPI CoWaitForMultipleHandles(DWORD flags, DWORD timeout, ULONG count, LPHANDLE handles, LPDWORD index) {
  if (!index) return E_INVALIDARG;
  if (!count || !handles) return E_INVALIDARG;
  DWORD r = WaitForMultipleObjectsEx(count, handles, (flags & COWAIT_WAITALL) ? TRUE : FALSE, timeout, (flags & COWAIT_ALERTABLE) ? TRUE : FALSE);
  if (r == WAIT_TIMEOUT) return RPC_E_TIMEOUT;
  if (r == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
  if (r == WAIT_IO_COMPLETION) return S_OK;
  *index = r;
  return S_OK;
}
EXPORT HRESULT WINAPI CoWaitForMultipleObjects(DWORD flags, DWORD timeout, ULONG count, const HANDLE *handles, LPDWORD index) {
  return CoWaitForMultipleHandles(flags, timeout, count, (LPHANDLE)handles, index);
}
