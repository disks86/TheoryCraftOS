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

/* Command lines, the old Windows 3.x helpers (She*, DoEnvironmentSubst, CheckEscapes), drives, the administrator test, the version. Included by shell32.c. */

/* ---- CommandLineToArgvW ---------------------------------------------------------------------------------------------------------------------------------- */

/* The rules of the C runtime since 2008 (they are not the rules of CreateProcess for the program name): the program name stops at the first space, or the second quote when it
 * starts with one, and backslashes mean nothing in it; the arguments after it treat 2n backslashes before a quote as n, 2n+1 as n and a literal quote, and a run of quotes
 * inside or outside of a quoted part as one quote per three (the first of a run that opens a quoted part counts). One block, to be freed with LocalFree. */
EXPORT LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR line, int *count) {
  if (!count || !line) { SetLastError(ERROR_INVALID_PARAMETER); if (count) *count = -1; return NULL; }
  const size_t length = wlen(line);
  WCHAR module[MAX_PATH_EX];
  size_t module_length = 0;
  if (!*line) {
    const DWORD n = GetModuleFileNameW(NULL, module, ARRAY_LEN(module));
    module_length = n < ARRAY_LEN(module) ? n : 0;
    module[module_length] = 0;
  }
  const size_t max_args = length / 2 + 2;
  const size_t strings = length + module_length + 2 * (max_args + 1);
  LPWSTR *argv = LocalAlloc(LMEM_FIXED, (max_args + 1) * sizeof(LPWSTR) + strings * sizeof(WCHAR));
  if (!argv) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); *count = -1; return NULL; }
  WCHAR *d = (WCHAR *)(argv + max_args + 1);
  int argc = 0;
  const WCHAR *s = line;
  /* the program name */
  argv[argc++] = d;
  if (!*line) {
    for (size_t i = 0; i < module_length; ++i) *d++ = module[i];
    *d++ = 0;
  } else {
    if (*s == '"') {
      ++s;
      while (*s && *s != '"') *d++ = *s++;
      if (*s == '"') ++s;
    } else {
      while (*s && *s != ' ' && *s != '\t') *d++ = *s++;
    }
    *d++ = 0;
  }
  /* the arguments */
  for (;;) {
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) break;
    argv[argc++] = d;
    int quotes = 0, backslashes = 0;
    while (*s) {
      if ((*s == ' ' || *s == '\t') && quotes == 0) break;
      if (*s == '\\') {
        *d++ = *s++;
        ++backslashes;
      } else if (*s == '"') {
        if ((backslashes & 1) == 0) {
          d -= backslashes / 2;      /* an even number: half of them, and the quote is syntax */
          ++quotes;
        } else {
          d = d - backslashes / 2 - 1;      /* an odd number: half of them, and the quote is text */
          *d++ = '"';
        }
        ++s;
        backslashes = 0;
        while (*s == '"') {
          if (++quotes == 3) { *d++ = '"'; quotes = 0; }
          ++s;
        }
        if (quotes == 2) quotes = 0;
      } else {
        *d++ = *s++;
        backslashes = 0;
      }
    }
    *d++ = 0;
  }
  argv[argc] = NULL;
  *count = argc;
  return argv;
}

/* ---- environment strings and escapes --------------------------------------------------------------------------------------------------------------------- */

/* Replaces %NAME% in a string, in place. Low word of the result: the length of the result; high word: TRUE if every variable that was named was found and the result fit. */
EXPORT DWORD WINAPI DoEnvironmentSubstW(LPWSTR string, UINT capacity) {
  if (!string || !capacity) return 0;
  WCHAR *out = mem_alloc((size_t)capacity * sizeof(WCHAR));
  if (!out) return 0;
  BOOL success = TRUE;
  size_t n = 0;
  const WCHAR *s = string;
  while (*s) {
    if (*s == '%') {
      const WCHAR *end = w_find(s + 1, '%');
      if (end && end > s + 1) {
        WCHAR name[260], value[MAX_PATH_EX];
        if (w_copy_n(name, ARRAY_LEN(name), s + 1, (size_t)(end - s - 1))) {
          const DWORD got = GetEnvironmentVariableW(name, value, ARRAY_LEN(value));
          if (got && got < ARRAY_LEN(value)) {
            if (n + got >= capacity) { success = FALSE; break; }
            for (DWORD i = 0; i < got; ++i) out[n++] = value[i];
            s = end + 1;
            continue;
          }
        }
        success = FALSE;
      }
    }
    if (n + 1 >= capacity) { success = FALSE; break; }
    out[n++] = *s++;
  }
  out[n] = 0;
  if (success || n) { for (size_t i = 0; i <= n; ++i) string[i] = out[i]; }
  mem_free(out);
  return MAKELONG((WORD)n, success ? 1 : 0);
}
EXPORT DWORD WINAPI DoEnvironmentSubstA(LPSTR string, UINT capacity) {
  if (!string || !capacity) return 0;
  WCHAR *wide = mem_alloc((size_t)capacity * sizeof(WCHAR));
  if (!wide) return 0;
  MultiByteToWideChar(CP_ACP, 0, string, -1, wide, (int)capacity);
  wide[capacity - 1] = 0;
  const DWORD r = DoEnvironmentSubstW(wide, capacity);
  WideCharToMultiByte(CP_ACP, 0, wide, -1, string, (int)capacity, NULL, NULL);
  mem_free(wide);
  return r;
}

/* Puts quotes around a string that has a character a short file name cannot have (a space, a comma ...) if there is room. */
static BOOL needs_escape(WCHAR c) { return c == ' ' || c == ',' || c == ';' || c == '=' || c == '+' || c == '[' || c == ']'; }
EXPORT void WINAPI CheckEscapesW(LPWSTR string, DWORD capacity) {
  if (!string) return;
  size_t n = wlen(string);
  BOOL special = FALSE;
  for (size_t i = 0; i < n; ++i) if (needs_escape(string[i])) { special = TRUE; break; }
  if (!special || n + 3 > capacity) return;
  for (size_t i = n + 1; i > 0; --i) string[i] = string[i - 1];
  string[0] = '"';
  string[n + 1] = '"';
  string[n + 2] = 0;
}
EXPORT void WINAPI CheckEscapesA(LPSTR string, DWORD capacity) {
  if (!string) return;
  size_t n = 0;
  while (string[n]) ++n;
  BOOL special = FALSE;
  for (size_t i = 0; i < n; ++i) if (needs_escape((WCHAR)(BYTE)string[i])) { special = TRUE; break; }
  if (!special || n + 3 > capacity) return;
  for (size_t i = n + 1; i > 0; --i) string[i] = string[i - 1];
  string[0] = '"';
  string[n + 1] = '"';
  string[n + 2] = 0;
}

/* ---- the She functions of Windows 3.x ---------------------------------------------------------------------------------------------------------------------- */

EXPORT int WINAPI SheGetCurDrive(void) {
  WCHAR d[MAX_PATH_EX];
  if (!GetCurrentDirectoryW(ARRAY_LEN(d), d) || !has_drive(d)) return 2;
  return wup(d[0]) - 'A';
}
EXPORT BOOL WINAPI SheSetCurDrive(int drive) {
  WCHAR root[3] = {(WCHAR)('A' + drive), ':', 0};
  return drive >= 0 && drive < 26 && SetCurrentDirectoryW(root);
}
EXPORT int WINAPI SheChangeDirW(LPWSTR path) { return SetCurrentDirectoryW(path) ? 0 : -1; }
EXPORT int WINAPI SheChangeDirA(LPSTR path) {
  WCHAR *w = a_to_w(path);
  const int r = w ? SheChangeDirW(w) : -1;
  mem_free(w);
  return r;
}
/* Changes the directory and, if the path names a drive, the drive too (the current directory is global here). */
EXPORT int WINAPI SheChangeDirExW(LPWSTR path) { return SheChangeDirW(path); }
EXPORT int WINAPI SheChangeDirExA(LPSTR path) { return SheChangeDirA(path); }
/* The current directory of a drive (0 is the current drive, 1 is A:) in `buffer`. 0 on success. */
EXPORT DWORD WINAPI SheGetDirExW(LPWSTR drive_text, LPDWORD size, LPWSTR buffer) {
  WCHAR root[4] = {0, ':', 0, 0};
  if (drive_text && *drive_text) root[0] = drive_text[0]; else { WCHAR d[MAX_PATH_EX]; GetCurrentDirectoryW(ARRAY_LEN(d), d); root[0] = d[0]; }
  WCHAR full[MAX_PATH_EX];
  const DWORD n = GetFullPathNameW(root, ARRAY_LEN(full), full, NULL);
  if (!n || n >= ARRAY_LEN(full)) return ERROR_INVALID_DRIVE;
  if (size && *size < n + 1) { *size = n + 1; return ERROR_INSUFFICIENT_BUFFER; }
  for (DWORD i = 0; i <= n; ++i) buffer[i] = full[i];
  return 0;
}
EXPORT DWORD WINAPI SheGetDirW(UINT drive, LPWSTR buffer) {
  WCHAR root[3] = {(WCHAR)(drive ? 'A' + drive - 1 : SheGetCurDrive() + 'A'), ':', 0};
  DWORD size = MAX_PATH;
  return SheGetDirExW(root, &size, buffer);
}
EXPORT DWORD WINAPI SheGetDirA(UINT drive, LPSTR buffer) {
  WCHAR w[MAX_PATH_EX];
  const DWORD r = SheGetDirW(drive, w);
  if (!r) w_to_a(w, buffer, MAX_PATH);
  return r;
}
EXPORT DWORD WINAPI SheFullPathW(LPWSTR file, DWORD size, LPWSTR buffer) { return GetFullPathNameW(file, size, buffer, NULL); }
EXPORT DWORD WINAPI SheFullPathA(LPSTR file, DWORD size, LPSTR buffer) { return GetFullPathNameA(file, size, buffer, NULL); }
EXPORT DWORD WINAPI SheShortenPathW(LPWSTR path, BOOL to_short) {
  if (!to_short) return 0;
  WCHAR out[MAX_PATH_EX];
  const DWORD n = GetShortPathNameW(path, out, ARRAY_LEN(out));
  if (!n || n >= ARRAY_LEN(out) || n > wlen(path)) return 0;
  for (DWORD i = 0; i <= n; ++i) path[i] = out[i];
  return n;
}
EXPORT DWORD WINAPI SheShortenPathA(LPSTR path, BOOL to_short) {
  if (!to_short) return 0;
  char out[MAX_PATH_EX];
  const DWORD n = GetShortPathNameA(path, out, sizeof(out));
  size_t len = 0;
  while (path[len]) ++len;
  if (!n || n >= sizeof(out) || n > len) return 0;
  for (DWORD i = 0; i <= n; ++i) path[i] = out[i];
  return n;
}
/* Takes the quotes off a string in place; returns the length. */
EXPORT int WINAPI SheRemoveQuotesW(LPWSTR s) {
  if (!s) return 0;
  if (s[0] == '"') {
    size_t n = wlen(s);
    if (n >= 2 && s[n - 1] == '"') { for (size_t i = 1; i + 1 < n; ++i) s[i - 1] = s[i]; s[n - 2] = 0; }
  }
  return (int)wlen(s);
}
EXPORT int WINAPI SheRemoveQuotesA(LPSTR s) {
  if (!s) return 0;
  size_t n = 0;
  while (s[n]) ++n;
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { for (size_t i = 1; i + 1 < n; ++i) s[i - 1] = s[i]; s[n - 2] = 0; n -= 2; }
  return (int)n;
}
/* Where the part after the root of a path starts (0 if there is no root). */
EXPORT int WINAPI SheGetPathOffsetW(LPWSTR path) { return (int)path_root_length(path); }
/* A path in the form of a short name (`mode` is 0 for the path as it is). */
EXPORT int WINAPI SheConvertPathW(LPWSTR path, LPWSTR out, UINT capacity) {
  const DWORD n = GetShortPathNameW(path, out, capacity);
  if (!n || n >= capacity) return w_copy(out, capacity, path) ? 0 : -1;
  return 0;
}

/* ---- drives ---------------------------------------------------------------------------------------------------------------------------------------------- */

/* The type of a drive by number (0 is A:): DRIVE_REMOVABLE, DRIVE_FIXED ... DRIVE_NO_ROOT_DIR for a number out of range. */
EXPORT int WINAPI DriveType(int drive) {
  if (drive < 0 || drive > 25) return DRIVE_NO_ROOT_DIR;
  WCHAR root[4] = {(WCHAR)('A' + drive), ':', '\\', 0};
  return GetDriveTypeW(root);
}
EXPORT int WINAPI RealDriveType(int drive, BOOL honour_autorun) { (void)honour_autorun; return DriveType(drive); }
EXPORT int WINAPI IsNetDrive(int drive) { return DriveType(drive) == DRIVE_REMOTE; }
static BOOL long_names_on(const WCHAR *path) {
  WCHAR root[MAX_PATH_EX];
  if (!path_full(path, root, ARRAY_LEN(root))) return FALSE;
  root[path_root_length(root)] = 0;
  DWORD max_component = 0;
  if (!GetVolumeInformationW(root, NULL, 0, NULL, &max_component, NULL, NULL, 0)) return FALSE;
  return max_component > 12;
}
EXPORT BOOL WINAPI IsLFNDriveW(LPCWSTR path) { return long_names_on(path); }
EXPORT BOOL WINAPI IsLFNDriveA(LPCSTR path) {
  WCHAR *w = a_to_w(path);
  const BOOL r = w && long_names_on(w);
  mem_free(w);
  return r;
}
EXPORT BOOL WINAPI IsLFNDrive(LPCWSTR path) { return IsLFNDriveW(path); }

EXPORT BOOL WINAPI SHGetDiskFreeSpaceExW(LPCWSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes) {
  return GetDiskFreeSpaceExW(directory, available, total, free_bytes);
}
EXPORT BOOL WINAPI SHGetDiskFreeSpaceExA(LPCSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes) {
  return GetDiskFreeSpaceExA(directory, available, total, free_bytes);
}
EXPORT BOOL WINAPI SHGetDiskFreeSpaceA(LPCSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes) {
  return GetDiskFreeSpaceExA(directory, available, total, free_bytes);
}
/* The media a drive holds, as the FILE_ ... bits of the shell (not known here: no optical drives): E_NOTIMPL like a machine without them. */
EXPORT HRESULT WINAPI SHGetDriveMedia(LPCWSTR drive, DWORD *media) {
  if (media) *media = 0;
  if (!drive || !has_drive(drive)) return E_INVALIDARG;
  return E_NOTIMPL;
}
EXPORT BOOL WINAPI PathIsSlowW(LPCWSTR path, DWORD attributes) { (void)attributes; return path && is_unc(path); }
EXPORT BOOL WINAPI PathIsSlowA(LPCSTR path, DWORD attributes) { (void)attributes; return path && path[0] == '\\' && path[1] == '\\'; }
EXPORT BOOL WINAPI SHValidateUNC(HWND owner, LPWSTR path, UINT flags) {
  (void)owner; (void)flags;
  SetLastError(ERROR_BAD_NETPATH);
  return path && is_unc(path) ? FALSE : FALSE;      /* (there is no network) */
}

/* ---- the user ---------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI SHTestTokenMembership(HANDLE token, ULONG rid) {
  SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
  PSID sid = NULL;
  BOOL member = FALSE;
  if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, rid, 0, 0, 0, 0, 0, 0, &sid)) return FALSE;
  if (!CheckTokenMembership(token, sid, &member)) member = FALSE;
  FreeSid(sid);
  return member;
}
EXPORT BOOL WINAPI IsUserAnAdmin(void) { return SHTestTokenMembership(NULL, DOMAIN_ALIAS_RID_ADMINS); }
EXPORT BOOL WINAPI IsNTAdmin(DWORD reserved, DWORD *unused) { (void)reserved; (void)unused; return IsUserAnAdmin(); }

/* The identity of the program for the taskbar (a string of at most 128 characters, set once per process). */
static WCHAR g_app_id[130];
EXPORT HRESULT WINAPI SetCurrentProcessExplicitAppUserModelID(PCWSTR id) {
  if (!id || !*id || wlen(id) > 128) return E_INVALIDARG;
  EnterCriticalSection(&g_lock);
  w_copy(g_app_id, ARRAY_LEN(g_app_id), id);
  LeaveCriticalSection(&g_lock);
  return S_OK;
}
EXPORT HRESULT WINAPI GetCurrentProcessExplicitAppUserModelID(PWSTR *id) {
  if (!id) return E_INVALIDARG;
  *id = NULL;
  EnterCriticalSection(&g_lock);
  if (g_app_id[0]) *id = w_task_dup(g_app_id);
  LeaveCriticalSection(&g_lock);
  if (!*id) return g_app_id[0] ? E_OUTOFMEMORY : E_FAIL;
  return S_OK;
}

/* ---- the DLL ------------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT HRESULT WINAPI DllGetVersion(DLLVERSIONINFO *info) {
  if (!info || (info->cbSize != sizeof(DLLVERSIONINFO) && info->cbSize != sizeof(DLLVERSIONINFO2))) return E_INVALIDARG;
  info->dwMajorVersion = 10;
  info->dwMinorVersion = 0;
  info->dwBuildNumber = 19041;
  info->dwPlatformID = DLLVER_PLATFORM_NT;
  if (info->cbSize == sizeof(DLLVERSIONINFO2)) ((DLLVERSIONINFO2 *)info)->ullVersion = MAKEDLLVERULL(10, 0, 19041, 1);
  return S_OK;
}
EXPORT HRESULT WINAPI DllCanUnloadNow(void) { return g_objects ? S_FALSE : S_OK; }

/* The extensions of the files a program is made of. */
static BOOL is_program_extension(const WCHAR *ext) {
  static const WCHAR *const kinds[] = {L".exe", L".com", L".bat", L".cmd", L".pif", L".scr"};
  for (size_t i = 0; i < ARRAY_LEN(kinds); ++i) if (!wicmp(ext, kinds[i])) return TRUE;
  return FALSE;
}
EXPORT BOOL WINAPI PathIsExeW(LPCWSTR path) { return path && is_program_extension(path_extension(path)); }
EXPORT BOOL WINAPI PathIsExeWorker(LPCWSTR path) { return PathIsExeW(path); }
EXPORT BOOL WINAPI PathIsExe(LPCWSTR path) { return PathIsExeW(path); }
