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

/* ntdll.dll: the registry query table, the current directory, DOS path helpers and some odds and ends of the Rtl library. Included by ntdll.c after rtlx2.inc. */

#define STATUS_OBJECT_NAME_INVALID_ ((NTSTATUS)0xC0000033)
#define STATUS_OBJECT_PATH_NOT_FOUND_ ((NTSTATUS)0xC000003A)
NTSTATUS NTAPI NtQueryValueKey(HANDLE key, PUNICODE_STRING name, ULONG information_class, PVOID information, ULONG length, PULONG result_length);

/* ---- RtlQueryRegistryValues ---- */

typedef NTSTATUS (NTAPI *RTL_QUERY_REGISTRY_ROUTINE_)(PWSTR name, ULONG type, PVOID data, ULONG length, PVOID context, PVOID entry_context);
typedef struct RTL_QUERY_REGISTRY_TABLE_ {
  RTL_QUERY_REGISTRY_ROUTINE_ routine;
  ULONG flags;
  PWSTR name;
  PVOID entry_context;
  ULONG default_type;
  PVOID default_data;
  ULONG default_length;
} RTL_QUERY_REGISTRY_TABLE_;
#define QRV_SUBKEY 0x01u
#define QRV_TOPKEY 0x02u
#define QRV_REQUIRED 0x04u
#define QRV_NOVALUE 0x08u
#define QRV_NOEXPAND 0x10u
#define QRV_DIRECT 0x20u
#define QRV_DELETE 0x40u
NTSTATUS NTAPI NtEnumerateValueKey(HANDLE key, ULONG index, ULONG information_class, PVOID information, ULONG length, PULONG returned);

/* One value found (or given as the default) goes to the routine, or straight into the caller's variable. */
static NTSTATUS qrv_deliver(const RTL_QUERY_REGISTRY_TABLE_ *e, PWSTR name, ULONG type, const BYTE *data, ULONG length, PVOID context, PVOID environment) {
  BYTE *expanded = NULL;
  if (type == 2 /* REG_EXPAND_SZ */ && !(e->flags & QRV_NOEXPAND)) {
    UNICODE_STRING source, destination;
    source.Buffer = (PWSTR)data;
    source.Length = (USHORT)(length >= 2 ? length - 2 : 0);
    source.MaximumLength = (USHORT)length;
    ULONG needed = 0;
    destination.Buffer = NULL; destination.Length = 0; destination.MaximumLength = 0;
    RtlExpandEnvironmentStrings_U(environment, &source, &destination, &needed);
    if (needed > 0 && needed < 0xFFFF) {
      expanded = RtlAllocateHeap(process_heap(), 0, needed + 2);
      if (!expanded) return STATUS_NO_MEMORY;
      destination.Buffer = (PWSTR)expanded; destination.Length = 0; destination.MaximumLength = (USHORT)(needed + 2);
      RtlExpandEnvironmentStrings_U(environment, &source, &destination, &needed);
      ((WCHAR *)expanded)[destination.Length / 2] = 0;
      data = expanded;
      length = destination.Length + 2;
      type = 1;
    }
  }
  NTSTATUS status = STATUS_SUCCESS;
  const BOOLEAN is_string = type == 1 || type == 2 || type == 6 || type == 7;
  if (e->flags & QRV_DIRECT) {
    if (is_string) {
      UNICODE_STRING *s = e->entry_context;
      const ULONG bytes = length >= 2 && (type != 7) ? length - 2 : length;
      if (!s->Buffer) {
        s->Buffer = RtlAllocateHeap(process_heap(), 0, (SIZE_T)length + 2);
        if (!s->Buffer) status = STATUS_NO_MEMORY; else s->MaximumLength = (USHORT)(length + 2);
      }
      if (NT_SUCCESS(status)) {
        if (s->MaximumLength < length) status = (NTSTATUS)0xC0000023;
        else { memcpy(s->Buffer, data, length); s->Length = (USHORT)bytes; if (type != 7 && s->MaximumLength >= length + 2 && length >= 2 && ((WCHAR *)s->Buffer)[length / 2 - 1] != 0) ((WCHAR *)s->Buffer)[length / 2] = 0; }
      }
    } else if (type == 4 && length >= 4) *(ULONG *)e->entry_context = *(const ULONG *)data;
    else {
      BYTE *dest = e->entry_context;
      const LONG capacity = *(LONG *)dest;
      if (capacity < 0 || (ULONG)capacity < length + 8) status = (NTSTATUS)0xC0000023;
      else { *(ULONG *)dest = length; *(ULONG *)(dest + 4) = type; memcpy(dest + 8, data, length); }
    }
  } else if (type == 7 /* REG_MULTI_SZ */ && !(e->flags & QRV_NOEXPAND)) {      /* a routine gets each string of a list on its own */
    const WCHAR *p = (const WCHAR *)data;
    const WCHAR *end = (const WCHAR *)(data + length);
    while (p < end && *p && NT_SUCCESS(status)) {
      SIZE_T n = 0;
      while (p + n < end && p[n]) ++n;
      status = e->routine(name, 1, (PVOID)p, (ULONG)(n * 2 + 2), context, e->entry_context);
      p += n + 1;
    }
  } else if (e->routine) status = e->routine(name, type, (PVOID)data, length, context, e->entry_context);
  if (expanded) RtlFreeHeap(process_heap(), 0, expanded);
  return status;
}

EXPORT NTSTATUS NTAPI RtlQueryRegistryValues(ULONG relative_to, PCWSTR path, RTL_QUERY_REGISTRY_TABLE_ *table, PVOID context, PVOID environment) {
  HANDLE base = NULL;
  NTSTATUS status = registry_open_relative(relative_to, path, FALSE, 0x20019, &base);
  if (!NT_SUCCESS(status)) return status;
  HANDLE current = base;
  status = STATUS_SUCCESS;
  for (; NT_SUCCESS(status) && (table->routine || table->name); ++table) {
    if (table->flags & (QRV_SUBKEY | QRV_TOPKEY)) {
      if (current != base) NtClose(current);
      current = base;
      if (table->flags & QRV_SUBKEY) {
        if (!table->name) { status = STATUS_INVALID_PARAMETER; break; }
        UNICODE_STRING sub;
        RtlInitUnicodeString(&sub, table->name);
        OBJECT_ATTRIBUTES attributes = {sizeof(attributes), base, &sub, 0x40, NULL, NULL};
        HANDLE opened = NULL;
        status = NtOpenKey(&opened, 0x20019, &attributes);
        if (!NT_SUCCESS(status)) break;
        current = opened;
        if (!table->routine && !(table->flags & QRV_DIRECT)) continue;
      }
      if (!(table->flags & QRV_SUBKEY) && !table->name && !table->routine) continue;
    }
    if (table->flags & QRV_NOVALUE) {
      if (table->routine) status = table->routine(NULL, 0 /* REG_NONE */, NULL, 0, context, table->entry_context);
      continue;
    }
    if (!table->name) {      /* all the values of the key */
      BYTE *buffer = RtlAllocateHeap(process_heap(), 0, 4096);
      if (!buffer) { status = STATUS_NO_MEMORY; break; }
      for (ULONG index = 0; NT_SUCCESS(status); ++index) {
        ULONG got = 0;
        NTSTATUS next = NtEnumerateValueKey(current, index, 1 /* KeyValueFullInformation */, buffer, 4096, &got);
        if (next == (NTSTATUS)0x8000001A /* STATUS_NO_MORE_ENTRIES */) break;
        if (!NT_SUCCESS(next)) { status = next; break; }
        const ULONG data_offset = *(ULONG *)(buffer + 8), data_length = *(ULONG *)(buffer + 12), name_length = *(ULONG *)(buffer + 16);
        WCHAR name[260];
        const ULONG n = name_length / 2 < 259 ? name_length / 2 : 259;
        memcpy(name, buffer + 20, n * 2);
        name[n] = 0;
        status = qrv_deliver(table, name, *(ULONG *)(buffer + 4), buffer + data_offset, data_length, context, environment);
      }
      RtlFreeHeap(process_heap(), 0, buffer);
      continue;
    }
    UNICODE_STRING value_name;
    RtlInitUnicodeString(&value_name, table->name);
    BYTE *buffer = RtlAllocateHeap(process_heap(), 0, 4096);
    if (!buffer) { status = STATUS_NO_MEMORY; break; }
    ULONG got = 0;
    NTSTATUS found = NtQueryValueKey(current, &value_name, 2 /* KeyValuePartialInformation */, buffer, 4096, &got);
    if (found == (NTSTATUS)0x80000005 || found == (NTSTATUS)0xC0000023) {      /* a longer value than the first try */
      RtlFreeHeap(process_heap(), 0, buffer);
      buffer = RtlAllocateHeap(process_heap(), 0, got + 16);
      if (!buffer) { status = STATUS_NO_MEMORY; break; }
      found = NtQueryValueKey(current, &value_name, 2, buffer, got + 16, &got);
    }
    if (NT_SUCCESS(found)) {
      status = qrv_deliver(table, table->name, *(ULONG *)(buffer + 4), buffer + 12, *(ULONG *)(buffer + 8), context, environment);
      if ((table->flags & QRV_DELETE) && NT_SUCCESS(status)) NtDeleteValueKey(current, &value_name);
    } else if (table->default_type != 0 /* REG_NONE */) {
      ULONG length = table->default_length;
      if (length == 0 && (table->default_type == 1 || table->default_type == 2 || table->default_type == 7) && table->default_data) {
        const WCHAR *p = table->default_data;
        SIZE_T n = 0;
        while (p[n]) ++n;
        length = (ULONG)(n * 2 + 2);
      }
      status = qrv_deliver(table, table->name, table->default_type, table->default_data, length, context, environment);
    } else if (table->flags & QRV_REQUIRED) status = STATUS_OBJECT_NAME_NOT_FOUND;
    RtlFreeHeap(process_heap(), 0, buffer);
  }
  if (current != base) NtClose(current);
  NtClose(base);
  return status;
}
EXPORT NTSTATUS NTAPI RtlQueryRegistryValuesEx(ULONG relative_to, PCWSTR path, RTL_QUERY_REGISTRY_TABLE_ *table, PVOID context, PVOID environment) { return RtlQueryRegistryValues(relative_to, path, table, context, environment); }
EXPORT NTSTATUS NTAPI RtlQueryRegistryValueWithFallback(HANDLE user_key, HANDLE system_key, PUNICODE_STRING name, ULONG *type, PVOID buffer, ULONG size, ULONG *returned) {
  BYTE *information = RtlAllocateHeap(process_heap(), 0, size + 64);
  if (!information) return STATUS_NO_MEMORY;
  ULONG got = 0;
  NTSTATUS status = user_key ? NtQueryValueKey(user_key, name, 2, information, size + 64, &got) : STATUS_OBJECT_NAME_NOT_FOUND;
  if (!NT_SUCCESS(status) && system_key) status = NtQueryValueKey(system_key, name, 2, information, size + 64, &got);
  if (NT_SUCCESS(status)) {
    const ULONG length = *(ULONG *)(information + 8);
    if (type) *type = *(ULONG *)(information + 4);
    if (returned) *returned = length;
    if (length > size) status = (NTSTATUS)0xC0000023;
    else memcpy(buffer, information + 12, length);
  }
  RtlFreeHeap(process_heap(), 0, information);
  return status;
}

/* ---- the current directory (the process parameters keep its path; the directory itself is held open) ---- */

EXPORT ULONG NTAPI RtlGetCurrentDirectory_U(ULONG size, PWSTR buffer) {
  RTL_USER_PROCESS_PARAMETERS *p = NtCurrentPeb()->ProcessParameters;
  const ULONG length = p->CurrentDirectoryPath.Length;
  RtlAcquirePebLock();
  ULONG result = length;
  if (size >= length + 2) {
    memcpy(buffer, p->CurrentDirectoryPath.Buffer, length);
    buffer[length / 2] = 0;
    /* a trailing separator stays only on a root ("C:\") */
    if (length > 6 && buffer[length / 2 - 1] == '\\') { buffer[length / 2 - 1] = 0; result = length - 2; }
  } else if (length > 6 && p->CurrentDirectoryPath.Buffer[length / 2 - 1] == '\\') result = length - 2;
  RtlReleasePebLock();
  return result;
}
EXPORT NTSTATUS NTAPI RtlSetCurrentDirectory_U(PUNICODE_STRING path) {
  WCHAR text[520], full[520];
  const SIZE_T n = path->Length / 2;
  if (n == 0 || n >= 520) return STATUS_OBJECT_NAME_INVALID_;
  memcpy(text, path->Buffer, n * 2);
  text[n] = 0;
  const ULONG chars = RtlGetFullPathName_U(text, sizeof(full) - 4, full, NULL) / 2;
  if (chars == 0 || chars >= 518) return STATUS_OBJECT_NAME_INVALID_;
  UNICODE_STRING nt;
  PWSTR part;
  if (!RtlDosPathNameToNtPathName_U(full, &nt, &part, NULL)) return STATUS_OBJECT_NAME_INVALID_;
  OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, &nt, 0x40, NULL, NULL};
  IO_STATUS_BLOCK iosb;
  HANDLE held = NULL;
  NTSTATUS status = NtOpenFile(&held, 0x20 | 0x00100000, &attributes, &iosb, 3, 0x1 | 0x20);
  RtlFreeUnicodeString(&nt);
  if (!NT_SUCCESS(status)) return status;
  SIZE_T length = chars;
  if (full[length - 1] != '\\') full[length++] = '\\';
  full[length] = 0;
  WCHAR *fresh = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, (length > 259 ? length + 1 : 260) * 2);
  if (!fresh) { NtClose(held); return STATUS_NO_MEMORY; }
  memcpy(fresh, full, (length + 1) * 2);
  RTL_USER_PROCESS_PARAMETERS *p = NtCurrentPeb()->ProcessParameters;
  RtlAcquirePebLock();
  WCHAR *old = p->CurrentDirectoryPath.Buffer;
  HANDLE old_handle = p->CurrentDirectoryHandle;
  p->CurrentDirectoryPath.Buffer = fresh;
  p->CurrentDirectoryPath.Length = (USHORT)(length * 2);
  p->CurrentDirectoryPath.MaximumLength = (USHORT)((length > 259 ? length + 1 : 260) * 2);
  p->CurrentDirectoryHandle = held;
  RtlReleasePebLock();
  if (old_handle) NtClose(old_handle);
  if (old) RtlFreeHeap(process_heap(), 0, old);
  return STATUS_SUCCESS;
}

/* ---- DOS paths ---- */

EXPORT BOOLEAN NTAPI RtlDoesFileExists_U(PCWSTR path) {
  UNICODE_STRING nt;
  PWSTR part;
  if (!RtlDosPathNameToNtPathName_U(path, &nt, &part, NULL)) return FALSE;
  OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, &nt, 0x40, NULL, NULL};
  BYTE basic[40];
  const NTSTATUS status = NtQueryAttributesFile(&attributes, basic);
  RtlFreeUnicodeString(&nt);
  return NT_SUCCESS(status);
}
/* Looks for a file in the directories of a ';' list (the extension is added to a name without one); the length in bytes of the full name written, or 0. */
EXPORT ULONG NTAPI RtlDosSearchPath_U(PCWSTR path, PCWSTR name, PCWSTR extension, ULONG size, PWSTR buffer, PWSTR *file_part) {
  WCHAR candidate[520];
  SIZE_T name_length = 0;
  while (name[name_length]) ++name_length;
  BOOLEAN has_extension = FALSE;
  for (SIZE_T i = name_length; i > 0 && name[i - 1] != '\\' && name[i - 1] != '/'; --i) if (name[i - 1] == '.') { has_extension = TRUE; break; }
  SIZE_T extension_length = 0;
  if (!has_extension && extension) while (extension[extension_length]) ++extension_length;
  const ULONG type = RtlDetermineDosPathNameType_U(name);
  const WCHAR *list = path;
  if (type != 5 /* RELATIVE */ && type != 0) list = NULL;
  for (;;) {
    SIZE_T length = 0;
    if (list) {
      const WCHAR *end = list;
      while (*end && *end != ';') ++end;
      const SIZE_T dir = (SIZE_T)(end - list);
      if (dir + name_length + extension_length + 2 >= 520) return 0;
      if (dir) {
        memcpy(candidate, list, dir * 2);
        length = dir;
        if (candidate[length - 1] != '\\' && candidate[length - 1] != '/') candidate[length++] = '\\';
      }
      list = *end ? end + 1 : NULL;
      if (dir == 0 && *end == 0 && end == path) { /* an empty list */ }
      memcpy(candidate + length, name, name_length * 2);
      length += name_length;
    } else {
      if (name_length + extension_length + 1 >= 520) return 0;
      memcpy(candidate, name, name_length * 2);
      length = name_length;
    }
    if (extension_length) { memcpy(candidate + length, extension, extension_length * 2); length += extension_length; }
    candidate[length] = 0;
    if (RtlDoesFileExists_U(candidate)) return RtlGetFullPathName_U(candidate, size, buffer, file_part);
    if (!list) {
      if (type == 5 || type == 0 || !path) return 0;
      return 0;
    }
  }
}
typedef struct RELATIVE_NAME_ { UNICODE_STRING relative; HANDLE containing_directory; PVOID cur_dir_ref; } RELATIVE_NAME_;
EXPORT NTSTATUS NTAPI RtlDosPathNameToRelativeNtPathName_U_WithStatus(PCWSTR dos, PUNICODE_STRING nt, PWSTR *file_part, RELATIVE_NAME_ *relative) {
  if (relative) memset(relative, 0, sizeof(*relative));
  if (!RtlDosPathNameToNtPathName_U(dos, nt, file_part, NULL)) return STATUS_OBJECT_PATH_NOT_FOUND_;
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlDosPathNameToRelativeNtPathName_U(PCWSTR dos, PUNICODE_STRING nt, PWSTR *file_part, RELATIVE_NAME_ *relative) { return NT_SUCCESS(RtlDosPathNameToRelativeNtPathName_U_WithStatus(dos, nt, file_part, relative)); }
EXPORT NTSTATUS NTAPI RtlDosLongPathNameToNtPathName_U_WithStatus(PCWSTR dos, PUNICODE_STRING nt, PWSTR *file_part, PVOID relative) {
  if (relative) memset(relative, 0, sizeof(RELATIVE_NAME_));
  if (!RtlDosPathNameToNtPathName_U(dos, nt, file_part, NULL)) return STATUS_OBJECT_PATH_NOT_FOUND_;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDosLongPathNameToRelativeNtPathName_U_WithStatus(PCWSTR dos, PUNICODE_STRING nt, PWSTR *file_part, RELATIVE_NAME_ *relative) { return RtlDosPathNameToRelativeNtPathName_U_WithStatus(dos, nt, file_part, relative); }
EXPORT void NTAPI RtlReleaseRelativeName(RELATIVE_NAME_ *relative) { (void)relative; }
EXPORT NTSTATUS NTAPI RtlGetLengthWithoutTrailingPathSeperators(ULONG flags, const UNICODE_STRING *path, ULONG *length) {
  if (flags != 0 || !path || !length) return STATUS_INVALID_PARAMETER;
  ULONG n = path->Length / 2;
  while (n > 0 && (path->Buffer[n - 1] == '\\' || path->Buffer[n - 1] == '/')) --n;
  *length = n;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlGetLengthWithoutLastFullDosOrNtPathElement(ULONG flags, const UNICODE_STRING *path, ULONG *length) {
  if (flags != 0 || !path || !length) return STATUS_INVALID_PARAMETER;
  ULONG n = path->Length / 2;
  while (n > 0 && (path->Buffer[n - 1] == '\\' || path->Buffer[n - 1] == '/')) --n;
  while (n > 0 && path->Buffer[n - 1] != '\\' && path->Buffer[n - 1] != '/') --n;
  while (n > 0 && (path->Buffer[n - 1] == '\\' || path->Buffer[n - 1] == '/')) --n;
  *length = n;
  return STATUS_SUCCESS;
}

/* ---- machines, odds and ends ---- */

EXPORT USHORT NTAPI RtlWow64GetCurrentMachine(void) { return 0x8664; }
EXPORT NTSTATUS NTAPI RtlWow64GetProcessMachines(HANDLE process, USHORT *process_machine, USHORT *native_machine) {
  (void)process;
  if (process_machine) *process_machine = 0;
  if (native_machine) *native_machine = 0x8664;
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlWow64IsWowGuestMachineSupported(USHORT machine, BOOLEAN *supported) { (void)machine; if (supported) *supported = FALSE; return TRUE; }
EXPORT NTSTATUS NTAPI RtlWow64EnableFsRedirection(BOOLEAN enable) { (void)enable; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlWow64EnableFsRedirectionEx(PVOID disable, PVOID *old) { (void)disable; if (old) *old = NULL; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlGetImageFileMachines(PCWSTR path, USHORT *machines) { (void)path; if (machines) *machines = 0x8664; return STATUS_SUCCESS; }
EXPORT ULONG NTAPI RtlRandomEx(PULONG seed) { return RtlRandom(seed); }
EXPORT BOOLEAN NTAPI RtlUdiv128(ULONGLONG high, ULONGLONG low, ULONGLONG divisor, ULONGLONG *quotient, ULONGLONG *remainder) {
  if (divisor == 0 || high >= divisor) return FALSE;
  unsigned __int128 n = ((unsigned __int128)high << 64) | low;
  /* a division the compiler's runtime would have to supply: done bit by bit */
  ULONGLONG q = 0, r = high;
  for (int i = 63; i >= 0; --i) {
    const int carry = (int)(r >> 63);
    r = (r << 1) | ((low >> i) & 1);
    if (carry || r >= divisor) { r -= divisor; q |= 1ull << i; }
  }
  (void)n;
  *quotient = q;
  *remainder = r;
  return TRUE;
}
EXPORT BOOLEAN NTAPI RtlGetIntegerAtom(PCWSTR name, USHORT *atom) {
  if ((ULONG_PTR)name <= 0xFFFF) { if (atom) *atom = (USHORT)(ULONG_PTR)name; return (ULONG_PTR)name != 0 && (ULONG_PTR)name < 0xC000; }
  if (name[0] != '#') return FALSE;
  ULONG value = 0;
  for (const WCHAR *p = name + 1; *p; ++p) { if (*p < '0' || *p > '9') return FALSE; value = value * 10 + (ULONG)(*p - '0'); if (value > 0xFFFF) return FALSE; }
  if (value == 0 || value >= 0xC000) return FALSE;
  if (atom) *atom = (USHORT)value;
  return TRUE;
}
EXPORT NTSTATUS NTAPI RtlInitializeAtomPackage(ULONG tag) { (void)tag; return STATUS_SUCCESS; }
EXPORT BOOLEAN NTAPI RtlComputeImportTableHash(HANDLE file, char *hash, ULONG version) { (void)file; (void)version; memset(hash, 0, 16); return FALSE; }
EXPORT PVOID NTAPI RtlGetFunctionTableListHead(void) { static LIST_ENTRY head = {&head, &head}; return &head; }
EXPORT PVOID NTAPI RtlAddressInSectionTable(PVOID nt_headers, PVOID base, ULONG rva) { return RtlImageRvaToVa(nt_headers, base, rva, NULL); }
EXPORT NTSTATUS NTAPI RtlSetEnvironmentStrings(WCHAR *strings, SIZE_T size) {
  if (!strings || size < 4) return STATUS_INVALID_PARAMETER;
  WCHAR *copy = RtlAllocateHeap(process_heap(), 0, size);
  if (!copy) return STATUS_NO_MEMORY;
  memcpy(copy, strings, size);
  PVOID old = NtCurrentPeb()->ProcessParameters->Environment;
  RtlAcquirePebLock();
  NtCurrentPeb()->ProcessParameters->Environment = copy;
  RtlReleasePebLock();
  if (old) RtlFreeHeap(process_heap(), 0, old);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlUnhandledExceptionFilter2(PEXCEPTION_POINTERS pointers, ULONG flags) {
  (void)pointers; (void)flags;
  return 0 /* EXCEPTION_CONTINUE_SEARCH */;
}
EXPORT LONG NTAPI RtlUnhandledExceptionFilter(PEXCEPTION_POINTERS pointers) { return (LONG)RtlUnhandledExceptionFilter2(pointers, 0); }
EXPORT LONG NTAPI RtlKnownExceptionFilter(PEXCEPTION_POINTERS pointers) { (void)pointers; return 0; }
EXPORT NTSTATUS NTAPI RtlReportException(EXCEPTION_RECORD *record, PCONTEXT context, ULONG flags) { (void)record; (void)context; (void)flags; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlReportExceptionEx(EXCEPTION_RECORD *record, PCONTEXT context, ULONG flags, PLARGE_INTEGER timeout) { (void)record; (void)context; (void)flags; (void)timeout; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlReportSilentProcessExit(HANDLE process, NTSTATUS exit_status) { (void)process; (void)exit_status; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlWerpReportException(ULONG pid, HANDLE process, PVOID record, PVOID context, ULONG flags, PHANDLE report) { (void)pid; (void)process; (void)record; (void)context; (void)flags; if (report) *report = NULL; return STATUS_NOT_IMPLEMENTED; }
EXPORT void NTAPI RtlRaiseNoncontinuableException(EXCEPTION_RECORD *record, PCONTEXT context) { (void)context; record->ExceptionFlags |= 1; RtlRaiseException(record); }
EXPORT BOOLEAN NTAPI RtlGuardCheckLongJumpTarget(PVOID target, BOOL is_export, PBOOL valid) { (void)target; (void)is_export; if (valid) *valid = TRUE; return TRUE; }
EXPORT NTSTATUS NTAPI RtlCheckProcessParameters(PVOID a, PVOID b, PVOID c, PVOID d) { (void)a; (void)b; (void)c; (void)d; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlCopyContext(PVOID destination, ULONG flags, PVOID source) {
  (void)flags;
  memcpy(destination, source, sizeof(CONTEXT));
  return STATUS_SUCCESS;
}
