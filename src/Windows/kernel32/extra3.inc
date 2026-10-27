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
    case 0x1006: return "MMMM yyyy";           /* SYEARMONTH */
    case 0x79: return "h:mm tt";               /* SSHORTTIME */
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

EXPORT BOOL WINAPI ReadProcessMemory(HANDLE process, LPCVOID base, LPVOID buffer, SIZE_T size, SIZE_T *done) {
  SIZE_T got = 0;
  const NTSTATUS status = NtReadVirtualMemory(process, (PVOID)base, buffer, size, &got);
  if (done) *done = got;
  return NT_SUCCESS(status) && status != (NTSTATUS)0x8000000D ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI WriteProcessMemory(HANDLE process, LPVOID base, LPCVOID buffer, SIZE_T size, SIZE_T *done) {
  SIZE_T got = 0;
  const NTSTATUS status = NtWriteVirtualMemory(process, base, (PVOID)buffer, size, &got);
  if (done) *done = got;
  return NT_SUCCESS(status) && status != (NTSTATUS)0x8000000D ? TRUE : fail_nt(status);
}
EXPORT HANDLE WINAPI CreateRemoteThreadEx(HANDLE process, LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPVOID attributes, LPDWORD id) {
  (void)sa; (void)attributes;
  HANDLE thread = NULL;
  const NTSTATUS status = NtCreateThreadEx(&thread, 0x1FFFFF, NULL, process, (PVOID)start, parameter, (flags & CREATE_SUSPENDED) ? 1 : 0, 0, stack, stack, NULL);
  if (!NT_SUCCESS(status)) { set_error(status == STATUS_NOT_IMPLEMENTED ? ERROR_NOT_SUPPORTED : RtlNtStatusToDosError(status)); return NULL; }
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
  for (int i = 0; i < PORT_MAX; ++i) if (g_ports[i] && g_ports[i]->semaphore == p->semaphore) { RtlFreeHeap(heap(), 0, g_ports[i]); g_ports[i] = NULL; }      /* (a closed port's handle value, used again) */
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

/* ---- process lists: Toolhelp32, EnumProcesses and the psapi functions ---------------------------------------------------------------------------------------- */

typedef struct { DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads, th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; WCHAR szExeFile[260]; } PROCESSENTRY32W_;
typedef struct { DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads, th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; CHAR szExeFile[260]; } PROCESSENTRY32A_;
typedef struct { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32_;
typedef struct { DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage; BYTE *modBaseAddr; DWORD modBaseSize; HMODULE hModule; WCHAR szModule[256]; WCHAR szExePath[260]; } MODULEENTRY32W_;
typedef struct { DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage; BYTE *modBaseAddr; DWORD modBaseSize; HMODULE hModule; CHAR szModule[256]; CHAR szExePath[260]; } MODULEENTRY32A_;
typedef struct { SIZE_T dwSize; DWORD th32ProcessID; ULONG_PTR th32HeapID; DWORD dwFlags; } HEAPLIST32_;
typedef struct { SIZE_T dwSize; HANDLE hHandle; ULONG_PTR dwAddress; SIZE_T dwBlockSize; DWORD dwFlags, dwLockCount, dwResvd, th32ProcessID; ULONG_PTR th32HeapID; } HEAPENTRY32_;
typedef struct { ULONG_PTR lpData; DWORD cbData; BYTE cbOverhead, iRegionIndex; WORD wFlags; ULONGLONG region[3]; } HEAPWALK_;
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
    if (s->modules) s->module_count = list_modules_of(pid, s->modules, 64); else ok = FALSE;
    if (ok && s->module_count == 0) { set_error(ERROR_INVALID_PARAMETER); ok = FALSE; }      /* (no such process) */
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
  if (!first) { w.lpData = e->dwAddress; w.iRegionIndex = (BYTE)e->dwResvd; }
  if (!NT_SUCCESS(RtlWalkHeap((PVOID)heap_id, &w))) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
  e->hHandle = (HANDLE)w.lpData; e->dwAddress = w.lpData; e->dwBlockSize = w.cbData;
  e->dwFlags = (w.wFlags & 4) ? 1 /* LF32_FIXED */ : 2 /* LF32_FREE */;
  e->dwLockCount = 0; e->dwResvd = w.iRegionIndex; e->th32ProcessID = GetCurrentProcessId(); e->th32HeapID = heap_id;
  return TRUE;
}
EXPORT BOOL WINAPI Heap32First(HEAPENTRY32_ *e, DWORD pid, ULONG_PTR heap_id) { return heap_entry_step(e, pid, heap_id, TRUE); }
EXPORT BOOL WINAPI Heap32Next(HEAPENTRY32_ *e) { return e ? heap_entry_step(e, 0, e->th32HeapID, FALSE) : (set_error(ERROR_INVALID_PARAMETER), FALSE); }
EXPORT BOOL WINAPI Toolhelp32ReadProcessMemory(DWORD pid, LPCVOID base, LPVOID buffer, SIZE_T size, SIZE_T *done) {
  if (pid != 0 && pid != GetCurrentProcessId()) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  return ReadProcessMemory(GetCurrentProcess(), base, buffer, size, done);
}
/* HeapWalk over the same walker (HeapLock and HeapUnlock are no-ops here: one lock covers the heaps). */
EXPORT BOOL WINAPI HeapWalk(HANDLE h, HEAPWALK_ *entry) {
  if (!entry) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const NTSTATUS s = RtlWalkHeap(h, entry);
  if (s == 0) return TRUE;
  set_error(s == (NTSTATUS)0x8000001A ? 259 /* ERROR_NO_MORE_ITEMS */ : ERROR_INVALID_HANDLE);
  return FALSE;
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
EXPORT DWORD WINAPI K32GetProcessImageFileNameW(HANDLE process, LPWSTR name, DWORD size) {
  ModuleRecord_ m;
  DWORD pid = 0;
  if (!find_module_record(process, NULL, &m, &pid)) return 0;
  MODULEENTRY32W_ *e = RtlAllocateHeap(heap(), 0, sizeof(*e));
  if (!e) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  fill_module_entry(e, &m, pid);
  const DWORD length = (DWORD)wcslen(e->szExePath), n = length < size ? length : (size ? size - 1 : 0);
  for (DWORD i = 0; i < n; ++i) name[i] = e->szExePath[i];
  if (size) name[n] = 0;
  RtlFreeHeap(heap(), 0, e);
  return n;
}
EXPORT DWORD WINAPI K32GetProcessImageFileNameA(HANDLE process, LPSTR name, DWORD size) {
  WCHAR wide[MAX_PATH];
  const DWORD n = K32GetProcessImageFileNameW(process, wide, MAX_PATH);
  const DWORD k = n < size ? n : (size ? size - 1 : 0);
  for (DWORD i = 0; i < k; ++i) name[i] = (char)wide[i];
  if (size) name[k] = 0;
  return k;
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
  const NTSTATUS s = NtSetInformationJobObject(job, (ULONG)info_class, info, length);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}
