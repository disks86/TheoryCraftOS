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

/* oleaut32.dll for TheoryCraftOS: BSTRs, SAFEARRAYs, VARIANTs (conversions, arithmetic, formatting, dates), error info, the active object table, IDispatch helpers,
 * the standard font and picture, type libraries and the marshalling of the automation types. Freestanding, built over kernel32.dll with the OS's mingw compiler.
 * The pieces are .inc files included here (one translation unit, so static helpers are shared); behaviour is checked with Wine's tests (tests/windows/wine/oleaut32),
 * which scripts/oleaut32-wine.sh also runs on a host with Wine against a native build of this file. */
#define _OLEAUT32_
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <olectl.h>
#include <ocidl.h>
#include <oaidl.h>
#include <string.h>
#include <wchar.h>

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#pragma GCC diagnostic ignored "-Wunused-function"
#define EXPORT __declspec(dllexport)

/* The compiler calls these for block copies and clears; they are defined here so that the DLL needs no C library (-fno-tree-loop-distribute-patterns keeps the loops loops). */
void *memcpy(void *dest, const void *source, size_t n) { unsigned char *d = dest; const unsigned char *s = source; while (n--) *d++ = *s++; return dest; }
void *memmove(void *dest, const void *source, size_t n) {
  unsigned char *d = dest; const unsigned char *s = source;
  if (d < s) while (n--) *d++ = *s++;
  else { d += n; s += n; while (n--) *--d = *--s; }
  return dest;
}
void *memset(void *dest, int c, size_t n) { unsigned char *d = dest; while (n--) *d++ = (unsigned char)c; return dest; }
int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
  return 0;
}

/* ---- memory: the process heap (what CoTaskMemAlloc is on Windows, too) ---------------------------------------------------------------------------------- */
static void *oa_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
static void *oa_calloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void *oa_realloc(void *p, SIZE_T n) { return p ? HeapReAlloc(GetProcessHeap(), 0, p, n ? n : 1) : oa_alloc(n); }
static void oa_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* ---- the C library is not available: small replacements, named oa_* so that they never clash with an import ------------------------------------------------ */
static size_t oa_wcslen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static int oa_wcscmp(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b; }
static WCHAR oa_towupper(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
static WCHAR oa_towlower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
#define OA_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ---- math without a C library (SSE2 only: no round instructions assumed) ------------------------------------------------------------------------------- */
static double oa_fabs(double x) { return __builtin_fabs(x); }
static double oa_sqrt(double x) { return __builtin_sqrt(x); }
static double oa_trunc(double x) {
  if (!(oa_fabs(x) < 4503599627370496.0)) return x;      /* 2^52: no fraction bits left (also NaN, infinities) */
  double t = (double)(long long)x;
  return t;
}
static double oa_floor(double x) { double t = oa_trunc(x); return t > x ? t - 1.0 : t; }
static double oa_ceil(double x) { double t = oa_trunc(x); return t < x ? t + 1.0 : t; }
static double oa_fmod(double x, double y) {
  if (y == 0.0 || x != x || y != y) return (x - x) / (y - y);
  double ay = oa_fabs(y), r = oa_fabs(x);
  if (r < ay) return x;
  /* exact long division by repeated scaling */
  while (r >= ay) {
    double m = ay;
    while (m * 2.0 <= r && m * 2.0 > m) m *= 2.0;
    r -= m;
  }
  return x < 0 ? -r : r;
}
/* x^n for integer n by squaring */
static double oa_powi(double x, int n) {
  int neg = n < 0;
  unsigned long long e = neg ? (unsigned long long)(-(long long)n) : (unsigned long long)n;
  double r = 1.0;
  while (e) { if (e & 1) r *= x; x *= x; e >>= 1; }
  return neg ? 1.0 / r : r;
}
static double oa_exp(double x) {
  if (x != x) return x;
  if (x > 709.78) return __builtin_huge_val();
  if (x < -745.2) return 0.0;
  int k = (int)oa_floor(x / 0.6931471805599453 + 0.5);
  double r = x - k * 0.6931471805599453, term = 1.0, sum = 1.0;
  for (int i = 1; i < 30; i++) { term *= r / i; sum += term; }
  while (k > 0) { sum *= 2.0; k--; }
  while (k < 0) { sum *= 0.5; k++; }
  return sum;
}
static double oa_log(double x) {
  if (x != x || x < 0) return (x - x) / (x - x);
  if (x == 0.0) return -__builtin_huge_val();
  if (x == __builtin_huge_val()) return x;
  int e = 0;
  while (x >= 2.0) { x *= 0.5; e++; }
  while (x < 1.0) { x *= 2.0; e--; }
  if (x > 1.4142135623730951) { x *= 0.5; e++; }
  double z = (x - 1.0) / (x + 1.0), z2 = z * z, term = z, sum = 0.0;
  for (int i = 1; i < 60; i += 2) { sum += term / i; term *= z2; }
  return 2.0 * sum + e * 0.6931471805599453;
}
static double oa_pow(double x, double y) {
  if (y == 0.0) return 1.0;
  if (x != x || y != y) return x + y;
  if (y == oa_trunc(y) && oa_fabs(y) < 2147483648.0) return oa_powi(x, (int)y);
  if (x < 0) return (x - x) / (x - x);
  if (x == 0.0) return y > 0 ? 0.0 : __builtin_huge_val();
  return oa_exp(y * oa_log(x));
}

/* ---- the pieces ------------------------------------------------------------------------------------------------------------------------------------------ */
#if __has_include("bstr.inc") && !defined(OA_SKIP_bstr)
#include "bstr.inc"
#endif
#if __has_include("safearray.inc") && !defined(OA_SKIP_safearray)
#include "safearray.inc"
#endif
#if __has_include("variant.inc") && !defined(OA_SKIP_variant)
#include "variant.inc"
#endif
#if __has_include("decimal.inc") && !defined(OA_SKIP_decimal)
#include "decimal.inc"
#endif
#if __has_include("parse.inc") && !defined(OA_SKIP_parse)
#include "parse.inc"
#endif
#if __has_include("conv.inc") && !defined(OA_SKIP_conv)
#include "conv.inc"
#endif
#if __has_include("ops.inc") && !defined(OA_SKIP_ops)
#include "ops.inc"
#endif
#if __has_include("date.inc") && !defined(OA_SKIP_date)
#include "date.inc"
#endif
#if __has_include("format.inc") && !defined(OA_SKIP_format)
#include "format.inc"
#endif
#if __has_include("errorinfo.inc") && !defined(OA_SKIP_errorinfo)
#include "errorinfo.inc"
#endif
#if __has_include("dispatch.inc") && !defined(OA_SKIP_dispatch)
#include "dispatch.inc"
#endif
#if __has_include("stdobj.inc") && !defined(OA_SKIP_stdobj)
#include "stdobj.inc"
#endif
#if __has_include("typelib.inc") && !defined(OA_SKIP_typelib)
#include "typelib.inc"
#endif
#if __has_include("marshal.inc") && !defined(OA_SKIP_marshal)
#include "marshal.inc"
#endif

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved) {
  (void)instance;
#ifdef OA_ERRORINFO_DLLMAIN
  switch (reason) {
    case DLL_PROCESS_ATTACH: errorinfo_process_attach(); break;
    case DLL_THREAD_DETACH: errorinfo_thread_detach(); break;
    case DLL_PROCESS_DETACH: if (!reserved) errorinfo_process_detach(); break;
    default: break;
  }
#else
  (void)reason;
#endif
  return TRUE;
}
