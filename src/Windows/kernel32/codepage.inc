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

/* Code page conversion of kernel32.dll (included by kernel32.c): MultiByteToWideChar and WideCharToMultiByte for UTF-8, UTF-7, CP_SYMBOL and the single and double byte code pages of
 * codepage_data.inc, with the parameter checks and the partial results Windows gives. The ANSI and OEM code pages of the system are UTF-8. */

#include "codepage_data.inc"

#define CPX_UTF7 65000u
#define CPX_SYMBOL 42u
#define CPX_MB_ERR_INVALID_CHARS 0x8u
#define CPX_MB_VALID (0x1u | 0x2u | 0x4u | CPX_MB_ERR_INVALID_CHARS)
#define CPX_WC_ERR_INVALID_CHARS 0x80u
#define CPX_WC_NO_BEST_FIT_CHARS 0x400u
#define CPX_WC_VALID (0x10u | 0x20u | 0x40u | 0x200u | CPX_WC_ERR_INVALID_CHARS | CPX_WC_NO_BEST_FIT_CHARS)
#define CPX_FAIL_OVERFLOW (-1)
#define CPX_FAIL_INVALID (-2)

/* The output of a conversion: with no buffer (capacity 0) only the units are counted. A unit that does not fit stops the conversion, what came before stays written. */
typedef struct { BYTE *dst; SIZE_T capacity; SIZE_T length; BOOL wide; } CpOut;

static BOOL cp_room(CpOut *o, SIZE_T units) { return !o->capacity || (units <= o->capacity && o->length <= o->capacity - units); }
static void cp_store(CpOut *o, UINT value) {
  if (o->capacity) {
    if (o->wide) { const WORD w = (WORD)value; memcpy(o->dst + o->length * 2, &w, 2); }
    else o->dst[o->length] = (BYTE)value;
  }
  o->length++;
}
static BOOL cp_put(CpOut *o, UINT value) { if (!cp_room(o, 1)) return FALSE; cp_store(o, value); return TRUE; }
/* One character of one to four bytes, all or none of it. */
static BOOL cp_put_bytes(CpOut *o, const BYTE *bytes, SIZE_T count) {
  if (!cp_room(o, count)) return FALSE;
  for (SIZE_T i = 0; i < count; ++i) cp_store(o, bytes[i]);
  return TRUE;
}
/* The strings of the caller may sit at any address (they may even overlap the buffer), so a character is read byte-wise through memcpy. */
static WCHAR cp_read_wide(const void *source, SIZE_T index) { WCHAR c; memcpy(&c, (const BYTE *)source + index * 2, 2); return c; }

static UINT cp_resolve(UINT cp) {
  switch (cp) {
    case 0: case 1: case 3: return CP_UTF8;
    case 2: return 10000;
    default: return cp;
  }
}
static const struct CodePageTable *cp_find(UINT cp) {
  for (SIZE_T i = 0; i < sizeof(g_code_pages) / sizeof(g_code_pages[0]); ++i) if (g_code_pages[i].cp == cp) return &g_code_pages[i];
  return NULL;
}
static WCHAR cp_unicode_default(const struct CodePageTable *t) { return t->cp == 932 ? 0x30fb : 0x3f; }
static BOOL cp_lookup(const DWORD *entries, DWORD count, WCHAR ch, UINT *mb) {
  DWORD lo = 0, hi = count;
  while (lo < hi) {
    const DWORD mid = lo + (hi - lo) / 2, wide = entries[mid] >> 16;
    if (wide == ch) { *mb = entries[mid] & 0xffff; return TRUE; }
    if (wide < ch) lo = mid + 1; else hi = mid;
  }
  return FALSE;
}

/* ---- UTF-8 ----------------------------------------------------------------------------------------------------------------------------------------------- */

static int cp_utf8_to_wide(DWORD flags, const BYTE *s, SIZE_T n, CpOut *o) {
  const BOOL strict = (flags & CPX_MB_ERR_INVALID_CHARS) != 0;
  BOOL invalid_seen = FALSE;      /* (in strict mode the text is converted all the same, with U+FFFD, and the call fails at the end) */
  for (SIZE_T i = 0; i < n;) {
    const BYTE b = s[i];
    UINT ch = b;
    SIZE_T need = 0, taken = 1;
    BOOL valid = TRUE;
    if (b >= 0x80) {
      UINT low = 0x80, high = 0xbf;
      if (b >= 0xc2 && b <= 0xdf) { need = 1; ch = b & 0x1f; }
      else if (b >= 0xe0 && b <= 0xef) { need = 2; ch = b & 0x0f; if (b == 0xe0) low = 0xa0; if (b == 0xed) high = 0x9f; }
      else if (b >= 0xf0 && b <= 0xf4) { need = 3; ch = b & 0x07; if (b == 0xf0) low = 0x90; if (b == 0xf4) high = 0x8f; }
      else valid = FALSE;
      SIZE_T k = 1;
      for (; valid && k <= need; ++k) {
        if (k >= n - i) { valid = FALSE; break; }      /* (the text ends inside the sequence) */
        const BYTE c = s[i + k];
        if (c < (k == 1 ? low : 0x80) || c > (k == 1 ? high : 0xbf)) {
          valid = FALSE;
          if ((c & 0xc0) == 0x80 && k == 1) ++k;      /* (a continuation byte that the lead byte does not take goes with it) */
          break;
        }
        ch = (ch << 6) | (c & 0x3f);
      }
      if (!valid && need) taken = k;
    }
    if (!valid) {
      invalid_seen = TRUE;
      if (!cp_put(o, 0xfffd)) return CPX_FAIL_OVERFLOW;
      i += taken;
      continue;
    }
    i += need + 1;
    if (ch >= 0x10000) {
      if (!cp_room(o, 2)) return CPX_FAIL_OVERFLOW;
      cp_store(o, 0xd800 + ((ch - 0x10000) >> 10));
      cp_store(o, 0xdc00 + ((ch - 0x10000) & 0x3ff));
    } else if (!cp_put(o, ch)) return CPX_FAIL_OVERFLOW;
  }
  return strict && invalid_seen ? CPX_FAIL_INVALID : 0;
}

static int cp_wide_to_utf8(DWORD flags, const void *s, SIZE_T n, CpOut *o) {
  BOOL invalid_seen = FALSE;
  for (SIZE_T i = 0; i < n; ++i) {
    UINT ch = cp_read_wide(s, i);
    if (ch >= 0xd800 && ch < 0xe000) {
      if (ch < 0xdc00 && i + 1 < n && (cp_read_wide(s, i + 1) & 0xfc00) == 0xdc00) { ch = 0x10000 + ((ch - 0xd800) << 10) + (cp_read_wide(s, i + 1) - 0xdc00); ++i; }
      else { invalid_seen = TRUE; ch = 0xfffd; }
    }
    BYTE out[4];
    SIZE_T count;
    if (ch < 0x80) { out[0] = (BYTE)ch; count = 1; }
    else if (ch < 0x800) { out[0] = (BYTE)(0xc0 | (ch >> 6)); out[1] = (BYTE)(0x80 | (ch & 0x3f)); count = 2; }
    else if (ch < 0x10000) { out[0] = (BYTE)(0xe0 | (ch >> 12)); out[1] = (BYTE)(0x80 | ((ch >> 6) & 0x3f)); out[2] = (BYTE)(0x80 | (ch & 0x3f)); count = 3; }
    else { out[0] = (BYTE)(0xf0 | (ch >> 18)); out[1] = (BYTE)(0x80 | ((ch >> 12) & 0x3f)); out[2] = (BYTE)(0x80 | ((ch >> 6) & 0x3f)); out[3] = (BYTE)(0x80 | (ch & 0x3f)); count = 4; }
    if (!cp_put_bytes(o, out, count)) return CPX_FAIL_OVERFLOW;
  }
  return (flags & CPX_WC_ERR_INVALID_CHARS) && invalid_seen ? CPX_FAIL_INVALID : 0;
}

/* ---- UTF-7 (the quirks are those of Windows: a block ends at any directly encoded character, "+-" is a plus sign) ------------------------------------------ */

static const char g_base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int cp_base64_value(BYTE c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
static BOOL cp_utf7_direct(UINT ch) {
  if (ch >= 0x80) return FALSE;
  if (ch == 0 || ch == '\t' || ch == '\r' || ch == '\n' || ch == ' ') return TRUE;
  if (ch >= 'A' && ch <= 'Z') return TRUE;
  if (ch >= 'a' && ch <= 'z') return TRUE;
  if (ch >= '0' && ch <= '9') return TRUE;
  return ch == '\'' || ch == '(' || ch == ')' || ch == ',' || ch == '-' || ch == '.' || ch == '/' || ch == ':' || ch == '?';
}

static int cp_utf7_to_wide(const BYTE *s, SIZE_T n, CpOut *o) {
  SIZE_T i = 0;
  unsigned offset = 0;
  DWORD pair = 0;
  while (i < n) {
    if (s[i] != '+') { if (!cp_put(o, s[i])) return CPX_FAIL_OVERFLOW; ++i; continue; }
    if (++i >= n) break;
    if (s[i] == '-') { if (!cp_put(o, '+')) return CPX_FAIL_OVERFLOW; ++i; continue; }
    do {
      const BYTE c = s[i];
      if (c == '-') { ++i; offset = 0; break; }
      if (c >= 0x80) break;   /* the unfinished pair is kept, as Windows does */
      const int sextet = cp_base64_value(c);
      if (sextet < 0) { offset = 0; break; }
      pair = (pair << 6) | (DWORD)sextet;
      offset += 6;
      if (offset >= 16) {
        if (!cp_put(o, (WORD)(pair >> (offset - 16)))) return CPX_FAIL_OVERFLOW;
        offset -= 16;
      }
      ++i;
    } while (i < n);
  }
  return 0;
}

static int cp_wide_to_utf7(const void *s, SIZE_T n, CpOut *o) {
  SIZE_T i = 0;
  while (i < n) {
    const WCHAR ch = cp_read_wide(s, i);
    if (ch == '+') { if (!cp_put(o, '+') || !cp_put(o, '-')) return CPX_FAIL_OVERFLOW; ++i; continue; }
    if (cp_utf7_direct(ch)) { if (!cp_put(o, ch)) return CPX_FAIL_OVERFLOW; ++i; continue; }
    unsigned offset = 0;
    DWORD pair = 0;
    if (!cp_put(o, '+')) return CPX_FAIL_OVERFLOW;
    while (i < n) {
      const WCHAR c = cp_read_wide(s, i);
      if (c == '+' || cp_utf7_direct(c)) break;
      pair = (pair << 16) | c;
      offset += 16;
      while (offset >= 6) { if (!cp_put(o, (BYTE)g_base64[(pair >> (offset - 6)) & 0x3f])) return CPX_FAIL_OVERFLOW; offset -= 6; }
      ++i;
    }
    if (offset) {
      /* Windows does not write the padding character when there is no room for the closing dash after it */
      if (!cp_room(o, 2)) return CPX_FAIL_OVERFLOW;
      cp_store(o, (BYTE)g_base64[(pair << (6 - offset)) & 0x3f]);
    }
    if (!cp_put(o, '-')) return CPX_FAIL_OVERFLOW;
  }
  return 0;
}

/* ---- CP_SYMBOL --------------------------------------------------------------------------------------------------------------------------------------- */

static int cp_symbol_to_wide(const BYTE *s, SIZE_T n, CpOut *o) {
  for (SIZE_T i = 0; i < n; ++i) if (!cp_put(o, s[i] < 0x20 ? s[i] : 0xf000u + s[i])) return CPX_FAIL_OVERFLOW;
  return 0;
}
static int cp_wide_to_symbol(const void *s, SIZE_T n, CpOut *o) {
  for (SIZE_T i = 0; i < n; ++i) {
    const WCHAR ch = cp_read_wide(s, i);
    if (ch < 0x20) { if (!cp_put(o, ch)) return CPX_FAIL_OVERFLOW; }
    else if (ch >= 0xf020 && ch <= 0xf0ff) { if (!cp_put(o, ch - 0xf000u)) return CPX_FAIL_OVERFLOW; }
    else return CPX_FAIL_INVALID;
  }
  return 0;
}

/* ---- single and double byte code pages ------------------------------------------------------------------------------------------------------------ */

static int cp_table_to_wide(const struct CodePageTable *t, DWORD flags, const BYTE *s, SIZE_T n, CpOut *o) {
  const BOOL strict = (flags & CPX_MB_ERR_INVALID_CHARS) != 0;
  for (SIZE_T i = 0; i < n; ++i) {
    const BYTE b = s[i];
    WORD ch;
    if (t->double_byte && t->lead_index[b]) {
      /* a lead byte takes the next byte as its trail unless that is missing or a NUL */
      if (i + 1 < n && s[i + 1]) { ++i; ch = t->pairs[(SIZE_T)(t->lead_index[b] - 1) * 256 + s[i]]; }
      else ch = 0xfffd;
    } else ch = t->single[b];
    if (ch == 0xfffd) {
      if (strict) return CPX_FAIL_INVALID;
      ch = cp_unicode_default(t);
    }
    if (!cp_put(o, ch)) return CPX_FAIL_OVERFLOW;
  }
  return 0;
}

static int cp_wide_to_table(const struct CodePageTable *t, DWORD flags, const void *s, SIZE_T n, const BYTE *default_char, BOOL *used, CpOut *o) {
  BYTE fallback[2] = {0x3f, 0};
  SIZE_T fallback_size = 1;
  if (default_char) {
    fallback[0] = default_char[0];
    if (t->double_byte && t->lead_index[default_char[0]] && default_char[1]) { fallback[1] = default_char[1]; fallback_size = 2; }
  }
  for (SIZE_T i = 0; i < n; ++i) {
    const WCHAR ch = cp_read_wide(s, i);
    UINT mb;
    BYTE bytes[2];
    SIZE_T count;
    BOOL substitute = FALSE;
    if (cp_lookup(t->reverse, t->reverse_count, ch, &mb) || (!(flags & CPX_WC_NO_BEST_FIT_CHARS) && cp_lookup(t->best_fit, t->best_fit_count, ch, &mb))) {
      if (mb > 0xff) { bytes[0] = (BYTE)(mb >> 8); bytes[1] = (BYTE)mb; count = 2; } else { bytes[0] = (BYTE)mb; count = 1; }
    } else {
      if (flags & CPX_WC_ERR_INVALID_CHARS) return CPX_FAIL_INVALID;
      bytes[0] = fallback[0]; bytes[1] = fallback[1]; count = fallback_size;
      substitute = TRUE;
    }
    if (!cp_put_bytes(o, bytes, count)) return CPX_FAIL_OVERFLOW;
    if (substitute && used) *used = TRUE;
  }
  return 0;
}

/* ---- the API ---------------------------------------------------------------------------------------------------------------------------------------- */

static int cp_finish(int status, const CpOut *o) {
  if (status == CPX_FAIL_OVERFLOW) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  if (status == CPX_FAIL_INVALID) { set_error(1113 /* ERROR_NO_UNICODE_TRANSLATION */); return 0; }
  if (o->length > 0x7fffffff) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return (int)o->length;
}

EXPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int length, LPWSTR dest, int capacity) {
  if (!source || length == 0 || (!dest && capacity) || capacity < 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const UINT cp = cp_resolve(code_page);
  const struct CodePageTable *table = NULL;
  if (cp == CPX_SYMBOL || cp == CPX_UTF7) { if (flags) { set_error(ERROR_INVALID_FLAGS); return 0; } }
  else if (cp == CP_UTF8) { if (flags & ~CPX_MB_ERR_INVALID_CHARS) { set_error(ERROR_INVALID_FLAGS); return 0; } }
  else {
    table = cp_find(cp);
    if (!table) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (flags & ~CPX_MB_VALID) { set_error(ERROR_INVALID_FLAGS); return 0; }
  }
  const SIZE_T n = length < 0 ? strlen(source) + 1 : (SIZE_T)length;
  CpOut out = { (BYTE *)dest, (SIZE_T)capacity, 0, TRUE };
  int status;
  if (cp == CPX_SYMBOL) status = cp_symbol_to_wide((const BYTE *)source, n, &out);
  else if (cp == CPX_UTF7) status = cp_utf7_to_wide((const BYTE *)source, n, &out);
  else if (cp == CP_UTF8) status = cp_utf8_to_wide(flags, (const BYTE *)source, n, &out);
  else status = cp_table_to_wide(table, flags, (const BYTE *)source, n, &out);
  return cp_finish(status, &out);
}

EXPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int length, LPSTR dest, int capacity, LPCSTR default_char, LPBOOL used_default) {
  if (!source || length == 0 || (!dest && capacity) || capacity < 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const UINT cp = cp_resolve(code_page);
  const struct CodePageTable *table = NULL;
  if (cp == CPX_SYMBOL || cp == CPX_UTF7) {
    if (default_char || used_default) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (flags) { set_error(ERROR_INVALID_FLAGS); return 0; }
  } else if (cp == CP_UTF8) {
    if (flags & ~CPX_WC_ERR_INVALID_CHARS) { set_error(ERROR_INVALID_FLAGS); return 0; }
  } else {
    table = cp_find(cp);
    if (!table) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (flags & ~CPX_WC_VALID) { set_error(ERROR_INVALID_FLAGS); return 0; }
  }
  BOOL used = FALSE;
  const SIZE_T n = length < 0 ? wcslen(source) + 1 : (SIZE_T)length;
  CpOut out = { (BYTE *)dest, (SIZE_T)capacity, 0, FALSE };
  int status;
  if (cp == CPX_SYMBOL) status = cp_wide_to_symbol(source, n, &out);
  else if (cp == CPX_UTF7) status = cp_wide_to_utf7(source, n, &out);
  else if (cp == CP_UTF8) status = cp_wide_to_utf8(flags, source, n, &out);
  else status = cp_wide_to_table(table, flags, source, n, (const BYTE *)default_char, &used, &out);
  if (used_default) *used_default = used;
  return cp_finish(status, &out);
}

EXPORT UINT WINAPI GetACP(void) { return CP_UTF8; }
EXPORT UINT WINAPI GetOEMCP(void) { return CP_UTF8; }
/* (GetConsoleCP, SetConsoleCP and the output ones are in console.inc: the code pages belong to the console) */

EXPORT BOOL WINAPI IsValidCodePage(UINT cp) { return cp == CP_UTF8 || cp == CPX_UTF7 || cp_find(cp) != NULL; }

typedef struct { UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]; } CPINFO;
typedef struct { UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]; WCHAR UnicodeDefaultChar; UINT CodePage; CHAR CodePageName[MAX_PATH]; } CPINFOEXA;
typedef struct { UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]; WCHAR UnicodeDefaultChar; UINT CodePage; WCHAR CodePageName[MAX_PATH]; } CPINFOEXW;

/* The details of a code page; the name is a pointer to a static string. */
static SIZE_T cp_digits(UINT cp, char *text);
/* "1252 (ANSI - Latin I)": the name of a code page as GetCPInfoEx gives it (a name that starts with the number is kept). */
static SIZE_T cp_display_name(UINT cp, const char *name, char *out /* MAX_PATH */) {
  SIZE_T n = 0;
  if (name[0] >= '0' && name[0] <= '9') { for (; name[n] && n < MAX_PATH - 1; ++n) out[n] = name[n]; out[n] = 0; return n; }
  char digits[8];
  const SIZE_T count = cp_digits(cp, digits);
  for (SIZE_T i = 0; i < count; ++i) out[n++] = digits[i];
  out[n++] = ' '; out[n++] = '(';
  for (SIZE_T i = 0; name[i] && n < MAX_PATH - 2; ++i) out[n++] = name[i];
  out[n++] = ')'; out[n] = 0;
  return n;
}

static BOOL cp_details(UINT code_page, CPINFO *info, WCHAR *unicode_default, UINT *resolved, const char **name) {
  const UINT cp = cp_resolve(code_page);
  memset(info, 0, sizeof(*info));
  info->DefaultChar[0] = '?';
  *unicode_default = '?';
  *resolved = cp;
  if (cp == CP_UTF8) { info->MaxCharSize = 4; *unicode_default = 0xfffd; *name = "65001 (UTF-8)"; return TRUE; }
  if (cp == CPX_UTF7) { info->MaxCharSize = 5; *unicode_default = 0xfffd; *name = "65000 (UTF-7)"; return TRUE; }
  const struct CodePageTable *t = cp_find(cp);
  if (!t) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  info->MaxCharSize = t->double_byte ? 2 : 1;
  *unicode_default = cp_unicode_default(t);
  *name = t->name;
  if (t->double_byte) {
    SIZE_T ranges = 0;
    for (unsigned b = 0x80; b < 0x100 && ranges < 5; ) {
      if (!t->lead_index[b]) { ++b; continue; }
      unsigned end = b;
      while (end + 1 < 0x100 && t->lead_index[end + 1]) ++end;
      info->LeadByte[ranges * 2] = (BYTE)b;
      info->LeadByte[ranges * 2 + 1] = (BYTE)end;
      ++ranges;
      b = end + 1;
    }
  }
  return TRUE;
}

EXPORT BOOL WINAPI GetCPInfo(UINT cp, CPINFO *info) {
  WCHAR unicode_default;
  UINT resolved;
  const char *name;
  if (!info) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return cp_details(cp, info, &unicode_default, &resolved, &name);
}
EXPORT BOOL WINAPI GetCPInfoExA(UINT cp, DWORD flags, CPINFOEXA *info) {
  WCHAR unicode_default;
  UINT resolved;
  const char *name;
  (void)flags;
  if (!info) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!cp_details(cp, (CPINFO *)info, &unicode_default, &resolved, &name)) return FALSE;
  info->UnicodeDefaultChar = unicode_default;
  info->CodePage = resolved;
  cp_display_name(resolved, name, info->CodePageName);
  return TRUE;
}
EXPORT BOOL WINAPI GetCPInfoExW(UINT cp, DWORD flags, CPINFOEXW *info) {
  WCHAR unicode_default;
  UINT resolved;
  const char *name;
  (void)flags;
  if (!info) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!cp_details(cp, (CPINFO *)info, &unicode_default, &resolved, &name)) return FALSE;
  info->UnicodeDefaultChar = unicode_default;
  info->CodePage = resolved;
  char text[MAX_PATH];
  const SIZE_T count = cp_display_name(resolved, name, text);
  for (SIZE_T i = 0; i <= count; ++i) info->CodePageName[i] = (BYTE)text[i];
  return TRUE;
}

EXPORT BOOL WINAPI IsDBCSLeadByteEx(UINT code_page, BYTE c) {
  const struct CodePageTable *t = cp_find(cp_resolve(code_page));
  return t && t->double_byte && t->lead_index[c];
}
EXPORT BOOL WINAPI IsDBCSLeadByte(BYTE c) { return IsDBCSLeadByteEx(CP_ACP, c); }

typedef BOOL (CALLBACK *CODEPAGE_ENUMPROCA_)(LPSTR);
typedef BOOL (CALLBACK *CODEPAGE_ENUMPROCW_)(LPWSTR);
/* The decimal digits of a code page number, written backwards into the end of `text` (of at least 8 characters, NUL ended). */
static SIZE_T cp_digits(UINT cp, char *text) {
  char digits[8];
  SIZE_T count = 0;
  do { digits[count++] = (char)('0' + cp % 10); cp /= 10; } while (cp && count < sizeof(digits) - 1);
  for (SIZE_T i = 0; i < count; ++i) text[i] = digits[count - 1 - i];
  text[count] = 0;
  return count;
}
static UINT cp_enumerated(SIZE_T index) {
  const SIZE_T tables = sizeof(g_code_pages) / sizeof(g_code_pages[0]);
  if (index < tables) return g_code_pages[index].cp;
  return index == tables ? CPX_UTF7 : CP_UTF8;
}
EXPORT BOOL WINAPI EnumSystemCodePagesA(CODEPAGE_ENUMPROCA_ proc, DWORD flags) {
  if (!proc || !flags || flags > 2) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  for (SIZE_T i = 0; i < sizeof(g_code_pages) / sizeof(g_code_pages[0]) + 2; ++i) {
    char text[8];
    cp_digits(cp_enumerated(i), text);
    if (!proc(text)) break;
  }
  return TRUE;
}
EXPORT BOOL WINAPI EnumSystemCodePagesW(CODEPAGE_ENUMPROCW_ proc, DWORD flags) {
  if (!proc || !flags || flags > 2) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  for (SIZE_T i = 0; i < sizeof(g_code_pages) / sizeof(g_code_pages[0]) + 2; ++i) {
    char text[8];
    WCHAR wide[8];
    const SIZE_T count = cp_digits(cp_enumerated(i), text);
    for (SIZE_T k = 0; k <= count; ++k) wide[k] = (BYTE)text[k];
    if (!proc(wide)) break;
  }
  return TRUE;
}
