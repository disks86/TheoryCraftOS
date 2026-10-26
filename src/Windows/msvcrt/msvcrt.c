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

/* msvcrt.dll for TheoryCraftOS: the C runtime that Windows programs built with mingw (or Visual C++) import. It exports the classic names (msvcrt.dll: printf, malloc,
 * __getmainargs, ...) and the Universal CRT's (ucrtbase.dll and the api-ms-win-crt-* sets: __stdio_common_vfprintf, _configure_narrow_argv, __acrt_iob_func, ...), because
 * the OS folds all of those names into this one library. Written over kernel32.dll; single-threaded; text is UTF-8; floating point is formatted with 80-bit long doubles
 * (exact for the usual 15-17 digits, not for more). Built freestanding with the OS's mingw compiler. */
#include "../tcos_win.h"
#include "../kernel32.h"
typedef unsigned short wchar_t;

#define API __declspec(dllexport)
#undef EXPORT
#define EXPORT API
typedef unsigned long long size_t_;
typedef unsigned long long uintptr_t_;
typedef long long intptr_t;
typedef __builtin_va_list va_list_;
#define va_start_(a, b) __builtin_va_start(a, b)
#define va_end_(a) __builtin_va_end(a)
#define va_arg_(a, t) __builtin_va_arg(a, t)
#define va_copy_(a, b) __builtin_va_copy(a, b)
#ifndef EOF
#define EOF (-1)
#endif

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define LIBC_NO_STRTOL      /* (this library has the complete ones) */
#include "../libc_string.inc"
#ifndef ERROR_INVALID_DRIVE
#define ERROR_INVALID_DRIVE 15
#endif
#ifndef ERROR_WRITE_PROTECT
#define ERROR_WRITE_PROTECT 19
#endif

/* ====================================================================================================================================================================
 * The errno variable, the arguments, the environment and what start-up and exit need
 * ==================================================================================================================================================================== */

static int g_errno, g_doserrno;
API int *_errno(void) { return &g_errno; }
API unsigned long *__doserrno(void) { return (unsigned long *)&g_doserrno; }
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOSPC 28
#define ESPIPE 29
#define EDOM 33
#define ERANGE 34
#define ENOTEMPTY 41
#define EILSEQ 42

/* Records a Win32 failure the way the CRT does: errno from the last error. */
static void map_last_error(void) {
  const DWORD e = GetLastError();
  g_doserrno = (int)e;
  switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_NO_MORE_FILES: g_errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_WRITE_PROTECT: g_errno = EACCES; break;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: g_errno = EEXIST; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: g_errno = ENOMEM; break;
    case ERROR_DISK_FULL: g_errno = ENOSPC; break;
    case ERROR_DIR_NOT_EMPTY: g_errno = ENOTEMPTY; break;
    case ERROR_TOO_MANY_OPEN_FILES: g_errno = EMFILE; break;
    case ERROR_DIRECTORY: g_errno = ENOTDIR; break;
    case ERROR_INVALID_HANDLE: g_errno = EBADF; break;
    default: g_errno = EINVAL; break;
  }
}

static const char *const g_error_texts[] = {
    "No error", "Operation not permitted", "No such file or directory", "No such process", "Interrupted function call", "Input/output error", "No such device or address",
    "Arg list too long", "Exec format error", "Bad file descriptor", "No child processes", "Resource temporarily unavailable", "Not enough space", "Permission denied",
    "Bad address", "Unknown error", "Resource device", "File exists", "Improper link", "No such device", "Not a directory", "Is a directory", "Invalid argument",
    "Too many open files in system", "Too many open files", "Inappropriate I/O control operation", "Unknown error", "File too large", "No space left on device",
    "Invalid seek", "Read-only file system", "Too many links", "Broken pipe", "Domain error", "Result too large"};
API char *strerror(int e) { return (char *)((e >= 0 && e < (int)(sizeof(g_error_texts) / sizeof(g_error_texts[0]))) ? g_error_texts[e] : "Unknown error"); }
API char *_strerror(const char *prefix) { (void)prefix; return strerror(g_errno); }
API int strerror_s(char *buffer, size_t_ size, int e) { const char *t = strerror(e); size_t_ n = strlen(t); if (n + 1 > size) return ERANGE; memcpy(buffer, t, n + 1); return 0; }

/* The process heap serves malloc. Blocks are 16-byte aligned. */
API void *malloc(size_t_ n) { void *p = HeapAlloc(GetProcessHeap(), 0, n ? n : 1); if (!p) g_errno = ENOMEM; return p; }
API void *calloc(size_t_ count, size_t_ size) {
  if (size && count > (size_t_)-1 / size) { g_errno = ENOMEM; return NULL; }
  void *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (count * size) != 0 ? count * size : 1);
  if (!p) g_errno = ENOMEM;
  return p;
}
API void *realloc(void *p, size_t_ n) {
  if (!p) return malloc(n);
  if (n == 0) { HeapFree(GetProcessHeap(), 0, p); return NULL; }
  void *q = HeapReAlloc(GetProcessHeap(), 0, p, n);
  if (!q) g_errno = ENOMEM;
  return q;
}
API void free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
API size_t_ _msize(void *p) { return HeapSize(GetProcessHeap(), 0, p); }
API void *_recalloc(void *p, size_t_ count, size_t_ size) {
  const size_t_ old = p ? HeapSize(GetProcessHeap(), 0, p) : 0, fresh = count * size;
  void *q = realloc(p, fresh);
  if (q && fresh > old) memset((char *)q + old, 0, fresh - old);
  return q;
}
API void *_aligned_malloc(size_t_ n, size_t_ alignment) {
  if (alignment & (alignment - 1)) { g_errno = EINVAL; return NULL; }
  if (alignment < 16) alignment = 16;
  char *raw = malloc(n + alignment + sizeof(void *));
  if (!raw) return NULL;
  char *aligned = (char *)(((ULONG_PTR)raw + sizeof(void *) + alignment - 1) & ~(ULONG_PTR)(alignment - 1));
  ((void **)aligned)[-1] = raw;
  return aligned;
}
API void _aligned_free(void *p) { if (p) free(((void **)p)[-1]); }
API void *_aligned_realloc(void *p, size_t_ n, size_t_ alignment) {
  void *q = _aligned_malloc(n, alignment);
  if (q && p) { size_t_ old = HeapSize(GetProcessHeap(), 0, ((void **)p)[-1]); memcpy(q, p, old < n ? old : n); _aligned_free(p); }
  return q;
}
API int _set_new_mode(int mode) { (void)mode; return 0; }
API int _set_new_handler(void *handler) { (void)handler; return 0; }
API int _callnewh(size_t_ n) { (void)n; return 0; }

/* ---- program arguments --------------------------------------------------------------------------------------------------------------------------------------- */

static int g_argc;
static char **g_argv, **g_environ;
static wchar_t **g_wargv;
static char *g_pgmptr;
static int g_fmode, g_commode;

/* Splits a command line the way the Microsoft runtime does: blanks separate; "..." groups; backslashes only count before a quote. Fills `argv` (if not NULL) with pointers
 * into `store`, returns the count and the bytes of store needed. */
static int split_command_line(const char *line, char **argv, char *store, size_t_ *needed) {
  int count = 0;
  size_t_ used = 0;
  const char *p = line;
  while (*p) {
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) break;
    if (argv) argv[count] = store + used;
    ++count;
    int quoted = 0;
    for (;;) {
      int backslashes = 0;
      while (*p == '\\') { ++backslashes; ++p; }
      if (*p == '"') {
        for (int i = 0; i < backslashes / 2; ++i) { if (argv) store[used] = '\\'; ++used; }
        if (backslashes & 1) { if (argv) store[used] = '"'; ++used; ++p; }
        else if (quoted && p[1] == '"') { if (argv) store[used] = '"'; ++used; p += 2; }      /* "" inside quotes is one quote */
        else { quoted = !quoted; ++p; }
        continue;
      }
      for (int i = 0; i < backslashes; ++i) { if (argv) store[used] = '\\'; ++used; }
      if (!*p || (!quoted && (*p == ' ' || *p == '\t'))) break;
      if (argv) store[used] = *p;
      ++used;
      ++p;
    }
    if (argv) store[used] = 0;
    ++used;
  }
  *needed = used;
  return count;
}

static void init_arguments(void) {
  if (g_argv) return;
  const char *line = GetCommandLineA();
  size_t_ needed = 0;
  const int count = split_command_line(line, NULL, NULL, &needed);
  char **argv = malloc((size_t_)(count + 1) * sizeof(char *));
  char *store = malloc(needed + 1);
  if (!argv || !store) ExitProcess(255);
  split_command_line(line, argv, store, &needed);
  argv[count] = NULL;
  g_argc = count;
  g_argv = argv;
  g_pgmptr = count ? argv[0] : "";
  /* the wide copy */
  wchar_t **wargv = malloc((size_t_)(count + 1) * sizeof(wchar_t *));
  for (int i = 0; i < count; ++i) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, NULL, 0);
    wargv[i] = malloc((size_t_)n * 2);
    MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, wargv[i], n);
  }
  wargv[count] = NULL;
  g_wargv = wargv;
}

/* The environment as an array of "NAME=value" strings; putenv changes the array and the process's block alike. */
static void init_environment(void) {
  if (g_environ) return;
  LPWSTR block = GetEnvironmentStringsW();
  int count = 0;
  for (LPWSTR e = block; *e; e += lstrlenW(e) + 1) ++count;
  char **env = malloc((size_t_)(count + 1) * sizeof(char *));
  int i = 0;
  for (LPWSTR e = block; *e; e += lstrlenW(e) + 1, ++i) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, e, -1, NULL, 0, NULL, NULL);
    env[i] = malloc((size_t_)n);
    WideCharToMultiByte(CP_UTF8, 0, e, -1, env[i], n, NULL, NULL);
  }
  env[count] = NULL;
  g_environ = env;
}

API int *__p___argc(void) { init_arguments(); return &g_argc; }
API char ***__p___argv(void) { init_arguments(); return &g_argv; }
API wchar_t ***__p___wargv(void) { init_arguments(); return &g_wargv; }
API char ***__p__environ(void) { init_environment(); return &g_environ; }
API char **__p__pgmptr(void) { init_arguments(); return &g_pgmptr; }

/* ====================================================================================================================================================================
 * Characters and strings beyond ntdll's
 * ==================================================================================================================================================================== */

API int isupper(int c) { return c >= 'A' && c <= 'Z'; }
API int islower(int c) { return c >= 'a' && c <= 'z'; }
API int isxdigit(int c) { return isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
API int isprint(int c) { return c >= 32 && c < 127; }
API int isgraph(int c) { return c > 32 && c < 127; }
API int ispunct(int c) { return isgraph(c) && !isalnum(c); }
API int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
API int isblank(int c) { return c == ' ' || c == '\t'; }
API int isascii(int c) { return (unsigned)c < 128; }
API int __isascii(int c) { return (unsigned)c < 128; }
API int __toascii(int c) { return c & 127; }
API int iswalpha(unsigned c) { return c < 128 ? isalpha((int)c) : c >= 0xC0; }
API int iswdigit(unsigned c) { return c >= '0' && c <= '9'; }
API int iswalnum(unsigned c) { return iswalpha(c) || iswdigit(c); }
API int iswspace(unsigned c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x2028; }
API int iswupper(unsigned c) { return c >= 'A' && c <= 'Z'; }
API int iswlower(unsigned c) { return c >= 'a' && c <= 'z'; }
API int iswprint(unsigned c) { return c >= 32 && c != 127; }
API int iswpunct(unsigned c) { return c < 128 && ispunct((int)c); }
API int iswxdigit(unsigned c) { return c < 128 && isxdigit((int)c); }
API int iswctype(unsigned c, unsigned short type) { (void)c; (void)type; return 0; }
API int _tolower(int c) { return c + 32; }
API int _toupper(int c) { return c - 32; }
API int _isctype(int c, int mask) {
  /* the mask bits of ctype.h: 1 upper, 2 lower, 4 digit, 8 space, 0x10 punct, 0x20 cntrl, 0x40 blank, 0x80 xdigit, 0x100 alpha */
  return ((mask & 1) && isupper(c)) || ((mask & 2) && islower(c)) || ((mask & 4) && isdigit(c)) || ((mask & 8) && isspace(c)) || ((mask & 0x10) && ispunct(c)) ||
         ((mask & 0x20) && iscntrl(c)) || ((mask & 0x40) && isblank(c)) || ((mask & 0x80) && isxdigit(c)) || ((mask & 0x100) && isalpha(c));
}

/* The table the ctype macros of the Microsoft headers index (pctype[c]), 257 entries starting at -1. */
static unsigned short g_ctype_table[257 + 128];
static unsigned short *g_pctype;
static const unsigned short *ctype_table(void) {
  if (!g_pctype) {
    for (int c = 0; c < 128; ++c) {
      unsigned short bits = 0;
      if (isupper(c)) bits |= 0x1 | 0x100;
      if (islower(c)) bits |= 0x2 | 0x100;
      if (isdigit(c)) bits |= 0x4;
      if (isspace(c)) bits |= 0x8;
      if (ispunct(c)) bits |= 0x10;
      if (iscntrl(c)) bits |= 0x20;
      if (c == ' ' || c == '\t') bits |= 0x40;
      if (isxdigit(c)) bits |= 0x80;
      g_ctype_table[1 + c] = bits;
    }
    g_pctype = g_ctype_table + 1;
  }
  return g_pctype;
}
API const unsigned short *__pctype_func(void) { return ctype_table(); }
API const unsigned short *_pctype_func(void) { return ctype_table(); }
API const unsigned short *__pwctype_func(void) { return ctype_table(); }
API int ___mb_cur_max_func(void) { return 4; }
API int __mb_cur_max_func(void) { return 4; }
API unsigned int ___lc_codepage_func(void) { return CP_UTF8; }
API int ___lc_locale_name_func(void) { return 0; }
API char *setlocale(int category, const char *locale) { (void)category; (void)locale; return (char *)"C"; }
API char *_setlocale(int category, const char *locale) { return setlocale(category, locale); }
API int _configthreadlocale(int mode) { (void)mode; return 0; }
API int _get_current_locale_dummy(void) { return 0; }
struct lconv_ { char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point, *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
  char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn; };
API struct lconv_ *localeconv(void) {
  static struct lconv_ c;
  static char dot[] = ".", empty[] = "";
  c.decimal_point = dot; c.thousands_sep = empty; c.grouping = empty; c.int_curr_symbol = empty; c.currency_symbol = empty; c.mon_decimal_point = empty;
  c.mon_thousands_sep = empty; c.mon_grouping = empty; c.positive_sign = empty; c.negative_sign = empty;
  return &c;
}

API size_t_ strnlen(const char *s, size_t_ n) { size_t_ i = 0; while (i < n && s[i]) ++i; return i; }
API size_t_ wcsnlen(const wchar_t *s, size_t_ n) { size_t_ i = 0; while (i < n && s[i]) ++i; return i; }
API char *strncat(char *d, const char *s, size_t_ n) { char *r = d; d += strlen(d); while (n-- && *s) *d++ = *s++; *d = 0; return r; }
API char *_strdup(const char *s) { size_t_ n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
API char *strdup(const char *s) { return _strdup(s); }
API wchar_t *_wcsdup(const wchar_t *s) { size_t_ n = (wcslen(s) + 1) * 2; wchar_t *p = malloc(n); if (p) memcpy(p, s, n); return p; }
API wchar_t *wcsdup(const wchar_t *s) { return _wcsdup(s); }
API size_t_ strspn(const char *s, const char *set) { size_t_ n = 0; while (s[n] && strchr(set, s[n])) ++n; return n; }
API size_t_ strcspn(const char *s, const char *set) { size_t_ n = 0; while (s[n] && !strchr(set, s[n])) ++n; return n; }
API char *strpbrk(const char *s, const char *set) { for (; *s; ++s) if (strchr(set, *s)) return (char *)s; return NULL; }
API char *strtok_s(char *s, const char *delim, char **context) {
  if (!s) s = *context;
  if (!s) return NULL;
  s += strspn(s, delim);
  if (!*s) { *context = NULL; return NULL; }
  char *end = s + strcspn(s, delim);
  if (*end) { *end = 0; *context = end + 1; } else { *context = NULL; }
  return s;
}
API char *strtok(char *s, const char *delim) { static char *context; return strtok_s(s, delim, &context); }
API char *_strlwr(char *s) { for (char *p = s; *p; ++p) *p = (char)tolower((unsigned char)*p); return s; }
API char *_strupr(char *s) { for (char *p = s; *p; ++p) *p = (char)toupper((unsigned char)*p); return s; }
API char *_strrev(char *s) { size_t_ n = strlen(s); for (size_t_ i = 0; i < n / 2; ++i) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } return s; }
API int strcoll(const char *a, const char *b) { return strcmp(a, b); }
API int _stricoll(const char *a, const char *b) { return _stricmp(a, b); }
API int _strcmpi(const char *a, const char *b) { return _stricmp(a, b); }
API int strcpy_s(char *d, size_t_ n, const char *s) { size_t_ l = strlen(s); if (l + 1 > n) { if (n) d[0] = 0; return ERANGE; } memcpy(d, s, l + 1); return 0; }
API int strcat_s(char *d, size_t_ n, const char *s) { size_t_ dl = strnlen(d, n), sl = strlen(s); if (dl + sl + 1 > n) { if (n) d[0] = 0; return ERANGE; } memcpy(d + dl, s, sl + 1); return 0; }
API int strncpy_s(char *d, size_t_ n, const char *s, size_t_ count) { size_t_ l = strnlen(s, count); if (l + 1 > n) { if (n) d[0] = 0; return ERANGE; } memcpy(d, s, l); d[l] = 0; return 0; }
API int memcpy_s(void *d, size_t_ dn, const void *s, size_t_ n) { if (n > dn) { memset(d, 0, dn); return ERANGE; } memcpy(d, s, n); return 0; }
API int memmove_s(void *d, size_t_ dn, const void *s, size_t_ n) { if (n > dn) return ERANGE; memmove(d, s, n); return 0; }
API void *memccpy(void *d, const void *s, int c, size_t_ n) { unsigned char *p = d; const unsigned char *q = s; while (n--) { *p++ = *q; if (*q++ == (unsigned char)c) return p; } return NULL; }
API void *_memccpy(void *d, const void *s, int c, size_t_ n) { return memccpy(d, s, c, n); }
API int _memicmp(const void *a, const void *b, size_t_ n) {
  const unsigned char *p = a, *q = b;
  for (; n; --n, ++p, ++q) { int x = tolower(*p), y = tolower(*q); if (x != y) return x < y ? -1 : 1; }
  return 0;
}
API wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t_ n) { size_t_ i = 0; for (; i < n && s[i]; ++i) d[i] = s[i]; for (; i < n; ++i) d[i] = 0; return d; }
API wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t_ n) { wchar_t *r = d; d += wcslen(d); while (n-- && *s) *d++ = *s++; *d = 0; return r; }
API wchar_t *wcsstr(const wchar_t *h, const wchar_t *n) { size_t_ l = wcslen(n); if (!l) return (wchar_t *)h; for (; *h; ++h) if (!wcsncmp(h, n, l)) return (wchar_t *)h; return NULL; }
API size_t_ wcsspn(const wchar_t *s, const wchar_t *set) { size_t_ n = 0; while (s[n] && wcschr(set, s[n])) ++n; return n; }
API size_t_ wcscspn(const wchar_t *s, const wchar_t *set) { size_t_ n = 0; while (s[n] && !wcschr(set, s[n])) ++n; return n; }
API wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t_ n) { memcpy(d, s, n * 2); return d; }
API wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t_ n) { memmove(d, s, n * 2); return d; }
API wchar_t *wmemset(wchar_t *d, wchar_t c, size_t_ n) { for (size_t_ i = 0; i < n; ++i) d[i] = c; return d; }
API int wmemcmp(const wchar_t *a, const wchar_t *b, size_t_ n) { return wcsncmp(a, b, n); }
API wchar_t *_wcslwr(wchar_t *s) { for (wchar_t *p = s; *p; ++p) *p = (wchar_t)towlower(*p); return s; }
API wchar_t *_wcsupr(wchar_t *s) { for (wchar_t *p = s; *p; ++p) *p = (wchar_t)towupper(*p); return s; }
API int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
API int wcscpy_s(wchar_t *d, size_t_ n, const wchar_t *s) { size_t_ l = wcslen(s); if (l + 1 > n) { if (n) d[0] = 0; return ERANGE; } memcpy(d, s, (l + 1) * 2); return 0; }
API int wcscat_s(wchar_t *d, size_t_ n, const wchar_t *s) { size_t_ dl = wcsnlen(d, n), sl = wcslen(s); if (dl + sl + 1 > n) { if (n) d[0] = 0; return ERANGE; } memcpy(d + dl, s, (sl + 1) * 2); return 0; }

/* ---- numbers from text ----------------------------------------------------------------------------------------------------------------------------------------- */

static unsigned long long parse_integer(const char *s, char **end, int base, int *negative, int *overflow, int *any) {
  const char *start = s;
  while (isspace((unsigned char)*s)) ++s;
  *negative = 0;
  if (*s == '-' || *s == '+') *negative = *s++ == '-';
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x' && isxdigit((unsigned char)s[2])) { s += 2; base = 16; }
  else if (base == 0) base = s[0] == '0' ? 8 : 10;
  unsigned long long value = 0;
  *overflow = 0;
  *any = 0;
  for (;; ++s) {
    int digit;
    if (*s >= '0' && *s <= '9') digit = *s - '0';
    else if ((*s | 32) >= 'a' && (*s | 32) <= 'z') digit = (*s | 32) - 'a' + 10;
    else break;
    if (digit >= base) break;
    *any = 1;
    if (value > (0xFFFFFFFFFFFFFFFFull - (unsigned)digit) / (unsigned)base) *overflow = 1;
    value = value * (unsigned)base + (unsigned)digit;
  }
  if (end) *end = (char *)(*any ? s : start);
  return value;
}
API long long strtoll(const char *s, char **end, int base) {
  int neg, over, any;
  unsigned long long v = parse_integer(s, end, base, &neg, &over, &any);
  if (over || (neg ? v > 0x8000000000000000ull : v > 0x7FFFFFFFFFFFFFFFull)) { g_errno = ERANGE; return neg ? (-0x7FFFFFFFFFFFFFFFll - 1) : 0x7FFFFFFFFFFFFFFFll; }
  return neg ? (long long)(0 - v) : (long long)v;
}
API unsigned long long strtoull(const char *s, char **end, int base) {
  int neg, over, any;
  unsigned long long v = parse_integer(s, end, base, &neg, &over, &any);
  if (over) { g_errno = ERANGE; return 0xFFFFFFFFFFFFFFFFull; }
  return neg ? 0 - v : v;
}
API long strtol(const char *s, char **end, int base) {      /* (long is 32 bits here) */
  long long v = strtoll(s, end, base);
  if (v > 0x7FFFFFFFll) { g_errno = ERANGE; return 0x7FFFFFFF; }
  if (v < -0x80000000ll) { g_errno = ERANGE; return (long)0x80000000; }
  return (long)v;
}
API unsigned long strtoul(const char *s, char **end, int base) {
  unsigned long long v = strtoull(s, end, base);
  if (v > 0xFFFFFFFFull && v != 0xFFFFFFFFFFFFFFFFull) { g_errno = ERANGE; return 0xFFFFFFFFul; }
  return (unsigned long)v;
}
API long long _strtoi64(const char *s, char **end, int base) { return strtoll(s, end, base); }
API long long atoll(const char *s) { return strtoll(s, NULL, 10); }
API long long _atoi64(const char *s) { return strtoll(s, NULL, 10); }
API long atol(const char *s) { return strtol(s, NULL, 10); }
API int abs(int x) { return x < 0 ? -x : x; }
API long labs(long x) { return x < 0 ? -x : x; }
API long long llabs(long long x) { return x < 0 ? -x : x; }
API long long _abs64(long long x) { return x < 0 ? -x : x; }
typedef struct { int quot, rem; } div_t_;
typedef struct { long long quot, rem; } lldiv_t_;
API div_t_ div(int a, int b) { div_t_ r = {a / b, a % b}; return r; }
API lldiv_t_ lldiv(long long a, long long b) { lldiv_t_ r = {a / b, a % b}; return r; }

static const char *const g_digit_chars = "0123456789abcdefghijklmnopqrstuvwxyz";
static char *u64_text(unsigned long long v, char *buffer, int radix) {
  char reversed[70];
  int n = 0;
  do { reversed[n++] = g_digit_chars[v % (unsigned)radix]; v /= (unsigned)radix; } while (v);
  int i = 0;
  while (n) buffer[i++] = reversed[--n];
  buffer[i] = 0;
  return buffer;
}
API char *_itoa(int v, char *b, int radix) { if (radix == 10 && v < 0) { b[0] = '-'; u64_text((unsigned long long)-(long long)v, b + 1, 10); return b; } return u64_text((unsigned)v, b, radix); }
API char *_ltoa(long v, char *b, int radix) { return _itoa((int)v, b, radix); }
API char *_ultoa(unsigned long v, char *b, int radix) { return u64_text(v, b, radix); }
API char *_i64toa(long long v, char *b, int radix) { if (radix == 10 && v < 0) { b[0] = '-'; u64_text((unsigned long long)-v, b + 1, 10); return b; } return u64_text((unsigned long long)v, b, radix); }
API char *_ui64toa(unsigned long long v, char *b, int radix) { return u64_text(v, b, radix); }
API int _itoa_s(int v, char *b, size_t_ n, int radix) { char t[40]; _itoa(v, t, radix); return strcpy_s(b, n, t); }

/* ====================================================================================================================================================================
 * Floating point: 80-bit long double arithmetic, and the x87 for what is not plain arithmetic
 * ==================================================================================================================================================================== */

static long double ld_rint(long double x) { __asm__("frndint" : "+t"(x)); return x; }
static long double ld_pow10(int e) {
  static const long double kTable[] = {1e1L, 1e2L, 1e4L, 1e8L, 1e16L, 1e32L, 1e64L, 1e128L, 1e256L, 1e512L, 1e1024L, 1e2048L, 1e4096L};
  long double r = 1.0L, base;
  const int negative = e < 0;
  if (negative) e = -e;
  for (int i = 0; e; ++i, e >>= 1) {
    if (e & 1) { base = kTable[i]; r *= base; }
  }
  return negative ? 1.0L / r : r;
}
static int ld_isnan(long double x) { return x != x; }
static int ld_isinf(long double x) { return x > 1.1e4932L || x < -1.1e4932L; }

/* The first `n` significant decimal digits of v > 0 (rounded to nearest), written as characters to out; *exp10 is the decimal exponent of the first digit. n <= 19. */
static void ld_digits(long double v, int n, char *out, int *exp10) {
  int e = 0;
  long double t = v;
  if (t >= 10.0L) { for (int p = 4096; p >= 1; p >>= 1) if (t >= ld_pow10(p)) { t /= ld_pow10(p); e += p; } }
  else if (t < 1.0L) { for (int p = 4096; p >= 1; p >>= 1) if (t * ld_pow10(p) < 10.0L && t < 1.0L) { t *= ld_pow10(p); e -= p; } if (t < 1.0L) { t *= 10.0L; --e; } }
  long double scaled = t * ld_pow10(n - 1);
  unsigned long long r = (unsigned long long)ld_rint(scaled);
  unsigned long long limit = 1;
  for (int i = 0; i < n; ++i) limit *= 10;
  if (r >= limit) { r /= 10; ++e; }
  for (int i = n - 1; i >= 0; --i) { out[i] = (char)('0' + r % 10); r /= 10; }
  *exp10 = e;
}

#define FMT_LEFT 1
#define FMT_PLUS 2
#define FMT_SPACE 4
#define FMT_ALT 8
#define FMT_ZERO 16
/* Formats a floating-point value as printf's e, f or g (`conv` lower case; `upper` for E, F, G); writes the text without padding. Returns its length. */
static int format_float(char *out, long double v, char conv, int precision, int flags, int upper) {
  char *o = out;
  int negative = 0;
  if (ld_isnan(v)) { const char *t = upper ? "NAN" : "nan"; for (int i = 0; t[i]; ++i) *o++ = t[i]; return (int)(o - out); }
  if (v < 0 || (v == 0 && 1.0L / v < 0)) { negative = 1; v = -v; }
  if (negative) *o++ = '-'; else if (flags & FMT_PLUS) *o++ = '+'; else if (flags & FMT_SPACE) *o++ = ' ';
  if (ld_isinf(v)) { const char *t = upper ? "INF" : "inf"; for (int i = 0; t[i]; ++i) *o++ = t[i]; return (int)(o - out); }
  if (precision < 0) precision = 6;
  char digits[40];
  int exponent = 0;
  if (conv == 'g') {
    if (precision == 0) precision = 1;
    if (v == 0) { exponent = 0; }
    else { int n = precision > 19 ? 19 : precision; ld_digits(v, n, digits, &exponent); }
    if (exponent < -4 || exponent >= precision) conv = 'e', precision -= 1;
    else conv = 'f', precision = precision - 1 - exponent;
    /* g drops trailing zeros (unless #): done below via the `trim` flag */
    flags |= 0x100;
  }
  const int trim = (flags & 0x100) && !(flags & FMT_ALT);
  if (conv == 'e') {
    int n = precision + 1;
    if (v == 0) { for (int i = 0; i < n && i < 40; ++i) digits[i] = '0'; exponent = 0; }
    else { int m = n > 19 ? 19 : n; ld_digits(v, m, digits, &exponent); for (int i = m; i < n && i < 40; ++i) digits[i] = '0'; }
    int last = n < 40 ? n : 40;
    if (trim) while (last > 1 && digits[last - 1] == '0') --last;
    *o++ = digits[0];
    if (last > 1 || (flags & FMT_ALT)) *o++ = '.';
    for (int i = 1; i < last; ++i) *o++ = digits[i];
    *o++ = upper ? 'E' : 'e';
    *o++ = exponent < 0 ? '-' : '+';
    int ex = exponent < 0 ? -exponent : exponent;
    if (ex >= 100) *o++ = (char)('0' + ex / 100);
    *o++ = (char)('0' + ex / 10 % 10);
    *o++ = (char)('0' + ex % 10);
    return (int)(o - out);
  }
  /* 'f' */
  char integer[48];
  int integer_length = 0;
  char fraction[64];
  int fraction_length = 0;
  if (v == 0) {
    integer[integer_length++] = '0';
    for (int i = 0; i < precision && fraction_length < 60; ++i) fraction[fraction_length++] = '0';
  } else {
    long double scaled = v * ld_pow10(precision);
    if (precision <= 30 && scaled < 1.8e19L) {
      unsigned long long r = (unsigned long long)ld_rint(scaled);
      char all[40];
      int n = 0;
      do { all[n++] = (char)('0' + r % 10); r /= 10; } while (r);
      while (n <= precision) all[n++] = '0';
      for (int i = n - 1; i >= precision; --i) integer[integer_length++] = all[i];
      for (int i = precision - 1; i >= 0 && fraction_length < 60; --i) fraction[fraction_length++] = all[i];
    } else {
      int m = 19;
      ld_digits(v, m, digits, &exponent);      /* a large value: 19 digits and zeros after them */
      for (int i = 0; i < m && integer_length < 47; ++i) integer[integer_length++] = digits[i];
      for (int i = m; i <= exponent && integer_length < 47; ++i) integer[integer_length++] = '0';
      for (int i = 0; i < precision && fraction_length < 60; ++i) fraction[fraction_length++] = '0';
    }
  }
  if (trim) while (fraction_length > 0 && fraction[fraction_length - 1] == '0') --fraction_length;
  for (int i = 0; i < integer_length; ++i) *o++ = integer[i];
  if (fraction_length > 0 || (flags & FMT_ALT)) *o++ = '.';
  for (int i = 0; i < fraction_length; ++i) *o++ = fraction[i];
  return (int)(o - out);
}

/* strtod: the longest prefix that is a number; mantissa digits are gathered exactly (up to 19) and scaled by a power of ten in long double. */
static long double parse_float(const char *s, char **end) {
  const char *start = s;
  while (isspace((unsigned char)*s)) ++s;
  int negative = 0;
  if (*s == '-' || *s == '+') negative = *s++ == '-';
  long double value = 0;
  if ((s[0] | 32) == 'i' && (s[1] | 32) == 'n' && (s[2] | 32) == 'f') { s += 3; if ((s[0] | 32) == 'i' && (s[1] | 32) == 'n' && (s[2] | 32) == 'i' && (s[3] | 32) == 't' && (s[4] | 32) == 'y') s += 5; if (end) *end = (char *)s; return negative ? -__builtin_infl() : __builtin_infl(); }
  if ((s[0] | 32) == 'n' && (s[1] | 32) == 'a' && (s[2] | 32) == 'n') { s += 3; if (end) *end = (char *)s; return negative ? -(0.0L / 0.0L) : (0.0L / 0.0L); }
  if (s[0] == '0' && (s[1] | 32) == 'x') {      /* hexadecimal float */
    const char* h = s + 2;
    int any = 0, exp2 = 0;
    for (; isxdigit((unsigned char)*h); ++h) { value = value * 16 + (*h <= '9' ? *h - '0' : (*h | 32) - 'a' + 10); any = 1; }
    if (*h == '.') { for (++h; isxdigit((unsigned char)*h); ++h) { value = value * 16 + (*h <= '9' ? *h - '0' : (*h | 32) - 'a' + 10); exp2 -= 4; any = 1; } }
    if (any && (*h | 32) == 'p') { const char *q = h + 1; int en = 0, ev = 0; if (*q == '-' || *q == '+') en = *q++ == '-'; if (isdigit((unsigned char)*q)) { while (isdigit((unsigned char)*q)) ev = ev * 10 + (*q++ - '0'); exp2 += en ? -ev : ev; h = q; } }
    if (!any) { if (end) *end = (char *)start; return 0; }
    while (exp2 > 0) { value *= 2; --exp2; }
    while (exp2 < 0) { value /= 2; ++exp2; }
    if (end) *end = (char *)h;
    return negative ? -value : value;
  }
  unsigned long long mantissa = 0;
  int digits = 0, exp10 = 0, any = 0;
  for (; isdigit((unsigned char)*s); ++s) {
    any = 1;
    if (digits < 19) { mantissa = mantissa * 10 + (unsigned)(*s - '0'); if (mantissa) ++digits; } else ++exp10;
  }
  if (*s == '.') {
    for (++s; isdigit((unsigned char)*s); ++s) {
      any = 1;
      if (digits < 19) { mantissa = mantissa * 10 + (unsigned)(*s - '0'); if (mantissa) ++digits; --exp10; }
    }
  }
  if (!any) { if (end) *end = (char *)start; return 0; }
  if ((*s | 32) == 'e') {
    const char *q = s + 1;
    int en = 0, ev = 0;
    if (*q == '-' || *q == '+') en = *q++ == '-';
    if (isdigit((unsigned char)*q)) {
      while (isdigit((unsigned char)*q)) { if (ev < 100000) ev = ev * 10 + (*q - '0'); ++q; }
      exp10 += en ? -ev : ev;
      s = q;
    }
  }
  value = (long double)mantissa;
  if (mantissa) {
    if (exp10 > 5000) value = __builtin_infl();
    else if (exp10 < -5000) value = 0;
    else if (exp10 > 0) value *= ld_pow10(exp10);
    else if (exp10 < 0) value /= ld_pow10(-exp10);
  }
  if (end) *end = (char *)s;
  return negative ? -value : value;
}
API double strtod(const char *s, char **end) { long double v = parse_float(s, end); if (v > 1.7976931348623157e308L) { g_errno = ERANGE; return 1.0 / 0.0; } if (v < -1.7976931348623157e308L) { g_errno = ERANGE; return -1.0 / 0.0; } return (double)v; }
API float strtof(const char *s, char **end) { return (float)parse_float(s, end); }
API long double strtold(const char *s, char **end) { return parse_float(s, end); }
API double atof(const char *s) { return strtod(s, NULL); }
API double _atof_l(const char *s, void *l) { (void)l; return strtod(s, NULL); }

/* ---- math ---------------------------------------------------------------------------------------------------------------------------------------------------------- */

API double fabs(double x) { return x < 0 ? -x : x; }
API float fabsf(float x) { return x < 0 ? -x : x; }
API double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
API float sqrtf(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
API double floor(double x) { if (ld_isnan(x) || x > 4.5e15 || x < -4.5e15) return x; double t = (double)(long long)x; return t > x ? t - 1 : t; }
API double ceil(double x) { if (ld_isnan(x) || x > 4.5e15 || x < -4.5e15) return x; double t = (double)(long long)x; return t < x ? t + 1 : t; }
API double trunc(double x) { if (ld_isnan(x) || x > 4.5e15 || x < -4.5e15) return x; return (double)(long long)x; }
API double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
API double rint(double x) { return (double)ld_rint(x); }
API double nearbyint(double x) { return (double)ld_rint(x); }
API long lround(double x) { return (long)round(x); }
API long long llround(double x) { return (long long)round(x); }
API long lrint(double x) { return (long)ld_rint(x); }
API long long llrint(double x) { return (long long)ld_rint(x); }
API float floorf(float x) { return (float)floor(x); }
API float ceilf(float x) { return (float)ceil(x); }
API double fmod(double a, double b) { long double r = a, d = b; if (b == 0 || ld_isnan(a) || ld_isnan(b)) return 0.0 / 0.0; __asm__("1: fprem\n fnstsw %%ax\n testb $4, %%ah\n jnz 1b" : "+t"(r) : "u"(d) : "ax", "cc"); return (double)r; }
API double ldexp(double x, int e) { long double r = x; if (e > 0) while (e--) r *= 2; else while (e++) r /= 2; return (double)r; }
API double frexp(double x, int *e) {
  *e = 0;
  if (x == 0 || ld_isnan(x) || ld_isinf(x)) return x;
  long double m = x < 0 ? -(long double)x : x;
  int ex = 0;
  while (m >= 1.0L) { m /= 2; ++ex; }
  while (m < 0.5L) { m *= 2; --ex; }
  *e = ex;
  return x < 0 ? -(double)m : (double)m;
}
API double modf(double x, double *ip) { double t = trunc(x); *ip = t; return x - t; }
API double exp(double x) {
  long double r = x;
  __asm__("fldl2e\n fmulp\n fld %%st(0)\n frndint\n fsub %%st(0), %%st(1)\n fxch\n f2xm1\n fld1\n faddp\n fscale\n fstp %%st(1)" : "+t"(r));
  return (double)r;
}
API double log(double x) { if (x < 0) return 0.0 / 0.0; if (x == 0) return -1.0 / 0.0; long double r = x; __asm__("fldln2\n fxch\n fyl2x" : "+t"(r)); return (double)r; }
API double log10(double x) { if (x < 0) return 0.0 / 0.0; if (x == 0) return -1.0 / 0.0; long double r = x; __asm__("fldlg2\n fxch\n fyl2x" : "+t"(r)); return (double)r; }
API double log2(double x) { if (x < 0) return 0.0 / 0.0; if (x == 0) return -1.0 / 0.0; long double r = x; __asm__("fld1\n fxch\n fyl2x" : "+t"(r)); return (double)r; }
API double pow(double x, double y) {
  if (y == 0) return 1.0;
  if (ld_isnan(x) || ld_isnan(y)) return 0.0 / 0.0;
  if (x == 0) return y > 0 ? 0.0 : 1.0 / 0.0;
  const int y_is_integer = y == floor(y) && fabs(y) < 9e15;
  if (x < 0 && !y_is_integer) return 0.0 / 0.0;
  long double base = x < 0 ? -(long double)x : x, r = y;
  __asm__("fyl2x\n fld %%st(0)\n frndint\n fsub %%st(0), %%st(1)\n fxch\n f2xm1\n fld1\n faddp\n fscale\n fstp %%st(1)" : "+t"(r) : "u"(base) : "st(1)");
  if (x < 0 && y_is_integer && ((long long)y & 1)) r = -r;
  return (double)r;
}
API double sin(double x) { long double r = x; __asm__("fsin" : "+t"(r)); return (double)r; }
API double cos(double x) { long double r = x; __asm__("fcos" : "+t"(r)); return (double)r; }
API double tan(double x) { long double r = x, one; __asm__("fptan\n fstp %0" : "+t"(r), "=u"(one)); return (double)r; }
API double atan(double x) { long double r = x; __asm__("fld1\n fpatan" : "+t"(r) : : "st(1)"); return (double)r; }
API double atan2(double y, double x) { long double a = y, b = x; __asm__("fpatan" : "+t"(b) : "u"(a) : "st(1)"); return (double)b; }
API double asin(double x) { return atan2(x, sqrt((1 - x) * (1 + x))); }
API double acos(double x) { return atan2(sqrt((1 - x) * (1 + x)), x); }
API double sinh(double x) { return (exp(x) - exp(-x)) / 2; }
API double cosh(double x) { return (exp(x) + exp(-x)) / 2; }
API double tanh(double x) { double a = exp(x), b = exp(-x); return (a - b) / (a + b); }
API double cbrt(double x) { return x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3); }
API double hypot(double a, double b) { return sqrt(a * a + b * b); }
API double copysign(double x, double y) { return (y < 0 || (y == 0 && 1 / y < 0)) ? -fabs(x) : fabs(x); }
API double fmax(double a, double b) { return a > b ? a : b; }
API double fmin(double a, double b) { return a < b ? a : b; }
API int _isnan(double x) { return x != x; }
API int _finite(double x) { return !(x != x) && !ld_isinf(x); }
API int _fpclass(double x) { return x != x ? 2 : ld_isinf(x) ? (x < 0 ? 4 : 0x200) : 0x80; }
API int __fpclassify(double x) { return x != x ? 0x0100 : ld_isinf(x) ? 0x0500 : x == 0 ? 0x4000 : 0x0400; }
API int __setusermatherr(void *handler) { (void)handler; return 0; }
API unsigned _controlfp(unsigned a, unsigned b) { (void)a; (void)b; return 0x9001F; }
API int _controlfp_s(unsigned *current, unsigned a, unsigned b) { (void)a; (void)b; if (current) *current = 0x9001F; return 0; }
API unsigned _clearfp(void) { return 0; }
API void _fpreset(void) {}
API void _statusfp_dummy(void) {}

/* ====================================================================================================================================================================
 * printf and scanf
 * ==================================================================================================================================================================== */

#define CONCAT_(a, b) a##b
#define CONCAT(a, b) CONCAT_(a, b)

#define CH char
#define CH_TAG _a
#define FORMAT_NAME format_a
#include "format.inc"
#undef CH
#undef CH_TAG
#undef FORMAT_NAME

#define CH wchar_t
#define CH_TAG _w
#define FORMAT_NAME format_w
#include "format.inc"
#undef CH
#undef CH_TAG
#undef FORMAT_NAME

/* Sinks: a buffer that counts what does not fit. */
typedef struct { char *buffer; size_t_ capacity, length; } ByteBuffer_;
static void put_to_bytes(void *context, char c) { ByteBuffer_ *b = context; if (b->length + 1 < b->capacity) b->buffer[b->length] = c; ++b->length; }
typedef struct { wchar_t *buffer; size_t_ capacity, length; } WideBuffer_;
static void put_to_wide(void *context, wchar_t c) { WideBuffer_ *b = context; if (b->length + 1 < b->capacity) b->buffer[b->length] = c; ++b->length; }

static int format_to_buffer(char *buffer, size_t_ capacity, const char *format, va_list_ args) {
  ByteBuffer_ b = {buffer, capacity, 0};
  const int n = format_a(put_to_bytes, &b, format, args);
  if (capacity) buffer[b.length < capacity ? b.length : capacity - 1] = 0;
  return n;
}
static int format_to_wide_buffer(wchar_t *buffer, size_t_ capacity, const wchar_t *format, va_list_ args) {
  WideBuffer_ b = {buffer, capacity, 0};
  const int n = format_w(put_to_wide, &b, format, args);
  if (capacity) buffer[b.length < capacity ? b.length : capacity - 1] = 0;
  return n;
}

API int vsnprintf(char *b, size_t_ n, const char *f, va_list_ a) { return format_to_buffer(b, n, f, a); }
API int _vsnprintf(char *b, size_t_ n, const char *f, va_list_ a) {      /* the old behaviour: -1 if it does not fit */
  const int r = format_to_buffer(b, n, f, a);
  return (r < 0 || (size_t_)r >= n) ? -1 : r;
}
API int vsprintf(char *b, const char *f, va_list_ a) { return format_to_buffer(b, (size_t_)-1 >> 2, f, a); }
API int _vscprintf(const char *f, va_list_ a) { return format_to_buffer(NULL, 0, f, a); }
API int sprintf(char *b, const char *f, ...) { va_list_ a; va_start_(a, f); int r = vsprintf(b, f, a); va_end_(a); return r; }
API int snprintf(char *b, size_t_ n, const char *f, ...) { va_list_ a; va_start_(a, f); int r = vsnprintf(b, n, f, a); va_end_(a); return r; }
API int _snprintf(char *b, size_t_ n, const char *f, ...) { va_list_ a; va_start_(a, f); int r = _vsnprintf(b, n, f, a); va_end_(a); return r; }
API int _scprintf(const char *f, ...) { va_list_ a; va_start_(a, f); int r = _vscprintf(f, a); va_end_(a); return r; }
API int sprintf_s(char *b, size_t_ n, const char *f, ...) { va_list_ a; va_start_(a, f); int r = format_to_buffer(b, n, f, a); va_end_(a); return (r < 0 || (size_t_)r >= n) ? -1 : r; }
API int vswprintf(wchar_t *b, size_t_ n, const wchar_t *f, va_list_ a) { return format_to_wide_buffer(b, n, f, a); }
API int _vsnwprintf(wchar_t *b, size_t_ n, const wchar_t *f, va_list_ a) { const int r = format_to_wide_buffer(b, n, f, a); return (r < 0 || (size_t_)r >= n) ? -1 : r; }
API int swprintf(wchar_t *b, size_t_ n, const wchar_t *f, ...) { va_list_ a; va_start_(a, f); int r = vswprintf(b, n, f, a); va_end_(a); return r; }
API int _snwprintf(wchar_t *b, size_t_ n, const wchar_t *f, ...) { va_list_ a; va_start_(a, f); int r = _vsnwprintf(b, n, f, a); va_end_(a); return r; }
API int swprintf_s(wchar_t *b, size_t_ n, const wchar_t *f, ...) { va_list_ a; va_start_(a, f); int r = _vsnwprintf(b, n, f, a); va_end_(a); return r; }
API int _vscwprintf(const wchar_t *f, va_list_ a) { return format_to_wide_buffer(NULL, 0, f, a); }

/* scanf: reads through a pair of functions. Returns the number of conversions made, or EOF if the input ended before the first one. */
typedef struct { int (*get)(void *); void (*unget)(void *, int); void *context; } Reader;
static int scan_digit(int c, int base) {
  int d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'z' ? (c | 32) - 'a' + 10 : 99;
  return d < base ? d : -1;
}
static int scan_format(Reader *r, const char *f, va_list_ args) {
  int converted = 0, characters = 0, c = 0;
#define GET() (c = r->get(r->context), (void)(c != EOF ? ++characters : 0), c)
#define UNGET() do { if (c != EOF) { r->unget(r->context, c); --characters; } } while (0)
  for (; *f; ++f) {
    if (isspace((unsigned char)*f)) { do GET(); while (c != EOF && isspace(c)); UNGET(); continue; }
    if (*f != '%') { GET(); if (c != *f) { UNGET(); return converted ? converted : (c == EOF ? EOF : 0); } continue; }
    ++f;
    int suppress = 0, width = 0, size = 0;
    if (*f == '*') { suppress = 1; ++f; }
    while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    for (;; ++f) { if (*f == 'h') size = size == 1 ? 2 : 1; else if (*f == 'l') size = size == 4 ? 8 : 4; else if (*f == 'L') size = 16; else if (*f == 'z' || *f == 'j' || *f == 't') size = 8; else if (*f == 'I' && f[1] == '6' && f[2] == '4') { size = 8; f += 2; } else break; }
    const char conv = *f;
    if (!conv) break;
    if (conv == '%') { GET(); if (c != '%') { UNGET(); break; } continue; }
    if (conv == 'n') { if (!suppress) { int *p = va_arg_(args, int *); *p = characters; } continue; }
    if (conv != 'c' && conv != '[') { do GET(); while (c != EOF && isspace(c)); if (c == EOF) return converted ? converted : EOF; UNGET(); }
    if (width == 0) width = conv == 'c' ? 1 : 0x7FFFFFFF;
    if (conv == 'c' || conv == 's' || conv == '[') {
      char set[256] = {0};
      int negate = 0;
      if (conv == '[') {
        ++f;
        if (*f == '^') { negate = 1; ++f; }
        const char *start = f;
        for (; *f && (*f != ']' || f == start); ++f) {
          if (f[1] == '-' && f[2] && f[2] != ']') { for (int k = (unsigned char)f[0]; k <= (unsigned char)f[2]; ++k) set[k] = 1; f += 2; }
          else set[(unsigned char)*f] = 1;
        }
      }
      char *out = suppress ? NULL : va_arg_(args, char *);
      int n = 0;
      for (; n < width; ++n) {
        GET();
        if (c == EOF) break;
        if (conv == 's' && isspace(c)) { UNGET(); break; }
        if (conv == '[' && (set[(unsigned char)c] == 0) != negate) { UNGET(); break; }
        if (out) out[n] = (char)c;
      }
      if (n == 0 && conv != 'c') return converted ? converted : (c == EOF ? EOF : 0);
      if (n < width && conv == 'c' && c == EOF && n == 0) return converted ? converted : EOF;
      if (out && conv != 'c') out[n] = 0;
      if (!suppress) ++converted;
      continue;
    }
    if (conv == 'e' || conv == 'f' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a') {
      char text[128];
      int n = 0;
      while (n < 127 && n < width) {
        GET();
        if (c == EOF) break;
        if (isdigit(c) || c == '.' || c == '+' || c == '-' || (c | 32) == 'e' || (c | 32) == 'x' || (c | 32) == 'n' || (c | 32) == 'i' || (c | 32) == 'f' || (c | 32) == 'a' || (c | 32) == 't' || (c | 32) == 'y' || (c | 32) == 'p' || isxdigit(c)) text[n++] = (char)c;
        else { UNGET(); break; }
      }
      text[n] = 0;
      char *end;
      const long double v = parse_float(text, &end);
      if (end == text) return converted ? converted : (c == EOF ? EOF : 0);
      for (int k = n; k > (int)(end - text); --k) { r->unget(r->context, (unsigned char)text[k - 1]); --characters; }
      if (!suppress) { if (size == 16) *va_arg_(args, long double *) = v; else if (size == 4 || size == 8) *va_arg_(args, double *) = (double)v; else *va_arg_(args, float *) = (float)v; ++converted; }
      continue;
    }
    /* integers and pointers */
    int base = conv == 'x' || conv == 'X' || conv == 'p' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
    int negative = 0, digits = 0, consumed = 0;
    unsigned long long value = 0;
    GET();
    if ((c == '-' || c == '+') && consumed < width) { negative = c == '-'; ++consumed; GET(); }
    if ((base == 0 || base == 16) && c == '0' && consumed < width) {
      ++consumed;
      digits = 1;
      GET();
      if ((c | 32) == 'x' && consumed < width) { base = 16; digits = 0; ++consumed; GET(); }
      else if (base == 0) base = 8;
    } else if (base == 0) {
      base = 10;
    }
    while (c != EOF && scan_digit(c, base) >= 0 && consumed < width) { value = value * (unsigned)base + (unsigned)scan_digit(c, base); ++digits; ++consumed; GET(); }
    UNGET();
    if (!digits) return converted ? converted : (c == EOF ? EOF : 0);
    if (negative) value = 0 - value;
    if (!suppress) {
      if (conv == 'p') *va_arg_(args, void **) = (void *)(ULONG_PTR)value;
      else if (size == 8 || size == 16) *va_arg_(args, long long *) = (long long)value;
      else if (size == 1) *va_arg_(args, short *) = (short)value;
      else if (size == 2) *va_arg_(args, char *) = (char)value;
      else *va_arg_(args, int *) = (int)value;
      ++converted;
    }
  }
  return converted;
#undef GET
#undef UNGET
}

/* ====================================================================================================================================================================
 * File descriptors (io.h) and streams (stdio.h)
 * ==================================================================================================================================================================== */

#define O_RDONLY_ 0x0000
#define O_WRONLY_ 0x0001
#define O_RDWR_ 0x0002
#define O_APPEND_ 0x0008
#define O_CREAT_ 0x0100
#define O_TRUNC_ 0x0200
#define O_EXCL_ 0x0400
#define O_TEXT_ 0x4000
#define O_BINARY_ 0x8000
#define O_NOINHERIT_ 0x0080
#define O_TEMPORARY_ 0x0040

#define MAX_FDS 512
#define FD_OPEN 1
#define FD_TEXT 2
#define FD_APPEND 4
#define FD_DEVICE 8
typedef struct { HANDLE handle; unsigned char flags; } FdEntry;
static FdEntry g_fds[MAX_FDS];
static int g_io_ready;

static void init_io(void) {
  if (g_io_ready) return;
  g_io_ready = 1;
  const DWORD which[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (int i = 0; i < 3; ++i) {
    HANDLE h = GetStdHandle(which[i]);
    if (h && h != INVALID_HANDLE_VALUE) g_fds[i].handle = h, g_fds[i].flags = FD_OPEN | FD_TEXT | (GetFileType(h) == FILE_TYPE_CHAR ? FD_DEVICE : 0);
  }
}
static FdEntry *fd_entry(int fd) {
  init_io();
  if (fd < 0 || fd >= MAX_FDS || !(g_fds[fd].flags & FD_OPEN)) { g_errno = EBADF; return NULL; }
  return &g_fds[fd];
}
static int fd_new(HANDLE h, unsigned char flags) {
  init_io();
  for (int i = 3; i < MAX_FDS; ++i) if (!(g_fds[i].flags & FD_OPEN)) { g_fds[i].handle = h; g_fds[i].flags = (unsigned char)(flags | FD_OPEN); return i; }
  g_errno = EMFILE;
  return -1;
}

/* Raw transfers: bytes as they are. */
static int raw_read(FdEntry *e, void *buffer, unsigned count) {
  DWORD got = 0;
  if (!ReadFile(e->handle, buffer, count, &got, NULL)) { map_last_error(); return -1; }
  return (int)got;
}
static int raw_write(FdEntry *e, const void *buffer, unsigned count) {
  unsigned done = 0;
  while (done < count) {
    DWORD put = 0;
    if (!WriteFile(e->handle, (const char *)buffer + done, count - done, &put, NULL) || put == 0) { map_last_error(); return done ? (int)done : -1; }
    done += put;
  }
  return (int)done;
}
static long long raw_seek(FdEntry *e, long long offset, int origin) {
  LARGE_INTEGER d, r;
  d.QuadPart = offset;
  if (!SetFilePointerEx(e->handle, d, &r, (DWORD)origin)) { map_last_error(); return -1; }
  return r.QuadPart;
}

API int _read(int fd, void *buffer, unsigned count) {
  FdEntry *e = fd_entry(fd);
  if (!e) return -1;
  if (!(e->flags & FD_TEXT) || (e->flags & FD_DEVICE)) return raw_read(e, buffer, count);
  /* text mode: CR LF becomes LF, Ctrl+Z ends the file */
  unsigned char *out = buffer;
  unsigned n = 0;
  while (n < count) {
    unsigned char c;
    const int got = raw_read(e, &c, 1);
    if (got < 0) return -1;
    if (got == 0 || c == 0x1A) break;
    if (c == '\r') {
      unsigned char next;
      const int g2 = raw_read(e, &next, 1);
      if (g2 == 1 && next != '\n') raw_seek(e, -1, FILE_CURRENT);
      if (g2 == 1 && next == '\n') c = '\n';
    }
    out[n++] = c;
  }
  return (int)n;
}
API int _write(int fd, const void *buffer, unsigned count) {
  FdEntry *e = fd_entry(fd);
  if (!e) return -1;
  if (e->flags & FD_APPEND) raw_seek(e, 0, FILE_END);
  if (!(e->flags & FD_TEXT) || (e->flags & FD_DEVICE)) return raw_write(e, buffer, count);
  const unsigned char *p = buffer;
  unsigned start = 0;
  for (unsigned i = 0; i <= count; ++i) {
    if (i == count || p[i] == '\n') {
      if (i > start && raw_write(e, p + start, i - start) < 0) return -1;
      if (i < count && raw_write(e, "\r\n", 2) < 0) return -1;
      start = i + 1;
    }
  }
  return (int)count;
}
API long long _lseeki64(int fd, long long offset, int origin) { FdEntry *e = fd_entry(fd); return e ? raw_seek(e, offset, origin) : -1; }
API long _lseek(int fd, long offset, int origin) { return (long)_lseeki64(fd, offset, origin); }
API long long _telli64(int fd) { return _lseeki64(fd, 0, 1); }
API long _tell(int fd) { return (long)_lseeki64(fd, 0, 1); }
API int _close(int fd) {
  FdEntry *e = fd_entry(fd);
  if (!e) return -1;
  const int ok = fd < 3 ? 1 : CloseHandle(e->handle);
  e->flags = 0;
  e->handle = NULL;
  if (!ok) { map_last_error(); return -1; }
  return 0;
}
API int _commit(int fd) { FdEntry *e = fd_entry(fd); return e && FlushFileBuffers(e->handle) ? 0 : -1; }
API int _isatty(int fd) { FdEntry *e = fd_entry(fd); return e && (e->flags & FD_DEVICE) ? 1 : 0; }
API int _eof(int fd) {
  FdEntry *e = fd_entry(fd);
  if (!e) return -1;
  LARGE_INTEGER size;
  const long long here = raw_seek(e, 0, FILE_CURRENT);
  return GetFileSizeEx(e->handle, &size) && here >= size.QuadPart;
}
API int _setmode(int fd, int mode) {
  FdEntry *e = fd_entry(fd);
  if (!e) return -1;
  const int old = (e->flags & FD_TEXT) ? O_TEXT_ : O_BINARY_;
  if (mode == O_TEXT_) e->flags |= FD_TEXT; else if (mode == O_BINARY_) e->flags &= (unsigned char)~FD_TEXT;
  return old;
}
API intptr_t _get_osfhandle(int fd) { FdEntry *e = fd_entry(fd); return e ? (intptr_t)e->handle : -1; }
API int _open_osfhandle(intptr_t h, int flags) { return fd_new((HANDLE)h, (unsigned char)(((flags & O_TEXT_) || !(flags & O_BINARY_) ? FD_TEXT : 0) | ((flags & O_APPEND_) ? FD_APPEND : 0) | (GetFileType((HANDLE)h) == FILE_TYPE_CHAR ? FD_DEVICE : 0))); }
API int _dup(int fd) {
  FdEntry *e = fd_entry(fd);
  if (!e) return -1;
  HANDLE copy;
  if (!DuplicateHandle(GetCurrentProcess(), e->handle, GetCurrentProcess(), &copy, 0, FALSE, 2)) { map_last_error(); return -1; }
  return fd_new(copy, e->flags);
}
API int _dup2(int fd, int to) {
  FdEntry *e = fd_entry(fd);
  if (!e || to < 0 || to >= MAX_FDS) return -1;
  if (fd == to) return 0;
  init_io();
  if (g_fds[to].flags & FD_OPEN) _close(to);
  g_fds[to].handle = e->handle;
  g_fds[to].flags = e->flags;
  return 0;
}

static int open_common(const char *path, int flags, int share) {
  (void)share;
  DWORD access = (flags & O_RDWR_) ? (GENERIC_READ | GENERIC_WRITE) : (flags & O_WRONLY_) ? GENERIC_WRITE : GENERIC_READ;
  DWORD creation;
  if ((flags & O_CREAT_) && (flags & O_EXCL_)) creation = CREATE_NEW;
  else if ((flags & O_CREAT_) && (flags & O_TRUNC_)) creation = CREATE_ALWAYS;
  else if (flags & O_CREAT_) creation = OPEN_ALWAYS;
  else if (flags & O_TRUNC_) creation = TRUNCATE_EXISTING;
  else creation = OPEN_EXISTING;
  HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, creation, (flags & O_TEMPORARY_) ? FILE_FLAG_DELETE_ON_CLOSE : FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) { map_last_error(); return -1; }
  const int text = (flags & O_TEXT_) || (!(flags & O_BINARY_) && g_fmode != O_BINARY_);
  const int fd = fd_new(h, (unsigned char)((text ? FD_TEXT : 0) | ((flags & O_APPEND_) ? FD_APPEND : 0)));
  if (fd < 0) CloseHandle(h);
  return fd;
}
API int _open(const char *path, int flags, ...) { return open_common(path, flags, 0); }
API int _sopen(const char *path, int flags, int share, ...) { return open_common(path, flags, share); }
API int _sopen_s(int *fd, const char *path, int flags, int share, int mode) { (void)mode; *fd = open_common(path, flags, share); return *fd < 0 ? g_errno : 0; }
API int _creat(const char *path, int mode) { (void)mode; return open_common(path, O_CREAT_ | O_TRUNC_ | O_WRONLY_, 0); }
API int open(const char *path, int flags, ...) { return open_common(path, flags, 0); }
API int close(int fd) { return _close(fd); }
API int read(int fd, void *b, unsigned n) { return _read(fd, b, n); }
API int write(int fd, const void *b, unsigned n) { return _write(fd, b, n); }
API long lseek(int fd, long o, int w) { return _lseek(fd, o, w); }
API int _chsize(int fd, long size) { FdEntry *e = fd_entry(fd); if (!e) return -1; long long here = raw_seek(e, 0, FILE_CURRENT); raw_seek(e, size, FILE_BEGIN); int ok = SetEndOfFile(e->handle); raw_seek(e, here, FILE_BEGIN); return ok ? 0 : -1; }
API int _chsize_s(int fd, long long size) { FdEntry *e = fd_entry(fd); if (!e) return EBADF; long long here = raw_seek(e, 0, FILE_CURRENT); raw_seek(e, size, FILE_BEGIN); int ok = SetEndOfFile(e->handle); raw_seek(e, here, FILE_BEGIN); return ok ? 0 : EIO; }
API int _unlink(const char *path) { if (DeleteFileA(path)) return 0; map_last_error(); return -1; }
API int unlink(const char *path) { return _unlink(path); }
API int remove(const char *path) {
  if (DeleteFileA(path) || RemoveDirectoryA(path)) return 0;
  map_last_error();
  return -1;
}
API int rename(const char *from, const char *to) { if (MoveFileA(from, to)) return 0; map_last_error(); return -1; }
API int _access(const char *path, int mode) {
  const DWORD a = GetFileAttributesA(path);
  if (a == INVALID_FILE_ATTRIBUTES) { map_last_error(); return -1; }
  if ((mode & 2) && (a & FILE_ATTRIBUTE_READONLY)) { g_errno = EACCES; return -1; }
  return 0;
}
API int access(const char *path, int mode) { return _access(path, mode); }
API int _mkdir(const char *path) { if (CreateDirectoryA(path, NULL)) return 0; map_last_error(); return -1; }
API int mkdir(const char *path) { return _mkdir(path); }
API int _rmdir(const char *path) { if (RemoveDirectoryA(path)) return 0; map_last_error(); return -1; }
API int rmdir(const char *path) { return _rmdir(path); }
API int _chdir(const char *path) { if (SetCurrentDirectoryA(path)) return 0; map_last_error(); return -1; }
API int chdir(const char *path) { return _chdir(path); }
API char *_getcwd(char *buffer, int size) {
  char tmp[MAX_PATH * 2];
  const DWORD n = GetCurrentDirectoryA(sizeof(tmp), tmp);
  if (n == 0 || n >= sizeof(tmp)) { map_last_error(); return NULL; }
  if (!buffer) { buffer = malloc(n + 1 > (DWORD)size ? n + 1 : (DWORD)size); if (!buffer) return NULL; }
  else if ((int)n + 1 > size) { g_errno = ERANGE; return NULL; }
  memcpy(buffer, tmp, n + 1);
  return buffer;
}
API char *getcwd(char *b, int n) { return _getcwd(b, n); }
API int _getdrive(void) { return 3; }
API int _chmod(const char *path, int mode) { (void)mode; return GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES ? -1 : 0; }
API int _umask(int mask) { (void)mask; return 0; }
API int _fileno(void *f);

/* stat: the two layouts programs use (st_size 32 bits, or 64) */
static void fill_stat(unsigned char *out, int wide_size, DWORD attributes, unsigned long long size, const FILETIME *created, const FILETIME *accessed, const FILETIME *written) {
  memset(out, 0, wide_size ? 56 : 48);
  const unsigned short mode = (unsigned short)((attributes & FILE_ATTRIBUTE_DIRECTORY ? 0x4000 | 0x49 : 0x8000) | 0x100 | ((attributes & FILE_ATTRIBUTE_READONLY) ? 0 : 0x80));      /* S_IFDIR/S_IFREG, r, w, x for directories */
  memcpy(out + 6, &mode, 2);
  const short links = 1;
  memcpy(out + 8, &links, 2);
  if (wide_size) memcpy(out + 24, &size, 8); else { unsigned s = (unsigned)size; memcpy(out + 20, &s, 4); }
  const FILETIME *t[3] = {accessed, written, created};
  for (int i = 0; i < 3; ++i) {
    unsigned long long ft = ((unsigned long long)t[i]->dwHighDateTime << 32) | t[i]->dwLowDateTime;
    long long unix_time = ft >= 116444736000000000ull ? (long long)((ft - 116444736000000000ull) / 10000000ull) : 0;
    memcpy(out + (wide_size ? 32 : 24) + 8 * i, &unix_time, 8);
  }
}
static int stat_path(const char *path, unsigned char *out, int wide_size) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  WCHAR *w = NULL;
  const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (n <= 0 || !(w = malloc((size_t_)n * 2))) return -1;
  MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
  const BOOL ok = GetFileAttributesExW(w, 0, &d);
  free(w);
  if (!ok) { map_last_error(); return -1; }
  fill_stat(out, wide_size, d.dwFileAttributes, ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow, &d.ftCreationTime, &d.ftLastAccessTime, &d.ftLastWriteTime);
  return 0;
}
static int stat_fd(int fd, unsigned char *out, int wide_size) {
  FdEntry *e = fd_entry(fd);
  if (!e) return -1;
  BY_HANDLE_FILE_INFORMATION i;
  if (!GetFileInformationByHandle(e->handle, &i)) {
    memset(out, 0, wide_size ? 56 : 48);
    const unsigned short mode = 0x2000 | 0x180;      /* S_IFCHR */
    memcpy(out + 6, &mode, 2);
    return 0;
  }
  fill_stat(out, wide_size, i.dwFileAttributes, ((unsigned long long)i.nFileSizeHigh << 32) | i.nFileSizeLow, &i.ftCreationTime, &i.ftLastAccessTime, &i.ftLastWriteTime);
  return 0;
}
API int _stat64i32(const char *path, void *buffer) { return stat_path(path, buffer, 0); }
API int _stat64(const char *path, void *buffer) { return stat_path(path, buffer, 1); }
API int _stat(const char *path, void *buffer) { return stat_path(path, buffer, 0); }
API int stat(const char *path, void *buffer) { return stat_path(path, buffer, 0); }
API int _fstat64i32(int fd, void *buffer) { return stat_fd(fd, buffer, 0); }
API int _fstat64(int fd, void *buffer) { return stat_fd(fd, buffer, 1); }
API int _fstat(int fd, void *buffer) { return stat_fd(fd, buffer, 0); }
API int fstat(int fd, void *buffer) { return stat_fd(fd, buffer, 0); }

/* ---- streams ------------------------------------------------------------------------------------------------------------------------------------------------------ */

#define S_READ 0x1
#define S_WRITE 0x2
#define S_EOF 0x4
#define S_ERR 0x8
#define S_UNBUF 0x40
#define S_LINEBUF 0x80
#define S_BUF_OWNED 0x100
#define S_STATIC 0x200
#define S_APPEND_ 0x400
#define S_TEXT_MODE 0x800
#define DEFAULT_BUFFER 4096
typedef struct Stream {
  int fd, flags;
  unsigned char *buffer;
  int buffer_size, read_pos, read_end, write_pos, ungot, pad;
  struct Stream *next;
} Stream;
_Static_assert(sizeof(Stream) == 48, "the streams of __iob_func() are 48 bytes apart in the Microsoft headers");

static Stream g_iob[3] = {{0, S_READ | S_STATIC, 0, 0, 0, 0, 0, -1, 0, 0}, {1, S_WRITE | S_STATIC, 0, 0, 0, 0, 0, -1, 0, 0}, {2, S_WRITE | S_STATIC | S_UNBUF, 0, 0, 0, 0, 0, -1, 0, 0}};
static Stream *g_streams;      /* the streams opened with fopen */
static int g_iob_ready;

static void init_streams(void) {
  if (g_iob_ready) return;
  g_iob_ready = 1;
  init_io();
  if (g_fds[1].flags & FD_DEVICE) g_iob[1].flags |= S_LINEBUF;
}
API void *__acrt_iob_func(unsigned i) { init_streams(); return &g_iob[i < 3 ? i : 0]; }
API void *__iob_func(void) { init_streams(); return g_iob; }
#define STDIN_ ((Stream *)__acrt_iob_func(0))
#define STDOUT_ ((Stream *)__acrt_iob_func(1))
#define STDERR_ ((Stream *)__acrt_iob_func(2))

static int stream_buffer(Stream *f) {
  if (f->buffer) return 1;
  f->buffer = malloc(DEFAULT_BUFFER);
  if (!f->buffer) return 0;
  f->buffer_size = DEFAULT_BUFFER;
  f->flags |= S_BUF_OWNED;
  return 1;
}
static int stream_text(Stream *f) { FdEntry *e = fd_entry(f->fd); return e && (e->flags & FD_TEXT) && !(e->flags & FD_DEVICE); }

static int stream_flush_write(Stream *f) {
  if (f->write_pos == 0) return 0;
  FdEntry *e = fd_entry(f->fd);
  const int n = f->write_pos;
  f->write_pos = 0;
  if (!e) { f->flags |= S_ERR; return EOF; }
  if (e->flags & FD_APPEND) raw_seek(e, 0, FILE_END);
  if (raw_write(e, f->buffer, (unsigned)n) != n) { f->flags |= S_ERR; return EOF; }
  return 0;
}
/* Drops what was read ahead: the file position goes back to where the program is. */
static void stream_drop_read_ahead(Stream *f) {
  if (f->read_end > f->read_pos) { FdEntry *e = fd_entry(f->fd); if (e) raw_seek(e, -(long long)(f->read_end - f->read_pos), FILE_CURRENT); }
  f->read_pos = f->read_end = 0;
  f->ungot = -1;
}
static int stream_fill(Stream *f) {
  if (f->write_pos && stream_flush_write(f)) return 0;
  if (!stream_buffer(f)) return 0;
  FdEntry *e = fd_entry(f->fd);
  if (!e) { f->flags |= S_ERR; return 0; }
  if (f == STDIN_) stream_flush_write(STDOUT_);      /* a prompt shows before the program waits for the answer */
  const int n = raw_read(e, f->buffer, (unsigned)f->buffer_size);
  if (n < 0) { f->flags |= S_ERR; return 0; }
  f->read_pos = 0;
  f->read_end = n;
  if (n == 0) { f->flags |= S_EOF; return 0; }
  return 1;
}
API int fgetc(void *stream) {
  Stream *f = stream;
  init_streams();
  if (!(f->flags & S_READ)) { f->flags |= S_ERR; g_errno = EBADF; return EOF; }
  if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
  if (f->read_pos >= f->read_end && !stream_fill(f)) return EOF;
  int c = f->buffer[f->read_pos++];
  if (stream_text(f)) {
    if (c == 0x1A) { f->read_pos--; f->flags |= S_EOF; return EOF; }
    if (c == '\r') {
      if (f->read_pos >= f->read_end) {
        /* keep the CR while the next block comes in */
        f->buffer[0] = '\r';
        FdEntry *e = fd_entry(f->fd);
        const int n = e ? raw_read(e, f->buffer + 1, (unsigned)f->buffer_size - 1) : 0;
        f->read_pos = 1;
        f->read_end = 1 + (n > 0 ? n : 0);
        if (n > 0 && f->buffer[1] == '\n') { f->read_pos = 2; return '\n'; }
        return '\r';
      }
      if (f->buffer[f->read_pos] == '\n') { ++f->read_pos; return '\n'; }
    }
  }
  return c;
}
API int getc(void *f) { return fgetc(f); }
API int _fgetc_nolock(void *f) { return fgetc(f); }
API int getchar(void) { return fgetc(STDIN_); }
API int ungetc(int c, void *stream) {
  Stream *f = stream;
  if (c == EOF || f->ungot >= 0) return EOF;
  f->ungot = c & 0xFF;
  f->flags &= ~S_EOF;
  return c;
}
API int fputc(int c, void *stream) {
  Stream *f = stream;
  init_streams();
  if (!(f->flags & S_WRITE)) { f->flags |= S_ERR; g_errno = EBADF; return EOF; }
  if (f->read_end > f->read_pos || f->ungot >= 0) stream_drop_read_ahead(f);
  f->read_pos = f->read_end = 0;
  const unsigned char ch = (unsigned char)c;
  if (f->flags & S_UNBUF) {
    FdEntry *e = fd_entry(f->fd);
    if (!e || _write(f->fd, &ch, 1) != 1) { f->flags |= S_ERR; return EOF; }
    return ch;
  }
  if (!stream_buffer(f)) { f->flags |= S_ERR; return EOF; }
  const int text = stream_text(f);
  if (f->write_pos + 2 > f->buffer_size && stream_flush_write(f)) return EOF;
  if (text && ch == '\n') f->buffer[f->write_pos++] = '\r';
  f->buffer[f->write_pos++] = ch;
  if (((f->flags & S_LINEBUF) && ch == '\n') || f->write_pos >= f->buffer_size - 1) { if (stream_flush_write(f)) return EOF; }
  return ch;
}
API int putc(int c, void *f) { return fputc(c, f); }
API int _fputc_nolock(int c, void *f) { return fputc(c, f); }
API int putchar(int c) { return fputc(c, STDOUT_); }
API int fputs(const char *s, void *f) { for (; *s; ++s) if (fputc((unsigned char)*s, f) == EOF) return EOF; return 0; }
API int puts(const char *s) { if (fputs(s, STDOUT_) == EOF || fputc('\n', STDOUT_) == EOF) return EOF; return 0; }
API int fflush(void *stream) {
  init_streams();
  if (!stream) { int r = 0; for (Stream *f = g_streams; f; f = f->next) if (f->flags & S_WRITE) r |= stream_flush_write(f); r |= stream_flush_write(STDOUT_); return r ? EOF : 0; }
  Stream *f = stream;
  if (f->flags & S_WRITE) return stream_flush_write(f);
  return 0;
}
API int _flushall(void) { fflush(NULL); return 0; }
API char *fgets(char *s, int n, void *f) {
  if (n <= 0) return NULL;
  int i = 0;
  while (i < n - 1) {
    const int c = fgetc(f);
    if (c == EOF) { if (i == 0) return NULL; break; }
    s[i++] = (char)c;
    if (c == '\n') break;
  }
  s[i] = 0;
  return s;
}
API char *gets_s(char *s, size_t_ n) { if (!fgets(s, (int)n, STDIN_)) return NULL; size_t_ l = strlen(s); if (l && s[l - 1] == '\n') s[l - 1] = 0; return s; }
API char *gets(char *s) { return gets_s(s, 0x7FFFFFFF); }
API size_t_ fread(void *buffer, size_t_ size, size_t_ count, void *stream) {
  Stream *f = stream;
  unsigned char *out = buffer;
  const size_t_ total = size * count;
  size_t_ got = 0;
  if (size == 0 || count == 0) return 0;
  while (got < total) {
    const int c = fgetc(f);
    if (c == EOF) break;
    out[got++] = (unsigned char)c;
  }
  return got / size;
}
API size_t_ fwrite(const void *buffer, size_t_ size, size_t_ count, void *stream) {
  const unsigned char *p = buffer;
  const size_t_ total = size * count;
  if (size == 0 || count == 0) return 0;
  for (size_t_ i = 0; i < total; ++i) if (fputc(p[i], stream) == EOF) return i / size;
  return count;
}
API int feof(void *f) { return (((Stream *)f)->flags & S_EOF) != 0; }
API int ferror(void *f) { return (((Stream *)f)->flags & S_ERR) != 0; }
API void clearerr(void *f) { ((Stream *)f)->flags &= ~(S_EOF | S_ERR); }
API int _fileno(void *f) { return ((Stream *)f)->fd; }
API int fileno(void *f) { return ((Stream *)f)->fd; }
API int setvbuf(void *stream, char *buffer, int mode, size_t_ size) {
  Stream *f = stream;
  if (f->buffer && (f->flags & S_BUF_OWNED)) { stream_flush_write(f); free(f->buffer); f->buffer = NULL; f->flags &= ~S_BUF_OWNED; }
  f->flags &= ~(S_UNBUF | S_LINEBUF);
  if (mode == 4) { f->flags |= S_UNBUF; return 0; }
  if (mode == 0x40) f->flags |= S_LINEBUF;
  if (buffer && size > 1) { f->buffer = (unsigned char *)buffer; f->buffer_size = (int)size; f->write_pos = f->read_pos = f->read_end = 0; }
  return 0;
}
API void setbuf(void *f, char *buffer) { setvbuf(f, buffer, buffer ? 0 : 4, 4096); }
API int fseek(void *stream, long offset, int origin) { 
  Stream *f = stream;
  FdEntry *e = fd_entry(f->fd);
  if (!e) return -1;
  if (stream_flush_write(f)) return -1;
  if (origin == 1) offset -= (long)(f->read_end - f->read_pos) + (f->ungot >= 0);
  f->read_pos = f->read_end = 0;
  f->ungot = -1;
  f->flags &= ~S_EOF;
  return raw_seek(e, offset, origin) < 0 ? -1 : 0;
}
API int _fseeki64(void *stream, long long offset, int origin) {
  Stream *f = stream;
  FdEntry *e = fd_entry(f->fd);
  if (!e) return -1;
  if (stream_flush_write(f)) return -1;
  if (origin == 1) offset -= (long long)(f->read_end - f->read_pos) + (f->ungot >= 0);
  f->read_pos = f->read_end = 0;
  f->ungot = -1;
  f->flags &= ~S_EOF;
  return raw_seek(e, offset, origin) < 0 ? -1 : 0;
}
API long long _ftelli64(void *stream) {
  Stream *f = stream;
  FdEntry *e = fd_entry(f->fd);
  if (!e) return -1;
  long long p = raw_seek(e, 0, FILE_CURRENT);
  if (p < 0) return -1;
  return p - (f->read_end - f->read_pos) - (f->ungot >= 0) + f->write_pos;
}
API long ftell(void *f) { return (long)_ftelli64(f); }
API void rewind(void *f) { fseek(f, 0, 0); clearerr(f); }
API int fgetpos(void *f, long long *pos) { long long p = _ftelli64(f); if (p < 0) return -1; *pos = p; return 0; }
API int fsetpos(void *f, const long long *pos) { return _fseeki64(f, *pos, 0); }
API int _lock_file_dummy(void) { return 0; }
API void _lock_file(void *f) { (void)f; }
API void _unlock_file(void *f) { (void)f; }

static Stream *stream_open(int fd, int flags) {
  Stream *f = calloc(1, sizeof(Stream));
  if (!f) return NULL;
  f->fd = fd;
  f->flags = flags;
  f->ungot = -1;
  f->next = g_streams;
  g_streams = f;
  return f;
}
API void *_fdopen(int fd, const char *mode) {
  int flags = 0;
  for (const char *m = mode; *m; ++m) { if (*m == 'r') flags |= S_READ; else if (*m == 'w' || *m == 'a') flags |= S_WRITE; else if (*m == '+') flags |= S_READ | S_WRITE; }
  return fd_entry(fd) ? stream_open(fd, flags) : NULL;
}
API void *fdopen(int fd, const char *mode) { return _fdopen(fd, mode); }
API void *fopen(const char *path, const char *mode) {
  init_streams();
  int oflags = 0, sflags = 0, plus = 0;
  const char *m = mode;
  if (*m == 'r') { sflags = S_READ; oflags = O_RDONLY_; }
  else if (*m == 'w') { sflags = S_WRITE; oflags = O_WRONLY_ | O_CREAT_ | O_TRUNC_; }
  else if (*m == 'a') { sflags = S_WRITE; oflags = O_WRONLY_ | O_CREAT_ | O_APPEND_; }
  else { g_errno = EINVAL; return NULL; }
  for (++m; *m; ++m) {
    if (*m == '+') plus = 1;
    else if (*m == 'b') oflags |= O_BINARY_;
    else if (*m == 't') oflags |= O_TEXT_;
    else if (*m == 'x') oflags |= O_EXCL_;
    else if (*m == ',') break;      /* ccs=UTF-8 and the like: the text is UTF-8 anyway */
  }
  if (plus) { sflags |= S_READ | S_WRITE; oflags = (oflags & ~(O_WRONLY_ | O_RDONLY_)) | O_RDWR_; }
  const int fd = open_common(path, oflags, 0);
  if (fd < 0) return NULL;
  Stream *f = stream_open(fd, sflags);
  if (!f) { _close(fd); return NULL; }
  if (oflags & O_APPEND_) g_fds[fd].flags |= FD_APPEND;
  return f;
}
API int fopen_s(void **out, const char *path, const char *mode) { *out = fopen(path, mode); return *out ? 0 : g_errno; }
API void *_fsopen(const char *path, const char *mode, int share) { (void)share; return fopen(path, mode); }
API void *freopen(const char *path, const char *mode, void *stream) {
  Stream *f = stream;
  fflush(f);
  if (!(f->flags & S_STATIC)) _close(f->fd);
  Stream *n = fopen(path, mode);
  if (!n) return NULL;
  if (f->flags & S_STATIC) { f->fd = n->fd; f->flags = (f->flags & S_STATIC) | (n->flags & (S_READ | S_WRITE)); f->read_pos = f->read_end = f->write_pos = 0; f->ungot = -1; return f; }
  return n;
}
API int fclose(void *stream) {
  Stream *f = stream;
  int r = 0;
  if (f->flags & S_WRITE) r = stream_flush_write(f);
  if (!(f->flags & S_STATIC)) { if (_close(f->fd)) r = EOF; }
  if (f->buffer && (f->flags & S_BUF_OWNED)) free(f->buffer);
  if (!(f->flags & S_STATIC)) {
    for (Stream **p = &g_streams; *p; p = &(*p)->next) if (*p == f) { *p = f->next; break; }
    free(f);
  }
  return r;
}
API int _fcloseall(void) { int n = 0; while (g_streams) { fclose(g_streams); ++n; } return n; }
API void *tmpfile(void) {
  char name[MAX_PATH + 16];
  char dir[MAX_PATH];
  GetTempPathA(sizeof(dir), dir);
  if (!GetTempFileNameA(dir, "tmp", 0, name)) return NULL;
  return fopen(name, "w+b");
}
API char *tmpnam(char *buffer) {
  static char internal[MAX_PATH + 16];
  char dir[MAX_PATH];
  GetTempPathA(sizeof(dir), dir);
  static unsigned counter;
  char *out = buffer ? buffer : internal;
  sprintf(out, "%stmp%X.%X", dir, (unsigned)GetTickCount(), ++counter);
  return out;
}
API void perror(const char *s) { if (s && *s) { fputs(s, STDERR_); fputs(": ", STDERR_); } fputs(strerror(g_errno), STDERR_); fputc('\n', STDERR_); }
API int *__p__fmode(void) { return &g_fmode; }
API int *__p__commode(void) { return &g_commode; }
API int _set_fmode(int mode) { g_fmode = mode; return 0; }
API int _get_fmode(int *mode) { *mode = g_fmode; return 0; }
API void _set_printf_count_output_dummy(void) {}

/* ---- the printf and scanf functions over streams -------------------------------------------------------------------------------------------------------- */

static void put_to_stream(void *stream, char c) { fputc((unsigned char)c, stream); }
static int stream_vprintf(void *stream, const char *format, va_list_ args) { return format_a(put_to_stream, stream, format, args); }
API int vfprintf(void *f, const char *format, va_list_ a) { return stream_vprintf(f, format, a); }
API int vprintf(const char *format, va_list_ a) { return stream_vprintf(STDOUT_, format, a); }
API int fprintf(void *f, const char *format, ...) { va_list_ a; va_start_(a, format); int r = stream_vprintf(f, format, a); va_end_(a); return r; }
API int printf(const char *format, ...) { va_list_ a; va_start_(a, format); int r = stream_vprintf(STDOUT_, format, a); va_end_(a); return r; }
API int fprintf_s(void *f, const char *format, ...) { va_list_ a; va_start_(a, format); int r = stream_vprintf(f, format, a); va_end_(a); return r; }
API int printf_s(const char *format, ...) { va_list_ a; va_start_(a, format); int r = stream_vprintf(STDOUT_, format, a); va_end_(a); return r; }
/* wide: formatted wide, written to the stream as UTF-8 */
static void put_wide_to_stream(void *stream, wchar_t c) {
  static wchar_t high;      /* (a surrogate pair arrives as two units) */
  unsigned cp = c;
  if (cp >= 0xD800 && cp < 0xDC00) { high = (wchar_t)cp; return; }
  if (cp >= 0xDC00 && cp < 0xE000 && high) { cp = 0x10000 + (((unsigned)high - 0xD800) << 10) + (cp - 0xDC00); high = 0; }
  if (cp < 0x80) fputc((int)cp, stream);
  else if (cp < 0x800) { fputc((int)(0xC0 | (cp >> 6)), stream); fputc((int)(0x80 | (cp & 0x3F)), stream); }
  else if (cp < 0x10000) { fputc((int)(0xE0 | (cp >> 12)), stream); fputc((int)(0x80 | ((cp >> 6) & 0x3F)), stream); fputc((int)(0x80 | (cp & 0x3F)), stream); }
  else { fputc((int)(0xF0 | (cp >> 18)), stream); fputc((int)(0x80 | ((cp >> 12) & 0x3F)), stream); fputc((int)(0x80 | ((cp >> 6) & 0x3F)), stream); fputc((int)(0x80 | (cp & 0x3F)), stream); }
}
API int vfwprintf(void *f, const wchar_t *format, va_list_ a) { return format_w(put_wide_to_stream, f, format, a); }
API int vwprintf(const wchar_t *format, va_list_ a) { return format_w(put_wide_to_stream, STDOUT_, format, a); }
API int fwprintf(void *f, const wchar_t *format, ...) { va_list_ a; va_start_(a, format); int r = vfwprintf(f, format, a); va_end_(a); return r; }
API int wprintf(const wchar_t *format, ...) { va_list_ a; va_start_(a, format); int r = vwprintf(format, a); va_end_(a); return r; }
API int fputws(const wchar_t *s, void *f) { for (; *s; ++s) put_wide_to_stream(f, *s); return 0; }
API int fputwc(wchar_t c, void *f) { put_wide_to_stream(f, c); return c; }
API int putwchar(wchar_t c) { return fputwc(c, STDOUT_); }
API void *_wfopen(const wchar_t *path, const wchar_t *mode) {
  char p[MAX_PATH * 3], m[16];
  if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof(p), NULL, NULL) || !WideCharToMultiByte(CP_UTF8, 0, mode, -1, m, sizeof(m), NULL, NULL)) return NULL;
  return fopen(p, m);
}
API int _wremove(const wchar_t *path) { char p[MAX_PATH * 3]; if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof(p), NULL, NULL)) return -1; return remove(p); }
API int _wunlink(const wchar_t *path) { return _wremove(path); }

static int reader_get(void *context) { return fgetc(context); }
static void reader_unget(void *context, int c) { ungetc(c, context); }
typedef struct { const char *text; size_t_ position, length; } TextReader;
static int text_get(void *context) { TextReader *r = context; return r->position < r->length && r->text[r->position] ? (unsigned char)r->text[r->position++] : EOF; }
static void text_unget(void *context, int c) { TextReader *r = context; (void)c; if (r->position > 0) --r->position; }
static int stream_vscanf(void *stream, const char *format, va_list_ args) { Reader r = {reader_get, reader_unget, stream}; return scan_format(&r, format, args); }
static int text_vscanf(const char *text, size_t_ length, const char *format, va_list_ args) { TextReader t = {text, 0, length}; Reader r = {text_get, text_unget, &t}; return scan_format(&r, format, args); }
API int vfscanf(void *f, const char *format, va_list_ a) { return stream_vscanf(f, format, a); }
API int vscanf(const char *format, va_list_ a) { return stream_vscanf(STDIN_, format, a); }
API int vsscanf(const char *s, const char *format, va_list_ a) { return text_vscanf(s, strlen(s), format, a); }
API int fscanf(void *f, const char *format, ...) { va_list_ a; va_start_(a, format); int r = stream_vscanf(f, format, a); va_end_(a); return r; }
API int scanf(const char *format, ...) { va_list_ a; va_start_(a, format); int r = stream_vscanf(STDIN_, format, a); va_end_(a); return r; }
API int sscanf(const char *s, const char *format, ...) { va_list_ a; va_start_(a, format); int r = text_vscanf(s, strlen(s), format, a); va_end_(a); return r; }

/* the Universal CRT's entry points */
API int __stdio_common_vfprintf(unsigned long long options, void *f, const char *format, void *locale, va_list_ a) { (void)options; (void)locale; return stream_vprintf(f, format, a); }
API int __stdio_common_vfprintf_s(unsigned long long options, void *f, const char *format, void *locale, va_list_ a) { (void)options; (void)locale; return stream_vprintf(f, format, a); }
API int __stdio_common_vsprintf(unsigned long long options, char *buffer, size_t_ size, const char *format, void *locale, va_list_ a) {
  (void)locale;
  const int r = format_to_buffer(buffer, size, format, a);
  if ((options & 1) == 0 && r >= 0 && (size_t_)r >= size && size) { /* the legacy behaviour (-1 when it does not fit) is asked for by option bit 1 clear + _CRT_INTERNAL_PRINTF_LEGACY... keep the standard result */ }
  return r;
}
API int __stdio_common_vsprintf_s(unsigned long long options, char *buffer, size_t_ size, const char *format, void *locale, va_list_ a) {
  (void)options; (void)locale;
  const int r = format_to_buffer(buffer, size, format, a);
  return (r < 0 || (size_t_)r >= size) ? -1 : r;
}
API int __stdio_common_vsnprintf_s(unsigned long long options, char *buffer, size_t_ size, size_t_ max, const char *format, void *locale, va_list_ a) {
  (void)options; (void)locale; (void)max;
  const int r = format_to_buffer(buffer, size, format, a);
  return (r < 0 || (size_t_)r >= size) ? -1 : r;
}
API int __stdio_common_vswprintf(unsigned long long options, wchar_t *buffer, size_t_ size, const wchar_t *format, void *locale, va_list_ a) { (void)options; (void)locale; return format_to_wide_buffer(buffer, size, format, a); }
API int __stdio_common_vfwprintf(unsigned long long options, void *f, const wchar_t *format, void *locale, va_list_ a) { (void)options; (void)locale; return vfwprintf(f, format, a); }
API int __stdio_common_vfscanf(unsigned long long options, void *f, const char *format, void *locale, va_list_ a) { (void)options; (void)locale; return stream_vscanf(f, format, a); }
API int __stdio_common_vsscanf(unsigned long long options, const char *s, size_t_ length, const char *format, void *locale, va_list_ a) { (void)options; (void)locale; return text_vscanf(s, length, format, a); }
API unsigned long long *__local_stdio_printf_options(void) { static unsigned long long o; return &o; }
API unsigned long long *__local_stdio_scanf_options(void) { static unsigned long long o; return &o; }

/* ====================================================================================================================================================================
 * stdlib: sorting, random numbers, environment, wide/multibyte conversion
 * ==================================================================================================================================================================== */

static void swap_bytes(unsigned char *a, unsigned char *b, size_t_ n) { while (n--) { unsigned char t = *a; *a++ = *b; *b++ = t; } }
typedef int (*compare_fn)(const void *, const void *);
typedef int (*compare_s_fn)(void *, const void *, const void *);
static void sift_down(unsigned char *base, size_t_ size, size_t_ start, size_t_ count, compare_s_fn cmp, void *context) {
  size_t_ root = start;
  while (root <= (count - 2) / 2 && count >= 2) {
    size_t_ child = 2 * root + 1, pick = root;
    if (cmp(context, base + pick * size, base + child * size) < 0) pick = child;
    if (child + 1 < count && cmp(context, base + pick * size, base + (child + 1) * size) < 0) pick = child + 1;
    if (pick == root) return;
    swap_bytes(base + root * size, base + pick * size, size);
    root = pick;
  }
}
/* Heap sort: in place, no recursion, no allocation (the C library may not run out of either). */
static void sort_with(void *base, size_t_ count, size_t_ size, compare_s_fn cmp, void *context) {
  if (count < 2 || size == 0) return;
  unsigned char *b = base;
  for (size_t_ i = count / 2; i-- > 0;) sift_down(b, size, i, count, cmp, context);
  for (size_t_ end = count - 1; end > 0; --end) { swap_bytes(b, b + end * size, size); sift_down(b, size, 0, end, cmp, context); }
}
static int compare_plain(void *context, const void *a, const void *b) { return ((compare_fn)context)(a, b); }
API void qsort(void *base, size_t_ count, size_t_ size, compare_fn cmp) { sort_with(base, count, size, compare_plain, (void *)cmp); }
API void qsort_s(void *base, size_t_ count, size_t_ size, compare_s_fn cmp, void *context) { sort_with(base, count, size, cmp, context); }
API void *bsearch(const void *key, const void *base, size_t_ count, size_t_ size, compare_fn cmp) {
  size_t_ lo = 0, hi = count;
  while (lo < hi) {
    const size_t_ mid = lo + (hi - lo) / 2;
    const void *item = (const char *)base + mid * size;
    const int r = cmp(key, item);
    if (r == 0) return (void *)item;
    if (r < 0) hi = mid; else lo = mid + 1;
  }
  return NULL;
}

static unsigned g_rand_state = 1;
API void srand(unsigned seed) { g_rand_state = seed; }
API int rand(void) { g_rand_state = g_rand_state * 214013u + 2531011u; return (int)((g_rand_state >> 16) & 0x7FFF); }
API int rand_s(unsigned *out) { LARGE_INTEGER c; QueryPerformanceCounter(&c); g_rand_state = g_rand_state * 214013u + 2531011u + (unsigned)c.QuadPart; *out = g_rand_state; return 0; }

API char *getenv(const char *name) {
  init_environment();
  const size_t_ n = strlen(name);
  for (char **e = g_environ; *e; ++e) if (_strnicmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
  return NULL;
}
API int getenv_s(size_t_ *needed, char *buffer, size_t_ size, const char *name) {
  const char *v = getenv(name);
  if (!v) { if (needed) *needed = 0; if (buffer && size) buffer[0] = 0; return 0; }
  const size_t_ n = strlen(v) + 1;
  if (needed) *needed = n;
  if (!buffer || n > size) return buffer ? ERANGE : 0;
  memcpy(buffer, v, n);
  return 0;
}
API int _putenv(const char *entry) {
  init_environment();
  const char *eq = strchr(entry, '=');
  if (!eq) return -1;
  const size_t_ n = (size_t_)(eq - entry);
  char name[256];
  if (n == 0 || n >= sizeof(name)) return -1;
  memcpy(name, entry, n);
  name[n] = 0;
  SetEnvironmentVariableA(name, eq[1] ? eq + 1 : NULL);
  int count = 0, found = -1;
  for (char **e = g_environ; *e; ++e, ++count) if (found < 0 && _strnicmp(*e, name, n) == 0 && (*e)[n] == '=') found = count;
  if (found >= 0) {
    if (!eq[1]) { for (int i = found; i < count; ++i) g_environ[i] = g_environ[i + 1]; return 0; }
    g_environ[found] = _strdup(entry);
    return 0;
  }
  if (!eq[1]) return 0;
  char **bigger = realloc(g_environ, (size_t_)(count + 2) * sizeof(char *));
  if (!bigger) return -1;
  bigger[count] = _strdup(entry);
  bigger[count + 1] = NULL;
  g_environ = bigger;
  return 0;
}
API int putenv(const char *entry) { return _putenv(entry); }
API int _putenv_s(const char *name, const char *value) { char entry[1024]; if (strlen(name) + strlen(value) + 2 > sizeof(entry)) return EINVAL; sprintf(entry, "%s=%s", name, value); return _putenv(entry) ? EINVAL : 0; }
API int system(const char *command) { (void)command; g_errno = ENOENT; return command ? -1 : 0; }
API int _getpid(void) { return (int)GetCurrentProcessId(); }
API int getpid(void) { return _getpid(); }
API void _sleep(unsigned long ms) { Sleep((DWORD)ms); }
API unsigned sleep(unsigned s) { Sleep(s * 1000u); return 0; }
API int usleep(unsigned us) { Sleep(us / 1000u); return 0; }
API int _getch(void) { unsigned char c; return _read(0, &c, 1) == 1 ? c : EOF; }
API int _getche(void) { int c = _getch(); if (c != EOF) { unsigned char ch = (unsigned char)c; _write(1, &ch, 1); } return c; }
API int _kbhit(void) { return 0; }
API int _putch(int c) { unsigned char ch = (unsigned char)c; return _write(1, &ch, 1) == 1 ? c : EOF; }

/* multibyte (UTF-8) and wide */
API size_t_ mbstowcs(wchar_t *out, const char *s, size_t_ n) {
  const int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (need <= 0) return (size_t_)-1;
  if (!out) return (size_t_)need - 1;
  wchar_t *tmp = malloc((size_t_)need * 2);
  if (!tmp) return (size_t_)-1;
  MultiByteToWideChar(CP_UTF8, 0, s, -1, tmp, need);
  size_t_ count = (size_t_)need - 1;
  if (count > n) count = n;
  memcpy(out, tmp, count * 2);
  if (count < n) out[count] = 0;
  free(tmp);
  return count;
}
API size_t_ wcstombs(char *out, const wchar_t *s, size_t_ n) {
  const int need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
  if (need <= 0) return (size_t_)-1;
  if (!out) return (size_t_)need - 1;
  char *tmp = malloc((size_t_)need);
  if (!tmp) return (size_t_)-1;
  WideCharToMultiByte(CP_UTF8, 0, s, -1, tmp, need, NULL, NULL);
  size_t_ count = (size_t_)need - 1;
  if (count > n) count = n;
  memcpy(out, tmp, count);
  if (count < n) out[count] = 0;
  free(tmp);
  return count;
}
API int mblen(const char *s, size_t_ n) {
  if (!s) return 0;
  if (!n || !*s) return 0;
  const unsigned char c = (unsigned char)*s;
  const int len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : -1;
  return len > 0 && (size_t_)len <= n ? len : -1;
}
API int mbtowc(wchar_t *out, const char *s, size_t_ n) {
  const int len = mblen(s, n);
  if (len <= 0) return len;
  wchar_t w[2];
  if (MultiByteToWideChar(CP_UTF8, 0, s, len, w, 2) < 1) return -1;
  if (out) *out = w[0];
  return len;
}
API int wctomb(char *out, wchar_t c) {
  if (!out) return 0;
  const int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, out, 4, NULL, NULL);
  return n > 0 ? n : -1;
}
API size_t_ wcrtomb(char *out, wchar_t c, void *state) { (void)state; if (!out) return 1; int n = wctomb(out, c); return n < 0 ? (size_t_)-1 : (size_t_)n; }
API size_t_ mbrtowc(wchar_t *out, const char *s, size_t_ n, void *state) { (void)state; if (!s) return 0; if (!n) return (size_t_)-2; int r = mbtowc(out, s, n); return r < 0 ? (size_t_)-1 : (size_t_)r; }
API unsigned btowc(int c) { return c >= 0 && c < 128 ? (unsigned)c : (unsigned)-1; }
API int wctob(unsigned c) { return c < 128 ? (int)c : EOF; }
API long wcstol(const wchar_t *s, wchar_t **end, int base) {
  char tmp[80];
  int i = 0;
  while (s[i] && i < 79 && s[i] < 128) { tmp[i] = (char)s[i]; ++i; }
  tmp[i] = 0;
  char *e;
  long v = strtol(tmp, &e, base);
  if (end) *end = (wchar_t *)s + (e - tmp);
  return v;
}
API unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base) { return (unsigned long)wcstol(s, end, base); }
API int _wtoi(const wchar_t *s) { return (int)wcstol(s, NULL, 10); }
API long _wtol(const wchar_t *s) { return wcstol(s, NULL, 10); }
API double wcstod(const wchar_t *s, wchar_t **end) {
  char tmp[128];
  int i = 0;
  while (s[i] && i < 127 && s[i] < 128) { tmp[i] = (char)s[i]; ++i; }
  tmp[i] = 0;
  char *e;
  double v = strtod(tmp, &e);
  if (end) *end = (wchar_t *)s + (e - tmp);
  return v;
}
API wchar_t *_wgetenv(const wchar_t *name) {
  static wchar_t *last;
  char n[256];
  if (wcstombs(n, name, sizeof(n)) == (size_t_)-1) return NULL;
  const char *v = getenv(n);
  if (!v) return NULL;
  free(last);
  const size_t_ len = mbstowcs(NULL, v, 0);
  last = malloc((len + 1) * 2);
  if (last) mbstowcs(last, v, len + 1);
  return last;
}

/* ====================================================================================================================================================================
 * time.h (UTC; the zone is UTC)
 * ==================================================================================================================================================================== */

typedef long long time64_t;
typedef struct { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; } Tm;
static const unsigned short g_days_before[2][13] = {{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365}, {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};
static int is_leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
API time64_t _time64(time64_t *out) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  const time64_t r = t >= 116444736000000000ull ? (time64_t)((t - 116444736000000000ull) / 10000000ull) : 0;
  if (out) *out = r;
  return r;
}
API time64_t time(time64_t *out) { return _time64(out); }
API int _gmtime64_s(Tm *tm, const time64_t *tp) {
  time64_t t = *tp;
  if (t < 0) { g_errno = EINVAL; return EINVAL; }
  const long long days = t / 86400;
  const long rem = (long)(t % 86400);
  tm->tm_hour = (int)(rem / 3600);
  tm->tm_min = (int)(rem % 3600 / 60);
  tm->tm_sec = (int)(rem % 60);
  tm->tm_wday = (int)((days + 4) % 7);
  long long year = 1970, left = days;
  for (;;) {
    const int len = is_leap(year) ? 366 : 365;
    if (left < len) break;
    left -= len;
    ++year;
  }
  const int leap = is_leap(year);
  tm->tm_year = (int)(year - 1900);
  tm->tm_yday = (int)left;
  int m = 0;
  while (left >= g_days_before[leap][m + 1]) ++m;
  tm->tm_mon = m;
  tm->tm_mday = (int)(left - g_days_before[leap][m]) + 1;
  tm->tm_isdst = 0;
  return 0;
}
static Tm g_tm;
API Tm *_gmtime64(const time64_t *t) { return _gmtime64_s(&g_tm, t) ? NULL : &g_tm; }
API Tm *gmtime(const time64_t *t) { return _gmtime64(t); }
API Tm *localtime(const time64_t *t) { return _gmtime64(t); }
API Tm *_localtime64(const time64_t *t) { return _gmtime64(t); }
API int _localtime64_s(Tm *tm, const time64_t *t) { return _gmtime64_s(tm, t); }
API int gmtime_s(Tm *tm, const time64_t *t) { return _gmtime64_s(tm, t); }
API int localtime_s(Tm *tm, const time64_t *t) { return _gmtime64_s(tm, t); }
API time64_t _mkgmtime64(Tm *tm) {
  long long year = 1900 + (long long)tm->tm_year + tm->tm_mon / 12;
  int mon = tm->tm_mon % 12;
  if (mon < 0) { mon += 12; --year; }
  long long days = 0;
  if (year >= 1970) for (long long y = 1970; y < year; ++y) days += is_leap(y) ? 366 : 365;
  else for (long long y = year; y < 1970; ++y) days -= is_leap(y) ? 366 : 365;
  days += g_days_before[is_leap(year)][mon] + tm->tm_mday - 1;
  const time64_t t = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
  _gmtime64_s(tm, &t);
  return t;
}
API time64_t _mkgmtime(Tm *tm) { return _mkgmtime64(tm); }
API time64_t mktime(Tm *tm) { return _mkgmtime64(tm); }
API time64_t _mktime64(Tm *tm) { return _mkgmtime64(tm); }
API double difftime(time64_t a, time64_t b) { return (double)(a - b); }
API double _difftime64(time64_t a, time64_t b) { return (double)(a - b); }
API long clock(void) { return (long)GetTickCount64(); }
API void _tzset(void) {}
API void tzset(void) {}
API long *__p__timezone(void) { static long z; return &z; }
API int *__p__daylight(void) { static int d; return &d; }
API char **__p__tzname(void) { static char *names[2] = {"UTC", "UTC"}; return names; }
API int _get_timezone(long *z) { *z = 0; return 0; }
API int _get_daylight(int *d) { *d = 0; return 0; }
static const char *const g_day_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const g_month_names[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
typedef struct { char *out; size_t_ capacity, length; int overflow; } StrfBuffer;
static void strf_put(StrfBuffer *b, const char *s, size_t_ n) { for (size_t_ i = 0; i < n; ++i) { if (b->length + 1 < b->capacity) b->out[b->length] = s[i]; else b->overflow = 1; ++b->length; } }
static void strf_str(StrfBuffer *b, const char *s) { strf_put(b, s, strlen(s)); }
static void strf_num(StrfBuffer *b, long v, int width, char pad) { char t[24]; int n = 0; if (v < 0) v = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n < width) t[n++] = pad; while (n) strf_put(b, &t[--n], 1); }
static void strf_format(StrfBuffer *b, const char *f, const Tm *tm) {
  for (; *f; ++f) {
    if (*f != '%' || !f[1]) { strf_put(b, f, 1); continue; }
    ++f;
    if (*f == '#' || *f == '-') ++f;
    switch (*f) {
      case 'a': strf_put(b, g_day_names[tm->tm_wday % 7], 3); break;
      case 'A': strf_str(b, g_day_names[tm->tm_wday % 7]); break;
      case 'b': case 'h': strf_put(b, g_month_names[tm->tm_mon % 12], 3); break;
      case 'B': strf_str(b, g_month_names[tm->tm_mon % 12]); break;
      case 'c': strf_format(b, "%a %b %e %H:%M:%S %Y", tm); break;
      case 'C': strf_num(b, (tm->tm_year + 1900) / 100, 2, '0'); break;
      case 'd': strf_num(b, tm->tm_mday, 2, '0'); break;
      case 'D': strf_format(b, "%m/%d/%y", tm); break;
      case 'e': strf_num(b, tm->tm_mday, 2, ' '); break;
      case 'F': strf_format(b, "%Y-%m-%d", tm); break;
      case 'H': strf_num(b, tm->tm_hour, 2, '0'); break;
      case 'I': strf_num(b, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, 2, '0'); break;
      case 'j': strf_num(b, tm->tm_yday + 1, 3, '0'); break;
      case 'm': strf_num(b, tm->tm_mon + 1, 2, '0'); break;
      case 'M': strf_num(b, tm->tm_min, 2, '0'); break;
      case 'n': strf_put(b, "\n", 1); break;
      case 'p': strf_str(b, tm->tm_hour < 12 ? "AM" : "PM"); break;
      case 'r': strf_format(b, "%I:%M:%S %p", tm); break;
      case 'R': strf_format(b, "%H:%M", tm); break;
      case 'S': strf_num(b, tm->tm_sec, 2, '0'); break;
      case 't': strf_put(b, "\t", 1); break;
      case 'T': case 'X': strf_format(b, "%H:%M:%S", tm); break;
      case 'u': strf_num(b, tm->tm_wday ? tm->tm_wday : 7, 1, '0'); break;
      case 'w': strf_num(b, tm->tm_wday, 1, '0'); break;
      case 'x': strf_format(b, "%m/%d/%y", tm); break;
      case 'y': strf_num(b, (tm->tm_year + 1900) % 100, 2, '0'); break;
      case 'Y': strf_num(b, tm->tm_year + 1900, 4, '0'); break;
      case 'z': strf_str(b, "+0000"); break;
      case 'Z': strf_str(b, "UTC"); break;
      case '%': strf_put(b, "%", 1); break;
      default: strf_put(b, f - 1, 2); break;
    }
  }
}
API size_t_ strftime(char *out, size_t_ capacity, const char *format, const Tm *tm) {
  StrfBuffer b = {out, capacity, 0, 0};
  strf_format(&b, format, tm);
  if (b.overflow || capacity == 0) { if (capacity) out[0] = 0; return 0; }
  out[b.length] = 0;
  return b.length;
}
API char *asctime(const Tm *tm) { static char text[32]; strftime(text, sizeof(text), "%a %b %e %H:%M:%S %Y\n", tm); return text; }
API char *ctime(const time64_t *t) { return asctime(gmtime(t)); }
API char *_ctime64(const time64_t *t) { return ctime(t); }
typedef struct { time64_t time; unsigned short millitm; short timezone, dstflag; } TimeB;
API void _ftime64(TimeB *tb) { FILETIME ft; GetSystemTimeAsFileTime(&ft); unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime; t -= 116444736000000000ull; tb->time = (time64_t)(t / 10000000ull); tb->millitm = (unsigned short)(t / 10000 % 1000); tb->timezone = 0; tb->dstflag = 0; }
API void _ftime(TimeB *tb) { _ftime64(tb); }
API int gettimeofday(void *tv, void *tz) { (void)tz; FILETIME ft; GetSystemTimeAsFileTime(&ft); unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime; t -= 116444736000000000ull; long long *p = tv; p[0] = (long long)(t / 10000000ull); p[1] = (long long)(t / 10 % 1000000); return 0; }

/* ====================================================================================================================================================================
 * setjmp and longjmp: the layout of the Microsoft _JUMP_BUFFER (256 bytes)
 * ==================================================================================================================================================================== */

__asm__(
    ".text\n"
    ".globl _setjmp\n_setjmp:\n"
    ".globl __intrinsic_setjmp\n__intrinsic_setjmp:\n"
    ".globl _setjmpex\n_setjmpex:\n"
    ".globl setjmp\nsetjmp:\n"
    "  movq %rdx, 0(%rcx)\n"
    "  movq %rbx, 8(%rcx)\n"
    "  leaq 8(%rsp), %rax\n"
    "  movq %rax, 16(%rcx)\n"
    "  movq %rbp, 24(%rcx)\n"
    "  movq %rsi, 32(%rcx)\n"
    "  movq %rdi, 40(%rcx)\n"
    "  movq %r12, 48(%rcx)\n"
    "  movq %r13, 56(%rcx)\n"
    "  movq %r14, 64(%rcx)\n"
    "  movq %r15, 72(%rcx)\n"
    "  movq (%rsp), %rax\n"
    "  movq %rax, 80(%rcx)\n"
    "  stmxcsr 88(%rcx)\n"
    "  fnstcw 92(%rcx)\n"
    "  movdqa %xmm6, 96(%rcx)\n"
    "  movdqa %xmm7, 112(%rcx)\n"
    "  movdqa %xmm8, 128(%rcx)\n"
    "  movdqa %xmm9, 144(%rcx)\n"
    "  movdqa %xmm10, 160(%rcx)\n"
    "  movdqa %xmm11, 176(%rcx)\n"
    "  movdqa %xmm12, 192(%rcx)\n"
    "  movdqa %xmm13, 208(%rcx)\n"
    "  movdqa %xmm14, 224(%rcx)\n"
    "  movdqa %xmm15, 240(%rcx)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".globl longjmp\nlongjmp:\n"
    "  movl %edx, %eax\n"
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  movl $1, %eax\n"
    "1:\n"
    "  movq 8(%rcx), %rbx\n"
    "  movq 24(%rcx), %rbp\n"
    "  movq 32(%rcx), %rsi\n"
    "  movq 40(%rcx), %rdi\n"
    "  movq 48(%rcx), %r12\n"
    "  movq 56(%rcx), %r13\n"
    "  movq 64(%rcx), %r14\n"
    "  movq 72(%rcx), %r15\n"
    "  ldmxcsr 88(%rcx)\n"
    "  fnclex\n"
    "  fldcw 92(%rcx)\n"
    "  movdqa 96(%rcx), %xmm6\n"
    "  movdqa 112(%rcx), %xmm7\n"
    "  movdqa 128(%rcx), %xmm8\n"
    "  movdqa 144(%rcx), %xmm9\n"
    "  movdqa 160(%rcx), %xmm10\n"
    "  movdqa 176(%rcx), %xmm11\n"
    "  movdqa 192(%rcx), %xmm12\n"
    "  movdqa 208(%rcx), %xmm13\n"
    "  movdqa 224(%rcx), %xmm14\n"
    "  movdqa 240(%rcx), %xmm15\n"
    "  movq 16(%rcx), %rsp\n"
    "  jmpq *80(%rcx)\n"
    ".section .drectve\n.ascii \" -export:_setjmp -export:__intrinsic_setjmp -export:_setjmpex -export:setjmp -export:longjmp\"\n.text\n");

/* ====================================================================================================================================================================
 * Signals, exit, atexit and the start-up entry points of the runtime
 * ==================================================================================================================================================================== */

typedef void (*signal_handler)(int);
#define SIG_DFL_ ((signal_handler)0)
#define SIG_IGN_ ((signal_handler)1)
#define SIG_ERR_ ((signal_handler)-1)
static signal_handler g_signals[23];
API signal_handler signal(int sig, signal_handler handler) {
  if (sig < 1 || sig > 22 || sig == 9 || sig == 17 || sig == 18) { g_errno = EINVAL; return SIG_ERR_; }
  signal_handler old = g_signals[sig];
  g_signals[sig] = handler;
  return old;
}
static void run_exit_functions(void);
API void abort(void);
API int raise(int sig) {
  if (sig < 1 || sig > 22) { g_errno = EINVAL; return -1; }
  signal_handler h = g_signals[sig];
  if (h == SIG_IGN_) return 0;
  if (h == SIG_DFL_) {
    if (sig == 2 || sig == 15 || sig == 21) { _flushall(); ExitProcess(3); }
    if (sig == 6) { _flushall(); ExitProcess(3); }
    ExitProcess(3);
  }
  g_signals[sig] = SIG_DFL_;
  h(sig);
  return 0;
}
static int g_abort_message = 1;
API unsigned _set_abort_behavior(unsigned flags, unsigned mask) { (void)mask; g_abort_message = (flags & 1) != 0; return 0; }
API void abort(void) {
  if (g_signals[6] && g_signals[6] != SIG_IGN_) raise(6);
  if (g_abort_message) { const char *m = "\nThis application has requested the Runtime to terminate it in an unusual way.\n"; _write(2, m, (unsigned)strlen(m)); }
  ExitProcess(3);
}
API void _assert(const char *message, const char *file, unsigned line) { fprintf(STDERR_, "Assertion failed: %s, file %s, line %u\n", message, file, line); abort(); }
API void _wassert(const wchar_t *message, const wchar_t *file, unsigned line) { fwprintf(STDERR_, L"Assertion failed: %ls, file %ls, line %u\n", message, file, line); abort(); }

/* atexit/_onexit functions run last-in first-out, before the streams are flushed. */
typedef void (*exit_fn)(void);
static exit_fn *g_exit_list;
static int g_exit_count, g_exit_capacity, g_exiting;
API int atexit(exit_fn f) {
  if (g_exit_count == g_exit_capacity) {
    const int capacity = g_exit_capacity ? g_exit_capacity * 2 : 32;
    exit_fn *bigger = realloc(g_exit_list, (size_t_)capacity * sizeof(exit_fn));
    if (!bigger) return -1;
    g_exit_list = bigger;
    g_exit_capacity = capacity;
  }
  g_exit_list[g_exit_count++] = f;
  return 0;
}
API int _crt_atexit(exit_fn f) { return atexit(f); }
API exit_fn _onexit(exit_fn f) { return atexit(f) == 0 ? f : NULL; }
API exit_fn __dllonexit(exit_fn f, exit_fn **begin, exit_fn **end) { (void)begin; (void)end; return atexit(f) == 0 ? f : NULL; }
API int _crt_at_quick_exit(exit_fn f) { (void)f; return 0; }
API int at_quick_exit(exit_fn f) { (void)f; return 0; }
typedef struct { exit_fn *first, *last, *end; } OnExitTable;
API int _initialize_onexit_table(OnExitTable *t) { t->first = t->last = t->end = NULL; return 0; }
API int _register_onexit_function(OnExitTable *t, exit_fn f) {
  const size_t_ count = (size_t_)(t->last - t->first);
  exit_fn *bigger = realloc(t->first, (count + 1) * sizeof(exit_fn));
  if (!bigger) return -1;
  bigger[count] = f;
  t->first = bigger;
  t->last = bigger + count + 1;
  t->end = t->last;
  return 0;
}
API int _execute_onexit_table(OnExitTable *t) {
  exit_fn *first = t->first, *last = t->last;
  t->first = t->last = t->end = NULL;
  while (last > first) { exit_fn f = *--last; if (f) f(); }
  free(first);
  return 0;
}
static void run_exit_functions(void) {
  while (g_exit_count > 0) { exit_fn f = g_exit_list[--g_exit_count]; if (f) f(); }
}
API void _cexit(void) { if (g_exiting) return; g_exiting = 1; run_exit_functions(); _flushall(); }
API void _c_exit(void) {}
API void exit(int code) { _cexit(); ExitProcess((UINT)code); }
API void _exit(int code) { ExitProcess((UINT)code); }
API void quick_exit(int code) { _exit(code); }
API void _Exit(int code) { _exit(code); }
API void _amsg_exit(int code) { char m[64]; sprintf(m, "runtime error R%04d\n", code); _write(2, m, (unsigned)strlen(m)); ExitProcess(255); }
API void terminate(void) { abort(); }
API void _invalid_parameter_noinfo(void) { g_errno = EINVAL; }
API void _invalid_parameter_noinfo_noreturn(void) { g_errno = EINVAL; ExitProcess(3); }
API void *_set_invalid_parameter_handler(void *h) { static void *old; void *r = old; old = h; return r; }
API void *_get_invalid_parameter_handler(void) { return NULL; }
API void *_set_purecall_handler(void *h) { (void)h; return NULL; }
API void _purecall(void) { ExitProcess(3); }
API int _get_errno(int *e) { *e = g_errno; return 0; }
API int _set_errno(int e) { g_errno = e; return 0; }
API int _XcptFilter(unsigned long code, void *pointers) { (void)code; (void)pointers; return 0; }
API int _seh_filter_exe(unsigned long code, void *pointers) { (void)code; (void)pointers; return 0; }
API int _seh_filter_dll(unsigned long code, void *pointers) { (void)code; (void)pointers; return 0; }
API void _lock(int n) { (void)n; }
API void _unlock(int n) { (void)n; }
API int _set_app_type(int type) { (void)type; return 0; }
API void __set_app_type(int type) { (void)type; }
API void _setargv(void) { init_arguments(); }
API void __setargv(void) { init_arguments(); }
API void _wsetargv(void) { init_arguments(); }
API int _configure_narrow_argv(int mode) { (void)mode; init_arguments(); return 0; }
API int _configure_wide_argv(int mode) { (void)mode; init_arguments(); return 0; }
API int _initialize_narrow_environment(void) { init_environment(); return 0; }
API int _initialize_wide_environment(void) { init_environment(); return 0; }
API char **_get_initial_narrow_environment(void) { init_environment(); return g_environ; }
API wchar_t **_get_initial_wide_environment(void) { return NULL; }
API char **__p__acmdln(void) { static char *line; if (!line) line = GetCommandLineA(); return &line; }
API wchar_t **__p__wcmdln(void) { static wchar_t *line; if (!line) line = GetCommandLineW(); return &line; }
API char ***__p___initenv(void) { init_environment(); return &g_environ; }
API void *__p__wenviron(void) { static wchar_t **e; return &e; }
API wchar_t **__p__wpgmptr(void) { static wchar_t *p; if (!p) { init_arguments(); p = g_wargv && g_wargv[0] ? g_wargv[0] : L""; } return &p; }
API int __getmainargs(int *argc, char ***argv, char ***env, int wildcard, void *startup_info) {
  (void)wildcard; (void)startup_info;
  init_arguments();
  init_environment();
  *argc = g_argc;
  *argv = g_argv;
  *env = g_environ;
  return 0;
}
API int __wgetmainargs(int *argc, wchar_t ***argv, wchar_t ***env, int wildcard, void *startup_info) {
  (void)wildcard; (void)startup_info;
  init_arguments();
  static wchar_t *no_env[1];
  *argc = g_argc;
  *argv = g_wargv;
  *env = no_env;
  return 0;
}
typedef void (*init_fn)(void);
typedef int (*init_e_fn)(void);
API void _initterm(init_fn *begin, init_fn *end) { for (; begin < end; ++begin) if (*begin) (*begin)(); }
API int _initterm_e(init_e_fn *begin, init_e_fn *end) { for (; begin < end; ++begin) if (*begin) { int r = (*begin)(); if (r) return r; } return 0; }

/* The two threading entry points are completed when the system can run more than one thread. */
API uintptr_t_ _beginthreadex(void *security, unsigned stack, unsigned (__stdcall *start)(void *), void *argument, unsigned flags, unsigned *id) {
  (void)security; (void)stack; (void)flags; (void)id;
  HANDLE h = CreateThread(NULL, stack, (LPTHREAD_START_ROUTINE)start, argument, flags, id);
  return (uintptr_t_)h;
}
API void _endthreadex(unsigned code) { ExitThread(code); }
API uintptr_t_ _beginthread(void (__cdecl *start)(void *), unsigned stack, void *argument) {
  HANDLE h = CreateThread(NULL, stack, (LPTHREAD_START_ROUTINE)(void *)start, argument, 0, NULL);
  return (uintptr_t_)h;
}
API void _endthread(void) { ExitThread(0); }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) { (void)module; (void)reason; (void)reserved; return TRUE; }

/* Structured exception handling arrives with the unwinder; until then no handler claims an exception. */
API int __C_specific_handler(void *record, void *frame, void *context, void *dispatcher) { (void)record; (void)frame; (void)context; (void)dispatcher; return 1; /* ExceptionContinueSearch */ }
