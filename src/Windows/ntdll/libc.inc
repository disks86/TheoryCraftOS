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

/* ntdll.dll: more of the C library functions that the real ntdll exports (character classes, number <-> string conversions, qsort and bsearch). Included by ntdll.c. */

#define CT_UPPER 1
#define CT_LOWER 2
#define CT_DIGIT 4
#define CT_SPACE 8
#define CT_PUNCT 0x10
#define CT_CNTRL 0x20
#define CT_BLANK 0x40
#define CT_XDIGIT 0x80
/* The C-locale classes of a code unit below 256 (above it, only the letters a wide function knows about). */
static unsigned ctype_of(int c) {
  unsigned t = 0;
  if (c >= 'A' && c <= 'Z') t |= CT_UPPER | (c <= 'F' ? CT_XDIGIT : 0);
  if (c >= 'a' && c <= 'z') t |= CT_LOWER | (c <= 'f' ? CT_XDIGIT : 0);
  if (c >= '0' && c <= '9') t |= CT_DIGIT | CT_XDIGIT;
  if ((c >= 9 && c <= 13) || c == ' ') t |= CT_SPACE;
  if (c == ' ' || c == '\t') t |= CT_BLANK;
  if ((c >= 0 && c < 32) || c == 127) t |= CT_CNTRL;
  if ((c > 32 && c < 127) && !(t & (CT_UPPER | CT_LOWER | CT_DIGIT))) t |= CT_PUNCT;
  return t;
}
static int in_byte_range(int c) { return c >= 0 && c < 256; }
EXPORT int iscntrl(int c) { return in_byte_range(c) && (ctype_of(c) & CT_CNTRL); }
EXPORT int isgraph(int c) { return in_byte_range(c) && c > 32 && c < 127; }
EXPORT int islower(int c) { return in_byte_range(c) && (ctype_of(c) & CT_LOWER); }
EXPORT int isupper(int c) { return in_byte_range(c) && (ctype_of(c) & CT_UPPER); }
EXPORT int isprint(int c) { return in_byte_range(c) && c >= 32 && c < 127; }
EXPORT int ispunct(int c) { return in_byte_range(c) && (ctype_of(c) & CT_PUNCT); }
EXPORT int isxdigit(int c) { return in_byte_range(c) && (ctype_of(c) & CT_XDIGIT); }
EXPORT int iswalpha(int c) { return (c >= 0 && c < 128 && (ctype_of(c) & (CT_UPPER | CT_LOWER))) || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x52F); }
EXPORT int iswdigit(int c) { return c >= '0' && c <= '9'; }
EXPORT int iswlower(int c) { return c >= 0 && c < 128 ? !!(ctype_of(c) & CT_LOWER) : (rtl_downcase((WCHAR)c) == c && rtl_upcase((WCHAR)c) != c); }
EXPORT int iswspace(int c) { return (c >= 9 && c <= 13) || c == ' ' || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A); }
EXPORT int iswxdigit(int c) { return c >= 0 && c < 128 && (ctype_of(c) & CT_XDIGIT); }
EXPORT int iswctype(int c, int mask) {
  if (c < 0 || c > 0xFFFF) return 0;
  int r = 0;
  if ((mask & 1) && c < 128 && (ctype_of(c) & CT_UPPER)) r = 1;      /* _UPPER */
  if ((mask & 2) && iswlower(c)) r = 1;
  if ((mask & 4) && iswdigit(c)) r = 1;
  if ((mask & 8) && iswspace(c)) r = 1;
  if ((mask & 0x10) && c < 128 && (ctype_of(c) & CT_PUNCT)) r = 1;
  if ((mask & 0x20) && c < 128 && (ctype_of(c) & CT_CNTRL)) r = 1;
  if ((mask & 0x80) && iswxdigit(c)) r = 1;
  if ((mask & 0x100) && iswalpha(c)) r = 1;
  return r;
}

EXPORT long atol(const char *s) { return (long)atoi(s); }
EXPORT LONGLONG _atoi64(const char *s) {
  while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s;
  int neg = 0;
  if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
  ULONGLONG v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (ULONGLONG)(*s++ - '0');
  return (LONGLONG)(neg ? 0 - v : v);
}
EXPORT int _wtoi(const WCHAR *s) {
  while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s;
  int neg = 0;
  if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
  unsigned v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
  return (int)(neg ? 0 - v : v);
}
EXPORT long _wtol(const WCHAR *s) { return (long)_wtoi(s); }
EXPORT LONGLONG _wtoi64(const WCHAR *s) {
  while (*s == ' ' || (*s >= 9 && *s <= 13)) ++s;
  int neg = 0;
  if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
  ULONGLONG v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (ULONGLONG)(*s++ - '0');
  return (LONGLONG)(neg ? 0 - v : v);
}
/* strtol/wcstol with the C rules: spaces, a sign, base 0 prefixes; overflow clamps. */
static ULONGLONG wide_to_integer(const WCHAR *s, WCHAR **end, int base, int is_signed, int *neg_out) {
  const WCHAR *p = s;
  while (iswspace(*p)) ++p;
  int neg = 0;
  if (*p == '-') { neg = 1; ++p; } else if (*p == '+') ++p;
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
  else if (base == 0 && p[0] == '0') base = 8;
  else if (base == 0) base = 10;
  ULONGLONG v = 0;
  int any = 0, over = 0;
  for (;; ++p) {
    int d = *p < 128 ? rtl_digit(*p, (ULONG)base) : -1;
    if (d < 0) break;
    any = 1;
    if (v > (~0ull - (ULONGLONG)d) / (ULONGLONG)base) over = 1; else v = v * (ULONGLONG)base + (ULONGLONG)d;
  }
  if (end) *end = (WCHAR *)(any ? p : s);
  if (neg_out) *neg_out = neg;
  (void)is_signed; (void)over;
  return over ? ~0ull : v;
}
EXPORT long wcstol(const WCHAR *s, WCHAR **end, int base) {
  int neg;
  const ULONGLONG v = wide_to_integer(s, end, base, 1, &neg);
  if (neg) return v > 0x80000000ull ? (long)0x80000000l : (long)(0 - (unsigned)v);
  return v > 0x7FFFFFFFull ? 0x7FFFFFFF : (long)v;
}
EXPORT unsigned long wcstoul(const WCHAR *s, WCHAR **end, int base) {
  int neg;
  const ULONGLONG v = wide_to_integer(s, end, base, 0, &neg);
  if (v > 0xFFFFFFFFull) return 0xFFFFFFFFul;
  return neg ? (unsigned long)(0 - (unsigned)v) : (unsigned long)v;
}
EXPORT WCHAR *_wcslwr(WCHAR *s) { for (WCHAR *p = s; *p; ++p) *p = rtl_downcase(*p); return s; }
EXPORT WCHAR *_wcsupr(WCHAR *s) { for (WCHAR *p = s; *p; ++p) *p = rtl_upcase(*p); return s; }
EXPORT WCHAR *wcsncpy(WCHAR *d, const WCHAR *s, size_t_ n) {
  size_t_ i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}

/* number -> string in a base (2..36); the buffers are the caller's */
static unsigned format_integer(ULONGLONG v, int base, char *out) {
  char tmp[72];
  unsigned n = 0;
  do { const unsigned d = (unsigned)(v % (ULONGLONG)base); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (ULONGLONG)base; } while (v);
  for (unsigned i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  out[n] = 0;
  return n;
}
static char *integer_to_text(LONGLONG v, char *out, int base, int is_signed) {
  if (base < 2 || base > 36) return out;      /* (an unusable base leaves the buffer alone) */
  if (is_signed && base == 10 && v < 0) { out[0] = '-'; format_integer(0 - (ULONGLONG)v, base, out + 1); }
  else format_integer(is_signed ? (ULONGLONG)(ULONG)(LONG)v == (ULONGLONG)v ? (ULONGLONG)(ULONG)v : (ULONGLONG)v : (ULONGLONG)v, base, out);
  return out;
}
EXPORT char *_itoa(int v, char *out, int base) { return integer_to_text(base == 10 ? (LONGLONG)v : (LONGLONG)(unsigned)v, out, base, base == 10); }
EXPORT char *_ltoa(long v, char *out, int base) { return integer_to_text(base == 10 ? (LONGLONG)v : (LONGLONG)(unsigned)v, out, base, base == 10); }
EXPORT char *_ultoa(unsigned long v, char *out, int base) { return integer_to_text((LONGLONG)(ULONGLONG)v, out, base, 0); }
EXPORT char *_i64toa(LONGLONG v, char *out, int base) { return integer_to_text(v, out, base, base == 10); }
EXPORT char *_ui64toa(ULONGLONG v, char *out, int base) { return integer_to_text((LONGLONG)v, out, base, 0); }
static WCHAR *widen_text(char *narrow, WCHAR *out) { if (!narrow[0]) return out; WCHAR *p = out; for (const char *q = narrow; (*p++ = (WCHAR)(BYTE)*q); ++q) {} return out; }
EXPORT WCHAR *_itow(int v, WCHAR *out, int base) { char t[80]; t[0] = 0; _itoa(v, t, base); return widen_text(t, out); }
EXPORT WCHAR *_ltow(long v, WCHAR *out, int base) { char t[80]; t[0] = 0; _ltoa(v, t, base); return widen_text(t, out); }
EXPORT WCHAR *_ultow(unsigned long v, WCHAR *out, int base) { char t[80]; t[0] = 0; _ultoa(v, t, base); return widen_text(t, out); }
EXPORT WCHAR *_i64tow(LONGLONG v, WCHAR *out, int base) { char t[80]; t[0] = 0; _i64toa(v, t, base); return widen_text(t, out); }
EXPORT WCHAR *_ui64tow(ULONGLONG v, WCHAR *out, int base) { char t[80]; t[0] = 0; _ui64toa(v, t, base); return widen_text(t, out); }

EXPORT void qsort(void *base, size_t_ n, size_t_ size, int (*cmp)(const void *, const void *)) {
  /* A heap sort: no recursion, no allocation, O(n log n) in the worst case. */
  BYTE *a = base;
  if (n < 2 || !size) return;
  for (size_t_ start = n / 2; start-- > 0;) {
    size_t_ root = start;
    for (;;) {
      size_t_ child = 2 * root + 1;
      if (child >= n) break;
      if (child + 1 < n && cmp(a + child * size, a + (child + 1) * size) < 0) ++child;
      if (cmp(a + root * size, a + child * size) >= 0) break;
      for (size_t_ k = 0; k < size; ++k) { const BYTE t = a[root * size + k]; a[root * size + k] = a[child * size + k]; a[child * size + k] = t; }
      root = child;
    }
  }
  for (size_t_ end = n - 1; end > 0; --end) {
    for (size_t_ k = 0; k < size; ++k) { const BYTE t = a[k]; a[k] = a[end * size + k]; a[end * size + k] = t; }
    size_t_ root = 0;
    for (;;) {
      size_t_ child = 2 * root + 1;
      if (child >= end) break;
      if (child + 1 < end && cmp(a + child * size, a + (child + 1) * size) < 0) ++child;
      if (cmp(a + root * size, a + child * size) >= 0) break;
      for (size_t_ k = 0; k < size; ++k) { const BYTE t = a[root * size + k]; a[root * size + k] = a[child * size + k]; a[child * size + k] = t; }
      root = child;
    }
  }
}
EXPORT void *bsearch(const void *key, const void *base, size_t_ n, size_t_ size, int (*cmp)(const void *, const void *)) {
  const BYTE *a = base;
  size_t_ lo = 0, hi = n;
  while (lo < hi) {
    const size_t_ mid = lo + (hi - lo) / 2;
    const int c = cmp(key, a + mid * size);
    if (c == 0) return (void *)(a + mid * size);
    if (c < 0) hi = mid; else lo = mid + 1;
  }
  return NULL;
}
EXPORT int _snprintf_s(char *buffer, size_t_ size, size_t_ count, const char *format, ...) {
  va_list_ a;
  __builtin_va_start(a, format);
  const size_t_ limit = count < size ? count : size - 1;
  const int r = vsnprintf(buffer, limit + 1, format, a);
  __builtin_va_end(a);
  if (size) buffer[(size_t_)r < limit ? (size_t_)r : limit] = 0;
  return r;
}
