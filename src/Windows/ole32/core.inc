/* ole32: memory, locks, strings, GUIDs, the per-thread state. */

/* ---- memory (the process heap: CoTaskMemAlloc and the helpers share it) -------------------------------------------------------------------------------------- */

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void *mem_realloc(void *p, SIZE_T n) {
  if (!p) return mem_alloc(n);
  return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n ? n : 1);
}
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static void mem_copy(void *d, const void *s, SIZE_T n) { BYTE *dp = d; const BYTE *sp = s; if (dp == sp) return; if (dp < sp || dp >= sp + n) { while (n--) *dp++ = *sp++; } else { dp += n; sp += n; while (n--) *--dp = *--sp; } }
static void mem_zero(void *d, SIZE_T n) { BYTE *dp = d; while (n--) *dp++ = 0; }
static int mem_cmp(const void *a, const void *b, SIZE_T n) { const BYTE *ap = a, *bp = b; for (; n; n--, ap++, bp++) if (*ap != *bp) return *ap < *bp ? -1 : 1; return 0; }

/* ---- locks --------------------------------------------------------------------------------------------------------------------------------------------------------- */

static CRITICAL_SECTION g_lock;      /* the one lock of the library: held only for short table operations, never across a call out */
static void lock(void) { EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }

/* ---- strings ------------------------------------------------------------------------------------------------------------------------------------------------------- */

static SIZE_T wlen(const WCHAR *s) { SIZE_T n = 0; while (s[n]) n++; return n; }
__declspec(dllimport) WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR c);
static WCHAR wup(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : (c > 127 ? RtlUpcaseUnicodeChar(c) : c); }
static int wicmp(const WCHAR *a, const WCHAR *b) { for (;; a++, b++) { WCHAR x = wup(*a), y = wup(*b); if (x != y) return x < y ? -1 : 1; if (!x) return 0; } }
static int wnicmp(const WCHAR *a, const WCHAR *b, SIZE_T n) { for (; n; n--, a++, b++) { WCHAR x = wup(*a), y = wup(*b); if (x != y) return x < y ? -1 : 1; if (!x) return 0; } return 0; }
static int wcmp(const WCHAR *a, const WCHAR *b) { for (;; a++, b++) { if (*a != *b) return *a < *b ? -1 : 1; if (!*a) return 0; } }
static WCHAR *wdup_heap(const WCHAR *s) { SIZE_T n = wlen(s) + 1; WCHAR *r = mem_alloc(n * sizeof(WCHAR)); if (r) mem_copy(r, s, n * sizeof(WCHAR)); return r; }
static void wcopy_n(WCHAR *d, const WCHAR *s, SIZE_T cap) { SIZE_T i = 0; if (!cap) return; for (; i + 1 < cap && s[i]; i++) d[i] = s[i]; d[i] = 0; }
/* ASCII text as wide characters (a short constant) */
static void ascii_to_wide(WCHAR *d, const char *s) { while ((*d++ = (WCHAR)(BYTE)*s++)) {} }
#define W(...) ((const WCHAR *)(const WCHAR[]){__VA_ARGS__, 0})

/* ---- CoTaskMem and IMalloc ----------------------------------------------------------------------------------------------------------------------------------------- */

static IMallocSpy *g_spy;

EXPORT LPVOID WINAPI CoTaskMemAlloc(SIZE_T size) {
  IMallocSpy *spy = g_spy;
  if (spy) size = IMallocSpy_PreAlloc(spy, size);
  void *p = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
  if (spy) p = IMallocSpy_PostAlloc(spy, p);
  return p;
}
EXPORT void WINAPI CoTaskMemFree(LPVOID p) {
  IMallocSpy *spy = g_spy;
  if (!p) return;
  if (spy) p = IMallocSpy_PreFree(spy, p, TRUE);
  if (p) HeapFree(GetProcessHeap(), 0, p);
  if (spy) IMallocSpy_PostFree(spy, TRUE);
}
EXPORT LPVOID WINAPI CoTaskMemRealloc(LPVOID p, SIZE_T size) {
  IMallocSpy *spy = g_spy;
  void *request = p, *result;
  if (!p) return CoTaskMemAlloc(size);
  if (!size) { CoTaskMemFree(p); return NULL; }
  if (spy) size = IMallocSpy_PreRealloc(spy, p, size, &request, TRUE);
  result = HeapReAlloc(GetProcessHeap(), 0, request, size ? size : 1);
  if (spy) result = IMallocSpy_PostRealloc(spy, result, TRUE);
  return result;
}

typedef struct { IMalloc iface; } MallocObject;
static HRESULT WINAPI malloc_qi(IMalloc *me, REFIID riid, void **out) {
  if (!out) return E_INVALIDARG;
  if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMalloc)) { *out = me; return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI malloc_addref(IMalloc *me) { (void)me; return 1; }
static ULONG WINAPI malloc_release(IMalloc *me) { (void)me; return 1; }
static void *WINAPI malloc_alloc(IMalloc *me, SIZE_T n) { (void)me; return CoTaskMemAlloc(n); }
static void *WINAPI malloc_realloc(IMalloc *me, void *p, SIZE_T n) { (void)me; return CoTaskMemRealloc(p, n); }
static void WINAPI malloc_free(IMalloc *me, void *p) { (void)me; CoTaskMemFree(p); }
static SIZE_T WINAPI malloc_getsize(IMalloc *me, void *p) {
  IMallocSpy *spy = g_spy;
  (void)me;
  if (spy) p = IMallocSpy_PreGetSize(spy, p, TRUE);
  SIZE_T n = p ? HeapSize(GetProcessHeap(), 0, p) : (SIZE_T)-1;
  if (spy) n = IMallocSpy_PostGetSize(spy, n, TRUE);
  return n;
}
static int WINAPI malloc_didalloc(IMalloc *me, void *p) {
  IMallocSpy *spy = g_spy;
  (void)me;
  if (spy) p = IMallocSpy_PreDidAlloc(spy, p, TRUE);
  int r = p ? (HeapValidate(GetProcessHeap(), 0, p) ? 1 : 0) : -1;
  if (spy) r = IMallocSpy_PostDidAlloc(spy, p, TRUE, r);
  return r;
}
static void WINAPI malloc_heapminimize(IMalloc *me) {
  IMallocSpy *spy = g_spy;
  (void)me;
  if (spy) IMallocSpy_PreHeapMinimize(spy);
  if (spy) IMallocSpy_PostHeapMinimize(spy);
}
static const IMallocVtbl g_malloc_vtbl = {malloc_qi, malloc_addref, malloc_release, malloc_alloc, malloc_realloc, malloc_free, malloc_getsize, malloc_didalloc, malloc_heapminimize};
static IMalloc g_malloc = {(IMallocVtbl *)&g_malloc_vtbl};

EXPORT HRESULT WINAPI CoGetMalloc(DWORD context, LPMALLOC *out) {
  if (!out) return E_INVALIDARG;
  if (context != MEMCTX_TASK) { *out = NULL; return E_INVALIDARG; }
  *out = &g_malloc;
  return S_OK;
}
EXPORT HRESULT WINAPI CoRegisterMallocSpy(LPMALLOCSPY spy) {
  IMallocSpy *probe;
  if (!spy) return E_INVALIDARG;
  if (FAILED(IMallocSpy_QueryInterface(spy, &IID_IMallocSpy, (void **)&probe))) return E_INVALIDARG;
  lock();
  if (g_spy) { unlock(); IMallocSpy_Release(probe); return CO_E_OBJISREG; }
  g_spy = probe;
  unlock();
  return S_OK;
}
EXPORT HRESULT WINAPI CoRevokeMallocSpy(void) {
  lock();
  IMallocSpy *spy = g_spy;
  g_spy = NULL;
  unlock();
  if (!spy) return CO_E_OBJNOTREG;
  IMallocSpy_Release(spy);
  return S_OK;
}

/* ---- the clock, versions, DOS dates -------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HRESULT WINAPI CoFileTimeNow(FILETIME *ft) { GetSystemTimeAsFileTime(ft); return S_OK; }
EXPORT BOOL WINAPI CoDosDateTimeToFileTime(WORD date, WORD time, FILETIME *ft) { return DosDateTimeToFileTime(date, time, ft); }
EXPORT BOOL WINAPI CoFileTimeToDosDateTime(FILETIME *ft, LPWORD date, LPWORD time) { return FileTimeToDosDateTime(ft, date, time); }
EXPORT DWORD WINAPI CoBuildVersion(void) { return 0x0700 << 16 | 0x0AFB; }
EXPORT DWORD WINAPI OleBuildVersion(void) { return 0x0700 << 16 | 0x0AFB; }
EXPORT HRESULT WINAPI CoGetSystemSecurityPermissions(COMSD kind, PSECURITY_DESCRIPTOR *sd) { (void)kind; if (sd) *sd = NULL; return E_NOTIMPL; }

/* ---- GUIDs --------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* a random generator: xorshift128+, seeded from the clock and the identity of the thread */
static ULONGLONG g_rng[2];
static void rng_seed(void) {
  LARGE_INTEGER qpc;
  FILETIME ft;
  QueryPerformanceCounter(&qpc);
  GetSystemTimeAsFileTime(&ft);
  g_rng[0] ^= (ULONGLONG)qpc.QuadPart * 0x9E3779B97F4A7C15ull ^ ((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) ^ (ULONGLONG)GetCurrentProcessId() << 17;
  g_rng[1] ^= (ULONGLONG)GetTickCount64() * 0xC2B2AE3D27D4EB4Full ^ (ULONGLONG)(ULONG_PTR)&qpc ^ (ULONGLONG)GetCurrentThreadId() << 40;
  if (!g_rng[0] && !g_rng[1]) g_rng[0] = 0x1234567;
}
static ULONGLONG rng_next(void) {
  ULONGLONG s1 = g_rng[0];
  const ULONGLONG s0 = g_rng[1];
  g_rng[0] = s0;
  s1 ^= s1 << 23;
  g_rng[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
  return g_rng[1] + s0;
}

EXPORT HRESULT WINAPI CoCreateGuid(GUID *guid) {
  if (!guid) return E_INVALIDARG;
  lock();
  if (!g_rng[0] && !g_rng[1]) rng_seed();
  ULONGLONG a = rng_next(), b = rng_next();
  unlock();
  BYTE raw[16];
  mem_copy(raw, &a, 8);
  mem_copy(raw + 8, &b, 8);
  raw[7] = (BYTE)((raw[7] & 0x0F) | 0x40);      /* version 4 (random) */
  raw[8] = (BYTE)((raw[8] & 0x3F) | 0x80);      /* variant 1 */
  mem_copy(guid, raw, 16);
  return S_OK;
}

EXPORT BOOL WINAPI Ole32_IsEqualGUID(REFGUID a, REFGUID b) __asm__("IsEqualGUID");
EXPORT BOOL WINAPI Ole32_IsEqualGUID(REFGUID a, REFGUID b) { return mem_cmp(a, b, sizeof(GUID)) == 0; }

static const char kHex[] = "0123456789ABCDEF";
static void put_hex(WCHAR **p, ULONGLONG v, int digits) { for (int i = digits - 1; i >= 0; i--) *(*p)++ = (WCHAR)kHex[(v >> (4 * i)) & 15]; }
/* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (38 characters) and the end mark */
static void guid_to_text(const GUID *g, WCHAR *out) {
  WCHAR *p = out;
  *p++ = '{';
  put_hex(&p, g->Data1, 8); *p++ = '-';
  put_hex(&p, g->Data2, 4); *p++ = '-';
  put_hex(&p, g->Data3, 4); *p++ = '-';
  put_hex(&p, g->Data4[0], 2); put_hex(&p, g->Data4[1], 2); *p++ = '-';
  for (int i = 2; i < 8; i++) put_hex(&p, g->Data4[i], 2);
  *p++ = '}';
  *p = 0;
}
EXPORT int WINAPI StringFromGUID2(REFGUID guid, LPOLESTR out, int cap) {
  if (!guid || cap < 39) return 0;
  guid_to_text(guid, out);
  return 39;
}
static HRESULT string_from_guid(const GUID *g, LPOLESTR *out) {
  if (!out) return E_INVALIDARG;
  *out = CoTaskMemAlloc(39 * sizeof(WCHAR));
  if (!*out) return E_OUTOFMEMORY;
  guid_to_text(g, *out);
  return S_OK;
}
EXPORT HRESULT WINAPI StringFromCLSID(REFCLSID clsid, LPOLESTR *out) { return string_from_guid(clsid, out); }
EXPORT HRESULT WINAPI StringFromIID(REFIID iid, LPOLESTR *out) { return string_from_guid(iid, out); }

static int hex_value(WCHAR c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }
static BOOL read_hex(const WCHAR **p, int digits, ULONGLONG *v) {
  *v = 0;
  for (int i = 0; i < digits; i++) { int h = hex_value((*p)[i]); if (h < 0) return FALSE; *v = *v << 4 | (ULONGLONG)h; }
  *p += digits;
  return TRUE;
}
/* "{...}" in the exact Windows layout; the string is checked whole (a longer string is not accepted) */
static BOOL guid_from_text(const WCHAR *s, GUID *g) {
  ULONGLONG v;
  GUID r;
  if (*s++ != '{') return FALSE;
  if (!read_hex(&s, 8, &v)) return FALSE; r.Data1 = (DWORD)v;
  if (*s++ != '-') return FALSE;
  if (!read_hex(&s, 4, &v)) return FALSE; r.Data2 = (WORD)v;
  if (*s++ != '-') return FALSE;
  if (!read_hex(&s, 4, &v)) return FALSE; r.Data3 = (WORD)v;
  if (*s++ != '-') return FALSE;
  for (int i = 0; i < 2; i++) { if (!read_hex(&s, 2, &v)) return FALSE; r.Data4[i] = (BYTE)v; }
  if (*s++ != '-') return FALSE;
  for (int i = 2; i < 8; i++) { if (!read_hex(&s, 2, &v)) return FALSE; r.Data4[i] = (BYTE)v; }
  if (*s++ != '}') return FALSE;
  if (*s) return FALSE;
  *g = r;
  return TRUE;
}
EXPORT HRESULT WINAPI IIDFromString(LPCOLESTR s, LPIID iid) {
  if (!s) { mem_zero(iid, sizeof(IID)); return S_OK; }
  GUID g;
  if (!guid_from_text(s, &g)) return s[0] == '{' ? CO_E_IIDSTRING : E_INVALIDARG;
  *iid = g;
  return S_OK;
}

/* ---- pointer checks ------------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT BOOL WINAPI IsValidPtrIn(const void *p, ULONG bytes) { return p && !IsBadReadPtr(p, bytes); }
EXPORT BOOL WINAPI IsValidPtrOut(void *p, ULONG bytes) { return p && !IsBadWritePtr(p, bytes); }
EXPORT BOOL WINAPI IsValidIid(REFIID iid) { (void)iid; return TRUE; }
EXPORT BOOL WINAPI IsValidInterface(LPUNKNOWN unk) { return unk && !IsBadReadPtr(unk, sizeof(void *)) && !IsBadReadPtr(unk->lpVtbl, 3 * sizeof(void *)); }

/* ---- BSTRs (the oleaut32 layout: a length in bytes before the characters) ------------------------------------------------------------------------------------------ */

static BSTR bstr_alloc_len(const WCHAR *s, UINT chars) {
  DWORD *block = mem_alloc(sizeof(DWORD) + (SIZE_T)chars * sizeof(WCHAR) + sizeof(WCHAR));
  if (!block) return NULL;
  block[0] = chars * sizeof(WCHAR);
  WCHAR *text = (WCHAR *)(block + 1);
  if (s) mem_copy(text, s, (SIZE_T)chars * sizeof(WCHAR));
  text[chars] = 0;
  return text;
}
static BSTR bstr_dup(const WCHAR *s) { return s ? bstr_alloc_len(s, (UINT)wlen(s)) : NULL; }
static void bstr_free(BSTR b) { if (b) mem_free((DWORD *)b - 1); }
static UINT bstr_len(BSTR b) { return b ? ((DWORD *)b)[-1] / sizeof(WCHAR) : 0; }
EXPORT BSTR WINAPI PropSysAllocString(LPCOLESTR s) { return bstr_dup(s); }
EXPORT void WINAPI PropSysFreeString(LPOLESTR s) { bstr_free(s); }

/* ---- per-thread state ---------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct ThreadData {
  void *threadbase;            /* (the layout of the first four fields is Windows' per-thread OLE data, which TEB.ReservedForOle points to) */
  void *smallocator;
  DWORD id;
  DWORD flags;                 /* OLETLS_* */
  LONG cancel_count;
  DWORD apartment_kind;        /* 0 none, 1 STA, 2 MTA */
  DWORD init_count;            /* CoInitialize calls not yet undone */
  DWORD ole_init_count;
  BOOL main_sta;
  IErrorInfo *error_info;
  void *state;                 /* CoGetState / CoSetState */
  IUnknown *call_context;
  IUnknown *message_filter;
  IUnknown *init_spy;          /* (the list of spies is in compobj.inc) */
  BOOL mta_implicit;
  DWORD process_id;
  void *sta_context;           /* the ComContext of this thread's apartment (compobj.inc) */
} ThreadData;

/* The data of a thread hangs on TEB.ReservedForOle (offset 0x1758 on x64), where Windows keeps it, so that programs can see its flags. */
static void **teb_ole_slot(void) { return (void **)((BYTE *)NtCurrentTeb() + 0x1758); }
static ThreadData *thread_data(BOOL create) {
  void **slot = teb_ole_slot();
  ThreadData *td = *slot;
  if (!td && create) {
    td = mem_alloc(sizeof(*td));
    if (td) { td->id = GetCurrentThreadId(); *slot = td; }
  }
  return td;
}

static void core_init(void) {
  InitializeCriticalSection(&g_lock);
}

EXPORT DWORD WINAPI CoGetCurrentProcess(void) {
  static LONG next = 0;
  ThreadData *td = thread_data(TRUE);
  if (!td) return 0;
  if (!td->process_id) td->process_id = (DWORD)InterlockedIncrement(&next);
  return td->process_id;
}
