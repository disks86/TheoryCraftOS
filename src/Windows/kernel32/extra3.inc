/* More of kernel32 (included by kernel32.c after extra2.inc): locale and string functions for the one locale there is (English, United States), atoms, process and module
   lists, completion ports and other small things. */

typedef long HRESULT;
typedef ULONG_PTR *PULONG_PTR;
#define ERROR_BUFFER_OVERFLOW 111
#define ERROR_PARTIAL_COPY 299
#define ERROR_OLD_WIN_VERSION 1150
static void copy_text(char *dst, const char *src) { while ((*dst++ = *src++) != 0) {} }

/* ---- characters ---------------------------------------------------------------------------------------------------------------------------------------- */

IMPORT WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR);
IMPORT WCHAR NTAPI RtlDowncaseUnicodeChar(WCHAR);
static WCHAR wide_lower(WCHAR c) { return RtlDowncaseUnicodeChar(c); }
static WCHAR wide_upper(WCHAR c) { return RtlUpcaseUnicodeChar(c); }
static BOOL wide_is_letter(WCHAR c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  if (c < 0x100) return (c >= 0xC0 && c != 0xD7 && c != 0xF7) || c == 0xAA || c == 0xB5 || c == 0xBA;
  return wide_lower(c) != c || wide_upper(c) != c || (c >= 0x100 && c <= 0x24F) || (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x52F) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3040 && c <= 0x30FF) || (c >= 0xAC00 && c <= 0xD7A3);
}

/* ---- GetStringType, LCMapString, folding ------------------------------------------------------------------------------------------------------------- */

#define C1_UPPER_ 1
#define C1_LOWER_ 2
#define C1_DIGIT_ 4
#define C1_SPACE_ 8
#define C1_PUNCT_ 0x10
#define C1_CNTRL_ 0x20
#define C1_BLANK_ 0x40
#define C1_XDIGIT_ 0x80
#define C1_ALPHA_ 0x100
#define C1_DEFINED_ 0x200
#include "ctype_data.inc"
static BOOL fold_digit(DWORD cp, DWORD *ascii);
/* The classes of a character: the three words of GetStringType. */
static void ctype_of(WCHAR c, WORD *c1, WORD *c2, WORD *c3) {
  SIZE_T lo = 0, hi = sizeof(k_ctype_runs) / sizeof(k_ctype_runs[0]);
  *c1 = *c2 = *c3 = 0;
  while (lo < hi) {
    const SIZE_T mid = lo + (hi - lo) / 2;
    if (c < k_ctype_runs[mid].first) hi = mid;
    else if (c > k_ctype_runs[mid].last) lo = mid + 1;
    else { *c1 = k_ctype_runs[mid].c1; *c2 = k_ctype_runs[mid].c2; *c3 = k_ctype_runs[mid].c3; break; }
  }
  DWORD ascii;
  if ((*c1 & C1_DIGIT_) && c != 0xB2 && c != 0xB3 && c != 0xB9 && !fold_digit(c, &ascii)) *c1 &= (WORD)~C1_DIGIT_;
}
static WORD ctype1(WCHAR c) { WORD c1, c2, c3; ctype_of(c, &c1, &c2, &c3); return c1; }
EXPORT BOOL WINAPI GetStringTypeW(DWORD info_type, LPCWSTR source, int count, LPWORD types) {
  if (!source || !types || (info_type != 1 && info_type != 2 && info_type != 4)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (count < 0) count = (int)wcslen(source);
  for (int i = 0; i < count; ++i) {
    WORD c1, c2, c3;
    ctype_of(source[i], &c1, &c2, &c3);
    types[i] = info_type == 1 ? c1 : info_type == 2 ? c2 : c3;
  }
  return TRUE;
}
EXPORT BOOL WINAPI GetStringTypeExW(LCID locale, DWORD info_type, LPCWSTR source, int count, LPWORD types) { (void)locale; return GetStringTypeW(info_type, source, count, types); }
EXPORT BOOL WINAPI GetStringTypeA(LCID locale, DWORD info_type, LPCSTR source, int count, LPWORD types) {
  (void)locale;
  if (!source || !types) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (count < 0) count = (int)strlen(source);
  WCHAR *w = RtlAllocateHeap(heap(), 0, ((SIZE_T)count + 1) * 2);
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  const int n = MultiByteToWideChar(CP_UTF8, 0, source, count, w, count);
  const BOOL ok = GetStringTypeW(info_type, w, n, types);
  RtlFreeHeap(heap(), 0, w);
  return ok;
}
EXPORT BOOL WINAPI GetStringTypeExA(LCID locale, DWORD info_type, LPCSTR source, int count, LPWORD types) { return GetStringTypeA(locale, info_type, source, count, types); }

/* ---- FoldString ------------------------------------------------------------------------------------------------------------------------------------ */

#define FOLD_CZONE 0x10u
#define FOLD_PRECOMPOSED 0x20u
#define FOLD_COMPOSITE 0x40u
#define FOLD_DIGITS 0x80u
#define FOLD_LIGATURES 0x2000u
/* The scripts' digits MAP_FOLDDIGITS turns into ASCII: the first character, the digit it stands for and how many follow (the numbers of Windows, not all of Unicode). */
static const struct { DWORD first; BYTE digit, count; } k_fold_digits[] = {{0x0030,0,10},{0x00b2,2,2},{0x00b9,1,1},{0x0660,0,10},{0x06f0,0,10},{0x07c0,0,10},{0x0966,0,10},{0x09e6,0,10},{0x0a66,0,10},{0x0ae6,0,10},{0x0b66,0,10},{0x0be6,0,10},{0x0c66,0,10},{0x0c78,0,4},{0x0c7c,1,3},{0x0ce6,0,10},{0x0d66,0,10},{0x0de6,0,10},{0x0e50,0,10},{0x0ed0,0,10},{0x0f20,0,10},{0x1040,0,10},{0x1090,0,10},{0x1369,1,9},{0x17e0,0,10},{0x1810,0,10},{0x1946,0,10},{0x19d0,0,10},{0x19da,1,1},{0x1a80,0,10},{0x1a90,0,10},{0x1b50,0,10},{0x1bb0,0,10},{0x1c40,0,10},{0x1c50,0,10},{0x2070,0,1},{0x2074,4,6},{0x2080,0,10},{0x2460,1,9},{0x2474,1,9},{0x2488,1,9},{0x24ea,0,1},{0x24f5,1,9},{0x24ff,0,1},{0x2776,1,9},{0x2780,1,9},{0x278a,1,9},{0x3007,0,1},{0x3021,1,9},{0xa620,0,10},{0xa8d0,0,10},{0xa8e0,0,10},{0xa900,0,10},{0xa9d0,0,10},{0xa9f0,0,10},{0xaa50,0,10},{0xabf0,0,10},{0xff10,0,10},{0x10107,1,9},{0x10320,1,1},{0x10321,5,1},{0x104a0,0,10},{0x10a40,1,4},{0x10d30,0,10},{0x10d40,0,10},{0x10e60,1,9},{0x11052,1,9},{0x11066,0,10},{0x110f0,0,10},{0x11136,0,10},{0x111d0,0,10},{0x112f0,0,10},{0x11450,0,10},{0x114d0,0,10},{0x11650,0,10},{0x116c0,0,10},{0x116d0,0,10},{0x116da,0,10},{0x11730,0,10},{0x118e0,0,10},{0x11950,0,10},{0x11bf0,0,10},{0x11c50,0,10},{0x11d50,0,10},{0x11da0,0,10},{0x11de0,0,10},{0x11f50,0,10},{0x16130,0,10},{0x16a60,0,10},{0x16ac0,0,10},{0x16b50,0,10},{0x16d70,0,10},{0x1ccf0,0,10},{0x1d7ce,0,10},{0x1d7d8,0,10},{0x1d7e2,0,10},{0x1d7ec,0,10},{0x1d7f6,0,10},{0x1e140,0,10},{0x1e2f0,0,10},{0x1e4f0,0,10},{0x1e5f1,0,10},{0x1e950,0,10},{0x1f100,0,1},{0x1f101,0,10},{0x1fbf0,0,10}};
/* What MAP_EXPAND_LIGATURES makes of a character (at most 3 characters). */
static const struct { WCHAR from; WCHAR to[3]; } k_ligatures[] = {
  {0x00c6, {'A', 'E', 0}}, {0x00de, {'T', 'H', 0}}, {0x00df, {'s', 's', 0}}, {0x00e6, {'a', 'e', 0}}, {0x00fe, {'t', 'h', 0}}, {0x0132, {'I', 'J', 0}}, {0x0133, {'i', 'j', 0}},
  {0x0152, {'O', 'E', 0}}, {0x0153, {'o', 'e', 0}}, {0x01c4, {'D', 0x017d, 0}}, {0x01c5, {'D', 0x017e, 0}}, {0x01c6, {'d', 0x017e, 0}}, {0x01c7, {'L', 'J', 0}},
  {0x01c8, {'L', 'j', 0}}, {0x01c9, {'l', 'j', 0}}, {0x01ca, {'N', 'J', 0}}, {0x01cb, {'N', 'j', 0}}, {0x01cc, {'n', 'j', 0}}, {0x01e2, {0x0100, 0x0112, 0}},
  {0x01e3, {0x0101, 0x0113, 0}}, {0x01f1, {'D', 'Z', 0}}, {0x01f2, {'D', 'z', 0}}, {0x01f3, {'d', 'z', 0}}, {0x01fc, {0x00c1, 0x00c9, 0}}, {0x01fd, {0x00e1, 0x00e9, 0}},
  {0x05f0, {0x05d5, 0x05d5, 0}}, {0x05f1, {0x05d5, 0x05d9, 0}}, {0x05f2, {0x05d9, 0x05d9, 0}}, {0xfb00, {'f', 'f', 0}}, {0xfb01, {'f', 'i', 0}}, {0xfb02, {'f', 'l', 0}},
  {0xfb03, {'f', 'f', 'i'}}, {0xfb04, {'f', 'f', 'l'}}, {0xfb05, {0x017f, 't', 0}}, {0xfb06, {'s', 't', 0}},
};
static BOOL fold_digit(DWORD cp, DWORD *ascii) {
  for (SIZE_T i = 0; i < sizeof(k_fold_digits) / sizeof(k_fold_digits[0]); ++i)
    if (cp >= k_fold_digits[i].first && cp - k_fold_digits[i].first < k_fold_digits[i].count) { *ascii = '0' + k_fold_digits[i].digit + (cp - k_fold_digits[i].first); return TRUE; }
  return FALSE;
}
NTSTATUS NTAPI RtlNormalizeString(ULONG form, const WCHAR *src, INT src_len, WCHAR *dst, INT *dst_len);
EXPORT int WINAPI FoldStringW(DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity) {
  const DWORD valid = FOLD_CZONE | FOLD_PRECOMPOSED | FOLD_COMPOSITE | FOLD_DIGITS | FOLD_LIGATURES;
  if (!flags || (flags & ~valid) || ((flags & FOLD_PRECOMPOSED) && (flags & (FOLD_COMPOSITE | FOLD_LIGATURES))) || ((flags & FOLD_COMPOSITE) && (flags & FOLD_LIGATURES))) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (!source || !count || capacity < 0 || (capacity > 0 && !dest) || source == dest) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const SIZE_T n = count < 0 ? wcslen(source) + 1 : (SIZE_T)count;
  /* the work buffer: every character can become at most three, and a normalization of the result is made in a second buffer */
  WCHAR *work = RtlAllocateHeap(heap(), 0, (n * 3 + 1) * 2);
  if (!work) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  SIZE_T length = 0;
  for (SIZE_T i = 0; i < n; ++i) {
    DWORD cp = source[i];
    BOOL pair = FALSE;
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && (source[i + 1] & 0xFC00) == 0xDC00) { cp = 0x10000 + ((cp - 0xD800) << 10) + (source[i + 1] - 0xDC00); pair = TRUE; }
    DWORD ascii;
    if ((flags & FOLD_DIGITS) && fold_digit(cp, &ascii)) { work[length++] = (WCHAR)ascii; if (pair) ++i; continue; }
    if (flags & FOLD_LIGATURES) {
      BOOL expanded = FALSE;
      for (SIZE_T k = 0; !pair && k < sizeof(k_ligatures) / sizeof(k_ligatures[0]); ++k)
        if (k_ligatures[k].from == cp) { for (int c = 0; c < 3 && k_ligatures[k].to[c]; ++c) work[length++] = k_ligatures[k].to[c]; expanded = TRUE; break; }
      if (expanded) continue;
    }
    work[length++] = source[i];
    if (pair) work[length++] = source[++i];
  }
  const WCHAR *result = work;
  SIZE_T result_length = length;
  WCHAR *normalized = NULL;
  if (flags & (FOLD_CZONE | FOLD_PRECOMPOSED | FOLD_COMPOSITE)) {
    const ULONG form = (flags & FOLD_COMPOSITE) ? ((flags & FOLD_CZONE) ? 6 : 2) : ((flags & FOLD_PRECOMPOSED) ? ((flags & FOLD_CZONE) ? 5 : 1) : 5);
    INT room = (INT)(length * 18 + 8);
    normalized = RtlAllocateHeap(heap(), 0, (SIZE_T)room * 2);
    if (normalized && RtlNormalizeString(form, work, (INT)length, normalized, &room) == STATUS_SUCCESS) { result = normalized; result_length = (SIZE_T)room; }
  }
  int ret;
  if (capacity == 0) ret = (int)result_length;
  else if ((SIZE_T)capacity < result_length) { set_error(ERROR_INSUFFICIENT_BUFFER); ret = 0; }
  else { memcpy(dest, result, result_length * 2); ret = (int)result_length; }
  RtlFreeHeap(heap(), 0, work);
  if (normalized) RtlFreeHeap(heap(), 0, normalized);
  return ret;
}
EXPORT int WINAPI FoldStringA(DWORD flags, LPCSTR source, int count, LPSTR dest, int capacity) { (void)flags; if (count < 0) count = (int)strlen(source) + 1; if (capacity == 0) return count; if (capacity < count) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; } memcpy(dest, source, (size_t_)count); return count; }
EXPORT int WINAPI CompareStringOrdinal(LPCWSTR a, int an, LPCWSTR b, int bn, BOOL ignore_case) {
  if (!a || !b) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (an < 0) an = (int)wcslen(a);
  if (bn < 0) bn = (int)wcslen(b);
  for (int i = 0; i < an && i < bn; ++i) {
    WCHAR x = a[i], y = b[i];
    if (ignore_case) { x = wide_upper(x); y = wide_upper(y); }
    if (x != y) return x < y ? 1 /* CSTR_LESS_THAN */ : 3 /* CSTR_GREATER_THAN */;
  }
  return an == bn ? 2 /* CSTR_EQUAL */ : an < bn ? 1 : 3;
}
#define FIND_STARTSWITH_ 0x00100000u
#define FIND_ENDSWITH_ 0x00200000u
#define FIND_FROMSTART_ 0x00400000u
#define FIND_FROMEND_ 0x00800000u
#define FIND_MODES_ (FIND_STARTSWITH_ | FIND_ENDSWITH_ | FIND_FROMSTART_ | FIND_FROMEND_)
/* Where a value is in a text, for one of the four modes: the start of the first match in `first` order, or of the last one. `same(start, length)` says whether the text from
 * start for length units equals the value; for a match the length is returned in *found. */
typedef BOOL (*FindMatcher)(void *context, int start, int length, int *found);
static int find_in(DWORD mode, int count, FindMatcher matcher, void *context, int *found) {
  if (mode == FIND_STARTSWITH_) return matcher(context, 0, count, found) ? 0 : -1;
  if (mode == FIND_ENDSWITH_) {
    for (int start = count; start >= 0; --start) { int length; if (matcher(context, start, count - start, &length) && length == count - start) { *found = length; return start; } }
    return -1;
  }
  if (mode == FIND_FROMEND_) {
    for (int start = count; start >= 0; --start) if (matcher(context, start, count - start, found)) return start;
    return -1;
  }
  for (int start = 0; start <= count; ++start) if (matcher(context, start, count - start, found)) return start;
  return -1;
}

typedef struct { const WCHAR *source, *value; int value_count; BOOL ignore_case; } OrdinalFind;
static BOOL ordinal_match(void *context, int start, int available, int *found) {
  const OrdinalFind *f = context;
  if (f->value_count > available) return FALSE;
  for (int i = 0; i < f->value_count; ++i) {
    WCHAR x = f->source[start + i], y = f->value[i];
    if (f->ignore_case) { x = wide_upper(x); y = wide_upper(y); }
    if (x != y) return FALSE;
  }
  *found = f->value_count;
  return TRUE;
}
EXPORT int WINAPI FindStringOrdinal(DWORD flags, LPCWSTR source, int source_count, LPCWSTR value, int value_count, BOOL ignore_case) {
  if (flags != FIND_STARTSWITH_ && flags != FIND_ENDSWITH_ && flags != FIND_FROMSTART_ && flags != FIND_FROMEND_) { set_error(ERROR_INVALID_FLAGS); return -1; }
  if (!source || !value || source_count < -1 || value_count < -1) { set_error(ERROR_INVALID_PARAMETER); return -1; }
  if (source_count < 0) source_count = (int)wcslen(source);
  if (value_count < 0) value_count = (int)wcslen(value);
  OrdinalFind f = { source, value, value_count, ignore_case };
  int found = 0;
  set_error(0);
  if (value_count == 0) return flags == FIND_FROMEND_ || flags == FIND_ENDSWITH_ ? source_count : 0;
  return find_in(flags, source_count, ordinal_match, &f, &found);
}

/* A text as the linguistic comparison sees it: canonically decomposed, and with the case, the nonspacing marks and the symbols left out as the flags ask. On the heap. */
static WCHAR *find_prepare(DWORD flags, const WCHAR *text, int count, int *length) {
  INT room = count * 18 + 8;
  WCHAR *normalized = RtlAllocateHeap(heap(), 0, (SIZE_T)room * 2);
  if (!normalized) return NULL;
  if (count == 0) { *length = 0; return normalized; }
  if (RtlNormalizeString(2, text, count, normalized, &room) != STATUS_SUCCESS) { memcpy(normalized, text, (SIZE_T)count * 2); room = count; }
  int out = 0;
  for (int i = 0; i < room; ++i) {
    WCHAR c = normalized[i];
    WORD c1, c2, c3;
    ctype_of(c, &c1, &c2, &c3);
    if ((flags & 2 /* NORM_IGNORENONSPACE */) && (c3 & 1 /* C3_NONSPACING */)) continue;
    if ((flags & 4 /* NORM_IGNORESYMBOLS */) && (c1 & (C1_PUNCT_ | C1_SPACE_) || c3 & 8 /* C3_SYMBOL */)) continue;
    if (flags & (1 /* NORM_IGNORECASE */ | 0x10 /* LINGUISTIC_IGNORECASE */)) c = wide_upper(c);
    normalized[out++] = c;
  }
  *length = out;
  return normalized;
}
typedef struct { const WCHAR *source; int source_count; const WCHAR *prepared_value; int prepared_count; DWORD flags; } LinguisticFind;
static BOOL linguistic_match(void *context, int start, int available, int *found) {
  const LinguisticFind *f = context;
  /* the shortest stretch of the source whose preparation equals the value */
  for (int length = 0; length <= available; ++length) {
    int prepared_count;
    WCHAR *prepared = find_prepare(f->flags, f->source + start, length, &prepared_count);
    if (!prepared) return FALSE;
    const BOOL same = prepared_count == f->prepared_count && memcmp(prepared, f->prepared_value, (SIZE_T)prepared_count * 2) == 0;
    RtlFreeHeap(heap(), 0, prepared);
    if (same && (length > 0 || f->prepared_count == 0)) { *found = length; return TRUE; }
    if (prepared_count > f->prepared_count + 4) return FALSE;
  }
  return FALSE;
}
EXPORT int WINAPI FindNLSStringEx(LPCWSTR locale_name, DWORD flags, LPCWSTR source, int source_count, LPCWSTR value, int value_count, LPINT found_length, LPVOID version, LPVOID reserved, LPARAM sort_handle) {
  (void)version; (void)reserved; (void)sort_handle;
  const DWORD mode = flags & FIND_MODES_;
  if ((locale_name && !nls_locale_of_name(locale_name)) || !source || !value || source_count < -1 || value_count < -1 || (mode != FIND_STARTSWITH_ && mode != FIND_ENDSWITH_ && mode != FIND_FROMSTART_ && mode != FIND_FROMEND_)) {
    set_error(ERROR_INVALID_PARAMETER);
    return -1;
  }
  if (source_count < 0) source_count = (int)wcslen(source);
  if (value_count < 0) value_count = (int)wcslen(value);
  int prepared_count;
  WCHAR *prepared = find_prepare(flags, value, value_count, &prepared_count);
  if (!prepared) { set_error(ERROR_NOT_ENOUGH_MEMORY); return -1; }
  LinguisticFind f = { source, source_count, prepared, prepared_count, flags };
  int found = 0;
  set_error(0);
  const int at = find_in(mode, source_count, linguistic_match, &f, &found);
  RtlFreeHeap(heap(), 0, prepared);
  if (at >= 0 && found_length) *found_length = found;
  return at;
}
EXPORT int WINAPI FindNLSString(LCID locale, DWORD flags, LPCWSTR source, int source_count, LPCWSTR value, int value_count, LPINT found_length) { (void)locale; return FindNLSStringEx(NULL, flags, source, source_count, value, value_count, found_length, NULL, NULL, 0); }
NTSTATUS NTAPI RtlNormalizeString(ULONG form, const WCHAR *src, INT src_len, WCHAR *dst, INT *dst_len);
NTSTATUS NTAPI RtlIsNormalizedString(ULONG form, const WCHAR *src, INT src_len, BOOLEAN *normalized);
EXPORT BOOL WINAPI IsNormalizedString(int form, LPCWSTR text, int count) {
  BOOLEAN normalized = FALSE;
  if (!text || count == 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const NTSTATUS status = RtlIsNormalizedString((ULONG)form, text, count, &normalized);
  if (!NT_SUCCESS(status)) { set_error(status == ((NTSTATUS)0xC0000717) ? 1113 : RtlNtStatusToDosError(status)); return FALSE; }
  return normalized;
}
/* A negative result is the size needed (ERROR_INSUFFICIENT_BUFFER) or minus the index of the invalid character (ERROR_NO_UNICODE_TRANSLATION). */
EXPORT int WINAPI NormalizeString(int form, LPCWSTR source, int count, LPWSTR dest, int capacity) {
  if (!source || count == 0 || capacity < 0 || (capacity > 0 && !dest)) {
    if (count == 0 && source) { set_error(0); return 0; }
    set_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  INT length = capacity;
  const NTSTATUS status = RtlNormalizeString((ULONG)form, source, count, dest, &length);
  if (status == STATUS_SUCCESS) { set_error(0); return length; }
  if (status == ((NTSTATUS)0xC0000023)) { set_error(ERROR_INSUFFICIENT_BUFFER); return -length; }
  if (status == ((NTSTATUS)0xC0000717)) { set_error(1113); return -length; }
  set_error(status == STATUS_NO_MEMORY ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
  return 0;
}

/* ---- locale information (English, United States) --------------------------------------------------------------------------------------------------------- */

NTSTATUS NTAPI RtlGetSystemPreferredUILanguages(DWORD flags, PVOID unknown, ULONG *count, WCHAR *buffer, ULONG *size);
NTSTATUS NTAPI RtlGetUserPreferredUILanguages(DWORD flags, PVOID unknown, ULONG *count, WCHAR *buffer, ULONG *size);
NTSTATUS NTAPI RtlGetProcessPreferredUILanguages(DWORD flags, ULONG *count, WCHAR *buffer, ULONG *size);
NTSTATUS NTAPI RtlGetThreadPreferredUILanguages(DWORD flags, ULONG *count, WCHAR *buffer, ULONG *size);
NTSTATUS NTAPI RtlSetThreadPreferredUILanguages(DWORD flags, const WCHAR *languages, ULONG *count);
NTSTATUS NTAPI RtlSetProcessPreferredUILanguages(DWORD flags, const WCHAR *languages, ULONG *count);
USHORT NTAPI RtlThreadUILanguageInternal(void);
int WINAPI LCIDToLocaleName(LCID lcid, LPWSTR name, int capacity, DWORD flags);
static BOOL ui_result(NTSTATUS status) { if (status == STATUS_SUCCESS) return TRUE; set_error(RtlNtStatusToDosError(status)); return FALSE; }
EXPORT BOOL WINAPI GetUserPreferredUILanguages(DWORD flags, PULONG count, LPWSTR languages, PULONG length) { return ui_result(RtlGetUserPreferredUILanguages(flags, NULL, count, languages, length)); }
EXPORT BOOL WINAPI GetSystemPreferredUILanguages(DWORD flags, PULONG count, LPWSTR languages, PULONG length) { return ui_result(RtlGetSystemPreferredUILanguages(flags, NULL, count, languages, length)); }
EXPORT BOOL WINAPI GetThreadPreferredUILanguages(DWORD flags, PULONG count, LPWSTR languages, PULONG length) { return ui_result(RtlGetThreadPreferredUILanguages(flags, count, languages, length)); }
EXPORT BOOL WINAPI GetProcessPreferredUILanguages(DWORD flags, PULONG count, LPWSTR languages, PULONG length) { return ui_result(RtlGetProcessPreferredUILanguages(flags, count, languages, length)); }
EXPORT BOOL WINAPI SetThreadPreferredUILanguages(DWORD flags, PCWSTR languages, PULONG count) { return ui_result(RtlSetThreadPreferredUILanguages(flags, languages, count)); }
EXPORT BOOL WINAPI SetProcessPreferredUILanguages(DWORD flags, PCWSTR languages, PULONG count) { return ui_result(RtlSetProcessPreferredUILanguages(flags, languages, count)); }
/* SetThreadUILanguage(0) just reports the language; any other makes it the language of the thread. */
EXPORT LANGID WINAPI SetThreadUILanguage(LANGID language) {
  if (language) {
    WCHAR name[85];
    if (LCIDToLocaleName(language, name, 85, 0) <= 0) return 0;
    WCHAR list[88] = {0};
    memcpy(list, name, wcslen(name) * 2);
    if (!ui_result(RtlSetThreadPreferredUILanguages(0x8, list, NULL))) return 0;
    return language;
  }
  return RtlThreadUILanguageInternal();
}

/* ---- atoms ----------------------------------------------------------------------------------------------------------------------------------------------- */

/* One table serves the local and the global atoms (a program is the only user of its global atoms). Atoms 0xC000.. name strings; integer atoms are below 0xC000. */
#define ATOM_MAX 256
typedef struct { WCHAR *name; DWORD refs; BOOL global; } AtomEntry;
static AtomEntry g_atoms[ATOM_MAX];
static CRITICAL_SECTION g_atom_lock;
static volatile LONG g_atom_ready;
static void atom_lock(void) {
  if (g_atom_ready != 2) {
    if (InterlockedCompareExchange(&g_atom_ready, 1, 0) == 0) { RtlInitializeCriticalSection(&g_atom_lock); g_atom_ready = 2; }
    else while (g_atom_ready != 2) SwitchToThread();
  }
  EnterCriticalSection(&g_atom_lock);
}
static WCHAR wide_fold(WCHAR c) { return wide_upper(c); }
static BOOL atom_equal(const WCHAR *a, const WCHAR *b) { while (*a && *b) { if (wide_fold(*a) != wide_fold(*b)) return FALSE; ++a; ++b; } return *a == *b; }
static ATOM atom_add(LPCWSTR name, BOOL global) {
  if (!name) return 0;      /* fails without an error code */
  if (((ULONG_PTR)name >> 16) == 0) {      /* MAKEINTATOM */
    const ULONG_PTR id = (ULONG_PTR)name;
    if (id == 0) return 0;      /* fails without an error code */
    if (id >= 0xC000) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    return (ATOM)id;
  }
  if (name[0] == '#') {      /* "#123" is the integer atom 123 */
    ULONG_PTR v = 0; const WCHAR *c = name + 1; BOOL digits = *c != 0;
    for (; *c; ++c) { if (*c < '0' || *c > '9') { digits = FALSE; break; } v = v * 10 + (ULONG_PTR)(*c - '0'); if (v > 0xFFFF) break; }
    if (digits) { if (v == 0 || v >= 0xC000) { set_error(ERROR_INVALID_PARAMETER); return 0; } return (ATOM)v; }
  }
  const size_t_ len = wcslen(name);
  if (len == 0 || len > 255) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  atom_lock();
  int free_slot = -1;
  for (int i = 0; i < ATOM_MAX; ++i) {
    if (!g_atoms[i].name) { if (free_slot < 0) free_slot = i; continue; }
    if (g_atoms[i].global == global && atom_equal(g_atoms[i].name, name)) { ++g_atoms[i].refs; LeaveCriticalSection(&g_atom_lock); return (ATOM)(0xC000 + i); }
  }
  ATOM result = 0;
  if (free_slot < 0) set_error(ERROR_NOT_ENOUGH_MEMORY);
  else {
    WCHAR *copy = RtlAllocateHeap(heap(), 0, (len + 1) * 2);
    if (!copy) set_error(ERROR_NOT_ENOUGH_MEMORY);
    else { memcpy(copy, name, (len + 1) * 2); g_atoms[free_slot].name = copy; g_atoms[free_slot].refs = 1; g_atoms[free_slot].global = global; result = (ATOM)(0xC000 + free_slot); }
  }
  LeaveCriticalSection(&g_atom_lock);
  return result;
}
static ATOM atom_find(LPCWSTR name, BOOL global) {
  if (!name) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (((ULONG_PTR)name >> 16) == 0) return (ATOM)(ULONG_PTR)name;
  atom_lock();
  ATOM result = 0;
  for (int i = 0; i < ATOM_MAX; ++i) if (g_atoms[i].name && g_atoms[i].global == global && atom_equal(g_atoms[i].name, name)) { result = (ATOM)(0xC000 + i); break; }
  LeaveCriticalSection(&g_atom_lock);
  if (!result) set_error(ERROR_FILE_NOT_FOUND);
  return result;
}
static ATOM atom_delete(ATOM atom, BOOL global) {
  if (atom < 0xC000) return 0;
  const int i = atom - 0xC000;
  atom_lock();
  ATOM result = atom;
  if (i >= ATOM_MAX || !g_atoms[i].name || g_atoms[i].global != global) set_error(ERROR_INVALID_HANDLE);
  else { if (--g_atoms[i].refs == 0) { RtlFreeHeap(heap(), 0, g_atoms[i].name); g_atoms[i].name = NULL; } result = 0; }
  LeaveCriticalSection(&g_atom_lock);
  return result;
}
/* The text of an atom (an integer atom is "#n"); returns its length, or 0 with the error set. */
static UINT atom_text(ATOM atom, BOOL global, WCHAR *text) {
  if (atom == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (atom < 0xC000) {
    WCHAR digits[6]; int d = 0, t = 0; unsigned x = atom;
    do { digits[d++] = (WCHAR)('0' + x % 10); x /= 10; } while (x);
    text[t++] = '#'; while (d) text[t++] = digits[--d]; text[t] = 0;
    return (UINT)t;
  }
  const int i = atom - 0xC000;
  atom_lock();
  UINT result = 0;
  if (i >= ATOM_MAX || !g_atoms[i].name || g_atoms[i].global != global) set_error(ERROR_INVALID_HANDLE);
  else { result = (UINT)wcslen(g_atoms[i].name); memcpy(text, g_atoms[i].name, ((size_t_)result + 1) * 2); }
  LeaveCriticalSection(&g_atom_lock);
  return result;
}
/* The local atoms cut a name that does not fit and terminate it; with room for no character the call fails with ERROR_INSUFFICIENT_BUFFER. */
static BOOL atom_cut_local(UINT len, int capacity, UINT *n) {
  if (capacity <= 1) { set_error(capacity <= 0 ? ERROR_INVALID_PARAMETER : ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  *n = len < (UINT)capacity - 1 ? len : (UINT)capacity - 1;
  return TRUE;
}
/* Wide (global atoms): as many characters as fit; the terminator only when there is room for it. */
static UINT atom_name(ATOM atom, BOOL global, LPWSTR buffer, int capacity) {
  WCHAR text[260];
  const UINT len = atom_text(atom, global, text);
  if (len == 0) return 0;
  if (!global) {
    UINT cut;
    if (!atom_cut_local(len, capacity, &cut)) return 0;
    memcpy(buffer, text, (size_t_)cut * 2);
    buffer[cut] = 0;
    return cut;
  }
  if (capacity <= 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const UINT n = len < (UINT)capacity ? len : (UINT)capacity;
  memcpy(buffer, text, (size_t_)n * 2);
  if (n < (UINT)capacity) buffer[n] = 0;
  return n;
}
/* ANSI: a name that does not fit is cut short, terminated, and reported as ERROR_MORE_DATA with a length of 0. */
static UINT atom_name_a(ATOM atom, BOOL global, LPSTR buffer, int capacity) {
  WCHAR text[260];
  const UINT len = atom_text(atom, global, text);
  if (len == 0) return 0;
  UINT n;
  if (!global) { if (!atom_cut_local(len, capacity, &n)) return 0; }
  else { if (capacity <= 0) { set_error(ERROR_INVALID_PARAMETER); return 0; } n = len < (UINT)capacity - 1 ? len : (UINT)capacity - 1; }
  for (UINT i = 0; i < n; ++i) buffer[i] = (char)text[i];
  buffer[n] = 0;
  if (global && n < len) { set_error(ERROR_MORE_DATA); return 0; }
  return n;
}
#define ATOM_FROM_A(call) do { WCHAR *w_ = ((ULONG_PTR)name >> 16) ? to_wide(name) : (WCHAR *)name; ATOM r_ = (call); if (((ULONG_PTR)name >> 16)) free_wide(w_); return r_; } while (0)
EXPORT ATOM WINAPI AddAtomW(LPCWSTR name) { return atom_add(name, FALSE); }
EXPORT ATOM WINAPI FindAtomW(LPCWSTR name) { return atom_find(name, FALSE); }
EXPORT ATOM WINAPI DeleteAtom(ATOM atom) { return atom_delete(atom, FALSE); }
EXPORT UINT WINAPI GetAtomNameW(ATOM atom, LPWSTR buffer, int capacity) { return atom_name(atom, FALSE, buffer, capacity); }
EXPORT UINT WINAPI GetAtomNameA(ATOM atom, LPSTR buffer, int capacity) { return atom_name_a(atom, FALSE, buffer, capacity); }
EXPORT ATOM WINAPI GlobalAddAtomW(LPCWSTR name) { return atom_add(name, TRUE); }
EXPORT ATOM WINAPI GlobalFindAtomW(LPCWSTR name) { return atom_find(name, TRUE); }
EXPORT ATOM WINAPI GlobalDeleteAtom(ATOM atom) { return atom_delete(atom, TRUE); }
EXPORT UINT WINAPI GlobalGetAtomNameW(ATOM atom, LPWSTR buffer, int capacity) { return atom_name(atom, TRUE, buffer, capacity); }
EXPORT UINT WINAPI GlobalGetAtomNameA(ATOM atom, LPSTR buffer, int capacity) { return atom_name_a(atom, TRUE, buffer, capacity); }
EXPORT ATOM WINAPI AddAtomA(LPCSTR name) { ATOM_FROM_A(atom_add(w_, FALSE)); }
EXPORT ATOM WINAPI FindAtomA(LPCSTR name) { ATOM_FROM_A(atom_find(w_, FALSE)); }
EXPORT ATOM WINAPI GlobalAddAtomA(LPCSTR name) { ATOM_FROM_A(atom_add(w_, TRUE)); }
EXPORT ATOM WINAPI GlobalFindAtomA(LPCSTR name) { ATOM_FROM_A(atom_find(w_, TRUE)); }

/* ---- thread descriptions, memory of the process itself, processors, versions ------------------------------------------------------------------------------------- */

/* A thread's description is kept by the system (ThreadNameInformation): a UNICODE_STRING and the characters after it. */
EXPORT HRESULT WINAPI SetThreadDescription(HANDLE thread, LPCWSTR description) {
  struct { USHORT length, maximum; ULONG pad; PWSTR buffer; } name;
  const SIZE_T n = description ? wcslen(description) : 0;
  if (n > 255) return (HRESULT)0x80070057;
  name.length = name.maximum = (USHORT)(n * 2);
  name.pad = 0;
  name.buffer = (PWSTR)description;
  const NTSTATUS s = NtSetInformationThread(thread, 38, &name, sizeof(name));
  if (s == 0) return (HRESULT)0x10000000;      /* HRESULT_FROM_NT(STATUS_SUCCESS) */
  return s == STATUS_INVALID_HANDLE ? (HRESULT)0x80070006 : (HRESULT)((ULONG)s | 0x10000000u);
}
EXPORT HRESULT WINAPI GetThreadDescription(HANDLE thread, WCHAR **description) {
  BYTE small[16];
  ULONG needed = 0;
  NTSTATUS s = NtQueryInformationThread(thread, 38, small, 0, &needed);
  if (s != (NTSTATUS)0xC0000023 && s != 0) return s == STATUS_INVALID_HANDLE ? (HRESULT)0x80070006 : (HRESULT)((ULONG)s | 0x10000000u);
  BYTE *buffer = RtlAllocateHeap(heap(), 0, needed + 2);
  if (!buffer) return (HRESULT)0x8007000E;
  s = NtQueryInformationThread(thread, 38, buffer, needed, &needed);
  if (s != 0) { RtlFreeHeap(heap(), 0, buffer); return (HRESULT)((ULONG)s | 0x10000000u); }
  const USHORT bytes = *(USHORT *)buffer;
  WCHAR *text = LocalAlloc(0, (SIZE_T)bytes + 2);
  if (!text) { RtlFreeHeap(heap(), 0, buffer); return (HRESULT)0x8007000E; }
  memcpy(text, buffer + 16, bytes);
  text[bytes / 2] = 0;
  RtlFreeHeap(heap(), 0, buffer);
  *description = text;
  return (HRESULT)0x10000000;
}

EXPORT BOOL WINAPI ReadProcessMemory(HANDLE process, LPCVOID base, LPVOID buffer, SIZE_T size, SIZE_T *done) {
  SIZE_T got = 0;
  const NTSTATUS status = NtReadVirtualMemory(process, (PVOID)base, buffer, size, &got);
  if (done) *done = got;
  return NT_SUCCESS(status) && status != (NTSTATUS)0x8000000D ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI WriteProcessMemory(HANDLE process, LPVOID base, LPCVOID buffer, SIZE_T size, SIZE_T *done) {
  SIZE_T got = 0;
  NTSTATUS status = NtWriteVirtualMemory(process, base, (PVOID)buffer, size, &got);
  if (status == (NTSTATUS)0x8000000D && got == 0 && size != 0) {      /* the first page cannot be written: code pages are made writable for the copy, anything else is refused */
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQueryEx(process, base, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT) {
      if (info.Protect == PAGE_EXECUTE_READ || info.Protect == PAGE_EXECUTE) {
        DWORD old = 0;
        if (VirtualProtectEx(process, base, size, PAGE_EXECUTE_READWRITE, &old)) {
          status = NtWriteVirtualMemory(process, base, (PVOID)buffer, size, &got);
          DWORD again = 0;
          VirtualProtectEx(process, base, size, old, &again);
        }
      } else if (info.Protect == PAGE_READONLY || info.Protect == PAGE_NOACCESS) {
        set_error(ERROR_NOACCESS);
        return FALSE;
      }
    }
  }
  if (done) *done = got;
  return NT_SUCCESS(status) && status != (NTSTATUS)0x8000000D ? TRUE : fail_nt(status);
}
/* Attribute lists (the type is in kernel32.c, where CreateProcess reads them): a header and room for as many entries as were asked for; an entry names an attribute and points to
   the caller's value. Only the four fields of the header are written by Initialize (the rest of the memory stays as it was). */
EXPORT BOOL WINAPI InitializeProcThreadAttributeList(ProcThreadAttributeList *list, DWORD count, DWORD flags, PSIZE_T size) {
  if (flags || count > 27) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const SIZE_T needed = 24 + (SIZE_T)count * sizeof(ProcThreadAttributeEntry);
  const SIZE_T have = *size;
  *size = needed;
  if (!list || have < needed) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  list->flags = 0;
  list->capacity = count;
  list->count = 0;
  list->unknown = NULL;
  return TRUE;
}
EXPORT void WINAPI DeleteProcThreadAttributeList(ProcThreadAttributeList *list) { (void)list; }
EXPORT BOOL WINAPI UpdateProcThreadAttribute(ProcThreadAttributeList *list, DWORD flags, DWORD_PTR attribute, PVOID value, SIZE_T size, PVOID previous, PSIZE_T returned) {
  (void)previous; (void)returned;
  if (flags & ~1u) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (list->count >= list->capacity) { set_error(31 /* ERROR_GEN_FAILURE */); return FALSE; }
  switch (attribute & ~(DWORD_PTR)0x40000000) {      /* (PROC_THREAD_ATTRIBUTE_ADDITIVE is not part of the identity) */
    case 0x00020000: if (size != sizeof(HANDLE)) { set_error(24 /* ERROR_BAD_LENGTH */); return FALSE; } break;      /* PARENT_PROCESS */
    case 0x00020002: if (size == 0 || size % sizeof(HANDLE)) { set_error(24); return FALSE; } break;      /* HANDLE_LIST */
    case 0x00030003: if (size != sizeof(GROUP_AFFINITY)) { set_error(24); return FALSE; } break;      /* GROUP_AFFINITY */
    case 0x00030005: if (size != 4) { set_error(24); return FALSE; } break;      /* IDEAL_PROCESSOR (a PROCESSOR_NUMBER) */
    case 0x00020016: if (size != sizeof(HANDLE)) { set_error(24); return FALSE; } break;      /* PSEUDOCONSOLE */
    default: set_error(ERROR_NOT_SUPPORTED); return FALSE;
  }
  const DWORD bit = 1u << (attribute & 0xFFFF);
  if (list->flags & bit) { set_error(698 /* ERROR_OBJECT_NAME_EXISTS */); return FALSE; }
  list->entries[list->count].attribute = attribute;
  list->entries[list->count].size = size;
  list->entries[list->count].value = value;
  list->count++;
  list->flags |= bit;
  return TRUE;
}
EXPORT HANDLE WINAPI CreateRemoteThreadEx(HANDLE process, LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPVOID attributes, LPDWORD id) {
  (void)sa;
  const GROUP_AFFINITY *affinity = NULL;
  if (attributes) {      /* the group affinity attribute (group affinity | thread | input) says where the thread may run */
    const ProcThreadAttributeList *list = attributes;
    for (DWORD i = 0; i < list->count && i < list->capacity; ++i)
      if (list->entries[i].attribute == 0x30003 && list->entries[i].size >= sizeof(GROUP_AFFINITY)) affinity = list->entries[i].value;
  }
  DWORD_PTR process_mask = 0, system_mask = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
  if (affinity && (affinity->Group != 0 || (affinity->Mask & ~system_mask))) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE thread = NULL;
  const BOOL start_suspended = (flags & CREATE_SUSPENDED) != 0;
  PVOID active = NULL;
  if (process == NtCurrentProcess()) {      /* a thread of this program starts with the activation context that is active here */
    RtlGetActiveActivationContext(&active);
    if (active) {
      ActThreadStart *a = RtlAllocateHeap(heap(), 0, sizeof(*a));
      if (a) { a->start = start; a->parameter = parameter; a->context = active; start = actctx_thread_start; parameter = a; }
      else { RtlReleaseActivationContext(active); active = NULL; }
    }
  }
  const NTSTATUS status = NtCreateThreadEx(&thread, 0x1FFFFF, NULL, process, (PVOID)start, parameter, (start_suspended || affinity) ? 1 : 0, 0, stack, stack, NULL);
  if (!NT_SUCCESS(status)) {
    if (active) { RtlReleaseActivationContext(active); RtlFreeHeap(heap(), 0, parameter); }
    set_error(status == STATUS_NOT_IMPLEMENTED ? ERROR_NOT_SUPPORTED : RtlNtStatusToDosError(status));
    return NULL;
  }
  if (affinity) {
    ULONGLONG mask = affinity->Mask ? affinity->Mask : process_mask;
    NtSetInformationThread(thread, 4, &mask, sizeof(mask));
    if (!start_suspended) ResumeThread(thread);
  }
  if (id) {
    ULONG64 info[6];
    *id = NT_SUCCESS(NtQueryInformationThread(thread, 0, info, sizeof(info), NULL)) ? (DWORD)info[3] : 0;
  }
  return thread;
}
EXPORT HANDLE WINAPI CreateRemoteThread(HANDLE process, LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD id) {
  return CreateRemoteThreadEx(process, sa, stack, start, parameter, flags, NULL, id);
}

typedef struct { ULONG_PTR ProcessorMask; int Relationship; union { struct { BYTE Flags; } ProcessorCore; struct { DWORD NodeNumber; } NumaNode; ULONGLONG Reserved[2]; }; } SYSTEM_LOGICAL_PROCESSOR_INFORMATION_;
EXPORT BOOL WINAPI GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_ *info, PDWORD length) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const DWORD count = si.dwNumberOfProcessors;
  const DWORD need = (count + 1) * (DWORD)sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_);      /* a core entry each and one NUMA node */
  if (!length || *length < need || !info) { if (length) *length = need; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memset(info, 0, need);
  ULONG_PTR all = 0;
  for (DWORD i = 0; i < count; ++i) { info[i].ProcessorMask = (ULONG_PTR)1 << i; info[i].Relationship = 0 /* RelationProcessorCore */; all |= info[i].ProcessorMask; }
  info[count].ProcessorMask = all; info[count].Relationship = 1 /* RelationNumaNode */;
  *length = need;
  return TRUE;
}

/* The processors in the shapes Windows 10 gives them: SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX records and CPU sets, made by the kernel. */
EXPORT BOOL WINAPI GetLogicalProcessorInformationEx(DWORD relation, BYTE *buffer, PDWORD length) {
  if (!length) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  ULONG returned = 0;
  const NTSTATUS status = NtQuerySystemInformationEx(107 /* SystemLogicalProcessorInformationEx */, &relation, sizeof(relation), buffer, buffer ? *length : 0, &returned);
  if (status == (NTSTATUS)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */) { *length = returned; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *length = returned;
  return TRUE;
}
EXPORT BOOL WINAPI GetSystemCpuSetInformation(BYTE *info, ULONG length, ULONG *returned, HANDLE process, ULONG flags) {
  if (returned) *returned = 0;
  if (flags) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!info && length) { set_error(ERROR_NOACCESS); return FALSE; }
  if (!process) process = GetCurrentProcess();
  ULONG got = 0;
  const NTSTATUS status = NtQuerySystemInformationEx(175 /* SystemCpuSetInformation */, &process, sizeof(process), info, length, &got);
  if (returned && (NT_SUCCESS(status) || status == (NTSTATUS)0xC0000023)) *returned = got;
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
/* Every processor is in NUMA node 0 of processor group 0. */
EXPORT BOOL WINAPI GetNumaProcessorNode(UCHAR processor, PUCHAR node) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  if (processor >= si.dwNumberOfProcessors) { *node = 0xFF; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *node = 0;
  return TRUE;
}
typedef struct { WORD Group; BYTE Number, Reserved; } PROCESSOR_NUMBER_;
EXPORT BOOL WINAPI GetNumaProcessorNodeEx(PROCESSOR_NUMBER_ *processor, USHORT *node) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  if (processor->Group != 0 || processor->Number >= si.dwNumberOfProcessors || processor->Number >= 64) { *node = 0xFFFF; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *node = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetNumaHighestNodeNumber(PULONG node) { *node = 0; return TRUE; }
/* Windows 10: one session for everything that runs here. */
EXPORT BOOL WINAPI ProcessIdToSessionId(DWORD pid, DWORD *session) {
  if (!session) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  HANDLE process = pid == GetCurrentProcessId() ? GetCurrentProcess() : OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, FALSE, pid);
  if (!process) { if (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_INVALID_PARAMETER) set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  ULONG id = 0;
  const NTSTATUS status = NtQueryInformationProcess(process, 24 /* ProcessSessionInformation */, &id, sizeof(id), NULL);
  if (process != GetCurrentProcess()) CloseHandle(process);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *session = id;
  return TRUE;
}
EXPORT DWORD WINAPI WTSGetActiveConsoleSessionId(void) { return 1; }
EXPORT BOOL WINAPI GetSystemRegistryQuota(PDWORD allowed, PDWORD used) {
  if (allowed) *allowed = 0x2000000;      /* the registry may hold 32 MiB, and holds a little */
  if (used) *used = 0x180000;
  return TRUE;
}
/* Nothing here runs under WOW64: the machine of a program is "unknown" (it is the native one), the native machine is x86-64. */
EXPORT BOOL WINAPI IsWow64Process(HANDLE process, PBOOL wow64) {
  ULONG_PTR information = 0;
  const NTSTATUS status = NtQueryInformationProcess(process, 26 /* ProcessWow64Information */, &information, sizeof(information), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (wow64) *wow64 = information != 0;
  return TRUE;
}
EXPORT BOOL WINAPI IsWow64Process2(HANDLE process, USHORT *machine, USHORT *native_machine) {
  ULONG_PTR information = 0;
  const NTSTATUS status = NtQueryInformationProcess(process, 26 /* ProcessWow64Information */, &information, sizeof(information), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (machine) *machine = 0;      /* IMAGE_FILE_MACHINE_UNKNOWN */
  if (native_machine) *native_machine = 0x8664;      /* IMAGE_FILE_MACHINE_AMD64 */
  return TRUE;
}

/* Pseudo consoles: an object that stands for the console of a program started with PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE (the program's standard handles are its own console
   in that case). The pipes it was made with are not read or written: what the program prints stays in its console model. */
EXPORT HRESULT WINAPI CreatePseudoConsole(COORD size, HANDLE input, HANDLE output, DWORD flags, HANDLE *console) {
  if (!console || size.X <= 0 || size.Y <= 0 || (flags & ~7u)) return (HRESULT)0x80070057;      /* E_INVALIDARG */
  if (GetFileType(input) == FILE_TYPE_UNKNOWN || GetFileType(output) == FILE_TYPE_UNKNOWN) return (HRESULT)0x80070006;      /* E_HANDLE */
  HANDLE object = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!object) return (HRESULT)0x8007000E;      /* E_OUTOFMEMORY */
  *console = object;
  return 0;
}
EXPORT HRESULT WINAPI ResizePseudoConsole(HANDLE console, COORD size) { (void)console; return size.X > 0 && size.Y > 0 ? 0 : (HRESULT)0x80070057; }
EXPORT void WINAPI ClosePseudoConsole(HANDLE console) { CloseHandle(console); }

/* GetProcessInformation: only the machine of the process (ProcessMachineTypeInfo) is answered. */
EXPORT BOOL WINAPI GetProcessInformation(HANDLE process, int information_class, LPVOID information, DWORD size) {
  if (information_class != 9 /* ProcessMachineTypeInfo */) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (size != 8) { set_error(ERROR_BAD_LENGTH); return FALSE; }
  DWORD machines[8] = {};
  const NTSTATUS status = NtQuerySystemInformationEx(230 /* SystemSupportedProcessorArchitectures2 */, &process, sizeof(process), machines, sizeof(machines), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  for (int i = 0; i < 8 && (machines[i] & 0xFFFF); ++i) {
    if (!(machines[i] & (1u << 19))) continue;      /* (the one the process runs as) */
    BYTE *out = information;
    memset(out, 0, 8);
    *(WORD *)out = (WORD)(machines[i] & 0xFFFF);
    *(DWORD *)(out + 4) = ((machines[i] >> 17) & 1) | (((machines[i] >> 16) & 1) << 1) | (((machines[i] >> 20) & 1) << 2);      /* UserEnabled, KernelEnabled, Wow64Container */
    return TRUE;
  }
  set_error(ERROR_INVALID_PARAMETER);
  return FALSE;
}

/* VerifyVersionInfo and GetProductInfo: ntdll knows the version (RtlGetVersion, RtlVerifyVersionInfo). */
NTSTATUS NTAPI RtlVerifyVersionInfo(const void *info, ULONG types, ULONGLONG mask);
EXPORT BOOL WINAPI VerifyVersionInfoW(OSVERSIONINFOEXW *info, DWORD type_mask, ULONGLONG condition_mask) {
  if (!info) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const NTSTATUS status = RtlVerifyVersionInfo(info, type_mask, condition_mask);
  if (status == STATUS_SUCCESS) return TRUE;
  set_error(status == (NTSTATUS)0xC0000059 /* STATUS_REVISION_MISMATCH */ ? 1150 /* ERROR_OLD_WIN_VERSION */ : ERROR_INVALID_PARAMETER);
  return FALSE;
}
EXPORT BOOL WINAPI VerifyVersionInfoA(OSVERSIONINFOEXA *info, DWORD type_mask, ULONGLONG condition_mask) {
  if (!info) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  OSVERSIONINFOEXW wide;
  memset(&wide, 0, sizeof(wide));
  wide.dwOSVersionInfoSize = sizeof(wide);
  wide.dwMajorVersion = info->dwMajorVersion;
  wide.dwMinorVersion = info->dwMinorVersion;
  wide.dwBuildNumber = info->dwBuildNumber;
  wide.dwPlatformId = info->dwPlatformId;
  wide.wServicePackMajor = info->wServicePackMajor;      /* (the structure is taken to be the extended one whatever its size says) */
  wide.wServicePackMinor = info->wServicePackMinor;
  wide.wSuiteMask = info->wSuiteMask;
  wide.wProductType = info->wProductType;
  return VerifyVersionInfoW(&wide, type_mask, condition_mask);
}
/* The product of a Windows version: before Vista there is none (FALSE, 0, the last error left alone), else this system is Windows 10 Pro. */
EXPORT BOOL WINAPI GetProductInfo(DWORD major, DWORD minor, DWORD sp_major, DWORD sp_minor, PDWORD type) {
  (void)minor; (void)sp_major; (void)sp_minor;
  if (!type) return FALSE;      /* (the last error is left alone) */
  if (major < 6) { *type = 0; return FALSE; }
  *type = 0x30;      /* PRODUCT_PROFESSIONAL */
  return TRUE;
}

/* ---- I/O completion ports (no file I/O here is asynchronous, so a port is a queue that programs post to) ---------------------------------------------------- */

#define PORT_MAX 16
#define PORT_QUEUE 1024
typedef struct { DWORD bytes; ULONG_PTR key; LPOVERLAPPED overlapped; } PortPacket;
typedef struct { HANDLE semaphore; PortPacket packets[PORT_QUEUE]; DWORD head, count; HANDLE jobs[4]; int job_count; } Port;      /* (jobs: the job objects whose events come out of this port) */
static Port *g_ports[PORT_MAX];
static Port *port_of(HANDLE h) {
  Port *p = NULL;
  atom_lock();
  for (int i = 0; i < PORT_MAX; ++i) if (g_ports[i] && g_ports[i]->semaphore == h) { p = g_ports[i]; break; }
  LeaveCriticalSection(&g_atom_lock);
  return p;
}
/* The files tied to a port: an overlapped call that finishes on one queues a packet (its key, the bytes moved, the OVERLAPPED) there, unless the low bit of the event
   handle is set. */
/* ReadFileScatter / WriteFileGather: the bytes go from (to) a list of page-sized buffers, ending with a null entry, to (from) the file at the offset of the OVERLAPPED. The
 * file has to be one opened for overlapped calls. The work is done at once, the call says what an asynchronous one says and the completion is there for the event and the port. */
typedef union { PVOID Buffer; ULONGLONG Alignment; } FILE_SEGMENT_ELEMENT_;
static BOOL scatter_gather(HANDLE h, FILE_SEGMENT_ELEMENT_ *segments, DWORD bytes, LPOVERLAPPED overlapped, BOOL write) {
  if (!overlapped || !segments || !overlapped_known(h)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  LARGE_INTEGER offset;
  offset.LowPart = overlapped->Offset;
  offset.HighPart = (LONG)overlapped->OffsetHigh;
  overlapped->Internal = (ULONG_PTR)STATUS_PENDING;
  DWORD done = 0;
  NTSTATUS status = STATUS_SUCCESS;
  while (done < bytes && segments->Buffer) {
    const DWORD n = bytes - done < 4096 ? bytes - done : 4096;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER at;
    at.QuadPart = offset.QuadPart + done;
    iosb.Information = 0;
    status = write ? NtWriteFile(h, NULL, NULL, NULL, &iosb, segments->Buffer, n, &at, NULL) : NtReadFile(h, NULL, NULL, NULL, &iosb, segments->Buffer, n, &at, NULL);
    if (!NT_SUCCESS(status)) break;
    done += (DWORD)iosb.Information;
    if (iosb.Information < n) break;
    ++segments;
  }
  if (done == 0 && status != STATUS_SUCCESS) {
    complete_overlapped(overlapped, status, 0);
    set_error(RtlNtStatusToDosError(status));
    return FALSE;
  }
  complete_overlapped(overlapped, STATUS_SUCCESS, done);
  port_note(h, overlapped, done);
  set_error(ERROR_IO_PENDING);
  return FALSE;
}
EXPORT BOOL WINAPI ReadFileScatter(HANDLE h, FILE_SEGMENT_ELEMENT_ *segments, DWORD bytes, LPDWORD reserved, LPOVERLAPPED overlapped) { (void)reserved; return scatter_gather(h, segments, bytes, overlapped, FALSE); }
EXPORT BOOL WINAPI WriteFileGather(HANDLE h, FILE_SEGMENT_ELEMENT_ *segments, DWORD bytes, LPDWORD reserved, LPOVERLAPPED overlapped) { (void)reserved; return scatter_gather(h, segments, bytes, overlapped, TRUE); }
/* FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (1) and FILE_SKIP_SET_EVENT_ON_HANDLE (2): a call here is done before it returns, and tells so only by the packet at the port; the modes are accepted. */
EXPORT BOOL WINAPI SetFileCompletionNotificationModes(HANDLE h, UCHAR modes) {
  if (modes & ~3) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (device_type(h) < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  return TRUE;
}
/* SetFileValidData: tells the file system that the data up to `length` is to be taken as written, which takes the privilege to manage volumes. The tokens here are one fixed user
 * without any privilege that can be enabled (see advapi32), so a handle that may write is told that it is not held. */
EXPORT BOOL WINAPI SetFileValidData(HANDLE h, LONGLONG length) {
  (void)length;
  IO_STATUS_BLOCK iosb;
  BYTE access[4];
  const NTSTATUS status = NtQueryInformationFile(h, &iosb, access, sizeof(access), 8);      /* FileAccessInformation */
  if (!NT_SUCCESS(status)) return fail_nt(status);
  set_error(get32(access) & 2 /* FILE_WRITE_DATA */ ? 1314 /* ERROR_PRIVILEGE_NOT_HELD */ : ERROR_ACCESS_DENIED);
  return FALSE;
}
EXPORT BOOL WINAPI PostQueuedCompletionStatus(HANDLE port, DWORD bytes, ULONG_PTR key, LPOVERLAPPED overlapped);
typedef struct { HANDLE file, port; ULONG_PTR key; } PortLink;
static PortLink g_links[64];
static void port_link(HANDLE file, HANDLE port, ULONG_PTR key) {
  atom_lock();
  int free_slot = -1;
  for (int i = 0; i < 64; ++i) {
    if (g_links[i].file == file) { free_slot = i; break; }
    if (free_slot < 0 && !g_links[i].file) free_slot = i;
  }
  if (free_slot >= 0) { g_links[free_slot].file = file; g_links[free_slot].port = port; g_links[free_slot].key = key; }
  LeaveCriticalSection(&g_atom_lock);
}
static void port_unlink(HANDLE file) {
  atom_lock();
  for (int i = 0; i < 64; ++i) if (g_links[i].file == file) g_links[i].file = NULL;
  LeaveCriticalSection(&g_atom_lock);
}
static void port_note(HANDLE file, LPOVERLAPPED overlapped, ULONG_PTR bytes) {
  if (!overlapped || ((ULONG_PTR)overlapped->hEvent & 1)) return;
  HANDLE port = NULL;
  ULONG_PTR key = 0;
  atom_lock();
  for (int i = 0; i < 64; ++i) if (g_links[i].file && g_links[i].file == file) { port = g_links[i].port; key = g_links[i].key; break; }
  LeaveCriticalSection(&g_atom_lock);
  if (port) PostQueuedCompletionStatus(port, (DWORD)bytes, key, overlapped);
}
EXPORT HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD threads) {
  (void)threads;
  if (file && file != INVALID_HANDLE_VALUE && GetFileType(file) == FILE_TYPE_DISK && !overlapped_known(file)) { set_error(ERROR_INVALID_PARAMETER); return NULL; }      /* (a file opened for synchronous calls cannot be tied to a port) */
  if (existing) {
    if (!port_of(existing)) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (file != INVALID_HANDLE_VALUE) port_link(file, existing, key);
    return existing;
  }
  Port *p = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(Port));
  if (!p) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  p->semaphore = CreateSemaphoreW(NULL, 0, PORT_QUEUE, NULL);
  if (!p->semaphore) { RtlFreeHeap(heap(), 0, p); return NULL; }
  atom_lock();
  int slot = -1;
  for (int i = 0; i < PORT_MAX; ++i) if (g_ports[i] && g_ports[i]->semaphore == p->semaphore) { RtlFreeHeap(heap(), 0, g_ports[i]); g_ports[i] = NULL; }      /* (a closed port's handle value, used again) */
  for (int i = 0; i < PORT_MAX; ++i) if (!g_ports[i]) { slot = i; break; }
  if (slot >= 0) g_ports[slot] = p;
  LeaveCriticalSection(&g_atom_lock);
  if (slot < 0) { CloseHandle(p->semaphore); RtlFreeHeap(heap(), 0, p); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  if (file != INVALID_HANDLE_VALUE && file) port_link(file, p->semaphore, key);
  return p->semaphore;
}
EXPORT BOOL WINAPI PostQueuedCompletionStatus(HANDLE port, DWORD bytes, ULONG_PTR key, LPOVERLAPPED overlapped) {
  Port *p = port_of(port);
  if (!p) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  atom_lock();
  if (p->count == PORT_QUEUE) { LeaveCriticalSection(&g_atom_lock); set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  PortPacket *k = &p->packets[(p->head + p->count++) % PORT_QUEUE];
  k->bytes = bytes; k->key = key; k->overlapped = overlapped;
  LeaveCriticalSection(&g_atom_lock);
  ReleaseSemaphore(p->semaphore, 1, NULL);
  return TRUE;
}
/* The next event of a job object tied to the port: the message is the byte count, the process id the "overlapped" (as Windows reports them). */
static BOOL job_port_event(Port *p, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED *overlapped) {
  for (int i = 0; i < p->job_count; ++i) {
    BYTE info[24];
    ULONG got = 0;
    const NTSTATUS status = NtQueryInformationJobObject(p->jobs[i], 0x80, info, sizeof(info), &got);
    if (NT_SUCCESS(status) && status != (NTSTATUS)0x8000001A) {
      if (bytes) *bytes = get32(info);
      if (overlapped) *overlapped = (LPOVERLAPPED)(ULONG_PTR)get64(info + 8);
      if (key) *key = (ULONG_PTR)get64(info + 16);
      return TRUE;
    }
    if (status == (NTSTATUS)0xC0000008) p->jobs[i--] = p->jobs[--p->job_count];      /* (the job's handle is gone) */
  }
  return FALSE;
}
static BOOL port_take(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED *overlapped, DWORD timeout, BOOL alertable) {
  Port *p = port_of(port);
  if (!p) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (overlapped) *overlapped = NULL;
  DWORD w = WaitForSingleObjectEx(p->semaphore, 0, FALSE);      /* (a packet that is there is taken before any queued call is run) */
  if (w != WAIT_OBJECT_0 && p->job_count) {      /* the events of the job objects come from the kernel: looked for between short waits */
    const DWORD start = GetTickCount();
    for (;;) {
      if (job_port_event(p, bytes, key, overlapped)) return TRUE;
      if (timeout == 0) { w = WAIT_TIMEOUT; break; }
      const DWORD left = timeout == INFINITE ? 5 : (GetTickCount() - start >= timeout ? 0 : timeout - (GetTickCount() - start));
      if (left == 0) { w = WAIT_TIMEOUT; break; }
      w = WaitForSingleObjectEx(p->semaphore, left < 5 ? left : 5, alertable);
      if (w != WAIT_TIMEOUT) break;
    }
  } else if (w != WAIT_OBJECT_0 && timeout) w = WaitForSingleObjectEx(p->semaphore, timeout, alertable);
  else if (w != WAIT_OBJECT_0 && alertable) w = WaitForSingleObjectEx(p->semaphore, 0, TRUE);
  if (w != WAIT_OBJECT_0) { set_error(w == WAIT_TIMEOUT || w == WAIT_IO_COMPLETION ? w : GetLastError()); return FALSE; }
  atom_lock();
  PortPacket k = p->packets[p->head];
  p->head = (p->head + 1) % PORT_QUEUE;
  --p->count;
  LeaveCriticalSection(&g_atom_lock);
  if (bytes) *bytes = k.bytes;
  if (key) *key = k.key;
  if (overlapped) *overlapped = k.overlapped;
  return TRUE;
}
EXPORT BOOL WINAPI GetQueuedCompletionStatus(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED *overlapped, DWORD timeout) { return port_take(port, bytes, key, overlapped, timeout, FALSE); }
typedef struct { ULONG_PTR key; LPOVERLAPPED lpOverlapped; ULONG_PTR internal; DWORD bytes; } OVERLAPPED_ENTRY_;
EXPORT BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE port, OVERLAPPED_ENTRY_ *entries, ULONG count, PULONG removed, DWORD timeout, BOOL alertable) {
  ULONG n = 0;
  while (n < count) {
    DWORD bytes; ULONG_PTR key; LPOVERLAPPED ov;
    if (!port_take(port, &bytes, &key, &ov, n == 0 ? timeout : 0, n == 0 && alertable)) break;
    entries[n].lpOverlapped = ov; entries[n].key = key; entries[n].internal = 0; entries[n].bytes = bytes; ++n;
  }
  if (removed) *removed = n;
  if (n == 0) { if (GetLastError() != ERROR_INVALID_HANDLE && GetLastError() != WAIT_IO_COMPLETION) set_error(WAIT_TIMEOUT); return FALSE; }
  return TRUE;
}

/* ---- process lists: Toolhelp32, EnumProcesses and the psapi functions ---------------------------------------------------------------------------------------- */

typedef struct { DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads, th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; WCHAR szExeFile[260]; } PROCESSENTRY32W_;
typedef struct { DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads, th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; CHAR szExeFile[260]; } PROCESSENTRY32A_;
typedef struct { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32_;
typedef struct { DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage; BYTE *modBaseAddr; DWORD modBaseSize; HMODULE hModule; WCHAR szModule[256]; WCHAR szExePath[260]; } MODULEENTRY32W_;
typedef struct { DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage; BYTE *modBaseAddr; DWORD modBaseSize; HMODULE hModule; CHAR szModule[256]; CHAR szExePath[260]; } MODULEENTRY32A_;
typedef struct { SIZE_T dwSize; DWORD th32ProcessID; ULONG_PTR th32HeapID; DWORD dwFlags; } HEAPLIST32_;
typedef struct { SIZE_T dwSize; HANDLE hHandle; ULONG_PTR dwAddress; SIZE_T dwBlockSize; DWORD dwFlags, dwLockCount, dwResvd, th32ProcessID; ULONG_PTR th32HeapID; } HEAPENTRY32_;
typedef struct { PVOID lpData; SIZE_T cbData; BYTE cbOverhead, iRegionIndex; WORD wFlags; DWORD pad; BYTE u[24]; } RTL_WALK_ENTRY_;      /* (what RtlWalkHeap fills) */
typedef struct { PVOID lpData; DWORD cbData; BYTE cbOverhead, iRegionIndex; WORD wFlags; BYTE u[24]; } PROCESS_HEAP_ENTRY_;
typedef RTL_WALK_ENTRY_ HEAPWALK_;
#define TH32CS_SNAPHEAPLIST_ 1
#define TH32CS_SNAPPROCESS_ 2
#define TH32CS_SNAPTHREAD_ 4
#define TH32CS_SNAPMODULE_ 8
#define TH32CS_SNAPMODULE32_ 0x10
#define SNAP_MAX 8
static BOOL IsHandleValid_(HANDLE h) { return SetEvent(h) || GetLastError() != ERROR_INVALID_HANDLE; }      /* (a snapshot's handle is an event) */
typedef struct {
  HANDLE handle;
  DWORD process_count, process_next, thread_count, thread_next, module_count, module_next, heap_count, heap_next;
  PROCESSENTRY32W_ *processes;
  THREADENTRY32_ *threads;
  MODULEENTRY32W_ *modules;
  ULONG_PTR *heaps;
  DWORD pid;
} Snapshot;
static Snapshot *g_snaps[SNAP_MAX];

/* The whole of SystemProcessInformation (a heap block; the size of the data in *length), or NULL. */
static BYTE *query_process_information(ULONG *length) {
  ULONG need = 0;
  NtQuerySystemInformation(5, NULL, 0, &need);
  if (need == 0) return NULL;
  BYTE *buffer = RtlAllocateHeap(heap(), 0, need + 4096);
  if (!buffer) return NULL;
  if (!NT_SUCCESS(NtQuerySystemInformation(5, buffer, need + 4096, length))) { RtlFreeHeap(heap(), 0, buffer); return NULL; }
  return buffer;
}
static void fill_process_entry(PROCESSENTRY32W_ *o, const BYTE *e) {
  memset(o, 0, sizeof(*o));
  o->dwSize = sizeof(*o);
  o->cntThreads = *(const DWORD *)(e + 4);
  o->th32ProcessID = (DWORD)*(const ULONG_PTR *)(e + 0x50);
  o->th32ParentProcessID = (DWORD)*(const ULONG_PTR *)(e + 0x58);
  o->pcPriClassBase = 8;
  const WORD length = *(const WORD *)(e + 0x38) / 2;
  const WCHAR *name = *(WCHAR *const *)(e + 0x40);
  for (WORD i = 0; i < length && i < 259; ++i) o->szExeFile[i] = name[i];
}
static DWORD list_all_processes(PROCESSENTRY32W_ *out, DWORD max) {
  ULONG length = 0;
  BYTE *buffer = query_process_information(&length);
  if (!buffer) return 0;
  DWORD n = 0;
  for (const BYTE *e = buffer;;) {
    if (n < max) fill_process_entry(&out[n++], e);
    const DWORD next = *(const DWORD *)e;
    if (next == 0) break;
    e += next;
  }
  RtlFreeHeap(heap(), 0, buffer);
  return n;
}
/* The threads of every process (SYSTEM_THREAD_INFORMATION follows each process entry). */
static DWORD list_all_threads(THREADENTRY32_ *out, DWORD max) {
  ULONG length = 0;
  BYTE *buffer = query_process_information(&length);
  if (!buffer) return 0;
  DWORD n = 0;
  for (const BYTE *e = buffer;;) {
    const DWORD threads = *(const DWORD *)(e + 4);
    for (DWORD k = 0; k < threads; ++k) {
      const BYTE *t = e + 0x100 + (size_t_)k * 0x50;
      if (n >= max) break;
      memset(&out[n], 0, sizeof(out[n]));
      out[n].dwSize = sizeof(THREADENTRY32_);
      out[n].th32OwnerProcessID = (DWORD)*(const ULONG_PTR *)(t + 0x28);
      out[n].th32ThreadID = (DWORD)*(const ULONG_PTR *)(t + 0x30);
      out[n].tpBasePri = *(const LONG *)(t + 0x3C);
      ++n;
    }
    const DWORD next = *(const DWORD *)e;
    if (next == 0) break;
    e += next;
  }
  RtlFreeHeap(heap(), 0, buffer);
  return n;
}
/* The modules of a process, as the kernel lists them. Returns how many (the buffer is a heap block in *raw, to free). */
typedef struct { ULONG_PTR base, size, entry; DWORD flags, name_length; WCHAR name[40]; } ModuleRecord_;
static DWORD query_modules(DWORD pid, ModuleRecord_ **records, BYTE **raw) {
  *records = NULL; *raw = NULL;
  BYTE *buffer = RtlAllocateHeap(heap(), 0, 8 + 112 * 64);
  if (!buffer) return 0;
  *(ULONGLONG *)buffer = pid;
  if (!NT_SUCCESS(NtQuerySystemInformation(0x80, buffer, 8 + 112 * 64, NULL)) || *(DWORD *)buffer == 0) { RtlFreeHeap(heap(), 0, buffer); return 0; }
  *raw = buffer;
  *records = (ModuleRecord_ *)(buffer + 8);
  return *(DWORD *)buffer;
}
static void fill_module_entry(MODULEENTRY32W_ *o, const ModuleRecord_ *m, DWORD pid) {
  memset(o, 0, sizeof(*o));
  o->dwSize = sizeof(*o);
  o->th32ModuleID = 1; o->th32ProcessID = pid; o->GlblcntUsage = 0xFFFF; o->ProccntUsage = 0xFFFF;
  o->modBaseAddr = (BYTE *)m->base; o->modBaseSize = (DWORD)m->size; o->hModule = (HMODULE)m->base;
  DWORD i = 0;
  for (; i < m->name_length && i < 40 && i < 255; ++i) o->szModule[i] = m->name[i];
  o->szModule[i] = 0;
  if ((m->flags & 1) && pid == GetCurrentProcessId()) GetModuleFileNameW(NULL, o->szExePath, 260);
  else if (m->flags & 1) memcpy(o->szExePath, o->szModule, ((size_t_)i + 1) * 2);
  else {
    static const WCHAR dir[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', '3', '2', '\\', 0};
    DWORD k = 0;
    for (; dir[k]; ++k) o->szExePath[k] = dir[k];
    for (DWORD j = 0; j <= i; ++j) o->szExePath[k + j] = o->szModule[j];
  }
}
static DWORD list_modules_of(DWORD pid, MODULEENTRY32W_ *out, DWORD max) {
  ModuleRecord_ *records;
  BYTE *raw;
  const DWORD count = query_modules(pid, &records, &raw);
  DWORD n = 0;
  for (; n < count && n < max; ++n) fill_module_entry(&out[n], &records[n], pid);
  if (raw) RtlFreeHeap(heap(), 0, raw);
  return n;
}
#define HEAP_LIST_MAX 128
static DWORD list_heaps(ULONG_PTR *out, DWORD max) {
  PVOID heaps[HEAP_LIST_MAX];
  const ULONG n = RtlGetProcessHeaps(HEAP_LIST_MAX, heaps);
  DWORD k = 0;
  for (ULONG i = 0; i < n && i < HEAP_LIST_MAX && k < max; ++i) out[k++] = (ULONG_PTR)heaps[i];
  return k;
}

static void snapshot_free(Snapshot *s) {
  if (s->processes) RtlFreeHeap(heap(), 0, s->processes);
  if (s->threads) RtlFreeHeap(heap(), 0, s->threads);
  if (s->modules) RtlFreeHeap(heap(), 0, s->modules);
  if (s->heaps) RtlFreeHeap(heap(), 0, s->heaps);
  RtlFreeHeap(heap(), 0, s);
}
EXPORT HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid) {
  const DWORD kinds = TH32CS_SNAPHEAPLIST_ | TH32CS_SNAPPROCESS_ | TH32CS_SNAPTHREAD_ | TH32CS_SNAPMODULE_ | TH32CS_SNAPMODULE32_;
  if (!(flags & kinds)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (pid == 0) pid = GetCurrentProcessId();
  if ((flags & TH32CS_SNAPHEAPLIST_) && pid != GetCurrentProcessId()) { set_error(299 /* ERROR_PARTIAL_COPY */); return INVALID_HANDLE_VALUE; }      /* (only this process's heaps can be listed) */
  Snapshot *s = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(Snapshot));
  if (!s) { set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  s->pid = pid;
  BOOL ok = TRUE;
  if (flags & TH32CS_SNAPPROCESS_) {
    s->processes = RtlAllocateHeap(heap(), 0, sizeof(PROCESSENTRY32W_) * 64);
    if (s->processes) s->process_count = list_all_processes(s->processes, 64); else ok = FALSE;
  }
  if (ok && (flags & TH32CS_SNAPTHREAD_)) {
    s->threads = RtlAllocateHeap(heap(), 0, sizeof(THREADENTRY32_) * 1024);
    if (s->threads) s->thread_count = list_all_threads(s->threads, 1024); else ok = FALSE;
  }
  if (ok && (flags & (TH32CS_SNAPMODULE_ | TH32CS_SNAPMODULE32_))) {
    s->modules = RtlAllocateHeap(heap(), 0, sizeof(MODULEENTRY32W_) * 64);
    if (s->modules) s->module_count = (flags & TH32CS_SNAPMODULE_) ? list_modules_of(pid, s->modules, 64) : 0; else ok = FALSE;      /* (the 32-bit modules alone: this machine's processes have none) */
    if (ok && s->module_count == 0 && (flags & TH32CS_SNAPMODULE_)) { set_error(ERROR_INVALID_PARAMETER); ok = FALSE; }      /* (no such process) */
  }
  if (ok && (flags & TH32CS_SNAPHEAPLIST_)) {
    s->heaps = RtlAllocateHeap(heap(), 0, sizeof(ULONG_PTR) * HEAP_LIST_MAX);
    if (s->heaps) s->heap_count = list_heaps(s->heaps, HEAP_LIST_MAX); else ok = FALSE;
  }
  if (!ok) { if (GetLastError() == ERROR_SUCCESS) set_error(ERROR_NOT_ENOUGH_MEMORY); snapshot_free(s); return INVALID_HANDLE_VALUE; }
  s->handle = CreateEventW(NULL, TRUE, FALSE, NULL);      /* the snapshot's handle: closing it is what the program does when it is done */
  if (!s->handle) { snapshot_free(s); return INVALID_HANDLE_VALUE; }
  atom_lock();
  int slot = -1;
  for (int i = 0; i < SNAP_MAX; ++i) {
    if (g_snaps[i] && g_snaps[i]->handle == s->handle) { snapshot_free(g_snaps[i]); g_snaps[i] = NULL; }      /* (a closed snapshot's handle value, used again) */
    if (g_snaps[i] && !IsHandleValid_(g_snaps[i]->handle)) { snapshot_free(g_snaps[i]); g_snaps[i] = NULL; }      /* (one whose handle was closed) */
    if (!g_snaps[i] && slot < 0) slot = i;
  }
  if (slot >= 0) g_snaps[slot] = s;
  LeaveCriticalSection(&g_atom_lock);
  if (slot < 0) { CloseHandle(s->handle); snapshot_free(s); set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  return s->handle;
}
static Snapshot *snapshot_of(HANDLE h) {
  Snapshot *s = NULL;
  atom_lock();
  for (int i = 0; i < SNAP_MAX; ++i) if (g_snaps[i] && g_snaps[i]->handle == h) { s = g_snaps[i]; break; }
  LeaveCriticalSection(&g_atom_lock);
  if (s && !IsHandleValid_(h)) s = NULL;      /* (a closed snapshot) */
  return s;
}
/* The next entry of one list of a snapshot. */
static BOOL snapshot_step(HANDLE h, BOOL first, int kind, void *out, DWORD out_size) {
  Snapshot *s = snapshot_of(h);
  if (!s) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (!out || *(DWORD *)out < out_size) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  DWORD *next = kind == 0 ? &s->process_next : kind == 1 ? &s->thread_next : &s->module_next;
  const DWORD count = kind == 0 ? s->process_count : kind == 1 ? s->thread_count : s->module_count;
  if ((kind == 0 && !s->processes) || (kind == 1 && !s->threads) || (kind == 2 && !s->modules)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (first) *next = 0;
  /* the threads of a snapshot taken for one process are that process's (Thread32First lists them all, as the original does) */
  if (*next >= count) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
  if (kind == 0) *(PROCESSENTRY32W_ *)out = s->processes[(*next)++];
  else if (kind == 1) *(THREADENTRY32_ *)out = s->threads[(*next)++];
  else *(MODULEENTRY32W_ *)out = s->modules[(*next)++];
  return TRUE;
}
EXPORT BOOL WINAPI Process32FirstW(HANDLE h, PROCESSENTRY32W_ *e) { return snapshot_step(h, TRUE, 0, e, sizeof(*e)); }
EXPORT BOOL WINAPI Process32NextW(HANDLE h, PROCESSENTRY32W_ *e) { return snapshot_step(h, FALSE, 0, e, sizeof(*e)); }
EXPORT BOOL WINAPI Thread32First(HANDLE h, THREADENTRY32_ *e) { return snapshot_step(h, TRUE, 1, e, sizeof(*e)); }
EXPORT BOOL WINAPI Thread32Next(HANDLE h, THREADENTRY32_ *e) { return snapshot_step(h, FALSE, 1, e, sizeof(*e)); }
EXPORT BOOL WINAPI Module32FirstW(HANDLE h, MODULEENTRY32W_ *e) { return snapshot_step(h, TRUE, 2, e, sizeof(*e)); }
EXPORT BOOL WINAPI Module32NextW(HANDLE h, MODULEENTRY32W_ *e) { return snapshot_step(h, FALSE, 2, e, sizeof(*e)); }
static BOOL snapshot_step_a(HANDLE h, BOOL first, PROCESSENTRY32A_ *out) {
  PROCESSENTRY32W_ w;
  if (!out || out->dwSize < sizeof(*out)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  w.dwSize = sizeof(w);
  if (!snapshot_step(h, first, 0, &w, sizeof(w))) return FALSE;
  out->cntUsage = w.cntUsage; out->th32ProcessID = w.th32ProcessID; out->th32DefaultHeapID = w.th32DefaultHeapID; out->th32ModuleID = w.th32ModuleID;
  out->cntThreads = w.cntThreads; out->th32ParentProcessID = w.th32ParentProcessID; out->pcPriClassBase = w.pcPriClassBase; out->dwFlags = w.dwFlags;
  int i = 0;
  for (; w.szExeFile[i] && i < 259; ++i) out->szExeFile[i] = (char)w.szExeFile[i];
  out->szExeFile[i] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI Process32First(HANDLE h, PROCESSENTRY32A_ *e) { return snapshot_step_a(h, TRUE, e); }
EXPORT BOOL WINAPI Process32Next(HANDLE h, PROCESSENTRY32A_ *e) { return snapshot_step_a(h, FALSE, e); }
static BOOL module_step_a(HANDLE h, BOOL first, MODULEENTRY32A_ *out) {
  MODULEENTRY32W_ *w = RtlAllocateHeap(heap(), 0, sizeof(*w));
  if (!out || out->dwSize < sizeof(*out)) { if (w) RtlFreeHeap(heap(), 0, w); set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  w->dwSize = sizeof(*w);
  BOOL ok = snapshot_step(h, first, 2, w, sizeof(*w));
  if (ok) {
    out->th32ModuleID = w->th32ModuleID; out->th32ProcessID = w->th32ProcessID; out->GlblcntUsage = w->GlblcntUsage; out->ProccntUsage = w->ProccntUsage;
    out->modBaseAddr = w->modBaseAddr; out->modBaseSize = w->modBaseSize; out->hModule = w->hModule;
    int i = 0;
    for (; w->szModule[i] && i < 255; ++i) out->szModule[i] = (char)w->szModule[i];
    out->szModule[i] = 0;
    for (i = 0; w->szExePath[i] && i < 259; ++i) out->szExePath[i] = (char)w->szExePath[i];
    out->szExePath[i] = 0;
  }
  RtlFreeHeap(heap(), 0, w);
  return ok;
}
EXPORT BOOL WINAPI Module32First(HANDLE h, MODULEENTRY32A_ *e) { return module_step_a(h, TRUE, e); }
EXPORT BOOL WINAPI Module32Next(HANDLE h, MODULEENTRY32A_ *e) { return module_step_a(h, FALSE, e); }

/* heaps: the list is the process's heaps; the blocks are walked with RtlWalkHeap */
static BOOL heap_list_step(HANDLE h, BOOL first, HEAPLIST32_ *out) {
  Snapshot *s = snapshot_of(h);
  if (!s) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (!out || out->dwSize < sizeof(*out)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (!s->heaps) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (first) s->heap_next = 0;
  if (s->heap_next >= s->heap_count) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
  const ULONG_PTR id = s->heaps[s->heap_next];
  out->th32ProcessID = s->pid;
  out->th32HeapID = id;
  out->dwFlags = s->heap_next == 0 ? 1 /* HF32_DEFAULT */ : 0;
  ++s->heap_next;
  return TRUE;
}
EXPORT BOOL WINAPI Heap32ListFirst(HANDLE h, HEAPLIST32_ *e) { return heap_list_step(h, TRUE, e); }
EXPORT BOOL WINAPI Heap32ListNext(HANDLE h, HEAPLIST32_ *e) { return heap_list_step(h, FALSE, e); }
static BOOL heap_entry_step(HEAPENTRY32_ *e, DWORD pid, ULONG_PTR heap_id, BOOL first) {
  if (!e || e->dwSize < sizeof(*e)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (pid != 0 && pid != GetCurrentProcessId()) { set_error(299); return FALSE; }
  HEAPWALK_ w;
  memset(&w, 0, sizeof(w));
  if (!first) { w.lpData = (PVOID)e->dwAddress; w.iRegionIndex = (BYTE)(e->dwResvd & 0xFF); w.wFlags = (WORD)(e->dwResvd >> 8); }
  do {      /* the blocks only: regions and uncommitted parts are skipped */
    if (!NT_SUCCESS(RtlWalkHeap((PVOID)heap_id, &w))) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
  } while (w.wFlags & 0x1002);
  e->hHandle = (HANDLE)w.lpData; e->dwAddress = (ULONG_PTR)w.lpData; e->dwBlockSize = (SIZE_T)w.cbData;
  e->dwFlags = (w.wFlags & 1) ? 1 /* LF32_FIXED */ : 2 /* LF32_FREE */;
  e->dwLockCount = 0; e->dwResvd = (DWORD)(w.iRegionIndex | ((DWORD)w.wFlags << 8)); e->th32ProcessID = GetCurrentProcessId(); e->th32HeapID = heap_id;
  return TRUE;
}
EXPORT BOOL WINAPI Heap32First(HEAPENTRY32_ *e, DWORD pid, ULONG_PTR heap_id) { return heap_entry_step(e, pid, heap_id, TRUE); }
EXPORT BOOL WINAPI Heap32Next(HEAPENTRY32_ *e) { return e ? heap_entry_step(e, 0, e->th32HeapID, FALSE) : (set_error(ERROR_INVALID_PARAMETER), FALSE); }
EXPORT BOOL WINAPI Toolhelp32ReadProcessMemory(DWORD pid, LPCVOID base, LPVOID buffer, SIZE_T size, SIZE_T *done) {
  if (pid != 0 && pid != GetCurrentProcessId()) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  return ReadProcessMemory(GetCurrentProcess(), base, buffer, size, done);
}
/* HeapWalk over the same walker: the entry of RtlWalkHeap becomes a PROCESS_HEAP_ENTRY (the sizes shrink to 32 bits, the flags are the Win32 ones). */
EXPORT BOOL WINAPI HeapWalk(HANDLE h, PROCESS_HEAP_ENTRY_ *entry) {
  if (!entry) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  RTL_WALK_ENTRY_ rtl;
  memset(&rtl, 0, sizeof(rtl));
  rtl.lpData = entry->lpData;
  rtl.cbData = entry->cbData;
  rtl.cbOverhead = entry->cbOverhead;
  rtl.iRegionIndex = entry->iRegionIndex;
  if (entry->lpData) { rtl.wFlags = entry->wFlags & 1 ? 2 : entry->wFlags & 2 ? 0x1000 : entry->wFlags & 4 ? 0x4011 : 0; }
  const NTSTATUS s = RtlWalkHeap(h, &rtl);
  if (s != 0) { set_error(s == (NTSTATUS)0x8000001A ? 259 /* ERROR_NO_MORE_ITEMS */ : ERROR_INVALID_HANDLE); return FALSE; }
  entry->lpData = rtl.lpData;
  entry->cbData = (DWORD)rtl.cbData;
  entry->cbOverhead = rtl.cbOverhead;
  entry->iRegionIndex = rtl.iRegionIndex;
  entry->wFlags = (rtl.wFlags & 0x2) ? 1 /* PROCESS_HEAP_REGION */ : (rtl.wFlags & 0x1000) ? 2 /* PROCESS_HEAP_UNCOMMITTED_RANGE */ : (rtl.wFlags & 1) ? 4 /* PROCESS_HEAP_ENTRY_BUSY */ : 0;
  memcpy(&entry->u, &rtl.u, sizeof(entry->u));
  return TRUE;
}

typedef struct { DWORD cb; SIZE_T cbAllocated, cbCommitted, cbReserved, cbMaxReserve; } HEAP_SUMMARY_;
EXPORT BOOL WINAPI HeapSummary(HANDLE h, DWORD flags, HEAP_SUMMARY_ *summary) {
  (void)flags;
  if (!summary || summary->cb != sizeof(*summary)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  RTL_WALK_ENTRY_ w;
  memset(&w, 0, sizeof(w));
  SIZE_T allocated = 0, committed = 0, reserved = 0;
  NTSTATUS status;
  while ((status = RtlWalkHeap(h, &w)) == 0) {
    if (w.wFlags & 2) { committed += *(DWORD *)&w.u[0]; reserved += (SIZE_T)*(DWORD *)&w.u[0] + *(DWORD *)&w.u[4]; }      /* a region */
    else if (w.wFlags & 1) { allocated += w.cbData; if (w.iRegionIndex == 64) { committed += (w.cbData + 0x40 + 0xFFF) & ~(SIZE_T)0xFFF; reserved += (w.cbData + 0x40 + 0xFFF) & ~(SIZE_T)0xFFF; } }
  }
  if (status != (NTSTATUS)0x8000001A) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  summary->cbAllocated = allocated;
  summary->cbCommitted = committed;
  summary->cbReserved = reserved;
  summary->cbMaxReserve = reserved;
  return TRUE;
}

EXPORT BOOL WINAPI K32EnumProcesses(DWORD *ids, DWORD size, DWORD *needed) {
  PROCESSENTRY32W_ *list = RtlAllocateHeap(heap(), 0, sizeof(PROCESSENTRY32W_) * 64);
  if (!list) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  const DWORD n = list_all_processes(list, 64);
  DWORD written = 0;
  for (DWORD i = 0; i < n && (written + 1) * 4 <= size; ++i) ids[written++] = list[i].th32ProcessID;
  RtlFreeHeap(heap(), 0, list);
  if (needed) *needed = written * 4;
  return TRUE;
}
EXPORT BOOL WINAPI EnumProcesses(DWORD *ids, DWORD size, DWORD *needed) { return K32EnumProcesses(ids, size, needed); }

/* psapi: modules, names and memory counters of a process */
static DWORD process_id_of(HANDLE process) { return process == GetCurrentProcess() ? GetCurrentProcessId() : GetProcessId(process); }
EXPORT BOOL WINAPI K32EnumProcessModules(HANDLE process, HMODULE *modules, DWORD cb, LPDWORD needed) {
  ModuleRecord_ *records;
  BYTE *raw;
  const DWORD pid = process_id_of(process), count = pid ? query_modules(pid, &records, &raw) : 0;
  if (count == 0) { set_error(pid ? 299 : ERROR_INVALID_HANDLE); return FALSE; }
  for (DWORD i = 0; i < count && (i + 1) * sizeof(HMODULE) <= cb; ++i) modules[i] = (HMODULE)records[i].base;
  if (needed) *needed = count * (DWORD)sizeof(HMODULE);
  RtlFreeHeap(heap(), 0, raw);
  return TRUE;
}
EXPORT BOOL WINAPI K32EnumProcessModulesEx(HANDLE process, HMODULE *modules, DWORD cb, LPDWORD needed, DWORD filter) { (void)filter; return K32EnumProcessModules(process, modules, cb, needed); }
/* The record of `module` (NULL: the program) of a process, copied to *out. */
static BOOL find_module_record(HANDLE process, HMODULE module, ModuleRecord_ *out, DWORD *pid_out) {
  ModuleRecord_ *records;
  BYTE *raw;
  const DWORD pid = process_id_of(process), count = pid ? query_modules(pid, &records, &raw) : 0;
  BOOL found = FALSE;
  for (DWORD i = 0; i < count; ++i) if (module ? records[i].base == (ULONG_PTR)module : (records[i].flags & 1)) { *out = records[i]; found = TRUE; break; }
  if (count != 0) RtlFreeHeap(heap(), 0, raw);
  if (!found) set_error(ERROR_INVALID_HANDLE);
  if (pid_out) *pid_out = pid;
  return found;
}
EXPORT DWORD WINAPI K32GetModuleBaseNameW(HANDLE process, HMODULE module, LPWSTR name, DWORD size) {
  ModuleRecord_ m;
  if (!find_module_record(process, module, &m, NULL)) return 0;
  DWORD n = m.name_length < size ? m.name_length : (size ? size - 1 : 0);
  for (DWORD i = 0; i < n; ++i) name[i] = m.name[i];
  if (size) name[n] = 0;
  return n;
}
EXPORT DWORD WINAPI K32GetModuleBaseNameA(HANDLE process, HMODULE module, LPSTR name, DWORD size) {
  WCHAR wide[64];
  const DWORD n = K32GetModuleBaseNameW(process, module, wide, 64);
  const DWORD k = n < size ? n : (size ? size - 1 : 0);
  for (DWORD i = 0; i < k; ++i) name[i] = (char)wide[i];
  if (size) name[k] = 0;
  return k;
}
EXPORT DWORD WINAPI K32GetModuleFileNameExW(HANDLE process, HMODULE module, LPWSTR name, DWORD size) {
  ModuleRecord_ m;
  DWORD pid = 0;
  if (!find_module_record(process, module, &m, &pid)) return 0;
  MODULEENTRY32W_ *e = RtlAllocateHeap(heap(), 0, sizeof(*e));
  if (!e) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  fill_module_entry(e, &m, pid);
  const DWORD length = (DWORD)wcslen(e->szExePath), n = length < size ? length : (size ? size - 1 : 0);
  for (DWORD i = 0; i < n; ++i) name[i] = e->szExePath[i];
  if (size) name[n] = 0;
  RtlFreeHeap(heap(), 0, e);
  return n;
}
EXPORT DWORD WINAPI K32GetModuleFileNameExA(HANDLE process, HMODULE module, LPSTR name, DWORD size) {
  WCHAR wide[MAX_PATH];
  const DWORD n = K32GetModuleFileNameExW(process, module, wide, MAX_PATH);
  const DWORD k = n < size ? n : (size ? size - 1 : 0);
  for (DWORD i = 0; i < k; ++i) name[i] = (char)wide[i];
  if (size) name[k] = 0;
  return k;
}
typedef struct { LPVOID lpBaseOfDll; DWORD SizeOfImage; LPVOID EntryPoint; } MODULEINFO_;
EXPORT BOOL WINAPI K32GetModuleInformation(HANDLE process, HMODULE module, MODULEINFO_ *info, DWORD cb) {
  ModuleRecord_ m;
  if (!info || cb < sizeof(*info)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (!find_module_record(process, module, &m, NULL)) return FALSE;
  info->lpBaseOfDll = (LPVOID)m.base; info->SizeOfImage = (DWORD)m.size; info->EntryPoint = (LPVOID)m.entry;
  return TRUE;
}
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage; } PROCESS_MEMORY_COUNTERS_EX_;
EXPORT BOOL WINAPI K32GetProcessMemoryInfo(HANDLE process, PROCESS_MEMORY_COUNTERS_EX_ *counters, DWORD cb) {
  if (!counters || cb < 40) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  const DWORD pid = process_id_of(process);
  ULONG length = 0;
  BYTE *buffer = query_process_information(&length);
  BOOL found = FALSE;
  if (buffer) {
    for (const BYTE *e = buffer;;) {
      if ((DWORD)*(const ULONG_PTR *)(e + 0x50) == pid) {
        memset(counters, 0, cb < sizeof(*counters) ? cb : sizeof(*counters));
        counters->cb = cb;
        counters->PeakWorkingSetSize = counters->WorkingSetSize = *(const ULONG_PTR *)(e + 0x90);
        counters->PagefileUsage = counters->PeakPagefileUsage = *(const ULONG_PTR *)(e + 0xB8);
        if (cb >= sizeof(*counters)) counters->PrivateUsage = *(const ULONG_PTR *)(e + 0xC8);
        found = TRUE;
        break;
      }
      const DWORD next = *(const DWORD *)e;
      if (next == 0) break;
      e += next;
    }
    RtlFreeHeap(heap(), 0, buffer);
  }
  if (!found) set_error(ERROR_INVALID_HANDLE);
  return found;
}
/* The path of the program as the NT name ("\\Device\\HarddiskVolume1\\..."): its length in characters, or 0 and ERROR_INSUFFICIENT_BUFFER if `size` characters (with the NUL) do not hold it. */
EXPORT DWORD WINAPI K32GetProcessImageFileNameW(HANDLE process, LPWSTR name, DWORD size) {
  {      /* the kernel knows the NT name of the program */
    BYTE info[16 + 2 * (MAX_PATH * 2 + 1)];
    if (NT_SUCCESS(NtQueryInformationProcess(process, 27, info, sizeof(info), NULL))) {
      const DWORD length = get16(info) / 2;
      if (!name || length + 1 > size) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
      memcpy(name, (const WCHAR *)(ULONG_PTR)get64(info + 8), ((size_t_)length + 1) * 2);
      return length;
    }
  }
  ModuleRecord_ m;
  DWORD pid = 0;
  if (!find_module_record(process, NULL, &m, &pid)) return 0;
  MODULEENTRY32W_ *e = RtlAllocateHeap(heap(), 0, sizeof(*e));
  WCHAR *native = RtlAllocateHeap(heap(), 0, PATH_CHARS * 2);
  DWORD result = 0;
  if (!e || !native) set_error(ERROR_NOT_ENOUGH_MEMORY);
  else {
    fill_module_entry(e, &m, pid);
    if (!dos_path_to_native(e->szExePath, native, PATH_CHARS)) lstrcpyW(native, e->szExePath);
    const DWORD length = (DWORD)wcslen(native);
    if (!name || length + 1 > size) set_error(ERROR_INSUFFICIENT_BUFFER);
    else { memcpy(name, native, ((size_t_)length + 1) * 2); result = length; }
  }
  if (e) RtlFreeHeap(heap(), 0, e);
  if (native) RtlFreeHeap(heap(), 0, native);
  return result;
}
EXPORT DWORD WINAPI K32GetProcessImageFileNameA(HANDLE process, LPSTR name, DWORD size) {
  WCHAR wide[PATH_CHARS];
  const DWORD n = K32GetProcessImageFileNameW(process, wide, PATH_CHARS);
  if (n == 0) return 0;
  if (!name) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, (int)n + 1, NULL, 0, NULL, NULL);
  if (needed <= 0 || (DWORD)needed > size) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  WideCharToMultiByte(CP_UTF8, 0, wide, (int)n + 1, name, needed, NULL, NULL);
  return (DWORD)needed - 1;
}
EXPORT BOOL WINAPI K32EmptyWorkingSet(HANDLE process) { (void)process; return TRUE; }
EXPORT BOOL WINAPI EnumProcessModules(HANDLE p, HMODULE *m, DWORD cb, LPDWORD n) { return K32EnumProcessModules(p, m, cb, n); }
EXPORT BOOL WINAPI GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS_EX_ *c, DWORD cb) { return K32GetProcessMemoryInfo(p, c, cb); }
EXPORT BOOL WINAPI GetModuleInformation(HANDLE p, HMODULE m, MODULEINFO_ *i, DWORD cb) { return K32GetModuleInformation(p, m, i, cb); }
EXPORT DWORD WINAPI GetModuleBaseNameW(HANDLE p, HMODULE m, LPWSTR n, DWORD s) { return K32GetModuleBaseNameW(p, m, n, s); }
EXPORT DWORD WINAPI GetModuleBaseNameA(HANDLE p, HMODULE m, LPSTR n, DWORD s) { return K32GetModuleBaseNameA(p, m, n, s); }
EXPORT DWORD WINAPI GetModuleFileNameExW(HANDLE p, HMODULE m, LPWSTR n, DWORD s) { return K32GetModuleFileNameExW(p, m, n, s); }
EXPORT DWORD WINAPI GetModuleFileNameExA(HANDLE p, HMODULE m, LPSTR n, DWORD s) { return K32GetModuleFileNameExA(p, m, n, s); }
EXPORT DWORD WINAPI GetProcessImageFileNameW(HANDLE p, LPWSTR n, DWORD s) { return K32GetProcessImageFileNameW(p, n, s); }
EXPORT DWORD WINAPI GetProcessImageFileNameA(HANDLE p, LPSTR n, DWORD s) { return K32GetProcessImageFileNameA(p, n, s); }

/* ---- job objects ----------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { LARGE_INTEGER PerProcessUserTimeLimit, PerJobUserTimeLimit; DWORD LimitFlags; SIZE_T MinimumWorkingSetSize, MaximumWorkingSetSize; DWORD ActiveProcessLimit; ULONG_PTR Affinity; DWORD PriorityClass, SchedulingClass; } JOBOBJECT_BASIC_LIMIT_INFORMATION_;
EXPORT HANDLE WINAPI CreateJobObjectW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name) {
  NamedAttributes n;
  HANDLE h = NULL;
  const NTSTATUS status = NtCreateJobObject(&h, 0x1F001F, named_attributes(&n, name, sa && sa->bInheritHandle));
  return created(status, h);
}
EXPORT HANDLE WINAPI CreateJobObjectA(LPSECURITY_ATTRIBUTES sa, LPCSTR name) { WCHAR *w = name ? to_wide(name) : NULL; HANDLE h = CreateJobObjectW(sa, w); free_wide(w); return h; }
EXPORT HANDLE WINAPI OpenJobObjectW(DWORD access, BOOL inherit, LPCWSTR name) {
  (void)access;
  NamedAttributes n;
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenJobObject(&h, 0x1F001F, named_attributes(&n, name, inherit));
  return opened(status, h);
}
EXPORT HANDLE WINAPI OpenJobObjectA(DWORD access, BOOL inherit, LPCSTR name) { WCHAR *w = name ? to_wide(name) : NULL; HANDLE h = OpenJobObjectW(access, inherit, w); free_wide(w); return h; }
EXPORT BOOL WINAPI AssignProcessToJobObject(HANDLE job, HANDLE process) { const NTSTATUS s = NtAssignProcessToJobObject(job, process); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }
EXPORT BOOL WINAPI TerminateJobObject(HANDLE job, UINT code) { const NTSTATUS s = NtTerminateJobObject(job, (NTSTATUS)code); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }
EXPORT BOOL WINAPI IsProcessInJob(HANDLE process, HANDLE job, PBOOL result) {
  const NTSTATUS s = NtIsProcessInJob(process, job);
  if (!NT_SUCCESS(s)) return fail_nt(s);
  if (result) *result = s == 0x124;      /* STATUS_PROCESS_IN_JOB */
  return TRUE;
}
EXPORT BOOL WINAPI QueryInformationJobObject(HANDLE job, int info_class, LPVOID info, DWORD length, LPDWORD returned) {
  ULONG got = 0;
  const NTSTATUS s = NtQueryInformationJobObject(job, (ULONG)info_class, info, length, &got);
  if (returned) *returned = got;
  return NT_SUCCESS(s) && s != (NTSTATUS)0x80000005 ? TRUE : fail_nt(s);
}
EXPORT BOOL WINAPI SetInformationJobObject(HANDLE job, int info_class, LPVOID info, DWORD length) {
  if (info_class == 7 && info && length == 2 * sizeof(PVOID)) {      /* JobObjectAssociateCompletionPortInformation: the port is this program's own, the kernel only keeps the events for it */
    HANDLE port = ((HANDLE *)info)[1];
    if (port && !port_of(port)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    const NTSTATUS s = NtSetInformationJobObject(job, 7, info, length);
    if (!NT_SUCCESS(s)) return fail_nt(s);
    atom_lock();
    for (int i = 0; i < PORT_MAX; ++i) {      /* (the job is tied to this port, and to no other) */
      Port *q = g_ports[i];
      if (!q) continue;
      for (int k = 0; k < q->job_count; ++k) if (q->jobs[k] == job) q->jobs[k--] = q->jobs[--q->job_count];
    }
    Port *p = port ? port_of(port) : NULL;
    if (p && p->job_count < 4) p->jobs[p->job_count++] = job;
    LeaveCriticalSection(&g_atom_lock);
    return TRUE;
  }
  const NTSTATUS s = NtSetInformationJobObject(job, (ULONG)info_class, info, length);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}

/* ---- debugging ----------------------------------------------------------------------------------------------------------------------------------------------- */

/* The DEBUG_EVENT of the debugger API, made from the DBGUI_WAIT_STATE_CHANGE the kernel gives (see src/Process/WindowsDebug.hpp). */
typedef struct {
  DWORD dwDebugEventCode, dwProcessId, dwThreadId, pad;
  union {
    struct { EXCEPTION_RECORD ExceptionRecord; DWORD dwFirstChance; } Exception;
    struct { HANDLE hThread; LPVOID lpThreadLocalBase; LPVOID lpStartAddress; } CreateThread;
    struct { HANDLE hFile, hProcess, hThread; LPVOID lpBaseOfImage; DWORD dwDebugInfoFileOffset, nDebugInfoSize; LPVOID lpThreadLocalBase, lpStartAddress, lpImageName; WORD fUnicode; } CreateProcessInfo;
    struct { DWORD dwExitCode; } ExitThread;
    struct { DWORD dwExitCode; } ExitProcess;
    struct { HANDLE hFile; LPVOID lpBaseOfDll; DWORD dwDebugInfoFileOffset, nDebugInfoSize; LPVOID lpImageName; WORD fUnicode; } LoadDll;
    struct { LPVOID lpBaseOfDll; } UnloadDll;
    struct { LPSTR lpDebugStringData; WORD fUnicode, nDebugStringLength; } DebugString;
    BYTE bytes[0xA0];
  } u;
} TCOS_DEBUG_EVENT;   /* (0xB0 bytes, as DEBUG_EVENT) */

static BOOL wait_debug_event(TCOS_DEBUG_EVENT *event, DWORD milliseconds) {
  HANDLE object = DbgUiGetThreadDebugObject();
  if (!object) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  BYTE state[0xC0];
  LARGE_INTEGER timeout;
  timeout.QuadPart = -(LONGLONG)milliseconds * 10000;
  NTSTATUS status;
  do status = NtWaitForDebugEvent(object, FALSE, milliseconds == INFINITE ? NULL : &timeout, state); while (status == STATUS_USER_APC);
  if (status == (NTSTATUS)0x102) { set_error(121 /* ERROR_SEM_TIMEOUT */); return FALSE; }
  if (!NT_SUCCESS(status)) return fail_nt(status);
  memset(event, 0, sizeof(*event));
  event->dwProcessId = (DWORD)*(ULONG64 *)(state + 8);
  event->dwThreadId = (DWORD)*(ULONG64 *)(state + 16);
  switch (*(DWORD *)state) {
    case 2:      /* DbgCreateThread */
      event->dwDebugEventCode = 2;
      event->u.CreateThread.hThread = *(HANDLE *)(state + 0x18);
      event->u.CreateThread.lpStartAddress = *(LPVOID *)(state + 0x28);
      break;
    case 3:      /* DbgCreateProcess */
      event->dwDebugEventCode = 3;
      event->u.CreateProcessInfo.hProcess = *(HANDLE *)(state + 0x18);
      event->u.CreateProcessInfo.hThread = *(HANDLE *)(state + 0x20);
      event->u.CreateProcessInfo.lpBaseOfImage = *(LPVOID *)(state + 0x38);
      event->u.CreateProcessInfo.lpStartAddress = *(LPVOID *)(state + 0x50);
      break;
    case 4: event->dwDebugEventCode = 4; event->u.ExitThread.dwExitCode = *(DWORD *)(state + 0x18); break;
    case 5: event->dwDebugEventCode = 5; event->u.ExitProcess.dwExitCode = *(DWORD *)(state + 0x18); break;
    case 6: {      /* DbgException */
      EXCEPTION_RECORD *record = (EXCEPTION_RECORD *)(state + 0x18);
      if (record->ExceptionCode == 0x40010006u && record->NumberParameters >= 2) {      /* DBG_PRINTEXCEPTION_C: OutputDebugString */
        event->dwDebugEventCode = 8;
        event->u.DebugString.lpDebugStringData = (LPSTR)record->ExceptionInformation[1];
        event->u.DebugString.nDebugStringLength = (WORD)record->ExceptionInformation[0];
      } else {
        event->dwDebugEventCode = 1;
        event->u.Exception.ExceptionRecord = *record;
        event->u.Exception.dwFirstChance = *(DWORD *)(state + 0xB0);
      }
      break;
    }
    case 9: event->dwDebugEventCode = 6; event->u.LoadDll.lpBaseOfDll = *(LPVOID *)(state + 0x20); break;
    case 10: event->dwDebugEventCode = 7; event->u.UnloadDll.lpBaseOfDll = *(LPVOID *)(state + 0x18); break;
    default: event->dwDebugEventCode = 0xFFFFFFFFu; break;
  }
  return TRUE;
}
EXPORT BOOL WINAPI WaitForDebugEvent(LPVOID event, DWORD milliseconds) { return wait_debug_event((TCOS_DEBUG_EVENT *)event, milliseconds); }
EXPORT BOOL WINAPI WaitForDebugEventEx(LPVOID event, DWORD milliseconds) { return wait_debug_event((TCOS_DEBUG_EVENT *)event, milliseconds); }
EXPORT BOOL WINAPI ContinueDebugEvent(DWORD process_id, DWORD thread_id, DWORD status) {
  HANDLE object = DbgUiGetThreadDebugObject();
  if (!object) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  ULONG64 id[2] = {process_id, thread_id};
  const NTSTATUS result = NtDebugContinue(object, id, (NTSTATUS)status);
  return NT_SUCCESS(result) ? TRUE : fail_nt(result);
}
EXPORT BOOL WINAPI DebugActiveProcess(DWORD id) {
  HANDLE process = OpenProcess(0x1FFFFF, FALSE, id);
  if (!process) return FALSE;
  NTSTATUS status = DbgUiConnectToDbg();
  if (NT_SUCCESS(status)) status = NtDebugActiveProcess(process, DbgUiGetThreadDebugObject());
  if (NT_SUCCESS(status)) {
    status = DbgUiIssueRemoteBreakin(process);
    if (!NT_SUCCESS(status)) NtRemoveProcessDebug(process, DbgUiGetThreadDebugObject());
  }
  NtClose(process);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI DebugActiveProcessStop(DWORD id) {
  HANDLE process = OpenProcess(0x1FFFFF, FALSE, id);
  if (!process) return FALSE;
  const NTSTATUS status = NtRemoveProcessDebug(process, DbgUiGetThreadDebugObject());
  NtClose(process);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI DebugSetProcessKillOnExit(BOOL kill) {
  HANDLE object = DbgUiGetThreadDebugObject();
  if (!object) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  ULONG flags = kill ? 1 : 0;
  const NTSTATUS status = NtSetInformationDebugObject(object, 1, &flags, sizeof(flags), NULL);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI DebugBreakProcess(HANDLE process) {
  const NTSTATUS status = DbgUiIssueRemoteBreakin(process);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* ---- delay-loaded imports -------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  ULONG Size;
  const void *DelayloadDescriptor;
  ULONG64 *ThunkAddress;
  LPCSTR TargetDllName;
  ULONG ImportDescribedByName;
  ULONG64 Description;      /* the name pointer, or the ordinal */
  PVOID TargetModuleBase;
  PVOID Unused;
  ULONG LastError;
} DelayLoadInfo;

/* ResolveDelayLoadedAPI(base, descriptor, dll failure hook, system failure hook, thunk, flags): loads the library the descriptor names (once: its handle is kept in the image),
 * finds the function the thunk stands for and stores its address in the thunk. A function that is not there goes to the hooks: the DLL hook first, then the system one. */
EXPORT PVOID WINAPI ResolveDelayLoadedAPI(PVOID parent, const void *descriptor, PVOID (WINAPI *dll_hook)(ULONG, DelayLoadInfo *), PVOID (WINAPI *system_hook)(LPCSTR, LPCSTR), ULONG64 *thunk, ULONG flags) {
  (void)flags;
  const ULONG *d = (const ULONG *)descriptor;      /* Attributes, DllNameRVA, ModuleHandleRVA, ImportAddressTableRVA, ImportNameTableRVA, ... */
  const BYTE *base = (const BYTE *)parent;
  LPCSTR dll = (LPCSTR)(base + d[1]);
  HMODULE *handle = (HMODULE *)(base + d[2]);
  const ULONG64 *names = (const ULONG64 *)(base + d[4]);
  const ULONG64 index = (ULONG64)(thunk - (const ULONG64 *)(base + d[3]));
  const ULONG64 entry = names[index];
  const BOOL by_ordinal = (entry >> 63) != 0;
  HMODULE module = *handle;
  if (!module) {
    module = LoadLibraryA(dll);
    if (module) *handle = module;
  }
  DelayLoadInfo info = { sizeof(info), descriptor, thunk, dll, by_ordinal ? 0 : 1, by_ordinal ? (entry & 0xFFFF) : (ULONG64)(ULONG_PTR)(base + (entry & 0x7FFFFFFF) + 2), module, NULL, 0 };
  PVOID address = NULL;
  if (module) address = (PVOID)GetProcAddress(module, by_ordinal ? (LPCSTR)(ULONG_PTR)(entry & 0xFFFF) : (LPCSTR)(base + (entry & 0x7FFFFFFF) + 2));
  info.LastError = GetLastError();
  if (!address) {
    if (dll_hook) address = dll_hook(4 /* DELAYLOAD_GPA_FAILURE */, &info);
    if (!address && system_hook) address = system_hook(dll, by_ordinal ? (LPCSTR)(ULONG_PTR)(entry & 0xFFFF) : (LPCSTR)(base + (entry & 0x7FFFFFFF) + 2));
  }
  if (address) *thunk = (ULONG64)(ULONG_PTR)address;
  return address;
}

VOID NTAPI NtFlushProcessWriteBuffers(void);
EXPORT VOID WINAPI FlushProcessWriteBuffers(void) { NtFlushProcessWriteBuffers(); }

/* BindIoCompletionCallback: the file is tied to a port of the process's own, which a thread of ours empties, calling the routine that was given with the file (the key). */
typedef void (WINAPI *IO_COMPLETION_ROUTINE_)(DWORD error, DWORD bytes, LPOVERLAPPED overlapped);
static HANDLE g_bind_port;
static HANDLE g_bound_files[64];
static DWORD WINAPI bind_dispatcher(LPVOID unused) {
  (void)unused;
  for (;;) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED overlapped = NULL;
    if (!GetQueuedCompletionStatus(g_bind_port, &bytes, &key, &overlapped, INFINITE) && !overlapped) continue;
    if (overlapped && key) ((IO_COMPLETION_ROUTINE_)key)(RtlNtStatusToDosError((NTSTATUS)overlapped->Internal), bytes, overlapped);
  }
  return 0;
}
EXPORT BOOL WINAPI BindIoCompletionCallback(HANDLE file, IO_COMPLETION_ROUTINE_ routine, ULONG flags) {
  (void)flags;
  if (!file || file == INVALID_HANDLE_VALUE) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (!routine || !overlapped_known(file)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }      /* (a file not opened for overlapped use, or no routine) */
  atom_lock();
  int free_slot = -1;
  for (int i = 0; i < 64; ++i) {
    if (g_bound_files[i] == file) { LeaveCriticalSection(&g_atom_lock); set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!g_bound_files[i] && free_slot < 0) free_slot = i;
  }
  LeaveCriticalSection(&g_atom_lock);
  if (free_slot < 0) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  if (!g_bind_port) {
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) return FALSE;
    if (__sync_val_compare_and_swap(&g_bind_port, (HANDLE)NULL, port) != NULL) CloseHandle(port);
    else {
      HANDLE t = CreateThread(NULL, 0, bind_dispatcher, NULL, 0, NULL);
      if (t) CloseHandle(t);
    }
  }
  if (!CreateIoCompletionPort(file, g_bind_port, (ULONG_PTR)routine, 0)) return FALSE;
  atom_lock();
  g_bound_files[free_slot] = file;
  LeaveCriticalSection(&g_atom_lock);
  return TRUE;
}
