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

/* advapi32.dll for TheoryCraftOS: the registry, and a few of the security functions programs link this library for without really using security. Written entirely over
 * kernel32.dll (no NT services of its own): a registry key is a directory under C:\Windows\System32\config\<hive>, and a value is one file in it holding a small
 * [type, length, data] header the program's bytes follow; the predefined keys (HKEY_LOCAL_MACHINE and so on) are fixed pseudo-handles that map to the hive directories, made
 * the first time something is created under them. This makes the registry an ordinary part of the file system: it is exactly as persistent as whatever is mounted there (the
 * RAM file system of the boot demo keeps nothing between boots, like everything else in it; a real disk would). There is one registry, shared by every process, matching how
 * the real one is machine-wide; this OS has no separate per-user hive, so HKEY_CURRENT_USER is just another fixed directory. Built freestanding with the OS's mingw compiler,
 * directly over kernel32.dll, the way a real advapi32.dll is layered over kernel32.dll's file functions for the registry. */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

typedef LONG LSTATUS;
typedef DWORD REGSAM;
typedef HKEY *PHKEY;

/* From kernel32.dll. */
IMPORT PVOID NTAPI RtlAllocateHeap(PVOID, ULONG, SIZE_T);
IMPORT BOOLEAN NTAPI RtlFreeHeap(PVOID, ULONG, PVOID);
IMPORT PVOID NTAPI RtlProcessHeap(void);
IMPORT void *memcpy(void *, const void *, size_t_);
IMPORT void *memset(void *, int, size_t_);
IMPORT int memcmp(const void *, const void *, size_t_);
IMPORT size_t_ wcslen(const WCHAR *);
IMPORT size_t_ strlen(const char *);
IMPORT DWORD WINAPI GetLastError(void);
IMPORT void WINAPI SetLastError(DWORD error);
IMPORT HANDLE WINAPI CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file);
IMPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPOVERLAPPED overlapped);
IMPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPOVERLAPPED overlapped);
IMPORT BOOL WINAPI CloseHandle(HANDLE h);
IMPORT BOOL WINAPI DeleteFileW(LPCWSTR path);
IMPORT BOOL WINAPI CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa);
IMPORT BOOL WINAPI RemoveDirectoryW(LPCWSTR path);
IMPORT DWORD WINAPI GetFileAttributesW(LPCWSTR path);
IMPORT HANDLE WINAPI FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data);
IMPORT BOOL WINAPI FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data);
IMPORT BOOL WINAPI FindClose(HANDLE h);
IMPORT ULONGLONG WINAPI GetTickCount64(void);
IMPORT DWORD WINAPI GetCurrentProcessId(void);

static PVOID heap(void) { return RtlProcessHeap(); }
static LSTATUS fail(void) { return (LSTATUS)GetLastError(); }

/* ---- wide string helpers (advapi32 is its own small freestanding program: no CRT, so no wcscpy/wcscat/wcsrchr) ---------------------------------------------- */

static void wcopy(WCHAR *dest, const WCHAR *src) { while ((*dest++ = *src++) != 0) {} }
static void wappend(WCHAR *dest, const WCHAR *src) { while (*dest) ++dest; wcopy(dest, src); }
static BOOL is_dot_entry(const WCHAR *name) { return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)); }      /* "." and ".." (FindFirstFile lists them here, unlike the real one) */
static BOOL wequal_ignore_case(const WCHAR *a, const WCHAR *b) {
  for (;; ++a, ++b) {
    WCHAR x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = (WCHAR)(x - 32);
    if (y >= 'a' && y <= 'z') y = (WCHAR)(y - 32);
    if (x != y) return FALSE;
    if (!x) return TRUE;
  }
}

/* ---- predefined keys and paths ------------------------------------------------------------------------------------------------------------------------- */

#define HKEY_CLASSES_ROOT_ ((HKEY)(ULONG_PTR)(LONG)0x80000000)
#define HKEY_CURRENT_USER_ ((HKEY)(ULONG_PTR)(LONG)0x80000001)
#define HKEY_LOCAL_MACHINE_ ((HKEY)(ULONG_PTR)(LONG)0x80000002)
#define HKEY_USERS_ ((HKEY)(ULONG_PTR)(LONG)0x80000003)
#define HKEY_CURRENT_CONFIG_ ((HKEY)(ULONG_PTR)(LONG)0x80000005)
#define PATH_CAP_ (PATH_CHARS + 8)

/* An open key: the directory it is (the registry has no separate "handle" from the path, so this is all there is to it). Freed by RegCloseKey. */
typedef struct { WCHAR path[PATH_CAP_]; } OpenKey_;

/* The fixed directory a predefined key maps to (made, with its parents, the first time something needs it to exist). NULL if `key` is not one of them. */
static const WCHAR *predefined_path(HKEY key) {
  if (key == HKEY_CLASSES_ROOT_) { static const WCHAR p[] = L"C:\\Windows\\System32\\config\\HKCR"; return p; }
  if (key == HKEY_CURRENT_USER_) { static const WCHAR p[] = L"C:\\Windows\\System32\\config\\HKCU"; return p; }
  if (key == HKEY_LOCAL_MACHINE_) { static const WCHAR p[] = L"C:\\Windows\\System32\\config\\HKLM"; return p; }
  if (key == HKEY_USERS_) { static const WCHAR p[] = L"C:\\Windows\\System32\\config\\HKU"; return p; }
  if (key == HKEY_CURRENT_CONFIG_) { static const WCHAR p[] = L"C:\\Windows\\System32\\config\\HKCC"; return p; }
  return NULL;
}
/* The directory of any key, predefined or one RegOpenKeyEx/RegCreateKeyEx gave out. NULL (with an error set) for anything else. */
static const WCHAR *key_path(HKEY key) {
  const WCHAR *predefined = predefined_path(key);
  if (predefined) return predefined;
  if (!key) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }      /* (any other non-null value is assumed to be one of ours, from RegOpenKeyEx/RegCreateKeyEx) */
  return ((OpenKey_ *)key)->path;
}
/* Makes every level of `path` that does not exist yet (CreateDirectory only makes one level, and real RegCreateKeyEx makes the whole chain). */
static BOOL make_directories(const WCHAR *path) {
  WCHAR scratch[PATH_CAP_];
  wcopy(scratch, path);
  for (WCHAR *p = scratch + 3; *p; ++p) {      /* skip the drive ("C:\\"): it exists */
    if (*p != '\\') continue;
    *p = 0;
    if (!CreateDirectoryW(scratch, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    *p = '\\';
  }
  return CreateDirectoryW(scratch, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
/* `base` \ `sub` (backslashes in `sub` make nested subkeys, as real key paths do); fails if the joined path is too long or `sub` reaches outside the registry. */
static BOOL join_path(const WCHAR *base, LPCWSTR sub, WCHAR *out) {
  wcopy(out, base);
  if (sub && *sub) {
    if (wcslen(base) + 1 + wcslen(sub) >= PATH_CAP_) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (const WCHAR *s = sub; *s; ++s) if (*s == '/' || (*s == '.' && s[1] == '.')) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    wappend(out, L"\\");
    wappend(out, sub);
  }
  return TRUE;
}
/* The file a value lives in: `key_directory`\(Default), or \<name> for a named one. NULL (with an error set) for a name this scheme cannot represent. */
static BOOL value_path(const WCHAR *directory, LPCWSTR name, WCHAR *out) {
  wcopy(out, directory);
  wappend(out, L"\\");
  if (!name || !*name) { wappend(out, L"(Default)"); return TRUE; }
  if (wequal_ignore_case(name, L"(Default)")) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }      /* the one name this scheme reserves */
  for (LPCWSTR s = name; *s; ++s) if (*s == '/' || *s == '\\') { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (wcslen(directory) + 1 + wcslen(name) >= PATH_CAP_) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  wappend(out, name);
  return TRUE;
}

/* ---- opening, creating, closing ------------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS WINAPI RegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD options, REGSAM access, PHKEY result) {
  (void)options; (void)access;
  if (!result) return ERROR_INVALID_PARAMETER;
  const WCHAR *base = key_path(key);
  if (!base) return fail();
  WCHAR joined[PATH_CAP_];
  if (!join_path(base, sub, joined)) return fail();
  const DWORD attributes = GetFileAttributesW(joined);
  if (attributes == INVALID_FILE_ATTRIBUTES) return ERROR_FILE_NOT_FOUND;
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_FILE_NOT_FOUND;      /* a value, not a key */
  OpenKey_ *opened = RtlAllocateHeap(heap(), 0, sizeof(OpenKey_));
  if (!opened) return ERROR_NOT_ENOUGH_MEMORY;
  wcopy(opened->path, joined);
  *result = (HKEY)opened;
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegOpenKeyW(HKEY key, LPCWSTR sub, PHKEY result) { return RegOpenKeyExW(key, sub, 0, 0, result); }
EXPORT LSTATUS WINAPI RegCreateKeyExW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR class_name, DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES sa, PHKEY result,
                                      LPDWORD disposition) {
  (void)reserved; (void)class_name; (void)options; (void)access; (void)sa;
  if (!result) return ERROR_INVALID_PARAMETER;
  const WCHAR *base = key_path(key);
  if (!base) return fail();
  WCHAR joined[PATH_CAP_];
  if (!join_path(base, sub, joined)) return fail();
  const BOOL existed = GetFileAttributesW(joined) != INVALID_FILE_ATTRIBUTES;
  if (!make_directories(joined)) return fail();
  OpenKey_ *opened = RtlAllocateHeap(heap(), 0, sizeof(OpenKey_));
  if (!opened) return ERROR_NOT_ENOUGH_MEMORY;
  wcopy(opened->path, joined);
  *result = (HKEY)opened;
  if (disposition) *disposition = existed ? 2 /* REG_OPENED_EXISTING_KEY */ : 1 /* REG_CREATED_NEW_KEY */;
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegCreateKeyW(HKEY key, LPCWSTR sub, PHKEY result) { return RegCreateKeyExW(key, sub, 0, NULL, 0, 0, NULL, result, NULL); }
EXPORT LSTATUS WINAPI RegCloseKey(HKEY key) {
  if (!predefined_path(key) && key) RtlFreeHeap(heap(), 0, key);      /* closing a predefined key does nothing, like the real one */
  return ERROR_SUCCESS;
}

/* ---- values --------------------------------------------------------------------------------------------------------------------------------------------- */

/* A value's file holds [type: 4][length: 4][data: length]; building and reading it. */
EXPORT LSTATUS WINAPI RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD length) {
  (void)reserved;
  const WCHAR *directory = key_path(key);
  if (!directory) return fail();
  WCHAR path[PATH_CAP_];
  if (!value_path(directory, name, path)) return fail();
  if (length && !data) return ERROR_INVALID_PARAMETER;
  HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) return fail();
  BYTE header[8];
  header[0] = (BYTE)type; header[1] = (BYTE)(type >> 8); header[2] = (BYTE)(type >> 16); header[3] = (BYTE)(type >> 24);
  header[4] = (BYTE)length; header[5] = (BYTE)(length >> 8); header[6] = (BYTE)(length >> 16); header[7] = (BYTE)(length >> 24);
  DWORD put = 0;
  BOOL ok = WriteFile(h, header, sizeof(header), &put, NULL) && put == sizeof(header);
  if (ok && length) ok = WriteFile(h, data, length, &put, NULL) && put == length;
  CloseHandle(h);
  if (!ok) { const LSTATUS failed = fail(); DeleteFileW(path); return failed; }      /* half-written: cleaned up rather than left behind */
  return ERROR_SUCCESS;
}
static LSTATUS read_value(const WCHAR *directory, LPCWSTR name, LPDWORD type, BYTE *data, LPDWORD capacity_io) {
  WCHAR path[PATH_CAP_];
  if (!value_path(directory, name, path)) return fail();
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) return fail();
  BYTE header[8];
  DWORD got = 0;
  if (!ReadFile(h, header, sizeof(header), &got, NULL) || got != sizeof(header)) { CloseHandle(h); return ERROR_INVALID_DATA; }
  const DWORD found_type = header[0] | ((DWORD)header[1] << 8) | ((DWORD)header[2] << 16) | ((DWORD)header[3] << 24);
  const DWORD length = header[4] | ((DWORD)header[5] << 8) | ((DWORD)header[6] << 16) | ((DWORD)header[7] << 24);
  if (type) *type = found_type;
  LSTATUS status = ERROR_SUCCESS;
  if (capacity_io) {
    if (!data) { *capacity_io = length; }
    else if (*capacity_io < length) { *capacity_io = length; status = ERROR_MORE_DATA; }
    else {
      *capacity_io = length;
      if (length && (!ReadFile(h, data, length, &got, NULL) || got != length)) status = ERROR_INVALID_DATA;
    }
  }
  CloseHandle(h);
  return status;
}
EXPORT LSTATUS WINAPI RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, BYTE *data, LPDWORD capacity) {
  (void)reserved;
  const WCHAR *directory = key_path(key);
  if (!directory) return fail();
  return read_value(directory, name, type, data, capacity);
}
EXPORT LSTATUS WINAPI RegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD capacity) {
  HKEY opened = key;
  BOOL close_it = FALSE;
  if (sub && *sub) {
    const LSTATUS status = RegOpenKeyExW(key, sub, 0, 0x20019 /* KEY_READ */, &opened);
    if (status != ERROR_SUCCESS) return status;
    close_it = TRUE;
  }
  DWORD found_type = 0;
  const LSTATUS status = RegQueryValueExW(opened, name, NULL, &found_type, (BYTE *)data, capacity);
  if (close_it) RegCloseKey(opened);
  if (status != ERROR_SUCCESS) return status;
  /* RRF_RT_ANY (0xFFFF omitted): restricting to specific types is not enforced, which is the permissive end of what real callers ask for anyway. */
  (void)flags;
  if (type) *type = found_type;
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegDeleteValueW(HKEY key, LPCWSTR name) {
  const WCHAR *directory = key_path(key);
  if (!directory) return fail();
  WCHAR path[PATH_CAP_];
  if (!value_path(directory, name, path)) return fail();
  return DeleteFileW(path) ? ERROR_SUCCESS : fail();
}
EXPORT LSTATUS WINAPI RegDeleteKeyW(HKEY key, LPCWSTR sub) {
  const WCHAR *base = key_path(key);
  if (!base) return fail();
  WCHAR joined[PATH_CAP_];
  if (!join_path(base, sub, joined) || !*sub) return sub && *sub ? fail() : ERROR_INVALID_PARAMETER;
  WCHAR pattern[PATH_CAP_];
  wcopy(pattern, joined);
  wappend(pattern, L"\\*");
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find != INVALID_HANDLE_VALUE) {
    BOOL more = TRUE;
    for (; more; more = FindNextFileW(find, &fd)) {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (is_dot_entry(fd.cFileName)) continue;
        FindClose(find); return ERROR_ACCESS_DENIED;      /* has subkeys: like the real one, this does not cascade */
      }
      WCHAR victim[PATH_CAP_];
      wcopy(victim, joined);
      wappend(victim, L"\\");
      wappend(victim, fd.cFileName);
      DeleteFileW(victim);
    }
    FindClose(find);
  }
  return RemoveDirectoryW(joined) ? ERROR_SUCCESS : fail();
}

/* ---- enumeration and key information --------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS WINAPI RegEnumKeyExW(HKEY key, DWORD index, LPWSTR name, LPDWORD name_capacity, LPDWORD reserved, LPWSTR class_name, LPDWORD class_capacity,
                                    PFILETIME write_time) {
  (void)reserved; (void)class_name; (void)class_capacity; (void)write_time;
  const WCHAR *directory = key_path(key);
  if (!directory) return fail();
  if (!name || !name_capacity) return ERROR_INVALID_PARAMETER;
  WCHAR pattern[PATH_CAP_];
  wcopy(pattern, directory);
  wappend(pattern, L"\\*");
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find == INVALID_HANDLE_VALUE) return ERROR_NO_MORE_ITEMS;
  DWORD at = 0;
  LSTATUS status = ERROR_NO_MORE_ITEMS;
  for (BOOL more = TRUE; more; more = FindNextFileW(find, &fd)) {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || is_dot_entry(fd.cFileName)) continue;      /* a value, or "." / "..": not a subkey */
    if (at++ != index) continue;
    const DWORD length = (DWORD)wcslen(fd.cFileName);
    if (*name_capacity <= length) { status = ERROR_MORE_DATA; break; }
    memcpy(name, fd.cFileName, (length + 1) * 2);
    *name_capacity = length;
    status = ERROR_SUCCESS;
    break;
  }
  FindClose(find);
  return status;
}
EXPORT LSTATUS WINAPI RegEnumValueW(HKEY key, DWORD index, LPWSTR name, LPDWORD name_capacity, LPDWORD reserved, LPDWORD type, BYTE *data, LPDWORD data_capacity) {
  (void)reserved;
  const WCHAR *directory = key_path(key);
  if (!directory) return fail();
  if (!name || !name_capacity) return ERROR_INVALID_PARAMETER;
  WCHAR pattern[PATH_CAP_];
  wcopy(pattern, directory);
  wappend(pattern, L"\\*");
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find == INVALID_HANDLE_VALUE) return ERROR_NO_MORE_ITEMS;
  DWORD at = 0;
  LSTATUS status = ERROR_NO_MORE_ITEMS;
  for (BOOL more = TRUE; more; more = FindNextFileW(find, &fd)) {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;      /* a subkey, not a value */
    if (at++ != index) continue;
    const BOOL is_default = wequal_ignore_case(fd.cFileName, L"(Default)");
    const DWORD length = is_default ? 0 : (DWORD)wcslen(fd.cFileName);
    if (*name_capacity <= length) { status = ERROR_MORE_DATA; break; }
    if (length) memcpy(name, fd.cFileName, (length + 1) * 2);
    else name[0] = 0;
    *name_capacity = length;
    status = read_value(directory, is_default ? NULL : fd.cFileName, type, data, data_capacity);
    break;
  }
  FindClose(find);
  return status;
}
EXPORT LSTATUS WINAPI RegQueryInfoKeyW(HKEY key, LPWSTR class_name, LPDWORD class_capacity, LPDWORD reserved, LPDWORD subkeys, LPDWORD max_subkey_length,
                                       LPDWORD max_class_length, LPDWORD values, LPDWORD max_value_name_length, LPDWORD max_value_data_length, LPDWORD security,
                                       PFILETIME write_time) {
  (void)class_name; (void)class_capacity; (void)reserved; (void)security; (void)write_time;
  const WCHAR *directory = key_path(key);
  if (!directory) return fail();
  WCHAR pattern[PATH_CAP_];
  wcopy(pattern, directory);
  wappend(pattern, L"\\*");
  DWORD subkey_count = 0, value_count = 0, longest_subkey = 0, longest_value_name = 0, longest_value_data = 0;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find != INVALID_HANDLE_VALUE) {
    for (BOOL more = TRUE; more; more = FindNextFileW(find, &fd)) {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && is_dot_entry(fd.cFileName)) continue;
      const DWORD length = (DWORD)wcslen(fd.cFileName);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ++subkey_count; if (length > longest_subkey) longest_subkey = length; continue; }
      ++value_count;
      const DWORD name_length = wequal_ignore_case(fd.cFileName, L"(Default)") ? 0 : length;
      if (name_length > longest_value_name) longest_value_name = name_length;
      const DWORD data_length = (fd.nFileSizeLow > 8 ? fd.nFileSizeLow - 8 : 0);
      if (data_length > longest_value_data) longest_value_data = data_length;
    }
    FindClose(find);
  }
  if (subkeys) *subkeys = subkey_count;
  if (max_subkey_length) *max_subkey_length = longest_subkey;
  if (values) *values = value_count;
  if (max_value_name_length) *max_value_name_length = longest_value_name;
  if (max_value_data_length) *max_value_data_length = longest_value_data;
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegFlushKey(HKEY key) { return key_path(key) ? ERROR_SUCCESS : fail(); }      /* always live: there is no cache to flush */

/* ---- ANSI wrappers ---------------------------------------------------------------------------------------------------------------------------------------- */

static WCHAR *to_wide_(const char *text) {
  if (!text) return NULL;
  const size_t_ n = strlen(text);
  WCHAR *w = RtlAllocateHeap(heap(), 0, (n + 1) * 2);
  if (!w) return NULL;
  for (size_t_ i = 0; i <= n; ++i) w[i] = (WCHAR)(BYTE)text[i];      /* registry key/value names here are plain ASCII in practice */
  return w;
}
static void free_wide_(WCHAR *w) { if (w) RtlFreeHeap(heap(), 0, w); }
EXPORT LSTATUS WINAPI RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD options, REGSAM access, PHKEY result) {
  WCHAR *w = to_wide_(sub);
  const LSTATUS status = (sub && !w) ? ERROR_NOT_ENOUGH_MEMORY : RegOpenKeyExW(key, w, options, access, result);
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegOpenKeyA(HKEY key, LPCSTR sub, PHKEY result) { return RegOpenKeyExA(key, sub, 0, 0, result); }
EXPORT LSTATUS WINAPI RegCreateKeyExA(HKEY key, LPCSTR sub, DWORD reserved, LPSTR class_name, DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES sa, PHKEY result,
                                      LPDWORD disposition) {
  (void)class_name;
  WCHAR *w = to_wide_(sub);
  const LSTATUS status = (sub && !w) ? ERROR_NOT_ENOUGH_MEMORY : RegCreateKeyExW(key, w, reserved, NULL, options, access, sa, result, disposition);
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegCreateKeyA(HKEY key, LPCSTR sub, PHKEY result) { return RegCreateKeyExA(key, sub, 0, NULL, 0, 0, NULL, result, NULL); }
EXPORT LSTATUS WINAPI RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD length) {
  WCHAR *w = to_wide_(name);
  const LSTATUS status = (name && !w) ? ERROR_NOT_ENOUGH_MEMORY : RegSetValueExW(key, w, reserved, type, data, length);
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, BYTE *data, LPDWORD capacity) {
  WCHAR *w = to_wide_(name);
  const LSTATUS status = (name && !w) ? ERROR_NOT_ENOUGH_MEMORY : RegQueryValueExW(key, w, reserved, type, data, capacity);
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegGetValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD capacity) {
  WCHAR *ws = to_wide_(sub), *wn = to_wide_(name);
  const LSTATUS status = ((sub && !ws) || (name && !wn)) ? ERROR_NOT_ENOUGH_MEMORY : RegGetValueW(key, ws, wn, flags, type, data, capacity);
  free_wide_(ws);
  free_wide_(wn);
  return status;
}
EXPORT LSTATUS WINAPI RegDeleteValueA(HKEY key, LPCSTR name) {
  WCHAR *w = to_wide_(name);
  const LSTATUS status = (name && !w) ? ERROR_NOT_ENOUGH_MEMORY : RegDeleteValueW(key, w);
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegDeleteKeyA(HKEY key, LPCSTR sub) {
  WCHAR *w = to_wide_(sub);
  const LSTATUS status = (sub && !w) ? ERROR_NOT_ENOUGH_MEMORY : RegDeleteKeyW(key, w);
  free_wide_(w);
  return status;
}
static void wide_to_ansi_(const WCHAR *w, char *out, DWORD n) { for (DWORD i = 0; i < n; ++i) out[i] = (char)w[i]; out[n] = 0; }
EXPORT LSTATUS WINAPI RegEnumKeyExA(HKEY key, DWORD index, LPSTR name, LPDWORD name_capacity, LPDWORD reserved, LPSTR class_name, LPDWORD class_capacity,
                                    PFILETIME write_time) {
  WCHAR wide[MAX_PATH];
  DWORD wide_capacity = MAX_PATH;
  const LSTATUS status = RegEnumKeyExW(key, index, wide, &wide_capacity, reserved, NULL, class_capacity, write_time);
  (void)class_name;
  if (status != ERROR_SUCCESS) return status;
  if (*name_capacity <= wide_capacity) return ERROR_MORE_DATA;
  wide_to_ansi_(wide, name, wide_capacity);
  *name_capacity = wide_capacity;
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegEnumValueA(HKEY key, DWORD index, LPSTR name, LPDWORD name_capacity, LPDWORD reserved, LPDWORD type, BYTE *data, LPDWORD data_capacity) {
  WCHAR wide[MAX_PATH];
  DWORD wide_capacity = MAX_PATH;
  const LSTATUS status = RegEnumValueW(key, index, wide, &wide_capacity, reserved, type, data, data_capacity);
  if (status != ERROR_SUCCESS) return status;
  if (*name_capacity <= wide_capacity) return ERROR_MORE_DATA;
  wide_to_ansi_(wide, name, wide_capacity);
  *name_capacity = wide_capacity;
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegQueryInfoKeyA(HKEY key, LPSTR class_name, LPDWORD class_capacity, LPDWORD reserved, LPDWORD subkeys, LPDWORD max_subkey_length,
                                       LPDWORD max_class_length, LPDWORD values, LPDWORD max_value_name_length, LPDWORD max_value_data_length, LPDWORD security,
                                       PFILETIME write_time) {
  (void)class_name;
  return RegQueryInfoKeyW(key, NULL, class_capacity, reserved, subkeys, max_subkey_length, max_class_length, values, max_value_name_length, max_value_data_length, security,
                           write_time);
}

/* ---- a little security: enough that a program only checking "am I an administrator" or using crypto randomness works -------------------------------------- */

EXPORT BOOL WINAPI GetUserNameW(LPWSTR buffer, LPDWORD capacity) {
  static const WCHAR name[] = L"User";
  const DWORD n = (DWORD)(sizeof(name) / 2);
  if (!capacity || *capacity < n) { if (capacity) *capacity = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memcpy(buffer, name, n * 2);
  *capacity = n;
  return TRUE;
}
EXPORT BOOL WINAPI GetUserNameA(LPSTR buffer, LPDWORD capacity) {
  if (!capacity || *capacity < 5) { if (capacity) *capacity = 5; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memcpy(buffer, "User", 5);
  *capacity = 5;
  return TRUE;
}
/* OpenProcessToken / GetTokenInformation: enough to answer "what user, and is it elevated" (always: yes). There is one user and no separate security principals. */
#define TOKEN_PSEUDO_HANDLE_ ((HANDLE)(ULONG_PTR)0x4B4F54)      /* "TOK" in ASCII, so it is never mistaken for a real one */
EXPORT BOOL WINAPI OpenProcessToken(HANDLE process, DWORD access, PHANDLE token) {
  (void)process; (void)access;
  if (!token) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *token = TOKEN_PSEUDO_HANDLE_;
  return TRUE;
}
EXPORT BOOL WINAPI OpenThreadToken(HANDLE thread, DWORD access, BOOL self, PHANDLE token) { (void)thread; (void)self; return OpenProcessToken(NULL, access, token); }
typedef struct { DWORD Attributes; } SID_AND_ATTRIBUTES_;
typedef struct { PVOID Sid; } TOKEN_USER_;
EXPORT BOOL WINAPI GetTokenInformation(HANDLE token, int class_, PVOID info, DWORD capacity, LPDWORD returned) {
  if (token != TOKEN_PSEUDO_HANDLE_) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  switch (class_) {
    case 2: {      /* TokenGroups: none beyond the default */
      if (returned) *returned = 4;
      if (capacity < 4) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
      memset(info, 0, 4);
      return TRUE;
    }
    case 18: case 20: {      /* TokenElevationType (1 = default, as if UAC had already allowed it), TokenElevation (1 = elevated: there is nothing this OS refuses a program) */
      if (returned) *returned = 4;
      if (capacity < 4) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
      *(DWORD *)info = 1;
      return TRUE;
    }
    default: SetLastError(ERROR_NOT_SUPPORTED); return FALSE;
  }
}
/* CryptAcquireContext / CryptGenRandom / CryptReleaseContext: a program that only wants non-predictable bytes (not a cryptographic guarantee: there is no entropy
 * driver here yet, matching kernel32's own GetCurrentProcessId()-seeded sources elsewhere) gets them; anything about named key containers or providers is refused. */
#define PROV_RSA_FULL_ 1
EXPORT BOOL WINAPI CryptAcquireContextW(HANDLE *provider, LPCWSTR container, LPCWSTR provider_name, DWORD provider_type, DWORD flags) {
  (void)container; (void)provider_name;
  if (provider_type != 0 && provider_type != PROV_RSA_FULL_) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
  (void)flags;
  if (!provider) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *provider = (HANDLE)(ULONG_PTR)0x435259;      /* "CRY": a pseudo-handle, like the token above */
  return TRUE;
}
EXPORT BOOL WINAPI CryptAcquireContextA(HANDLE *provider, LPCSTR container, LPCSTR provider_name, DWORD provider_type, DWORD flags) {
  (void)container; (void)provider_name;
  return CryptAcquireContextW(provider, NULL, NULL, provider_type, flags);
}
EXPORT BOOL WINAPI CryptReleaseContext(HANDLE provider, DWORD flags) { (void)provider; (void)flags; return TRUE; }
EXPORT BOOL WINAPI CryptGenRandom(HANDLE provider, DWORD length, BYTE *buffer) {
  (void)provider;
  static ULONGLONG state;
  if (!state) state = GetTickCount64() ^ ((ULONGLONG)GetCurrentProcessId() << 32) ^ 0x9E3779B97F4A7C15ull;
  for (DWORD i = 0; i < length; ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    buffer[i] = (BYTE)(state >> 56);
  }
  return TRUE;
}

EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved; (void)reason;
  return TRUE;
}
