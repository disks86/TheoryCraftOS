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
