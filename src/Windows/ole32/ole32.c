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

/* ole32.dll for TheoryCraftOS: the COM runtime inside one process. Memory (CoTaskMem*), the apartment count of a thread, GUID strings and ProgIDs, class objects and
 * CoCreateInstance for in-process servers (registered in HKEY_CLASSES_ROOT\CLSID\{...}\InprocServer32, or registered by the process with CoRegisterClassObject), bind contexts,
 * streams over memory, and the clean-up of storage media and property variants. Nothing crosses processes: there is no RPC, so out-of-process servers, marshalling,
 * monikers and the clipboard are not here (their exports are the stubs of src/Windows/stubs/ole32.inc). Built freestanding over kernel32.dll and advapi32.dll's registry. */
#define _OLE32_
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <oleauto.h>

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define EXPORT __declspec(dllexport)
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static CRITICAL_SECTION g_lock;
static DWORD g_tls_apartment = TLS_OUT_OF_INDEXES;
static LONG g_servers;

/* ---- memory ------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT LPVOID WINAPI CoTaskMemAlloc(SIZE_T size) { return HeapAlloc(GetProcessHeap(), 0, size ? size : 1); }
EXPORT void WINAPI CoTaskMemFree(LPVOID p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
EXPORT LPVOID WINAPI CoTaskMemRealloc(LPVOID p, SIZE_T size) {
  if (!p) return CoTaskMemAlloc(size);
  if (!size) { CoTaskMemFree(p); return NULL; }
  return HeapReAlloc(GetProcessHeap(), 0, p, size);
}
static HRESULT STDMETHODCALLTYPE m_query(IMalloc *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMalloc)) { *out = self; return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE m_addref(IMalloc *self) { (void)self; return 1; }
static ULONG STDMETHODCALLTYPE m_release(IMalloc *self) { (void)self; return 1; }
static void *STDMETHODCALLTYPE m_alloc(IMalloc *self, SIZE_T n) { (void)self; return CoTaskMemAlloc(n); }
static void *STDMETHODCALLTYPE m_realloc(IMalloc *self, void *p, SIZE_T n) { (void)self; return CoTaskMemRealloc(p, n); }
static void STDMETHODCALLTYPE m_free(IMalloc *self, void *p) { (void)self; CoTaskMemFree(p); }
static SIZE_T STDMETHODCALLTYPE m_size(IMalloc *self, void *p) { (void)self; return p ? HeapSize(GetProcessHeap(), 0, p) : (SIZE_T)-1; }
static int STDMETHODCALLTYPE m_did(IMalloc *self, void *p) { (void)self; return p ? (HeapValidate(GetProcessHeap(), 0, p) ? 1 : 0) : -1; }
static void STDMETHODCALLTYPE m_min(IMalloc *self) { (void)self; }
static const IMallocVtbl g_malloc_vtbl = {m_query, m_addref, m_release, m_alloc, m_realloc, m_free, m_size, m_did, m_min};
static IMalloc g_malloc = {(IMallocVtbl *)&g_malloc_vtbl};
EXPORT HRESULT WINAPI CoGetMalloc(DWORD context, LPMALLOC *out) {
  if (!out) return E_INVALIDARG;
  if (context != 1) { *out = NULL; return E_INVALIDARG; }
  *out = &g_malloc;
  return S_OK;
}

/* ---- apartments --------------------------------------------------------------------------------------------------------------------------------------------- */

/* The thread's apartment: 0 not initialised, else the count in the low 16 bits and the mode (COINIT_MULTITHREADED = 0, apartment-threaded = 2) in bit 16-17 (+1 when set). */
static HRESULT apartment_enter(DWORD mode) {
  const DWORD_PTR state = (DWORD_PTR)TlsGetValue(g_tls_apartment);
  const DWORD_PTR kind = (mode & COINIT_APARTMENTTHREADED) ? 2 : 1;
  if (!state) { TlsSetValue(g_tls_apartment, (LPVOID)((kind << 16) | 1)); return S_OK; }
  if (((state >> 16) & 3) != kind) return RPC_E_CHANGED_MODE;
  if ((state & 0xFFFF) == 0xFFFF) return S_OK;
  TlsSetValue(g_tls_apartment, (LPVOID)(state + 1));
  return S_FALSE;
}
EXPORT HRESULT WINAPI CoInitializeEx(LPVOID reserved, DWORD mode) {
  if (reserved) return E_INVALIDARG;
  return apartment_enter(mode);
}
EXPORT HRESULT WINAPI CoInitialize(LPVOID reserved) { return CoInitializeEx(reserved, COINIT_APARTMENTTHREADED); }
EXPORT void WINAPI CoUninitialize(void) {
  const DWORD_PTR state = (DWORD_PTR)TlsGetValue(g_tls_apartment);
  if (!state) return;
  if ((state & 0xFFFF) <= 1) TlsSetValue(g_tls_apartment, NULL); else TlsSetValue(g_tls_apartment, (LPVOID)(state - 1));
}
EXPORT HRESULT WINAPI OleInitialize(LPVOID reserved) { return CoInitializeEx(reserved, COINIT_APARTMENTTHREADED); }
EXPORT void WINAPI OleUninitialize(void) { CoUninitialize(); }
EXPORT HRESULT WINAPI CoInitializeSecurity(PSECURITY_DESCRIPTOR sd, LONG count, SOLE_AUTHENTICATION_SERVICE *names, void *reserved, DWORD level, DWORD impersonation, void *auth, DWORD caps, void *reserved3) {
  (void)sd; (void)count; (void)names; (void)reserved; (void)level; (void)impersonation; (void)auth; (void)caps; (void)reserved3;
  return S_OK;
}
EXPORT HRESULT WINAPI CoSetProxyBlanket(IUnknown *proxy, DWORD authn, DWORD authz, OLECHAR *server, DWORD level, DWORD impersonation, RPC_AUTH_IDENTITY_HANDLE identity, DWORD caps) {
  (void)proxy; (void)authn; (void)authz; (void)server; (void)level; (void)impersonation; (void)identity; (void)caps;
  return S_OK;
}

/* ---- GUIDs and names ------------------------------------------------------------------------------------------------------------------------------------------ */

static const WCHAR kHex[] = L"0123456789ABCDEF";
EXPORT int WINAPI StringFromGUID2(REFGUID guid, LPOLESTR out, int capacity) {
  if (capacity < 39) return 0;
  WCHAR *p = out;
  *p++ = '{';
  for (int i = 0; i < 8; ++i) *p++ = kHex[(guid->Data1 >> (28 - 4 * i)) & 15];
  *p++ = '-';
  for (int i = 0; i < 4; ++i) *p++ = kHex[(guid->Data2 >> (12 - 4 * i)) & 15];
  *p++ = '-';
  for (int i = 0; i < 4; ++i) *p++ = kHex[(guid->Data3 >> (12 - 4 * i)) & 15];
  *p++ = '-';
  for (int i = 0; i < 2; ++i) { *p++ = kHex[guid->Data4[i] >> 4]; *p++ = kHex[guid->Data4[i] & 15]; }
  *p++ = '-';
  for (int i = 2; i < 8; ++i) { *p++ = kHex[guid->Data4[i] >> 4]; *p++ = kHex[guid->Data4[i] & 15]; }
  *p++ = '}';
  *p = 0;
  return 39;
}
EXPORT HRESULT WINAPI StringFromCLSID(REFCLSID clsid, LPOLESTR *out) {
  if (!out) return E_INVALIDARG;
  *out = CoTaskMemAlloc(39 * sizeof(WCHAR));
  if (!*out) return E_OUTOFMEMORY;
  StringFromGUID2(clsid, *out, 39);
  return S_OK;
}
EXPORT HRESULT WINAPI StringFromIID(REFIID iid, LPOLESTR *out) { return StringFromCLSID(iid, out); }
static int hex_value(WCHAR c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static BOOL parse_hex(const WCHAR *s, int digits, unsigned long long *value) {
  unsigned long long v = 0;
  for (int i = 0; i < digits; ++i) { const int d = hex_value(s[i]); if (d < 0) return FALSE; v = v << 4 | (unsigned)d; }
  *value = v;
  return TRUE;
}
/* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" */
static BOOL parse_guid(const WCHAR *s, GUID *g) {
  unsigned long long v;
  if (s[0] != '{') return FALSE;
  if (!parse_hex(s + 1, 8, &v)) return FALSE; g->Data1 = (DWORD)v;
  if (s[9] != '-' || !parse_hex(s + 10, 4, &v)) return FALSE; g->Data2 = (WORD)v;
  if (s[14] != '-' || !parse_hex(s + 15, 4, &v)) return FALSE; g->Data3 = (WORD)v;
  if (s[19] != '-') return FALSE;
  if (!parse_hex(s + 20, 2, &v)) return FALSE; g->Data4[0] = (BYTE)v;
  if (!parse_hex(s + 22, 2, &v)) return FALSE; g->Data4[1] = (BYTE)v;
  if (s[24] != '-') return FALSE;
  for (int i = 0; i < 6; ++i) { if (!parse_hex(s + 25 + 2 * i, 2, &v)) return FALSE; g->Data4[2 + i] = (BYTE)v; }
  return s[37] == '}' && s[38] == 0;
}
static HRESULT lookup_progid(const WCHAR *progid, CLSID *clsid) {
  WCHAR key[300], text[64];
  DWORD size = sizeof(text);
  HKEY k;
  if (wcslen(progid) > 200) return CO_E_CLASSSTRING;
  lstrcpyW(key, progid);
  lstrcatW(key, L"\\CLSID");
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return CO_E_CLASSSTRING;
  const LONG r = RegQueryValueExW(k, NULL, NULL, NULL, (BYTE *)text, &size);
  RegCloseKey(k);
  if (r != ERROR_SUCCESS || size >= sizeof(text)) return CO_E_CLASSSTRING;
  text[size / sizeof(WCHAR)] = 0;
  return parse_guid(text, clsid) ? S_OK : CO_E_CLASSSTRING;
}
EXPORT HRESULT WINAPI CLSIDFromProgID(LPCOLESTR progid, LPCLSID clsid) {
  if (!progid || !clsid) return E_INVALIDARG;
  const HRESULT hr = lookup_progid(progid, clsid);
  return hr == S_OK ? S_OK : CO_E_CLASSSTRING;
}
EXPORT HRESULT WINAPI CLSIDFromString(LPCOLESTR text, LPCLSID clsid) {
  if (!clsid) return E_INVALIDARG;
  if (!text) { ZeroMemory(clsid, sizeof(*clsid)); return S_OK; }
  GUID g;
  if (text[0] == '{') { if (!parse_guid(text, &g)) return CO_E_CLASSSTRING; *clsid = g; return S_OK; }
  return CLSIDFromProgID(text, clsid);
}
EXPORT HRESULT WINAPI IIDFromString(LPCOLESTR text, LPIID iid) {
  if (!iid) return E_INVALIDARG;
  if (!text) { ZeroMemory(iid, sizeof(*iid)); return S_OK; }
  GUID g;
  if (!parse_guid(text, &g)) return CO_E_IIDSTRING;
  *iid = g;
  return S_OK;
}
EXPORT HRESULT WINAPI ProgIDFromCLSID(REFCLSID clsid, LPOLESTR *progid) {
  WCHAR key[100], text[100];
  DWORD size = sizeof(text);
  HKEY k;
  if (!progid) return E_INVALIDARG;
  *progid = NULL;
  lstrcpyW(key, L"CLSID\\");
  StringFromGUID2(clsid, key + 6, 40);
  lstrcatW(key, L"\\ProgID");
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return REGDB_E_CLASSNOTREG;
  const LONG r = RegQueryValueExW(k, NULL, NULL, NULL, (BYTE *)text, &size);
  RegCloseKey(k);
  if (r != ERROR_SUCCESS || size >= sizeof(text)) return REGDB_E_CLASSNOTREG;
  text[size / sizeof(WCHAR)] = 0;
  *progid = CoTaskMemAlloc((wcslen(text) + 1) * sizeof(WCHAR));
  if (!*progid) return E_OUTOFMEMORY;
  lstrcpyW(*progid, text);
  return S_OK;
}
/* A random (version 4) GUID. */
EXPORT HRESULT WINAPI CoCreateGuid(GUID *guid) {
  static ULONGLONG state;
  LARGE_INTEGER counter;
  FILETIME now;
  if (!guid) return E_INVALIDARG;
  EnterCriticalSection(&g_lock);
  QueryPerformanceCounter(&counter);
  GetSystemTimeAsFileTime(&now);
  if (!state) state = 0x9E3779B97F4A7C15ull ^ (ULONGLONG)GetCurrentProcessId() << 32 ^ GetTickCount();
  BYTE bytes[16];
  for (int i = 0; i < 2; ++i) {
    state ^= (ULONGLONG)counter.QuadPart + ((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime);
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;      /* xorshift64 */
    state *= 0x2545F4914F6CDD1Dull;
    CopyMemory(bytes + 8 * i, &state, 8);
  }
  LeaveCriticalSection(&g_lock);
  CopyMemory(guid, bytes, 16);
  guid->Data3 = (guid->Data3 & 0x0FFF) | 0x4000;
  guid->Data4[0] = (guid->Data4[0] & 0x3F) | 0x80;
  return S_OK;
}

/* ---- class objects ---------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { DWORD cookie; CLSID clsid; IUnknown *object; DWORD flags; } Registration;
static Registration g_registrations[32];
static DWORD g_next_cookie = 1;

EXPORT HRESULT WINAPI CoRegisterClassObject(REFCLSID clsid, IUnknown *object, DWORD context, DWORD flags, LPDWORD cookie) {
  if (!object || !cookie) return E_INVALIDARG;
  *cookie = 0;
  if (!(context & CLSCTX_INPROC_SERVER) && !(context & CLSCTX_LOCAL_SERVER)) return E_INVALIDARG;
  HRESULT hr = CO_E_OBJISREG;
  EnterCriticalSection(&g_lock);
  for (size_t i = 0; i < ARRAY_LEN(g_registrations); ++i) {
    if (g_registrations[i].cookie) continue;
    g_registrations[i].cookie = g_next_cookie++;
    g_registrations[i].clsid = *clsid;
    g_registrations[i].object = object;
    g_registrations[i].flags = flags;
    IUnknown_AddRef(object);
    *cookie = g_registrations[i].cookie;
    hr = S_OK;
    break;
  }
  LeaveCriticalSection(&g_lock);
  return hr;
}
EXPORT HRESULT WINAPI CoRevokeClassObject(DWORD cookie) {
  IUnknown *object = NULL;
  EnterCriticalSection(&g_lock);
  for (size_t i = 0; i < ARRAY_LEN(g_registrations); ++i) {
    if (g_registrations[i].cookie != cookie || !cookie) continue;
    object = g_registrations[i].object;
    ZeroMemory(&g_registrations[i], sizeof(g_registrations[i]));
    break;
  }
  LeaveCriticalSection(&g_lock);
  if (!object) return E_INVALIDARG;
  IUnknown_Release(object);
  return S_OK;
}

/* Servers loaded for classes, so that CoFreeUnusedLibraries can let go of them. */
static HMODULE g_loaded[32];
static HRESULT server_class_object(HMODULE module, REFCLSID clsid, REFIID riid, void **out) {
  typedef HRESULT (WINAPI *GetClassObject)(REFCLSID, REFIID, void **);
  GetClassObject get = (GetClassObject)GetProcAddress(module, "DllGetClassObject");
  if (!get) return CO_E_DLLNOTFOUND;
  return get(clsid, riid, out);
}
/* The libraries that serve classes the system itself provides, for when the registry (which a fresh system has little in) does not name them. */
static const struct { GUID clsid; const WCHAR *dll; } g_builtin[] = {
  {{0x00021401, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, L"shell32.dll"},      /* CLSID_ShellLink */
  {{0x9AC9FBE1, 0xE0A2, 0x4AD6, {0xB4, 0xEE, 0xE2, 0x12, 0x01, 0x3E, 0xA9, 0x17}}, L"shell32.dll"},      /* CLSID_ShellItem */
  {{0x00021400, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}, L"shell32.dll"},      /* CLSID_ShellDesktop */
  {{0xF3364BA0, 0x65B9, 0x11CE, {0xA9, 0xBA, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}}, L"shell32.dll"},      /* CLSID_ShellFSFolder */
  {{0x20D04FE0, 0x3AEA, 0x1069, {0xA2, 0xD8, 0x08, 0x00, 0x2B, 0x30, 0x30, 0x9D}}, L"shell32.dll"},      /* CLSID_MyComputer */
  {{0x4DF0C730, 0xDF9D, 0x4AE3, {0x91, 0x53, 0xAA, 0x6B, 0x82, 0xE9, 0x79, 0x5A}}, L"shell32.dll"},      /* CLSID_KnownFolderManager */
};

EXPORT HRESULT WINAPI CoGetClassObject(REFCLSID clsid, DWORD context, LPVOID server_info, REFIID riid, LPVOID *out) {
  (void)server_info;
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!clsid || !riid) return E_INVALIDARG;
  /* registered by this process */
  IUnknown *object = NULL;
  EnterCriticalSection(&g_lock);
  for (size_t i = 0; i < ARRAY_LEN(g_registrations); ++i)
    if (g_registrations[i].cookie && IsEqualGUID(&g_registrations[i].clsid, clsid)) { object = g_registrations[i].object; IUnknown_AddRef(object); break; }
  LeaveCriticalSection(&g_lock);
  if (object) {
    const HRESULT hr = IUnknown_QueryInterface(object, riid, out);
    IUnknown_Release(object);
    return hr;
  }
  if (!(context & (CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER))) return REGDB_E_CLASSNOTREG;
  /* the registry names the library */
  WCHAR key[100], dll[MAX_PATH];
  DWORD size = sizeof(dll);
  HKEY k;
  const WCHAR *library = NULL;
  lstrcpyW(key, L"CLSID\\");
  StringFromGUID2(clsid, key + 6, 40);
  lstrcatW(key, L"\\InprocServer32");
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &k) == ERROR_SUCCESS) {
    DWORD type;
    if (RegQueryValueExW(k, NULL, &type, NULL, (BYTE *)dll, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(WCHAR)) {
      dll[size / sizeof(WCHAR)] = 0;
      library = dll;
      if (type == REG_EXPAND_SZ) { WCHAR expanded[MAX_PATH]; if (ExpandEnvironmentStringsW(dll, expanded, MAX_PATH)) lstrcpyW(dll, expanded); }
    }
    RegCloseKey(k);
  }
  if (!library) for (size_t i = 0; i < ARRAY_LEN(g_builtin); ++i) if (IsEqualGUID(&g_builtin[i].clsid, clsid)) library = g_builtin[i].dll;
  if (!library) return REGDB_E_CLASSNOTREG;
  HMODULE module = LoadLibraryW(library);
  if (!module) return HRESULT_FROM_WIN32(GetLastError() == ERROR_MOD_NOT_FOUND ? ERROR_MOD_NOT_FOUND : GetLastError());
  const HRESULT hr = server_class_object(module, clsid, riid, out);
  if (FAILED(hr)) { FreeLibrary(module); return hr; }
  EnterCriticalSection(&g_lock);
  BOOL kept = FALSE;
  for (size_t i = 0; i < ARRAY_LEN(g_loaded) && !kept; ++i) if (!g_loaded[i]) { g_loaded[i] = module; kept = TRUE; }
  LeaveCriticalSection(&g_lock);
  if (!kept) {}      /* (the library stays loaded for good) */
  return hr;
}
EXPORT HRESULT WINAPI CoCreateInstanceEx(REFCLSID clsid, IUnknown *outer, DWORD context, COSERVERINFO *server, DWORD count, MULTI_QI *results) {
  if (server) return E_NOTIMPL;
  if (!results || !count) return E_INVALIDARG;
  IClassFactory *factory = NULL;
  HRESULT hr = CoGetClassObject(clsid, context, NULL, &IID_IClassFactory, (void **)&factory);
  if (FAILED(hr)) { for (DWORD i = 0; i < count; ++i) { results[i].pItf = NULL; results[i].hr = hr; } return hr; }
  IUnknown *object = NULL;
  hr = IClassFactory_CreateInstance(factory, outer, &IID_IUnknown, (void **)&object);
  IClassFactory_Release(factory);
  if (FAILED(hr)) { for (DWORD i = 0; i < count; ++i) { results[i].pItf = NULL; results[i].hr = hr; } return hr; }
  DWORD succeeded = 0;
  for (DWORD i = 0; i < count; ++i) {
    results[i].hr = IUnknown_QueryInterface(object, results[i].pIID, (void **)&results[i].pItf);
    if (SUCCEEDED(results[i].hr)) ++succeeded;
  }
  IUnknown_Release(object);
  return succeeded == count ? S_OK : (succeeded ? CO_S_NOTALLINTERFACES : E_NOINTERFACE);
}
EXPORT HRESULT WINAPI CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID riid, LPVOID *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  if (!clsid || !riid) return E_INVALIDARG;
  if (outer && !IsEqualIID(riid, &IID_IUnknown)) return CLASS_E_NOAGGREGATION;
  IClassFactory *factory = NULL;
  HRESULT hr = CoGetClassObject(clsid, context, NULL, &IID_IClassFactory, (void **)&factory);
  if (FAILED(hr)) return hr;
  hr = IClassFactory_CreateInstance(factory, outer, riid, out);
  IClassFactory_Release(factory);
  return hr;
}
EXPORT HMODULE WINAPI CoLoadLibrary(LPOLESTR name, BOOL auto_free) { (void)auto_free; return LoadLibraryW(name); }
EXPORT void WINAPI CoFreeLibrary(HMODULE module) { FreeLibrary(module); }
EXPORT void WINAPI CoFreeUnusedLibraries(void) {
  typedef HRESULT (WINAPI *CanUnload)(void);
  EnterCriticalSection(&g_lock);
  for (size_t i = 0; i < ARRAY_LEN(g_loaded); ++i) {
    if (!g_loaded[i]) continue;
    CanUnload can = (CanUnload)GetProcAddress(g_loaded[i], "DllCanUnloadNow");
    if (can && can() == S_OK) { FreeLibrary(g_loaded[i]); g_loaded[i] = NULL; }
  }
  LeaveCriticalSection(&g_lock);
}
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return g_servers ? S_FALSE : S_OK; }

/* ---- bind contexts ------------------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct { LPOLESTR key; IUnknown *object; } BindParam;
typedef struct { const IBindCtxVtbl *vtbl; LONG ref; BIND_OPTS2 options; BindParam params[16]; } BindContext;

static HRESULT STDMETHODCALLTYPE bc_query(IBindCtx *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IBindCtx)) { *out = self; InterlockedIncrement(&((BindContext *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE bc_addref(IBindCtx *self) { return (ULONG)InterlockedIncrement(&((BindContext *)self)->ref); }
static void bind_params_release(BindContext *c) {
  for (size_t i = 0; i < ARRAY_LEN(c->params); ++i) {
    if (c->params[i].object) IUnknown_Release(c->params[i].object);
    if (c->params[i].key) CoTaskMemFree(c->params[i].key);
    c->params[i].object = NULL; c->params[i].key = NULL;
  }
}
static ULONG STDMETHODCALLTYPE bc_release(IBindCtx *self) {
  BindContext *c = (BindContext *)self;
  const LONG n = InterlockedDecrement(&c->ref);
  if (!n) { bind_params_release(c); HeapFree(GetProcessHeap(), 0, c); }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE bc_register_object(IBindCtx *self, IUnknown *object) { (void)self; (void)object; return S_OK; }
static HRESULT STDMETHODCALLTYPE bc_revoke_object(IBindCtx *self, IUnknown *object) { (void)self; (void)object; return S_OK; }
static HRESULT STDMETHODCALLTYPE bc_release_objects(IBindCtx *self) { bind_params_release((BindContext *)self); return S_OK; }
static HRESULT STDMETHODCALLTYPE bc_set_options(IBindCtx *self, BIND_OPTS *options) {
  BindContext *c = (BindContext *)self;
  if (!options || options->cbStruct < sizeof(BIND_OPTS) || options->cbStruct > sizeof(BIND_OPTS2)) return E_INVALIDARG;
  CopyMemory(&c->options, options, options->cbStruct);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bc_get_options(IBindCtx *self, BIND_OPTS *options) {
  BindContext *c = (BindContext *)self;
  if (!options || options->cbStruct < sizeof(BIND_OPTS) || options->cbStruct > sizeof(BIND_OPTS2)) return E_INVALIDARG;
  const DWORD size = options->cbStruct;
  CopyMemory(options, &c->options, size);
  options->cbStruct = size;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE bc_running(IBindCtx *self, IRunningObjectTable **out) { (void)self; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE bc_register_param(IBindCtx *self, LPOLESTR key, IUnknown *object) {
  BindContext *c = (BindContext *)self;
  if (!key) return E_INVALIDARG;
  for (size_t i = 0; i < ARRAY_LEN(c->params); ++i) {
    if (c->params[i].key && !lstrcmpiW(c->params[i].key, key)) {
      if (c->params[i].object) IUnknown_Release(c->params[i].object);
      c->params[i].object = object;
      if (object) IUnknown_AddRef(object);
      return S_OK;
    }
  }
  for (size_t i = 0; i < ARRAY_LEN(c->params); ++i) {
    if (c->params[i].key) continue;
    c->params[i].key = CoTaskMemAlloc((wcslen(key) + 1) * sizeof(WCHAR));
    if (!c->params[i].key) return E_OUTOFMEMORY;
    lstrcpyW(c->params[i].key, key);
    c->params[i].object = object;
    if (object) IUnknown_AddRef(object);
    return S_OK;
  }
  return E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE bc_get_param(IBindCtx *self, LPOLESTR key, IUnknown **out) {
  BindContext *c = (BindContext *)self;
  if (!out) return E_POINTER;
  *out = NULL;
  for (size_t i = 0; i < ARRAY_LEN(c->params); ++i) {
    if (c->params[i].key && key && !lstrcmpiW(c->params[i].key, key)) {
      *out = c->params[i].object;
      if (*out) IUnknown_AddRef(*out);
      return *out ? S_OK : E_FAIL;
    }
  }
  return E_FAIL;
}
static HRESULT STDMETHODCALLTYPE bc_enum_keys(IBindCtx *self, IEnumString **out) { (void)self; if (out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE bc_revoke_param(IBindCtx *self, LPOLESTR key) {
  BindContext *c = (BindContext *)self;
  for (size_t i = 0; i < ARRAY_LEN(c->params); ++i) {
    if (c->params[i].key && key && !lstrcmpiW(c->params[i].key, key)) {
      if (c->params[i].object) IUnknown_Release(c->params[i].object);
      CoTaskMemFree(c->params[i].key);
      c->params[i].key = NULL; c->params[i].object = NULL;
      return S_OK;
    }
  }
  return E_FAIL;
}
static const IBindCtxVtbl g_bind_vtbl = {bc_query, bc_addref, bc_release, bc_register_object, bc_revoke_object, bc_release_objects, bc_set_options, bc_get_options, bc_running,
                                         bc_register_param, bc_get_param, bc_enum_keys, bc_revoke_param};
EXPORT HRESULT WINAPI CreateBindCtx(DWORD reserved, IBindCtx **out) {
  if (reserved) return E_INVALIDARG;
  if (!out) return E_POINTER;
  BindContext *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
  if (!c) { *out = NULL; return E_OUTOFMEMORY; }
  c->vtbl = &g_bind_vtbl;
  c->ref = 1;
  c->options.cbStruct = sizeof(BIND_OPTS2);
  c->options.grfMode = STGM_READWRITE;
  c->options.dwTickCountDeadline = 0;
  c->options.dwClassContext = CLSCTX_SERVER;
  c->options.locale = GetThreadLocale();
  *out = (IBindCtx *)c;
  return S_OK;
}

/* ---- streams over memory ------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct { const IStreamVtbl *vtbl; LONG ref; HGLOBAL global; BOOL delete_on_release; ULONGLONG size, position; } MemoryStream;

static HRESULT STDMETHODCALLTYPE ms_query(IStream *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStream) || IsEqualIID(riid, &IID_ISequentialStream)) { *out = self; InterlockedIncrement(&((MemoryStream *)self)->ref); return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ms_addref(IStream *self) { return (ULONG)InterlockedIncrement(&((MemoryStream *)self)->ref); }
static ULONG STDMETHODCALLTYPE ms_release(IStream *self) {
  MemoryStream *s = (MemoryStream *)self;
  const LONG n = InterlockedDecrement(&s->ref);
  if (!n) { if (s->delete_on_release && s->global) GlobalFree(s->global); HeapFree(GetProcessHeap(), 0, s); }
  return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE ms_read(IStream *self, void *buffer, ULONG count, ULONG *read) {
  MemoryStream *s = (MemoryStream *)self;
  ULONG n = 0;
  if (!buffer) return STG_E_INVALIDPOINTER;
  if (s->position < s->size) {
    const ULONGLONG left = s->size - s->position;
    n = left < count ? (ULONG)left : count;
    BYTE *p = GlobalLock(s->global);
    if (!p) { if (read) *read = 0; return E_OUTOFMEMORY; }
    CopyMemory(buffer, p + s->position, n);
    GlobalUnlock(s->global);
    s->position += n;
  }
  if (read) *read = n;
  return S_OK;
}
static HRESULT memory_stream_grow(MemoryStream *s, ULONGLONG size) {
  const SIZE_T have = GlobalSize(s->global);
  if (size <= have) return S_OK;
  if (size > 0x7FFFFFFF) return STG_E_MEDIUMFULL;
  SIZE_T want = have * 2;
  if (want < size) want = (SIZE_T)size;
  HGLOBAL grown = GlobalReAlloc(s->global, want, GMEM_MOVEABLE | GMEM_ZEROINIT);
  if (!grown) return STG_E_MEDIUMFULL;
  s->global = grown;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_write(IStream *self, const void *buffer, ULONG count, ULONG *written) {
  MemoryStream *s = (MemoryStream *)self;
  if (written) *written = 0;
  if (!buffer) return STG_E_INVALIDPOINTER;
  if (!count) return S_OK;
  if (s->position > 0x7FFFFFFF - count) return STG_E_MEDIUMFULL;
  const HRESULT hr = memory_stream_grow(s, s->position + count);
  if (FAILED(hr)) return hr;
  BYTE *p = GlobalLock(s->global);
  if (!p) return E_OUTOFMEMORY;
  CopyMemory(p + s->position, buffer, count);
  GlobalUnlock(s->global);
  s->position += count;
  if (s->position > s->size) s->size = s->position;
  if (written) *written = count;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_seek(IStream *self, LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out) {
  MemoryStream *s = (MemoryStream *)self;
  LONGLONG base;
  switch (origin) {
    case STREAM_SEEK_SET: base = 0; break;
    case STREAM_SEEK_CUR: base = (LONGLONG)s->position; break;
    case STREAM_SEEK_END: base = (LONGLONG)s->size; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  const LONGLONG target = base + move.QuadPart;
  if (target < 0) return STG_E_INVALIDFUNCTION;
  s->position = (ULONGLONG)target;
  if (out) out->QuadPart = s->position;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_set_size(IStream *self, ULARGE_INTEGER size) {
  MemoryStream *s = (MemoryStream *)self;
  const HRESULT hr = memory_stream_grow(s, size.QuadPart);
  if (FAILED(hr)) return hr;
  if (size.QuadPart > s->size) {
    BYTE *p = GlobalLock(s->global);
    if (p) { ZeroMemory(p + s->size, (SIZE_T)(size.QuadPart - s->size)); GlobalUnlock(s->global); }
  }
  s->size = size.QuadPart;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_copy_to(IStream *self, IStream *dest, ULARGE_INTEGER count, ULARGE_INTEGER *read, ULARGE_INTEGER *written) {
  BYTE buffer[1024];
  ULONGLONG total_read = 0, total_written = 0, left = count.QuadPart;
  HRESULT hr = S_OK;
  if (!dest) return STG_E_INVALIDPOINTER;
  while (left) {
    ULONG got = 0, put = 0;
    const ULONG want = left < sizeof(buffer) ? (ULONG)left : (ULONG)sizeof(buffer);
    hr = ms_read(self, buffer, want, &got);
    if (FAILED(hr) || !got) break;
    total_read += got;
    hr = IStream_Write(dest, buffer, got, &put);
    total_written += put;
    if (FAILED(hr) || put != got) break;
    left -= got;
  }
  if (read) read->QuadPart = total_read;
  if (written) written->QuadPart = total_written;
  return hr;
}
static HRESULT STDMETHODCALLTYPE ms_commit(IStream *self, DWORD flags) { (void)self; (void)flags; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_revert(IStream *self) { (void)self; return S_OK; }
static HRESULT STDMETHODCALLTYPE ms_lock(IStream *self, ULARGE_INTEGER offset, ULARGE_INTEGER length, DWORD type) { (void)self; (void)offset; (void)length; (void)type; return STG_E_INVALIDFUNCTION; }
static HRESULT STDMETHODCALLTYPE ms_stat(IStream *self, STATSTG *stat, DWORD flags) {
  MemoryStream *s = (MemoryStream *)self;
  if (!stat) return STG_E_INVALIDPOINTER;
  ZeroMemory(stat, sizeof(*stat));
  (void)flags;
  stat->type = STGTY_STREAM;
  stat->cbSize.QuadPart = s->size;
  stat->grfMode = STGM_READWRITE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE ms_clone(IStream *self, IStream **out) {
  MemoryStream *s = (MemoryStream *)self;
  if (!out) return STG_E_INVALIDPOINTER;
  MemoryStream *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
  if (!c) return E_OUTOFMEMORY;
  *c = *s;
  c->ref = 1;
  c->delete_on_release = FALSE;      /* (the original owns the memory) */
  *out = (IStream *)c;
  return S_OK;
}
static const IStreamVtbl g_stream_vtbl = {ms_query, ms_addref, ms_release, ms_read, ms_write, ms_seek, ms_set_size, ms_copy_to, ms_commit, ms_revert, ms_lock, ms_lock, ms_stat, ms_clone};

EXPORT HRESULT WINAPI CreateStreamOnHGlobal(HGLOBAL global, BOOL delete_on_release, LPSTREAM *out) {
  if (!out) return E_INVALIDARG;
  *out = NULL;
  BOOL own = FALSE;
  if (!global) { global = GlobalAlloc(GMEM_MOVEABLE, 0); own = TRUE; if (!global) return E_OUTOFMEMORY; }
  MemoryStream *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
  if (!s) { if (own) GlobalFree(global); return E_OUTOFMEMORY; }
  s->vtbl = &g_stream_vtbl;
  s->ref = 1;
  s->global = global;
  s->delete_on_release = own ? TRUE : delete_on_release;
  if (own) s->delete_on_release = delete_on_release;
  s->size = GlobalSize(global);
  *out = (IStream *)s;
  return S_OK;
}
EXPORT HRESULT WINAPI GetHGlobalFromStream(LPSTREAM stream, HGLOBAL *global) {
  if (!stream || !global) return E_INVALIDARG;
  MemoryStream *s = (MemoryStream *)stream;
  if (s->vtbl != &g_stream_vtbl) return E_INVALIDARG;
  *global = s->global;
  return S_OK;
}

/* ---- media and variants -------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT void WINAPI ReleaseStgMedium(STGMEDIUM *medium) {
  if (!medium) return;
  switch (medium->tymed) {
    case TYMED_HGLOBAL: if (!medium->pUnkForRelease) GlobalFree(medium->hGlobal); break;
    case TYMED_FILE: if (!medium->pUnkForRelease) CoTaskMemFree(medium->lpszFileName); break;
    case TYMED_ISTREAM: IStream_Release(medium->pstm); break;
    case TYMED_ISTORAGE: IStorage_Release(medium->pstg); break;
    case TYMED_GDI: if (!medium->pUnkForRelease) DeleteObject(medium->hBitmap); break;
    case TYMED_MFPICT: if (!medium->pUnkForRelease) { GlobalFree(medium->hMetaFilePict); } break;
    case TYMED_ENHMF: if (!medium->pUnkForRelease) DeleteEnhMetaFile(medium->hEnhMetaFile); break;
    default: break;
  }
  if (medium->pUnkForRelease) { IUnknown_Release(medium->pUnkForRelease); medium->pUnkForRelease = NULL; }
  medium->tymed = TYMED_NULL;
}
EXPORT HRESULT WINAPI PropVariantClear(PROPVARIANT *v) {
  if (!v) return S_OK;
  switch (v->vt) {
    case VT_LPSTR: case VT_LPWSTR: CoTaskMemFree(v->pszVal); break;
    case VT_BLOB: case VT_BLOB_OBJECT: CoTaskMemFree(v->blob.pBlobData); break;
    case VT_CF: if (v->pclipdata) { CoTaskMemFree(v->pclipdata->pClipData); CoTaskMemFree(v->pclipdata); } break;
    case VT_STREAM: case VT_STREAMED_OBJECT: case VT_STORAGE: case VT_STORED_OBJECT: case VT_UNKNOWN: if (v->punkVal) IUnknown_Release(v->punkVal); break;
    case VT_CLSID: CoTaskMemFree(v->puuid); break;
    case VT_BSTR: if (v->bstrVal) SysFreeString(v->bstrVal); break;
    default: break;
  }
  ZeroMemory(v, sizeof(*v));
  return S_OK;
}
EXPORT HRESULT WINAPI FreePropVariantArray(ULONG count, PROPVARIANT *v) {
  if (!v) return E_INVALIDARG;
  for (ULONG i = 0; i < count; ++i) PropVariantClear(&v[i]);
  return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) { InitializeCriticalSection(&g_lock); g_tls_apartment = TlsAlloc(); }
  return TRUE;
}
