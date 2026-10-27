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

/* ntdll.dll: the Rtl helpers over the registry services: the path of the current user's key, and reading and writing values by a path relative to one of the usual places.
 * Included by ntdll.c after rtl2.inc. */

typedef ULONG ACCESS_MASK;
NTSTATUS NTAPI NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
NTSTATUS NTAPI NtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
NTSTATUS NTAPI NtSetValueKey(HANDLE, PUNICODE_STRING, ULONG, ULONG, PVOID, ULONG);
NTSTATUS NTAPI NtDeleteValueKey(HANDLE, PUNICODE_STRING);

#define RTL_REGISTRY_ABSOLUTE_ 0
#define RTL_REGISTRY_SERVICES_ 1
#define RTL_REGISTRY_CONTROL_ 2
#define RTL_REGISTRY_WINDOWS_NT_ 3
#define RTL_REGISTRY_DEVICEMAP_ 4
#define RTL_REGISTRY_USER_ 5
#define RTL_REGISTRY_HANDLE_ 0x40000000u
#define RTL_REGISTRY_OPTIONAL_ 0x80000000u

#define REGISTRY_USER_PATH_ L"\\Registry\\User\\S-1-5-21-0-0-0-1000"

EXPORT NTSTATUS NTAPI RtlFormatCurrentUserKeyPath(PUNICODE_STRING path) {
  static const WCHAR text[] = REGISTRY_USER_PATH_;
  path->Length = (USHORT)(sizeof(text) - 2);
  path->MaximumLength = (USHORT)sizeof(text);
  path->Buffer = RtlAllocateHeap(RtlProcessHeap(), 0, sizeof(text));
  if (!path->Buffer) return STATUS_NO_MEMORY;
  memcpy(path->Buffer, text, sizeof(text));
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlOpenCurrentUser(ACCESS_MASK access, PHANDLE key) {
  static const WCHAR text[] = REGISTRY_USER_PATH_;
  UNICODE_STRING name;
  name.Buffer = (PWSTR)text;
  name.Length = (USHORT)(sizeof(text) - 2);
  name.MaximumLength = (USHORT)sizeof(text);
  OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, &name, 0x40, NULL, NULL};
  return NtOpenKey(key, access, &attributes);
}

/* The key a relative_to/path pair names (for the callers below): the base is one of the places under \Registry\Machine\System\CurrentControlSet and so on. */
static NTSTATUS registry_open_relative(ULONG relative_to, PCWSTR path, BOOLEAN create, ACCESS_MASK access, PHANDLE key) {
  static const WCHAR *const bases[] = {L"", L"\\Registry\\Machine\\System\\CurrentControlSet\\Services", L"\\Registry\\Machine\\System\\CurrentControlSet\\Control",
                                       L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion", L"\\Registry\\Machine\\Hardware\\DeviceMap", REGISTRY_USER_PATH_};
  WCHAR full[600];
  SIZE_T n = 0;
  HANDLE root = NULL;
  const ULONG kind = relative_to & ~(RTL_REGISTRY_HANDLE_ | RTL_REGISTRY_OPTIONAL_);
  if (relative_to & RTL_REGISTRY_HANDLE_) root = (HANDLE)path;      /* (the path is then the handle itself) */
  else if (kind > RTL_REGISTRY_USER_) return STATUS_INVALID_PARAMETER;
  else for (const WCHAR *b = bases[kind]; *b && n < 400; ++b) full[n++] = *b;
  if (!(relative_to & RTL_REGISTRY_HANDLE_) && path && *path) {
    if (n && path[0] != '\\' && n < 599) full[n++] = '\\';
    for (const WCHAR *c = path; *c && n < 599; ++c) full[n++] = *c;
  }
  full[n] = 0;
  UNICODE_STRING name;
  name.Buffer = full;
  name.Length = (USHORT)(n * 2);
  name.MaximumLength = (USHORT)(n * 2 + 2);
  OBJECT_ATTRIBUTES attributes = {sizeof(attributes), root, root ? NULL : &name, 0x40, NULL, NULL};
  if (root) { name.Buffer = full; attributes.ObjectName = &name; name.Length = 0; }
  if (!create) return NtOpenKey(key, access, &attributes);
  /* every level of the path is made, one after the other */
  NTSTATUS status = STATUS_SUCCESS;
  HANDLE opened = NULL;
  for (SIZE_T end = 1; end <= n; ++end) {
    if (end != n && full[end] != '\\') continue;
    UNICODE_STRING part = name;
    part.Length = (USHORT)(end * 2);
    part.MaximumLength = (USHORT)(end * 2 + 2);
    OBJECT_ATTRIBUTES level = {sizeof(level), NULL, &part, 0x40, NULL, NULL};
    ULONG disposition = 0;
    HANDLE h = NULL;
    status = NtCreateKey(&h, access, &level, 0, NULL, 0, &disposition);
    if (!NT_SUCCESS(status)) return status;
    if (opened) NtClose(opened);
    opened = h;
  }
  *key = opened;
  return status;
}
EXPORT NTSTATUS NTAPI RtlCheckRegistryKey(ULONG relative_to, PWSTR path) {
  HANDLE key = NULL;
  const NTSTATUS status = registry_open_relative(relative_to, path, FALSE, 0x20019, &key);
  if (NT_SUCCESS(status)) NtClose(key);
  return status;
}
EXPORT NTSTATUS NTAPI RtlCreateRegistryKey(ULONG relative_to, PWSTR path) {
  HANDLE key = NULL;
  const NTSTATUS status = registry_open_relative(relative_to, path, TRUE, 0xF003F, &key);
  if (NT_SUCCESS(status)) NtClose(key);
  return status;
}
EXPORT NTSTATUS NTAPI RtlWriteRegistryValue(ULONG relative_to, PCWSTR path, PCWSTR name, ULONG type, PVOID data, ULONG length) {
  HANDLE key = NULL;
  NTSTATUS status = registry_open_relative(relative_to, path, FALSE, 0xF003F, &key);
  if (!NT_SUCCESS(status)) return status;
  UNICODE_STRING value;
  RtlInitUnicodeString(&value, name);
  status = NtSetValueKey(key, &value, 0, type, data, length);
  NtClose(key);
  return status;
}
EXPORT NTSTATUS NTAPI RtlDeleteRegistryValue(ULONG relative_to, PCWSTR path, PCWSTR name) {
  HANDLE key = NULL;
  NTSTATUS status = registry_open_relative(relative_to, path, FALSE, 0xF003F, &key);
  if (!NT_SUCCESS(status)) return status;
  UNICODE_STRING value;
  RtlInitUnicodeString(&value, name);
  status = NtDeleteValueKey(key, &value);
  NtClose(key);
  return status;
}
