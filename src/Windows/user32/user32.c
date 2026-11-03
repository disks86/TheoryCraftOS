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

/* user32.dll for TheoryCraftOS: the parts that are implemented (wsprintf so far); every other export is a stub (src/Windows/stubs/user32.inc).
 * Behaviour checked with Wine's tests (tests/windows/wine/user32). Built freestanding over kernel32.dll with the OS's mingw compiler. */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define WSPRINTF_LIMIT 1024      /* wsprintf writes at most this many characters, without the terminator */

typedef struct { WCHAR text[WSPRINTF_LIMIT + 1]; int length; } Out;
static void out_put(Out *o, WCHAR c) { if (o->length < WSPRINTF_LIMIT) o->text[o->length] = c; ++o->length; }

/* Formats into a wide buffer. `wide_strings` is what %s means (wide for wsprintfW, narrow for wsprintfA; %S, %hs, %ls and %C/%c pick the other kind). */
static int format_wide(Out *o, const void *format, int format_is_wide, int wide_strings, va_list_ args) {
  const char *fa = format;
  const WCHAR *fw = format;
#define FMT(i) (format_is_wide ? (WCHAR)fw[i] : (WCHAR)(BYTE)fa[i])
  for (int i = 0; FMT(i); ++i) {
    if (FMT(i) != '%') { out_put(o, FMT(i)); continue; }
    ++i;
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, precision = -1, size = 0;
    for (;; ++i) {
      const WCHAR c = FMT(i);
      if (c == '-') left = 1; else if (c == '0') zero = 1; else if (c == '+') plus = 1; else if (c == ' ') space = 1; else if (c == '#') alt = 1; else break;
    }
    if (FMT(i) == '*') { width = __builtin_va_arg(args, int); if (width < 0) { left = 1; width = -width; } ++i; }
    else while (FMT(i) >= '0' && FMT(i) <= '9') { if (width < 100000) width = width * 10 + (FMT(i) - '0'); ++i; }
    if (FMT(i) == '.') {
      ++i; precision = 0;
      if (FMT(i) == '*') { precision = __builtin_va_arg(args, int); ++i; }
      else while (FMT(i) >= '0' && FMT(i) <= '9') { if (precision < 100000) precision = precision * 10 + (FMT(i) - '0'); ++i; }
    }
    for (;; ++i) {      /* 1 = h, 4 = l, 8 = ll / I64 */
      const WCHAR c = FMT(i);
      if (c == 'h') size = 1; else if (c == 'l') size = size == 4 ? 8 : 4; else if (c == 'w') size = 4;
      else if (c == 'I' && FMT(i + 1) == '6' && FMT(i + 2) == '4') { size = 8; i += 2; }
      else break;
    }
    const WCHAR conversion = FMT(i);
    if (!conversion) break;
    WCHAR text[80];
    const WCHAR *body = text;
    const char *narrow = NULL;
    int length = 0;
    WCHAR sign = 0;
    switch (conversion) {
      case '%': out_put(o, '%'); continue;
      case 'c': case 'C': {
        const int wide = conversion == 'C' ? !wide_strings : wide_strings;
        const int w = size == 4 ? 1 : size == 1 ? 0 : wide;
        const unsigned v = __builtin_va_arg(args, unsigned);
        text[0] = w ? (WCHAR)v : (WCHAR)(BYTE)v; length = 1;
        break;
      }
      case 's': case 'S': {
        const int wide = conversion == 'S' ? !wide_strings : wide_strings;
        const int w = size == 4 ? 1 : size == 1 ? 0 : wide;
        const void *p = __builtin_va_arg(args, const void *);
        if (!p) { static const char nul[] = "(null)"; narrow = nul; length = 6; if (precision >= 0 && precision < length) length = precision; break; }
        if (w) { body = p; while (body[length] && (precision < 0 || length < precision)) ++length; }
        else { narrow = p; while (narrow[length] && (precision < 0 || length < precision)) ++length; }
        break;
      }
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
        unsigned long long value;
        int is_signed = conversion == 'd' || conversion == 'i';
        const unsigned base = conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 : 10;
        const char *digits = conversion == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
        if (conversion == 'p') { value = (unsigned long long)__builtin_va_arg(args, void *); is_signed = 0; if (precision < 0) precision = 16; }
        else if (is_signed) {
          long long v = size == 8 ? __builtin_va_arg(args, long long) : __builtin_va_arg(args, int);
          if (size == 1) v = (short)v;
          if (v < 0) { sign = '-'; value = 0ull - (unsigned long long)v; } else { value = (unsigned long long)v; if (plus) sign = '+'; else if (space) sign = ' '; }
        } else {
          value = size == 8 ? __builtin_va_arg(args, unsigned long long) : __builtin_va_arg(args, unsigned int);
          if (size == 1) value &= 0xFFFF;
        }
        char reversed[72];
        int n = 0;
        do { reversed[n++] = digits[value % base]; value /= base; } while (value);
        while (precision >= 0 && n < precision && n < 66) reversed[n++] = '0';
        if (alt && base == 16 && conversion != 'p') { reversed[n++] = conversion; reversed[n++] = '0'; }
        for (int k = 0; k < n; ++k) text[k] = (WCHAR)reversed[n - 1 - k];
        length = n;
        if (precision >= 0 && conversion != 'p') zero = 0;
        break;
      }
      default: out_put(o, '%'); out_put(o, conversion); continue;
    }
    const int pad = width - length - (sign ? 1 : 0);
    const int numeric = conversion != 's' && conversion != 'S' && conversion != 'c' && conversion != 'C';
    if (!left && !(zero && numeric)) for (int k = 0; k < pad; ++k) out_put(o, ' ');
    if (sign) out_put(o, sign);
    if (!left && zero && numeric) for (int k = 0; k < pad; ++k) out_put(o, '0');
    for (int k = 0; k < length; ++k) out_put(o, narrow ? (WCHAR)(BYTE)narrow[k] : body[k]);
    if (left) for (int k = 0; k < pad; ++k) out_put(o, ' ');
  }
#undef FMT
  return o->length;
}

EXPORT int WINAPI wvsprintfW(LPWSTR buffer, LPCWSTR format, va_list_ args) {
  Out o = {{0}, 0};
  format_wide(&o, format, 1, 1, args);
  const int n = o.length < WSPRINTF_LIMIT ? o.length : WSPRINTF_LIMIT;
  for (int i = 0; i < n; ++i) buffer[i] = o.text[i];
  buffer[n] = 0;
  return n;
}
EXPORT int WINAPI wvsprintfA(LPSTR buffer, LPCSTR format, va_list_ args) {
  Out o = {{0}, 0};
  format_wide(&o, format, 0, 0, args);
  const int n = o.length < WSPRINTF_LIMIT ? o.length : WSPRINTF_LIMIT;
  for (int i = 0; i < n; ++i) buffer[i] = o.text[i] < 0x100 ? (char)o.text[i] : '?';
  buffer[n] = 0;
  return n;
}
EXPORT int WINAPI wsprintfW(LPWSTR buffer, LPCWSTR format, ...) { va_list_ a; __builtin_va_start(a, format); const int r = wvsprintfW(buffer, format, a); __builtin_va_end(a); return r; }
EXPORT int WINAPI wsprintfA(LPSTR buffer, LPCSTR format, ...) { va_list_ a; __builtin_va_start(a, format); const int r = wvsprintfA(buffer, format, a); __builtin_va_end(a); return r; }

/* ---- the console window --------------------------------------------------------------------------------------------------------------------------------- *
 * There is no window system. The one window that exists is the window of the console of the process (kernel32's GetConsoleWindow); it is answered for, sized from the window of the
 * screen buffer shown (cells of the console font, 8 x 16) and put at a fixed place on a 1024 x 768 desktop whose task bar takes the lower 40 lines. */
typedef struct { LONG left, top, right, bottom; } RECT_;
IMPORT HANDLE WINAPI GetConsoleWindow(void);
IMPORT void WINAPI SetLastError(DWORD error);
#define SCREEN_W 1024
#define SCREEN_H 768
#define WORK_H 728
#define CAPTION_H 23
#define FRAME 8

EXPORT BOOL WINAPI IsWindow(HANDLE window) { return window && window == GetConsoleWindow(); }
IMPORT BOOL WINAPI TcosConsoleWindowCells(LONG *columns, LONG *rows);
/* The size of the cells the console window shows. */
static BOOL console_client_size(LONG *width, LONG *height) {
  LONG columns, rows;
  if (!TcosConsoleWindowCells(&columns, &rows)) return FALSE;
  *width = columns * 8;
  *height = rows * 16;
  return TRUE;
}
EXPORT BOOL WINAPI GetClientRect(HANDLE window, RECT_ *rect) {
  LONG w, h;
  if (!IsWindow(window) || !rect) { SetLastError(1400 /* ERROR_INVALID_WINDOW_HANDLE */); return FALSE; }
  if (!console_client_size(&w, &h)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  rect->left = rect->top = 0; rect->right = w; rect->bottom = h;
  return TRUE;
}
EXPORT BOOL WINAPI GetWindowRect(HANDLE window, RECT_ *rect) {
  LONG w, h;
  if (!IsWindow(window) || !rect) { SetLastError(1400); return FALSE; }
  if (!console_client_size(&w, &h)) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  rect->left = 100; rect->top = 100; rect->right = 100 + w + 2 * FRAME; rect->bottom = 100 + h + CAPTION_H + 2 * FRAME;
  return TRUE;
}
/* SM_CXSCREEN, SM_CYSCREEN, SM_CYCAPTION, SM_CXFRAME/SM_CYFRAME: the numbers of the desktop above; the others are 0. */
EXPORT int WINAPI GetSystemMetrics(int index) {
  switch (index) {
    case 0: return SCREEN_W;
    case 1: return SCREEN_H;
    case 4: return CAPTION_H;
    case 32: case 33: return FRAME;
    default: return 0;
  }
}
EXPORT BOOL WINAPI SystemParametersInfoW(UINT action, UINT param, PVOID data, UINT flags) {
  (void)param; (void)flags;
  if (action == 0x30 /* SPI_GETWORKAREA */ && data) { RECT_ *r = data; r->left = 0; r->top = 0; r->right = SCREEN_W; r->bottom = WORK_H; return TRUE; }
  SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}

/* ---- characters ------------------------------------------------------------------------------------------------------------------------------------------- */

/* The ANSI code page is single-byte here, so a character is a byte; wide letters are the Latin-1, Greek and Cyrillic ones that kernel32's case tables know. */
static unsigned char_upper(unsigned c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
  if (c == 0xFF) return 0x178;
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
  if (c >= 0x430 && c <= 0x44F) return c - 32;
  if (c >= 0x450 && c <= 0x45F) return c - 80;
  return c;
}
static unsigned char_lower(unsigned c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  return c;
}
static int char_is_alpha(unsigned c) { return char_upper(c) != char_lower(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == 0xAA || c == 0xB5 || c == 0xBA || c == 0xDF || c == 0xFF; }

EXPORT LPSTR WINAPI CharNextA(LPCSTR p) { return (LPSTR)(*p ? p + 1 : p); }
EXPORT LPSTR WINAPI CharNextExA(WORD code_page, LPCSTR p, DWORD flags) { (void)code_page; (void)flags; return CharNextA(p); }
EXPORT LPWSTR WINAPI CharNextW(LPCWSTR p) { return (LPWSTR)(*p ? p + 1 : p); }
EXPORT LPSTR WINAPI CharPrevA(LPCSTR start, LPCSTR p) { return (LPSTR)(p > start ? p - 1 : start); }
EXPORT LPSTR WINAPI CharPrevExA(WORD code_page, LPCSTR start, LPCSTR p, DWORD flags) { (void)code_page; (void)flags; return CharPrevA(start, p); }
EXPORT LPWSTR WINAPI CharPrevW(LPCWSTR start, LPCWSTR p) { return (LPWSTR)(p > start ? p - 1 : start); }
/* a single character in the low word is converted; a pointer is a string, converted in place */
EXPORT LPSTR WINAPI CharUpperA(LPSTR s) {
  if (((ULONG_PTR)s >> 16) == 0) return (LPSTR)(ULONG_PTR)char_upper((BYTE)(ULONG_PTR)s);
  for (LPSTR p = s; *p; ++p) *p = (char)char_upper((BYTE)*p);
  return s;
}
EXPORT LPSTR WINAPI CharLowerA(LPSTR s) {
  if (((ULONG_PTR)s >> 16) == 0) return (LPSTR)(ULONG_PTR)char_lower((BYTE)(ULONG_PTR)s);
  for (LPSTR p = s; *p; ++p) *p = (char)char_lower((BYTE)*p);
  return s;
}
EXPORT LPWSTR WINAPI CharUpperW(LPWSTR s) {
  if (((ULONG_PTR)s >> 16) == 0) return (LPWSTR)(ULONG_PTR)char_upper((WCHAR)(ULONG_PTR)s);
  for (LPWSTR p = s; *p; ++p) *p = (WCHAR)char_upper(*p);
  return s;
}
EXPORT LPWSTR WINAPI CharLowerW(LPWSTR s) {
  if (((ULONG_PTR)s >> 16) == 0) return (LPWSTR)(ULONG_PTR)char_lower((WCHAR)(ULONG_PTR)s);
  for (LPWSTR p = s; *p; ++p) *p = (WCHAR)char_lower(*p);
  return s;
}
EXPORT DWORD WINAPI CharUpperBuffA(LPSTR s, DWORD n) { DWORD i = 0; for (; s && i < n; ++i) s[i] = (char)char_upper((BYTE)s[i]); return i; }
EXPORT DWORD WINAPI CharLowerBuffA(LPSTR s, DWORD n) { DWORD i = 0; for (; s && i < n; ++i) s[i] = (char)char_lower((BYTE)s[i]); return i; }
EXPORT DWORD WINAPI CharUpperBuffW(LPWSTR s, DWORD n) { DWORD i = 0; for (; s && i < n; ++i) s[i] = (WCHAR)char_upper(s[i]); return i; }
EXPORT DWORD WINAPI CharLowerBuffW(LPWSTR s, DWORD n) { DWORD i = 0; for (; s && i < n; ++i) s[i] = (WCHAR)char_lower(s[i]); return i; }
EXPORT BOOL WINAPI IsCharAlphaA(CHAR c) { return char_is_alpha((BYTE)c); }
EXPORT BOOL WINAPI IsCharAlphaW(WCHAR c) { return char_is_alpha(c); }
EXPORT BOOL WINAPI IsCharAlphaNumericA(CHAR c) { return char_is_alpha((BYTE)c) || ((BYTE)c >= '0' && (BYTE)c <= '9'); }
EXPORT BOOL WINAPI IsCharAlphaNumericW(WCHAR c) { return char_is_alpha(c) || (c >= '0' && c <= '9'); }
EXPORT BOOL WINAPI IsCharUpperA(CHAR c) { return char_is_alpha((BYTE)c) && char_lower((BYTE)c) != (BYTE)c; }
EXPORT BOOL WINAPI IsCharUpperW(WCHAR c) { return char_is_alpha(c) && char_lower(c) != c; }
EXPORT BOOL WINAPI IsCharLowerA(CHAR c) { return char_is_alpha((BYTE)c) && char_upper((BYTE)c) != (BYTE)c; }
EXPORT BOOL WINAPI IsCharLowerW(WCHAR c) { return char_is_alpha(c) && char_upper(c) != c; }
