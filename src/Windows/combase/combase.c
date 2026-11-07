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

/* combase.dll for TheoryCraftOS: the Windows Runtime part of it (the api-ms-win-core-winrt-* API sets fold to this library, see canonical_library_name()). HSTRINGs are
 * real: a counted, immutable, NUL-terminated UTF-16 string, either on the heap with a reference count or a "fast-pass" reference that lives in the caller's
 * HSTRING_HEADER and points at the caller's buffer. RoInitialize/RoUninitialize are the COM apartment calls of ole32.dll (looked up at run time). There are no WinRT
 * classes: RoGetActivationFactory and RoActivateInstance fail with REGDB_E_CLASSNOTREG, so a program takes its non-WinRT path. The error-reporting calls
 * (RoOriginateError ...) record nothing. Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

#define S_OK_ ((HRESULT)0)
#define E_NOTIMPL_ ((HRESULT)0x80004001L)
#define E_POINTER_ ((HRESULT)0x80004003L)
#define E_INVALIDARG_ ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY_ ((HRESULT)0x8007000EL)
#define E_BOUNDS_ ((HRESULT)0x8000000BL)
#define REGDB_E_CLASSNOTREG_ ((HRESULT)0x80040154L)
#define MEM_E_INVALID_SIZE_ ((HRESULT)0x80080011L)

/* ------------------------------------------------------------------------------------------------------------------- HSTRING */

/* What an HSTRING points at. A reference string's record is the caller's HSTRING_HEADER (24 bytes on x64, this record is 24 too); a heap string's is followed by its
 * characters. */
typedef struct {
  UINT32 flags;      /* FLAG_REFERENCE: lives in a header, never freed */
  UINT32 length;      /* in characters, without the terminator */
  const WCHAR *buffer;
  volatile LONG references;
  UINT32 reserved;
} StringRecord;
typedef StringRecord *HSTRING_;
typedef struct { union { PVOID Reserved1; char Reserved2[24]; } Reserved; } HSTRING_HEADER_;
#define FLAG_REFERENCE 1u
#define FLAG_BUFFER 2u      /* a preallocated buffer that is not a string yet (WindowsPreallocateStringBuffer) */
#define MAX_CHARS 0x7FFFFFF0u

_Static_assert(sizeof(StringRecord) <= sizeof(HSTRING_HEADER_), "a reference string's record fits the caller's header");

static void copy_chars(WCHAR *to, const WCHAR *from, UINT32 n) {
  for (UINT32 i = 0; i < n; ++i) to[i] = from[i];
}

/* A new heap string of n characters (the characters are left to the caller; the terminator is set). */
static StringRecord *new_string(UINT32 n, WCHAR **chars) {
  if (n > MAX_CHARS) return NULL;
  StringRecord *s = HeapAlloc(GetProcessHeap(), 0, sizeof(StringRecord) + ((SIZE_T)n + 1) * sizeof(WCHAR));
  if (!s) return NULL;
  WCHAR *c = (WCHAR *)(s + 1);
  c[n] = 0;
  s->flags = 0;
  s->length = n;
  s->buffer = c;
  s->references = 1;
  s->reserved = 0;
  *chars = c;
  return s;
}

static HRESULT make_string(const WCHAR *source, UINT32 length, HSTRING_ *out) {
  WCHAR *chars;
  StringRecord *s = new_string(length, &chars);
  if (!s) return E_OUTOFMEMORY_;
  copy_chars(chars, source, length);
  *out = s;
  return S_OK_;
}

EXPORT HRESULT WINAPI WindowsCreateString(const WCHAR *source, UINT32 length, HSTRING_ *out) {
  if (!out) return E_INVALIDARG_;
  *out = NULL;
  if (length == 0) return S_OK_;      /* the empty string is NULL */
  if (!source) return E_POINTER_;
  return make_string(source, length, out);
}

EXPORT HRESULT WINAPI WindowsCreateStringReference(const WCHAR *source, UINT32 length, HSTRING_HEADER_ *header, HSTRING_ *out) {
  if (!out || !header) return E_INVALIDARG_;
  *out = NULL;
  if (length == 0) return S_OK_;
  if (!source) return E_POINTER_;
  if (length > MAX_CHARS || source[length] != 0) return E_INVALIDARG_;      /* the source must be terminated where the length says */
  StringRecord *s = (StringRecord *)header;
  s->flags = FLAG_REFERENCE;
  s->length = length;
  s->buffer = source;
  s->references = 1;
  s->reserved = 0;
  *out = s;
  return S_OK_;
}

EXPORT HRESULT WINAPI WindowsDeleteString(HSTRING_ string) {
  if (!string || (string->flags & FLAG_REFERENCE)) return S_OK_;
  if (InterlockedDecrement(&string->references) == 0) HeapFree(GetProcessHeap(), 0, string);
  return S_OK_;
}

EXPORT HRESULT WINAPI WindowsDuplicateString(HSTRING_ string, HSTRING_ *out) {
  if (!out) return E_INVALIDARG_;
  *out = NULL;
  if (!string) return S_OK_;
  if (string->flags & FLAG_REFERENCE) return make_string(string->buffer, string->length, out);      /* a reference lives only as long as its header */
  InterlockedIncrement(&string->references);
  *out = string;
  return S_OK_;
}

EXPORT UINT32 WINAPI WindowsGetStringLen(HSTRING_ string) { return string ? string->length : 0; }

EXPORT const WCHAR *WINAPI WindowsGetStringRawBuffer(HSTRING_ string, UINT32 *length) {
  if (length) *length = string ? string->length : 0;
  return string ? string->buffer : L"";
}

EXPORT BOOL WINAPI WindowsIsStringEmpty(HSTRING_ string) { return !string || string->length == 0; }

EXPORT HRESULT WINAPI WindowsStringHasEmbeddedNull(HSTRING_ string, BOOL *has) {
  if (!has) return E_INVALIDARG_;
  *has = FALSE;
  if (string)
    for (UINT32 i = 0; i < string->length; ++i)
      if (string->buffer[i] == 0) { *has = TRUE; break; }
  return S_OK_;
}

/* Code unit by code unit; a prefix sorts first. */
EXPORT HRESULT WINAPI WindowsCompareStringOrdinal(HSTRING_ a, HSTRING_ b, INT32 *result) {
  if (!result) return E_INVALIDARG_;
  UINT32 na, nb;
  const WCHAR *pa = WindowsGetStringRawBuffer(a, &na), *pb = WindowsGetStringRawBuffer(b, &nb);
  const UINT32 n = na < nb ? na : nb;
  for (UINT32 i = 0; i < n; ++i)
    if (pa[i] != pb[i]) { *result = pa[i] < pb[i] ? -1 : 1; return S_OK_; }
  *result = na == nb ? 0 : (na < nb ? -1 : 1);
  return S_OK_;
}

EXPORT HRESULT WINAPI WindowsConcatString(HSTRING_ a, HSTRING_ b, HSTRING_ *out) {
  if (!out) return E_INVALIDARG_;
  *out = NULL;
  UINT32 na, nb;
  const WCHAR *pa = WindowsGetStringRawBuffer(a, &na), *pb = WindowsGetStringRawBuffer(b, &nb);
  if (na == 0) return WindowsDuplicateString(b, out);
  if (nb == 0) return WindowsDuplicateString(a, out);
  if (nb > MAX_CHARS - na) return MEM_E_INVALID_SIZE_;
  WCHAR *chars;
  StringRecord *s = new_string(na + nb, &chars);
  if (!s) return E_OUTOFMEMORY_;
  copy_chars(chars, pa, na);
  copy_chars(chars + na, pb, nb);
  *out = s;
  return S_OK_;
}

EXPORT HRESULT WINAPI WindowsSubstringWithSpecifiedLength(HSTRING_ string, UINT32 start, UINT32 length, HSTRING_ *out) {
  if (!out) return E_INVALIDARG_;
  *out = NULL;
  UINT32 n;
  const WCHAR *p = WindowsGetStringRawBuffer(string, &n);
  if (start > n || length > n - start) return E_BOUNDS_;
  if (length == 0) return S_OK_;
  return make_string(p + start, length, out);
}

EXPORT HRESULT WINAPI WindowsSubstring(HSTRING_ string, UINT32 start, HSTRING_ *out) {
  const UINT32 n = WindowsGetStringLen(string);
  if (start > n) { if (out) *out = NULL; return out ? E_BOUNDS_ : E_INVALIDARG_; }
  return WindowsSubstringWithSpecifiedLength(string, start, n - start, out);
}

/* Preallocated buffers: a heap string of the length asked for that the caller fills, then promotes into a string. The buffer handle is the record. */
EXPORT HRESULT WINAPI WindowsPreallocateStringBuffer(UINT32 length, WCHAR **chars, HANDLE *buffer) {
  if (!chars || !buffer) return E_POINTER_;
  *chars = NULL;
  *buffer = NULL;
  if (length == 0) { static WCHAR empty[1]; *chars = empty; return S_OK_; }      /* (promotes to NULL, the empty string) */
  WCHAR *c;
  StringRecord *s = new_string(length, &c);
  if (!s) return length > MAX_CHARS ? MEM_E_INVALID_SIZE_ : E_OUTOFMEMORY_;
  s->flags = FLAG_BUFFER;
  *chars = c;
  *buffer = s;
  return S_OK_;
}

EXPORT HRESULT WINAPI WindowsDeleteStringBuffer(HANDLE buffer) {
  StringRecord *s = buffer;
  if (!s) return S_OK_;
  if (!(s->flags & FLAG_BUFFER)) return E_INVALIDARG_;
  HeapFree(GetProcessHeap(), 0, s);
  return S_OK_;
}

EXPORT HRESULT WINAPI WindowsPromoteStringBuffer(HANDLE buffer, HSTRING_ *out) {
  if (!out) return E_POINTER_;
  *out = NULL;
  StringRecord *s = buffer;
  if (!s) return S_OK_;
  if (!(s->flags & FLAG_BUFFER)) return E_INVALIDARG_;
  WCHAR *chars = (WCHAR *)(s + 1);
  if (chars[s->length] != 0) return E_INVALIDARG_;      /* the caller wrote past the end */
  s->flags = 0;
  *out = s;
  return S_OK_;
}

static BOOL contains(const WCHAR *set, UINT32 n, WCHAR c) {
  for (UINT32 i = 0; i < n; ++i)
    if (set[i] == c) return TRUE;
  return FALSE;
}

EXPORT HRESULT WINAPI WindowsTrimStringStart(HSTRING_ string, HSTRING_ trim, HSTRING_ *out) {
  if (!out || !trim) return E_INVALIDARG_;
  *out = NULL;
  UINT32 n, nt;
  const WCHAR *p = WindowsGetStringRawBuffer(string, &n), *t = WindowsGetStringRawBuffer(trim, &nt);
  UINT32 start = 0;
  while (start < n && contains(t, nt, p[start])) ++start;
  if (start == 0) return WindowsDuplicateString(string, out);
  return WindowsSubstringWithSpecifiedLength(string, start, n - start, out);
}

EXPORT HRESULT WINAPI WindowsTrimStringEnd(HSTRING_ string, HSTRING_ trim, HSTRING_ *out) {
  if (!out || !trim) return E_INVALIDARG_;
  *out = NULL;
  UINT32 n, nt;
  const WCHAR *p = WindowsGetStringRawBuffer(string, &n), *t = WindowsGetStringRawBuffer(trim, &nt);
  UINT32 end = n;
  while (end > 0 && contains(t, nt, p[end - 1])) --end;
  if (end == n) return WindowsDuplicateString(string, out);
  return WindowsSubstringWithSpecifiedLength(string, 0, end, out);
}

EXPORT HRESULT WINAPI WindowsReplaceString(HSTRING_ string, HSTRING_ find, HSTRING_ replace, HSTRING_ *out) {
  if (!out) return E_INVALIDARG_;
  *out = NULL;
  UINT32 n, nf, nr;
  const WCHAR *p = WindowsGetStringRawBuffer(string, &n), *f = WindowsGetStringRawBuffer(find, &nf), *r = WindowsGetStringRawBuffer(replace, &nr);
  if (nf == 0) return E_INVALIDARG_;
  /* Count, then build. */
  ULONGLONG total = 0;
  for (UINT32 i = 0; i < n;) {
    BOOL match = nf <= n - i;
    for (UINT32 k = 0; match && k < nf; ++k) match = p[i + k] == f[k];
    if (match) { total += nr; i += nf; } else { total += 1; ++i; }
  }
  if (total > MAX_CHARS) return MEM_E_INVALID_SIZE_;
  if (total == 0) return S_OK_;
  WCHAR *chars;
  StringRecord *s = new_string((UINT32)total, &chars);
  if (!s) return E_OUTOFMEMORY_;
  UINT32 o = 0;
  for (UINT32 i = 0; i < n;) {
    BOOL match = nf <= n - i;
    for (UINT32 k = 0; match && k < nf; ++k) match = p[i + k] == f[k];
    if (match) { copy_chars(chars + o, r, nr); o += nr; i += nf; } else { chars[o++] = p[i++]; }
  }
  *out = s;
  return S_OK_;
}

/* ------------------------------------------------------------------------------------------------------------------- Ro* */

typedef HRESULT (WINAPI *CoInitializeExFn)(LPVOID, DWORD);
typedef void (WINAPI *CoUninitializeFn)(void);

static FARPROC ole32_function(const char *name) {
  HMODULE ole32 = GetModuleHandleW(L"ole32.dll");
  if (!ole32) ole32 = LoadLibraryW(L"ole32.dll");
  return ole32 ? GetProcAddress(ole32, name) : NULL;
}

/* RO_INIT_SINGLETHREADED (0) is an apartment-threaded COM apartment, RO_INIT_MULTITHREADED (1) the multithreaded one. */
EXPORT HRESULT WINAPI RoInitialize(int type) {
  if (type != 0 && type != 1) return E_INVALIDARG_;
  CoInitializeExFn init = (CoInitializeExFn)(void *)ole32_function("CoInitializeEx");
  if (!init) return E_NOTIMPL_;
  return init(NULL, type == 1 ? 0 /* COINIT_MULTITHREADED */ : 2 /* COINIT_APARTMENTTHREADED */);
}

EXPORT void WINAPI RoUninitialize(void) {
  CoUninitializeFn uninit = (CoUninitializeFn)(void *)ole32_function("CoUninitialize");
  if (uninit) uninit();
}

EXPORT HRESULT WINAPI RoGetActivationFactory(HSTRING_ class_id, REFIID iid, void **factory) {
  (void)class_id;
  (void)iid;
  if (!factory) return E_POINTER_;
  *factory = NULL;
  return REGDB_E_CLASSNOTREG_;
}

EXPORT HRESULT WINAPI RoActivateInstance(HSTRING_ class_id, void **instance) {
  (void)class_id;
  if (!instance) return E_POINTER_;
  *instance = NULL;
  return REGDB_E_CLASSNOTREG_;
}

EXPORT HRESULT WINAPI RoGetActivatableClassRegistration(HSTRING_ class_id, void **registration) {
  (void)class_id;
  if (registration) *registration = NULL;
  return REGDB_E_CLASSNOTREG_;
}

/* Error reporting: nothing is kept; the originate calls say the error was not stored (FALSE), as when no debugger or error object is there. */
EXPORT BOOL WINAPI RoOriginateError(HRESULT error, HSTRING_ message) { (void)error; (void)message; return FALSE; }
EXPORT BOOL WINAPI RoOriginateErrorW(HRESULT error, UINT length, const WCHAR *message) { (void)error; (void)length; (void)message; return FALSE; }
EXPORT BOOL WINAPI RoTransformError(HRESULT old_error, HRESULT new_error, HSTRING_ message) { (void)old_error; (void)new_error; (void)message; return FALSE; }
EXPORT BOOL WINAPI RoTransformErrorW(HRESULT old_error, HRESULT new_error, UINT length, const WCHAR *message) { (void)old_error; (void)new_error; (void)length; (void)message; return FALSE; }
EXPORT void WINAPI RoClearError(void) {}
EXPORT HRESULT WINAPI GetRestrictedErrorInfo(void **info) {
  if (!info) return E_POINTER_;
  *info = NULL;
  return 1;      /* S_FALSE: no error information */
}
EXPORT HRESULT WINAPI SetRestrictedErrorInfo(void *info) { (void)info; return S_OK_; }
EXPORT BOOL WINAPI IsErrorPropagationEnabled(void) { return FALSE; }
EXPORT HRESULT WINAPI RoGetErrorReportingFlags(UINT32 *flags) {
  if (!flags) return E_POINTER_;
  *flags = 0;
  return S_OK_;
}
EXPORT HRESULT WINAPI RoSetErrorReportingFlags(UINT32 flags) { (void)flags; return S_OK_; }
EXPORT HRESULT WINAPI RoCaptureErrorContext(HRESULT error) { (void)error; return S_OK_; }
EXPORT HRESULT WINAPI RoGetApartmentIdentifier(UINT64 *id) {
  if (!id) return E_POINTER_;
  *id = 0;
  return E_NOTIMPL_;
}
EXPORT HRESULT WINAPI RoRegisterForApartmentShutdown(void *callback, UINT64 *id, void **cookie) {
  (void)callback;
  if (id) *id = 0;
  if (cookie) *cookie = NULL;
  return E_NOTIMPL_;
}
EXPORT HRESULT WINAPI RoUnregisterForApartmentShutdown(void *cookie) { (void)cookie; return E_INVALIDARG_; }
EXPORT HRESULT WINAPI RoRegisterActivationFactories(HSTRING_ *ids, void **callbacks, UINT32 count, void **cookie) {
  (void)ids;
  (void)callbacks;
  (void)count;
  if (cookie) *cookie = NULL;
  return E_NOTIMPL_;
}
EXPORT void WINAPI RoRevokeActivationFactories(void *cookie) { (void)cookie; }
EXPORT HRESULT WINAPI RoGetMetaDataFile(HSTRING_ name, void *dispenser, HSTRING_ *path, void **import, DWORD *token) {
  (void)name;
  (void)dispenser;
  if (path) *path = NULL;
  if (import) *import = NULL;
  if (token) *token = 0;
  return (HRESULT)0x80073D54L;      /* RO_E_METADATA_NAME_NOT_FOUND */
}
