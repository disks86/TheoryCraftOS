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

/* The C library functions that ntdll.dll and msvcrt.dll both export: memory and string functions, written once. The including file defines EXPORT (and the types of
 * tcos_win.h). Not compiled by itself. */


EXPORT void *memcpy(void *d, const void *s, size_t_ n) {
  BYTE *p = d;
  const BYTE *q = s;
  while (n--) *p++ = *q++;
  return d;
}
EXPORT void *memmove(void *d, const void *s, size_t_ n) {
  BYTE *p = d;
  const BYTE *q = s;
  if (p < q) {
    while (n--) *p++ = *q++;
  } else {
    p += n;
    q += n;
    while (n--) *--p = *--q;
  }
  return d;
}
EXPORT void *memset(void *d, int c, size_t_ n) {
  BYTE *p = d;
  while (n--) *p++ = (BYTE)c;
  return d;
}
EXPORT int memcmp(const void *a, const void *b, size_t_ n) {
  const BYTE *p = a, *q = b;
  for (; n; --n, ++p, ++q) if (*p != *q) return *p < *q ? -1 : 1;
  return 0;
}
EXPORT void *memchr(const void *s, int c, size_t_ n) {
  const BYTE *p = s;
  for (; n; --n, ++p) if (*p == (BYTE)c) return (void *)p;
  return NULL;
}
EXPORT size_t_ strlen(const char *s) { size_t_ n = 0; while (s[n]) ++n; return n; }
EXPORT size_t_ wcslen(const WCHAR *s) { size_t_ n = 0; while (s[n]) ++n; return n; }
EXPORT int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) ++a, ++b;
  return (BYTE)*a < (BYTE)*b ? -1 : (BYTE)*a > (BYTE)*b;
}
EXPORT int strncmp(const char *a, const char *b, size_t_ n) {
  for (; n; --n, ++a, ++b) {
    if (*a != *b) return (BYTE)*a < (BYTE)*b ? -1 : 1;
    if (!*a) return 0;
  }
  return 0;
}
EXPORT int wcscmp(const WCHAR *a, const WCHAR *b) {
  while (*a && *a == *b) ++a, ++b;
  return *a < *b ? -1 : *a > *b;
}
EXPORT int wcsncmp(const WCHAR *a, const WCHAR *b, size_t_ n) {
  for (; n; --n, ++a, ++b) {
    if (*a != *b) return *a < *b ? -1 : 1;
    if (!*a) return 0;
  }
  return 0;
}
EXPORT char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
EXPORT char *strncpy(char *d, const char *s, size_t_ n) {
  size_t_ i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
EXPORT char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
EXPORT WCHAR *wcscpy(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while ((*d++ = *s++)) {} return r; }
EXPORT WCHAR *wcscat(WCHAR *d, const WCHAR *s) { wcscpy(d + wcslen(d), s); return d; }
EXPORT char *strchr(const char *s, int c) {
  for (;; ++s) {
    if (*s == (char)c) return (char *)s;
    if (!*s) return NULL;
  }
}
EXPORT char *strrchr(const char *s, int c) {
  const char *found = NULL;
  for (;; ++s) {
    if (*s == (char)c) found = s;
    if (!*s) return (char *)found;
  }
}
EXPORT WCHAR *wcschr(const WCHAR *s, WCHAR c) {
  for (;; ++s) {
    if (*s == c) return (WCHAR *)s;
    if (!*s) return NULL;
  }
}
EXPORT WCHAR *wcsrchr(const WCHAR *s, WCHAR c) {
  const WCHAR *found = NULL;
  for (;; ++s) {
    if (*s == c) found = s;
    if (!*s) return (WCHAR *)found;
  }
}
EXPORT char *strstr(const char *h, const char *n) {
  const size_t_ length = strlen(n);
  if (!length) return (char *)h;
  for (; *h; ++h) if (!strncmp(h, n, length)) return (char *)h;
  return NULL;
}
#ifdef OWN_TOWCASE
int toupper(int c);
int tolower(int c);
#else
EXPORT int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
EXPORT int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
#endif
#ifdef OWN_TOWCASE      /* (ntdll defines them from its case tables) */
int towupper(int c);
int towlower(int c);
#else
EXPORT int towupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
EXPORT int towlower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
#endif
#ifdef OWN_TOWCASE
int isdigit(int c);
#else
EXPORT int isdigit(int c) { return c >= '0' && c <= '9'; }
#endif
#ifdef OWN_TOWCASE
int isalpha(int c);
#else
EXPORT int isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
#endif
#ifdef OWN_TOWCASE
int isalnum(int c);
#else
EXPORT int isalnum(int c) { return isdigit(c) || isalpha(c); }
#endif
#ifdef OWN_TOWCASE
int isspace(int c);
#else
EXPORT int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
#endif
EXPORT int _stricmp(const char *a, const char *b) {
  for (;; ++a, ++b) {
    const int x = tolower((BYTE)*a), y = tolower((BYTE)*b);
    if (x != y) return x - y;
    if (!x) return 0;
  }
}
EXPORT int _strnicmp(const char *a, const char *b, size_t_ n) {
  for (; n; --n, ++a, ++b) {
    const int x = tolower((BYTE)*a), y = tolower((BYTE)*b);
    if (x != y) return x - y;
    if (!x) return 0;
  }
  return 0;
}
static int wide_fold(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
EXPORT int _wcsicmp(const WCHAR *a, const WCHAR *b) {
  for (;; ++a, ++b) {
    const int x = wide_fold(*a), y = wide_fold(*b);
    if (x != y) return x - y;
    if (!x) return 0;
  }
}
EXPORT int _wcsnicmp(const WCHAR *a, const WCHAR *b, size_t_ n) {
  for (; n; --n, ++a, ++b) {
    const int x = wide_fold(*a), y = wide_fold(*b);
    if (x != y) return x - y;
    if (!x) return 0;
  }
  return 0;
}
EXPORT unsigned long long _strtoui64(const char *s, char **end, int base) {
  while (isspace((BYTE)*s)) ++s;
  int negative = 0;
  if (*s == '-' || *s == '+') negative = *s++ == '-';
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x') { s += 2; base = 16; }
  else if (base == 0) base = s[0] == '0' ? 8 : 10;
  unsigned long long value = 0;
  for (;; ++s) {
    int digit;
    if (*s >= '0' && *s <= '9') digit = *s - '0';
    else if ((*s | 32) >= 'a' && (*s | 32) <= 'z') digit = (*s | 32) - 'a' + 10;
    else break;
    if (digit >= base) break;
    value = value * (unsigned)base + (unsigned)digit;
  }
  if (end) *end = (char *)s;
  return negative ? (unsigned long long)-(long long)value : value;
}
#ifndef LIBC_NO_STRTOL
EXPORT long strtol(const char *s, char **end, int base) { return (long)(int)_strtoui64(s, end, base); }
EXPORT unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)(unsigned int)_strtoui64(s, end, base); }
#endif
EXPORT int atoi(const char *s) { return (int)_strtoui64(s, NULL, 10); }

