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

/* The preferred user interface languages of ntdll.dll (included by ntdll.c after rtl2.inc): the system, user and process languages are English (United States); a thread has a
 * list of up to five of its own. */

#define MUI_FULL_LANGUAGE_ 0x01u
#define MUI_LANGUAGE_ID_ 0x04u
#define MUI_LANGUAGE_NAME_ 0x08u
#define MUI_THREAD_LANGUAGES_ 0x40u
#define MUI_MACHINE_LANGUAGE_SETTINGS_ 0x400u
#define MUI_VALID_GET_ (0x02u | 0x04u | 0x08u | 0x10u | 0x20u | 0x40u | 0x100u | 0x200u | 0x400u)
#define UI_THREAD_MAX 5
static ULONG g_ui_thread[UI_THREAD_MAX];      /* LCIDs of the thread's preferred languages */
static ULONG g_ui_thread_count;

/* Writes the languages (LCIDs) as a multi-string of names or of identifiers in hexadecimal. */
static NTSTATUS ui_languages_out(DWORD flags, const ULONG *lcids, ULONG n, ULONG *count, WCHAR *buffer, ULONG *size) {
  const BOOL ids = (flags & MUI_LANGUAGE_ID_) != 0;
  ULONG needed = 1;
  for (ULONG i = 0; i < n; ++i) {
    if (ids) needed += 5;
    else { const int k = nls_find_lcid(lcids[i]); const char *name = k_rtl_locale_names + k_rtl_locale_name[k]; ULONG len = 0; while (name[len]) ++len; needed += len + 1; }
  }
  if (n == 0) needed = 2;
  *count = n;
  if (!buffer) {
    if (*size != 0) return STATUS_INVALID_PARAMETER;
    *size = needed;
    return STATUS_SUCCESS;
  }
  if (*size < needed) { *size = needed; return NLS_STATUS_BUFFER_TOO_SMALL; }
  ULONG at = 0;
  for (ULONG i = 0; i < n; ++i) {
    if (ids) {
      for (int d = 3; d >= 0; --d) buffer[at++] = (WCHAR)"0123456789ABCDEF"[(lcids[i] >> (d * 4)) & 15];
      buffer[at++] = 0;
    } else {
      const int k = nls_find_lcid(lcids[i]);
      const char *name = k_rtl_locale_names + k_rtl_locale_name[k];
      for (ULONG c = 0; name[c]; ++c) buffer[at++] = (BYTE)name[c];
      buffer[at++] = 0;
    }
  }
  buffer[at++] = 0;
  if (n == 0) buffer[at++] = 0;
  *size = at;
  return STATUS_SUCCESS;
}
static NTSTATUS ui_check_flags(DWORD flags, BOOL allow_machine) {
  if ((flags & MUI_FULL_LANGUAGE_) || (flags & ~(MUI_VALID_GET_ | 0x1u))) return STATUS_INVALID_PARAMETER;
  if ((flags & MUI_LANGUAGE_ID_) && (flags & MUI_LANGUAGE_NAME_)) return STATUS_INVALID_PARAMETER;
  if (!allow_machine && (flags & MUI_MACHINE_LANGUAGE_SETTINGS_)) return STATUS_INVALID_PARAMETER;
  return STATUS_SUCCESS;
}
static NTSTATUS ui_default_languages(DWORD flags, BOOL allow_machine, ULONG *count, WCHAR *buffer, ULONG *size) {
  static const ULONG english[1] = {0x409};
  const NTSTATUS status = ui_check_flags(flags, allow_machine);
  if (status) return status;
  if (!count || !size) return STATUS_INVALID_PARAMETER;
  return ui_languages_out(flags, english, 1, count, buffer, size);
}
EXPORT NTSTATUS NTAPI RtlGetSystemPreferredUILanguages(DWORD flags, PVOID unknown, ULONG *count, WCHAR *buffer, ULONG *size) { (void)unknown; return ui_default_languages(flags, TRUE, count, buffer, size); }
EXPORT NTSTATUS NTAPI RtlGetUserPreferredUILanguages(DWORD flags, PVOID unknown, ULONG *count, WCHAR *buffer, ULONG *size) { (void)unknown; return ui_default_languages(flags, FALSE, count, buffer, size); }
EXPORT NTSTATUS NTAPI RtlGetProcessPreferredUILanguages(DWORD flags, ULONG *count, WCHAR *buffer, ULONG *size) { return ui_default_languages(flags, FALSE, count, buffer, size); }
EXPORT NTSTATUS NTAPI RtlGetThreadPreferredUILanguages(DWORD flags, ULONG *count, WCHAR *buffer, ULONG *size) {
  const NTSTATUS status = ui_check_flags(flags, FALSE);
  if (status) return status;
  if (!count || !size) return STATUS_INVALID_PARAMETER;
  if (g_ui_thread_count || (flags & MUI_THREAD_LANGUAGES_)) return ui_languages_out(flags, g_ui_thread, g_ui_thread_count, count, buffer, size);
  return ui_default_languages(flags, FALSE, count, buffer, size);
}
/* Sets the languages of the thread from a multi-string (NULL or an empty one clears them): the names (or hexadecimal identifiers) that are known, without repeats, at most five. */
static NTSTATUS ui_set_thread(DWORD flags, const WCHAR *languages, ULONG *count) {
  if ((flags & ~(MUI_LANGUAGE_ID_ | MUI_LANGUAGE_NAME_ | 0x1u)) || ((flags & MUI_LANGUAGE_ID_) && (flags & MUI_LANGUAGE_NAME_))) return STATUS_INVALID_PARAMETER;
  if (!languages || !languages[0]) { g_ui_thread_count = 0; return STATUS_SUCCESS; }
  ULONG found[UI_THREAD_MAX], n = 0;
  BOOL any = FALSE;
  for (const WCHAR *p = languages; *p; p += wcslen(p) + 1) {
    any = TRUE;
    ULONG lcid = 0;
    if (flags & MUI_LANGUAGE_ID_) {
      BOOL valid = wcslen(p) == 4;
      for (int d = 0; valid && d < 4; ++d) {
        const WCHAR c = p[d];
        const int v = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
        if (v < 0) valid = FALSE; else lcid = lcid * 16 + (ULONG)v;
      }
      if (!valid || nls_find_lcid(lcid) < 0) continue;
    } else {
      const int k = nls_find_name(p);
      if (k < 0) continue;
      lcid = k_rtl_locale_lcid[k];
    }
    BOOL repeated = FALSE;
    for (ULONG i = 0; i < n; ++i) if (found[i] == lcid) repeated = TRUE;
    if (repeated || n >= UI_THREAD_MAX) continue;
    found[n++] = lcid;
  }
  if (any && n == 0) return NLS_STATUS_UNSUCCESSFUL;
  for (ULONG i = 0; i < n; ++i) g_ui_thread[i] = found[i];
  g_ui_thread_count = n;
  if (count) *count = n;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlSetThreadPreferredUILanguages(DWORD flags, const WCHAR *languages, ULONG *count) { return ui_set_thread(flags, languages, count); }
EXPORT NTSTATUS NTAPI RtlSetProcessPreferredUILanguages(DWORD flags, const WCHAR *languages, ULONG *count) { (void)flags; (void)languages; if (count) *count = 1; return STATUS_SUCCESS; }
/* The user interface language of the thread: the first of its languages, or English (United States). */
EXPORT USHORT NTAPI RtlThreadUILanguageInternal(void) { return (USHORT)(g_ui_thread_count ? g_ui_thread[0] : 0x409); }
