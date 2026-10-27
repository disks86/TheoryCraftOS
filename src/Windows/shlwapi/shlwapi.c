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

/* shlwapi.dll for TheoryCraftOS: the string and path functions (StrCmp*, StrStr*, StrTo*, Path*), written once for narrow and wide characters (str.inc is included twice).
 * Behaviour checked with Wine's tests (tests/windows/wine/shlwapi). Built freestanding over kernel32.dll with the OS's mingw compiler. */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

typedef WORD *LPWORD;
typedef long long INT64_;
typedef struct _FILETIME_ { DWORD dwLowDateTime, dwHighDateTime; } FILETIME_;

IMPORT HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T bytes);
IMPORT HLOCAL WINAPI LocalFree(HLOCAL memory);
IMPORT DWORD WINAPI GetFileAttributesA(LPCSTR path);
IMPORT DWORD WINAPI GetFileAttributesW(LPCWSTR path);
IMPORT DWORD WINAPI GetCurrentDirectoryW(DWORD capacity, LPWSTR buffer);
IMPORT DWORD WINAPI GetCurrentDirectoryA(DWORD capacity, LPSTR buffer);
IMPORT DWORD WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD capacity);
IMPORT void WINAPI SetLastError(DWORD error);
IMPORT int WINAPI lstrlenA(LPCSTR s);
IMPORT int WINAPI lstrlenW(LPCWSTR s);
IMPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int length, LPWSTR dest, int capacity);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int length, LPSTR dest, int capacity, LPCSTR default_char, LPBOOL used_default);
IMPORT void *memcpy(void *, const void *, size_t_);
IMPORT void *memmove(void *, const void *, size_t_);
IMPORT void *memset(void *, int, size_t_);
IMPORT size_t_ strlen(const char *);
IMPORT size_t_ wcslen(const WCHAR *);

#define MAX_PATH_ 260
#define E_INVALIDARG_ ((LONG)0x80070057)
#define E_OUTOFMEMORY_ ((LONG)0x8007000E)
#define S_OK_ 0
#define S_FALSE_ 1
#define ERROR_INVALID_PARAMETER_ 87
#define ERROR_FILENAME_EXCED_RANGE_ 206
#define FILE_ATTRIBUTE_DIRECTORY_ 0x10u

/* ---- character helpers ----------------------------------------------------------------------------------------------------------------------------------- */

static unsigned up_char(unsigned c) {
  if (c < 128) return c >= 'a' && c <= 'z' ? c - 32 : c;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7 && c < 256) return c - 32;
  return c;
}
EXPORT BOOL WINAPI ChrCmpIA(WORD a, WORD b) { return up_char(a & 0xFF) != up_char(b & 0xFF); }
EXPORT BOOL WINAPI ChrCmpIW(WCHAR a, WCHAR b) { return up_char(a) != up_char(b); }

#define SUF A
#define CHAR_T CHAR
#define UPC(c) ((CHAR)up_char((BYTE)(c)))
#define UV(c) ((unsigned)(BYTE)(c))
#define LEN(s) strlen(s)
#define TXT(s) s
#include "str.inc"
#undef SUF
#undef CHAR_T
#undef UPC
#undef UV
#undef LEN
#undef TXT
#define SUF W
#define CHAR_T WCHAR
#define UPC(c) ((WCHAR)up_char(c))
#define UV(c) ((unsigned)(WCHAR)(c))
#define LEN(s) wcslen(s)
#define TXT(s) L##s
#include "str.inc"
#undef SUF
#undef CHAR_T
#undef UPC
#undef UV
#undef LEN
#undef TXT

/* ---- sizes as text ------------------------------------------------------------------------------------------------------------------------------------------ */

static int append_decimal(char *out, ULONGLONG v) {
  char tmp[24];
  int n = 0;
  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  return n;
}
static int append_text(char *out, const char *text) { int n = 0; while (text[n]) { out[n] = text[n]; ++n; } return n; }
/* "309 bytes", "9.95 KB", "98.0 KB", "998 KB", "0.99 MB": three digits, truncated, the unit chosen so that the number stays below 1000. */
static int format_byte_size(LONGLONG size, char *out) {
  static const char *const units[] = {" KB", " MB", " GB", " TB", " PB", " EB"};
  int n = 0;
  if (size < 0) { out[n++] = '-'; n += append_decimal(out + n, (ULONGLONG)(0 - (ULONGLONG)size)); n += append_text(out + n, " bytes"); out[n] = 0; return n; }
  const ULONGLONG v = (ULONGLONG)size;
  if (v < 1000) { n += append_decimal(out, v); n += append_text(out + n, " bytes"); out[n] = 0; return n; }
  int k = 1;
  while (k < 6 && v >= 1000ull * (1ull << (10 * k))) ++k;
  const unsigned shift = 10u * (unsigned)k;
  const ULONGLONG part = v >> shift, rest = v & ((1ull << shift) - 1);
  /* thousandths of the unit, truncated: rest * 1000 >> shift, without a 128-bit product (rest < 2^60: split it) */
  ULONGLONG thousandths = ((rest >> 20) * 1000 >> (shift - 20)) + (((rest & 0xFFFFF) * 1000) >> shift);
  if (thousandths > 999) thousandths = 999;
  n += append_decimal(out, part);
  if (part < 10) { out[n++] = '.'; out[n++] = (char)('0' + thousandths / 100); out[n++] = (char)('0' + thousandths / 10 % 10); }
  else if (part < 100) { out[n++] = '.'; out[n++] = (char)('0' + thousandths / 100); }
  n += append_text(out + n, units[k - 1]);
  out[n] = 0;
  return n;
}
static int format_kb_size(LONGLONG size, char *out) {
  ULONGLONG kb = size < 0 ? 0 : ((ULONGLONG)size + 1023) / 1024;
  char digits[24];
  const int d = append_decimal(digits, kb);
  int n = 0;
  for (int i = 0; i < d; ++i) {
    if (i && (d - i) % 3 == 0) out[n++] = ',';
    out[n++] = digits[i];
  }
  n += append_text(out + n, " KB");
  out[n] = 0;
  return n;
}
static void widen_into(const char *text, LPWSTR out, UINT capacity) {
  UINT i = 0;
  for (; text[i] && i + 1 < capacity; ++i) out[i] = (WCHAR)(BYTE)text[i];
  if (capacity) out[i] = 0;
}
static void narrow_into(const char *text, LPSTR out, UINT capacity) {
  UINT i = 0;
  for (; text[i] && i + 1 < capacity; ++i) out[i] = text[i];
  if (capacity) out[i] = 0;
}
EXPORT LPSTR WINAPI StrFormatByteSize64A(LONGLONG size, LPSTR buffer, UINT capacity) { char t[40]; format_byte_size(size, t); narrow_into(t, buffer, capacity); return buffer; }
EXPORT LPSTR WINAPI StrFormatByteSizeA(DWORD size, LPSTR buffer, UINT capacity) { return StrFormatByteSize64A((LONGLONG)size, buffer, capacity); }
EXPORT LPWSTR WINAPI StrFormatByteSizeW(LONGLONG size, LPWSTR buffer, UINT capacity) { char t[40]; format_byte_size(size, t); widen_into(t, buffer, capacity); return buffer; }
EXPORT LPSTR WINAPI StrFormatKBSizeA(LONGLONG size, LPSTR buffer, UINT capacity) { char t[40]; format_kb_size(size, t); narrow_into(t, buffer, capacity); return buffer; }
EXPORT LPWSTR WINAPI StrFormatKBSizeW(LONGLONG size, LPWSTR buffer, UINT capacity) { char t[40]; format_kb_size(size, t); widen_into(t, buffer, capacity); return buffer; }

/* ---- a few more wide-string functions ------------------------------------------------------------------------------------------------------------------------- */

EXPORT LPWSTR WINAPI StrCpyW(LPWSTR dst, LPCWSTR src) { LPWSTR p = dst; while ((*p++ = *src++)) {} return dst; }
EXPORT LPSTR WINAPI StrCpyA(LPSTR dst, LPCSTR src) { LPSTR p = dst; while ((*p++ = *src++)) {} return dst; }
EXPORT LPWSTR WINAPI StrChrNW(LPCWSTR s, WCHAR c, UINT n) {
  if (!s) return NULL;
  for (; n && *s; ++s, --n) if (*s == c) return (LPWSTR)s;
  return NULL;
}
EXPORT LPWSTR WINAPI StrStrNW(LPCWSTR s, LPCWSTR find, UINT n) {
  if (!s || !find || !*find) return NULL;
  const size_t_ m = wcslen(find);
  for (; n >= m && *s; ++s, --n) {
    size_t_ k = 0;
    while (k < m && s[k] == find[k]) ++k;
    if (k == m) return (LPWSTR)s;
  }
  return NULL;
}
EXPORT LPWSTR WINAPI StrStrNIW(LPCWSTR s, LPCWSTR find, UINT n) {
  if (!s || !find || !*find) return NULL;
  const size_t_ m = wcslen(find);
  for (; n >= m && *s; ++s, --n) {
    size_t_ k = 0;
    while (k < m && up_char(s[k]) == up_char(find[k])) ++k;
    if (k == m) return (LPWSTR)s;
  }
  return NULL;
}
EXPORT DWORD WINAPI StrCatChainW(LPWSTR dst, DWORD max, DWORD offset, LPCWSTR src) {
  if (!dst || !max) return offset;
  if (offset == (DWORD)-1) offset = (DWORD)wcslen(dst);
  if (offset >= max) return offset;
  if (src) {
    DWORD i = 0;
    while (src[i] && offset + i + 1 < max) { dst[offset + i] = src[i]; ++i; }
    offset += i;
  }
  dst[offset] = 0;
  return offset;
}
