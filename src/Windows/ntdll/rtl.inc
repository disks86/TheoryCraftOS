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

/* ntdll.dll: the Rtl* string, integer, bitmap and GUID functions. Written from the documented behaviour of the Windows functions and checked by Wine's tests
 * (tests/windows/wine/ntdll: rtlstr, rtlbitmap, large_int, string). Included by ntdll.c. */

typedef signed char CCHAR_;
#define STATUS_BUFFER_TOO_SMALL_ 0xC0000023u
#define STATUS_NAME_TOO_LONG_ 0xC0000106u
#define STATUS_INVALID_PARAMETER_ 0xC000000Du
#define STATUS_INVALID_PARAMETER_1_ 0xC00000EFu
#define STATUS_INVALID_PARAMETER_2_ 0xC00000F0u
#define STATUS_INVALID_PARAMETER_MIX_ 0xC0000030u
#define STATUS_NO_MEMORY_ 0xC0000017u
#define STATUS_BUFFER_OVERFLOW_ 0x80000005u
#define STATUS_UNSUCCESSFUL_ 0xC0000001u
#define STATUS_ILLEGAL_CHARACTER_ 0xC0000161u
#define STATUS_UNMAPPABLE_CHARACTER_ 0xC0000162u
#define STATUS_INTEGER_OVERFLOW_ 0xC0000095u
#define STATUS_SOME_NOT_MAPPED_ 0x00000107u
#define STATUS_ACCESS_VIOLATION_ 0xC0000005u

/* ---- strings ------------------------------------------------------------------------------------------------------------------------------------------- */

#include "casetab.inc"
/* The mapping of a character in one of the range tables (binary search on the first character of each range). */
static WCHAR rtl_case_map(WCHAR c, const CaseRange *table, int count) {
  if (c < 0x80) {
    if (table == (const void *)k_upcase_ranges) return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c;
    return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c;
  }
  int low = 0, high = count - 1;
  while (low <= high) {
    const int mid = (low + high) / 2;
    if (c < table[mid].first) high = mid - 1;
    else {
      const unsigned offset = (unsigned)c - table[mid].first;
      if (offset % table[mid].step == 0 && offset / table[mid].step < table[mid].count) return (WCHAR)(c + table[mid].delta);
      if (offset < (unsigned)table[mid].count * table[mid].step) return c;
      low = mid + 1;
    }
  }
  return c;
}
static WCHAR rtl_upcase(WCHAR c) { return rtl_case_map(c, k_upcase_ranges, (int)(sizeof(k_upcase_ranges) / sizeof(k_upcase_ranges[0]))); }
static WCHAR rtl_downcase(WCHAR c) { return rtl_case_map(c, k_downcase_ranges, (int)(sizeof(k_downcase_ranges) / sizeof(k_downcase_ranges[0]))); }
EXPORT WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR c) { return rtl_upcase(c); }
EXPORT WCHAR NTAPI RtlDowncaseUnicodeChar(WCHAR c) { return rtl_downcase(c); }
EXPORT CHAR NTAPI RtlUpperChar(CHAR c) { return c >= 'a' && c <= 'z' ? (CHAR)(c - 32) : c; }
EXPORT CHAR NTAPI RtlLowerChar(CHAR c) { return c >= 'A' && c <= 'Z' ? (CHAR)(c + 32) : c; }

EXPORT void NTAPI RtlInitString(PANSI_STRING s, PCSTR text) { RtlInitAnsiString(s, text); }
EXPORT NTSTATUS NTAPI RtlInitUnicodeStringEx(PUNICODE_STRING s, PCWSTR text) {
  if (text && wcslen(text) * 2 > 0xFFFC) return STATUS_NAME_TOO_LONG_;      /* (the string is left as it was) */
  RtlInitUnicodeString(s, text);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlInitAnsiStringEx(PANSI_STRING s, PCSTR text) {
  if (text && strlen(text) > 0xFFFE) return STATUS_NAME_TOO_LONG_;
  RtlInitAnsiString(s, text);
  return STATUS_SUCCESS;
}
static PVOID rtl_alloc(SIZE_T n) { return RtlAllocateHeap(NtCurrentPeb()->ProcessHeap, 0, n); }
EXPORT void NTAPI RtlFreeUnicodeString(PUNICODE_STRING s) {
  if (s->Buffer) RtlFreeHeap(NtCurrentPeb()->ProcessHeap, 0, s->Buffer);
  s->Length = s->MaximumLength = 0;
  s->Buffer = NULL;
}
EXPORT void NTAPI RtlFreeAnsiString(PANSI_STRING s) {
  if (s->Buffer) RtlFreeHeap(NtCurrentPeb()->ProcessHeap, 0, s->Buffer);
  s->Length = s->MaximumLength = 0;
  s->Buffer = NULL;
}
EXPORT void NTAPI RtlFreeOemString(PANSI_STRING s) { RtlFreeAnsiString(s); }

EXPORT void NTAPI RtlCopyString(PANSI_STRING dst, const ANSI_STRING *src) {
  if (!src) { dst->Length = 0; return; }
  const USHORT n = src->Length < dst->MaximumLength ? src->Length : dst->MaximumLength;
  memcpy(dst->Buffer, src->Buffer, n);
  dst->Length = n;
}
EXPORT void NTAPI RtlCopyUnicodeString(PUNICODE_STRING dst, const UNICODE_STRING *src) {
  if (!src) { dst->Length = 0; return; }
  USHORT n = src->Length < dst->MaximumLength ? src->Length : dst->MaximumLength;
  n &= ~1u;
  memcpy(dst->Buffer, src->Buffer, n);
  dst->Length = n;
  if (n + 1u < dst->MaximumLength) dst->Buffer[n / 2] = 0;
}
EXPORT NTSTATUS NTAPI RtlAppendStringToString(PANSI_STRING dst, const ANSI_STRING *src) {
  if (src->Length) {
    if ((ULONG)dst->Length + src->Length > dst->MaximumLength) return STATUS_BUFFER_TOO_SMALL_;
    memcpy(dst->Buffer + dst->Length, src->Buffer, src->Length);
    dst->Length = (USHORT)(dst->Length + src->Length);
  }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAppendAsciizToString(PANSI_STRING dst, PCSTR text) {
  if (text) {
    const size_t_ n = strlen(text);
    if (n + dst->Length > dst->MaximumLength) return STATUS_BUFFER_TOO_SMALL_;
    memcpy(dst->Buffer + dst->Length, text, n);
    dst->Length = (USHORT)(dst->Length + n);
  }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAppendUnicodeStringToString(PUNICODE_STRING dst, const UNICODE_STRING *src) {
  if (src->Length) {
    if ((ULONG)dst->Length + src->Length > dst->MaximumLength) return STATUS_BUFFER_TOO_SMALL_;
    memcpy((char *)dst->Buffer + dst->Length, src->Buffer, src->Length);
    dst->Length = (USHORT)(dst->Length + src->Length);
    if ((ULONG)dst->Length + 2 <= dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
  }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAppendUnicodeToString(PUNICODE_STRING dst, PCWSTR text) {
  if (text) {
    const size_t_ n = wcslen(text) * 2;
    if (n + dst->Length > dst->MaximumLength) return STATUS_BUFFER_TOO_SMALL_;
    memcpy((char *)dst->Buffer + dst->Length, text, n);
    dst->Length = (USHORT)(dst->Length + n);
    if ((ULONG)dst->Length + 2 <= dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
  }
  return STATUS_SUCCESS;
}

EXPORT LONG NTAPI RtlCompareUnicodeStrings(PCWSTR a, SIZE_T alen, PCWSTR b, SIZE_T blen, BOOLEAN ci) {
  const SIZE_T n = alen < blen ? alen : blen;
  for (SIZE_T i = 0; i < n; ++i) {
    WCHAR x = a[i], y = b[i];
    if (ci) { x = rtl_upcase(x); y = rtl_upcase(y); }
    if (x != y) return (LONG)x - (LONG)y;
  }
  return (LONG)alen - (LONG)blen;
}
EXPORT LONG NTAPI RtlCompareUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, BOOLEAN ci) {
  const LONG r = RtlCompareUnicodeStrings(a->Buffer, a->Length / 2, b->Buffer, b->Length / 2, ci);
  return r;
}
EXPORT LONG NTAPI RtlCompareString(const ANSI_STRING *a, const ANSI_STRING *b, BOOLEAN ci) {
  const unsigned n = a->Length < b->Length ? a->Length : b->Length;
  for (unsigned i = 0; i < n; ++i) {
    int x = (BYTE)a->Buffer[i], y = (BYTE)b->Buffer[i];
    if (ci) { x = (BYTE)RtlUpperChar((CHAR)x); y = (BYTE)RtlUpperChar((CHAR)y); }
    if (x != y) return x - y;
  }
  return (LONG)a->Length - (LONG)b->Length;
}
EXPORT BOOLEAN NTAPI RtlEqualString(const ANSI_STRING *a, const ANSI_STRING *b, BOOLEAN ci) { return a->Length == b->Length && RtlCompareString(a, b, ci) == 0; }
EXPORT BOOLEAN NTAPI RtlEqualUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, BOOLEAN ci) { return a->Length == b->Length && RtlCompareUnicodeString(a, b, ci) == 0; }
EXPORT BOOLEAN NTAPI RtlPrefixString(const ANSI_STRING *p, const ANSI_STRING *s, BOOLEAN ci) {
  if (p->Length > s->Length) return FALSE;
  ANSI_STRING t = *s;
  t.Length = p->Length;
  return RtlCompareString(p, &t, ci) == 0;
}
EXPORT BOOLEAN NTAPI RtlPrefixUnicodeString(const UNICODE_STRING *p, const UNICODE_STRING *s, BOOLEAN ci) {
  if (p->Length > s->Length) return FALSE;
  UNICODE_STRING t = *s;
  t.Length = p->Length;
  return RtlCompareUnicodeString(p, &t, ci) == 0;
}
EXPORT void NTAPI RtlUpperString(PANSI_STRING dst, const ANSI_STRING *src) {
  const unsigned n = src->Length < dst->MaximumLength ? src->Length : dst->MaximumLength;
  for (unsigned i = 0; i < n; ++i) dst->Buffer[i] = RtlUpperChar(src->Buffer[i]);
  dst->Length = (USHORT)n;
}
EXPORT void NTAPI RtlLowerString(PANSI_STRING dst, const ANSI_STRING *src) {
  const unsigned n = src->Length < dst->MaximumLength ? src->Length : dst->MaximumLength;
  for (unsigned i = 0; i < n; ++i) dst->Buffer[i] = RtlLowerChar(src->Buffer[i]);
  dst->Length = (USHORT)n;
}

/* The result of a conversion into a string the caller (alloc = FALSE) or the function (alloc = TRUE) provides memory for. */
static NTSTATUS rtl_dest_unicode(PUNICODE_STRING dst, ULONG bytes, BOOLEAN alloc) {
  if (bytes > 0xFFFE) return STATUS_INVALID_PARAMETER_;      /* (the length does not fit the string) */
  if (alloc) {
    dst->Buffer = rtl_alloc(bytes + 2);
    if (!dst->Buffer) return STATUS_NO_MEMORY_;
    dst->MaximumLength = (USHORT)(bytes + 2);
  } else if (bytes > dst->MaximumLength) {
    return STATUS_BUFFER_OVERFLOW_;
  }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAnsiStringToUnicodeString(PUNICODE_STRING dst, const ANSI_STRING *src, BOOLEAN alloc) {
  ULONG bytes;
  RtlMultiByteToUnicodeSize(&bytes, src->Buffer, src->Length);
  NTSTATUS st = rtl_dest_unicode(dst, bytes, alloc);
  if ((ULONG)st == STATUS_BUFFER_OVERFLOW_) {      /* converts what fits */
    RtlMultiByteToUnicodeN(dst->Buffer, dst->MaximumLength & ~1u, &bytes, src->Buffer, src->Length);
    dst->Length = (USHORT)bytes;
    return st;
  }
  if (st) return st;
  RtlMultiByteToUnicodeN(dst->Buffer, bytes, &bytes, src->Buffer, src->Length);
  dst->Length = (USHORT)bytes;
  if ((ULONG)dst->Length + 2 <= dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlOemStringToUnicodeString(PUNICODE_STRING dst, const ANSI_STRING *src, BOOLEAN alloc) { return RtlAnsiStringToUnicodeString(dst, src, alloc); }
EXPORT NTSTATUS NTAPI RtlUnicodeStringToAnsiString(PANSI_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) {
  ULONG bytes;
  RtlUnicodeToMultiByteSize(&bytes, src->Buffer, src->Length);
  if (bytes > 0xFFFE) return STATUS_INVALID_PARAMETER_;
  if (alloc) {
    dst->Buffer = rtl_alloc(bytes + 1);
    if (!dst->Buffer) return STATUS_NO_MEMORY_;
    dst->MaximumLength = (USHORT)(bytes + 1);
  } else if (bytes >= dst->MaximumLength) {
    if (!dst->MaximumLength) return STATUS_BUFFER_OVERFLOW_;
    RtlUnicodeToMultiByteN(dst->Buffer, dst->MaximumLength - 1u, &bytes, src->Buffer, src->Length);
    dst->Length = (USHORT)bytes;
    dst->Buffer[bytes] = 0;
    return STATUS_BUFFER_OVERFLOW_;
  }
  RtlUnicodeToMultiByteN(dst->Buffer, bytes, &bytes, src->Buffer, src->Length);
  dst->Length = (USHORT)bytes;
  dst->Buffer[bytes] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlUnicodeStringToOemString(PANSI_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) { return RtlUnicodeStringToAnsiString(dst, src, alloc); }
EXPORT NTSTATUS NTAPI RtlUpcaseUnicodeString(PUNICODE_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) {
  const ULONG bytes = src->Length & ~1u;
  NTSTATUS st = rtl_dest_unicode(dst, bytes, alloc);
  if (st) return st;
  for (ULONG i = 0; i < bytes / 2; ++i) dst->Buffer[i] = rtl_upcase(src->Buffer[i]);
  dst->Length = (USHORT)bytes;
  if (bytes + 2u <= dst->MaximumLength) dst->Buffer[bytes / 2] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDowncaseUnicodeString(PUNICODE_STRING dst, const UNICODE_STRING *src, BOOLEAN alloc) {
  const ULONG bytes = src->Length & ~1u;
  NTSTATUS st = rtl_dest_unicode(dst, bytes, alloc);
  if (st) return st;
  for (ULONG i = 0; i < bytes / 2; ++i) dst->Buffer[i] = rtl_downcase(src->Buffer[i]);
  dst->Length = (USHORT)bytes;
  if (bytes + 2u <= dst->MaximumLength) dst->Buffer[bytes / 2] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDuplicateUnicodeString(ULONG flags, const UNICODE_STRING *src, PUNICODE_STRING dst) {
  /* flags: 1 = end the copy with a NUL, 2 = (only with 1) make a buffer even for an empty string */
  if (!src || !dst || (flags & ~3u) || flags == 2) return STATUS_INVALID_PARAMETER_;
  if ((!src->Buffer && (src->Length || src->MaximumLength)) || src->Length > src->MaximumLength) return STATUS_INVALID_PARAMETER_;
  if (src->Length == 0 && !(flags & 2)) { dst->Length = dst->MaximumLength = 0; dst->Buffer = NULL; return STATUS_SUCCESS; }
  const ULONG size = src->Length + ((flags & 1) ? 2u : 0u);
  if (size > 0xFFFE) return STATUS_NAME_TOO_LONG_;
  WCHAR *buffer = rtl_alloc(size ? size : 2);
  if (!buffer) return STATUS_NO_MEMORY_;
  if (src->Length) memcpy(buffer, src->Buffer, src->Length);
  if (flags & 1) buffer[src->Length / 2] = 0;
  dst->Buffer = buffer;
  dst->Length = src->Length;
  dst->MaximumLength = (USHORT)size;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlFindCharInUnicodeString(ULONG flags, const UNICODE_STRING *s, const UNICODE_STRING *chars, USHORT *pos) {
  /* flags: 1 = search from the end, 2 = find a character that is NOT in the set, 4 = ignore case. A forward search answers the offset after the character it found, a
   * backward one the offset of the character. */
  if (flags & ~7u) return STATUS_INVALID_PARAMETER_;
  const unsigned n = s->Length / 2, m = chars->Length / 2;
  *pos = 0;
  for (unsigned step = 0; step < n; ++step) {
    const unsigned i = (flags & 1) ? n - 1 - step : step;
    int found = 0;
    for (unsigned k = 0; k < m && !found; ++k)
      found = (flags & 4) ? rtl_upcase(s->Buffer[i]) == rtl_upcase(chars->Buffer[k]) : s->Buffer[i] == chars->Buffer[k];
    if (found != !!(flags & 2)) { *pos = (USHORT)((flags & 1) ? i * 2 : (i + 1) * 2); return STATUS_SUCCESS; }
  }
  return 0xC0000225u;      /* STATUS_NOT_FOUND */
}
EXPORT NTSTATUS NTAPI RtlHashUnicodeString(const UNICODE_STRING *s, BOOLEAN ci, ULONG algorithm, PULONG hash) {
  if (!s || !hash || algorithm > 1) return STATUS_INVALID_PARAMETER_;
  ULONG h = 0;
  for (unsigned i = 0; i < s->Length / 2; ++i) {
    const WCHAR c = ci ? rtl_upcase(s->Buffer[i]) : s->Buffer[i];
    h = algorithm == 0 ? h * 65599u + c : (h << 5) + h + c;
  }
  *hash = h;
  return STATUS_SUCCESS;
}

/* ---- integers and strings ------------------------------------------------------------------------------------------------------------------------------ */

static int rtl_digit(int c, ULONG base) {
  int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
  return (ULONG)v < base ? v : -1;
}
EXPORT NTSTATUS NTAPI RtlCharToInteger(PCSTR s, ULONG base, PULONG value) {
  while ((BYTE)*s && (BYTE)*s <= ' ') ++s;
  int neg = 0;
  if (*s == '-') { neg = 1; ++s; } else if (*s == '+') ++s;
  if (!base) {
    base = 10;
    if (s[0] == '0') {
      if (s[1] == 'b') { base = 2; s += 2; } else if (s[1] == 'o') { base = 8; s += 2; } else if (s[1] == 'x') { base = 16; s += 2; }
    }
  } else if (base != 2 && base != 8 && base != 10 && base != 16) {
    return STATUS_INVALID_PARAMETER_;
  }
  ULONG r = 0;
  int d;
  while ((d = rtl_digit(*s, base)) >= 0) { r = r * base + (ULONG)d; ++s; }
  if (!value) return STATUS_ACCESS_VIOLATION_;      /* (Windows faults on the store and answers with the fault) */
  *value = neg ? (ULONG)-(LONG)r : r;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlUnicodeStringToInteger(const UNICODE_STRING *s, ULONG base, PULONG value) {
  const WCHAR *p = s->Buffer;
  unsigned n = s->Length / 2;
  while (n && *p && *p <= ' ') { ++p; --n; }
  int neg = 0;
  if (n && *p == '-') { neg = 1; ++p; --n; } else if (n && *p == '+') { ++p; --n; }
  if (!base) {
    base = 10;
    if (n > 1 && p[0] == '0') {
      if (p[1] == 'b') { base = 2; p += 2; n -= 2; } else if (p[1] == 'o') { base = 8; p += 2; n -= 2; } else if (p[1] == 'x') { base = 16; p += 2; n -= 2; }
    }
  } else if (base != 2 && base != 8 && base != 10 && base != 16) {
    return STATUS_INVALID_PARAMETER_;
  }
  ULONG r = 0;
  while (n && *p < 128 && rtl_digit(*p, base) >= 0) { r = r * base + (ULONG)rtl_digit(*p, base); ++p; --n; }
  if (!value) return STATUS_ACCESS_VIOLATION_;
  *value = neg ? (ULONG)-(LONG)r : r;
  return STATUS_SUCCESS;
}
/* Digits of a number in a base, into a buffer of `size` characters (the terminator is not counted); the length, or 0 when it does not fit. */
static unsigned rtl_digits(ULONGLONG v, ULONG base, char *out, unsigned size) {
  char tmp[72];
  unsigned n = 0;
  do { const unsigned d = (unsigned)(v % base); tmp[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v /= base; } while (v);
  if (n > size) return 0;
  for (unsigned i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  return n;
}
static int rtl_base_ok(ULONG base) { return base == 2 || base == 8 || base == 10 || base == 16; }
EXPORT NTSTATUS NTAPI RtlIntegerToChar(ULONG value, ULONG base, ULONG length, PSTR str) {
  if (!base) base = 10;
  if (!rtl_base_ok(base)) return STATUS_INVALID_PARAMETER_;
  char buf[40];
  const unsigned n = rtl_digits(value, base, buf, 33);
  if (n > length) return STATUS_BUFFER_OVERFLOW_;
  if (!str) return STATUS_ACCESS_VIOLATION_;
  for (unsigned i = 0; i < n; ++i) str[i] = buf[i];
  if (n < length) str[n] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlLargeIntegerToChar(const ULONGLONG *value, ULONG base, ULONG length, PSTR str) {
  if (!base) base = 10;
  if (!rtl_base_ok(base)) return STATUS_INVALID_PARAMETER_;
  char buf[72];
  const unsigned n = rtl_digits(*value, base, buf, 65);
  if (n > length) return STATUS_BUFFER_OVERFLOW_;
  if (!str) return STATUS_ACCESS_VIOLATION_;
  for (unsigned i = 0; i < n; ++i) str[i] = buf[i];
  if (n < length) str[n] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlIntegerToUnicodeString(ULONG value, ULONG base, PUNICODE_STRING s) {
  if (!base) base = 10;
  if (!rtl_base_ok(base)) return STATUS_INVALID_PARAMETER_;
  char buf[40];
  const unsigned n = rtl_digits(value, base, buf, 33);
  if (n * 2 > s->MaximumLength) return STATUS_BUFFER_OVERFLOW_;
  for (unsigned i = 0; i < n; ++i) s->Buffer[i] = (WCHAR)buf[i];
  s->Length = (USHORT)(n * 2);
  if (s->Length + 2u <= s->MaximumLength) s->Buffer[n] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlInt64ToUnicodeString(ULONGLONG value, ULONG base, PUNICODE_STRING s) {
  if (!base) base = 10;
  if (!rtl_base_ok(base)) return STATUS_INVALID_PARAMETER_;
  char buf[72];
  const unsigned n = rtl_digits(value, base, buf, 65);
  if (n * 2 > s->MaximumLength) return STATUS_BUFFER_OVERFLOW_;
  for (unsigned i = 0; i < n; ++i) s->Buffer[i] = (WCHAR)buf[i];
  s->Length = (USHORT)(n * 2);
  s->Buffer[n] = 0;
  return STATUS_SUCCESS;
}
EXPORT ULONGLONG NTAPI RtlExtendedMagicDivide(LONGLONG a, LONGLONG magic, CHAR shift) {
  /* (a * magic) >> (64 + shift), with the sign of a. */
  const int neg = a < 0;
  const ULONGLONG x = neg ? (ULONGLONG)-a : (ULONGLONG)a, y = (ULONGLONG)magic;
  /* the high 64 bits of x * y, shifted */
  const ULONGLONG xl = x & 0xFFFFFFFFu, xh = x >> 32, yl = y & 0xFFFFFFFFu, yh = y >> 32;
  const ULONGLONG ll = xl * yl, lh = xl * yh, hl = xh * yl, hh = xh * yh;
  const ULONGLONG mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
  const ULONGLONG high = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
  const ULONGLONG r = (unsigned)shift ? high >> (unsigned)shift : high;
  return neg ? (ULONGLONG)-(LONGLONG)r : r;
}
EXPORT LONGLONG NTAPI RtlExtendedIntegerMultiply(LONGLONG a, LONG b) { return a * b; }
EXPORT LONGLONG NTAPI RtlExtendedLargeIntegerDivide(LONGLONG a, ULONG b, PULONG rem) {
  if (!b) return 0;
  const int neg = a < 0;
  const ULONGLONG x = neg ? (ULONGLONG)-a : (ULONGLONG)a;
  if (rem) *rem = (ULONG)(x % b);
  const ULONGLONG q = x / b;
  return neg ? -(LONGLONG)q : (LONGLONG)q;
}
EXPORT LONGLONG NTAPI RtlEnlargedIntegerMultiply(LONG a, LONG b) { return (LONGLONG)a * b; }
EXPORT ULONGLONG NTAPI RtlEnlargedUnsignedMultiply(ULONG a, ULONG b) { return (ULONGLONG)a * b; }
EXPORT ULONG NTAPI RtlEnlargedUnsignedDivide(ULONGLONG a, ULONG b, PULONG rem) {
  if (rem) *rem = (ULONG)(a % b);
  return (ULONG)(a / b);
}
EXPORT LONGLONG NTAPI RtlLargeIntegerAdd(LONGLONG a, LONGLONG b) { return (LONGLONG)((ULONGLONG)a + (ULONGLONG)b); }
EXPORT LONGLONG NTAPI RtlLargeIntegerSubtract(LONGLONG a, LONGLONG b) { return (LONGLONG)((ULONGLONG)a - (ULONGLONG)b); }
EXPORT LONGLONG NTAPI RtlLargeIntegerNegate(LONGLONG a) { return (LONGLONG)(0 - (ULONGLONG)a); }
EXPORT LONGLONG NTAPI RtlLargeIntegerShiftLeft(LONGLONG a, CHAR n) { return (LONGLONG)((ULONGLONG)a << (n & 63)); }
EXPORT LONGLONG NTAPI RtlLargeIntegerShiftRight(LONGLONG a, CHAR n) { return (LONGLONG)((ULONGLONG)a >> (n & 63)); }
EXPORT LONGLONG NTAPI RtlLargeIntegerArithmeticShift(LONGLONG a, CHAR n) { return a >> (n & 63); }
EXPORT ULONGLONG NTAPI RtlLargeIntegerDivide(ULONGLONG a, ULONGLONG b, ULONGLONG *rem) {
  if (!b) return 0;
  if (rem) *rem = a % b;
  return a / b;
}
EXPORT LONGLONG NTAPI RtlConvertLongToLargeInteger(LONG a) { return a; }
EXPORT ULONGLONG NTAPI RtlConvertUlongToLargeInteger(ULONG a) { return a; }

/* ---- GUIDs --------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { ULONG Data1; USHORT Data2, Data3; BYTE Data4[8]; } RTL_GUID;
EXPORT NTSTATUS NTAPI RtlStringFromGUID(const RTL_GUID *g, PUNICODE_STRING s) {
  static const char hex[] = "0123456789ABCDEF";
  WCHAR *buf = rtl_alloc(39 * 2);
  if (!buf) return STATUS_NO_MEMORY_;
  unsigned n = 0;
  buf[n++] = '{';
  for (int i = 7; i >= 0; --i) buf[n++] = (WCHAR)hex[(g->Data1 >> (i * 4)) & 15];
  buf[n++] = '-';
  for (int i = 3; i >= 0; --i) buf[n++] = (WCHAR)hex[(g->Data2 >> (i * 4)) & 15];
  buf[n++] = '-';
  for (int i = 3; i >= 0; --i) buf[n++] = (WCHAR)hex[(g->Data3 >> (i * 4)) & 15];
  buf[n++] = '-';
  for (int i = 0; i < 8; ++i) {
    buf[n++] = (WCHAR)hex[g->Data4[i] >> 4];
    buf[n++] = (WCHAR)hex[g->Data4[i] & 15];
    if (i == 1) buf[n++] = '-';
  }
  buf[n++] = '}';
  buf[n] = 0;
  s->Buffer = buf;
  s->Length = 38 * 2;
  s->MaximumLength = 39 * 2;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlGUIDFromString(const UNICODE_STRING *s, RTL_GUID *g) {
  const WCHAR *p = s->Buffer;
  if (s->Length != 38 * 2 || p[0] != '{' || p[37] != '}' || p[9] != '-' || p[14] != '-' || p[19] != '-' || p[24] != '-') return STATUS_INVALID_PARAMETER_;
  ULONGLONG parts[11];
  const int pos[11] = {1, 10, 15, 20, 22, 25, 27, 29, 31, 33, 35};
  const int len[11] = {8, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2};
  for (int k = 0; k < 11; ++k) {
    ULONGLONG v = 0;
    for (int i = 0; i < len[k]; ++i) {
      const WCHAR c = p[pos[k] + i];
      const int d = c < 128 ? rtl_digit(c, 16) : -1;
      if (d < 0) return STATUS_INVALID_PARAMETER_;
      v = v * 16 + (ULONGLONG)d;
    }
    parts[k] = v;
  }
  g->Data1 = (ULONG)parts[0];
  g->Data2 = (USHORT)parts[1];
  g->Data3 = (USHORT)parts[2];
  for (int i = 0; i < 8; ++i) g->Data4[i] = (BYTE)parts[3 + i];
  return STATUS_SUCCESS;
}

/* ---- bitmaps ------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { ULONG SizeOfBitMap; ULONG *Buffer; } RTL_BITMAP;
static int bm_get(const RTL_BITMAP *b, ULONG i) { return (b->Buffer[i >> 5] >> (i & 31)) & 1; }
static void bm_put(RTL_BITMAP *b, ULONG i, int v) { if (v) b->Buffer[i >> 5] |= 1u << (i & 31); else b->Buffer[i >> 5] &= ~(1u << (i & 31)); }
EXPORT void NTAPI RtlInitializeBitMap(RTL_BITMAP *b, ULONG *buffer, ULONG size) { b->SizeOfBitMap = size; b->Buffer = buffer; }
EXPORT void NTAPI RtlSetAllBits(RTL_BITMAP *b) { for (ULONG i = 0; i < (b->SizeOfBitMap + 31) / 32; ++i) b->Buffer[i] = ~0u; }
EXPORT void NTAPI RtlClearAllBits(RTL_BITMAP *b) { for (ULONG i = 0; i < (b->SizeOfBitMap + 31) / 32; ++i) b->Buffer[i] = 0; }
EXPORT void NTAPI RtlSetBits(RTL_BITMAP *b, ULONG from, ULONG count) {
  if (from > b->SizeOfBitMap || count > b->SizeOfBitMap - from) return;
  for (ULONG i = 0; i < count; ++i) bm_put(b, from + i, 1);
}
EXPORT void NTAPI RtlClearBits(RTL_BITMAP *b, ULONG from, ULONG count) {
  if (from > b->SizeOfBitMap || count > b->SizeOfBitMap - from) return;
  for (ULONG i = 0; i < count; ++i) bm_put(b, from + i, 0);
}
EXPORT BOOLEAN NTAPI RtlAreBitsSet(const RTL_BITMAP *b, ULONG from, ULONG count) {
  if (!count || from > b->SizeOfBitMap || count > b->SizeOfBitMap - from) return FALSE;
  for (ULONG i = 0; i < count; ++i) if (!bm_get(b, from + i)) return FALSE;
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlAreBitsClear(const RTL_BITMAP *b, ULONG from, ULONG count) {
  if (!count || from > b->SizeOfBitMap || count > b->SizeOfBitMap - from) return FALSE;
  for (ULONG i = 0; i < count; ++i) if (bm_get(b, from + i)) return FALSE;
  return TRUE;
}
EXPORT ULONG NTAPI RtlNumberOfSetBits(const RTL_BITMAP *b) { ULONG n = 0; for (ULONG i = 0; i < b->SizeOfBitMap; ++i) n += (ULONG)bm_get(b, i); return n; }
EXPORT ULONG NTAPI RtlNumberOfClearBits(const RTL_BITMAP *b) { return b->SizeOfBitMap - RtlNumberOfSetBits(b); }
/* The first run of `count` bits equal to `value`, from `hint` (wrapping once); 0xFFFFFFFF if none. */
static ULONG bm_find(const RTL_BITMAP *b, ULONG count, ULONG hint, int value) {
  if (!count || count > b->SizeOfBitMap) return ~0u;
  if (hint >= b->SizeOfBitMap) hint = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const ULONG from = pass ? 0 : hint, to = pass ? hint + count - 1 : b->SizeOfBitMap;
    ULONG run = 0;
    for (ULONG i = from; i < to && i < b->SizeOfBitMap; ++i) {
      if (bm_get(b, i) == value) { if (++run == count) return i + 1 - count; } else run = 0;
    }
  }
  return ~0u;
}
EXPORT ULONG NTAPI RtlFindClearBits(const RTL_BITMAP *b, ULONG count, ULONG hint) { return bm_find(b, count, hint, 0); }
EXPORT ULONG NTAPI RtlFindSetBits(const RTL_BITMAP *b, ULONG count, ULONG hint) { return bm_find(b, count, hint, 1); }
EXPORT ULONG NTAPI RtlFindClearBitsAndSet(RTL_BITMAP *b, ULONG count, ULONG hint) {
  const ULONG r = bm_find(b, count, hint, 0);
  if (r != ~0u) RtlSetBits(b, r, count);
  return r;
}
EXPORT ULONG NTAPI RtlFindSetBitsAndClear(RTL_BITMAP *b, ULONG count, ULONG hint) {
  const ULONG r = bm_find(b, count, hint, 1);
  if (r != ~0u) RtlClearBits(b, r, count);
  return r;
}
typedef struct { ULONG StartingIndex, NumberOfBits; } RTL_BITMAP_RUN;
EXPORT ULONG NTAPI RtlFindClearRuns(const RTL_BITMAP *b, RTL_BITMAP_RUN *runs, ULONG max, BOOLEAN longest_first) {
  ULONG n = 0, i = 0;
  while (i < b->SizeOfBitMap) {
    if (bm_get(b, i)) { ++i; continue; }
    const ULONG start = i;
    while (i < b->SizeOfBitMap && !bm_get(b, i)) ++i;
    if (!longest_first) {
      if (n == max) break;
      runs[n].StartingIndex = start;
      runs[n].NumberOfBits = i - start;
      ++n;
      continue;
    }
    /* keep the `max` longest runs seen so far, longest first (a shorter or equal run does not replace an earlier one) */
    ULONG at = n;
    while (at > 0 && runs[at - 1].NumberOfBits < i - start) --at;
    if (at >= max) continue;
    if (n < max) ++n;
    for (ULONG k = n - 1; k > at; --k) runs[k] = runs[k - 1];
    runs[at].StartingIndex = start;
    runs[at].NumberOfBits = i - start;
  }
  return n;
}
EXPORT ULONG NTAPI RtlFindLongestRunClear(const RTL_BITMAP *b, PULONG start) {
  ULONG best = 0, best_at = 0, i = 0;
  while (i < b->SizeOfBitMap) {
    if (bm_get(b, i)) { ++i; continue; }
    const ULONG s = i;
    while (i < b->SizeOfBitMap && !bm_get(b, i)) ++i;
    if (i - s > best) { best = i - s; best_at = s; }
  }
  if (start) *start = best_at;
  return best;
}
EXPORT ULONG NTAPI RtlFindLongestRunSet(const RTL_BITMAP *b, PULONG start) {
  ULONG best = 0, best_at = 0, i = 0;
  while (i < b->SizeOfBitMap) {
    if (!bm_get(b, i)) { ++i; continue; }
    const ULONG s = i;
    while (i < b->SizeOfBitMap && bm_get(b, i)) ++i;
    if (i - s > best) { best = i - s; best_at = s; }
  }
  if (start) *start = best_at;
  return best;
}
EXPORT ULONG NTAPI RtlFindNextForwardRunClear(const RTL_BITMAP *b, ULONG from, PULONG start) {
  ULONG i = from;
  while (i < b->SizeOfBitMap && bm_get(b, i)) ++i;
  *start = i;
  ULONG n = 0;
  while (i < b->SizeOfBitMap && !bm_get(b, i)) { ++i; ++n; }
  return n;
}
EXPORT ULONG NTAPI RtlFindNextForwardRunSet(const RTL_BITMAP *b, ULONG from, PULONG start) {
  ULONG i = from;
  while (i < b->SizeOfBitMap && !bm_get(b, i)) ++i;
  *start = i;
  ULONG n = 0;
  while (i < b->SizeOfBitMap && bm_get(b, i)) { ++i; ++n; }
  return n;
}
EXPORT CCHAR_ NTAPI RtlFindLeastSignificantBit(ULONGLONG v) { if (!v) return -1; CCHAR_ n = 0; while (!(v & 1)) { v >>= 1; ++n; } return n; }
EXPORT CCHAR_ NTAPI RtlFindMostSignificantBit(ULONGLONG v) { if (!v) return -1; CCHAR_ n = 63; while (!(v >> 63)) { v <<= 1; --n; } return n; }
