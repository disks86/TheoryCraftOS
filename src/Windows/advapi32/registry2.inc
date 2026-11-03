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

/* advapi32.dll: the registry functions that go beyond keys and values - hive files (save, restore, load, unload, replace), change notification, copying and renaming a tree, several
 * values at once, the transacted forms, remote connections. Included by advapi32.c after registry.inc. The hive services are the kernel's (NtSaveKeyEx, NtRestoreKey, NtLoadKey,
 * NtUnloadKey, NtReplaceKey, NtRenameKey, NtNotifyChangeKey). */

IMPORT NTSTATUS NTAPI NtSaveKeyEx(HANDLE, HANDLE, ULONG);
IMPORT NTSTATUS NTAPI NtRestoreKey(HANDLE, HANDLE, ULONG);
IMPORT NTSTATUS NTAPI NtLoadKey(POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES);
IMPORT NTSTATUS NTAPI NtUnloadKey(POBJECT_ATTRIBUTES);
IMPORT NTSTATUS NTAPI NtReplaceKey(POBJECT_ATTRIBUTES, HANDLE, POBJECT_ATTRIBUTES);
IMPORT NTSTATUS NTAPI NtRenameKey(HANDLE, PUNICODE_STRING);
IMPORT NTSTATUS NTAPI NtNotifyChangeKey(HANDLE, HANDLE, PVOID, PVOID, PVOID, ULONG, BOOLEAN, PVOID, ULONG, BOOLEAN);
IMPORT NTSTATUS NTAPI RtlAdjustPrivilege(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);
IMPORT DWORD WINAPI GetFullPathNameW(LPCWSTR path, DWORD capacity, LPWSTR buffer, LPWSTR *file_part);

#ifndef KEY_ALL_ACCESS
#define KEY_ALL_ACCESS 0xF003F
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#endif
#ifndef REG_SZ
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#endif
#ifndef ERROR_BAD_PATHNAME
#define ERROR_BAD_PATHNAME 161
#endif
#ifndef ERROR_BAD_NETPATH
#define ERROR_BAD_NETPATH 53
#endif
IMPORT BOOL WINAPI GetComputerNameW(LPWSTR name, LPDWORD size);
#define REG_STANDARD_FORMAT_ 1
#define REG_NO_COMPRESSION_ 4
#define SE_RESTORE_PRIVILEGE_ 18
#define SE_BACKUP_PRIVILEGE_ 17

/* ---- hive files --------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS WINAPI RegSaveKeyExW(HKEY key, LPCWSTR file, LPSECURITY_ATTRIBUTES sa, DWORD flags) {
  HANDLE h = handle_of(key);
  if (!h) return ERROR_INVALID_HANDLE;
  if (!file) return ERROR_INVALID_PARAMETER;
  const ULONG format = flags == 0 || flags == REG_STANDARD_FORMAT_ ? 1 : (flags == 2 ? 2 : (flags == REG_NO_COMPRESSION_ ? 3 : 0));
  if (!format) return ERROR_INVALID_PARAMETER;
  HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return fail();
  const NTSTATUS status = NtSaveKeyEx(h, f, format);
  CloseHandle(f);
  if (!NT_SUCCESS(status)) { DeleteFileW(file); return nt_error(status); }
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegSaveKeyW(HKEY key, LPCWSTR file, LPSECURITY_ATTRIBUTES sa) { return RegSaveKeyExW(key, file, sa, REG_STANDARD_FORMAT_); }
EXPORT LSTATUS WINAPI RegSaveKeyExA(HKEY key, LPCSTR file, LPSECURITY_ATTRIBUTES sa, DWORD flags) {
  WITH_WIDE(w, file);
  const LSTATUS r = RegSaveKeyExW(key, w, sa, flags);
  free_wide_(w);
  return r;
}
EXPORT LSTATUS WINAPI RegSaveKeyA(HKEY key, LPCSTR file, LPSECURITY_ATTRIBUTES sa) { return RegSaveKeyExA(key, file, sa, REG_STANDARD_FORMAT_); }

EXPORT LSTATUS WINAPI RegRestoreKeyW(HKEY key, LPCWSTR file, DWORD flags) {
  HANDLE h = handle_of(key);
  if (!h) return ERROR_INVALID_HANDLE;
  if (!file) return ERROR_INVALID_PARAMETER;
  HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return fail();
  const NTSTATUS status = NtRestoreKey(h, f, flags);
  CloseHandle(f);
  return NT_SUCCESS(status) ? ERROR_SUCCESS : nt_error(status);
}
EXPORT LSTATUS WINAPI RegRestoreKeyA(HKEY key, LPCSTR file, DWORD flags) {
  WITH_WIDE(w, file);
  const LSTATUS r = RegRestoreKeyW(key, w, flags);
  free_wide_(w);
  return r;
}

/* The NT name of a file: \??\ and the full path. `out` holds MAX_PATH + 8 characters. */
static BOOL nt_file_name(LPCWSTR file, WCHAR *out) {
  WCHAR full[300];
  const DWORD n = GetFullPathNameW(file, 300, full, NULL);
  if (n == 0 || n >= 300) return FALSE;
  memcpy(out, L"\\??\\", 8);
  memcpy(out + 4, full, (n + 1) * 2);
  return TRUE;
}

EXPORT LSTATUS WINAPI RegLoadKeyW(HKEY key, LPCWSTR sub, LPCWSTR file) {
  HANDLE root = handle_of(key);
  if (!root) return ERROR_INVALID_HANDLE;
  if (!sub || !*sub || !file) return ERROR_INVALID_PARAMETER;
  WCHAR source[310];
  if (!nt_file_name(file, source)) return ERROR_INVALID_PARAMETER;
  UNICODE_STRING target_name, source_name;
  wide_set(&target_name, sub);
  wide_set(&source_name, source);
  OBJECT_ATTRIBUTES target = {sizeof(target), root, &target_name, 0x40, NULL, NULL};
  OBJECT_ATTRIBUTES from = {sizeof(from), NULL, &source_name, 0x40, NULL, NULL};
  const NTSTATUS status = NtLoadKey(&target, &from);
  return NT_SUCCESS(status) ? ERROR_SUCCESS : (status == 0xC0000035u ? ERROR_ALREADY_EXISTS : nt_error(status));
}
EXPORT LSTATUS WINAPI RegLoadKeyA(HKEY key, LPCSTR sub, LPCSTR file) {
  WCHAR *ws = to_wide_(sub), *wf = to_wide_(file);
  LSTATUS r = (sub && !ws) || (file && !wf) ? ERROR_NOT_ENOUGH_MEMORY : RegLoadKeyW(key, ws, wf);
  free_wide_(ws);
  free_wide_(wf);
  return r;
}
EXPORT LSTATUS WINAPI RegUnLoadKeyW(HKEY key, LPCWSTR sub) {
  HANDLE root = handle_of(key);
  if (!root) return ERROR_INVALID_HANDLE;
  if (!sub || !*sub) return ERROR_INVALID_PARAMETER;
  UNICODE_STRING name;
  wide_set(&name, sub);
  OBJECT_ATTRIBUTES target = {sizeof(target), root, &name, 0x40, NULL, NULL};
  const NTSTATUS status = NtUnloadKey(&target);
  return NT_SUCCESS(status) ? ERROR_SUCCESS : nt_error(status);
}
EXPORT LSTATUS WINAPI RegUnLoadKeyA(HKEY key, LPCSTR sub) {
  WITH_WIDE(w, sub);
  const LSTATUS r = RegUnLoadKeyW(key, w);
  free_wide_(w);
  return r;
}

EXPORT LSTATUS WINAPI RegReplaceKeyW(HKEY key, LPCWSTR sub, LPCWSTR new_file, LPCWSTR old_file) {
  HKEY target = key;
  LSTATUS result;
  if (!new_file || !old_file) return ERROR_INVALID_PARAMETER;
  if (sub && *sub && (result = RegOpenKeyExW(key, sub, 0, KEY_ALL_ACCESS, &target)) != ERROR_SUCCESS) return result;
  HANDLE h = handle_of(target);
  WCHAR new_nt[310], old_nt[310];
  if (!h || !nt_file_name(new_file, new_nt) || !nt_file_name(old_file, old_nt)) result = ERROR_INVALID_PARAMETER;
  else {
    UNICODE_STRING a, b;
    wide_set(&a, new_nt);
    wide_set(&b, old_nt);
    OBJECT_ATTRIBUTES new_attributes = {sizeof(new_attributes), NULL, &a, 0x40, NULL, NULL};
    OBJECT_ATTRIBUTES old_attributes = {sizeof(old_attributes), NULL, &b, 0x40, NULL, NULL};
    const NTSTATUS status = NtReplaceKey(&new_attributes, h, &old_attributes);
    result = NT_SUCCESS(status) ? ERROR_SUCCESS : nt_error(status);
  }
  if (target != key) RegCloseKey(target);
  return result;
}
EXPORT LSTATUS WINAPI RegReplaceKeyA(HKEY key, LPCSTR sub, LPCSTR new_file, LPCSTR old_file) {
  WCHAR *ws = to_wide_(sub), *wn = to_wide_(new_file), *wo = to_wide_(old_file);
  LSTATUS r = ((sub && !ws) || (new_file && !wn) || (old_file && !wo)) ? ERROR_NOT_ENOUGH_MEMORY : RegReplaceKeyW(key, ws, wn, wo);
  free_wide_(ws); free_wide_(wn); free_wide_(wo);
  return r;
}

/* An application hive: loaded under \Registry\User with a name of its own, which only this process knows, and opened. */
static LONG g_app_hive_counter;
EXPORT LSTATUS WINAPI RegLoadAppKeyW(LPCWSTR file, PHKEY result, REGSAM access, DWORD options, DWORD reserved) {
  (void)options;
  if (!file || !result || reserved) return ERROR_INVALID_PARAMETER;
  WCHAR name[40] = L"_App_";
  DWORD value = GetCurrentProcessId() * 1000 + (DWORD)(++g_app_hive_counter), digits[12], n = 0, at = 5;
  do { digits[n++] = value % 10; value /= 10; } while (value);
  while (n) name[at++] = (WCHAR)('0' + digits[--n]);
  name[at] = 0;
  BOOLEAN previous = FALSE;
  RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE_, TRUE, FALSE, &previous);      /* (an application hive needs no privilege of its caller) */
  LSTATUS r = RegLoadKeyW(HKEY_USERS_, name, file);
  if (!previous) RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE_, FALSE, FALSE, &previous);
  if (r != ERROR_SUCCESS) return r;
  return RegOpenKeyExW(HKEY_USERS_, name, 0, access, result);
}
EXPORT LSTATUS WINAPI RegLoadAppKeyA(LPCSTR file, PHKEY result, REGSAM access, DWORD options, DWORD reserved) {
  WITH_WIDE(w, file);
  const LSTATUS r = RegLoadAppKeyW(w, result, access, options, reserved);
  free_wide_(w);
  return r;
}

/* ---- renaming, copying --------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS WINAPI RegRenameKey(HKEY key, LPCWSTR sub, LPCWSTR new_name) {
  HKEY target = key;
  LSTATUS result;
  if (!new_name || !*new_name) return ERROR_INVALID_PARAMETER;
  if (sub && *sub && (result = RegOpenKeyExW(key, sub, 0, KEY_WRITE, &target)) != ERROR_SUCCESS) return result;
  HANDLE h = handle_of(target);
  if (!h || is_predefined(target)) result = ERROR_INVALID_HANDLE;
  else {
    UNICODE_STRING name;
    wide_set(&name, new_name);
    const NTSTATUS status = NtRenameKey(h, &name);
    result = NT_SUCCESS(status) ? ERROR_SUCCESS : (status == 0xC0000035u ? ERROR_ALREADY_EXISTS : nt_error(status));
  }
  if (target != key) RegCloseKey(target);
  return result;
}

static LSTATUS copy_tree_keys(HKEY from, HKEY to, DWORD depth) {
  if (depth > 128) return ERROR_BAD_PATHNAME;
  DWORD names, values, longest_name, longest_value_name, longest_data;
  LSTATUS r = RegQueryInfoKeyW(from, NULL, NULL, NULL, &names, &longest_name, NULL, &values, &longest_value_name, &longest_data, NULL, NULL);
  if (r != ERROR_SUCCESS) return r;
  WCHAR *name = RtlAllocateHeap(heap(), 0, (SIZE_T)(longest_name > longest_value_name ? longest_name : longest_value_name) * 2 + 4);
  BYTE *data = RtlAllocateHeap(heap(), 0, (SIZE_T)longest_data + 8);
  if (!name || !data) { if (name) RtlFreeHeap(heap(), 0, name); if (data) RtlFreeHeap(heap(), 0, data); return ERROR_NOT_ENOUGH_MEMORY; }
  for (DWORD i = 0; i < values && r == ERROR_SUCCESS; ++i) {
    DWORD name_size = longest_value_name + 1, data_size = longest_data, type = 0;
    r = RegEnumValueW(from, i, name, &name_size, NULL, &type, data, &data_size);
    if (r == ERROR_SUCCESS) r = RegSetValueExW(to, name, 0, type, data, data_size);
  }
  for (DWORD i = 0; i < names && r == ERROR_SUCCESS; ++i) {
    DWORD name_size = longest_name + 1;
    r = RegEnumKeyExW(from, i, name, &name_size, NULL, NULL, NULL, NULL);
    if (r != ERROR_SUCCESS) break;
    HKEY child_from = NULL, child_to = NULL;
    r = RegOpenKeyExW(from, name, 0, KEY_READ, &child_from);
    if (r == ERROR_SUCCESS) r = RegCreateKeyExW(to, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &child_to, NULL);
    if (r == ERROR_SUCCESS) r = copy_tree_keys(child_from, child_to, depth + 1);
    if (child_from) RegCloseKey(child_from);
    if (child_to) RegCloseKey(child_to);
  }
  RtlFreeHeap(heap(), 0, name);
  RtlFreeHeap(heap(), 0, data);
  return r;
}
EXPORT LSTATUS WINAPI RegCopyTreeW(HKEY source, LPCWSTR sub, HKEY dest) {
  HKEY from = source;
  LSTATUS r;
  if (!handle_of(dest)) return ERROR_INVALID_HANDLE;
  if (sub && *sub && (r = RegOpenKeyExW(source, sub, 0, KEY_READ, &from)) != ERROR_SUCCESS) return r;
  r = handle_of(from) ? copy_tree_keys(from, dest, 0) : ERROR_INVALID_HANDLE;
  if (from != source) RegCloseKey(from);
  return r;
}
EXPORT LSTATUS WINAPI RegCopyTreeA(HKEY source, LPCSTR sub, HKEY dest) {
  WITH_WIDE(w, sub);
  const LSTATUS r = RegCopyTreeW(source, w, dest);
  free_wide_(w);
  return r;
}

/* ---- several values ------------------------------------------------------------------------------------------------------------------------------------------ */

typedef struct { LPWSTR ve_valuename; DWORD ve_valuelen; DWORD_PTR ve_valueptr; DWORD ve_type; } VALENTW_;
typedef struct { LPSTR ve_valuename; DWORD ve_valuelen; DWORD_PTR ve_valueptr; DWORD ve_type; } VALENTA_;

EXPORT LSTATUS WINAPI RegQueryMultipleValuesW(HKEY key, VALENTW_ *list, DWORD count, LPWSTR buffer, LPDWORD total) {
  if (!list || count == 0) return ERROR_INVALID_PARAMETER;
  DWORD needed = 0, capacity = total ? *total : 0;
  LSTATUS result = ERROR_SUCCESS;
  for (DWORD i = 0; i < count; ++i) {      /* every value is looked at, so that the sizes are all known; they go in the buffer only if all fit */
    DWORD size = 0, type = 0;
    LSTATUS r = RegQueryValueExW(key, list[i].ve_valuename, NULL, &type, NULL, &size);
    if (r != ERROR_SUCCESS) return r;
    list[i].ve_valuelen = size;
    list[i].ve_type = type;
    needed = (needed + 7) & ~7u;
    list[i].ve_valueptr = needed;
    needed += size;
  }
  if (total) *total = needed;
  if (needed > capacity || (needed && !buffer)) return ERROR_MORE_DATA;
  for (DWORD i = 0; i < count; ++i) {
    DWORD size = list[i].ve_valuelen;
    BYTE *at = (BYTE *)buffer + list[i].ve_valueptr;
    result = RegQueryValueExW(key, list[i].ve_valuename, NULL, NULL, at, &size);
    if (result != ERROR_SUCCESS) return result;
    list[i].ve_valuelen = size;
    list[i].ve_valueptr = (DWORD_PTR)at;
  }
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegQueryMultipleValuesA(HKEY key, VALENTA_ *list, DWORD count, LPSTR buffer, LPDWORD total) {
  if (!list || count == 0) return ERROR_INVALID_PARAMETER;
  DWORD needed = 0, capacity = total ? *total : 0;
  for (DWORD i = 0; i < count; ++i) {
    DWORD size = 0, type = 0;
    LSTATUS r = RegQueryValueExA(key, list[i].ve_valuename, NULL, &type, NULL, &size);
    if (r != ERROR_SUCCESS) return r;
    list[i].ve_valuelen = size;
    list[i].ve_type = type;
    needed = (needed + 7) & ~7u;
    list[i].ve_valueptr = needed;
    needed += size;
  }
  if (total) *total = needed;
  if (needed > capacity || (needed && !buffer)) return ERROR_MORE_DATA;
  for (DWORD i = 0; i < count; ++i) {
    DWORD size = list[i].ve_valuelen;
    BYTE *at = (BYTE *)buffer + list[i].ve_valueptr;
    LSTATUS r = RegQueryValueExA(key, list[i].ve_valuename, NULL, NULL, at, &size);
    if (r != ERROR_SUCCESS) return r;
    list[i].ve_valuelen = size;
    list[i].ve_valueptr = (DWORD_PTR)at;
  }
  return ERROR_SUCCESS;
}

/* RegLoadMUIString: a value that is plain text is answered as it is; "@file,-id" names a string resource of a module, which kernel32's LoadStringW reads. */
IMPORT HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);
IMPORT BOOL WINAPI FreeLibrary(HMODULE module);
IMPORT HRSRC WINAPI FindResourceExW(HMODULE module, LPCWSTR type, LPCWSTR name, WORD language);
IMPORT HGLOBAL WINAPI LoadResource(HMODULE module, HRSRC resource);
IMPORT DWORD WINAPI SizeofResource(HMODULE module, HRSRC resource);
/* A string of a module's string table: the table is blocks of 16 strings (a length in characters, then the characters), block (id / 16) + 1. */
static int load_string_(HMODULE module, UINT id, WCHAR *out, int capacity) {
  HRSRC r = FindResourceExW(module, (LPCWSTR)(ULONG_PTR)6, (LPCWSTR)(ULONG_PTR)(id / 16 + 1), 0);
  if (!r) return 0;
  const WCHAR *p = (const WCHAR *)LoadResource(module, r);
  const DWORD total = SizeofResource(module, r) / 2;
  DWORD at = 0;
  if (!p) return 0;
  for (UINT i = 0; i < 16; ++i) {
    if (at >= total) return 0;
    const DWORD n = p[at++];
    if (n > total - at) return 0;
    if (i == id % 16) {
      if ((int)n > capacity - 1) return 0;
      memcpy(out, p + at, n * 2);
      out[n] = 0;
      return (int)n;
    }
    at += n;
  }
  return 0;
}
EXPORT LSTATUS WINAPI RegLoadMUIStringW(HKEY key, LPCWSTR value, LPWSTR out, DWORD out_bytes, LPDWORD needed, DWORD flags, LPCWSTR directory) {
  (void)flags; (void)directory;
  DWORD type = 0, size = 0;
  LSTATUS r = RegQueryValueExW(key, value, NULL, &type, NULL, &size);
  if (r != ERROR_SUCCESS) return r;
  if (type != REG_SZ && type != REG_EXPAND_SZ) return ERROR_INVALID_DATA;
  WCHAR *text = RtlAllocateHeap(heap(), 0, (SIZE_T)size + 4);
  if (!text) return ERROR_NOT_ENOUGH_MEMORY;
  DWORD got = size;
  r = RegQueryValueExW(key, value, NULL, NULL, (BYTE *)text, &got);
  if (r == ERROR_SUCCESS) {
    text[got / 2] = 0;
    DWORD id = 0;
    WCHAR *comma = NULL;
    if (text[0] == '@') for (WCHAR *c = text; *c; ++c) if (*c == ',') comma = c;
    if (comma && comma[1] == '-') {
      for (WCHAR *c = comma + 2; *c >= '0' && *c <= '9'; ++c) id = id * 10 + (DWORD)(*c - '0');
      *comma = 0;
      HMODULE m = LoadLibraryExW(text + 1, NULL, 0x2 /* LOAD_LIBRARY_AS_DATAFILE */);
      WCHAR loaded[512];
      int n = m ? load_string_(m, id, loaded, 512) : 0;
      if (m) FreeLibrary(m);
      if (n <= 0) r = 1814;
      else {
        loaded[n] = 0;
        const DWORD bytes = ((DWORD)n + 1) * 2;
        if (needed) *needed = bytes;
        if (!out || out_bytes < bytes) r = ERROR_MORE_DATA; else memcpy(out, loaded, bytes);
      }
    } else {
      const DWORD bytes = (DWORD)(wcslen(text) + 1) * 2;
      if (needed) *needed = bytes;
      if (!out || out_bytes < bytes) r = ERROR_MORE_DATA; else memcpy(out, text, bytes);
    }
  }
  RtlFreeHeap(heap(), 0, text);
  return r;
}
EXPORT LSTATUS WINAPI RegLoadMUIStringA(HKEY key, LPCSTR value, LPSTR out, DWORD out_bytes, LPDWORD needed, DWORD flags, LPCSTR directory) {
  (void)directory;
  WITH_WIDE(w, value);
  WCHAR wide[512];
  DWORD wide_needed = 0;
  LSTATUS r = RegLoadMUIStringW(key, w, wide, sizeof(wide), &wide_needed, flags, NULL);
  free_wide_(w);
  if (r != ERROR_SUCCESS) return r;
  const DWORD bytes = wide_to_utf8_(wide, wide_needed / 2 - 1, out, out_bytes ? out_bytes - 1 : 0) + 1;
  if (needed) *needed = bytes;
  if (!out || out_bytes < bytes) return ERROR_MORE_DATA;
  return ERROR_SUCCESS;
}

/* ---- change notification ------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS WINAPI RegNotifyChangeKeyValue(HKEY key, BOOL subtree, DWORD filter, HANDLE event, BOOL asynchronous) {
  HANDLE h = handle_of(key);
  ULONG_PTR status_block[2] = {0, 0};
  if (!h) return ERROR_INVALID_HANDLE;
  filter &= ~0x10000000u;      /* REG_NOTIFY_THREAD_AGNOSTIC: every wait here belongs to the process */
  if (filter == 0 || (filter & ~0xFu)) return ERROR_INVALID_PARAMETER;
  if (asynchronous && !event) return ERROR_INVALID_PARAMETER;
  const NTSTATUS status = NtNotifyChangeKey(h, event, NULL, NULL, status_block, filter, subtree ? TRUE : FALSE, NULL, 0, asynchronous ? TRUE : FALSE);
  return NT_SUCCESS(status) ? ERROR_SUCCESS : nt_error(status);      /* (STATUS_PENDING is a success code: the event says when) */
}

/* ---- transacted forms, remote registries, reflection --------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS WINAPI RegCreateKeyTransactedW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR class_name, DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES sa, PHKEY result,
                                              LPDWORD disposition, HANDLE transaction, PVOID extended) {
  (void)extended;
  if (!transaction) return ERROR_INVALID_PARAMETER;
  return RegCreateKeyExW(key, sub, reserved, class_name, options, access, sa, result, disposition);
}
EXPORT LSTATUS WINAPI RegCreateKeyTransactedA(HKEY key, LPCSTR sub, DWORD reserved, LPSTR class_name, DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES sa, PHKEY result,
                                              LPDWORD disposition, HANDLE transaction, PVOID extended) {
  (void)extended;
  if (!transaction) return ERROR_INVALID_PARAMETER;
  return RegCreateKeyExA(key, sub, reserved, class_name, options, access, sa, result, disposition);
}
EXPORT LSTATUS WINAPI RegOpenKeyTransactedW(HKEY key, LPCWSTR sub, DWORD options, REGSAM access, PHKEY result, HANDLE transaction, PVOID extended) {
  (void)extended;
  if (!transaction) return ERROR_INVALID_PARAMETER;
  return RegOpenKeyExW(key, sub, options, access, result);
}
EXPORT LSTATUS WINAPI RegOpenKeyTransactedA(HKEY key, LPCSTR sub, DWORD options, REGSAM access, PHKEY result, HANDLE transaction, PVOID extended) {
  (void)extended;
  if (!transaction) return ERROR_INVALID_PARAMETER;
  return RegOpenKeyExA(key, sub, options, access, result);
}
EXPORT LSTATUS WINAPI RegDeleteKeyTransactedW(HKEY key, LPCWSTR sub, REGSAM access, DWORD reserved, HANDLE transaction, PVOID extended) {
  (void)extended;
  if (!transaction) return ERROR_INVALID_PARAMETER;
  return RegDeleteKeyExW(key, sub, access, reserved);
}
EXPORT LSTATUS WINAPI RegDeleteKeyTransactedA(HKEY key, LPCSTR sub, REGSAM access, DWORD reserved, HANDLE transaction, PVOID extended) {
  (void)extended;
  if (!transaction) return ERROR_INVALID_PARAMETER;
  return RegDeleteKeyExA(key, sub, access, reserved);
}

/* A machine name that is this one (or none) is answered with the predefined key; any other machine is out of reach (there is no network registry service). */
static BOOL is_local_machine(LPCWSTR machine) {
  if (!machine || !*machine) return TRUE;
  WCHAR name[64];
  DWORD n = 64;
  if (machine[0] == '\\' && machine[1] == '\\') machine += 2;
  if (!GetComputerNameW(name, &n)) return FALSE;
  return wequal_ignore_case(machine, name) || wequal_ignore_case(machine, L"localhost");
}
EXPORT LSTATUS WINAPI RegConnectRegistryW(LPCWSTR machine, HKEY key, PHKEY result) {
  if (!result) return ERROR_INVALID_PARAMETER;
  if (key != HKEY_LOCAL_MACHINE_ && key != HKEY_USERS_ && key != HKEY_PERFORMANCE_DATA_) return ERROR_INVALID_HANDLE;
  if (!is_local_machine(machine)) return ERROR_BAD_NETPATH;
  *result = key;
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegConnectRegistryExW(LPCWSTR machine, HKEY key, ULONG flags, PHKEY result) { (void)flags; return RegConnectRegistryW(machine, key, result); }
EXPORT LSTATUS WINAPI RegConnectRegistryA(LPCSTR machine, HKEY key, PHKEY result) {
  WCHAR *w = to_wide_(machine);
  if (machine && !w) return ERROR_NOT_ENOUGH_MEMORY;
  const LSTATUS r = RegConnectRegistryW(w, key, result);
  free_wide_(w);
  return r;
}
EXPORT LSTATUS WINAPI RegConnectRegistryExA(LPCSTR machine, HKEY key, ULONG flags, PHKEY result) { (void)flags; return RegConnectRegistryA(machine, key, result); }

/* The 32-bit view of the registry is the same registry here: reflection is never on. */
EXPORT LSTATUS WINAPI RegDisableReflectionKey(HKEY key) { return handle_of(key) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE; }
EXPORT LSTATUS WINAPI RegEnableReflectionKey(HKEY key) { return handle_of(key) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE; }
EXPORT LSTATUS WINAPI RegQueryReflectionKey(HKEY key, BOOL *disabled) {
  if (!handle_of(key)) return ERROR_INVALID_HANDLE;
  if (!disabled) return ERROR_INVALID_PARAMETER;
  *disabled = FALSE;
  return ERROR_SUCCESS;
}
