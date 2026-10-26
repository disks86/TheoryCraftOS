/* More of kernel32 (included by kernel32.c after extra2.inc): locale and string functions for the one locale there is (English, United States), atoms, process and module
   lists, completion ports and other small things. */

typedef long HRESULT;
typedef ULONG_PTR *PULONG_PTR;
#define ERROR_BUFFER_OVERFLOW 111
#define ERROR_PARTIAL_COPY 299
#define ERROR_OLD_WIN_VERSION 1150
static void copy_text(char *dst, const char *src) { while ((*dst++ = *src++) != 0) {} }

/* ---- characters ---------------------------------------------------------------------------------------------------------------------------------------- */

static WCHAR wide_lower(WCHAR c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c;
  if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return (WCHAR)(c + 32);
  if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x178 && c != 0x17F) {
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? (WCHAR)(c + 1) : c;
    return (c & 1) ? c : (WCHAR)(c + 1);
  }
  if (c == 0x178) return 0xFF;
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return (WCHAR)(c + 32);
  if (c >= 0x410 && c <= 0x42F) return (WCHAR)(c + 32);
  if (c >= 0x400 && c <= 0x40F) return (WCHAR)(c + 80);
  return c;
}
static WCHAR wide_upper(WCHAR c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (WCHAR)(c - 32);
  if (c == 0xFF) return 0x178;
  if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : (WCHAR)(c - 1);
    return (c & 1) ? (WCHAR)(c - 1) : c;
  }
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (WCHAR)(c - 32);
  if (c >= 0x430 && c <= 0x44F) return (WCHAR)(c - 32);
  if (c >= 0x450 && c <= 0x45F) return (WCHAR)(c - 80);
  return c;
}
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
static WORD ctype1(WCHAR c) {
  WORD t = 0;
  if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) t |= C1_CNTRL_;
  if (c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0x2000 || c == 0x3000) t |= C1_SPACE_;
  if (c == ' ' || c == 9 || c == 0xA0) t |= C1_BLANK_;
  if (c >= '0' && c <= '9') t |= C1_DIGIT_ | C1_XDIGIT_;
  if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) t |= C1_XDIGIT_;
  if (wide_is_letter(c)) { t |= C1_ALPHA_; if (wide_lower(c) != c) t |= C1_UPPER_; else if (wide_upper(c) != c) t |= C1_LOWER_; }
  if (c < 0x80 ? ((c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E))
               : ((c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) || c == 0xD7 || c == 0xF7 || (c >= 0x2010 && c <= 0x2027) || (c >= 0x3001 && c <= 0x3003)))
    t |= C1_PUNCT_;
  if (c != 0xFFFF && c != 0xFFFE) t |= C1_DEFINED_;
  return t;
}
EXPORT BOOL WINAPI GetStringTypeW(DWORD info_type, LPCWSTR source, int count, LPWORD types) {
  if (!source || !types || (info_type != 1 && info_type != 2 && info_type != 4)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (count < 0) count = (int)wcslen(source);
  for (int i = 0; i < count; ++i) {
    if (info_type == 1) types[i] = ctype1(source[i]);
    else if (info_type == 2) types[i] = source[i] >= 0x590 && source[i] <= 0x8FF ? 2 /* C2_RIGHTTOLEFT */ : (ctype1(source[i]) & C1_CNTRL_ ? 0 : 1 /* C2_LEFTTORIGHT */);
    else types[i] = (ctype1(source[i]) & C1_ALPHA_) ? 0x8000 /* C3_ALPHA */ : 0;
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

#define LCMAP_LOWERCASE_ 0x100
#define LCMAP_UPPERCASE_ 0x200
#define LCMAP_SORTKEY_ 0x400
#define LCMAP_BYTEREV_ 0x800
EXPORT int WINAPI LCMapStringW(LCID locale, DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity) {
  (void)locale;
  if (!source || (flags & (LCMAP_LOWERCASE_ | LCMAP_UPPERCASE_)) == (LCMAP_LOWERCASE_ | LCMAP_UPPERCASE_)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (count < 0) count = (int)wcslen(source) + 1;
  const int needed = (flags & LCMAP_SORTKEY_) ? count * 2 + 2 : count;
  if (capacity == 0) return needed;
  if (capacity < needed) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  if (flags & LCMAP_SORTKEY_) {      /* a key that sorts as the text does (case folded, big-endian), ended by 0 0 */
    BYTE *out = (BYTE *)dest;
    for (int i = 0; i < count; ++i) { const WCHAR c = (flags & 1 /* NORM_IGNORECASE */) ? wide_lower(source[i]) : source[i]; out[2 * i] = (BYTE)(c >> 8); out[2 * i + 1] = (BYTE)c; }
    out[2 * count] = 0; out[2 * count + 1] = 0;
    return needed;
  }
  for (int i = 0; i < count; ++i) {
    WCHAR c = source[i];
    if (flags & LCMAP_LOWERCASE_) c = wide_lower(c);
    else if (flags & LCMAP_UPPERCASE_) c = wide_upper(c);
    if (flags & LCMAP_BYTEREV_) c = (WCHAR)((c << 8) | (c >> 8));
    dest[i] = c;
  }
  return count;
}
EXPORT int WINAPI LCMapStringEx(LPCWSTR locale_name, DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity, LPVOID version, LPVOID reserved, LPARAM sort_handle) {
  (void)locale_name; (void)version; (void)reserved; (void)sort_handle;
  return LCMapStringW(0x409, flags, source, count, dest, capacity);
}
EXPORT int WINAPI LCMapStringA(LCID locale, DWORD flags, LPCSTR source, int count, LPSTR dest, int capacity) {
  if (!source) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (count < 0) count = (int)strlen(source) + 1;
  WCHAR *w = RtlAllocateHeap(heap(), 0, ((SIZE_T)count + 1) * 4);
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  const int n = MultiByteToWideChar(CP_UTF8, 0, source, count, w, count);
  int result = 0;
  if (flags & LCMAP_SORTKEY_) {
    result = LCMapStringW(locale, flags, w, n, (LPWSTR)dest, capacity / 2 > 0 ? capacity / 2 : 0);
    if (capacity == 0) result = n * 2 + 2;
  } else {
    WCHAR *mapped = w + count + 1;
    const int m = LCMapStringW(locale, flags, w, n, mapped, count);
    if (m > 0) {
      if (capacity == 0) result = WideCharToMultiByte(CP_UTF8, 0, mapped, m, NULL, 0, NULL, NULL);
      else result = WideCharToMultiByte(CP_UTF8, 0, mapped, m, dest, capacity, NULL, NULL);
    }
  }
  RtlFreeHeap(heap(), 0, w);
  return result;
}
EXPORT int WINAPI FoldStringW(DWORD flags, LPCWSTR source, int count, LPWSTR dest, int capacity) {
  if (!source) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (count < 0) count = (int)wcslen(source) + 1;
  if (capacity == 0) return count;
  if (capacity < count) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < count; ++i) {
    WCHAR c = source[i];
    if ((flags & 0x2 /* MAP_FOLDCZONE */) && c >= 0xFF01 && c <= 0xFF5E) c = (WCHAR)(c - 0xFEE0);      /* full-width to ASCII */
    if ((flags & 0x8 /* MAP_EXPAND_LIGATURES */) == 0 && (flags & 0x10 /* MAP_FOLDDIGITS */) && c >= 0x660 && c <= 0x669) c = (WCHAR)('0' + c - 0x660);
    dest[i] = c;
  }
  return count;
}
EXPORT int WINAPI FoldStringA(DWORD flags, LPCSTR source, int count, LPSTR dest, int capacity) { (void)flags; if (count < 0) count = (int)strlen(source) + 1; if (capacity == 0) return count; if (capacity < count) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; } memcpy(dest, source, (size_t_)count); return count; }
EXPORT int WINAPI CompareStringEx(LPCWSTR locale_name, DWORD flags, LPCWSTR a, int an, LPCWSTR b, int bn, LPVOID version, LPVOID reserved, LPARAM sort_handle) {
  (void)locale_name; (void)version; (void)reserved; (void)sort_handle;
  return CompareStringW(0x409, flags, a, an, b, bn);
}
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
EXPORT int WINAPI FindStringOrdinal(DWORD flags, LPCWSTR source, int source_count, LPCWSTR value, int value_count, BOOL ignore_case) {
  if (!source || !value) { set_error(ERROR_INVALID_PARAMETER); return -1; }
  if (source_count < 0) source_count = (int)wcslen(source);
  if (value_count < 0) value_count = (int)wcslen(value);
  if (value_count > source_count) return -1;
  if (flags & 2 /* FIND_FROMEND */) {
    for (int i = source_count - value_count; i >= 0; --i) if (CompareStringOrdinal(source + i, value_count, value, value_count, ignore_case) == 2) return i;
  } else {
    for (int i = 0; i + value_count <= source_count; ++i) if (CompareStringOrdinal(source + i, value_count, value, value_count, ignore_case) == 2) return i;
  }
  return -1;
}
EXPORT int WINAPI FindNLSStringEx(LPCWSTR locale_name, DWORD flags, LPCWSTR source, int source_count, LPCWSTR value, int value_count, LPINT found_length, LPVOID version, LPVOID reserved, LPARAM sort_handle) {
  (void)locale_name; (void)version; (void)reserved; (void)sort_handle;
  const int at = FindStringOrdinal((flags & 0x00400000) ? 2 : 1, source, source_count, value, value_count, (flags & 1) != 0);
  if (at >= 0 && found_length) *found_length = value_count < 0 ? (int)wcslen(value) : value_count;
  return at;
}
EXPORT int WINAPI FindNLSString(LCID locale, DWORD flags, LPCWSTR source, int source_count, LPCWSTR value, int value_count, LPINT found_length) { (void)locale; return FindNLSStringEx(NULL, flags, source, source_count, value, value_count, found_length, NULL, NULL, 0); }
EXPORT BOOL WINAPI IsNLSDefinedString(int function, DWORD flags, LPVOID version, LPCWSTR text, int count) { (void)function; (void)flags; (void)version; (void)text; (void)count; return TRUE; }
EXPORT BOOL WINAPI IsNormalizedString(int form, LPCWSTR text, int count) {      /* (text without combining marks is in every form already) */
  (void)form;
  if (count < 0) count = (int)wcslen(text);
  for (int i = 0; i < count; ++i) if (text[i] >= 0x300 && text[i] <= 0x36F) return FALSE;
  return TRUE;
}
EXPORT int WINAPI NormalizeString(int form, LPCWSTR source, int count, LPWSTR dest, int capacity) {
  (void)form;
  if (count < 0) count = (int)wcslen(source) + 1;
  if (capacity == 0) return count;
  if (capacity < count) { set_error(ERROR_INSUFFICIENT_BUFFER); return count; }
  memcpy(dest, source, (size_t_)count * 2);
  return count;
}
EXPORT BOOL WINAPI IsDBCSLeadByte(BYTE c) { (void)c; return FALSE; }
EXPORT BOOL WINAPI IsDBCSLeadByteEx(UINT cp, BYTE c) { (void)cp; (void)c; return FALSE; }

/* ---- locale information (English, United States) --------------------------------------------------------------------------------------------------------- */

static const char *const k_days[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
static const char *const k_months[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
/* The text of a LOCALE_ value, or NULL for a number (*number) or an unknown one. */
static const char *locale_text(DWORD type, DWORD *number, char *scratch) {
  *number = (DWORD)-1;
  switch (type & 0xFFFF) {
    case 0x01: return "1033";                  /* LOCALE_ILANGUAGE */
    case 0x02: return "English (United States)";      /* SLANGUAGE */
    case 0x03: case 0x1001: return "English";   /* SABBREVLANGNAME, SENGLISHLANGUAGENAME */
    case 0x04: return "English (United States)";
    case 0x05: *number = 1; return NULL;       /* ICOUNTRY */
    case 0x06: return "United States";         /* SCOUNTRY */
    case 0x07: return "USA";
    case 0x08: return "USA";
    case 0x09: *number = 1; return NULL;       /* SABBREVCTRYNAME... ICOUNTRY (kept) */
    case 0x0A: return "United States";
    case 0x0B: return "1";                     /* IDEFAULTCOUNTRY */
    case 0x0C: return ",";                     /* SLIST */
    case 0x0D: *number = 1; return NULL;       /* IMEASURE: US */
    case 0x0E: return ".";                     /* SDECIMAL */
    case 0x0F: return ",";                     /* STHOUSAND */
    case 0x10: return "3;0";                   /* SGROUPING */
    case 0x11: *number = 2; return NULL;       /* IDIGITS */
    case 0x12: *number = 1; return NULL;       /* ILZERO */
    case 0x13: *number = 0; return NULL;       /* INEGNUMBER... (see 0x1010) */
    case 0x14: return "$";                     /* SCURRENCY */
    case 0x15: return "USD";                   /* SINTLSYMBOL */
    case 0x16: return ".";                     /* SMONDECIMALSEP */
    case 0x17: return ",";                     /* SMONTHOUSANDSEP */
    case 0x18: return "3;0";                   /* SMONGROUPING */
    case 0x19: *number = 2; return NULL;       /* ICURRDIGITS */
    case 0x1A: *number = 2; return NULL;       /* IINTLCURRDIGITS */
    case 0x1B: *number = 0; return NULL;       /* ICURRENCY: $1 */
    case 0x1C: *number = 0; return NULL;       /* INEGCURR: ($1) */
    case 0x1D: return "/";                     /* SDATE */
    case 0x1E: return ":";                     /* STIME */
    case 0x1F: return "M/d/yyyy";              /* SSHORTDATE */
    case 0x20: return "dddd, MMMM d, yyyy";    /* SLONGDATE */
    case 0x21: *number = 0; return NULL;       /* IDATE: month-day-year */
    case 0x22: *number = 0; return NULL;       /* ILDATE */
    case 0x23: *number = 0; return NULL;       /* ITIME: 12 hour */
    case 0x24: *number = 0; return NULL;       /* ICENTURY... */
    case 0x25: *number = 0; return NULL;       /* ITLZERO */
    case 0x26: *number = 0; return NULL;       /* IDAYLZERO */
    case 0x27: *number = 0; return NULL;       /* IMONLZERO */
    case 0x28: return "AM";
    case 0x29: return "PM";
    case 0x4A: *number = 0; return NULL;       /* ITIMEMARKPOSN */
    case 0x50: return "";                      /* SPOSITIVESIGN */
    case 0x51: return "-";                     /* SNEGATIVESIGN */
    case 0x59: return "en";                    /* SISO639LANGNAME */
    case 0x5A: return "US";                    /* SISO3166CTRYNAME */
    case 0x5C: return "en-US";                 /* SNAME */
    case 0x1002: return "United States";       /* SENGLISHCOUNTRYNAME */
    case 0x1003: return "h:mm:ss tt";          /* STIMEFORMAT */
    case 0x1004: *number = 65001; return NULL; /* IDEFAULTANSICODEPAGE */
    case 0x1007: *number = 437; return NULL;   /* IDEFAULTCODEPAGE */
    case 0x100B: *number = 0; return NULL;     /* IFIRSTWEEKOFYEAR */
    case 0x100C: *number = 6; return NULL;     /* IFIRSTDAYOFWEEK: Sunday */
    case 0x1010: *number = 1; return NULL;     /* INEGNUMBER */
    case 0x1014: *number = 0; return NULL;     /* ILANGUAGE (kept) */
    default: break;
  }
  const DWORD t = type & 0xFFFF;
  if (t >= 0x2A && t <= 0x30) return k_days[t - 0x2A];      /* SDAYNAME1..7 */
  if (t >= 0x31 && t <= 0x37) { memcpy(scratch, k_days[t - 0x31], 3); scratch[3] = 0; return scratch; }      /* SABBREVDAYNAME */
  if (t >= 0x38 && t <= 0x43) return k_months[t - 0x38];     /* SMONTHNAME1..12 */
  if (t >= 0x44 && t <= 0x4F) { memcpy(scratch, k_months[t - 0x44], 3); scratch[3] = 0; return scratch; }      /* SABBREVMONTHNAME */
  return NULL;
}
EXPORT int WINAPI GetLocaleInfoW(LCID locale, LCTYPE type, LPWSTR buffer, int capacity) {
  if (locale != 0x409 && locale != 0x400 && locale != 0x800 && locale != 0x1000 && locale != 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  char scratch[8], number_text[16];
  DWORD number = (DWORD)-1;
  const char *text = locale_text(type, &number, scratch);
  if ((type & 0x20000000 /* LOCALE_RETURN_NUMBER */)) {
    if (number == (DWORD)-1 && text) { number = 0; for (const char *c = text; *c >= '0' && *c <= '9'; ++c) number = number * 10 + (DWORD)(*c - '0'); }
    if (number == (DWORD)-1) { set_error(ERROR_INVALID_FLAGS); return 0; }
    if (capacity == 0) return 2;
    if (capacity < 2) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buffer, &number, 4);
    return 2;
  }
  if (!text && number != (DWORD)-1) {
    char *p = number_text + sizeof(number_text) - 1;
    *p = 0;
    DWORD v = number;
    do { *--p = (char)('0' + v % 10); v /= 10; } while (v);
    text = p;
  }
  if (!text) { set_error(ERROR_INVALID_FLAGS); return 0; }
  const int n = (int)strlen(text) + 1;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < n; ++i) buffer[i] = (WCHAR)(BYTE)text[i];
  return n;
}
EXPORT int WINAPI GetLocaleInfoA(LCID locale, LCTYPE type, LPSTR buffer, int capacity) {
  WCHAR wide[96];
  if (type & 0x20000000) { if (capacity == 0) return 4; if (capacity < 4) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; } return GetLocaleInfoW(locale, type, (LPWSTR)buffer, 2) ? 4 : 0; }
  const int n = GetLocaleInfoW(locale, type, wide, 96);
  if (n == 0) return 0;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < n; ++i) buffer[i] = (char)wide[i];
  return n;
}
EXPORT int WINAPI GetLocaleInfoEx(LPCWSTR name, LCTYPE type, LPWSTR buffer, int capacity) { (void)name; return GetLocaleInfoW(0x409, type, buffer, capacity); }
EXPORT BOOL WINAPI SetLocaleInfoW(LCID locale, LCTYPE type, LPCWSTR value) { (void)locale; (void)type; (void)value; set_error(ERROR_ACCESS_DENIED); return FALSE; }
EXPORT BOOL WINAPI SetLocaleInfoA(LCID locale, LCTYPE type, LPCSTR value) { (void)locale; (void)type; (void)value; set_error(ERROR_ACCESS_DENIED); return FALSE; }
EXPORT int WINAPI LCIDToLocaleName(LCID locale, LPWSTR name, int capacity, DWORD flags) { (void)flags; return locale == 0x409 ? GetUserDefaultLocaleName(name, capacity) : (set_error(ERROR_INVALID_PARAMETER), 0); }
EXPORT LCID WINAPI LocaleNameToLCID(LPCWSTR name, DWORD flags) { (void)flags; if (!name || !*name) return 0x409; if ((name[0] | 0x20) == 'e' && (name[1] | 0x20) == 'n') return 0x409; set_error(ERROR_INVALID_PARAMETER); return 0; }
EXPORT BOOL WINAPI IsValidLocaleName(LPCWSTR name) { return name && (name[0] | 0x20) == 'e' && (name[1] | 0x20) == 'n'; }
EXPORT int WINAPI GetUserPreferredUILanguages(DWORD flags, PULONG count, LPWSTR languages, PULONG length) {
  (void)flags;
  static const WCHAR en[] = {'e', 'n', '-', 'U', 'S', 0, 0};
  if (count) *count = 1;
  if (length) { if (languages && *length < 7) { *length = 7; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; } *length = 7; }
  if (languages) memcpy(languages, en, sizeof(en));
  return TRUE;
}
EXPORT BOOL WINAPI GetSystemPreferredUILanguages(DWORD flags, PULONG count, LPWSTR languages, PULONG length) { return GetUserPreferredUILanguages(flags, count, languages, length); }
EXPORT BOOL WINAPI GetThreadPreferredUILanguages(DWORD flags, PULONG count, LPWSTR languages, PULONG length) { return GetUserPreferredUILanguages(flags, count, languages, length); }
EXPORT BOOL WINAPI GetProcessPreferredUILanguages(DWORD flags, PULONG count, LPWSTR languages, PULONG length) { return GetUserPreferredUILanguages(flags, count, languages, length); }
typedef BOOL (CALLBACK *LOCALE_ENUMPROCA_)(LPSTR);
typedef BOOL (CALLBACK *LOCALE_ENUMPROCW_)(LPWSTR);
typedef BOOL (CALLBACK *LOCALE_ENUMPROCEX_)(LPWSTR, DWORD, LPARAM);
EXPORT BOOL WINAPI EnumSystemLocalesA(LOCALE_ENUMPROCA_ proc, DWORD flags) { (void)flags; char id[] = "00000409"; return proc(id) || TRUE; }
EXPORT BOOL WINAPI EnumSystemLocalesW(LOCALE_ENUMPROCW_ proc, DWORD flags) { (void)flags; WCHAR id[] = {'0', '0', '0', '0', '0', '4', '0', '9', 0}; return proc(id) || TRUE; }
EXPORT BOOL WINAPI EnumSystemLocalesEx(LOCALE_ENUMPROCEX_ proc, DWORD flags, LPARAM param, LPVOID reserved) { (void)reserved; WCHAR name[] = {'e', 'n', '-', 'U', 'S', 0}; return proc(name, flags, param) || TRUE; }
typedef BOOL (CALLBACK *CODEPAGE_ENUMPROCA_)(LPSTR);
typedef BOOL (CALLBACK *CODEPAGE_ENUMPROCW_)(LPWSTR);
EXPORT BOOL WINAPI EnumSystemCodePagesA(CODEPAGE_ENUMPROCA_ proc, DWORD flags) { (void)flags; char cps[][6] = {"65001", "1252", "437"}; for (int i = 0; i < 3; ++i) if (!proc(cps[i])) break; return TRUE; }
EXPORT BOOL WINAPI EnumSystemCodePagesW(CODEPAGE_ENUMPROCW_ proc, DWORD flags) { (void)flags; WCHAR a[] = {'6', '5', '0', '0', '1', 0}, b[] = {'1', '2', '5', '2', 0}, c[] = {'4', '3', '7', 0}; if (proc(a) && proc(b)) proc(c); return TRUE; }

/* ---- number, currency, date and time formats ---------------------------------------------------------------------------------------------------------------- */

/* The number in `text` ("-1234.5") as the locale writes it: grouped with thousands separators, `digits` decimals (rounded away), a leading zero, the sign. */
static int format_decimal_text(const char *text, int digits, BOOL group, const char *decimal, const char *thousand, char *out, int capacity) {
  char whole[64], frac[64];
  int w = 0, f = 0;
  BOOL negative = FALSE;
  const char *c = text;
  while (*c == ' ') ++c;
  if (*c == '-') { negative = TRUE; ++c; }
  while (*c >= '0' && *c <= '9' && w < 60) whole[w++] = *c++;
  if (*c == '.') { ++c; while (*c >= '0' && *c <= '9' && f < 60) frac[f++] = *c++; }
  if (w == 0) whole[w++] = '0';
  while (w > 1 && whole[0] == '0') { memmove(whole, whole + 1, (size_t_)--w); }
  /* round to `digits` decimals (the digit after decides) */
  if (f > digits) {
    const BOOL up = frac[digits] >= '5';
    f = digits;
    if (up) {
      int i = f - 1;
      while (i >= 0 && frac[i] == '9') frac[i--] = '0';
      if (i >= 0) ++frac[i];
      else { i = w - 1; while (i >= 0 && whole[i] == '9') whole[i--] = '0'; if (i >= 0) ++whole[i]; else { memmove(whole + 1, whole, (size_t_)w); whole[0] = '1'; ++w; } }
    }
  }
  while (f < digits && f < 60) frac[f++] = '0';
  int n = 0;
  if (negative) { BOOL zero = TRUE; for (int i = 0; i < w; ++i) if (whole[i] != '0') zero = FALSE; for (int i = 0; i < f; ++i) if (frac[i] != '0') zero = FALSE; if (!zero) { if (n < capacity) out[n] = '-'; ++n; } }
  for (int i = 0; i < w; ++i) {
    if (group && i > 0 && (w - i) % 3 == 0) for (const char *t = thousand; *t; ++t) { if (n < capacity) out[n] = *t; ++n; }
    if (n < capacity) out[n] = whole[i];
    ++n;
  }
  if (f > 0) {
    for (const char *d = decimal; *d; ++d) { if (n < capacity) out[n] = *d; ++n; }
    for (int i = 0; i < f; ++i) { if (n < capacity) out[n] = frac[i]; ++n; }
  }
  if (n < capacity) out[n] = 0;
  return n + 1;
}
typedef struct { UINT NumDigits, LeadingZero, Grouping; LPWSTR lpDecimalSep, lpThousandSep; UINT NegativeOrder; } NUMBERFMTW_;
typedef struct { UINT NumDigits, LeadingZero, Grouping; LPWSTR lpDecimalSep, lpThousandSep; UINT NegativeOrder, PositiveOrder; LPWSTR lpCurrencySymbol; } CURRENCYFMTW_;
static int copy_narrow_to_wide(const char *text, int n, LPWSTR out, int capacity) {
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < n; ++i) out[i] = (WCHAR)(BYTE)text[i];
  return n;
}
EXPORT int WINAPI GetNumberFormatW(LCID locale, DWORD flags, LPCWSTR value, const NUMBERFMTW_ *format, LPWSTR out, int capacity) {
  (void)locale; (void)flags;
  if (!value) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  char text[64];
  int i = 0;
  for (; value[i] && i < 63; ++i) text[i] = (char)value[i];
  text[i] = 0;
  char decimal[8] = ".", thousand[8] = ",";
  int digits = 2;
  BOOL group = TRUE;
  if (format) {
    digits = (int)format->NumDigits;
    group = format->Grouping != 0;
    if (format->lpDecimalSep) { int k = 0; for (; format->lpDecimalSep[k] && k < 7; ++k) decimal[k] = (char)format->lpDecimalSep[k]; decimal[k] = 0; }
    if (format->lpThousandSep) { int k = 0; for (; format->lpThousandSep[k] && k < 7; ++k) thousand[k] = (char)format->lpThousandSep[k]; thousand[k] = 0; }
  }
  char result[128];
  const int n = format_decimal_text(text, digits, group, decimal, thousand, result, sizeof(result));
  return copy_narrow_to_wide(result, n, out, capacity);
}
EXPORT int WINAPI GetNumberFormatEx(LPCWSTR locale_name, DWORD flags, LPCWSTR value, const NUMBERFMTW_ *format, LPWSTR out, int capacity) { (void)locale_name; return GetNumberFormatW(0x409, flags, value, format, out, capacity); }
EXPORT int WINAPI GetNumberFormatA(LCID locale, DWORD flags, LPCSTR value, const NUMBERFMTW_ *format, LPSTR out, int capacity) {
  (void)format;
  WCHAR *w = value ? to_wide(value) : NULL;
  if (!w) return 0;
  WCHAR wide[128];
  int n = GetNumberFormatW(locale, flags, w, NULL, wide, 128);
  free_wide(w);
  if (n == 0) return 0;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < n; ++i) out[i] = (char)wide[i];
  return n;
}
EXPORT int WINAPI GetCurrencyFormatW(LCID locale, DWORD flags, LPCWSTR value, const CURRENCYFMTW_ *format, LPWSTR out, int capacity) {
  (void)locale; (void)flags;
  if (!value) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  char text[64];
  int i = 0;
  for (; value[i] && i < 63; ++i) text[i] = (char)value[i];
  text[i] = 0;
  const BOOL negative = text[0] == '-';
  char number[128];
  const int digits = format ? (int)format->NumDigits : 2;
  const int n = format_decimal_text(negative ? text + 1 : text, digits, TRUE, ".", ",", number, sizeof(number));
  char symbol[16] = "$";
  if (format && format->lpCurrencySymbol) { int k = 0; for (; format->lpCurrencySymbol[k] && k < 15; ++k) symbol[k] = (char)format->lpCurrencySymbol[k]; symbol[k] = 0; }
  char result[160];
  int len = 0;
  if (negative) result[len++] = '(';      /* the US negative form is ($1.00) */
  for (const char *s = symbol; *s; ++s) result[len++] = *s;
  for (int k = 0; k < n - 1; ++k) result[len++] = number[k];
  if (negative) result[len++] = ')';
  result[len++] = 0;
  return copy_narrow_to_wide(result, len, out, capacity);
}
EXPORT int WINAPI GetCurrencyFormatEx(LPCWSTR locale_name, DWORD flags, LPCWSTR value, const CURRENCYFMTW_ *format, LPWSTR out, int capacity) { (void)locale_name; return GetCurrencyFormatW(0x409, flags, value, format, out, capacity); }
EXPORT int WINAPI GetCurrencyFormatA(LCID locale, DWORD flags, LPCSTR value, const CURRENCYFMTW_ *format, LPSTR out, int capacity) {
  (void)format;
  WCHAR *w = value ? to_wide(value) : NULL;
  if (!w) return 0;
  WCHAR wide[160];
  int n = GetCurrencyFormatW(locale, flags, w, NULL, wide, 160);
  free_wide(w);
  if (n == 0) return 0;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < n; ++i) out[i] = (char)wide[i];
  return n;
}

/* A picture of a date or time ("dddd, MMMM d, yyyy", "h:mm:ss tt") filled in from a SYSTEMTIME. */
static int format_picture(const char *picture, const SYSTEMTIME *st, WCHAR *out, int capacity) {
  char result[160];
  int n = 0;
  #define PUT(ch) do { if (n < 159) result[n] = (ch); ++n; } while (0)
  #define PUTS(text) do { for (const char *q = (text); *q; ++q) PUT(*q); } while (0)
  char number[16];
  for (const char *p = picture; *p;) {
    const char c = *p;
    int run = 1;
    while (p[run] == c) ++run;
    if (c == '\'') {      /* quoted text; '' is a quote */
      ++p;
      while (*p && !(*p == '\'' && p[1] != '\'')) { if (*p == '\'' && p[1] == '\'') ++p; PUT(*p); ++p; }
      if (*p) ++p;
      continue;
    }
    p += run;
    #define NUM(v, width) do { unsigned vv = (unsigned)(v); char tmp[12]; int t = 0; do { tmp[t++] = (char)('0' + vv % 10); vv /= 10; } while (vv); while (t < (width)) tmp[t++] = '0'; for (int z = t - 1; z >= 0; --z) PUT(tmp[z]); } while (0)
    switch (c) {
      case 'd': if (run <= 2) { NUM(st->wDay, run); } else { const int day = (st->wDayOfWeek + 6) % 7; if (run == 3) { PUT(k_days[day][0]); PUT(k_days[day][1]); PUT(k_days[day][2]); } else PUTS(k_days[day]); } break;
      case 'M': if (run <= 2) { NUM(st->wMonth, run); } else if (st->wMonth >= 1 && st->wMonth <= 12) { const char *mn = k_months[st->wMonth - 1]; if (run == 3) { PUT(mn[0]); PUT(mn[1]); PUT(mn[2]); } else PUTS(mn); } break;
      case 'y': if (run <= 2) { NUM(st->wYear % 100, 2); } else { NUM(st->wYear, run); } break;
      case 'g': PUTS("A.D."); break;
      case 'h': { int h = st->wHour % 12; if (h == 0) h = 12; NUM(h, run >= 2 ? 2 : 1); break; }
      case 'H': NUM(st->wHour, run >= 2 ? 2 : 1); break;
      case 'm': NUM(st->wMinute, run >= 2 ? 2 : 1); break;
      case 's': NUM(st->wSecond, run >= 2 ? 2 : 1); break;
      case 't': if (run == 1) PUT(st->wHour < 12 ? 'A' : 'P'); else PUTS(st->wHour < 12 ? "AM" : "PM"); break;
      default: for (int k = 0; k < run; ++k) PUT(c); break;
    }
    #undef NUM
  }
  (void)number;
  #undef PUT
  #undef PUTS
  result[n < 159 ? n : 159] = 0;
  return copy_narrow_to_wide(result, (n < 159 ? n : 159) + 1, out, capacity);
}
#define DATE_SHORTDATE_ 1
#define DATE_LONGDATE_ 2
#define TIME_NOSECONDS_ 2
#define TIME_FORCE24HOURFORMAT_ 8
EXPORT int WINAPI GetDateFormatW(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  (void)locale;
  SYSTEMTIME now;
  if (!time) { GetLocalTime(&now); time = &now; }
  if (time->wMonth < 1 || time->wMonth > 12 || time->wDay < 1 || time->wDay > 31) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  char picture[64];
  if (format) { int i = 0; for (; format[i] && i < 63; ++i) picture[i] = (char)format[i]; picture[i] = 0; }
  else copy_text(picture, (flags & DATE_LONGDATE_) ? "dddd, MMMM d, yyyy" : "M/d/yyyy");
  return format_picture(picture, time, out, capacity);
}
EXPORT int WINAPI GetDateFormatEx(LPCWSTR locale_name, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity, LPCWSTR calendar) { (void)locale_name; (void)calendar; return GetDateFormatW(0x409, flags, time, format, out, capacity); }
EXPORT int WINAPI GetTimeFormatW(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) {
  (void)locale;
  SYSTEMTIME now;
  if (!time) { GetLocalTime(&now); time = &now; }
  if (time->wHour > 23 || time->wMinute > 59 || time->wSecond > 59) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  char picture[64];
  if (format) { int i = 0; for (; format[i] && i < 63; ++i) picture[i] = (char)format[i]; picture[i] = 0; }
  else if (flags & TIME_FORCE24HOURFORMAT_) copy_text(picture, (flags & TIME_NOSECONDS_) ? "HH:mm" : "HH:mm:ss");
  else copy_text(picture, (flags & TIME_NOSECONDS_) ? "h:mm tt" : "h:mm:ss tt");
  return format_picture(picture, time, out, capacity);
}
EXPORT int WINAPI GetTimeFormatEx(LPCWSTR locale_name, DWORD flags, const SYSTEMTIME *time, LPCWSTR format, LPWSTR out, int capacity) { (void)locale_name; return GetTimeFormatW(0x409, flags, time, format, out, capacity); }
EXPORT int WINAPI GetDateFormatA(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCSTR format, LPSTR out, int capacity) {
  WCHAR wide[160], *f = format ? to_wide(format) : NULL;
  const int n = GetDateFormatW(locale, flags, time, f, wide, 160);
  free_wide(f);
  if (n == 0) return 0;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < n; ++i) out[i] = (char)wide[i];
  return n;
}
EXPORT int WINAPI GetTimeFormatA(LCID locale, DWORD flags, const SYSTEMTIME *time, LPCSTR format, LPSTR out, int capacity) {
  WCHAR wide[160], *f = format ? to_wide(format) : NULL;
  const int n = GetTimeFormatW(locale, flags, time, f, wide, 160);
  free_wide(f);
  if (n == 0) return 0;
  if (capacity == 0) return n;
  if (capacity < n) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < n; ++i) out[i] = (char)wide[i];
  return n;
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
  if (!name) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (((ULONG_PTR)name >> 16) == 0) {      /* MAKEINTATOM */
    const ULONG_PTR id = (ULONG_PTR)name;
    if (id == 0 || id >= 0xC000) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    return (ATOM)id;
  }
  if (name[0] == '#') {      /* "#123" is the integer atom 123 */
    ULONG_PTR v = 0; const WCHAR *c = name + 1; BOOL digits = *c != 0;
    for (; *c; ++c) { if (*c < '0' || *c > '9') { digits = FALSE; break; } v = v * 10 + (ULONG_PTR)(*c - '0'); if (v > 0xFFFF) break; }
    if (digits) { if (v == 0 || v >= 0xC000) { set_error(ERROR_INVALID_PARAMETER); return 0; } return (ATOM)v; }
  }
  const size_t_ len = wcslen(name);
  if (len == 0 || len > 255) { set_error(len == 0 ? ERROR_INVALID_PARAMETER : ERROR_BUFFER_OVERFLOW); return 0; }
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
static UINT atom_name(ATOM atom, BOOL global, LPWSTR buffer, int capacity) {
  if (atom == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (atom < 0xC000) {      /* an integer atom is written as #n */
    WCHAR tmp[8]; int t = 0; unsigned x = atom; WCHAR digits[6]; int d = 0;
    do { digits[d++] = (WCHAR)('0' + x % 10); x /= 10; } while (x);
    tmp[t++] = '#'; while (d) tmp[t++] = digits[--d]; tmp[t] = 0;
    if (capacity <= t) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buffer, tmp, ((size_t_)t + 1) * 2);
    return (UINT)t;
  }
  const int i = atom - 0xC000;
  atom_lock();
  UINT result = 0;
  if (i >= ATOM_MAX || !g_atoms[i].name || g_atoms[i].global != global) set_error(ERROR_INVALID_HANDLE);
  else {
    const int len = (int)wcslen(g_atoms[i].name);
    if (capacity <= 0) set_error(ERROR_INSUFFICIENT_BUFFER);
    else {      /* truncated to fit, like the original */
      const int n = len < capacity - 1 ? len : capacity - 1;
      memcpy(buffer, g_atoms[i].name, (size_t_)n * 2);
      buffer[n] = 0;
      result = (UINT)n;
    }
  }
  LeaveCriticalSection(&g_atom_lock);
  return result;
}
static UINT atom_name_a(ATOM atom, BOOL global, LPSTR buffer, int capacity) {
  WCHAR wide[256];
  const UINT n = atom_name(atom, global, wide, 256);
  if (n == 0) return 0;
  const int m = capacity - 1 < (int)n ? capacity - 1 : (int)n;
  if (capacity <= 0) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < m; ++i) buffer[i] = (char)wide[i];
  buffer[m] = 0;
  return (UINT)m;
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

#define DESC_SLOTS 64
static struct { DWORD thread; WCHAR *text; } g_descriptions[DESC_SLOTS];
EXPORT HRESULT WINAPI SetThreadDescription(HANDLE thread, LPCWSTR description) {
  if (!description) return (HRESULT)0x80070057;
  DWORD id = GetThreadId(thread);
  if (id == 0) return (HRESULT)0x80070006;
  const size_t_ len = wcslen(description);
  WCHAR *copy = RtlAllocateHeap(heap(), 0, (len + 1) * 2);
  if (!copy) return (HRESULT)0x8007000E;
  memcpy(copy, description, (len + 1) * 2);
  atom_lock();      /* (any lock will do; this one is there) */
  int slot = -1;
  for (int i = 0; i < DESC_SLOTS; ++i) if (g_descriptions[i].text && g_descriptions[i].thread == id) { slot = i; break; }
  if (slot < 0) for (int i = 0; i < DESC_SLOTS; ++i) if (!g_descriptions[i].text) { slot = i; break; }
  HRESULT hr = 0;
  if (slot < 0) { RtlFreeHeap(heap(), 0, copy); hr = (HRESULT)0x8007000E; }
  else { if (g_descriptions[slot].text) RtlFreeHeap(heap(), 0, g_descriptions[slot].text); g_descriptions[slot].text = copy; g_descriptions[slot].thread = id; }
  LeaveCriticalSection(&g_atom_lock);
  return hr;
}
EXPORT HRESULT WINAPI GetThreadDescription(HANDLE thread, WCHAR **description) {
  if (!description) return (HRESULT)0x80070057;
  DWORD id = GetThreadId(thread);
  if (id == 0) return (HRESULT)0x80070006;
  atom_lock();
  const WCHAR *found = NULL;
  for (int i = 0; i < DESC_SLOTS; ++i) if (g_descriptions[i].text && g_descriptions[i].thread == id) { found = g_descriptions[i].text; break; }
  const size_t_ len = found ? wcslen(found) : 0;
  WCHAR *copy = LocalAlloc(0, (len + 1) * 2);      /* the caller frees it with LocalFree */
  if (copy) { if (found) memcpy(copy, found, (len + 1) * 2); else copy[0] = 0; }
  LeaveCriticalSection(&g_atom_lock);
  if (!copy) return (HRESULT)0x8007000E;
  *description = copy;
  return 0;
}

static BOOL is_own_process(HANDLE process) { return process == GetCurrentProcess() || (process != NULL && GetProcessId(process) == GetCurrentProcessId()); }
EXPORT BOOL WINAPI ReadProcessMemory(HANDLE process, LPCVOID base, LPVOID buffer, SIZE_T size, SIZE_T *done) {
  if (!is_own_process(process)) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  if (done) *done = 0;
  if (IsBadReadPtr(base, size)) { set_error(ERROR_PARTIAL_COPY); return FALSE; }
  memcpy(buffer, base, size);
  if (done) *done = size;
  return TRUE;
}
EXPORT BOOL WINAPI WriteProcessMemory(HANDLE process, LPVOID base, LPCVOID buffer, SIZE_T size, SIZE_T *done) {
  if (!is_own_process(process)) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  if (done) *done = 0;
  if (IsBadWritePtr(base, size)) { set_error(ERROR_PARTIAL_COPY); return FALSE; }
  memcpy(base, buffer, size);
  if (done) *done = size;
  return TRUE;
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

/* VerifyVersionInfo: the operating system calls itself Windows 10 (10.0, build 19045), like GetVersionEx does. */
#define VER_MINORVERSION_ 1
#define VER_MAJORVERSION_ 2
#define VER_BUILDNUMBER_ 4
#define VER_PLATFORMID_ 8
#define VER_SERVICEPACKMINOR_ 0x10
#define VER_SERVICEPACKMAJOR_ 0x20
EXPORT ULONGLONG WINAPI VerSetConditionMask(ULONGLONG mask, DWORD type, BYTE condition) {
  int bit = 0;
  for (DWORD t = type; t > 1; t >>= 1) ++bit;      /* VER_MINORVERSION=1 -> 0, MAJOR=2 -> 1, BUILD=4 -> 2, PLATFORM=8 -> 3, SPMINOR=0x10 -> 4, SPMAJOR=0x20 -> 5 */
  if (type == 0 || (type & (type - 1))) return mask;
  mask &= ~((ULONGLONG)7 << (bit * 3));
  return mask | ((ULONGLONG)(condition & 7) << (bit * 3));
}
static BOOL version_compare(DWORD have, DWORD want, int condition) {
  switch (condition) { case 1: return have == want; case 2: return have > want; case 3: return have >= want; case 4: return have < want; case 5: return have <= want; default: return have == want; }
}
EXPORT BOOL WINAPI VerifyVersionInfoW(OSVERSIONINFOW *info, DWORD type_mask, ULONGLONG condition_mask) {
  if (!info) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  OSVERSIONINFOW os;
  memset(&os, 0, sizeof(os));
  os.dwOSVersionInfoSize = sizeof(os);
  GetVersionExW(&os);
  #define COND(bit) ((int)((condition_mask >> ((bit) * 3)) & 7))
  /* Major, then minor: a later field only counts when the earlier ones are equal. */
  if (type_mask & VER_MAJORVERSION_) {
    if (os.dwMajorVersion != info->dwMajorVersion) {
      const int c = COND(1);
      if (!version_compare(os.dwMajorVersion, info->dwMajorVersion, c)) { set_error(ERROR_OLD_WIN_VERSION); return FALSE; }
      return TRUE;
    }
  }
  if ((type_mask & VER_MINORVERSION_) && !version_compare(os.dwMinorVersion, info->dwMinorVersion, COND(0))) { set_error(ERROR_OLD_WIN_VERSION); return FALSE; }
  if ((type_mask & VER_BUILDNUMBER_) && !version_compare(os.dwBuildNumber, info->dwBuildNumber, COND(2))) { set_error(ERROR_OLD_WIN_VERSION); return FALSE; }
  if ((type_mask & VER_PLATFORMID_) && !version_compare(os.dwPlatformId, info->dwPlatformId, COND(3))) { set_error(ERROR_OLD_WIN_VERSION); return FALSE; }
  #undef COND
  return TRUE;
}
EXPORT BOOL WINAPI GetProductInfo(DWORD major, DWORD minor, DWORD sp_major, DWORD sp_minor, PDWORD type) {
  (void)major; (void)minor; (void)sp_major; (void)sp_minor;
  if (!type) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *type = 0x30;      /* PRODUCT_PROFESSIONAL */
  return TRUE;
}

/* ---- I/O completion ports (no file I/O here is asynchronous, so a port is a queue that programs post to) ---------------------------------------------------- */

#define PORT_MAX 16
#define PORT_QUEUE 1024
typedef struct { DWORD bytes; ULONG_PTR key; LPOVERLAPPED overlapped; } PortPacket;
typedef struct { HANDLE semaphore; PortPacket packets[PORT_QUEUE]; DWORD head, count; } Port;
static Port *g_ports[PORT_MAX];
static Port *port_of(HANDLE h) {
  Port *p = NULL;
  atom_lock();
  for (int i = 0; i < PORT_MAX; ++i) if (g_ports[i] && g_ports[i]->semaphore == h) { p = g_ports[i]; break; }
  LeaveCriticalSection(&g_atom_lock);
  return p;
}
EXPORT HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD threads) {
  (void)key; (void)threads;
  if (existing) {
    if (!port_of(existing)) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
    return existing;      /* (associating a file changes nothing: its I/O completes at once) */
  }
  Port *p = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(Port));
  if (!p) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  p->semaphore = CreateSemaphoreW(NULL, 0, PORT_QUEUE, NULL);
  if (!p->semaphore) { RtlFreeHeap(heap(), 0, p); return NULL; }
  atom_lock();
  int slot = -1;
  for (int i = 0; i < PORT_MAX; ++i) if (!g_ports[i]) { slot = i; break; }
  if (slot >= 0) g_ports[slot] = p;
  LeaveCriticalSection(&g_atom_lock);
  if (slot < 0) { CloseHandle(p->semaphore); RtlFreeHeap(heap(), 0, p); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  (void)file;
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
EXPORT BOOL WINAPI GetQueuedCompletionStatus(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED *overlapped, DWORD timeout) {
  Port *p = port_of(port);
  if (!p) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (overlapped) *overlapped = NULL;
  const DWORD w = WaitForSingleObject(p->semaphore, timeout);
  if (w != WAIT_OBJECT_0) { set_error(w == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError()); return FALSE; }
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
typedef struct { LPOVERLAPPED lpOverlapped; ULONG_PTR key; ULONG_PTR internal; DWORD bytes; } OVERLAPPED_ENTRY_;
EXPORT BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE port, OVERLAPPED_ENTRY_ *entries, ULONG count, PULONG removed, DWORD timeout, BOOL alertable) {
  (void)alertable;
  ULONG n = 0;
  DWORD wait = timeout;
  while (n < count) {
    DWORD bytes; ULONG_PTR key; LPOVERLAPPED ov;
    if (!GetQueuedCompletionStatus(port, &bytes, &key, &ov, n == 0 ? wait : 0)) break;
    entries[n].lpOverlapped = ov; entries[n].key = key; entries[n].internal = 0; entries[n].bytes = bytes; ++n;
  }
  if (removed) *removed = n;
  if (n == 0) { if (GetLastError() != ERROR_INVALID_HANDLE) set_error(WAIT_TIMEOUT); return FALSE; }
  return TRUE;
}

/* ---- process lists: Toolhelp32 and EnumProcesses -------------------------------------------------------------------------------------------------------------- */

typedef struct { DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads, th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; WCHAR szExeFile[260]; } PROCESSENTRY32W_;
typedef struct { DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads, th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; CHAR szExeFile[260]; } PROCESSENTRY32A_;
#define TH32CS_SNAPPROCESS_ 2
#define SNAP_MAX 8
#define SNAP_PROCESSES 64
static BOOL IsHandleValid_(HANDLE h) { return SetEvent(h) || GetLastError() != ERROR_INVALID_HANDLE; }      /* (a snapshot's handle is an event) */
typedef struct { HANDLE handle; DWORD count, next; PROCESSENTRY32W_ entries[SNAP_PROCESSES]; } Snapshot;
static Snapshot *g_snaps[SNAP_MAX];

/* The running programs as the kernel lists them (SystemProcessInformation); returns how many were put in `out` (up to `max`). */
static DWORD list_all_processes(PROCESSENTRY32W_ *out, DWORD max) {
  ULONG need = 0;
  NtQuerySystemInformation(5, NULL, 0, &need);
  if (need == 0) return 0;
  BYTE *buffer = RtlAllocateHeap(heap(), 0, need + 4096);
  if (!buffer) return 0;
  DWORD n = 0;
  if (NT_SUCCESS(NtQuerySystemInformation(5, buffer, need + 4096, NULL))) {
    const BYTE *e = buffer;
    for (;;) {
      if (n < max) {
        PROCESSENTRY32W_ *o = &out[n];
        memset(o, 0, sizeof(*o));
        o->dwSize = sizeof(*o);
        o->cntThreads = *(const DWORD *)(e + 4);
        o->th32ProcessID = (DWORD)*(const ULONG_PTR *)(e + 0x50);
        o->th32ParentProcessID = (DWORD)*(const ULONG_PTR *)(e + 0x58);
        o->pcPriClassBase = 8;
        const WORD length = *(const WORD *)(e + 0x38) / 2;
        const WCHAR *name = *(WCHAR *const *)(e + 0x40);
        for (WORD i = 0; i < length && i < 259; ++i) o->szExeFile[i] = name[i];
        ++n;
      }
      const DWORD next = *(const DWORD *)e;
      if (next == 0) break;
      e += next;
    }
  }
  RtlFreeHeap(heap(), 0, buffer);
  return n;
}
EXPORT HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid) {
  (void)pid;
  if (!(flags & TH32CS_SNAPPROCESS_)) { set_error(ERROR_NOT_SUPPORTED); return INVALID_HANDLE_VALUE; }      /* (only the list of processes) */
  Snapshot *s = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(Snapshot));
  if (!s) { set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  s->handle = CreateEventW(NULL, TRUE, FALSE, NULL);      /* the snapshot's handle: closing it is what the program does when it is done */
  if (!s->handle) { RtlFreeHeap(heap(), 0, s); return INVALID_HANDLE_VALUE; }
  s->count = list_all_processes(s->entries, SNAP_PROCESSES);
  atom_lock();
  int slot = -1;
  for (int i = 0; i < SNAP_MAX; ++i) {
    if (g_snaps[i] && !IsHandleValid_(g_snaps[i]->handle)) { RtlFreeHeap(heap(), 0, g_snaps[i]); g_snaps[i] = NULL; }      /* (one whose handle was closed) */
    if (!g_snaps[i] && slot < 0) slot = i;
  }
  if (slot >= 0) g_snaps[slot] = s;
  LeaveCriticalSection(&g_atom_lock);
  if (slot < 0) { CloseHandle(s->handle); RtlFreeHeap(heap(), 0, s); set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  return s->handle;
}
static Snapshot *snapshot_of(HANDLE h) {
  Snapshot *s = NULL;
  atom_lock();
  for (int i = 0; i < SNAP_MAX; ++i) if (g_snaps[i] && g_snaps[i]->handle == h) { s = g_snaps[i]; break; }
  LeaveCriticalSection(&g_atom_lock);
  return s;
}
static BOOL snapshot_step(HANDLE h, BOOL first, PROCESSENTRY32W_ *out) {
  Snapshot *s = snapshot_of(h);
  if (!s) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (!out || out->dwSize < sizeof(*out)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (first) s->next = 0;
  if (s->next >= s->count) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
  *out = s->entries[s->next++];
  return TRUE;
}
EXPORT BOOL WINAPI Process32FirstW(HANDLE h, PROCESSENTRY32W_ *e) { return snapshot_step(h, TRUE, e); }
EXPORT BOOL WINAPI Process32NextW(HANDLE h, PROCESSENTRY32W_ *e) { return snapshot_step(h, FALSE, e); }
static BOOL snapshot_step_a(HANDLE h, BOOL first, PROCESSENTRY32A_ *out) {
  PROCESSENTRY32W_ w;
  if (!out || out->dwSize < sizeof(*out)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  w.dwSize = sizeof(w);
  if (!snapshot_step(h, first, &w)) return FALSE;
  out->cntUsage = w.cntUsage; out->th32ProcessID = w.th32ProcessID; out->th32DefaultHeapID = w.th32DefaultHeapID; out->th32ModuleID = w.th32ModuleID;
  out->cntThreads = w.cntThreads; out->th32ParentProcessID = w.th32ParentProcessID; out->pcPriClassBase = w.pcPriClassBase; out->dwFlags = w.dwFlags;
  int i = 0;
  for (; w.szExeFile[i] && i < 259; ++i) out->szExeFile[i] = (char)w.szExeFile[i];
  out->szExeFile[i] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI Process32First(HANDLE h, PROCESSENTRY32A_ *e) { return snapshot_step_a(h, TRUE, e); }
EXPORT BOOL WINAPI Process32Next(HANDLE h, PROCESSENTRY32A_ *e) { return snapshot_step_a(h, FALSE, e); }
EXPORT BOOL WINAPI K32EnumProcesses(DWORD *ids, DWORD size, DWORD *needed) {
  PROCESSENTRY32W_ *list = RtlAllocateHeap(heap(), 0, sizeof(PROCESSENTRY32W_) * SNAP_PROCESSES);
  if (!list) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  const DWORD n = list_all_processes(list, SNAP_PROCESSES);
  DWORD written = 0;
  for (DWORD i = 0; i < n && (written + 1) * 4 <= size; ++i) ids[written++] = list[i].th32ProcessID;
  RtlFreeHeap(heap(), 0, list);
  if (needed) *needed = written * 4;
  return TRUE;
}
