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

/* Enumerations of the locales, the language groups and the user interface languages of kernel32.dll (included by kernel32.c after nls.inc). */

typedef BOOL (CALLBACK *LOCALE_ENUMPROCA_)(LPSTR);
typedef BOOL (CALLBACK *LOCALE_ENUMPROCW_)(LPWSTR);
typedef BOOL (CALLBACK *LOCALE_ENUMPROCEX_)(LPWSTR, DWORD, LPARAM);
typedef BOOL (CALLBACK *LANGGROUP_ENUMPROCA_)(DWORD, LPSTR, LPSTR, DWORD, LONG_PTR);
typedef BOOL (CALLBACK *LANGGROUP_ENUMPROCW_)(DWORD, LPWSTR, LPWSTR, DWORD, LONG_PTR);
typedef BOOL (CALLBACK *LANGGROUPLOCALE_ENUMPROCA_)(DWORD, LCID, LPSTR, LONG_PTR);
typedef BOOL (CALLBACK *LANGGROUPLOCALE_ENUMPROCW_)(DWORD, LCID, LPWSTR, LONG_PTR);
typedef BOOL (CALLBACK *UILANGUAGE_ENUMPROCA_)(LPSTR, LONG_PTR);
typedef BOOL (CALLBACK *UILANGUAGE_ENUMPROCW_)(LPWSTR, LONG_PTR);

#define LCID_INSTALLED_ 0x1u
#define LCID_SUPPORTED_ 0x2u
#define LCID_ALTERNATE_SORTS_ 0x4u

/* "00000409" */
static void nls_hex8(DWORD value, char *text) {
  for (int i = 7; i >= 0; --i) { text[i] = "0123456789ABCDEF"[value & 15]; value >>= 4; }
  text[8] = 0;
}

EXPORT BOOL WINAPI EnumSystemLocalesA(LOCALE_ENUMPROCA_ proc, DWORD flags) {
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (flags & ~(LCID_INSTALLED_ | LCID_SUPPORTED_ | LCID_ALTERNATE_SORTS_)) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  const BOOL alternates = (flags & LCID_ALTERNATE_SORTS_) != 0, defaults = !alternates || (flags & (LCID_INSTALLED_ | LCID_SUPPORTED_));
  for (int i = 0; i < NLS_LOCALE_COUNT; ++i) {
    if (k_locale_default[i] != i || k_locale_lcid[i] == NLS_LCID_INVARIANT) continue;      /* (the specific locales only) */
    const BOOL is_alternate = k_locale_lcid[i] > 0xFFFF;
    if (is_alternate ? !alternates : !defaults) continue;
    char text[9];
    nls_hex8(k_locale_lcid[i], text);
    if (!proc(text)) break;
  }
  return TRUE;
}
EXPORT BOOL WINAPI EnumSystemLocalesW(LOCALE_ENUMPROCW_ proc, DWORD flags) {
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (flags & ~(LCID_INSTALLED_ | LCID_SUPPORTED_ | LCID_ALTERNATE_SORTS_)) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  const BOOL alternates = (flags & LCID_ALTERNATE_SORTS_) != 0, defaults = !alternates || (flags & (LCID_INSTALLED_ | LCID_SUPPORTED_));
  for (int i = 0; i < NLS_LOCALE_COUNT; ++i) {
    if (k_locale_default[i] != i || k_locale_lcid[i] == NLS_LCID_INVARIANT) continue;
    const BOOL is_alternate = k_locale_lcid[i] > 0xFFFF;
    if (is_alternate ? !alternates : !defaults) continue;
    char text[9];
    WCHAR wide[9];
    nls_hex8(k_locale_lcid[i], text);
    for (int k = 0; k < 9; ++k) wide[k] = (BYTE)text[k];
    if (!proc(wide)) break;
  }
  return TRUE;
}
/* LOCALE_WINDOWS 1, LOCALE_SUPPLEMENTAL 2, LOCALE_ALTERNATE_SORTS 4, LOCALE_NEUTRALDATA 0x10, LOCALE_SPECIFICDATA 0x20 */
EXPORT BOOL WINAPI EnumSystemLocalesEx(LOCALE_ENUMPROCEX_ proc, DWORD flags, LPARAM param, LPVOID reserved) {
  if (!proc || reserved) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (flags & ~(0x1u | 0x2u | 0x4u | 0x10u | 0x20u)) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  if ((flags & 0x1u) == 0 && (flags & 0x2u)) return TRUE;      /* (no supplemental locales) */
  const BOOL want_neutral = !(flags & 0x30u) || (flags & 0x10u), want_specific = !(flags & 0x30u) || (flags & 0x20u);
  for (int i = 0; i < NLS_LOCALE_COUNT; ++i) {
    if (k_locale_lcid[i] == NLS_LCID_INVARIANT) continue;
    const BOOL neutral = k_locale_default[i] != i;
    if (neutral ? !want_neutral : !want_specific) continue;
    if (k_locale_lcid[i] > 0xFFFF && !(flags & 0x4u)) continue;
    WCHAR name[80];
    nls_text_wide(i + 1, 0x5C, name);
    if (!proc(name, 0x1u, param)) break;
  }
  return TRUE;
}

/* ---- language groups ---------------------------------------------------------------------------------------------------------------------------------- */

#define LGRPID_MAX 17
static const char *const k_language_group_names[LGRPID_MAX] = {
  "Western Europe and United States", "Central Europe", "Baltic", "Greek", "Cyrillic", "Turkic", "Japanese", "Korean", "Traditional Chinese", "Simplified Chinese", "Thai",
  "Hebrew", "Arabic", "Vietnamese", "Indic", "Georgian", "Armenian"
};
/* The language group a locale belongs to, from its ANSI code page (the languages of Unicode only from their own groups). */
static int nls_language_group(int kind) {
  const char *cp = nls_text_of(kind, 0x1004);
  int value = 0;
  for (; cp && *cp >= '0' && *cp <= '9'; ++cp) value = value * 10 + (*cp - '0');
  switch (value) {
    case 1252: return 1;
    case 1250: return 2;
    case 1257: return 3;
    case 1253: return 4;
    case 1251: return 5;
    case 1254: return 6;
    case 932: return 7;
    case 949: return 8;
    case 950: return 9;
    case 936: return 10;
    case 874: return 11;
    case 1255: return 12;
    case 1256: return 13;
    case 1258: return 14;
    default: break;
  }
  const char *iso = nls_text_of(kind, 0x59);
  if (iso && iso[0] == 'k' && iso[1] == 'a' && !iso[2]) return 16;
  if (iso && iso[0] == 'h' && iso[1] == 'y' && !iso[2]) return 17;
  return 15;
}
EXPORT BOOL WINAPI IsValidLanguageGroup(DWORD group, DWORD flags) {
  if (flags != LCID_INSTALLED_ && flags != LCID_SUPPORTED_) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  return group >= 1 && group <= LGRPID_MAX;
}
EXPORT BOOL WINAPI EnumSystemLanguageGroupsA(LANGGROUP_ENUMPROCA_ proc, DWORD flags, LONG_PTR param) {
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (flags == 0) flags = LCID_INSTALLED_;
  if (flags != LCID_INSTALLED_ && flags != LCID_SUPPORTED_) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  for (DWORD g = 1; g <= LGRPID_MAX; ++g) {
    char number[9], name[80];
    nls_hex8(g, number);
    int n = 0;
    for (; k_language_group_names[g - 1][n]; ++n) name[n] = k_language_group_names[g - 1][n];
    name[n] = 0;
    if (!proc(g, number, name, flags, param)) break;
  }
  return TRUE;
}
EXPORT BOOL WINAPI EnumSystemLanguageGroupsW(LANGGROUP_ENUMPROCW_ proc, DWORD flags, LONG_PTR param) {
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (flags == 0) flags = LCID_INSTALLED_;
  if (flags != LCID_INSTALLED_ && flags != LCID_SUPPORTED_) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  for (DWORD g = 1; g <= LGRPID_MAX; ++g) {
    char number[9];
    WCHAR wnumber[9], name[80];
    nls_hex8(g, number);
    for (int k = 0; k < 9; ++k) wnumber[k] = (BYTE)number[k];
    int n = 0;
    for (; k_language_group_names[g - 1][n]; ++n) name[n] = (BYTE)k_language_group_names[g - 1][n];
    name[n] = 0;
    if (!proc(g, wnumber, name, flags, param)) break;
  }
  return TRUE;
}
EXPORT BOOL WINAPI EnumLanguageGroupLocalesA(LANGGROUPLOCALE_ENUMPROCA_ proc, DWORD group, DWORD flags, LONG_PTR param) {
  if (!proc || group < 1 || group > LGRPID_MAX || flags) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  for (int i = 0; i < NLS_LOCALE_COUNT; ++i) {
    if (k_locale_default[i] != i || k_locale_lcid[i] == NLS_LCID_INVARIANT || k_locale_lcid[i] > 0xFFFF) continue;
    if (nls_language_group(i + 1) != (int)group) continue;
    char text[9];
    nls_hex8(k_locale_lcid[i], text);
    if (!proc(group, k_locale_lcid[i], text, param)) break;
  }
  return TRUE;
}
EXPORT BOOL WINAPI EnumLanguageGroupLocalesW(LANGGROUPLOCALE_ENUMPROCW_ proc, DWORD group, DWORD flags, LONG_PTR param) {
  if (!proc || group < 1 || group > LGRPID_MAX || flags) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  for (int i = 0; i < NLS_LOCALE_COUNT; ++i) {
    if (k_locale_default[i] != i || k_locale_lcid[i] == NLS_LCID_INVARIANT || k_locale_lcid[i] > 0xFFFF) continue;
    if (nls_language_group(i + 1) != (int)group) continue;
    char text[9];
    WCHAR wide[9];
    nls_hex8(k_locale_lcid[i], text);
    for (int k = 0; k < 9; ++k) wide[k] = (BYTE)text[k];
    if (!proc(group, k_locale_lcid[i], wide, param)) break;
  }
  return TRUE;
}

/* ---- user interface languages ------------------------------------------------------------------------------------------------------------------------- */

/* The user interface language of this system is English (United States) only. */
EXPORT BOOL WINAPI EnumUILanguagesA(UILANGUAGE_ENUMPROCA_ proc, DWORD flags, LONG_PTR param) {
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if ((flags & ~(0x4u /* MUI_LANGUAGE_ID */ | 0x8u /* MUI_LANGUAGE_NAME */)) || flags == 0xC) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  char text[] = "0409", name[] = "en-US";
  proc((flags & 0x8u) ? name : text, param);
  return TRUE;
}
EXPORT BOOL WINAPI EnumUILanguagesW(UILANGUAGE_ENUMPROCW_ proc, DWORD flags, LONG_PTR param) {
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if ((flags & ~(0x4u | 0x8u)) || flags == 0xC) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  WCHAR text[] = {'0', '4', '0', '9', 0}, name[] = {'e', 'n', '-', 'U', 'S', 0};
  proc((flags & 0x8u) ? name : text, param);
  return TRUE;
}

/* ---- NLS versions ---------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } NLS_GUID_;
typedef struct { DWORD dwNLSVersionInfoSize, dwNLSVersion, dwDefinedVersion, dwEffectiveId; NLS_GUID_ guidCustomVersion; } NLSVERSIONINFOEX_;
#define NLS_VERSION_COMPARE_STRING 1u
#define NLS_VERSION_CURRENT 0x00060100u
#define NLS_VERSION_SHORT 12u      /* the structure without the effective identifier and the custom version */
/* The sort identifier that goes into the custom version of a language (French and Japanese sort in a way of their own). */
static DWORD nls_sort_id(int kind) {
  const char *iso = nls_text_of(kind, 0x59);
  if (iso && iso[0] == 'f' && iso[1] == 'r' && !iso[2]) return 3;
  if (iso && iso[0] == 'j' && iso[1] == 'a' && !iso[2]) return 0x46;
  return 1;
}
static void nls_version_fill(int kind, DWORD lcid, NLSVERSIONINFOEX_ *info) {
  info->dwNLSVersion = NLS_VERSION_CURRENT;
  info->dwDefinedVersion = NLS_VERSION_CURRENT;
  if (info->dwNLSVersionInfoSize >= sizeof(*info)) {
    info->dwEffectiveId = lcid;
    info->guidCustomVersion.Data1 = nls_sort_id(kind);
    info->guidCustomVersion.Data2 = 0x57ee;
    info->guidCustomVersion.Data3 = 0x1e5c;
    static const BYTE tail[8] = {0x00, 0xb4, 0xd0, 0x00, 0x0b, 0xb1, 0xe1, 0x1e};
    memcpy(info->guidCustomVersion.Data4, tail, 8);
  }
}
EXPORT BOOL WINAPI GetNLSVersion(DWORD function, LCID locale, NLSVERSIONINFOEX_ *info) {
  if (!info || info->dwNLSVersionInfoSize < NLS_VERSION_SHORT) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (function != NLS_VERSION_COMPARE_STRING) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  const int kind = nls_locale_of(locale);
  if (!kind) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  nls_version_fill(kind, k_locale_lcid[kind - 1], info);
  return TRUE;
}
EXPORT BOOL WINAPI GetNLSVersionEx(DWORD function, LPCWSTR name, NLSVERSIONINFOEX_ *info) {
  if (!info || info->dwNLSVersionInfoSize < NLS_VERSION_SHORT) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (info->dwNLSVersionInfoSize != NLS_VERSION_SHORT && info->dwNLSVersionInfoSize < sizeof(*info)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (function != NLS_VERSION_COMPARE_STRING) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  const int kind = nls_locale_of_name(name);
  if (!kind) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  int specific = kind;
  if (nls_kind_is_neutral(kind)) specific = k_locale_default[kind - 1] + 1;
  nls_version_fill(specific, name && !*name ? NLS_LCID_INVARIANT : k_locale_lcid[specific - 1], info);
  return TRUE;
}
/* The version is valid when the major part of the NLS version is the current one and the custom version is the one of the locale (or none). */
EXPORT BOOL WINAPI IsValidNLSVersion(DWORD function, LPCWSTR name, NLSVERSIONINFOEX_ *info) {
  if (function != NLS_VERSION_COMPARE_STRING || !info) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const int kind = nls_locale_of_name(name);
  if (!kind) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (info->dwNLSVersionInfoSize < NLS_VERSION_SHORT) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  BOOL valid = (info->dwNLSVersion >> 8) == (NLS_VERSION_CURRENT >> 8);
  if (valid && info->dwNLSVersionInfoSize >= sizeof(*info)) {
    int specific = nls_kind_is_neutral(kind) ? k_locale_default[kind - 1] + 1 : kind;
    const BOOL none = info->guidCustomVersion.Data1 == 0 && info->guidCustomVersion.Data2 == 0 && info->guidCustomVersion.Data3 == 0;
    valid = none || info->guidCustomVersion.Data1 == 1 || info->guidCustomVersion.Data1 == nls_sort_id(specific);
  }
  if (!valid) set_error(0);
  return valid;
}
/* Every defined character is in the Unicode of the NLS tables; a lone surrogate is not. */
EXPORT BOOL WINAPI IsNLSDefinedString(DWORD function, DWORD flags, NLSVERSIONINFOEX_ *version, LPCWSTR text, int count) {
  (void)flags;
  if (function != NLS_VERSION_COMPARE_STRING) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  if (version && version->dwNLSVersionInfoSize != NLS_VERSION_SHORT && version->dwNLSVersionInfoSize > sizeof(*version)) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (!text) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (count < 0) count = (int)wcslen(text);
  for (int i = 0; i < count; ++i) {
    const WCHAR c = text[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < count && (text[i + 1] & 0xFC00) == 0xDC00) { ++i; continue; }
    if (c >= 0xD800 && c < 0xE000) return FALSE;
    WORD c1, c2, c3;
    ctype_of(c, &c1, &c2, &c3);
    if (!(c1 & C1_DEFINED_)) return FALSE;
  }
  return TRUE;
}

/* ---- geographical locations ------------------------------------------------------------------------------------------------------------------------- */

#include "geo_data.inc"

#define GEO_NATION_ 1
#define GEO_ISO2_ 4
#define GEO_ISO3_ 5
#define GEO_RFC1766_ 6
#define GEO_LCID_ 7
#define GEO_FRIENDLYNAME_ 8
#define GEO_ISO_UN_NUMBER_ 12
#define GEO_PARENT_ 13
#define GEO_NAME_ 17
#define GEO_ID_ 18
#define GEOCLASS_NATION_ 16
#define GEOCLASS_REGION_ 14
#define GEOID_WORLD 39070u
#define GEOID_NETHERLANDS_ANTILLES 333u
static DWORD g_user_nation = 244, g_user_region = GEOID_WORLD;
static WCHAR g_user_geo_name[8] = {'U', 'S', 0};
static int geo_find(DWORD geoid) {
  for (int i = 0; i < NLS_GEO_COUNT; ++i) if (k_geo_nations[i].geoid == geoid) return i;
  return -1;
}
static BOOL geo_is_region(DWORD geoid) { return geoid == GEOID_WORLD || geoid == 47609 || geoid == 47610; }
static void geo_uint(DWORD value, char *text) {
  char digits[12];
  int n = 0, k = 0;
  do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
  while (n) text[k++] = digits[--n];
  text[k] = 0;
}
static void geo_copy(char *to, const char *from) { while ((*to++ = *from++) != 0) {} }
/* The text of an information about a location (0: no such location, -1: no such type, otherwise the length with the NUL, 0 and *empty set when it has none). */
static int geo_text(DWORD geoid, DWORD type, LANGID language, char *text /* 64 */, BOOL *has_none) {
  const int i = geo_find(geoid);
  const BOOL region = geo_is_region(geoid);
  *has_none = FALSE;
  text[0] = 0;
  if (i < 0 && !region && geoid != GEOID_NETHERLANDS_ANTILLES) return 0;
  switch (type) {
    case GEO_NATION_: if (i < 0) { *has_none = TRUE; return 1; } geo_uint(geoid, text); break;
    case GEO_ID_: geo_uint(geoid, text); break;
    case GEO_ISO2_: if (i < 0) { *has_none = TRUE; return 1; } geo_copy(text, k_geo_nations[i].iso2); break;
    case GEO_ISO3_: if (i < 0) { *has_none = TRUE; return 1; } geo_copy(text, k_geo_nations[i].iso3); break;
    case GEO_ISO_UN_NUMBER_: {
      if (i < 0) { *has_none = TRUE; return 1; }
      const unsigned un = k_geo_nations[i].un;
      text[0] = (char)('0' + un / 100); text[1] = (char)('0' + un / 10 % 10); text[2] = (char)('0' + un % 10); text[3] = 0;
      break;
    }
    case GEO_PARENT_: if (i < 0) { *has_none = TRUE; return 1; } geo_uint(k_geo_nations[i].parent, text); break;
    case GEO_NAME_:
      if (i >= 0) geo_copy(text, k_geo_nations[i].iso2);
      else if (geoid == GEOID_WORLD) geo_copy(text, "001");
      else if (geoid == 47609) geo_copy(text, "151");
      else if (geoid == 47610) geo_copy(text, "039");
      else { *has_none = TRUE; return 1; }
      break;
    case GEO_FRIENDLYNAME_: {
      if (i < 0) { *has_none = TRUE; return 1; }
      const char *name = k_geo_pool + k_geo_nations[i].name;
      int n = 0;
      for (; name[n] && n < 62; ++n) text[n] = name[n];
      text[n] = 0;
      break;
    }
    case GEO_LCID_: case GEO_RFC1766_: {
      if (i < 0) { *has_none = TRUE; return 1; }
      DWORD lang = language ? language : 0x409;
      const int kind = nls_locale_of(lang);
      if (!kind || nls_index_of_lcid(lang) < 0) return -2;
      if (type == GEO_LCID_) {
        for (int d = 7; d >= 0; --d) text[7 - d] = "0123456789ABCDEF"[(lang >> (d * 4)) & 15];
        text[8] = 0;
      } else {
        const char *iso = nls_text_of(kind, 0x59);
        int n = 0;
        for (; iso[n] && n < 8; ++n) text[n] = iso[n];
        text[n++] = '-';
        for (int k = 0; k < 2; ++k) text[n++] = (char)(k_geo_nations[i].iso2[k] | 0x20);
        text[n] = 0;
      }
      break;
    }
    case 2: case 3: case 9: case 14: case 15: case 16: *has_none = TRUE; return 1;     /* (details this system has no data for) */
    case 10: case 11: *has_none = TRUE; return 0;
    default: return -1;
  }
  return (int)strlen(text) + 1;
}
EXPORT int WINAPI GetGeoInfoA(DWORD geoid, DWORD type, LPSTR data, int capacity, LANGID language) {
  char text[64];
  BOOL none;
  const int n = geo_text(geoid, type, language, text, &none);
  if (n == -1) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (n < 0 || (n == 0 && !none)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (none) { if (capacity > 0 && data) data[0] = 0; return 0; }
  if (capacity == 0 || !data) return n;
  if (capacity < n) { for (int i = 0; i < capacity; ++i) data[i] = text[i]; set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(data, text, (SIZE_T)n);
  return n;
}
EXPORT int WINAPI GetGeoInfoW(DWORD geoid, DWORD type, LPWSTR data, int capacity, LANGID language) {
  char text[64];
  BOOL none;
  const int n = geo_text(geoid, type, language, text, &none);
  if (n == -1) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (n < 0 || (n == 0 && !none)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (none) { if (capacity > 0 && data) data[0] = 0; return 0; }
  if (capacity == 0 || !data) return n;
  if (capacity < n) { for (int i = 0; i < capacity; ++i) data[i] = (BYTE)text[i]; set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (int i = 0; i < n; ++i) data[i] = (BYTE)text[i];
  return n;
}
/* By the ISO 3166 name of the nation: "AR". */
EXPORT int WINAPI GetGeoInfoEx(LPWSTR location, DWORD type, LPWSTR data, int capacity) {
  if (!location) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (type == GEO_LCID_ || type == GEO_RFC1766_ || type == GEO_NATION_) { set_error(ERROR_INVALID_FLAGS); return 0; }
  for (int i = 0; i < NLS_GEO_COUNT; ++i) {
    if (nls_lower(location[0]) == (WCHAR)(k_geo_nations[i].iso2[0] | 0x20) && nls_lower(location[1]) == (WCHAR)(k_geo_nations[i].iso2[1] | 0x20) && !location[2])
      return GetGeoInfoW(k_geo_nations[i].geoid, type, data, capacity, 0);
  }
  set_error(ERROR_INVALID_PARAMETER);
  return 0;
}

typedef BOOL (CALLBACK *GEO_ENUMPROC_)(DWORD);
EXPORT BOOL WINAPI EnumSystemGeoID(DWORD geo_class, DWORD parent, GEO_ENUMPROC_ proc) {
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (geo_class != GEOCLASS_NATION_ && geo_class != GEOCLASS_REGION_ && geo_class != 0) { set_error(ERROR_INVALID_FLAGS); return FALSE; }
  if (geo_class != GEOCLASS_NATION_) {
    static const DWORD regions[] = {47609, 47610};
    if (parent == 0 || parent == GEOID_WORLD) for (int i = 0; i < 2; ++i) if (!proc(regions[i])) return TRUE;
    if (geo_class == GEOCLASS_REGION_) return TRUE;
  }
  for (int i = 0; i < NLS_GEO_COUNT; ++i) {
    if (parent && parent != k_geo_nations[i].parent && parent != GEOID_WORLD) continue;
    if (!proc(k_geo_nations[i].geoid)) break;
  }
  return TRUE;
}
EXPORT DWORD WINAPI GetUserGeoID(DWORD geo_class) {
  if (geo_class == GEOCLASS_NATION_) return g_user_nation;
  if (geo_class == GEOCLASS_REGION_) return g_user_region;
  return 0xFFFFFFFFu;
}
EXPORT BOOL WINAPI SetUserGeoID(DWORD geoid) {
  if (geo_find(geoid) < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  g_user_nation = geoid;
  const int i = geo_find(geoid);
  g_user_geo_name[0] = (WCHAR)(BYTE)k_geo_nations[i].iso2[0];
  g_user_geo_name[1] = (WCHAR)(BYTE)k_geo_nations[i].iso2[1];
  g_user_geo_name[2] = 0;
  return TRUE;
}
EXPORT int WINAPI GetUserDefaultGeoName(LPWSTR name, int capacity) {
  const int n = (int)wcslen(g_user_geo_name) + 1;
  if (capacity < 0 || (capacity > 0 && !name) || (capacity > 0 && capacity < n)) { set_error(capacity > 0 ? ERROR_INSUFFICIENT_BUFFER : ERROR_INVALID_PARAMETER); return 0; }
  if (capacity == 0) { if (name) { set_error(ERROR_INVALID_PARAMETER); return 0; } return n; }
  memcpy(name, g_user_geo_name, (SIZE_T)n * 2);
  return n;
}
EXPORT BOOL WINAPI SetUserGeoName(LPWSTR name) {
  if (!name) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const SIZE_T length = wcslen(name);
  if (length == 2) {
    for (int i = 0; i < NLS_GEO_COUNT; ++i)
      if (nls_lower(name[0]) == (WCHAR)(k_geo_nations[i].iso2[0] | 0x20) && nls_lower(name[1]) == (WCHAR)(k_geo_nations[i].iso2[1] | 0x20)) {
        g_user_nation = k_geo_nations[i].geoid;
        g_user_region = GEOID_WORLD;
        g_user_geo_name[0] = (WCHAR)(BYTE)k_geo_nations[i].iso2[0];
        g_user_geo_name[1] = (WCHAR)(BYTE)k_geo_nations[i].iso2[1];
        g_user_geo_name[2] = 0;
        return TRUE;
      }
    if (nls_lower(name[0]) == 'x' && nls_lower(name[1]) == 'x') return TRUE;
  } else if (length == 3 && name[0] >= '0' && name[0] <= '9') {
    memcpy(g_user_geo_name, name, 4 * 2);
    g_user_region = GEOID_WORLD;
    return TRUE;
  }
  set_error(ERROR_INVALID_PARAMETER);
  return FALSE;
}
