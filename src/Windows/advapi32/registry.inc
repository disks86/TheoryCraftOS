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

/* advapi32.dll: the registry functions, over the NT registry services (NtCreateKey, NtOpenKey, NtQueryValueKey, ...). Included by advapi32.c. A key is a kernel handle; the
 * predefined keys (HKEY_LOCAL_MACHINE and so on) are the fixed values of Windows and are opened under \Registry the first time they are used. */

IMPORT NTSTATUS NTAPI NtCreateKey(PHANDLE, ULONG, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
IMPORT NTSTATUS NTAPI NtOpenKey(PHANDLE, ULONG, POBJECT_ATTRIBUTES);
IMPORT NTSTATUS NTAPI NtQueryKey(HANDLE, ULONG, PVOID, ULONG, PULONG);
IMPORT NTSTATUS NTAPI NtEnumerateKey(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
IMPORT NTSTATUS NTAPI NtEnumerateValueKey(HANDLE, ULONG, ULONG, PVOID, ULONG, PULONG);
IMPORT NTSTATUS NTAPI NtQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);
IMPORT NTSTATUS NTAPI NtSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
IMPORT NTSTATUS NTAPI NtDeleteKey(HANDLE);
IMPORT NTSTATUS NTAPI NtDeleteValueKey(HANDLE, PUNICODE_STRING);
IMPORT NTSTATUS NTAPI NtFlushKey(HANDLE);
IMPORT ULONG NTAPI RtlNtStatusToDosError(NTSTATUS);

#define HKEY_CLASSES_ROOT_ ((HKEY)(ULONG_PTR)(LONG)0x80000000)
#define HKEY_CURRENT_USER_ ((HKEY)(ULONG_PTR)(LONG)0x80000001)
#define HKEY_LOCAL_MACHINE_ ((HKEY)(ULONG_PTR)(LONG)0x80000002)
#define HKEY_USERS_ ((HKEY)(ULONG_PTR)(LONG)0x80000003)
#define HKEY_PERFORMANCE_DATA_ ((HKEY)(ULONG_PTR)(LONG)0x80000004)
#define HKEY_CURRENT_CONFIG_ ((HKEY)(ULONG_PTR)(LONG)0x80000005)
#define HKEY_DYN_DATA_ ((HKEY)(ULONG_PTR)(LONG)0x80000006)
#define KEY_WOW64_MASK_ 0x300

static LSTATUS nt_error(NTSTATUS status) { return (LSTATUS)RtlNtStatusToDosError(status); }
static BOOL is_predefined(HKEY key) { return ((ULONG_PTR)key & 0xFFFFFFFFFFFFFFF0ull) == 0xFFFFFFFF80000000ull; }

static void wide_set(UNICODE_STRING *u, const WCHAR *text) {
  size_t_ n = 0;
  while (text[n]) ++n;
  u->Buffer = (PWSTR)text;
  u->Length = (USHORT)(n * 2);
  u->MaximumLength = (USHORT)(n * 2 + 2);
}

/* The NT path each predefined key stands for. */
static const WCHAR *predefined_nt_path(HKEY key) {
  if (key == HKEY_LOCAL_MACHINE_) return L"\\Registry\\Machine";
  if (key == HKEY_USERS_) return L"\\Registry\\User";
  if (key == HKEY_CURRENT_USER_) return L"\\Registry\\User\\S-1-5-21-0-0-0-1000";
  if (key == HKEY_CLASSES_ROOT_) return L"\\Registry\\Machine\\Software\\Classes";
  if (key == HKEY_CURRENT_CONFIG_) return L"\\Registry\\Machine\\System\\CurrentControlSet\\Hardware Profiles\\Current";
  return NULL;
}
static HANDLE g_predefined[8];
static HANDLE open_predefined(HKEY key) {
  const WCHAR *path = predefined_nt_path(key);
  if (!path) return NULL;
  const int slot = (int)((ULONG_PTR)key & 7);
  if (!g_predefined[slot]) {
    UNICODE_STRING name;
    wide_set(&name, path);
    OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, &name, 0x40 /* OBJ_CASE_INSENSITIVE */, NULL, NULL};
    HANDLE h = NULL;
    ULONG disposition = 0;
    if (!NT_SUCCESS(NtCreateKey(&h, 0xF003F, &attributes, 0, NULL, 0, &disposition))) return NULL;
    g_predefined[slot] = h;
  }
  return g_predefined[slot];
}
/* The kernel handle behind an HKEY (NULL if it is not a key). */
static HANDLE handle_of(HKEY key) {
  if (is_predefined(key)) return open_predefined(key);
  return (HANDLE)key;
}

static LSTATUS open_or_create(HKEY key, LPCWSTR sub, BOOL create, DWORD options, PHKEY result, LPDWORD disposition) {
  if (!result) return ERROR_INVALID_PARAMETER;
  HANDLE root = handle_of(key);
  if (!root) return ERROR_INVALID_HANDLE;
  if (!sub || !*sub) {      /* the same key again: a predefined key is itself, any other gets a handle of its own */
    if (is_predefined(key)) { *result = key; if (disposition) *disposition = 2; return ERROR_SUCCESS; }
  }
  UNICODE_STRING name;
  wide_set(&name, sub ? sub : L"");
  if (sub && sub[0] == '\\') return ERROR_PATH_NOT_FOUND;      /* (an absolute name is not a subkey) */
  OBJECT_ATTRIBUTES attributes = {sizeof(attributes), root, &name, 0x40, NULL, NULL};
  HANDLE h = NULL;
  ULONG made = 0;
  NTSTATUS status;
  if (create) status = NtCreateKey(&h, 0xF003F, &attributes, 0, NULL, options, &made);
  else status = NtOpenKey(&h, 0x20019, &attributes);
  if (!NT_SUCCESS(status)) return status == 0xC0000034u ? ERROR_FILE_NOT_FOUND : nt_error(status);
  *result = (HKEY)h;
  if (disposition) *disposition = made;
  return ERROR_SUCCESS;
}

EXPORT LSTATUS WINAPI RegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD options, REGSAM access, PHKEY result) { (void)options; (void)access; return open_or_create(key, sub, FALSE, 0, result, NULL); }
EXPORT LSTATUS WINAPI RegOpenKeyW(HKEY key, LPCWSTR sub, PHKEY result) { return RegOpenKeyExW(key, sub, 0, 0, result); }
EXPORT LSTATUS WINAPI RegCreateKeyExW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR class_name, DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES sa, PHKEY result,
                                      LPDWORD disposition) {
  (void)reserved; (void)class_name; (void)access; (void)sa;
  if (!sub) return ERROR_INVALID_PARAMETER;
  return open_or_create(key, sub, TRUE, options, result, disposition);
}
EXPORT LSTATUS WINAPI RegCreateKeyW(HKEY key, LPCWSTR sub, PHKEY result) { return RegCreateKeyExW(key, sub, 0, NULL, 0, 0, NULL, result, NULL); }
EXPORT LSTATUS WINAPI RegCloseKey(HKEY key) {
  if (!key) return ERROR_INVALID_HANDLE;
  if (is_predefined(key)) return ERROR_SUCCESS;
  const NTSTATUS status = NtClose((HANDLE)key);
  return NT_SUCCESS(status) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}
EXPORT LSTATUS WINAPI RegFlushKey(HKEY key) { HANDLE h = handle_of(key); return h ? nt_error(NtFlushKey(h)) : ERROR_INVALID_HANDLE; }
EXPORT LSTATUS WINAPI RegOpenCurrentUser(REGSAM access, PHKEY result) { (void)access; if (!result) return ERROR_INVALID_PARAMETER; return RegOpenKeyExW(HKEY_CURRENT_USER_, NULL, 0, 0, result); }
EXPORT LSTATUS WINAPI RegOpenUserClassesRoot(HANDLE token, DWORD options, REGSAM access, PHKEY result) { (void)token; (void)options; (void)access; return RegOpenKeyExW(HKEY_CLASSES_ROOT_, NULL, 0, 0, result); }
EXPORT LSTATUS WINAPI RegOverridePredefKey(HKEY key, HKEY replacement) { (void)key; (void)replacement; return ERROR_SUCCESS; }
EXPORT LSTATUS WINAPI RegDisablePredefinedCache(void) { return ERROR_SUCCESS; }
EXPORT LSTATUS WINAPI RegDisablePredefinedCacheEx(void) { return ERROR_SUCCESS; }

/* ---- values ------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS WINAPI RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD length) {
  (void)reserved;
  HANDLE h = handle_of(key);
  if (!h) return ERROR_INVALID_HANDLE;
  if (!data && length) return ERROR_NOACCESS;
  if ((type == 1 || type == 2 || type == 7) && length & 1) { /* (odd lengths for text are stored as given) */ }
  UNICODE_STRING value;
  wide_set(&value, name ? name : L"");
  return nt_error(NtSetValueKey(h, &value, 0, type, (PVOID)data, length));
}

/* A value is read in KeyValuePartialInformation form: 12 bytes (TitleIndex, Type, DataLength) and the data. */
static LSTATUS query_value(HANDLE h, LPCWSTR name, LPDWORD type, BYTE *data, LPDWORD capacity) {
  UNICODE_STRING value;
  wide_set(&value, name ? name : L"");
  BYTE head[12];
  ULONG needed = 0;
  NTSTATUS status = NtQueryValueKey(h, &value, 2, head, 12, &needed);      /* the header says how long the data is */
  if (status == 0xC0000034u) return ERROR_FILE_NOT_FOUND;
  if (!NT_SUCCESS(status) && status != 0x80000005u) return nt_error(status);
  const DWORD size = *(DWORD *)(head + 8);
  if (type) *type = *(DWORD *)(head + 4);
  if (!data) { if (capacity) *capacity = size; return ERROR_SUCCESS; }
  const DWORD room = *capacity;
  *capacity = size;
  if (size > room) return ERROR_MORE_DATA;
  if (size == 0) return ERROR_SUCCESS;
  BYTE *buffer = RtlAllocateHeap(heap(), 0, 12 + (SIZE_T)size);
  if (!buffer) { *capacity = room; return ERROR_NOT_ENOUGH_MEMORY; }
  status = NtQueryValueKey(h, &value, 2, buffer, 12 + size, &needed);
  LSTATUS result = ERROR_SUCCESS;
  if (!NT_SUCCESS(status)) result = nt_error(status);
  else memcpy(data, buffer + 12, size);
  RtlFreeHeap(heap(), 0, buffer);
  return result;
}
EXPORT LSTATUS WINAPI RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, BYTE *data, LPDWORD capacity) {
  (void)reserved;
  HANDLE h = handle_of(key);
  if (!h) return ERROR_INVALID_HANDLE;
  if (data && !capacity) return ERROR_INVALID_PARAMETER;
  return query_value(h, name, type, data, capacity);
}
static LSTATUS rrf_arguments(DWORD flags, PVOID data, LPDWORD capacity) {
  if (data && !capacity) return ERROR_INVALID_PARAMETER;
  if ((flags & 0x10000) && (flags & 0x20000)) return ERROR_INVALID_PARAMETER;      /* RRF_SUBKEY_WOW6464KEY and RRF_SUBKEY_WOW6432KEY */
  return ERROR_SUCCESS;
}
static BOOL rrf_type_accepted(DWORD flags, DWORD found_type) {
  if (!(flags & 0xFFFF)) return TRUE;
  if (found_type <= 4) return (flags & (0x1u << found_type)) != 0;      /* RRF_RT_REG_NONE, SZ, EXPAND_SZ, BINARY, DWORD */
  if (found_type == 7) return (flags & 0x20) != 0;
  if (found_type == 11) return (flags & 0x40) != 0;
  return FALSE;
}
EXPORT LSTATUS WINAPI RegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD capacity) {
  { const LSTATUS bad = rrf_arguments(flags, data, capacity); if (bad != ERROR_SUCCESS) return bad; }
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
  if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) return status;
  if (!rrf_type_accepted(flags, found_type)) return ERROR_UNSUPPORTED_TYPE;
  if (type) *type = found_type;
  return status;
}
EXPORT LSTATUS WINAPI RegDeleteValueW(HKEY key, LPCWSTR name) {
  HANDLE h = handle_of(key);
  if (!h) return ERROR_INVALID_HANDLE;
  UNICODE_STRING value;
  wide_set(&value, name ? name : L"");
  const NTSTATUS status = NtDeleteValueKey(h, &value);
  return status == 0xC0000034u ? ERROR_FILE_NOT_FOUND : nt_error(status);
}
EXPORT LSTATUS WINAPI RegDeleteKeyW(HKEY key, LPCWSTR sub) {
  if (!sub) return ERROR_INVALID_PARAMETER;
  HKEY opened = NULL;
  LSTATUS status = open_or_create(key, sub, FALSE, 0, &opened, NULL);
  if (status != ERROR_SUCCESS) return status;
  status = nt_error(NtDeleteKey((HANDLE)opened));
  RegCloseKey(opened);
  return status;
}
EXPORT LSTATUS WINAPI RegDeleteKeyExW(HKEY key, LPCWSTR sub, REGSAM access, DWORD reserved) { (void)access; (void)reserved; return RegDeleteKeyW(key, sub); }
EXPORT LSTATUS WINAPI RegDeleteKeyValueW(HKEY key, LPCWSTR sub, LPCWSTR name) {
  HKEY opened = key;
  if (sub && *sub) { const LSTATUS status = RegOpenKeyExW(key, sub, 0, 0, &opened); if (status != ERROR_SUCCESS) return status; }
  const LSTATUS result = RegDeleteValueW(opened, name);
  if (opened != key) RegCloseKey(opened);
  return result;
}
EXPORT LSTATUS WINAPI RegSetKeyValueW(HKEY key, LPCWSTR sub, LPCWSTR name, DWORD type, LPCVOID data, DWORD length) {
  HKEY opened = key;
  if (sub && *sub) { const LSTATUS status = RegCreateKeyExW(key, sub, 0, NULL, 0, 0, NULL, &opened, NULL); if (status != ERROR_SUCCESS) return status; }
  const LSTATUS result = RegSetValueExW(opened, name, 0, type, data, length);
  if (opened != key) RegCloseKey(opened);
  return result;
}
/* RegSetValue / RegQueryValue: the unnamed value of a subkey, as text. */
EXPORT LSTATUS WINAPI RegSetValueW(HKEY key, LPCWSTR sub, DWORD type, LPCWSTR data, DWORD length) {
  if (type != 1 /* REG_SZ */ || !data) return ERROR_INVALID_PARAMETER;
  HKEY opened = key;
  if (sub && *sub) { const LSTATUS status = RegCreateKeyExW(key, sub, 0, NULL, 0, 0, NULL, &opened, NULL); if (status != ERROR_SUCCESS) return status; }
  size_t_ n = 0;
  while (data[n]) ++n;
  (void)length;
  const LSTATUS result = RegSetValueExW(opened, NULL, 0, 1, (const BYTE *)data, (DWORD)(n * 2 + 2));
  if (opened != key) RegCloseKey(opened);
  return result;
}
EXPORT LSTATUS WINAPI RegQueryValueW(HKEY key, LPCWSTR sub, LPWSTR data, PLONG length) {
  HKEY opened = key;
  if (sub && *sub) { const LSTATUS status = RegOpenKeyExW(key, sub, 0, 0, &opened); if (status != ERROR_SUCCESS) return status; }
  DWORD type = 0, size = length ? (DWORD)*length : 0;
  LSTATUS result = RegQueryValueExW(opened, NULL, NULL, &type, (BYTE *)data, length ? &size : NULL);
  if (opened != key) RegCloseKey(opened);
  if (result == ERROR_SUCCESS && type != 1) result = ERROR_FILE_NOT_FOUND;
  if (length) *length = (LONG)size;
  return result;
}

/* ---- enumeration and information -------------------------------------------------------------------------------------------------------------------------- */

EXPORT LSTATUS WINAPI RegEnumKeyExW(HKEY key, DWORD index, LPWSTR name, LPDWORD name_capacity, LPDWORD reserved, LPWSTR class_name, LPDWORD class_capacity, PFILETIME write_time) {
  (void)reserved;
  HANDLE h = handle_of(key);
  if (!h) return ERROR_INVALID_HANDLE;
  if (!name || !name_capacity) return ERROR_INVALID_PARAMETER;
  BYTE *buffer = RtlAllocateHeap(heap(), 0, 20 + 1024);
  if (!buffer) return ERROR_NOT_ENOUGH_MEMORY;
  ULONG needed = 0;
  const NTSTATUS status = NtEnumerateKey(h, index, 1, buffer, 20 + 1024, &needed);      /* KeyNodeInformation */
  LSTATUS result = ERROR_SUCCESS;
  if (status == 0x8000001Au) result = ERROR_NO_MORE_ITEMS;
  else if (!NT_SUCCESS(status)) result = nt_error(status);
  else {
    const DWORD chars = *(DWORD *)(buffer + 16) / 2;
    if (chars >= *name_capacity) { result = ERROR_MORE_DATA; }
    else {
      memcpy(name, buffer + 20, chars * 2);
      name[chars] = 0;
      *name_capacity = chars;
      if (write_time) { write_time->dwLowDateTime = *(DWORD *)buffer; write_time->dwHighDateTime = *(DWORD *)(buffer + 4); }
      if (class_name && class_capacity && *class_capacity) class_name[0] = 0;
      if (class_capacity) *class_capacity = 0;
    }
  }
  RtlFreeHeap(heap(), 0, buffer);
  return result;
}
EXPORT LSTATUS WINAPI RegEnumKeyW(HKEY key, DWORD index, LPWSTR name, DWORD capacity) {
  DWORD n = capacity;
  return RegEnumKeyExW(key, index, name, &n, NULL, NULL, NULL, NULL);
}
EXPORT LSTATUS WINAPI RegEnumValueW(HKEY key, DWORD index, LPWSTR name, LPDWORD name_capacity, LPDWORD reserved, LPDWORD type, BYTE *data, LPDWORD data_capacity) {
  (void)reserved;
  HANDLE h = handle_of(key);
  if (!h) return ERROR_INVALID_HANDLE;
  if (!name || !name_capacity || (data && !data_capacity)) return ERROR_INVALID_PARAMETER;
  const DWORD room = data ? *data_capacity : 0;
  const SIZE_T size = 20 + 1024 + 8 + room + 8;
  BYTE *buffer = RtlAllocateHeap(heap(), 0, size);
  if (!buffer) return ERROR_NOT_ENOUGH_MEMORY;
  ULONG needed = 0;
  const NTSTATUS status = NtEnumerateValueKey(h, index, 1, buffer, (ULONG)size, &needed);      /* KeyValueFullInformation */
  LSTATUS result = ERROR_SUCCESS;
  if (status == 0x8000001Au) result = ERROR_NO_MORE_ITEMS;
  else if (!NT_SUCCESS(status) && status != 0x80000005u) result = nt_error(status);
  else {
    const DWORD name_bytes = *(DWORD *)(buffer + 16), data_offset = *(DWORD *)(buffer + 8), data_length = *(DWORD *)(buffer + 12);
    if (type) *type = *(DWORD *)(buffer + 4);
    if (name_bytes / 2 >= *name_capacity) result = ERROR_MORE_DATA;
    else {
      memcpy(name, buffer + 20, name_bytes);
      name[name_bytes / 2] = 0;
      *name_capacity = name_bytes / 2;
      if (data_capacity) {
        if (data && data_length > room) result = ERROR_MORE_DATA;
        else if (data) memcpy(data, buffer + data_offset, data_length);
        *data_capacity = data_length;
      }
    }
  }
  RtlFreeHeap(heap(), 0, buffer);
  return result;
}
EXPORT LSTATUS WINAPI RegQueryInfoKeyW(HKEY key, LPWSTR class_name, LPDWORD class_capacity, LPDWORD reserved, LPDWORD subkeys, LPDWORD max_subkey_length,
                                       LPDWORD max_class_length, LPDWORD values, LPDWORD max_value_name_length, LPDWORD max_value_data_length, LPDWORD security,
                                       PFILETIME write_time) {
  (void)reserved;
  HANDLE h = handle_of(key);
  if (!h) return ERROR_INVALID_HANDLE;
  BYTE info[44];
  ULONG needed = 0;
  const NTSTATUS status = NtQueryKey(h, 2, info, sizeof(info), &needed);      /* KeyFullInformation */
  if (!NT_SUCCESS(status)) return nt_error(status);
  if (class_name && class_capacity && *class_capacity) class_name[0] = 0;
  if (class_capacity) *class_capacity = 0;
  if (subkeys) *subkeys = *(DWORD *)(info + 20);
  if (max_subkey_length) *max_subkey_length = *(DWORD *)(info + 24) / 2;
  if (max_class_length) *max_class_length = 0;
  if (values) *values = *(DWORD *)(info + 32);
  if (max_value_name_length) *max_value_name_length = *(DWORD *)(info + 36) / 2;
  if (max_value_data_length) *max_value_data_length = *(DWORD *)(info + 40);
  if (security) *security = 0;
  if (write_time) { write_time->dwLowDateTime = *(DWORD *)info; write_time->dwHighDateTime = *(DWORD *)(info + 4); }
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegDeleteTreeW(HKEY key, LPCWSTR sub) {
  HKEY opened = key;
  if (sub && *sub) { const LSTATUS status = RegOpenKeyExW(key, sub, 0, 0xF003F, &opened); if (status != ERROR_SUCCESS) return status; }
  WCHAR name[260];
  for (;;) {      /* the subkeys first, each with everything below it */
    DWORD n = 260;
    if (RegEnumKeyExW(opened, 0, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
    const LSTATUS status = RegDeleteTreeW(opened, name);
    if (status != ERROR_SUCCESS) { if (opened != key) RegCloseKey(opened); return status; }
  }
  for (;;) {
    DWORD n = 260;
    if (RegEnumValueW(opened, 0, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
    RegDeleteValueW(opened, name);
  }
  LSTATUS result = ERROR_SUCCESS;
  if (opened != key) { RegCloseKey(opened); result = RegDeleteKeyW(key, sub); }
  return result;
}
EXPORT LSTATUS WINAPI RegNotifyChangeKeyValue(HKEY key, BOOL watch_subtree, DWORD filter, HANDLE event, BOOL asynchronous) { (void)watch_subtree; (void)filter; (void)event; (void)asynchronous; return handle_of(key) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE; }

/* ---- ANSI forms ----------------------------------------------------------------------------------------------------------------------------------------------- */

static WCHAR *to_wide_(const char *text) {
  if (!text) return NULL;
  const size_t_ n = strlen(text);
  WCHAR *w = RtlAllocateHeap(heap(), 0, (n + 1) * 2 * 2);
  if (!w) return NULL;
  int out = 0;
  for (size_t_ i = 0; i < n; ++i) {      /* UTF-8 to UTF-16 */
    const BYTE b = (BYTE)text[i];
    unsigned cp;
    int extra;
    if (b < 0x80) { cp = b; extra = 0; } else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; extra = 1; } else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; } else { cp = b & 0x07; extra = 3; }
    for (int k = 0; k < extra && i + 1 < n; ++k) cp = (cp << 6) | ((BYTE)text[++i] & 0x3F);
    if (cp >= 0x10000) { cp -= 0x10000; w[out++] = (WCHAR)(0xD800 + (cp >> 10)); w[out++] = (WCHAR)(0xDC00 + (cp & 0x3FF)); } else w[out++] = (WCHAR)cp;
  }
  w[out] = 0;
  return w;
}
static void free_wide_(WCHAR *w) { if (w) RtlFreeHeap(heap(), 0, w); }
/* UTF-16 to UTF-8 (up to `capacity` bytes with the NUL); returns the bytes needed without the NUL. */
static DWORD wide_to_utf8_(const WCHAR *w, DWORD count, char *out, DWORD capacity) {
  DWORD n = 0;
  for (DWORD i = 0; i < count; ++i) {
    unsigned cp = w[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00); ++i; }
    char bytes[4];
    int len;
    if (cp < 0x80) { bytes[0] = (char)cp; len = 1; }
    else if (cp < 0x800) { bytes[0] = (char)(0xC0 | (cp >> 6)); bytes[1] = (char)(0x80 | (cp & 0x3F)); len = 2; }
    else if (cp < 0x10000) { bytes[0] = (char)(0xE0 | (cp >> 12)); bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); bytes[2] = (char)(0x80 | (cp & 0x3F)); len = 3; }
    else { bytes[0] = (char)(0xF0 | (cp >> 18)); bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); bytes[3] = (char)(0x80 | (cp & 0x3F)); len = 4; }
    for (int k = 0; k < len; ++k, ++n) if (out && n < capacity) out[n] = bytes[k];
  }
  if (out && n < capacity) out[n] = 0;
  return n;
}
#define WITH_WIDE(var, text) WCHAR *var = to_wide_(text); if ((text) && !var) return ERROR_NOT_ENOUGH_MEMORY
EXPORT LSTATUS WINAPI RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD options, REGSAM access, PHKEY result) {
  WITH_WIDE(w, sub);
  const LSTATUS status = RegOpenKeyExW(key, w, options, access, result);
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegOpenKeyA(HKEY key, LPCSTR sub, PHKEY result) { return RegOpenKeyExA(key, sub, 0, 0, result); }
EXPORT LSTATUS WINAPI RegCreateKeyExA(HKEY key, LPCSTR sub, DWORD reserved, LPSTR class_name, DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES sa, PHKEY result, LPDWORD disposition) {
  (void)class_name;
  WITH_WIDE(w, sub);
  const LSTATUS status = RegCreateKeyExW(key, w, reserved, NULL, options, access, sa, result, disposition);
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegCreateKeyA(HKEY key, LPCSTR sub, PHKEY result) { return RegCreateKeyExA(key, sub, 0, NULL, 0, 0, NULL, result, NULL); }
EXPORT LSTATUS WINAPI RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD length) {
  WITH_WIDE(w, name);
  LSTATUS status;
  if ((type == 1 || type == 2 || type == 7) && data) {      /* text: the bytes are converted to wide characters */
    WCHAR *text = RtlAllocateHeap(heap(), 0, ((SIZE_T)length + 2) * 4);
    if (!text) { free_wide_(w); return ERROR_NOT_ENOUGH_MEMORY; }
    DWORD out = 0;
    for (DWORD i = 0; i < length; ++i) {
      const BYTE b = data[i];
      unsigned cp;
      int extra;
      if (b < 0x80) { cp = b; extra = 0; } else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; extra = 1; } else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; } else { cp = b & 0x07; extra = 3; }
      for (int k = 0; k < extra && i + 1 < length; ++k) cp = (cp << 6) | (data[++i] & 0x3F);
      if (cp >= 0x10000) { cp -= 0x10000; text[out++] = (WCHAR)(0xD800 + (cp >> 10)); text[out++] = (WCHAR)(0xDC00 + (cp & 0x3FF)); } else text[out++] = (WCHAR)cp;
    }
    if ((type == 1 || type == 2) && (out == 0 || text[out - 1] != 0)) text[out++] = 0;      /* a string is stored with its end */
    status = RegSetValueExW(key, w, reserved, type, (const BYTE *)text, out * 2);
    RtlFreeHeap(heap(), 0, text);
  } else status = RegSetValueExW(key, w, reserved, type, data, length);
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, BYTE *data, LPDWORD capacity) {
  WITH_WIDE(w, name);
  DWORD found = 0, size = 0;
  LSTATUS status = RegQueryValueExW(key, w, reserved, &found, NULL, &size);
  if (status == ERROR_SUCCESS && (found == 1 || found == 2 || found == 7)) {      /* text: converted back to narrow characters */
    WCHAR *text = RtlAllocateHeap(heap(), 0, (SIZE_T)size + 4);
    if (!text) status = ERROR_NOT_ENOUGH_MEMORY;
    else {
      DWORD got = size;
      status = RegQueryValueExW(key, w, reserved, &found, (BYTE *)text, &got);
      if (status == ERROR_SUCCESS) {
        const DWORD needed = wide_to_utf8_(text, got / 2, NULL, 0);
        if (type) *type = found;
        if (capacity) {
          if (data && needed > *capacity) status = ERROR_MORE_DATA;
          else if (data) wide_to_utf8_(text, got / 2, (char *)data, *capacity);
          *capacity = needed;
        }
      }
      RtlFreeHeap(heap(), 0, text);
    }
  } else if (status == ERROR_SUCCESS) {
    status = RegQueryValueExW(key, w, reserved, type, data, capacity);
  } else if (type) *type = found;
  free_wide_(w);
  return status;
}
EXPORT LSTATUS WINAPI RegGetValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD flags, LPDWORD type, PVOID data, LPDWORD capacity) {
  { const LSTATUS bad = rrf_arguments(flags, data, capacity); if (bad != ERROR_SUCCESS) return bad; }
  WITH_WIDE(ws, sub);
  HKEY opened = key;
  LSTATUS status = ERROR_SUCCESS;
  if (ws && *ws) status = RegOpenKeyExW(key, ws, 0, 0x20019, &opened);
  free_wide_(ws);
  if (status != ERROR_SUCCESS) return status;
  DWORD found = 0;
  status = RegQueryValueExA(opened, name, NULL, &found, (BYTE *)data, capacity);
  if (opened != key) RegCloseKey(opened);
  if (status == ERROR_SUCCESS || status == ERROR_MORE_DATA) {
    if (!rrf_type_accepted(flags, found)) status = ERROR_UNSUPPORTED_TYPE;
    else if (type) *type = found;
  }
  if (status != ERROR_SUCCESS && (flags & 0x20000000) && data && capacity) memset(data, 0, *capacity);      /* RRF_ZEROONFAILURE */
  return status;
}
EXPORT LSTATUS WINAPI RegDeleteValueA(HKEY key, LPCSTR name) { WITH_WIDE(w, name); const LSTATUS status = RegDeleteValueW(key, w); free_wide_(w); return status; }
EXPORT LSTATUS WINAPI RegDeleteKeyA(HKEY key, LPCSTR sub) { WITH_WIDE(w, sub); const LSTATUS status = RegDeleteKeyW(key, w); free_wide_(w); return status; }
EXPORT LSTATUS WINAPI RegDeleteKeyExA(HKEY key, LPCSTR sub, REGSAM access, DWORD reserved) { (void)access; (void)reserved; return RegDeleteKeyA(key, sub); }
EXPORT LSTATUS WINAPI RegDeleteTreeA(HKEY key, LPCSTR sub) { WITH_WIDE(w, sub); const LSTATUS status = RegDeleteTreeW(key, w); free_wide_(w); return status; }
EXPORT LSTATUS WINAPI RegDeleteKeyValueA(HKEY key, LPCSTR sub, LPCSTR name) {
  WITH_WIDE(ws, sub);
  WCHAR *wn = to_wide_(name);
  const LSTATUS status = RegDeleteKeyValueW(key, ws, wn);
  free_wide_(ws);
  free_wide_(wn);
  return status;
}
EXPORT LSTATUS WINAPI RegEnumKeyExA(HKEY key, DWORD index, LPSTR name, LPDWORD name_capacity, LPDWORD reserved, LPSTR class_name, LPDWORD class_capacity, PFILETIME write_time) {
  (void)class_name;
  if (!name || !name_capacity) return ERROR_INVALID_PARAMETER;
  WCHAR wide[300];
  DWORD wide_capacity = 300;
  const LSTATUS status = RegEnumKeyExW(key, index, wide, &wide_capacity, reserved, NULL, class_capacity, write_time);
  if (status != ERROR_SUCCESS) return status;
  const DWORD needed = wide_to_utf8_(wide, wide_capacity, NULL, 0);
  if (needed >= *name_capacity) return ERROR_MORE_DATA;
  wide_to_utf8_(wide, wide_capacity, name, *name_capacity);
  *name_capacity = needed;
  return ERROR_SUCCESS;
}
EXPORT LSTATUS WINAPI RegEnumKeyA(HKEY key, DWORD index, LPSTR name, DWORD capacity) { DWORD n = capacity; return RegEnumKeyExA(key, index, name, &n, NULL, NULL, NULL, NULL); }
EXPORT LSTATUS WINAPI RegEnumValueA(HKEY key, DWORD index, LPSTR name, LPDWORD name_capacity, LPDWORD reserved, LPDWORD type, BYTE *data, LPDWORD data_capacity) {
  if (!name || !name_capacity) return ERROR_INVALID_PARAMETER;
  WCHAR wide[16400];
  DWORD wide_capacity = 16400, found = 0;
  WCHAR *text = NULL;
  DWORD room = data && data_capacity ? *data_capacity * 2 + 4 : 0;
  if (room) text = RtlAllocateHeap(heap(), 0, room);
  DWORD size = room;
  LSTATUS status = RegEnumValueW(key, index, wide, &wide_capacity, reserved, &found, (BYTE *)text, data ? &size : data_capacity);
  if (status == ERROR_SUCCESS) {
    if (type) *type = found;
    const DWORD needed = wide_to_utf8_(wide, wide_capacity, NULL, 0);
    if (needed >= *name_capacity) status = ERROR_MORE_DATA;
    else {
      wide_to_utf8_(wide, wide_capacity, name, *name_capacity);
      *name_capacity = needed;
      if (data && data_capacity) {
        if (found == 1 || found == 2 || found == 7) {
          const DWORD bytes = wide_to_utf8_(text, size / 2, NULL, 0);
          if (bytes > *data_capacity) status = ERROR_MORE_DATA; else wide_to_utf8_(text, size / 2, (char *)data, *data_capacity);
          *data_capacity = bytes;
        } else { memcpy(data, text, size); *data_capacity = size; }
      }
    }
  }
  if (status == ERROR_MORE_DATA && data && data_capacity && wide_capacity < 16400) {
    if (found == 1 || found == 2 || found == 7) *data_capacity = size / 2 > 0 ? size / 2 : size;
    else *data_capacity = size;
  }
  if (text) RtlFreeHeap(heap(), 0, text);
  return status;
}
EXPORT LSTATUS WINAPI RegQueryInfoKeyA(HKEY key, LPSTR class_name, LPDWORD class_capacity, LPDWORD reserved, LPDWORD subkeys, LPDWORD max_subkey_length,
                                       LPDWORD max_class_length, LPDWORD values, LPDWORD max_value_name_length, LPDWORD max_value_data_length, LPDWORD security,
                                       PFILETIME write_time) {
  if (class_name && class_capacity && *class_capacity) class_name[0] = 0;
  return RegQueryInfoKeyW(key, NULL, class_capacity, reserved, subkeys, max_subkey_length, max_class_length, values, max_value_name_length, max_value_data_length, security, write_time);
}
EXPORT LSTATUS WINAPI RegSetValueA(HKEY key, LPCSTR sub, DWORD type, LPCSTR data, DWORD length) {
  WITH_WIDE(ws, sub);
  WCHAR *wd = to_wide_(data);
  const LSTATUS status = RegSetValueW(key, ws, type, wd, length);
  free_wide_(ws);
  free_wide_(wd);
  return status;
}
EXPORT LSTATUS WINAPI RegQueryValueA(HKEY key, LPCSTR sub, LPSTR data, PLONG length) {
  WITH_WIDE(ws, sub);
  HKEY opened = key;
  LSTATUS status = ERROR_SUCCESS;
  if (ws && *ws) status = RegOpenKeyExW(key, ws, 0, 0, &opened);
  free_wide_(ws);
  if (status != ERROR_SUCCESS) return status;
  DWORD type = 0, size = length ? (DWORD)*length : 0;
  status = RegQueryValueExA(opened, NULL, NULL, &type, (BYTE *)data, length ? &size : NULL);
  if (opened != key) RegCloseKey(opened);
  if (status == ERROR_SUCCESS && type != 1) status = ERROR_FILE_NOT_FOUND;
  if (length) *length = (LONG)size;
  return status;
}
EXPORT LSTATUS WINAPI RegSetKeyValueA(HKEY key, LPCSTR sub, LPCSTR name, DWORD type, LPCVOID data, DWORD length) {
  WITH_WIDE(ws, sub);
  HKEY opened = key;
  LSTATUS status = ERROR_SUCCESS;
  if (ws && *ws) status = RegCreateKeyExW(key, ws, 0, NULL, 0, 0, NULL, &opened, NULL);
  free_wide_(ws);
  if (status != ERROR_SUCCESS) return status;
  status = RegSetValueExA(opened, name, 0, type, data, length);
  if (opened != key) RegCloseKey(opened);
  return status;
}

