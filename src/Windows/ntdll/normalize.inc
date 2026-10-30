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

/* Unicode normalization (forms C, D, KC and KD) of ntdll.dll (included by ntdll.c): RtlNormalizeString and RtlIsNormalizedString, over the tables of unidata.inc. */

#include "unidata.inc"
#include "idn_data.inc"

#define NORM_C 1u
#define NORM_D 2u
#define NORM_KC 5u
#define NORM_KD 6u
#define NORM_IDN 13u
#define NORM_NO_UNICODE_TRANSLATION ((NTSTATUS)0xC0000717)
#define NORM_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)

#define HANGUL_S 0xAC00u
#define HANGUL_L 0x1100u
#define HANGUL_V 0x1161u
#define HANGUL_T 0x11A7u
#define HANGUL_LCOUNT 19u
#define HANGUL_VCOUNT 21u
#define HANGUL_TCOUNT 28u
#define HANGUL_NCOUNT (HANGUL_VCOUNT * HANGUL_TCOUNT)
#define HANGUL_SCOUNT (HANGUL_LCOUNT * HANGUL_NCOUNT)

/* A growing list of code points, on the process heap; a failed growth leaves `failed` set and later pushes do nothing. */
typedef struct { DWORD *items; SIZE_T count, capacity; BOOL failed; } NormList;

static void norm_push(NormList *l, DWORD cp) {
  if (l->failed) return;
  if (l->count == l->capacity) {
    if (l->capacity > ((SIZE_T)-1) / 8) { l->failed = TRUE; return; }
    const SIZE_T capacity = l->capacity ? l->capacity * 2 : 64;
    DWORD *grown = l->items ? RtlReAllocateHeap(RtlProcessHeap(), 0, l->items, capacity * 4) : RtlAllocateHeap(RtlProcessHeap(), 0, capacity * 4);
    if (!grown) { l->failed = TRUE; return; }
    l->items = grown;
    l->capacity = capacity;
  }
  l->items[l->count++] = cp;
}
static void norm_free(NormList *l) { if (l->items) RtlFreeHeap(RtlProcessHeap(), 0, l->items); l->items = NULL; l->count = l->capacity = 0; }

static BYTE norm_ccc(DWORD cp) {
  SIZE_T lo = 0, hi = sizeof(uni_ccc) / sizeof(uni_ccc[0]);
  if (cp < 0x300) return 0;
  while (lo < hi) {
    const SIZE_T mid = lo + (hi - lo) / 2;
    if (cp < uni_ccc[mid].first) hi = mid;
    else if (cp > uni_ccc[mid].last) lo = mid + 1;
    else return uni_ccc[mid].ccc;
  }
  return 0;
}

/* Appends the full decomposition of a character. */
static void norm_decompose(NormList *out, DWORD cp, BOOL compat) {
  if (cp >= HANGUL_S && cp < HANGUL_S + HANGUL_SCOUNT) {
    const DWORD s = cp - HANGUL_S;
    norm_push(out, HANGUL_L + s / HANGUL_NCOUNT);
    norm_push(out, HANGUL_V + (s % HANGUL_NCOUNT) / HANGUL_TCOUNT);
    if (s % HANGUL_TCOUNT) norm_push(out, HANGUL_T + s % HANGUL_TCOUNT);
    return;
  }
  if (cp >= 0x80) {
    for (int pass = compat ? 1 : 0; pass >= 0; --pass) {
      const BOOL use_compat = pass == 1;
      SIZE_T lo = 0, hi = use_compat ? sizeof(uni_compat) / sizeof(uni_compat[0]) : sizeof(uni_canonical) / sizeof(uni_canonical[0]);
      while (lo < hi) {
        const SIZE_T mid = lo + (hi - lo) / 2;
        const DWORD key = use_compat ? uni_compat[mid].cp : uni_canonical[mid].cp;
        if (key < cp) lo = mid + 1; else if (key > cp) hi = mid;
        else {
          const DWORD *pool = use_compat ? uni_compat_pool + uni_compat[mid].offset : uni_canonical_pool + uni_canonical[mid].offset;
          const BYTE length = use_compat ? uni_compat[mid].length : uni_canonical[mid].length;
          for (BYTE i = 0; i < length; ++i) norm_push(out, pool[i]);
          return;
        }
      }
    }
  }
  norm_push(out, cp);
}

/* The canonical ordering: characters with a combining class sort by it, stable, inside each run of them. */
static void norm_reorder(NormList *l) {
  for (SIZE_T i = 1; i < l->count; ++i) {
    const BYTE class_i = norm_ccc(l->items[i]);
    if (!class_i) continue;
    SIZE_T j = i;
    while (j > 0) {
      const BYTE class_prev = norm_ccc(l->items[j - 1]);
      if (class_prev <= class_i) break;
      const DWORD t = l->items[j - 1]; l->items[j - 1] = l->items[j]; l->items[j] = t;
      --j;
    }
  }
}

static DWORD norm_pair(DWORD first, DWORD second) {
  if (first >= HANGUL_L && first < HANGUL_L + HANGUL_LCOUNT && second >= HANGUL_V && second < HANGUL_V + HANGUL_VCOUNT)
    return HANGUL_S + ((first - HANGUL_L) * HANGUL_VCOUNT + (second - HANGUL_V)) * HANGUL_TCOUNT;
  if (first >= HANGUL_S && first < HANGUL_S + HANGUL_SCOUNT && (first - HANGUL_S) % HANGUL_TCOUNT == 0 && second > HANGUL_T && second < HANGUL_T + HANGUL_TCOUNT)
    return first + (second - HANGUL_T);
  SIZE_T lo = 0, hi = sizeof(uni_compose) / sizeof(uni_compose[0]);
  while (lo < hi) {
    const SIZE_T mid = lo + (hi - lo) / 2;
    if (uni_compose[mid].first < first || (uni_compose[mid].first == first && uni_compose[mid].second < second)) lo = mid + 1;
    else if (uni_compose[mid].first == first && uni_compose[mid].second == second) return uni_compose[mid].composed;
    else hi = mid;
  }
  return 0;
}

static void norm_compose(NormList *l) {
  if (l->count < 2) return;
  SIZE_T starter = 0, out = 1;
  DWORD starter_cp = l->items[0];
  unsigned last_class = norm_ccc(starter_cp) ? 256 : 0;
  for (SIZE_T i = 1; i < l->count; ++i) {
    const DWORD cp = l->items[i];
    const unsigned cls = norm_ccc(cp);
    const DWORD composite = norm_pair(starter_cp, cp);
    if (composite && (last_class < cls || last_class == 0)) {
      l->items[starter] = composite;
      starter_cp = composite;
    } else {
      if (cls == 0) { starter = out; starter_cp = cp; }
      last_class = cls;
      l->items[out++] = cp;
    }
  }
  l->count = out;
}

/* UTF-16 to code points. FALSE on an unpaired surrogate; *bad is the index the error is reported at (for a high surrogate the character after it). */
static BOOL norm_decode(const WCHAR *src, SIZE_T length, NormList *out, SIZE_T *bad) {
  for (SIZE_T i = 0; i < length; ++i) {
    const DWORD c = src[i];
    if (c >= 0xD800 && c < 0xDC00) {
      if (i + 1 < length && (src[i + 1] & 0xFC00) == 0xDC00) { norm_push(out, 0x10000 + ((c - 0xD800) << 10) + (src[i + 1] - 0xDC00)); ++i; continue; }
      *bad = i + 1;
      return FALSE;
    }
    if (c >= 0xDC00 && c < 0xE000) { *bad = i; return FALSE; }
    norm_push(out, c);
  }
  return TRUE;
}
static SIZE_T norm_utf16_length(const NormList *l) {
  SIZE_T n = 0;
  for (SIZE_T i = 0; i < l->count; ++i) n += l->items[i] >= 0x10000 ? 2 : 1;
  return n;
}

/* The first step of nameprep (the IDN form): characters that map to nothing go, the others are case folded. The text is refused when it holds a NUL that is not the last
 * character or one of the bidirectional isolates Windows does not allow. */
static NTSTATUS norm_idn_map(NormList *cps, SIZE_T *bad) {
  NormList mapped = {0};
  for (SIZE_T i = 0; i < cps->count; ++i) {
    const DWORD cp = cps->items[i];
    if ((cp == 0 && i + 1 != cps->count) || (cp >= 0x2066 && cp <= 0x2069)) { *bad = i; norm_free(&mapped); return NORM_NO_UNICODE_TRANSLATION; }
    BOOL ignored = FALSE;
    for (SIZE_T k = 0; k < sizeof(uni_idn_ignored) / sizeof(uni_idn_ignored[0]); ++k) if (cp >= uni_idn_ignored[k].first && cp <= uni_idn_ignored[k].last) { ignored = TRUE; break; }
    if (ignored) continue;
    SIZE_T lo = 0, hi = sizeof(uni_idn_fold) / sizeof(uni_idn_fold[0]);
    BOOL folded = FALSE;
    while (lo < hi) {
      const SIZE_T mid = lo + (hi - lo) / 2;
      if (uni_idn_fold[mid].cp < cp) lo = mid + 1;
      else if (uni_idn_fold[mid].cp > cp) hi = mid;
      else { for (BYTE k = 0; k < uni_idn_fold[mid].length; ++k) norm_push(&mapped, uni_idn_fold_pool[uni_idn_fold[mid].offset + k]); folded = TRUE; break; }
    }
    if (!folded) norm_push(&mapped, cp);
  }
  norm_free(cps);
  *cps = mapped;
  return mapped.failed ? STATUS_NO_MEMORY : STATUS_SUCCESS;
}

/* Normalizes into a list of code points. */
static NTSTATUS norm_run(ULONG form, const WCHAR *src, SIZE_T length, NormList *out, SIZE_T *bad) {
  NormList decoded = {0};
  if (!norm_decode(src, length, &decoded, bad)) { norm_free(&decoded); return NORM_NO_UNICODE_TRANSLATION; }
  if (form == NORM_IDN) {
    const NTSTATUS mapped = norm_idn_map(&decoded, bad);
    if (mapped != STATUS_SUCCESS) { norm_free(&decoded); return mapped; }
    form = NORM_KC;
  }
  const BOOL compat = form == NORM_KC || form == NORM_KD;
  const BOOL compose = form == NORM_C || form == NORM_KC;
  for (SIZE_T i = 0; i < decoded.count; ++i) norm_decompose(out, decoded.items[i], compat);
  norm_free(&decoded);
  if (out->failed) return STATUS_NO_MEMORY;
  norm_reorder(out);
  if (compose) norm_compose(out);
  return STATUS_SUCCESS;
}

/* What Windows reports for a buffer of 0: a guess from the length of the source. */
static INT norm_estimate(INT length, BOOL compat) {
  INT n = length * (compat ? 18 : 3);
  if (n > 64) n = length + length / 8 > 64 ? length + length / 8 : 64;
  return n;
}

EXPORT NTSTATUS NTAPI RtlNormalizeString(ULONG form, const WCHAR *src, INT src_len, WCHAR *dst, INT *dst_len) {
  if (form == 0) return STATUS_INVALID_PARAMETER;
  if (form != NORM_C && form != NORM_D && form != NORM_KC && form != NORM_KD && form != NORM_IDN) return STATUS_OBJECT_NAME_NOT_FOUND;
  if (!src || !dst_len) return STATUS_INVALID_PARAMETER;
  if (src_len < 0) { SIZE_T n = 0; while (src[n]) ++n; src_len = (INT)n + 1; }
  if (!src_len) { *dst_len = 0; return STATUS_SUCCESS; }
  if (*dst_len <= 0) { *dst_len = norm_estimate(src_len, form == NORM_KC || form == NORM_KD); return STATUS_SUCCESS; }
  NormList list = {0};
  SIZE_T bad = 0;
  NTSTATUS status = norm_run(form, src, (SIZE_T)src_len, &list, &bad);
  if (status == NORM_NO_UNICODE_TRANSLATION) {
    /* Windows works through the text and meets a buffer that is too small before an invalid character that lies behind it: it then suggests a size from what is left */
    NormList prefix = {0};
    SIZE_T ignored = 0;
    const BOOL compat = form == NORM_KC || form == NORM_KD;
    const SIZE_T prefix_units = bad > 0 && bad <= (SIZE_T)src_len && src[bad - 1] >= 0xD800 && src[bad - 1] < 0xDC00 ? bad - 1 : bad;
    norm_run(compat ? NORM_KD : NORM_D, src, prefix_units, &prefix, &ignored);
    const SIZE_T written = prefix.count;
    norm_free(&prefix);
    norm_free(&list);
    if (written > (SIZE_T)*dst_len) {
      const INT have = *dst_len, rest = src_len > have ? src_len - have : 0;
      INT estimate = rest * (compat ? 18 : 3);
      *dst_len = compat && estimate > 64 ? have + 72 : have + estimate + 1;
      return NORM_BUFFER_TOO_SMALL;
    }
    *dst_len = (INT)bad;
    return status;
  }
  if (!NT_SUCCESS(status)) { norm_free(&list); return status; }
  const SIZE_T needed = norm_utf16_length(&list);
  if (needed > (SIZE_T)*dst_len) { norm_free(&list); *dst_len = needed > 0x7fffffff ? 0x7fffffff : (INT)needed; return NORM_BUFFER_TOO_SMALL; }
  SIZE_T n = 0;
  for (SIZE_T i = 0; i < list.count; ++i) {
    const DWORD cp = list.items[i];
    if (cp >= 0x10000) { dst[n++] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)); dst[n++] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
    else dst[n++] = (WCHAR)cp;
  }
  norm_free(&list);
  *dst_len = (INT)n;
  return STATUS_SUCCESS;
}

EXPORT NTSTATUS NTAPI RtlIsNormalizedString(ULONG form, const WCHAR *src, INT src_len, BOOLEAN *normalized) {
  if (!normalized) return STATUS_INVALID_PARAMETER;
  if (form != NORM_C && form != NORM_D && form != NORM_KC && form != NORM_KD) return form == 0 ? STATUS_INVALID_PARAMETER : STATUS_OBJECT_NAME_NOT_FOUND;
  if (!src) return STATUS_INVALID_PARAMETER;
  if (src_len < 0) { SIZE_T n = 0; while (src[n]) ++n; src_len = (INT)n; }
  NormList list = {0};
  SIZE_T bad = 0;
  const NTSTATUS status = norm_run(form, src, (SIZE_T)src_len, &list, &bad);
  if (!NT_SUCCESS(status)) { norm_free(&list); return status; }
  BOOL same = norm_utf16_length(&list) == (SIZE_T)src_len;
  for (SIZE_T i = 0, k = 0; same && i < list.count; ++i) {
    const DWORD cp = list.items[i];
    if (cp >= 0x10000) { same = src[k] == (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)) && src[k + 1] == (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF)); k += 2; }
    else { same = src[k] == (WCHAR)cp; ++k; }
  }
  norm_free(&list);
  *normalized = same;
  return STATUS_SUCCESS;
}
