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

/* ntdll.dll: services that are made of other services (the newer forms of the older calls, and calls that open a file to ask it something), so that the kernel needs no code for them.
 * Their numbered stubs are not generated (scripts/gen-windows-stubs.py lists them in src/Windows/ntdll/usermode.txt). Included by ntdll.c after rtlx3.inc. */

IMPORT NTSTATUS NTAPI NtContinue(CONTEXT *, BOOLEAN);
NTSTATUS NTAPI NtOpenDirectoryObject(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES attributes);
NTSTATUS NTAPI NtCreateDirectoryObject(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES attributes);
NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK iosb, PVOID information, ULONG length, ULONG information_class, BOOLEAN single, PUNICODE_STRING pattern, BOOLEAN restart);
NTSTATUS NTAPI NtReadFile(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK iosb, PVOID buffer, ULONG length, PLARGE_INTEGER offset, PULONG key);
NTSTATUS NTAPI NtWriteFile(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK iosb, PVOID buffer, ULONG length, PLARGE_INTEGER offset, PULONG key);

/* The file as a handle to read about: opened with no rights but reading its attributes, the way a path is asked about. */
static NTSTATUS wrap_open_for_query(POBJECT_ATTRIBUTES attributes, PHANDLE file) {
  IO_STATUS_BLOCK iosb;
  return NtOpenFile(file, 0x80 /* FILE_READ_ATTRIBUTES */ | 0x00100000, attributes, &iosb, 7, 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */ | 0x00004000 /* FILE_OPEN_FOR_BACKUP_INTENT */);
}
EXPORT NTSTATUS NTAPI NtQueryFullAttributesFile(POBJECT_ATTRIBUTES attributes, PVOID information) {
  HANDLE file = NULL;
  NTSTATUS status = wrap_open_for_query(attributes, &file);
  if (!NT_SUCCESS(status)) return status;
  IO_STATUS_BLOCK iosb;
  status = NtQueryInformationFile(file, &iosb, information, 56, 34 /* FileNetworkOpenInformation */);
  NtClose(file);
  return status;
}
EXPORT NTSTATUS NTAPI NtQueryInformationByName(POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK iosb, PVOID information, ULONG length, ULONG information_class) {
  HANDLE file = NULL;
  NTSTATUS status = wrap_open_for_query(attributes, &file);
  if (!NT_SUCCESS(status)) return status;
  status = NtQueryInformationFile(file, iosb, information, length, information_class);
  NtClose(file);
  return status;
}
/* NtQueryDirectoryFileEx's flags: 1 restart the scan, 2 return a single entry, 4 start at an index (not supported), 8 only entries that are on disk. */
EXPORT NTSTATUS NTAPI NtQueryDirectoryFileEx(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK iosb, PVOID information, ULONG length, ULONG information_class, ULONG flags, PUNICODE_STRING pattern) {
  if (flags & ~0xFu) return STATUS_INVALID_PARAMETER;
  if (flags & 4) return STATUS_NOT_IMPLEMENTED;
  return NtQueryDirectoryFile(file, event, apc, apc_context, iosb, information, length, information_class, (flags & 2) != 0, pattern, (flags & 1) != 0);
}
EXPORT NTSTATUS NTAPI NtFlushBuffersFileEx(HANDLE file, ULONG flags, PVOID parameters, ULONG length, PIO_STATUS_BLOCK iosb) {
  (void)parameters; (void)length;
  if (flags & ~7u) return STATUS_INVALID_PARAMETER;
  return NtFlushBuffersFile(file, iosb);
}
/* Scatter/gather: every element is a page; they are read or written one after the other (the file must be opened for it, and the calls are synchronous). */
static NTSTATUS wrap_scatter(BOOLEAN write, HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK iosb, const ULONGLONG *segments, ULONG length, PLARGE_INTEGER offset, PULONG key) {
  (void)apc; (void)apc_context;
  if (length == 0) { iosb->Status = STATUS_SUCCESS; iosb->Information = 0; if (event) NtSetEvent(event, NULL); return STATUS_SUCCESS; }
  ULONG_PTR total = 0;
  LARGE_INTEGER position = offset ? *offset : (LARGE_INTEGER){{0}};
  NTSTATUS status = STATUS_SUCCESS;
  for (ULONG done = 0; done < length && NT_SUCCESS(status); ) {
    const ULONG chunk = length - done < 0x1000 ? length - done : 0x1000;
    IO_STATUS_BLOCK part;
    PVOID buffer = (PVOID)(ULONG_PTR)segments[done / 0x1000];
    status = write ? NtWriteFile(file, NULL, NULL, NULL, &part, buffer, chunk, offset ? &position : NULL, key)
                   : NtReadFile(file, NULL, NULL, NULL, &part, buffer, chunk, offset ? &position : NULL, key);
    if (status == STATUS_PENDING) { NtWaitForSingleObject(file, FALSE, NULL); status = part.Status; }
    total += part.Information;
    position.QuadPart += (LONGLONG)part.Information;
    if (part.Information < chunk) break;
    done += chunk;
  }
  iosb->Status = status;
  iosb->Information = total;
  if (event) NtSetEvent(event, NULL);
  return status;
}
EXPORT NTSTATUS NTAPI NtReadFileScatter(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK iosb, const ULONGLONG *segments, ULONG length, PLARGE_INTEGER offset, PULONG key) {
  return wrap_scatter(FALSE, file, event, apc, apc_context, iosb, segments, length, offset, key);
}
EXPORT NTSTATUS NTAPI NtWriteFileGather(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK iosb, const ULONGLONG *segments, ULONG length, PLARGE_INTEGER offset, PULONG key) {
  return wrap_scatter(TRUE, file, event, apc, apc_context, iosb, segments, length, offset, key);
}
EXPORT NTSTATUS NTAPI NtNotifyChangeDirectoryFileEx(HANDLE file, HANDLE event, PVOID apc, PVOID apc_context, PIO_STATUS_BLOCK iosb, PVOID buffer, ULONG length, ULONG filter, ULONG flags, ULONG information_class) {
  (void)information_class;
  return NtNotifyChangeDirectoryFile(file, event, apc, apc_context, iosb, buffer, length, filter, (flags & 1) != 0);
}

/* Memory: the extended forms take parameters (address requirements, a preferred node) that are not used; a call without any is the plain call. */
typedef struct EXTENDED_PARAMETER_ { ULONGLONG type; ULONGLONG value; } EXTENDED_PARAMETER_;
static NTSTATUS wrap_check_extended(const EXTENDED_PARAMETER_ *parameters, ULONG count) {
  if (count && !parameters) return STATUS_INVALID_PARAMETER;
  for (ULONG i = 0; i < count; ++i) if ((parameters[i].type & 0xFF) < 1 || (parameters[i].type & 0xFF) > 8) return STATUS_INVALID_PARAMETER;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI NtMapViewOfSectionEx(HANDLE section, HANDLE process, PVOID *base, PLARGE_INTEGER offset, PSIZE_T size, ULONG type, ULONG protect, EXTENDED_PARAMETER_ *parameters, ULONG count) {
  NTSTATUS status = wrap_check_extended(parameters, count);
  if (!NT_SUCCESS(status)) return status;
  return NtMapViewOfSection(section, process, base, 0, 0, offset, size, 1 /* ViewShare */, type, protect);
}
EXPORT NTSTATUS NTAPI NtUnmapViewOfSectionEx(HANDLE process, PVOID base, ULONG flags) {
  if (flags & ~0x3u) return STATUS_INVALID_PARAMETER;
  return NtUnmapViewOfSection(process, base);
}
EXPORT NTSTATUS NTAPI NtCreateSectionEx(PHANDLE section, ULONG access, POBJECT_ATTRIBUTES attributes, PLARGE_INTEGER size, ULONG protect, ULONG allocation, HANDLE file, EXTENDED_PARAMETER_ *parameters, ULONG count) {
  NTSTATUS status = wrap_check_extended(parameters, count);
  if (!NT_SUCCESS(status)) return status;
  return NtCreateSection(section, access, attributes, size, protect, allocation, file);
}
EXPORT NTSTATUS NTAPI NtReadVirtualMemoryEx(HANDLE process, PVOID base, PVOID buffer, SIZE_T size, PSIZE_T read, ULONG flags) {
  if (flags & ~1u) return STATUS_INVALID_PARAMETER;
  return NtReadVirtualMemory(process, base, buffer, size, read);
}
/* NtContinueEx(PCONTEXT, PVOID Argument): the argument is a BOOLEAN for the test alert, or (when it is a pointer) a CONTINUE_ARGUMENT whose first word holds flags (bit 0: test alert). */
EXPORT NTSTATUS NTAPI NtContinueEx(PVOID context, PVOID argument) {
  BOOLEAN test_alert = FALSE;
  if ((ULONG_PTR)argument > 0xFF) test_alert = (*(const ULONG *)argument & 1) != 0; else test_alert = argument != NULL;
  return NtContinue((CONTEXT *)context, test_alert);
}
EXPORT NTSTATUS NTAPI NtCreateDirectoryObjectEx(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES attributes, HANDLE shadow, ULONG flags) {
  (void)shadow; (void)flags;
  return NtCreateDirectoryObject(handle, access, attributes);
}
/* A private namespace is a directory of objects whose name is the namespace's (the boundary descriptor is kept by the caller). */
EXPORT NTSTATUS NTAPI NtCreatePrivateNamespace(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES attributes, PVOID descriptor) {
  if (!descriptor) return STATUS_INVALID_PARAMETER;
  return NtCreateDirectoryObject(handle, access, attributes);
}
EXPORT NTSTATUS NTAPI NtOpenPrivateNamespace(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES attributes, PVOID descriptor) {
  if (!descriptor) return STATUS_INVALID_PARAMETER;
  return NtOpenDirectoryObject(handle, access, attributes);
}
EXPORT NTSTATUS NTAPI NtDeletePrivateNamespace(HANDLE handle) { return NtClose(handle); }

/* Boundary descriptors: a name and a list of the identifiers that may open the namespace. */
typedef struct BOUNDARY_ { ULONG count, capacity; BYTE *entries; WCHAR name[1]; } BOUNDARY_;
EXPORT PVOID NTAPI RtlCreateBoundaryDescriptor(const UNICODE_STRING *name, ULONG flags) {
  (void)flags;
  BOUNDARY_ *b = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, sizeof(*b) + name->Length);
  if (!b) return NULL;
  memcpy(b->name, name->Buffer, name->Length);
  return b;
}
EXPORT void NTAPI RtlDeleteBoundaryDescriptor(PVOID descriptor) { BOUNDARY_ *b = descriptor; if (b->entries) RtlFreeHeap(process_heap(), 0, b->entries); RtlFreeHeap(process_heap(), 0, b); }
static NTSTATUS boundary_add(BOUNDARY_ *b, const BYTE *sid, ULONG length) {
  BYTE *grown = RtlReAllocateHeap(process_heap(), 0, b->entries, b->capacity + length + 4);
  if (b->entries == NULL) grown = RtlAllocateHeap(process_heap(), 0, length + 4);
  if (!grown) return STATUS_NO_MEMORY;
  *(ULONG *)(grown + b->capacity) = length;
  memcpy(grown + b->capacity + 4, sid, length);
  b->entries = grown;
  b->capacity += length + 4;
  ++b->count;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAddSIDToBoundaryDescriptor(PVOID *descriptor, PVOID sid) { return boundary_add(*descriptor, sid, RtlLengthSid(sid)); }
EXPORT NTSTATUS NTAPI RtlAddIntegrityLabelToBoundaryDescriptor(PVOID *descriptor, PVOID sid) { return boundary_add(*descriptor, sid, RtlLengthSid(sid)); }

/* ---- ALPC message attributes: a header of two masks followed by the attributes that are allocated, the one with the highest bit first ---- */

EXPORT SIZE_T NTAPI AlpcGetHeaderSize(ULONG flags) {
  SIZE_T size = 8;      /* ALPC_MESSAGE_ATTRIBUTES: AllocatedAttributes and ValidAttributes */
  if (flags & 0x80000000u) size += 24;      /* security */
  if (flags & 0x40000000u) size += 32;      /* data view */
  if (flags & 0x20000000u) size += 32;      /* context */
  if (flags & 0x10000000u) size += 24;      /* handle */
  if (flags & 0x08000000u) size += 24;      /* token */
  if (flags & 0x04000000u) size += 8;       /* direct */
  if (flags & 0x02000000u) size += 8;       /* work on behalf of */
  return size;
}
EXPORT NTSTATUS NTAPI AlpcInitializeMessageAttribute(ULONG attributes, PVOID message, SIZE_T buffer_size, SIZE_T *required) {
  const SIZE_T size = AlpcGetHeaderSize(attributes);
  if (required) *required = size;
  if (buffer_size < size) return (NTSTATUS)0xC0000023;      /* STATUS_BUFFER_TOO_SMALL */
  if (message) { ((ULONG *)message)[0] = attributes; ((ULONG *)message)[1] = 0; }
  return STATUS_SUCCESS;
}
EXPORT PVOID NTAPI AlpcGetMessageAttribute(PVOID message, ULONG attribute) {
  if (!attribute || (attribute & (attribute - 1)) || !(attribute & 0xFE000000u)) return NULL;
  const ULONG allocated = ((const ULONG *)message)[0];
  if (!(allocated & attribute)) return NULL;
  return (BYTE *)message + AlpcGetHeaderSize(allocated & ~(attribute | (attribute - 1)));
}
EXPORT ULONG NTAPI AlpcMaxAllowedMessageLength(void) { return 0x7FFFFFFFu; }

/* ---- WNF state: the Rtl calls over the services (a state name is passed by value: it is 64 bits) ---- */

NTSTATUS NTAPI NtUpdateWnfStateData(const ULONGLONG *name, const void *buffer, ULONG length, const void *type_id, const void *scope, ULONG matching, ULONG check);
NTSTATUS NTAPI NtQueryWnfStateData(const ULONGLONG *name, const void *type_id, const void *scope, ULONG *stamp, void *buffer, ULONG *size);
typedef NTSTATUS (NTAPI *WNF_USER_CALLBACK_)(ULONGLONG name, ULONG stamp, PVOID type_id, PVOID context, const void *buffer, ULONG size);
EXPORT NTSTATUS NTAPI RtlPublishWnfStateData(ULONGLONG name, const void *type_id, const void *buffer, ULONG length, const void *scope) { return NtUpdateWnfStateData(&name, buffer, length, type_id, scope, 0, 0); }
EXPORT NTSTATUS NTAPI RtlTestAndPublishWnfStateData(ULONGLONG name, const void *type_id, const void *buffer, ULONG length, const void *scope, ULONG matching, ULONG check) { return NtUpdateWnfStateData(&name, buffer, length, type_id, scope, matching, check); }
EXPORT NTSTATUS NTAPI RtlQueryWnfStateData(ULONG *stamp, ULONGLONG name, WNF_USER_CALLBACK_ callback, PVOID context, PVOID type_id) {
  BYTE data[1024];
  ULONG size = sizeof(data), change = 0;
  const NTSTATUS status = NtQueryWnfStateData(&name, type_id, NULL, &change, data, &size);
  if (!NT_SUCCESS(status)) return status;
  if (stamp) *stamp = change;
  return callback ? callback(name, change, type_id, context, data, size) : STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlQueryWnfStateDataWithExplicitScope(ULONG *stamp, ULONGLONG name, WNF_USER_CALLBACK_ callback, PVOID context, PVOID type_id, const void *scope) { (void)scope; return RtlQueryWnfStateData(stamp, name, callback, context, type_id); }
EXPORT LONG NTAPI RtlWnfCompareChangeStamp(ULONG a, ULONG b) { return a < b ? -1 : (a > b ? 1 : 0); }
EXPORT BOOLEAN NTAPI RtlEqualWnfChangeStamps(ULONG a, ULONG b) { return a == b; }
EXPORT void NTAPI RtlWnfDllUnloadCallback(PVOID module) { (void)module; }
