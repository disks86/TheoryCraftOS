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

/* Error info objects (per thread), the active object table (process local: there is no running object table to share with), the typelib name hash and a few small exports. */

/* ---- a tiny lock for the tables below (the sections are short and never call out) ---------------------------------------------------------------------- */
static volatile LONG oa_com_lock_state;
static void oa_com_lock(void) { while (__sync_lock_test_and_set(&oa_com_lock_state, 1)) Sleep(0); }
static void oa_com_unlock(void) { __sync_lock_release(&oa_com_lock_state); }

/* ---- the error object: ICreateErrorInfo, IErrorInfo and ISupportErrorInfo of one object -------------------------------------------------------------- */
typedef struct {
  ICreateErrorInfo create;
  IErrorInfo info;
  ISupportErrorInfo support;
  LONG ref;
  GUID guid;
  BSTR source, description, help_file;
  DWORD help_context;
} ErrorInfoObject;

#define EI_FROM_CREATE(p) ((ErrorInfoObject *)((char *)(p) - __builtin_offsetof(ErrorInfoObject, create)))
#define EI_FROM_INFO(p) ((ErrorInfoObject *)((char *)(p) - __builtin_offsetof(ErrorInfoObject, info)))
#define EI_FROM_SUPPORT(p) ((ErrorInfoObject *)((char *)(p) - __builtin_offsetof(ErrorInfoObject, support)))

static HRESULT ei_query(ErrorInfoObject *object, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ICreateErrorInfo)) *out = &object->create;
  else if (IsEqualIID(riid, &IID_IErrorInfo)) *out = &object->info;
  else if (IsEqualIID(riid, &IID_ISupportErrorInfo)) *out = &object->support;
  else return E_NOINTERFACE;
  InterlockedIncrement(&object->ref);
  return S_OK;
}
static ULONG ei_addref(ErrorInfoObject *object) { return (ULONG)InterlockedIncrement(&object->ref); }
static ULONG ei_release(ErrorInfoObject *object) {
  LONG ref = InterlockedDecrement(&object->ref);
  if (ref == 0) {
    SysFreeString(object->source);
    SysFreeString(object->description);
    SysFreeString(object->help_file);
    oa_free(object);
  }
  return (ULONG)ref;
}

static HRESULT WINAPI ei_create_query(ICreateErrorInfo *This, REFIID riid, void **out) { return ei_query(EI_FROM_CREATE(This), riid, out); }
static ULONG WINAPI ei_create_addref(ICreateErrorInfo *This) { return ei_addref(EI_FROM_CREATE(This)); }
static ULONG WINAPI ei_create_release(ICreateErrorInfo *This) { return ei_release(EI_FROM_CREATE(This)); }
static HRESULT WINAPI ei_set_guid(ICreateErrorInfo *This, REFGUID guid) { EI_FROM_CREATE(This)->guid = *guid; return S_OK; }
static HRESULT ei_set_string(BSTR *slot, LPCOLESTR text) {
  BSTR copy = text ? SysAllocString(text) : NULL;
  if (text && !copy) return E_OUTOFMEMORY;
  SysFreeString(*slot);
  *slot = copy;
  return S_OK;
}
static HRESULT WINAPI ei_set_source(ICreateErrorInfo *This, LPOLESTR text) { return ei_set_string(&EI_FROM_CREATE(This)->source, text); }
static HRESULT WINAPI ei_set_description(ICreateErrorInfo *This, LPOLESTR text) { return ei_set_string(&EI_FROM_CREATE(This)->description, text); }
static HRESULT WINAPI ei_set_help_file(ICreateErrorInfo *This, LPOLESTR text) { return ei_set_string(&EI_FROM_CREATE(This)->help_file, text); }
static HRESULT WINAPI ei_set_help_context(ICreateErrorInfo *This, DWORD context) { EI_FROM_CREATE(This)->help_context = context; return S_OK; }

static const ICreateErrorInfoVtbl ei_create_vtbl = {
  ei_create_query, ei_create_addref, ei_create_release, ei_set_guid, ei_set_source, ei_set_description, ei_set_help_file, ei_set_help_context,
};

static HRESULT WINAPI ei_info_query(IErrorInfo *This, REFIID riid, void **out) { return ei_query(EI_FROM_INFO(This), riid, out); }
static ULONG WINAPI ei_info_addref(IErrorInfo *This) { return ei_addref(EI_FROM_INFO(This)); }
static ULONG WINAPI ei_info_release(IErrorInfo *This) { return ei_release(EI_FROM_INFO(This)); }
static HRESULT WINAPI ei_get_guid(IErrorInfo *This, GUID *guid) {
  if (!guid) return E_INVALIDARG;
  *guid = EI_FROM_INFO(This)->guid;
  return S_OK;
}
static HRESULT ei_get_string(BSTR value, BSTR *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!value) return S_OK;
  *out = SysAllocStringLen(value, SysStringLen(value));
  return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI ei_get_source(IErrorInfo *This, BSTR *out) { return ei_get_string(EI_FROM_INFO(This)->source, out); }
static HRESULT WINAPI ei_get_description(IErrorInfo *This, BSTR *out) { return ei_get_string(EI_FROM_INFO(This)->description, out); }
static HRESULT WINAPI ei_get_help_file(IErrorInfo *This, BSTR *out) { return ei_get_string(EI_FROM_INFO(This)->help_file, out); }
static HRESULT WINAPI ei_get_help_context(IErrorInfo *This, DWORD *context) {
  if (!context) return E_INVALIDARG;
  *context = EI_FROM_INFO(This)->help_context;
  return S_OK;
}

static const IErrorInfoVtbl ei_info_vtbl = {
  ei_info_query, ei_info_addref, ei_info_release, ei_get_guid, ei_get_source, ei_get_description, ei_get_help_file, ei_get_help_context,
};

static HRESULT WINAPI ei_support_query(ISupportErrorInfo *This, REFIID riid, void **out) { return ei_query(EI_FROM_SUPPORT(This), riid, out); }
static ULONG WINAPI ei_support_addref(ISupportErrorInfo *This) { return ei_addref(EI_FROM_SUPPORT(This)); }
static ULONG WINAPI ei_support_release(ISupportErrorInfo *This) { return ei_release(EI_FROM_SUPPORT(This)); }
static HRESULT WINAPI ei_supports(ISupportErrorInfo *This, REFIID riid) {
  (void)This;
  return IsEqualIID(riid, &IID_IErrorInfo) ? S_OK : S_FALSE;
}

static const ISupportErrorInfoVtbl ei_support_vtbl = { ei_support_query, ei_support_addref, ei_support_release, ei_supports };

EXPORT HRESULT WINAPI CreateErrorInfo(ICreateErrorInfo **created) {
  if (!created) return E_INVALIDARG;
  ErrorInfoObject *object = oa_calloc(sizeof(*object));
  if (!object) return E_OUTOFMEMORY;
  object->create.lpVtbl = (ICreateErrorInfoVtbl *)&ei_create_vtbl;
  object->info.lpVtbl = (IErrorInfoVtbl *)&ei_info_vtbl;
  object->support.lpVtbl = (ISupportErrorInfoVtbl *)&ei_support_vtbl;
  object->ref = 1;
  *created = &object->create;
  return S_OK;
}

/* ---- the thread's current error object: a TLS slot, allocated on first use (or at process attach) --------------------------------------------------- */
static volatile LONG ei_tls_state;     /* 0: no slot, 1: being allocated, 2: ready */
static DWORD ei_tls_index = TLS_OUT_OF_INDEXES;

static BOOL ei_tls_ready(void) {
  for (;;) {
    LONG state = ei_tls_state;
    if (state == 2) return TRUE;
    if (state == 0 && __sync_bool_compare_and_swap(&ei_tls_state, 0, 1)) {
      DWORD index = TlsAlloc();
      if (index == TLS_OUT_OF_INDEXES) { ei_tls_state = 0; return FALSE; }
      ei_tls_index = index;
      __sync_synchronize();
      ei_tls_state = 2;
      return TRUE;
    }
    if (state == 1) Sleep(0);
  }
}

/* Replaces the thread's error object (the new one's reference is taken over) and releases the old one. */
static void ei_set_current(IErrorInfo *fresh) {
  IErrorInfo *old = TlsGetValue(ei_tls_index);
  TlsSetValue(ei_tls_index, fresh);
  if (old) IErrorInfo_Release(old);
}

/* ole32.dll holds the thread's error object when it has the functions (its GetErrorInfo and this one then see the same object); the local slot is the fallback. */
static FARPROC ei_ole32_proc(const char *name) {
  HMODULE module = LoadLibraryA("ole32.dll");
  return module ? GetProcAddress(module, name) : NULL;
}

EXPORT HRESULT WINAPI SetErrorInfo(ULONG reserved, IErrorInfo *info) {
  HRESULT (WINAPI *forward)(ULONG, IErrorInfo *) = (void *)ei_ole32_proc("SetErrorInfo");
  if (forward && (void *)forward != (void *)SetErrorInfo) return forward(reserved, info);
  if (reserved) return E_INVALIDARG;
  if (!ei_tls_ready()) return E_OUTOFMEMORY;
  if (info) IErrorInfo_AddRef(info);
  ei_set_current(info);
  return S_OK;
}

EXPORT HRESULT WINAPI GetErrorInfo(ULONG reserved, IErrorInfo **info) {
  HRESULT (WINAPI *forward)(ULONG, IErrorInfo **) = (void *)ei_ole32_proc("GetErrorInfo");
  if (forward && (void *)forward != (void *)GetErrorInfo) return forward(reserved, info);
  if (reserved || !info) return E_INVALIDARG;
  *info = NULL;
  if (!ei_tls_ready()) return S_FALSE;
  IErrorInfo *current = TlsGetValue(ei_tls_index);
  if (!current) return S_FALSE;
  TlsSetValue(ei_tls_index, NULL);      /* the reference moves to the caller */
  *info = current;
  return S_OK;
}

/* DllMain hooks: the thread's error object goes away with the thread. */
#define OA_ERRORINFO_DLLMAIN
static void errorinfo_thread_detach(void) {
  if (ei_tls_state == 2) {
    IErrorInfo *current = TlsGetValue(ei_tls_index);
    if (current) { TlsSetValue(ei_tls_index, NULL); IErrorInfo_Release(current); }
  }
}
static void errorinfo_process_attach(void) { ei_tls_ready(); }
static void errorinfo_process_detach(void) {
  errorinfo_thread_detach();
  if (ei_tls_state == 2) { ei_tls_state = 0; TlsFree(ei_tls_index); ei_tls_index = TLS_OUT_OF_INDEXES; }
}

/* ---- the active object table: registrations by CLSID. A strong registration holds a reference to the object, a weak one does not. ----------------------- */
typedef struct { DWORD cookie; CLSID clsid; IUnknown *object; BOOL strong; } ActiveObject;
static ActiveObject *ao_table;
static SIZE_T ao_count, ao_capacity;
static DWORD ao_next_cookie = 1;

EXPORT HRESULT WINAPI RegisterActiveObject(IUnknown *object, REFCLSID clsid, DWORD flags, DWORD *cookie) {
  if (!object || !clsid || !cookie || flags > ACTIVEOBJECT_WEAK) return E_INVALIDARG;
  *cookie = 0;
  if (flags == ACTIVEOBJECT_STRONG) IUnknown_AddRef(object);
  HRESULT hr = S_OK;
  oa_com_lock();
  for (SIZE_T i = 0; i < ao_count; i++) if (IsEqualGUID(&ao_table[i].clsid, clsid)) hr = MK_S_MONIKERALREADYREGISTERED;
  if (ao_count == ao_capacity) {
    SIZE_T capacity = ao_capacity ? ao_capacity * 2 : 8;
    ActiveObject *grown = capacity > ao_capacity && capacity <= (SIZE_T)-1 / sizeof(*grown) ? oa_realloc(ao_table, capacity * sizeof(*grown)) : NULL;
    if (!grown) hr = E_OUTOFMEMORY;
    else { ao_table = grown; ao_capacity = capacity; }
  }
  if (hr == E_OUTOFMEMORY) {
    oa_com_unlock();
    if (flags == ACTIVEOBJECT_STRONG) IUnknown_Release(object);
    return hr;
  }
  ActiveObject *entry = &ao_table[ao_count++];
  entry->cookie = ao_next_cookie++;
  if (!ao_next_cookie) ao_next_cookie = 1;
  entry->clsid = *clsid;
  entry->object = object;
  entry->strong = flags == ACTIVEOBJECT_STRONG;
  *cookie = entry->cookie;
  oa_com_unlock();
  return hr;
}

EXPORT HRESULT WINAPI RevokeActiveObject(DWORD cookie, void *reserved) {
  if (reserved) return E_INVALIDARG;
  IUnknown *release = NULL;
  HRESULT hr = E_INVALIDARG;
  oa_com_lock();
  for (SIZE_T i = 0; i < ao_count; i++) {
    if (ao_table[i].cookie != cookie || !cookie) continue;
    if (ao_table[i].strong) release = ao_table[i].object;
    for (SIZE_T j = i + 1; j < ao_count; j++) ao_table[j - 1] = ao_table[j];
    ao_count--;
    hr = S_OK;
    break;
  }
  oa_com_unlock();
  if (release) IUnknown_Release(release);
  return hr;
}

EXPORT HRESULT WINAPI GetActiveObject(REFCLSID clsid, void *reserved, IUnknown **object) {
  if (!clsid || reserved || !object) return E_INVALIDARG;
  *object = NULL;
  IUnknown *found = NULL;
  oa_com_lock();
  for (SIZE_T i = ao_count; i > 0; i--) if (IsEqualGUID(&ao_table[i - 1].clsid, clsid)) { found = ao_table[i - 1].object; break; }
  /* (the reference is taken inside the lock: a revoke on another thread must not release the object between the lookup and the AddRef) */
  if (found) IUnknown_AddRef(found);
  oa_com_unlock();
  if (!found) return MK_E_UNAVAILABLE;
  HRESULT hr = IUnknown_QueryInterface(found, &IID_IUnknown, (void **)object);
  IUnknown_Release(found);
  return hr;
}

/* ---- small exports --------------------------------------------------------------------------------------------------------------------------------- */
EXPORT ULONG WINAPI OaBuildVersion(void) { return 0x000A0000u; }
EXPORT void WINAPI OACleanup(void) {}          /* (no caches to empty: BSTRs and the like are plain heap blocks) */
EXPORT void WINAPI SetOaNoCache(void) {}

EXPORT void WINAPI ClearCustData(CUSTDATA *data) {
  if (!data) return;
  if (data->prgCustData) {
    for (DWORD i = 0; i < data->cCustData; i++) VariantClear(&data->prgCustData[i].varValue);
    oa_free(data->prgCustData);      /* (CoTaskMemAlloc is the process heap) */
    data->prgCustData = NULL;
  }
  data->cCustData = 0;
}

/* ---- LHashValOfNameSys: the hash of a name in a type library's name table. The names are folded to bytes and the bytes through a table that depends on the language
 * (and on the Mac/Windows kind of the library); the 32 bit accumulator is multiplied by 37 per character and reduced modulo 65599, and the language class and kind go
 * to the upper bits. ----------------------------------------------------------------------------------------------------------------------------------------------- */

/* lhash_tables[n][c - 1] is the value of byte c; lhash_classes says which tables a language class uses (the class is also the upper part of the result). */
static const unsigned char lhash_tables[18][255] = {
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,0,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,86,88,85,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,86,88,85,90,123,124,125,126,127,127,
    127,130,70,132,133,134,135,127,137,83,139,140,127,127,127,127,145,146,147,148,149,150,150,152,153,83,155,140,127,127,85,160,
    161,162,163,164,165,166,167,168,169,65,171,172,150,174,175,176,177,50,51,180,181,182,183,184,49,79,187,188,189,190,191,65,
    65,65,65,65,65,65,67,69,69,69,69,73,73,73,73,68,78,79,79,79,79,79,215,79,85,85,85,85,85,222,223,65,
    65,65,65,65,65,65,67,69,69,69,69,73,73,73,73,68,78,79,79,79,79,79,247,79,85,85,85,85,85,222,85
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,0,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,86,88,85,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,86,88,85,90,123,124,125,126,127,65,
    65,67,69,78,79,85,65,65,65,65,65,65,67,69,69,69,69,73,73,73,73,78,79,79,79,79,79,85,85,85,85,160,
    161,162,163,164,165,166,167,168,169,170,171,172,173,65,79,176,177,178,179,180,181,182,183,184,185,186,65,79,189,65,79,192,
    193,194,195,70,197,198,199,200,201,202,65,65,79,206,206,208,208,210,211,212,213,214,215,85,85,218,219,220,221,63,63,224,
    225,226,227,228,65,69,65,69,69,73,73,73,73,79,79,63,79,85,85,85,73,127,247,127,249,250,251,63,253,254,127
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,128,
    129,130,70,132,133,134,135,94,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,140,157,0,159,160,
    161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,50,51,180,181,182,183,184,49,186,187,188,189,190,191,192,
    193,194,193,193,193,193,199,200,201,201,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,0,221,222,223,65,
    225,65,227,228,229,230,67,69,69,69,69,236,236,73,73,240,241,242,243,79,245,246,247,248,85,250,85,85,0,0,255
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,65,
    129,67,69,78,79,85,65,65,65,65,139,140,67,69,69,69,69,73,147,73,73,78,79,152,79,79,155,85,85,85,85,160,
    161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,
    193,194,193,193,193,193,199,200,201,201,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,0,
    225,226,227,228,229,230,231,232,233,233,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,0,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,86,88,85,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,86,88,85,90,123,124,125,126,127,127,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,85
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,0,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,127,
    127,130,127,132,133,134,135,127,137,83,139,83,84,90,90,127,145,146,147,148,149,150,150,127,153,83,155,83,84,90,90,160,
    127,162,76,164,65,166,167,168,169,83,171,172,150,174,90,176,177,178,76,180,181,182,183,184,65,83,187,76,189,76,90,82,
    65,65,65,65,76,67,67,67,69,69,69,69,73,73,68,208,78,78,79,79,79,79,215,82,85,85,85,85,89,84,223,82,
    65,65,65,65,76,67,67,67,69,69,69,69,73,73,68,208,78,78,79,79,79,79,247,82,85,85,85,85,89,84,255
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,0,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,65,
    65,65,69,65,79,85,65,65,67,65,67,67,67,69,90,90,68,73,68,69,69,69,79,69,79,79,79,85,69,69,85,160,
    161,69,163,164,165,166,167,168,169,170,69,172,173,71,73,73,73,178,179,73,75,182,183,76,76,76,76,76,76,76,78,78,
    78,194,195,78,78,198,199,200,201,202,78,79,79,79,79,208,208,210,211,212,213,214,215,79,82,82,82,220,221,82,82,82,
    83,226,227,83,83,83,65,84,84,73,90,90,85,79,79,85,85,85,85,85,85,85,85,89,89,75,90,76,76,71,255
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,0,0,
    0,130,70,132,133,134,135,0,137,0,139,0,0,0,0,0,145,146,147,148,149,45,45,0,153,0,155,0,0,0,0,9,
    161,162,163,164,165,166,167,168,169,0,171,172,45,174,45,176,177,50,51,180,181,182,183,184,185,186,187,188,189,190,191,186,
    162,194,195,196,184,198,185,200,186,202,203,204,205,206,188,208,209,0,211,212,190,214,215,216,191,186,190,162,184,185,186,190,
    162,194,195,196,184,198,185,200,186,202,203,204,205,206,188,208,209,211,211,212,190,214,215,216,191,186,190,188,190,191,0
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,0,65,
    49,50,69,51,79,85,135,65,65,65,0,140,67,69,69,69,69,146,147,73,73,150,151,152,79,79,155,63,85,85,85,160,
    161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,171,186,187,188,189,190,191,176,
    193,194,195,196,197,198,199,200,201,9,189,204,176,182,207,45,45,210,211,212,213,214,184,171,195,189,182,184,171,195,191,189,
    176,181,190,162,182,188,161,184,171,165,186,164,187,193,195,166,191,196,170,198,163,191,170,204,189,183,171,189,171,189,63
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,0,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,127,
    127,130,70,132,133,134,135,127,137,83,139,140,127,127,127,127,145,146,147,148,149,150,150,152,153,83,155,140,127,127,89,160,
    161,162,163,164,165,166,167,168,169,65,171,172,150,174,175,176,177,50,51,180,181,182,183,184,49,79,187,188,189,190,191,65,
    65,65,65,65,65,198,67,69,69,69,69,73,73,73,73,208,78,79,79,79,79,79,215,79,85,85,85,85,89,222,223,65,
    65,65,65,65,65,198,67,69,69,69,69,73,73,73,73,208,78,79,79,79,79,79,247,79,85,85,85,85,89,222,89
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,0,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,128,
    129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,
    161,162,163,164,165,166,167,168,169,170,171,171,173,174,174,176,177,178,179,167,181,182,183,184,184,186,186,188,188,190,190,183,
    193,194,195,70,197,198,199,200,201,202,203,203,205,205,193,208,208,210,211,212,213,214,215,216,216,218,218,220,221,221,159,128,
    129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,0,0,
    0,130,70,132,133,134,135,0,137,83,139,140,0,0,0,0,145,146,147,148,149,45,45,152,153,83,155,140,0,0,89,9,
    161,162,163,164,165,166,167,168,169,65,171,172,45,174,175,176,177,50,51,180,181,182,183,184,49,79,187,188,189,190,191,65,
    193,65,65,65,65,198,67,69,201,69,69,73,205,73,73,208,78,79,211,79,79,214,215,214,85,218,85,85,221,222,223,65,
    193,65,65,65,65,198,67,69,201,69,69,73,205,73,73,208,78,79,211,79,79,214,247,214,85,218,85,85,221,222,89
  },
  {
    14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,
    46,0,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,
    78,79,80,81,82,83,84,85,86,86,88,85,90,91,92,93,94,95,96,65,66,67,68,69,70,71,72,73,74,75,76,77,
    78,79,80,81,82,83,84,85,86,86,88,85,90,123,124,125,126,127,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,162,163,164,165,166,167,168,169,65,171,172,150,
    174,175,176,177,50,51,180,181,182,183,184,49,79,187,188,189,190,191,65,65,65,65,65,65,65,67,69,69,69,69,73,73,
    73,73,68,78,79,79,79,79,79,215,79,85,85,85,85,85,222,223,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,128,
    129,130,70,132,133,134,135,94,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,
    161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,50,51,180,181,182,183,184,49,186,187,188,189,190,191,192,
    193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,
    225,226,227,228,229,230,231,232,233,234,234,236,237,237,239,239,241,242,243,243,245,245,247,248,249,250,251,252,0,0,255
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,65,
    65,67,69,78,79,85,65,65,65,65,65,65,67,69,69,69,69,73,73,73,73,78,79,79,79,79,79,85,85,85,85,160,
    161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,
    193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,
    225,226,227,228,229,230,231,232,233,234,234,236,237,237,239,239,241,242,243,243,245,245,247,248,249,250,251,252,253,254,255
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,0,0,
    0,130,70,132,133,134,135,0,137,83,139,140,0,0,0,0,145,146,147,148,149,45,45,152,153,83,155,140,0,0,89,9,
    161,162,163,164,165,166,167,168,169,65,171,172,45,174,175,176,177,50,51,180,181,182,183,184,49,79,187,188,189,190,191,65,
    65,65,65,65,65,198,199,69,69,69,69,73,73,73,73,208,78,79,79,79,79,214,215,79,85,85,85,220,221,222,223,65,
    65,65,65,65,65,198,199,69,69,69,69,73,73,73,73,208,78,79,79,79,79,214,247,79,85,85,85,220,221,222,89
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,0,0,
    0,130,70,132,133,134,135,0,137,83,139,140,0,0,0,0,145,146,147,148,149,45,45,152,153,83,155,140,0,0,89,9,
    161,162,163,164,165,166,167,168,169,65,171,172,45,174,175,176,177,50,51,180,181,182,183,184,49,79,187,188,189,190,191,65,
    65,65,65,196,197,196,67,69,69,69,69,73,73,73,73,68,78,79,79,79,79,214,215,214,85,85,85,89,89,222,223,65,
    65,65,65,196,197,196,67,69,69,69,69,73,73,73,73,68,78,79,79,79,79,214,247,214,85,85,85,89,89,222,89
  },
  {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,0,128,
    129,67,69,78,133,89,65,65,65,128,65,129,67,69,69,69,69,73,73,73,73,78,79,79,79,133,79,85,85,85,89,160,
    161,162,163,164,165,166,167,168,169,170,171,172,173,128,133,176,177,178,179,180,181,182,183,184,185,186,65,79,189,128,133,192,
    193,194,195,70,197,198,199,200,201,9,65,65,79,206,206,45,45,210,211,212,213,214,215,89,89,218,219,220,221,63,63,224,
    225,226,227,228,65,69,65,69,69,73,73,73,73,79,79,63,79,85,85,85,73,0,247,0,249,250,251,63,253,254,0
  },
};
static const struct { unsigned char klass, win, mac; } lhash_classes[] = {
  {0x10, 0, 1},
  {0xd0, 2, 3},
  {0x70, 4, 4},
  {0x20, 5, 6},
  {0x80, 7, 8},
  {0x30, 9, 10},
  {0x90, 11, 11},
  {0x40, 12, 12},
  {0x50, 4, 4},
  {0xe0, 13, 14},
  {0xa0, 15, 15},
  {0xb0, 16, 17},
};
static const unsigned char lhash_wide[0x150] = {
  65,65,65,65,65,65,67,67,67,67,67,67,67,67,68,68,68,68,69,69,69,69,69,69,
  69,69,69,69,71,71,71,71,71,71,71,71,72,72,72,72,73,73,73,73,73,73,73,73,
  73,73,63,63,74,74,75,75,63,76,76,76,76,76,76,63,63,76,76,78,78,78,78,78,
  78,63,63,63,79,79,79,79,79,79,140,140,82,82,82,82,82,82,83,83,83,83,83,83,
  83,83,84,84,84,84,84,84,85,85,85,85,85,85,85,85,85,85,85,85,86,86,85,85,
  85,90,90,90,90,127,127,63,66,63,63,63,63,63,63,63,63,68,63,63,63,63,63,63,
  63,70,70,63,63,63,63,73,63,63,76,63,63,63,63,79,79,79,63,63,63,63,63,63,
  63,63,63,84,63,63,84,85,85,63,63,63,63,63,90,63,63,63,63,63,63,63,63,63,
  124,63,63,33,63,63,63,63,63,63,63,63,63,65,65,73,73,79,79,85,85,85,85,85,
  85,85,85,85,85,63,65,65,63,63,63,63,71,71,71,71,75,75,79,79,79,79,63,63,
  74,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,
  63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,
  63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,
  63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,63,
};

static const struct { unsigned short wide; unsigned char byte; } lhash_wide_extra[] = {
  {0x261,71}, {0x2b9,39}, {0x2ba,34}, {0x2bc,39}, {0x2c4,94}, {0x2c6,127},
  {0x2c8,39}, {0x2c9,175}, {0x2ca,180}, {0x2cb,96}, {0x2cd,95}, {0x2da,176},
  {0x2dc,152}, {0x300,96}, {0x301,180}, {0x302,94}, {0x303,126}, {0x304,175},
  {0x305,175}, {0x308,168}, {0x30a,176}, {0x30e,34}, {0x327,184}, {0x331,95},
  {0x332,95}, {0x37e,59}, {0x393,71}, {0x398,84}, {0x3a3,83}, {0x3a6,70},
  {0x3a9,79}, {0x3b1,65}, {0x3b2,223}, {0x3b4,68}, {0x3b5,69}, {0x3bc,181},
  {0x3c0,80}, {0x3c3,83}, {0x3c4,84}, {0x3c6,70}, {0x4bb,72}, {0x589,58},
  {0x66a,37},
};

static ULONG lhash_class_for(LCID lcid) {
  if ((lcid & 0xFFFF) == 0x0814) return 0xB0;     /* Norwegian (Nynorsk) */
  switch (lcid & 0x3FF) {
    case 0x01: case 0x29: return 0xD0;            /* Arabic, Farsi */
    case 0x04: return 0x70;                       /* Chinese */
    case 0x12: return 0x50;                       /* Korean */
    case 0x05: case 0x0A: case 0x0E: case 0x15: case 0x1B: return 0x20;   /* Czech, Spanish, Hungarian, Polish, Slovak */
    case 0x08: return 0x80;                       /* Greek */
    case 0x0D: return 0x30;                       /* Hebrew */
    case 0x0F: return 0x90;                       /* Icelandic */
    case 0x11: return 0x40;                       /* Japanese */
    case 0x19: return 0xE0;                       /* Russian */
    case 0x1F: return 0xA0;                       /* Turkish */
    default: return 0x10;
  }
}

/* The byte a wide character is hashed as (what the ANSI code page makes of it, with the nearest base letter for the Latin letters it lacks). */
static unsigned lhash_wide_byte(WCHAR c) {
  if (c < 0x80) return c;
  if (c < 0xA0) return c == 0x81 || c == 0x8D || c == 0x8F || c == 0x90 || c == 0x9D ? c : '?';
  if (c < 0x100) return c;
  if (c < 0x100 + sizeof(lhash_wide)) return lhash_wide[c - 0x100];
  for (SIZE_T i = 0; i < OA_ARRAY_SIZE(lhash_wide_extra); i++) if (lhash_wide_extra[i].wide == c) return lhash_wide_extra[i].byte;
  return '?';
}

static ULONG lhash_finish(SYSKIND kind, LCID lcid, ULONG state) {
  const ULONG klass = lhash_class_for(lcid);
  return (state % 65599u) | ((klass + (kind == SYS_MAC ? 1u : 0u)) << 16);
}

static const unsigned char *lhash_table(SYSKIND kind, LCID lcid) {
  const ULONG klass = lhash_class_for(lcid);
  for (SIZE_T i = 0; i < OA_ARRAY_SIZE(lhash_classes); i++)
    if (lhash_classes[i].klass == klass) return lhash_tables[kind == SYS_MAC ? lhash_classes[i].mac : lhash_classes[i].win];
  return lhash_tables[0];
}

EXPORT ULONG WINAPI LHashValOfNameSysA(SYSKIND kind, LCID lcid, LPCSTR name) {
  const unsigned char *table = lhash_table(kind, lcid);
  ULONG state = 0x0DEADBEEu;
  if (name) for (; *name; name++) state = state * 37u + table[(unsigned char)*name - 1];
  return lhash_finish(kind, lcid, state);
}

EXPORT ULONG WINAPI LHashValOfNameSys(SYSKIND kind, LCID lcid, LPCOLESTR name) {
  const unsigned char *table = lhash_table(kind, lcid);
  ULONG state = 0x0DEADBEEu;
  if (name) for (; *name; name++) state = state * 37u + table[lhash_wide_byte(*name) - 1];
  return lhash_finish(kind, lcid, state);
}
