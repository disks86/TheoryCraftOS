/* TheoryCraftOS - a minimal UEFI x86_64 operating system.
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

/* Internationalized domain names of kernel32.dll (included by kernel32.c): IdnToNameprepUnicode, IdnToAscii and IdnToUnicode with the nameprep of RFC 3491 (its mapping step is
 * RtlNormalizeString of ntdll with the form 13) and the Punycode of RFC 3492. */

#include "idn_data.inc"

#define IDN_ALLOW_UNASSIGNED_ 0x1u
#define IDN_USE_STD3_ASCII_RULES_ 0x2u
#define IDN_NAMEPREP 0
#define IDN_TO_ASCII 1
#define IDN_TO_UNICODE 2
#define IDN_LABEL_MAX 63
#define IDN_ERROR_NAME 123   /* ERROR_INVALID_NAME */

NTSTATUS NTAPI RtlNormalizeString(ULONG form, const WCHAR *src, INT src_len, WCHAR *dst, INT *dst_len);

static BOOL idn_in(const DWORD *ranges, SIZE_T count /* DWORDs: two for each range */, DWORD cp) {
  SIZE_T lo = 0, hi = count / 2;
  while (lo < hi) {
    const SIZE_T mid = lo + (hi - lo) / 2;
    if (cp < ranges[mid * 2]) hi = mid;
    else if (cp > ranges[mid * 2 + 1]) lo = mid + 1;
    else return TRUE;
  }
  return FALSE;
}
static BOOL idn_is_dot(WCHAR c) { return c == '.' || c == 0x3002 || c == 0xFF0E || c == 0xFF61; }

/* ---- Punycode ---------------------------------------------------------------------------------------------------------------------------------------- */

#define PUNY_BASE 36u
#define PUNY_TMIN 1u
#define PUNY_TMAX 26u
#define PUNY_SKEW 38u
#define PUNY_DAMP 700u
#define PUNY_INITIAL_BIAS 72u
#define PUNY_INITIAL_N 128u

static DWORD puny_adapt(DWORD delta, DWORD points, BOOL first) {
  delta = first ? delta / PUNY_DAMP : delta / 2;
  delta += delta / points;
  DWORD k = 0;
  while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2) { delta /= PUNY_BASE - PUNY_TMIN; k += PUNY_BASE; }
  return k + (PUNY_BASE - PUNY_TMIN + 1) * delta / (delta + PUNY_SKEW);
}
static WCHAR puny_digit(DWORD d) { return (WCHAR)(d < 26 ? 'a' + d : '0' + (d - 26)); }
static int puny_value(WCHAR c) {
  if (c >= '0' && c <= '9') return c - '0' + 26;
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a';
  return -1;
}
static DWORD puny_threshold(DWORD k, DWORD bias) { return k <= bias ? PUNY_TMIN : k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias; }

/* Encodes code points; FALSE if the output does not fit. */
static BOOL puny_encode(const DWORD *in, SIZE_T count, WCHAR *out, SIZE_T capacity, SIZE_T *length) {
  SIZE_T n_out = 0;
  DWORD basic = 0;
  for (SIZE_T i = 0; i < count; ++i) if (in[i] < 0x80) { if (n_out >= capacity) return FALSE; out[n_out++] = (WCHAR)in[i]; ++basic; }
  DWORD handled = basic;
  if (basic) { if (n_out >= capacity) return FALSE; out[n_out++] = '-'; }
  DWORD n = PUNY_INITIAL_N, delta = 0, bias = PUNY_INITIAL_BIAS;
  while (handled < count) {
    DWORD m = 0xFFFFFFFFu;
    for (SIZE_T i = 0; i < count; ++i) if (in[i] >= n && in[i] < m) m = in[i];
    if ((m - n) > (0xFFFFFFFFu - delta) / (handled + 1)) return FALSE;
    delta += (m - n) * (handled + 1);
    n = m;
    for (SIZE_T i = 0; i < count; ++i) {
      if (in[i] < n && ++delta == 0) return FALSE;
      if (in[i] != n) continue;
      DWORD q = delta;
      for (DWORD k = PUNY_BASE;; k += PUNY_BASE) {
        const DWORD t = puny_threshold(k, bias);
        if (q < t) break;
        if (n_out >= capacity) return FALSE;
        out[n_out++] = puny_digit(t + (q - t) % (PUNY_BASE - t));
        q = (q - t) / (PUNY_BASE - t);
      }
      if (n_out >= capacity) return FALSE;
      out[n_out++] = puny_digit(q);
      bias = puny_adapt(delta, handled + 1, handled == basic);
      delta = 0;
      ++handled;
    }
    ++delta;
    ++n;
  }
  *length = n_out;
  return TRUE;
}

static BOOL puny_decode(const WCHAR *in, SIZE_T count, DWORD *out, SIZE_T capacity, SIZE_T *length) {
  SIZE_T n_out = 0, b = 0;
  for (SIZE_T i = 0; i < count; ++i) if (in[i] == '-') b = i;
  for (SIZE_T i = 0; i < b; ++i) {
    if (in[i] >= 0x80 || n_out >= capacity) return FALSE;
    out[n_out++] = in[i];
  }
  DWORD n = PUNY_INITIAL_N, i = 0, bias = PUNY_INITIAL_BIAS;
  for (SIZE_T pos = b > 0 ? b + 1 : 0; pos < count; ++n_out) {
    const DWORD old_i = i;
    DWORD w = 1;
    for (DWORD k = PUNY_BASE;; k += PUNY_BASE) {
      if (pos >= count) return FALSE;
      const int digit = puny_value(in[pos++]);
      if (digit < 0 || (DWORD)digit > (0xFFFFFFFFu - i) / w) return FALSE;
      i += (DWORD)digit * w;
      const DWORD t = puny_threshold(k, bias);
      if ((DWORD)digit < t) break;
      if (w > 0xFFFFFFFFu / (PUNY_BASE - t)) return FALSE;
      w *= PUNY_BASE - t;
    }
    bias = puny_adapt(i - old_i, (DWORD)n_out + 1, old_i == 0);
    if (i / ((DWORD)n_out + 1) > 0xFFFFFFFFu - n) return FALSE;
    n += i / ((DWORD)n_out + 1);
    i %= (DWORD)n_out + 1;
    if (n > 0x10FFFF || (n >= 0xD800 && n < 0xE000) || n_out >= capacity) return FALSE;
    memmove(out + i + 1, out + i, (n_out - i) * sizeof(DWORD));
    out[i++] = n;
  }
  *length = n_out;
  return TRUE;
}

/* ---- a label --------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { WCHAR *text; SIZE_T length, capacity; BOOL failed; } IdnText;
static void idn_add(IdnText *t, WCHAR c) {
  if (t->failed) return;
  if (t->length == t->capacity) {
    const SIZE_T capacity = t->capacity ? t->capacity * 2 : 128;
    WCHAR *grown = RtlAllocateHeap(heap(), 0, capacity * 2);
    if (!grown) { t->failed = TRUE; return; }
    if (t->text) { memcpy(grown, t->text, t->length * 2); RtlFreeHeap(heap(), 0, t->text); }
    t->text = grown;
    t->capacity = capacity;
  }
  t->text[t->length++] = c;
}
static BOOL idn_ascii_only(const WCHAR *s, SIZE_T n) { for (SIZE_T i = 0; i < n; ++i) if (s[i] >= 0x80) return FALSE; return TRUE; }

/* The rules for ASCII in a label (with IDN_USE_STD3_ASCII_RULES): letters, digits and hyphens, and no hyphen at either end. */
static BOOL idn_std3(const WCHAR *s, SIZE_T n) {
  for (SIZE_T i = 0; i < n; ++i) {
    const WCHAR c = s[i];
    if (c >= 0x80) continue;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return FALSE;
  }
  return n == 0 || (s[0] != '-' && s[n - 1] != '-');
}

/* The checks of nameprep after the mapping: prohibited and unassigned characters and the rules for the bidirectional text of RFC 3454. The label is UTF-16. */
static BOOL idn_check(DWORD flags, const WCHAR *s, SIZE_T n) {
  BOOL has_randal = FALSE, has_lcat = FALSE;
  DWORD first = 0, last = 0;
  for (SIZE_T i = 0; i < n; ++i) {
    DWORD cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && (s[i + 1] & 0xFC00) == 0xDC00) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00); ++i; }
    else if (cp >= 0xD800 && cp < 0xE000) return FALSE;
    if (idn_in(k_idn_prohibited, sizeof(k_idn_prohibited) / sizeof(DWORD), cp)) return FALSE;
    if (!(flags & IDN_ALLOW_UNASSIGNED_) && idn_in(k_idn_unassigned, sizeof(k_idn_unassigned) / sizeof(DWORD), cp)) return FALSE;
    if (idn_in(k_idn_randal, sizeof(k_idn_randal) / sizeof(DWORD), cp)) has_randal = TRUE;
    if (idn_in(k_idn_lcat, sizeof(k_idn_lcat) / sizeof(DWORD), cp)) has_lcat = TRUE;
    if (i == 0 || first == 0) first = cp;
    last = cp;
  }
  if (has_randal) {
    if (has_lcat) return FALSE;
    if (!idn_in(k_idn_randal, sizeof(k_idn_randal) / sizeof(DWORD), first) || !idn_in(k_idn_randal, sizeof(k_idn_randal) / sizeof(DWORD), last)) return FALSE;
  }
  return TRUE;
}

/* Nameprep of one label: ASCII only labels stay as they are. Appends the result; FALSE if the label is not valid. */
static BOOL idn_prep_label(DWORD flags, BOOL name_is_ascii, const WCHAR *label, SIZE_T n, IdnText *out) {
  if (name_is_ascii && idn_ascii_only(label, n)) {
    if ((flags & IDN_USE_STD3_ASCII_RULES_) && !idn_std3(label, n)) return FALSE;
    for (SIZE_T i = 0; i < n; ++i) idn_add(out, label[i]);
    return !out->failed;
  }
  INT room = (INT)(n * 18 + 16);
  WCHAR *mapped = RtlAllocateHeap(heap(), 0, (SIZE_T)room * 2);
  if (!mapped) return FALSE;
  BOOL ok = RtlNormalizeString(13, label, (INT)n, mapped, &room) == STATUS_SUCCESS && room > 0;
  if (ok) ok = idn_check(flags, mapped, (SIZE_T)room);
  if (ok && (flags & IDN_USE_STD3_ASCII_RULES_)) ok = idn_std3(mapped, (SIZE_T)room);
  if (ok) for (INT i = 0; i < room; ++i) idn_add(out, mapped[i]);
  RtlFreeHeap(heap(), 0, mapped);
  return ok && !out->failed;
}

/* UTF-16 to code points into a heap array. */
static DWORD *idn_code_points(const WCHAR *s, SIZE_T n, SIZE_T *count) {
  DWORD *cps = RtlAllocateHeap(heap(), 0, (n + 1) * sizeof(DWORD));
  if (!cps) return NULL;
  SIZE_T k = 0;
  for (SIZE_T i = 0; i < n; ++i) {
    DWORD cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && (s[i + 1] & 0xFC00) == 0xDC00) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00); ++i; }
    cps[k++] = cp;
  }
  *count = k;
  return cps;
}

static BOOL idn_starts_xn(const WCHAR *s, SIZE_T n) {
  return n >= 4 && (s[0] | 0x20) == 'x' && (s[1] | 0x20) == 'n' && s[2] == '-' && s[3] == '-';
}

/* The whole name in one of the three modes. The result is on the heap (without any terminating NUL); FALSE sets the error. */
static BOOL idn_process(DWORD flags, int mode, const WCHAR *text, SIZE_T length, IdnText *result) {
  if (length == 0) return FALSE;
  for (SIZE_T i = 0; i < length; ++i) if (text[i] == 0) return FALSE;
  const BOOL name_is_ascii = idn_ascii_only(text, length);
  SIZE_T start = 0;
  for (;;) {
    SIZE_T end = start;
    while (end < length && !idn_is_dot(text[end])) ++end;
    const WCHAR *label = text + start;
    const SIZE_T n = end - start;
    const BOOL last = end >= length;
    if (n == 0) {
      if (!last || start == 0) return FALSE;      /* an empty label is only allowed at the end, after another one */
    } else if (mode == IDN_TO_UNICODE) {
      if (!idn_ascii_only(label, n) || n > IDN_LABEL_MAX) return FALSE;
      if (idn_starts_xn(label, n)) {
        SIZE_T count = 0;
        DWORD *cps = RtlAllocateHeap(heap(), 0, (n + 1) * sizeof(DWORD));
        if (!cps) return FALSE;
        BOOL ok = puny_decode(label + 4, n - 4, cps, n, &count) && count > 0;
        IdnText decoded = {0};
        for (SIZE_T i = 0; ok && i < count; ++i) {
          if (cps[i] >= 0x10000) { idn_add(&decoded, (WCHAR)(0xD800 + ((cps[i] - 0x10000) >> 10))); idn_add(&decoded, (WCHAR)(0xDC00 + ((cps[i] - 0x10000) & 0x3FF))); }
          else idn_add(&decoded, (WCHAR)cps[i]);
        }
        /* what was decoded must prepare to itself and encode back to the label */
        IdnText prepared = {0};
        ok = ok && !decoded.failed && idn_prep_label(flags, FALSE, decoded.text, decoded.length, &prepared);
        if (ok) {
          IdnText again = {0};
          SIZE_T pcount = 0;
          DWORD *pcps = idn_code_points(prepared.text, prepared.length, &pcount);
          WCHAR encoded[IDN_LABEL_MAX + 1];
          SIZE_T encoded_length = 0;
          ok = pcps && puny_encode(pcps, pcount, encoded, IDN_LABEL_MAX, &encoded_length) && encoded_length == n - 4;
          for (SIZE_T i = 0; ok && i < encoded_length; ++i) if ((encoded[i] | 0x20) != (label[4 + i] | 0x20) && encoded[i] != label[4 + i]) ok = FALSE;
          if (pcps) RtlFreeHeap(heap(), 0, pcps);
          if (again.text) RtlFreeHeap(heap(), 0, again.text);
        }
        if (ok) for (SIZE_T i = 0; i < decoded.length; ++i) idn_add(result, decoded.text[i]);
        if (decoded.text) RtlFreeHeap(heap(), 0, decoded.text);
        if (prepared.text) RtlFreeHeap(heap(), 0, prepared.text);
        RtlFreeHeap(heap(), 0, cps);
        if (!ok) return FALSE;
      } else {
        for (SIZE_T i = 0; i < n; ++i) idn_add(result, label[i]);
      }
    } else {
      IdnText prepared = {0};
      BOOL ok = idn_prep_label(flags, name_is_ascii, label, n, &prepared);
      if (ok && prepared.length == 0) ok = FALSE;
      if (ok && mode == IDN_TO_ASCII && !idn_ascii_only(prepared.text, prepared.length)) {
        SIZE_T count = 0;
        DWORD *cps = idn_code_points(prepared.text, prepared.length, &count);
        WCHAR encoded[IDN_LABEL_MAX + 1];
        SIZE_T encoded_length = 0;
        ok = cps && puny_encode(cps, count, encoded, IDN_LABEL_MAX - 4, &encoded_length);
        if (ok) { idn_add(result, 'x'); idn_add(result, 'n'); idn_add(result, '-'); idn_add(result, '-'); for (SIZE_T i = 0; i < encoded_length; ++i) idn_add(result, encoded[i]); }
        if (cps) RtlFreeHeap(heap(), 0, cps);
      } else if (ok) {
        ok = prepared.length <= IDN_LABEL_MAX;
        for (SIZE_T i = 0; ok && i < prepared.length; ++i) idn_add(result, prepared.text[i]);
      }
      if (prepared.text) RtlFreeHeap(heap(), 0, prepared.text);
      if (!ok) return FALSE;
    }
    if (last) break;
    idn_add(result, '.');
    start = end + 1;
  }
  return !result->failed;
}

static int idn_api(DWORD flags, int mode, LPCWSTR source, int count, LPWSTR dest, int capacity) {
  if (flags & ~(IDN_ALLOW_UNASSIGNED_ | IDN_USE_STD3_ASCII_RULES_)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (!source || count < -1 || capacity < 0 || (capacity > 0 && !dest)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (count == 0) { set_error(IDN_ERROR_NAME); return 0; }
  SIZE_T length = count < 0 ? wcslen(source) + 1 : (SIZE_T)count;
  const BOOL terminated = source[length - 1] == 0;
  if (terminated) --length;
  IdnText result = {0};
  if (!idn_process(flags, mode, source, length, &result)) {
    if (result.text) RtlFreeHeap(heap(), 0, result.text);
    set_error(result.failed ? ERROR_NOT_ENOUGH_MEMORY : IDN_ERROR_NAME);
    return 0;
  }
  if (terminated) idn_add(&result, 0);
  int ret = 0;
  if (result.failed) set_error(ERROR_NOT_ENOUGH_MEMORY);
  else if (capacity == 0) ret = (int)result.length;
  else if ((SIZE_T)capacity < result.length) set_error(ERROR_INSUFFICIENT_BUFFER);
  else { memcpy(dest, result.text, result.length * 2); ret = (int)result.length; }
  if (result.text) RtlFreeHeap(heap(), 0, result.text);
  return ret;
}

EXPORT int WINAPI IdnToNameprepUnicode(DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity) { return idn_api(flags, IDN_NAMEPREP, source, count, dest, capacity); }
EXPORT int WINAPI IdnToAscii(DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity) { return idn_api(flags, IDN_TO_ASCII, source, count, dest, capacity); }
EXPORT int WINAPI IdnToUnicode(DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity) { return idn_api(flags, IDN_TO_UNICODE, source, count, dest, capacity); }
