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
  if (!s || !find || !*find || !n) return NULL;
  const size_t_ m = wcslen(find);
  for (; n && *s; ++s, --n) {
    size_t_ k = 0;
    while (k < m && s[k] == find[k]) ++k;
    if (k == m) return (LPWSTR)s;
  }
  return NULL;
}
EXPORT LPWSTR WINAPI StrStrNIW(LPCWSTR s, LPCWSTR find, UINT n) {
  if (!s || !find || !*find || !n) return NULL;
  const size_t_ m = wcslen(find);
  for (; n && *s; ++s, --n) {
    size_t_ k = 0;
    while (k < m && up_char(s[k]) == up_char(find[k])) ++k;
    if (k == m) return (LPWSTR)s;
  }
  return NULL;
}
EXPORT DWORD WINAPI StrCatChainW(LPWSTR dst, DWORD max, DWORD offset, LPCWSTR src) {
  if (offset == (DWORD)-1) offset = dst ? (DWORD)wcslen(dst) : 0;
  if (!dst || !max || offset > max) return offset;      /* (an offset past the buffer is answered as it is, and nothing is written) */
  if (offset == max) offset = max - 1;
  if (src) {
    DWORD i = 0;
    while (src[i] && offset + i + 1 < max) { dst[offset + i] = src[i]; ++i; }
    offset += i;
  }
  dst[offset] = 0;
  return offset;
}

/* ---- the shell's helpers: STRRET, the registry, streams on files, associations ---------------------------------------------------------------------------------------- */

typedef LONG HRESULT_;
typedef BYTE *LPBYTE;
IMPORT int memcmp(const void *, const void *, size_t_);
static inline LONG hresult_from_win32(LONG e) { return e <= 0 ? e : (LONG)((e & 0xFFFF) | 0x80070000); }      /* (a function: the argument is often a call with side effects) */
#define HRESULT_FROM_WIN32_(e) hresult_from_win32((LONG)(e))
#define ERROR_KEY_HAS_CHILDREN 1020
#define E_OUTOFMEMORY_VALUE E_OUTOFMEMORY_
IMPORT DWORD WINAPI GetLastError(void);
/* a\b\c into out (a buffer of MAX_PATH_ * 2 characters), cut when too long */
static void swprintf_(WCHAR *out, const WCHAR *a, const WCHAR *b, const WCHAR *c, const WCHAR *d) {
  size_t_ n = 0;
  const WCHAR *parts[4] = {a, b, c, d};
  for (int i = 0; i < 4; ++i) for (const WCHAR *p = parts[i]; *p && n + 1 < MAX_PATH_ * 2; ++p) out[n++] = *p;
  out[n] = 0;
}
#include "shell.inc"

/* wnsprintf: ntdll's _vsnprintf with the limit rules of Windows (a limit of 0 or less, or output that does not fit, answers -1; the buffer then ends with a NUL). */
IMPORT int _vsnprintf(char *buffer, size_t_ capacity, const char *format, va_list_ args);
IMPORT int _vsnwprintf(WCHAR *buffer, size_t_ capacity, const WCHAR *format, va_list_ args);
EXPORT INT WINAPI wvnsprintfA(LPSTR out, INT limit, LPCSTR format, va_list_ args) {
  if (limit <= 0) {
    if (limit < 0 && out) out[0] = 0;
    return -1;
  }
  const int n = _vsnprintf(out, (size_t_)limit, format, args);
  if (n < 0 || n >= limit) {
    out[limit - 1] = 0;
    return -1;
  }
  return n;
}
EXPORT INT WINAPI wvnsprintfW(LPWSTR out, INT limit, LPCWSTR format, va_list_ args) {
  if (limit <= 0) {
    if (limit < 0 && out) out[0] = 0;
    return -1;
  }
  const int n = _vsnwprintf(out, (size_t_)limit, format, args);
  if (n < 0 || n >= limit) {
    out[limit - 1] = 0;
    return -1;
  }
  return n;
}
EXPORT INT WINAPI wnsprintfA(LPSTR out, INT limit, LPCSTR format, ...) { va_list_ a; __builtin_va_start(a, format); const int r = wvnsprintfA(out, limit, format, a); __builtin_va_end(a); return r; }
EXPORT INT WINAPI wnsprintfW(LPWSTR out, INT limit, LPCWSTR format, ...) { va_list_ a; __builtin_va_start(a, format); const int r = wvnsprintfW(out, limit, format, a); __builtin_va_end(a); return r; }

/* StrFromTimeInterval: " 16 min 40 sec" (the time is rounded to seconds; `digits` is how many significant digits of hours, minutes and seconds are shown, the rest become zeros). */
static int time_class(char *out, unsigned value, int *digits, const char *unit) {
  int d = 1;
  for (unsigned v = value; v >= 10; v /= 10) ++d;
  if (d > *digits) {
    unsigned p = 1;
    for (int i = 0; i < d - *digits; ++i) p *= 10;
    value = value / p * p;
    d = *digits;
  }
  *digits -= d;
  int n = 0;
  out[n++] = ' ';
  n += append_decimal(out + n, value);
  out[n++] = ' ';
  n += append_text(out + n, unit);
  out[n] = 0;
  return n;
}
static int format_time_interval(DWORD ms, int digits, char *out) {
  const ULONGLONG total = ((ULONGLONG)ms + 500) / 1000;
  const unsigned hours = (unsigned)(total / 3600), minutes = (unsigned)(total / 60 % 60), seconds = (unsigned)(total % 60);
  int n = 0;
  out[0] = 0;
  if (digits <= 0) return 0;
  if (hours) n += time_class(out + n, hours, &digits, "hr");
  if (digits > 0 && (minutes || n)) n += time_class(out + n, minutes, &digits, "min");
  if (digits > 0) n += time_class(out + n, seconds, &digits, "sec");
  return n;
}
EXPORT INT WINAPI StrFromTimeIntervalA(LPSTR out, UINT capacity, DWORD ms, int digits) {
  char t[64];
  const int n = format_time_interval(ms, digits, t);
  if (!out || !capacity) return n;
  narrow_into(t, out, capacity);
  return n;
}
EXPORT INT WINAPI StrFromTimeIntervalW(LPWSTR out, UINT capacity, DWORD ms, int digits) {
  char t[64];
  const int n = format_time_interval(ms, digits, t);
  if (!out || !capacity) return n;
  widen_into(t, out, capacity);
  return n;
}

/* StrFormatByteSizeEx: StrFormatByteSize with the digits shown either rounded (flag 1, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT) or truncated (flag 2). */
EXPORT HRESULT_ WINAPI StrFormatByteSizeEx(ULONGLONG size, DWORD flags, LPWSTR out, UINT capacity) {
  if ((flags != 1 && flags != 2) || !out || !capacity) return 0x80070057;
  char t[40];
  if (size < 1000) { int n = append_decimal(t, size); n += append_text(t + n, " bytes"); t[n] = 0; }
  else {
    static const char *const units[] = {" KB", " MB", " GB", " TB", " PB", " EB"};
    const int round = flags == 1;
    int k = 1, n = 0;
    for (; k <= 6; ++k) {
      const unsigned shift = 10u * (unsigned)k;
      int found = 0;
      for (int d = 2; d >= 0 && !found; --d) {
        unsigned long long scale = d == 2 ? 100 : d == 1 ? 10 : 1;
        unsigned __int128 x = (unsigned __int128)size * scale;
        x = round ? (x + ((unsigned __int128)1 << (shift - 1))) >> shift : x >> shift;
        if (x >= 1000) continue;      /* (it needs a bigger unit, or fewer decimals) */
        const ULONGLONG whole = (ULONGLONG)x / scale, frac = (ULONGLONG)x % scale;
        n = append_decimal(t, whole);
        if (d) { t[n++] = '.'; if (d == 2) t[n++] = (char)('0' + frac / 10); t[n++] = (char)('0' + frac % 10); }
        found = 1;
      }
      if (found) break;
    }
    if (k > 6) k = 6;
    n += append_text(t + n, units[k - 1]);
    t[n] = 0;
  }
  widen_into(t, out, capacity);
  return 0;
}

/* SHAnsiToAnsi and SHUnicodeToUnicode (ordinals 345 and 346): a bounded copy; the answer counts the NUL, 0 is an error. */
EXPORT DWORD WINAPI SHAnsiToAnsi(LPCSTR src, LPSTR dst, int capacity) {
  if (!src || !dst || capacity <= 0) return 0;
  int n = 0;
  for (; n + 1 < capacity && src[n]; ++n) dst[n] = src[n];
  dst[n] = 0;
  return (DWORD)n + 1;
}
EXPORT DWORD WINAPI SHUnicodeToUnicode(LPCWSTR src, LPWSTR dst, int capacity) {
  if (!src || !dst || capacity <= 0) return 0;
  int n = 0;
  for (; n + 1 < capacity && src[n]; ++n) dst[n] = src[n];
  dst[n] = 0;
  return (DWORD)n + 1;
}

/* PathIsValidCharA/W (ordinals 455 and 456): `class` is a mask of what the caller accepts (0x100 any other valid character, 0x80 a quote, ...); a character is valid if the mask meets its classes. */
static const DWORD g_char_class[0x7f] = {
  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
  0x00000080, 0x00000100, 0x00000200, 0x00000100, 0x00000100, 0x00000100, 0x00000100, 0x00000100,
  0x00000100, 0x00000100, 0x00000002, 0x00000100, 0x00000040, 0x00000100, 0x00000004, 0x00000000,
  0x00000100, 0x00000100, 0x00000100, 0x00000100, 0x00000100, 0x00000100, 0x00000100, 0x00000100,
  0x00000100, 0x00000100, 0x00000010, 0x00000020, 0x00000000, 0x00000100, 0x00000000, 0x00000001,
  0x00000100, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
  0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
  0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
  0xffffffff, 0xffffffff, 0xffffffff, 0x00000100, 0x00000008, 0x00000100, 0x00000100, 0x00000100,
  0x00000100, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
  0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
  0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
  0xffffffff, 0xffffffff, 0xffffffff, 0x00000100, 0x00000000, 0x00000100, 0x00000100,
};
EXPORT BOOL WINAPI PathIsValidCharA(char c, DWORD class) { return (unsigned char)c > 0x7e ? (class & 0x100) != 0 : (class & g_char_class[(unsigned char)c]) != 0; }
EXPORT BOOL WINAPI PathIsValidCharW(WCHAR c, DWORD class) { return c > 0x7e ? (class & 0x100) != 0 : (class & g_char_class[c]) != 0; }

IMPORT DWORD WINAPI SearchPathA(LPCSTR path, LPCSTR file, LPCSTR ext, DWORD capacity, LPSTR buffer, LPSTR *file_part);
IMPORT DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD capacity, LPSTR buffer, LPSTR *file_part);
IMPORT DWORD WINAPI GetFullPathNameW(LPCWSTR name, DWORD capacity, LPWSTR buffer, LPWSTR *file_part);
/* PathSearchAndQualify: the file on the search path, else its full name. */
EXPORT BOOL WINAPI PathSearchAndQualifyA(LPCSTR path, LPSTR buffer, UINT capacity) {
  if (SearchPathA(NULL, path, NULL, capacity, buffer, NULL)) return TRUE;
  return GetFullPathNameA(path, capacity, buffer, NULL) != 0;
}
EXPORT BOOL WINAPI PathSearchAndQualifyW(LPCWSTR path, LPWSTR buffer, UINT capacity) {
  if (SearchPathW(NULL, path, NULL, capacity, buffer, NULL)) return TRUE;
  return GetFullPathNameW(path, capacity, buffer, NULL) != 0;
}

/* PathUnExpandEnvStrings: a leading folder that an environment variable holds ("C:\Windows\System32" -> "%SystemRoot%\System32"); the longest value wins. */
static BOOL unexpand_env(const WCHAR *path, WCHAR *out, UINT capacity) {
  static const WCHAR *const names[] = {L"ALLUSERSPROFILE", L"APPDATA", L"ProgramData", L"ProgramFiles", L"SystemRoot", L"SystemDrive", L"USERPROFILE"};
  if (!path || !out) return FALSE;
  const WCHAR *best = NULL;
  size_t_ best_length = 0;
  WCHAR value[MAX_PATH_];
  for (size_t_ i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    const DWORD n = GetEnvironmentVariableW(names[i], value, MAX_PATH_);
    if (!n || n >= MAX_PATH_) continue;
    size_t_ k = 0;
    while (k < n && path[k] && up_char(path[k]) == up_char(value[k])) ++k;
    if (k == n && n > best_length) { best = names[i]; best_length = n; }
  }
  if (!best) return FALSE;
  const size_t_ name_length = wcslen(best), rest = wcslen(path + best_length);
  if (name_length + 2 + rest + 1 > capacity) return FALSE;
  size_t_ n = 0;
  out[n++] = '%';
  for (size_t_ i = 0; i < name_length; ++i) out[n++] = best[i];
  out[n++] = '%';
  for (size_t_ i = 0; i <= rest; ++i) out[n++] = path[best_length + i];
  return TRUE;
}
EXPORT BOOL WINAPI PathUnExpandEnvStringsW(LPCWSTR path, LPWSTR out, UINT capacity) { return unexpand_env(path, out, capacity); }
EXPORT BOOL WINAPI PathUnExpandEnvStringsA(LPCSTR path, LPSTR out, UINT capacity) {
  WCHAR wide[MAX_PATH_ * 2], result[MAX_PATH_ * 2];
  if (!path || !out || !MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH_ * 2)) return FALSE;
  if (!unexpand_env(wide, result, MAX_PATH_ * 2)) return FALSE;
  char narrow[MAX_PATH_ * 2];
  if (!WideCharToMultiByte(CP_ACP, 0, result, -1, narrow, sizeof(narrow), NULL, NULL)) return FALSE;
  UINT n = 0;
  while (narrow[n]) ++n;
  if (n + 1 > capacity) return FALSE;
  for (UINT i = 0; i <= n; ++i) out[i] = narrow[i];
  return TRUE;
}

/* ---- PathCreateFromUrl ------------------------------------------------------------------------------------------------------------------------------------------ */

#define URL_PATH_MAX 4200
static int is_slash_(WCHAR c) { return c == '/' || c == '\\'; }
static int is_drive_spec(const WCHAR *s) { return ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) && (s[1] == ':' || s[1] == '|'); }
static int hex_digit(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* Appends `text` to `out` with its slashes turned around and (when `unescape`) its %XX escapes decoded; a decoded character is not changed again. Returns the new length, or -1 if it does not fit. */
static int url_append(WCHAR *out, int n, const WCHAR *text, int unescape) {
  for (; *text; ++text) {
    if (n + 1 >= URL_PATH_MAX) return -1;
    WCHAR c = *text == '/' ? '\\' : *text;
    if (unescape && *text == '%' && hex_digit(text[1]) >= 0 && hex_digit(text[2]) >= 0) {
      c = (WCHAR)(hex_digit(text[1]) * 16 + hex_digit(text[2]));
      text += 2;
    }
    out[n++] = c;
  }
  return n;
}
/* "file:" URLs and what Windows makes of the slashes after the colon (see Wine's PathCreateFromUrl table: one or three slashes keep a lone backslash, two or more a UNC name, a drive letter drops them). */
static HRESULT_ url_to_path(const WCHAR *url, WCHAR *out) {
  static const WCHAR file[] = {'f', 'i', 'l', 'e', ':', 0};
  for (int i = 0; i < 5; ++i) if (!url[i] || up_char(url[i]) != up_char(file[i])) return 0x80070057;
  const WCHAR *src = url + 5;
  int slashes = 0;
  while (is_slash_(src[slashes])) ++slashes;
  src += slashes;
  int n = 0, unescape = 1;
  const WCHAR *rest = src;
  if (slashes == 2 || slashes == 4) {
    if (slashes == 4) unescape = 0;
    static const WCHAR local[] = {'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', 0};
    int k = 0;
    while (local[k] && src[k] && up_char(src[k]) == up_char(local[k])) ++k;
    if (slashes == 2 && !local[k] && is_slash_(src[k]) && is_drive_spec(src + k + 1)) rest = src + k + 1;
    else if (is_drive_spec(src)) { if (slashes == 2) unescape = 0; }
    else {
      /* a host: its trailing slash is dropped when a drive follows */
      int host = 0;
      while (src[host] && !is_slash_(src[host])) ++host;
      WCHAR head[URL_PATH_MAX];
      if (host >= URL_PATH_MAX - 4) return 0x8007007A;
      for (int i = 0; i < host; ++i) head[i] = src[i];
      head[host] = 0;
      out[n++] = '\\'; out[n++] = '\\';
      n = url_append(out, n, head, unescape);
      if (n < 0) return 0x8007007A;
      rest = src + host;
      if (is_slash_(rest[0]) && is_drive_spec(rest + 1)) ++rest;
    }
  } else if (slashes >= 5) {
    out[n++] = '\\'; out[n++] = '\\';
  } else if (slashes == 1 || slashes == 3) {
    if (!is_drive_spec(src)) out[n++] = '\\';
  }
  const int start = n;
  n = url_append(out, n, rest, unescape);
  if (n < 0) return 0x8007007A;
  out[n] = 0;
  /* "c|" is "c:" at the start of a path with a drive */
  if (start == 0 && is_drive_spec(out) && out[1] == '|') out[1] = ':';
  return 0;
}
EXPORT HRESULT_ WINAPI PathCreateFromUrlW(LPCWSTR url, LPWSTR path, DWORD *length, DWORD reserved) {
  (void)reserved;
  if (!url || !path || !length || !*length) return 0x80070057;
  WCHAR result[URL_PATH_MAX];
  const HRESULT_ hr = url_to_path(url, result);
  if (hr) return hr;
  const DWORD n = (DWORD)wcslen(result);
  if (n + 1 > *length) { *length = n + 1; return 0x80004003; }
  for (DWORD i = 0; i <= n; ++i) path[i] = result[i];
  *length = n;
  return 0;
}
EXPORT HRESULT_ WINAPI PathCreateFromUrlAlloc(LPCWSTR url, LPWSTR *path, DWORD reserved) {
  (void)reserved;
  if (!url || !path) return 0x80070057;
  WCHAR result[URL_PATH_MAX];
  const HRESULT_ hr = url_to_path(url, result);
  if (hr) return hr;
  const size_t_ n = wcslen(result);
  *path = LocalAlloc(0, (n + 1) * sizeof(WCHAR));
  if (!*path) return 0x8007000E;
  for (size_t_ i = 0; i <= n; ++i) (*path)[i] = result[i];
  return 0;
}
EXPORT HRESULT_ WINAPI PathCreateFromUrlA(LPCSTR url, LPSTR path, DWORD *length, DWORD reserved) {
  if (!url || !path || !length || !*length) return 0x80070057;
  WCHAR wide[URL_PATH_MAX], result[URL_PATH_MAX];
  if (!MultiByteToWideChar(CP_ACP, 0, url, -1, wide, URL_PATH_MAX)) return 0x80070057;
  DWORD n = URL_PATH_MAX;
  const HRESULT_ hr = PathCreateFromUrlW(wide, result, &n, reserved);
  if (hr) return hr;
  char narrow[URL_PATH_MAX * 3];
  const int bytes = WideCharToMultiByte(CP_ACP, 0, result, -1, narrow, sizeof(narrow), NULL, NULL);
  if (bytes <= 0) return 0x80070057;
  if ((DWORD)bytes > *length) { *length = (DWORD)bytes; return 0x80004003; }
  for (int i = 0; i < bytes; ++i) path[i] = narrow[i];
  *length = (DWORD)bytes - 1;
  return 0;
}

#include "reg.inc"
#include "misc.inc"
#include "url.inc"
