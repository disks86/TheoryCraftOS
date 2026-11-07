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

/* propsys.dll for TheoryCraftOS: the PROPVARIANT helpers programs use to fill property stores, and VariantCompare. The values are allocated with ole32's CoTaskMemAlloc
 * (looked up when first needed: this library is linked over kernel32 only), so PropVariantClear of ole32 frees them. There is no property system: no in-memory property
 * store (PSCreateMemoryPropertyStore answers E_NOTIMPL) and no schema (PSGetPropertyKeyFromName finds no name). The rest is still stubs (src/Windows/stubs/propsys.inc).
 * Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define S_OK_ ((HRESULT)0)
#define E_NOTIMPL_ ((HRESULT)0x80004001)
#define E_INVALIDARG_ ((HRESULT)0x80070057)
#define E_OUTOFMEMORY_ ((HRESULT)0x8007000E)
#define TYPE_E_ELEMENTNOTFOUND_ ((HRESULT)0x8002802B)

/* The parts of PROPVARIANT / VARIANT this library writes or reads: vt, three reserved words, then the value (16 bytes on x64 for a VARIANT, 24 with the record form). */
enum { VT_EMPTY_ = 0, VT_NULL_ = 1, VT_I2_ = 2, VT_I4_ = 3, VT_R4_ = 4, VT_R8_ = 5, VT_BSTR_ = 8, VT_BOOL_ = 11, VT_I1_ = 16, VT_UI1_ = 17, VT_UI2_ = 18, VT_UI4_ = 19,
       VT_I8_ = 20, VT_UI8_ = 21, VT_INT_ = 22, VT_UINT_ = 23, VT_LPWSTR_ = 31, VT_CLSID_ = 72, VT_VECTOR_ = 0x1000 };
typedef struct {
  WORD vt, reserved1, reserved2, reserved3;
  union {
    LONGLONG i8;
    ULONGLONG u8;
    LONG i4;
    ULONG u4;
    SHORT i2;
    USHORT u2;
    signed char i1;
    BYTE u1;
    float r4;
    double r8;
    GUID *clsid;
    WCHAR *bstr;
    struct { ULONG count; WCHAR **items; } wide_vector;
  } v;
  void *record;      /* (pad to the size of a VARIANT) */
} Variant;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

typedef void *(WINAPI *TaskAlloc)(SIZE_T);
typedef void (WINAPI *TaskFree)(void *);
static TaskAlloc g_alloc;
static TaskFree g_free;
static BOOL task_memory(void) {
  if (g_alloc && g_free) return TRUE;
  HMODULE ole32 = LoadLibraryW(L"ole32.dll");
  if (!ole32) return FALSE;
  g_free = (TaskFree)(void *)GetProcAddress(ole32, "CoTaskMemFree");
  g_alloc = (TaskAlloc)(void *)GetProcAddress(ole32, "CoTaskMemAlloc");
  return g_alloc && g_free;
}
static void *task_alloc(SIZE_T n) { return task_memory() ? g_alloc(n) : NULL; }
static void zero_variant(Variant *v) {
  BYTE *p = (BYTE *)v;
  for (SIZE_T i = 0; i < sizeof *v; ++i) p[i] = 0;
}

EXPORT HRESULT WINAPI InitPropVariantFromCLSID(const GUID *clsid, Variant *pv) {
  if (!pv) return E_INVALIDARG_;
  zero_variant(pv);
  if (!clsid) return E_INVALIDARG_;
  GUID *copy = task_alloc(sizeof *copy);
  if (!copy) return E_OUTOFMEMORY_;
  *copy = *clsid;
  pv->vt = VT_CLSID_;
  pv->v.clsid = copy;
  return S_OK_;
}

/* A VT_VECTOR | VT_LPWSTR of copies of the strings (an empty vector for count 0). */
EXPORT HRESULT WINAPI InitPropVariantFromStringVector(const WCHAR *const *strings, ULONG count, Variant *pv) {
  if (!pv) return E_INVALIDARG_;
  zero_variant(pv);
  if (count && !strings) return E_INVALIDARG_;
  if (count > 0x1000000u) return E_OUTOFMEMORY_;
  WCHAR **items = task_alloc((SIZE_T)(count ? count : 1) * sizeof *items);
  if (!items) return E_OUTOFMEMORY_;
  for (ULONG i = 0; i < count; ++i) {
    const WCHAR *s = strings[i] ? strings[i] : L"";
    SIZE_T n = 0;
    while (s[n]) ++n;
    items[i] = task_alloc((n + 1) * sizeof(WCHAR));
    if (!items[i]) {
      for (ULONG k = 0; k < i; ++k) g_free(items[k]);
      g_free(items);
      return E_OUTOFMEMORY_;
    }
    for (SIZE_T k = 0; k <= n; ++k) items[i][k] = s[k];
  }
  pv->vt = VT_VECTOR_ | VT_LPWSTR_;
  pv->v.wide_vector.count = count;
  pv->v.wide_vector.items = items;
  return S_OK_;
}

EXPORT HRESULT WINAPI PSCreateMemoryPropertyStore(const GUID *iid, void **store) {
  (void)iid;
  if (store) *store = NULL;
  return E_NOTIMPL_;
}

typedef struct { GUID fmtid; DWORD pid; } PropertyKey;
EXPORT HRESULT WINAPI PSGetPropertyKeyFromName(LPCWSTR name, PropertyKey *key) {
  if (!key) return E_INVALIDARG_;
  BYTE *p = (BYTE *)key;
  for (SIZE_T i = 0; i < sizeof *key; ++i) p[i] = 0;
  if (!name) return E_INVALIDARG_;
  return TYPE_E_ELEMENTNOTFOUND_;
}

/* VariantCompare: -1, 0 or 1. Empty / null values sort first; values of different types compare by type; numbers by value, strings ordinally ignoring case as the
 * shell's default comparison does (Windows uses the locale's collation, which agrees for plain text). */
static int sign_ll(LONGLONG a, LONGLONG b) { return a < b ? -1 : a > b; }
static int sign_ull(ULONGLONG a, ULONGLONG b) { return a < b ? -1 : a > b; }
static int sign_d(double a, double b) { return a < b ? -1 : a > b; }
static WCHAR fold(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
static int compare_text(const WCHAR *a, const WCHAR *b) {
  if (!a) a = L"";
  if (!b) b = L"";
  while (*a && fold(*a) == fold(*b)) { ++a; ++b; }
  return fold(*a) < fold(*b) ? -1 : fold(*a) > fold(*b);
}
EXPORT int WINAPI VariantCompare(const Variant *a, const Variant *b) {
  const BOOL a_empty = !a || a->vt == VT_EMPTY_ || a->vt == VT_NULL_, b_empty = !b || b->vt == VT_EMPTY_ || b->vt == VT_NULL_;
  if (a_empty || b_empty) return a_empty && b_empty ? 0 : a_empty ? -1 : 1;
  if (a->vt != b->vt) return a->vt < b->vt ? -1 : 1;
  switch (a->vt) {
    case VT_I1_: return sign_ll(a->v.i1, b->v.i1);
    case VT_UI1_: return sign_ll(a->v.u1, b->v.u1);
    case VT_I2_: case VT_BOOL_: return sign_ll(a->v.i2, b->v.i2);
    case VT_UI2_: return sign_ll(a->v.u2, b->v.u2);
    case VT_I4_: case VT_INT_: return sign_ll(a->v.i4, b->v.i4);
    case VT_UI4_: case VT_UINT_: return sign_ll(a->v.u4, b->v.u4);
    case VT_I8_: return sign_ll(a->v.i8, b->v.i8);
    case VT_UI8_: return sign_ull(a->v.u8, b->v.u8);
    case VT_R4_: return sign_d(a->v.r4, b->v.r4);
    case VT_R8_: return sign_d(a->v.r8, b->v.r8);
    case VT_BSTR_: case VT_LPWSTR_: return compare_text(a->v.bstr, b->v.bstr);
    default: return 0;      /* (types without an order here compare equal) */
  }
}
