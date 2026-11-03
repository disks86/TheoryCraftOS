/* ole32: the error object of a thread (CreateErrorInfo, SetErrorInfo, GetErrorInfo). */

typedef struct ErrorInfoObject {
  IErrorInfo info;
  ICreateErrorInfo create;
  ISupportErrorInfo support;
  LONG refs;
  GUID guid;
  BSTR source, description, help_file;
  DWORD help_context;
} ErrorInfoObject;

#define EI_FROM(field, me) ((ErrorInfoObject *)((BYTE *)(me) - offsetof(ErrorInfoObject, field)))

static HRESULT ei_qi(ErrorInfoObject *e, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IErrorInfo)) *out = &e->info;
  else if (IsEqualGUID(riid, &IID_ICreateErrorInfo)) *out = &e->create;
  else if (IsEqualGUID(riid, &IID_ISupportErrorInfo)) *out = &e->support;
  else return E_NOINTERFACE;
  InterlockedIncrement(&e->refs);
  return S_OK;
}
static ULONG ei_release(ErrorInfoObject *e) {
  LONG refs = InterlockedDecrement(&e->refs);
  if (refs == 0) {
    bstr_free(e->source);
    bstr_free(e->description);
    bstr_free(e->help_file);
    mem_free(e);
  }
  return (ULONG)refs;
}

static HRESULT WINAPI ei_i_qi(IErrorInfo *me, REFIID riid, void **out) { return ei_qi(EI_FROM(info, me), riid, out); }
static ULONG WINAPI ei_i_addref(IErrorInfo *me) { return (ULONG)InterlockedIncrement(&EI_FROM(info, me)->refs); }
static ULONG WINAPI ei_i_release(IErrorInfo *me) { return ei_release(EI_FROM(info, me)); }
static HRESULT WINAPI ei_getguid(IErrorInfo *me, GUID *g) { if (!g) return E_INVALIDARG; *g = EI_FROM(info, me)->guid; return S_OK; }
static HRESULT ei_get_string(BSTR s, BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = s ? bstr_dup(s) : NULL;
  if (s && !*out) return E_OUTOFMEMORY;
  return S_OK;
}
static HRESULT WINAPI ei_getsource(IErrorInfo *me, BSTR *out) { return ei_get_string(EI_FROM(info, me)->source, out); }
static HRESULT WINAPI ei_getdescription(IErrorInfo *me, BSTR *out) { return ei_get_string(EI_FROM(info, me)->description, out); }
static HRESULT WINAPI ei_gethelpfile(IErrorInfo *me, BSTR *out) { return ei_get_string(EI_FROM(info, me)->help_file, out); }
static HRESULT WINAPI ei_gethelpcontext(IErrorInfo *me, DWORD *ctx) { if (!ctx) return E_INVALIDARG; *ctx = EI_FROM(info, me)->help_context; return S_OK; }
static const IErrorInfoVtbl g_errorinfo_vtbl = {ei_i_qi, ei_i_addref, ei_i_release, ei_getguid, ei_getsource, ei_getdescription, ei_gethelpfile, ei_gethelpcontext};

static HRESULT WINAPI ei_c_qi(ICreateErrorInfo *me, REFIID riid, void **out) { return ei_qi(EI_FROM(create, me), riid, out); }
static ULONG WINAPI ei_c_addref(ICreateErrorInfo *me) { return (ULONG)InterlockedIncrement(&EI_FROM(create, me)->refs); }
static ULONG WINAPI ei_c_release(ICreateErrorInfo *me) { return ei_release(EI_FROM(create, me)); }
static HRESULT WINAPI ei_setguid(ICreateErrorInfo *me, REFGUID g) { if (!g) return E_INVALIDARG; EI_FROM(create, me)->guid = *g; return S_OK; }
static HRESULT ei_set_string(BSTR *slot, LPCOLESTR s) {
  BSTR copy = s ? bstr_dup(s) : NULL;
  if (s && !copy) return E_OUTOFMEMORY;
  bstr_free(*slot);
  *slot = copy;
  return S_OK;
}
static HRESULT WINAPI ei_setsource(ICreateErrorInfo *me, LPOLESTR s) { return ei_set_string(&EI_FROM(create, me)->source, s); }
static HRESULT WINAPI ei_setdescription(ICreateErrorInfo *me, LPOLESTR s) { return ei_set_string(&EI_FROM(create, me)->description, s); }
static HRESULT WINAPI ei_sethelpfile(ICreateErrorInfo *me, LPOLESTR s) { return ei_set_string(&EI_FROM(create, me)->help_file, s); }
static HRESULT WINAPI ei_sethelpcontext(ICreateErrorInfo *me, DWORD ctx) { EI_FROM(create, me)->help_context = ctx; return S_OK; }
static const ICreateErrorInfoVtbl g_createerrorinfo_vtbl = {ei_c_qi, ei_c_addref, ei_c_release, ei_setguid, ei_setsource, ei_setdescription, ei_sethelpfile, ei_sethelpcontext};

static HRESULT WINAPI ei_s_qi(ISupportErrorInfo *me, REFIID riid, void **out) { return ei_qi(EI_FROM(support, me), riid, out); }
static ULONG WINAPI ei_s_addref(ISupportErrorInfo *me) { return (ULONG)InterlockedIncrement(&EI_FROM(support, me)->refs); }
static ULONG WINAPI ei_s_release(ISupportErrorInfo *me) { return ei_release(EI_FROM(support, me)); }
static HRESULT WINAPI ei_interface_supports(ISupportErrorInfo *me, REFIID riid) { (void)me; (void)riid; return S_FALSE; }
static const ISupportErrorInfoVtbl g_supporterrorinfo_vtbl = {ei_s_qi, ei_s_addref, ei_s_release, ei_interface_supports};

EXPORT HRESULT WINAPI CreateErrorInfo(ICreateErrorInfo **out) {
  if (!out) return E_INVALIDARG;
  ErrorInfoObject *e = mem_alloc(sizeof(*e));
  if (!e) return E_OUTOFMEMORY;
  e->info.lpVtbl = (IErrorInfoVtbl *)&g_errorinfo_vtbl;
  e->create.lpVtbl = (ICreateErrorInfoVtbl *)&g_createerrorinfo_vtbl;
  e->support.lpVtbl = (ISupportErrorInfoVtbl *)&g_supporterrorinfo_vtbl;
  e->refs = 1;
  *out = &e->create;
  return S_OK;
}

EXPORT HRESULT WINAPI SetErrorInfo(ULONG reserved, IErrorInfo *info) {
  ThreadData *td = thread_data(TRUE);
  if (reserved) return E_INVALIDARG;
  if (!td) return E_OUTOFMEMORY;
  if (info) IErrorInfo_AddRef(info);
  if (td->error_info) IErrorInfo_Release(td->error_info);
  td->error_info = info;
  return S_OK;
}
EXPORT HRESULT WINAPI GetErrorInfo(ULONG reserved, IErrorInfo **out) {
  ThreadData *td = thread_data(FALSE);
  if (reserved || !out) return E_INVALIDARG;
  if (!td || !td->error_info) { *out = NULL; return S_FALSE; }
  *out = td->error_info;
  td->error_info = NULL;
  return S_OK;
}
