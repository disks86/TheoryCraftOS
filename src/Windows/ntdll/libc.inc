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
#define CT_ALPHA 0x100
/* The classes (the C1 bits of GetStringTypeW) of the code units below 256; above that nothing is classified. */
static const unsigned short k_char_types[256] = {
  0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0068, 0x0028, 0x0028, 0x0028, 0x0028, 0x0020, 0x0020,
  0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
  0x0048, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010,
  0x0084, 0x0084, 0x0084, 0x0084, 0x0084, 0x0084, 0x0084, 0x0084, 0x0084, 0x0084, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010,
  0x0010, 0x0181, 0x0181, 0x0181, 0x0181, 0x0181, 0x0181, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101,
  0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010,
  0x0010, 0x0182, 0x0182, 0x0182, 0x0182, 0x0182, 0x0182, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102,
  0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0010, 0x0010, 0x0010, 0x0010, 0x0020,
  0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
  0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
  0x0048, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010,
  0x0010, 0x0010, 0x0014, 0x0014, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0014, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010, 0x0010,
  0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101,
  0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0010, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0102,
  0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102,
  0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0010, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102, 0x0102,
};
static unsigned ctype_of(int c) { return c >= 0 && c < 256 ? k_char_types[c] : 0; }
/* The narrow functions look at the first 128 only, and a tab is no blank for them. */
static unsigned narrow_type(int c) { return c >= 0 && c < 128 ? (c == 9 ? 0x28u : k_char_types[c]) : 0; }
EXPORT int isalnum(int c) { return (int)(narrow_type(c) & (CT_DIGIT | CT_LOWER | CT_UPPER)); }
EXPORT int isalpha(int c) { return (int)(narrow_type(c) & (CT_LOWER | CT_UPPER)); }
EXPORT int iscntrl(int c) { return (int)(narrow_type(c) & CT_CNTRL); }
EXPORT int isdigit(int c) { return (int)(narrow_type(c) & CT_DIGIT); }
EXPORT int isgraph(int c) { return (int)(narrow_type(c) & (CT_DIGIT | CT_PUNCT | CT_LOWER | CT_UPPER)); }
EXPORT int islower(int c) { return (int)(narrow_type(c) & CT_LOWER); }
EXPORT int isprint(int c) { return (int)(narrow_type(c) & (CT_DIGIT | CT_BLANK | CT_PUNCT | CT_LOWER | CT_UPPER)); }
EXPORT int ispunct(int c) { return (int)(narrow_type(c) & CT_PUNCT); }
EXPORT int isspace(int c) { return (int)(narrow_type(c) & CT_SPACE); }
EXPORT int isupper(int c) { return (int)(narrow_type(c) & CT_UPPER); }
EXPORT int isxdigit(int c) { return (int)(narrow_type(c) & CT_XDIGIT); }
/* towupper follows the full upcase table; towlower only the ASCII and Latin-1 letters, as ntdll does. */
EXPORT int towupper(int c) { return c < 0 || c > 0xFFFF ? c : (int)rtl_upcase((WCHAR)c); }
EXPORT int towlower(int c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE) ? c + 32 : c; }
EXPORT int tolower(int c) { return (char)c >= 'A' && (char)c <= 'Z' ? c + 32 : c; }      /* (it looks at the low byte only) */
/* toupper takes the two low bytes as an ANSI (UTF-8) character, upcases it and gives the bytes back (two of them as one number, first byte high); a result
   that needs more than two bytes leaves the argument as it is. */
WCHAR NTAPI RtlAnsiCharToUnicodeChar(PUCHAR *ansi);
EXPORT int toupper(int c) {
  UCHAR in[4] = {(UCHAR)c, (UCHAR)(c >> 8), 0, 0};
  PUCHAR cursor = in;
  const WCHAR up = rtl_upcase(RtlAnsiCharToUnicodeChar(&cursor));
  if (up < 0x80) return up;
  if (up < 0x800) return ((0xC0 | (up >> 6)) << 8) | (0x80 | (up & 0x3F));
  return (WCHAR)c;
}
EXPORT int iswalpha(int c) { return (int)(ctype_of(c) & (CT_ALPHA | CT_LOWER | CT_UPPER)); }
EXPORT int iswdigit(int c) { return (int)(ctype_of(c) & CT_DIGIT); }
EXPORT int iswlower(int c) { return (int)(ctype_of(c) & CT_LOWER); }
EXPORT int iswspace(int c) { return (int)(ctype_of(c) & CT_SPACE); }
EXPORT int iswxdigit(int c) { return (int)(ctype_of(c) & CT_XDIGIT); }
EXPORT int iswctype(int c, int mask) { return (int)(ctype_of(c) & (unsigned)mask); }

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
  while (*s == ' ' || *s == 0xA0 || (*s >= 9 && *s <= 13)) ++s;
  int neg = 0;
  if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
  unsigned v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
  return (int)(neg ? 0 - v : v);
}
EXPORT long _wtol(const WCHAR *s) { return (long)_wtoi(s); }
EXPORT LONGLONG _wtoi64(const WCHAR *s) {
  while (*s == ' ' || *s == 0xA0 || (*s >= 9 && *s <= 13)) ++s;
  int neg = 0;
  if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
  ULONGLONG v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (ULONGLONG)(*s++ - '0');
  return (LONGLONG)(neg ? 0 - v : v);
}
/* The value of a digit (ASCII letters and digits, and the ten digits of the blocks of other scripts) in a base, or -1. */
static int wide_digit(WCHAR c, int base) {
  static const WCHAR zeros[] = {0x0660, 0x06f0, 0x0966, 0x09e6, 0x0a66, 0x0ae6, 0x0b66, 0x0c66, 0x0ce6, 0x0d66, 0x0e50, 0x0ed0, 0x0f20, 0x1040, 0x17e0, 0x1810, 0xff10};
  if (c < 128) return rtl_digit(c, (ULONG)base);
  if (c < 128) return rtl_digit(c, (ULONG)base);
  for (unsigned i = 0; i < sizeof(zeros) / sizeof(zeros[0]); ++i) if (c >= zeros[i] && c < zeros[i] + 10) return c - zeros[i] < base ? (int)(c - zeros[i]) : -1;
  return -1;
}
/* strtol/wcstol with the C rules: spaces, a sign, base 0 prefixes; overflow clamps. */
static ULONGLONG wide_to_integer(const WCHAR *s, WCHAR **end, int base, int is_signed, int *neg_out) {
  const WCHAR *p = s;
  while ((*p >= 9 && *p <= 13) || *p == ' ' || *p == 0xA0) ++p;
  int neg = 0;
  if (*p == '-') { neg = 1; ++p; } else if (*p == '+') ++p;
  if ((base == 0 || base == 16) && wide_digit(p[0], 10) == 0 && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
  else if (base == 0 && wide_digit(p[0], 10) == 0) base = 8;
  else if (base == 0) base = 10;
  ULONGLONG v = 0;
  int any = 0, over = 0;
  for (;; ++p) {
    int d = wide_digit(*p, base);
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
  const unsigned long c = v > 0xFFFFFFFFull ? 0xFFFFFFFFul : (unsigned long)v;
  return neg ? (unsigned long)(0 - (unsigned)c) : c;
}
EXPORT WCHAR *_wcslwr(WCHAR *s) { for (WCHAR *p = s; *p; ++p) *p = (WCHAR)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p); return s; }
EXPORT WCHAR *_wcsupr(WCHAR *s) { for (WCHAR *p = s; *p; ++p) *p = (WCHAR)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p); return s; }
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
  do { const unsigned d = (unsigned)(v % (ULONGLONG)base); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);      /* (past 'z' the characters go on) */ v /= (ULONGLONG)base; } while (v);
  for (unsigned i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  out[n] = 0;
  return n;
}
static char *integer_to_text(LONGLONG v, char *out, int base, int is_signed) {
  if (base < 2 || base > 255) return out;      /* (an unusable base leaves the buffer alone) */
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

typedef int (*sort_compare)(const void *, const void *);
static void sort_swap(BYTE *a, BYTE *b, size_t_ size) { for (size_t_ k = 0; k < size; ++k) { const BYTE t = a[k]; a[k] = b[k]; b[k] = t; } }
static BYTE *sort_median(BYTE *a, BYTE *b, BYTE *c, sort_compare cmp) {
  return cmp(a, b) < 0 ? (cmp(b, c) < 0 ? b : (cmp(a, c) < 0 ? c : a)) : (cmp(b, c) > 0 ? b : (cmp(a, c) < 0 ? a : c));
}
static void sort_heap(void *base, size_t_ n, size_t_ size, sort_compare cmp);
/* The quicksort of the Windows C library (so that equal keys end up in the same order): a short sort up to 8 elements,
   a median of three (of nine from 40) as the pivot, equal keys gathered at the ends. Too deep a recursion falls back to a heap sort. */
static void sort_quick(BYTE *a, size_t_ n, size_t_ es, sort_compare cmp, int depth) {
  for (;;) {
    if (depth > 48) { sort_heap(a, n, es, cmp); return; }
    int swapped = 0;
    if (n <= 8) {      /* (the short sort of the Windows C library: the largest goes to the end, over and over) */
      for (BYTE *hi = a + (n - 1) * es; hi > a; hi -= es) {
        BYTE *max = a;
        for (BYTE *q = a + es; q <= hi; q += es) if (cmp(q, max) > 0) max = q;
        sort_swap(max, hi, es);
      }
      return;
    }
    BYTE *pm = a + (n / 2) * es;
    if (n > 7) {
      BYTE *pl = a, *pn = a + (n - 1) * es;
      if (n > 40) {
        const size_t_ d = (n / 8) * es;
        pl = sort_median(pl, pl + d, pl + 2 * d, cmp);
        pm = sort_median(pm - d, pm, pm + d, cmp);
        pn = sort_median(pn - 2 * d, pn - d, pn, cmp);
      }
      pm = sort_median(pl, pm, pn, cmp);
    }
    sort_swap(a, pm, es);
    BYTE *pa = a + es, *pb = pa, *pc = a + (n - 1) * es, *pd = pc;
    for (;;) {
      int r;
      while (pb <= pc && (r = cmp(pb, a)) <= 0) { if (r == 0) { swapped = 1; sort_swap(pa, pb, es); pa += es; } pb += es; }
      while (pb <= pc && (r = cmp(pc, a)) >= 0) { if (r == 0) { swapped = 1; sort_swap(pc, pd, es); pd -= es; } pc -= es; }
      if (pb > pc) break;
      sort_swap(pb, pc, es);
      swapped = 1;
      pb += es; pc -= es;
    }
    if (!swapped) {
      for (BYTE *q = a + es; q < a + n * es; q += es)
        for (BYTE *pl = q; pl > a && cmp(pl - es, pl) > 0; pl -= es) sort_swap(pl, pl - es, es);
      return;
    }
    BYTE *pn = a + n * es;
    size_t_ r = (size_t_)(pa - a) < (size_t_)(pb - pa) ? (size_t_)(pa - a) : (size_t_)(pb - pa);
    for (size_t_ x = 0; x < r; x += es) sort_swap(a + x, pb - r + x, es);
    r = (size_t_)(pd - pc) < (size_t_)(pn - pd) - es ? (size_t_)(pd - pc) : (size_t_)(pn - pd) - es;
    for (size_t_ x = 0; x < r; x += es) sort_swap(pb + x, pn - r + x, es);
    if ((r = (size_t_)(pb - pa)) > es) sort_quick(a, r / es, es, cmp, depth + 1);
    if ((r = (size_t_)(pd - pc)) > es) { a = pn - r; n = r / es; ++depth; continue; }
    return;
  }
}
EXPORT void qsort(void *base, size_t_ n, size_t_ size, sort_compare cmp) {
  if (n >= 2 && size) sort_quick(base, n, size, cmp, 0);
}
static void sort_heap(void *base, size_t_ n, size_t_ size, sort_compare cmp) {
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
  va_list_ a, b;
  __builtin_va_start(a, format);
  __builtin_va_copy(b, a);
  const int measured = _vsnprintf(NULL, 0, format, b);
  __builtin_va_end(b);
  int result = -1;
  size_t_ keep = 0;      /* (characters to keep before the NUL) */
  if (!size) { __builtin_va_end(a); return -1; }
  const size_t_ len = measured < 0 ? 0 : (size_t_)measured;
  if (count == (size_t_)-1) { keep = len < size ? len : size - 1; result = len < size ? (int)len : -1; }      /* _TRUNCATE */
  else if (len <= count) { if (len < size) { keep = len; result = (int)len; } }
  else if (count < size) keep = count;
  if (keep) _vsnprintf(buffer, keep, format, a);
  __builtin_va_end(a);
  buffer[keep] = 0;
  return result;
}
EXPORT int _snwprintf_s(WCHAR *buffer, size_t_ size, size_t_ count, const WCHAR *format, ...) {
  va_list_ a, b;
  __builtin_va_start(a, format);
  __builtin_va_copy(b, a);
  const int measured = _vsnwprintf(NULL, 0, format, b);
  __builtin_va_end(b);
  int result = -1;
  size_t_ keep = 0;      /* (characters to keep before the NUL) */
  if (!size) { __builtin_va_end(a); return -1; }
  const size_t_ len = measured < 0 ? 0 : (size_t_)measured;
  if (count == (size_t_)-1) { keep = len < size ? len : size - 1; result = len < size ? (int)len : -1; }      /* _TRUNCATE */
  else if (len <= count) { if (len < size) { keep = len; result = (int)len; } }
  else if (count < size) keep = count;
  if (keep) _vsnwprintf(buffer, keep, format, a);
  __builtin_va_end(a);
  buffer[keep] = 0;
  return result;
}

/* ---- sscanf (no floating point, as in the real ntdll) -------------------------------------------------------------------------------------------------- */

static int scan_is_space(int c) { return c == ' ' || (c >= 9 && c <= 13); }
EXPORT int sscanf(const char *input, const char *format, ...) {
  va_list_ args;
  __builtin_va_start(args, format);
  const char *in = input;
  int assigned = 0, failed = 0;
  for (const char *f = format; *f && !failed; ++f) {
    if (scan_is_space((BYTE)*f)) { while (scan_is_space((BYTE)*in)) ++in; continue; }
    if (*f != '%') { if (*in != *f) break; ++in; continue; }
    ++f;
    int suppress = 0, width = 0, size = 0;
    if (*f == '*') { suppress = 1; ++f; }
    while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    for (;; ++f) {
      if (*f == 'h') size = size == 1 ? 2 : 1;
      else if (*f == 'l' || *f == 'L') size = 4;
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') { size = 8; f += 2; }
      else break;
    }
    const char conversion = *f;
    if (!conversion) break;
    if (conversion == '%') { while (scan_is_space((BYTE)*in)) ++in; if (*in != '%') break; ++in; continue; }
    if (conversion == 'n') { if (!suppress) { int *p = __builtin_va_arg(args, int *); *p = (int)(in - input); } continue; }
    if (conversion != 'c' && conversion != '[') while (scan_is_space((BYTE)*in)) ++in;
    if (!*in) { if (!assigned) assigned = -1; break; }
    const int limit = width ? width : 0x7FFFFFFF;
    if (conversion == 'd' || conversion == 'i' || conversion == 'u' || conversion == 'x' || conversion == 'X' || conversion == 'o') {
      int base = conversion == 'x' || conversion == 'X' ? 16 : conversion == 'o' ? 8 : 10;
      int neg = 0, used = 0, any = 0;
      if (*in == '-' || *in == '+') { neg = *in == '-'; ++in; ++used; }
      if (conversion == 'i' || base == 16) {
        if (in[0] == '0' && (in[1] == 'x' || in[1] == 'X') && used + 2 <= limit) { in += 2; used += 2; base = 16; }
        else if (conversion == 'i' && in[0] == '0') base = 8;
      }
      ULONGLONG v = 0;
      for (; used < limit; ++in, ++used) {
        const int d = rtl_digit((BYTE)*in, (ULONG)base);
        if (d < 0) break;
        v = v * (ULONGLONG)base + (ULONGLONG)d;
        any = 1;
      }
      if (!any) break;
      if (neg) v = 0 - v;
      if (!suppress) {
        void *p = __builtin_va_arg(args, void *);
        if (size == 8) *(ULONGLONG *)p = v;
        else if (size == 1) *(USHORT *)p = (USHORT)v;
        else if (size == 2) *(BYTE *)p = (BYTE)v;
        else *(ULONG *)p = (ULONG)v;
        ++assigned;
      }
    } else if (conversion == 's' || conversion == 'c' || conversion == '[') {
      const char *set = NULL;
      int invert = 0;
      size_t_ set_length = 0;
      if (conversion == '[') {
        ++f;
        if (*f == '^') { invert = 1; ++f; }
        set = f;
        if (*f == ']') ++f;
        while (*f && *f != ']') ++f;
        set_length = (size_t_)(f - set);
      }
      char *out = suppress ? NULL : __builtin_va_arg(args, char *);
      int n = 0;
      const int count = conversion == 'c' ? (width ? width : 1) : limit;
      for (; n < count && *in; ++in, ++n) {
        if (conversion == 's' && scan_is_space((BYTE)*in)) break;
        if (conversion == '[') {
          int hit = 0;
          for (size_t_ k = 0; k < set_length; ++k) {
            if (k + 2 < set_length && set[k + 1] == '-') { if (*in >= set[k] && *in <= set[k + 2]) hit = 1; k += 2; }
            else if (*in == set[k]) hit = 1;
          }
          if (hit == invert) break;
        }
        if (out) out[n] = *in;
      }
      if (!n) break;
      if (out) { if (conversion != 'c') out[n] = 0; ++assigned; }
    } else break;      /* (floating point and the rest are not scanned) */
  }
  __builtin_va_end(args);
  return assigned;
}
