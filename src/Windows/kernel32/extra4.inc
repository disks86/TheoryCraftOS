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
/* More of kernel32 (included by kernel32.c after appmodel.inc): WaitOnAddress, resource enumeration, memory functions with NUMA and extended parameters, working set and
   mitigation settings, processor groups and CPU sets, and the small functions of a machine that has one NUMA node, one processor group and no tape drive, firmware variables
   or packages. */

typedef char *LPCH_;
typedef unsigned int UINT32;
typedef const WCHAR *PCZZWSTR_;
typedef WCHAR *PZZWSTR_;
typedef long long LONG64_;
typedef const WCHAR *PCWSTR;
typedef WCHAR *PWSTR;
typedef USHORT *PUSHORT_;
typedef LPSECURITY_ATTRIBUTES PSECURITY_ATTRIBUTES_;
IMPORT NTSTATUS NTAPI NtCreateDirectoryObject(PHANDLE, ULONG, POBJECT_ATTRIBUTES);
IMPORT NTSTATUS NTAPI NtOpenDirectoryObject(PHANDLE, ULONG, POBJECT_ATTRIBUTES);
IMPORT NTSTATUS NTAPI NtAllocateVirtualMemoryEx(HANDLE, PVOID *, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
IMPORT NTSTATUS NTAPI RtlWaitOnAddress(volatile VOID *, PVOID, SIZE_T, PLARGE_INTEGER);
IMPORT VOID NTAPI RtlWakeAddressSingle(PVOID);
IMPORT VOID NTAPI RtlWakeAddressAll(PVOID);
#ifndef ERROR_TIMEOUT
#define ERROR_TIMEOUT 1460
#endif
#ifndef ERROR_NOT_SUPPORTED
#define ERROR_NOT_SUPPORTED 50
#endif
#ifndef ERROR_INSUFFICIENT_BUFFER
#define ERROR_INSUFFICIENT_BUFFER 122
#endif
#ifndef ERROR_CALL_NOT_IMPLEMENTED
#define ERROR_CALL_NOT_IMPLEMENTED 120
#endif
#define ERROR_APPMODEL_NO_PACKAGE 15700
#ifndef ERROR_LABEL_TOO_LONG
#define ERROR_LABEL_TOO_LONG 154
#endif
#define ERROR_NO_SYSTEM_RESOURCES 1450

/* ---- WaitOnAddress -------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI WaitOnAddress(volatile VOID *address, PVOID compare, SIZE_T size, DWORD milliseconds) {
  LARGE_INTEGER timeout;
  if (!address || !compare) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (milliseconds != 0xFFFFFFFFu) timeout.QuadPart = -(LONGLONG)milliseconds * 10000;
  const NTSTATUS status = RtlWaitOnAddress(address, compare, size, milliseconds == 0xFFFFFFFFu ? NULL : &timeout);
  if (status == 0x102) { set_error(ERROR_TIMEOUT); return FALSE; }
  if (!NT_SUCCESS(status)) return fail_nt(status);
  return TRUE;
}
EXPORT void WINAPI WakeByAddressSingle(PVOID address) { RtlWakeAddressSingle(address); }
EXPORT void WINAPI WakeByAddressAll(PVOID address) { RtlWakeAddressAll(address); }

/* ---- resources: enumeration ----------------------------------------------------------------------------------------------------------------------------- */

typedef BOOL (CALLBACK *ENUMRESTYPEPROCW_)(HMODULE, LPWSTR, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESNAMEPROCW_)(HMODULE, LPCWSTR, LPWSTR, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESLANGPROCW_)(HMODULE, LPCWSTR, LPCWSTR, WORD, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESTYPEPROCA_)(HMODULE, LPSTR, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESNAMEPROCA_)(HMODULE, LPCSTR, LPSTR, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESLANGPROCA_)(HMODULE, LPCSTR, LPCSTR, WORD, LONG_PTR);
typedef enum { ENUM_TYPES = 0, ENUM_NAMES = 1, ENUM_LANGS = 2 } EnumLevel;
typedef struct {
  EnumLevel level;
  BOOL ansi;
  void *callback;
  LONG_PTR param;
  HMODULE module;
  LPCWSTR type, name;
} EnumState;
/* The name of an entry as the callbacks get it: an integer id as a MAKEINTRESOURCE value, a named entry as a string (counted in the file, ended here). The text is freed by
 * the caller. */
static void *enum_entry_name(const ResourceDirectory *root, const ResourceEntry *e, BOOL ansi, void **to_free) {
  *to_free = NULL;
  if (!(e->Name & 0x80000000u)) return (void *)(ULONG_PTR)(e->Name & 0xFFFF);
  const WORD *s = (const WORD *)((const BYTE *)root + (e->Name & 0x7FFFFFFF));
  const WORD length = s[0];
  WCHAR *wide = RtlAllocateHeap(heap(), 0, ((SIZE_T)length + 1) * sizeof(WCHAR));
  if (!wide) return NULL;
  for (WORD i = 0; i < length; ++i) wide[i] = s[1 + i];
  wide[length] = 0;
  if (!ansi) { *to_free = wide; return wide; }
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
  char *narrow = bytes > 0 ? RtlAllocateHeap(heap(), 0, (SIZE_T)bytes) : NULL;
  if (narrow) WideCharToMultiByte(CP_ACP, 0, wide, -1, narrow, bytes, NULL, NULL);
  RtlFreeHeap(heap(), 0, wide);
  *to_free = narrow;
  return narrow;
}
static BOOL enum_walk(const EnumState *st, const ResourceDirectory *root, const ResourceDirectory *dir) {
  const ResourceEntry *entries = (const ResourceEntry *)(dir + 1);
  const unsigned count = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
  for (unsigned i = 0; i < count; ++i) {
    void *free_me, *name = enum_entry_name(root, &entries[i], st->ansi, &free_me);
    if ((entries[i].Name & 0x80000000u) && !name) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    BOOL go_on;
    if (st->level == ENUM_TYPES) go_on = st->ansi ? ((ENUMRESTYPEPROCA_)st->callback)(st->module, name, st->param) : ((ENUMRESTYPEPROCW_)st->callback)(st->module, name, st->param);
    else if (st->level == ENUM_NAMES) go_on = st->ansi ? ((ENUMRESNAMEPROCA_)st->callback)(st->module, (LPCSTR)st->type, name, st->param) : ((ENUMRESNAMEPROCW_)st->callback)(st->module, st->type, name, st->param);
    else go_on = st->ansi ? ((ENUMRESLANGPROCA_)st->callback)(st->module, (LPCSTR)st->type, (LPCSTR)st->name, (WORD)(ULONG_PTR)name, st->param) : ((ENUMRESLANGPROCW_)st->callback)(st->module, st->type, st->name, (WORD)(ULONG_PTR)name, st->param);
    if (free_me) RtlFreeHeap(heap(), 0, free_me);
    if (!go_on) return FALSE;
  }
  return TRUE;
}
/* The directory below `parent` that the entry `key` (an id or a name) leads to. */
static const ResourceDirectory *enum_child(const ResourceDirectory *root, const ResourceDirectory *parent, LPCWSTR key, BOOL ansi, DWORD missing_error) {
  WCHAR *wide = NULL;
  if (ansi && ((ULONG_PTR)key >> 16)) { wide = to_wide((LPCSTR)key); if (!wide) return NULL; key = wide; }
  const ResourceEntry *e = resource_find(root, parent, key);
  free_wide(wide);
  if (!e || !(e->OffsetToData & 0x80000000u)) { set_error(missing_error); return NULL; }
  return (const ResourceDirectory *)((const BYTE *)root + (e->OffsetToData & 0x7FFFFFFF));
}
static BOOL enum_resources(EnumLevel level, BOOL ansi, HMODULE module, LPCWSTR type, LPCWSTR name, void *callback, LONG_PTR param, DWORD flags, WORD language) {
  (void)flags; (void)language;
  if (!callback) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  DWORD size = 0;
  const ResourceDirectory *root = resource_root(module, &size);
  if (!root) { set_error(1812 /* ERROR_RESOURCE_DATA_NOT_FOUND */); return FALSE; }
  EnumState st = {level, ansi, callback, param, module, type, name};
  const ResourceDirectory *dir = root;
  if (level >= ENUM_NAMES) { dir = enum_child(root, root, type, ansi, 1813); if (!dir) return FALSE; }
  if (level == ENUM_LANGS) { dir = enum_child(root, dir, name, ansi, 1814); if (!dir) return FALSE; }
  enum_walk(&st, root, dir);
  return TRUE;      /* (a callback that stops the walk is not an error) */
}
EXPORT BOOL WINAPI EnumResourceTypesExW(HMODULE m, ENUMRESTYPEPROCW_ proc, LONG_PTR param, DWORD flags, LANGID lang) { return enum_resources(ENUM_TYPES, FALSE, m, NULL, NULL, (void *)proc, param, flags, lang); }
EXPORT BOOL WINAPI EnumResourceTypesExA(HMODULE m, ENUMRESTYPEPROCA_ proc, LONG_PTR param, DWORD flags, LANGID lang) { return enum_resources(ENUM_TYPES, TRUE, m, NULL, NULL, (void *)proc, param, flags, lang); }
EXPORT BOOL WINAPI EnumResourceTypesW(HMODULE m, ENUMRESTYPEPROCW_ proc, LONG_PTR param) { return EnumResourceTypesExW(m, proc, param, 0, 0); }
EXPORT BOOL WINAPI EnumResourceTypesA(HMODULE m, ENUMRESTYPEPROCA_ proc, LONG_PTR param) { return EnumResourceTypesExA(m, proc, param, 0, 0); }
EXPORT BOOL WINAPI EnumResourceNamesExW(HMODULE m, LPCWSTR type, ENUMRESNAMEPROCW_ proc, LONG_PTR param, DWORD flags, LANGID lang) { return enum_resources(ENUM_NAMES, FALSE, m, type, NULL, (void *)proc, param, flags, lang); }
EXPORT BOOL WINAPI EnumResourceNamesExA(HMODULE m, LPCSTR type, ENUMRESNAMEPROCA_ proc, LONG_PTR param, DWORD flags, LANGID lang) { return enum_resources(ENUM_NAMES, TRUE, m, (LPCWSTR)type, NULL, (void *)proc, param, flags, lang); }
EXPORT BOOL WINAPI EnumResourceNamesW(HMODULE m, LPCWSTR type, ENUMRESNAMEPROCW_ proc, LONG_PTR param) { return EnumResourceNamesExW(m, type, proc, param, 0, 0); }
EXPORT BOOL WINAPI EnumResourceNamesA(HMODULE m, LPCSTR type, ENUMRESNAMEPROCA_ proc, LONG_PTR param) { return EnumResourceNamesExA(m, type, proc, param, 0, 0); }
EXPORT BOOL WINAPI EnumResourceLanguagesExW(HMODULE m, LPCWSTR type, LPCWSTR name, ENUMRESLANGPROCW_ proc, LONG_PTR param, DWORD flags, LANGID lang) { return enum_resources(ENUM_LANGS, FALSE, m, type, name, (void *)proc, param, flags, lang); }
EXPORT BOOL WINAPI EnumResourceLanguagesExA(HMODULE m, LPCSTR type, LPCSTR name, ENUMRESLANGPROCA_ proc, LONG_PTR param, DWORD flags, LANGID lang) { return enum_resources(ENUM_LANGS, TRUE, m, (LPCWSTR)type, (LPCWSTR)name, (void *)proc, param, flags, lang); }
EXPORT BOOL WINAPI EnumResourceLanguagesW(HMODULE m, LPCWSTR type, LPCWSTR name, ENUMRESLANGPROCW_ proc, LONG_PTR param) { return EnumResourceLanguagesExW(m, type, name, proc, param, 0, 0); }
EXPORT BOOL WINAPI EnumResourceLanguagesA(HMODULE m, LPCSTR type, LPCSTR name, ENUMRESLANGPROCA_ proc, LONG_PTR param) { return EnumResourceLanguagesExA(m, type, name, proc, param, 0, 0); }

/* ---- file API code page, binary types, environment ------------------------------------------------------------------------------------------------------ */

static volatile LONG g_file_apis_oem;
EXPORT BOOL WINAPI AreFileApisANSI(void) { return !g_file_apis_oem; }
EXPORT void WINAPI SetFileApisToANSI(void) { g_file_apis_oem = 0; }
EXPORT void WINAPI SetFileApisToOEM(void) { g_file_apis_oem = 1; }

#define SCS_32BIT_BINARY 0
#define SCS_DOS_BINARY 1
#define SCS_WOW_BINARY 2
#define SCS_64BIT_BINARY 6
EXPORT BOOL WINAPI GetBinaryTypeW(LPCWSTR path, LPDWORD type) {
  if (!path || !type) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  HANDLE file = CreateFileW(path, 0x80000000u /* GENERIC_READ */, 1 | 2, NULL, 3 /* OPEN_EXISTING */, 0, NULL);
  if (file == INVALID_HANDLE_VALUE) return FALSE;
  BYTE head[64], nt[24];
  DWORD got = 0;
  BOOL ok = FALSE;
  if (ReadFile(file, head, sizeof(head), &got, NULL) && got == sizeof(head) && head[0] == 'M' && head[1] == 'Z') {
    const DWORD pe = *(DWORD *)(head + 0x3C);
    if (SetFilePointer(file, (LONG)pe, NULL, 0) != INVALID_SET_FILE_POINTER && ReadFile(file, nt, sizeof(nt), &got, NULL) && got == sizeof(nt) && nt[0] == 'P' && nt[1] == 'E' && !nt[2] && !nt[3]) {
      const WORD machine = *(WORD *)(nt + 4), characteristics = *(WORD *)(nt + 22);
      if (!(characteristics & 0x0002)) set_error(193 /* ERROR_BAD_EXE_FORMAT: not an executable image */);
      else if (machine == 0x8664 || machine == 0xAA64) { *type = SCS_64BIT_BINARY; ok = TRUE; }
      else if (machine == 0x14C) { *type = SCS_32BIT_BINARY; ok = TRUE; }
      else set_error(193);
    } else {
      *type = SCS_DOS_BINARY;      /* an MZ file with no PE header after it is a DOS program */
      ok = TRUE;
    }
  } else if (got != 0 || GetLastError() == 0) {
    set_error(193);
  }
  CloseHandle(file);
  return ok;
}
EXPORT BOOL WINAPI GetBinaryTypeA(LPCSTR path, LPDWORD type) {
  WCHAR *w = path ? to_wide(path) : NULL;
  if (!w) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const BOOL ok = GetBinaryTypeW(w, type);
  free_wide(w);
  return ok;
}
EXPORT BOOL WINAPI GetBinaryType(LPCSTR path, LPDWORD type) { return GetBinaryTypeA(path, type); }

EXPORT BOOL WINAPI SetEnvironmentStringsA(LPCH_ block) {
  if (!block) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  SIZE_T length = 0;
  while (block[length] || block[length + 1]) ++length;
  length += 2;      /* (the last string's end and the block's) */
  const int wide_count = MultiByteToWideChar(CP_ACP, 0, block, (int)length, NULL, 0);
  WCHAR *wide = wide_count > 0 ? RtlAllocateHeap(heap(), 0, (SIZE_T)wide_count * sizeof(WCHAR)) : NULL;
  if (!wide) { set_error(wide_count > 0 ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return FALSE; }
  MultiByteToWideChar(CP_ACP, 0, block, (int)length, wide, wide_count);
  const BOOL ok = SetEnvironmentStringsW(wide);
  RtlFreeHeap(heap(), 0, wide);
  return ok;
}

/* ---- memory: NUMA, extended parameters, write watch ----------------------------------------------------------------------------------------------------- */

#define MEM_WRITE_WATCH_ 0x00200000
#define WRITE_WATCH_FLAG_RESET_ 1
EXPORT SIZE_T WINAPI GetLargePageMinimum(void) { return 2u * 1024 * 1024; }
EXPORT LPVOID WINAPI VirtualAlloc2(HANDLE process, PVOID base, SIZE_T size, ULONG type, ULONG protect, PVOID parameters, ULONG count) {
  PVOID address = base;
  SIZE_T bytes = size;
  const NTSTATUS status = NtAllocateVirtualMemoryEx(process ? process : NtCurrentProcess(), &address, &bytes, type, protect, parameters, count);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  if ((type & MEM_WRITE_WATCH_) && (!process || process == NtCurrentProcess())) watch_note(address, bytes);
  return address;
}
EXPORT LPVOID WINAPI VirtualAlloc2FromApp(HANDLE process, PVOID base, SIZE_T size, ULONG type, ULONG protect, PVOID parameters, ULONG count) {
  if (protect == 0x40 || protect == 0x20 || protect == 0x80 || protect == 0x10) { set_error(ERROR_INVALID_PARAMETER); return NULL; }      /* (no executable pages for a Store app) */
  return VirtualAlloc2(process, base, size, type, protect, parameters, count);
}
EXPORT LPVOID WINAPI VirtualAllocExNuma(HANDLE process, LPVOID address, SIZE_T size, DWORD type, DWORD protect, DWORD node) {
  if (node != 0 && node != 0xFFFFFFFFu) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  return VirtualAllocEx(process, address, size, type, protect);
}
EXPORT LPVOID WINAPI VirtualAllocFromApp(PVOID base, SIZE_T size, ULONG type, ULONG protect) { return VirtualAlloc2FromApp(NtCurrentProcess(), base, size, type, protect, NULL, 0); }
EXPORT BOOL WINAPI AllocateUserPhysicalPages(HANDLE process, PULONG_PTR pages, PULONG_PTR array) { (void)process; (void)pages; (void)array; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI AllocateUserPhysicalPagesNuma(HANDLE process, PULONG_PTR pages, PULONG_PTR array, DWORD node) { (void)node; return AllocateUserPhysicalPages(process, pages, array); }
EXPORT BOOL WINAPI FreeUserPhysicalPages(HANDLE process, PULONG_PTR pages, PULONG_PTR array) { (void)process; (void)pages; (void)array; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI MapUserPhysicalPages(PVOID address, ULONG_PTR pages, PULONG_PTR array) { (void)address; (void)pages; (void)array; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI MapUserPhysicalPagesScatter(PVOID *addresses, ULONG_PTR pages, PULONG_PTR array) { (void)addresses; (void)pages; (void)array; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT LPVOID WINAPI MapViewOfFileExNuma(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T bytes, LPVOID base, DWORD node) {
  if (node != 0 && node != 0xFFFFFFFFu) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  return MapViewOfFileEx(mapping, access, offset_high, offset_low, bytes, base);
}
EXPORT LPVOID WINAPI MapViewOfFileFromApp(HANDLE mapping, ULONG access, ULONG64 offset, SIZE_T bytes) { return MapViewOfFileEx(mapping, access, (DWORD)(offset >> 32), (DWORD)offset, bytes, NULL); }
EXPORT BOOL WINAPI UnmapViewOfFileEx(PVOID base, ULONG flags) {
  if (flags & ~(ULONG)0x3) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }      /* MEM_UNMAP_WITH_TRANSIENT_BOOST, MEM_PRESERVE_PLACEHOLDER */
  return UnmapViewOfFile(base);
}
EXPORT HANDLE WINAPI CreateFileMappingFromApp(HANDLE file, PSECURITY_ATTRIBUTES_ sa, ULONG protect, ULONG64 size, PCWSTR name) {
  return CreateFileMappingW(file, sa, protect, (DWORD)(size >> 32), (DWORD)size, name);
}
EXPORT HANDLE WINAPI CreateFileMappingNumaW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD high, DWORD low, LPCWSTR name, DWORD node) {
  if (node != 0 && node != 0xFFFFFFFFu) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  return CreateFileMappingW(file, sa, protect, high, low, name);
}
EXPORT HANDLE WINAPI CreateFileMappingNumaA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD high, DWORD low, LPCSTR name, DWORD node) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (name && !w) return NULL;
  HANDLE h = CreateFileMappingNumaW(file, sa, protect, high, low, w, node);
  free_wide(w);
  return h;
}
EXPORT HANDLE WINAPI CreateSemaphoreExA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name, DWORD flags, DWORD access) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (name && !w) return NULL;
  HANDLE h = CreateSemaphoreExW(sa, initial, maximum, w, flags, access);
  free_wide(w);
  return h;
}

/* Offer, reclaim, discard: a machine with no memory to give back to anyone treats offered pages as still its own. */
EXPORT DWORD WINAPI OfferVirtualMemory(PVOID address, SIZE_T size, DWORD priority) {
  MEMORY_BASIC_INFORMATION info;
  if (!address || !size || priority < 1 || priority > 5 || ((ULONG_PTR)address & 0xFFF) || (size & 0xFFF)) return ERROR_INVALID_PARAMETER;
  if (!VirtualQuery(address, &info, sizeof(info)) || info.State != 0x1000) return ERROR_INVALID_PARAMETER;
  return 0;
}
EXPORT DWORD WINAPI ReclaimVirtualMemory(PVOID address, SIZE_T size) {
  MEMORY_BASIC_INFORMATION info;
  if (!address || !size || ((ULONG_PTR)address & 0xFFF) || (size & 0xFFF)) return ERROR_INVALID_PARAMETER;
  if (!VirtualQuery(address, &info, sizeof(info)) || info.State != 0x1000) return ERROR_INVALID_PARAMETER;
  return 0;
}
EXPORT DWORD WINAPI DiscardVirtualMemory(PVOID address, SIZE_T size) {
  if (!address || !size) return ERROR_INVALID_PARAMETER;
  return VirtualAlloc(address, size, 0x80000 /* MEM_RESET */, 0x04 /* PAGE_READWRITE */) ? 0 : GetLastError();
}

/* Write watch: the regions allocated with MEM_WRITE_WATCH are remembered; the pages that were written are not tracked, so every page of a watched range is reported as written
 * (a superset, which is what a collector that rescans the reported pages needs) until the watch is reset. */
typedef struct { ULONG_PTR base; SIZE_T size; } WatchRegion;
static WatchRegion g_watched[32];
static volatile LONG g_watched_lock;
static void watched_lock(void) { while (__sync_lock_test_and_set(&g_watched_lock, 1)) { while (g_watched_lock) __asm__ volatile("pause"); } }
static void watched_unlock(void) { __sync_lock_release(&g_watched_lock); }
static void watch_note(LPVOID base, SIZE_T size) {
  MEMORY_BASIC_INFORMATION info;
  if (!base || !VirtualQuery(base, &info, sizeof(info))) return;
  watched_lock();
  for (int i = 0; i < 32; ++i) if (!g_watched[i].base) { g_watched[i].base = (ULONG_PTR)info.AllocationBase; g_watched[i].size = size; break; }
  watched_unlock();
}
static void watch_forget(LPVOID base) {
  watched_lock();
  for (int i = 0; i < 32; ++i) if (g_watched[i].base == (ULONG_PTR)base) g_watched[i].base = 0;
  watched_unlock();
}
static BOOL watch_covers(ULONG_PTR address, SIZE_T size) {
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery((LPCVOID)address, &info, sizeof(info))) return FALSE;
  BOOL found = FALSE;
  watched_lock();
  for (int i = 0; i < 32; ++i) if (g_watched[i].base == (ULONG_PTR)info.AllocationBase) found = TRUE;
  watched_unlock();
  return found && size != 0;
}
EXPORT UINT WINAPI GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addresses, ULONG_PTR *count, ULONG *granularity) {
  if ((flags & ~(DWORD)WRITE_WATCH_FLAG_RESET_) || !count || !size || ((ULONG_PTR)base + size < (ULONG_PTR)base) || (*count && !addresses)) { set_error(ERROR_INVALID_PARAMETER); return (UINT)-1; }
  const ULONG_PTR first = (ULONG_PTR)base & ~(ULONG_PTR)0xFFF, last = ((ULONG_PTR)base + size + 0xFFF) & ~(ULONG_PTR)0xFFF;
  if (!watch_covers(first, size)) { set_error(ERROR_INVALID_PARAMETER); return (UINT)-1; }
  ULONG_PTR n = 0;
  for (ULONG_PTR page = first; page < last && n < *count; page += 0x1000) addresses[n++] = (PVOID)page;
  *count = n;
  if (granularity) *granularity = 0x1000;
  return 0;
}
EXPORT UINT WINAPI ResetWriteWatch(LPVOID base, SIZE_T size) {
  if (!size || ((ULONG_PTR)base & 0xFFF) || (size & 0xFFF) || !watch_covers((ULONG_PTR)base, size)) { set_error(ERROR_INVALID_PARAMETER); return (UINT)-1; }
  return 0;
}

/* ---- NUMA (one node), processor groups (one group) -------------------------------------------------------------------------------------------------------- */

static ULONGLONG numa_mask(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors >= 64 ? ~(ULONGLONG)0 : (((ULONGLONG)1 << si.dwNumberOfProcessors) - 1);
}
EXPORT BOOL WINAPI GetNumaNodeProcessorMask(UCHAR node, PULONGLONG mask) {
  if (!mask) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (node != 0) { *mask = 0; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *mask = numa_mask();
  return TRUE;
}
EXPORT BOOL WINAPI GetNumaNodeProcessorMaskEx(USHORT node, GROUP_AFFINITY *affinity) {
  if (!affinity || node != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  affinity->Mask = (ULONG_PTR)numa_mask();
  affinity->Group = 0;
  affinity->Reserved[0] = affinity->Reserved[1] = affinity->Reserved[2] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetNumaNodeProcessorMask2(USHORT node, GROUP_AFFINITY *affinities, USHORT capacity, USHORT *count) {
  if (!count || node != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (capacity < 1 || !affinities) { *count = 1; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  *count = 1;
  return GetNumaNodeProcessorMaskEx(0, &affinities[0]);
}
EXPORT BOOL WINAPI GetNumaAvailableMemoryNode(UCHAR node, PULONGLONG bytes) {
  MEMORYSTATUSEX m;
  m.dwLength = sizeof(m);
  if (!bytes || node != 0 || !GlobalMemoryStatusEx(&m)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *bytes = m.ullAvailPhys;
  return TRUE;
}
EXPORT BOOL WINAPI GetNumaAvailableMemoryNodeEx(USHORT node, PULONGLONG bytes) { return GetNumaAvailableMemoryNode(node > 255 ? 255 : (UCHAR)node, bytes); }
EXPORT BOOL WINAPI GetNumaProximityNode(ULONG proximity, PUCHAR node) {
  if (!node || proximity != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *node = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetNumaProximityNodeEx(ULONG proximity, PUSHORT_ node) {
  if (!node || proximity != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *node = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetNumaNodeNumberFromHandle(HANDLE file, PUSHORT_ node) {
  if (!node) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (GetFileType(file) == 0 /* FILE_TYPE_UNKNOWN */ && GetLastError() != 0) return FALSE;
  *node = 0;
  return TRUE;
}
EXPORT void WINAPI GetCurrentProcessorNumberEx(PROCESSOR_NUMBER_ *number) {
  number->Group = 0;
  number->Number = (BYTE)GetCurrentProcessorNumber();
  number->Reserved = 0;
}
EXPORT BOOL WINAPI GetProcessGroupAffinity(HANDLE process, PUSHORT_ count, PUSHORT_ groups) {
  DWORD_PTR a, b;
  if (!count || !GetProcessAffinityMask(process, &a, &b)) { if (!count) set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (*count < 1 || !groups) { *count = 1; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  *count = 1;
  groups[0] = 0;
  return TRUE;
}

/* ---- what a program keeps about itself: working set, shutdown order, mitigations, CPU sets, memory priority, power throttling ------------------------------------------ */

typedef struct {
  volatile LONG lock;
  SIZE_T ws_min, ws_max;
  DWORD ws_flags;
  DWORD shutdown_level, shutdown_flags;
  DWORD affinity_update_mode;
  DWORD mitigation[20][2];
  DWORD memory_priority;
  DWORD throttle_control, throttle_state;
  DWORD leap_second_flags;
  DWORD protection_level;
  DWORD default_cpu_sets[64];
  DWORD default_cpu_set_count;
  DWORD execution_state;
} SelfState;
static SelfState g_self = {0, 204800, 1413120, 0x2, 0x280, 0, 0, {{0}}, 5, 0, 0, 0, 0, {0}, 0, 0x80000000u};
static void self_lock(void) { while (__sync_lock_test_and_set(&g_self.lock, 1)) { while (g_self.lock) __asm__ volatile("pause"); } }
static void self_unlock(void) { __sync_lock_release(&g_self.lock); }
/* TRUE if `process` is a handle to a process; *self says whether it is this one. */
static BOOL process_handle_is(HANDLE process, BOOL *self) {
  if (process == NtCurrentProcess() || process == (HANDLE)(LONG_PTR)0) { *self = TRUE; return TRUE; }
  const DWORD id = GetProcessId(process);
  if (!id) return FALSE;
  *self = id == GetCurrentProcessId();
  return TRUE;
}

EXPORT BOOL WINAPI GetProcessWorkingSetSizeEx(HANDLE process, PSIZE_T minimum, PSIZE_T maximum, PDWORD flags) {
  BOOL self;
  if (!minimum || !maximum || !flags) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!process_handle_is(process, &self)) return FALSE;
  self_lock();
  *minimum = self ? g_self.ws_min : 204800;
  *maximum = self ? g_self.ws_max : 1413120;
  *flags = self ? g_self.ws_flags : 2;      /* QUOTA_LIMITS_HARDWS_MIN_DISABLE */
  self_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI GetProcessWorkingSetSize(HANDLE process, PSIZE_T minimum, PSIZE_T maximum) {
  DWORD flags;
  return GetProcessWorkingSetSizeEx(process, minimum, maximum, &flags);
}
EXPORT BOOL WINAPI SetProcessWorkingSetSizeEx(HANDLE process, SIZE_T minimum, SIZE_T maximum, DWORD flags) {
  BOOL self;
  if (!process_handle_is(process, &self)) return FALSE;
  if (minimum == (SIZE_T)-1 && maximum == (SIZE_T)-1) return TRUE;      /* (trim the working set: there is nothing to give back) */
  if ((minimum == 0) != (maximum == 0) || minimum > maximum || (flags & ~(DWORD)0xF) || ((flags & 1) && (flags & 2)) || ((flags & 4) && (flags & 8))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!self) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  if (minimum != 0) {
    self_lock();
    g_self.ws_min = minimum < 204800 ? 204800 : minimum;
    g_self.ws_max = maximum;
    g_self.ws_flags = flags;
    self_unlock();
  }
  return TRUE;
}
EXPORT BOOL WINAPI SetProcessWorkingSetSize(HANDLE process, SIZE_T minimum, SIZE_T maximum) { return SetProcessWorkingSetSizeEx(process, minimum, maximum, 0); }
EXPORT BOOL WINAPI GetProcessIoCounters(HANDLE process, PVOID counters) {
  if (!counters) { set_error(ERROR_NOACCESS); return FALSE; }
  const NTSTATUS status = NtQueryInformationProcess(process, 2 /* ProcessIoCounters */, counters, 48, NULL);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI GetProcessShutdownParameters(LPDWORD level, LPDWORD flags) {
  if (!level || !flags) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  self_lock();
  *level = g_self.shutdown_level;
  *flags = g_self.shutdown_flags;
  self_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI QueryProcessAffinityUpdateMode(HANDLE process, LPDWORD flags) {
  BOOL self;
  if (!flags || !process_handle_is(process, &self)) { if (!flags) set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  self_lock();
  *flags = self ? g_self.affinity_update_mode : 0;
  self_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetProcessAffinityUpdateMode(HANDLE process, DWORD flags) {
  BOOL self;
  if (!process_handle_is(process, &self)) return FALSE;
  if (flags & ~(DWORD)1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }      /* PROCESS_AFFINITY_ENABLE_AUTO_UPDATE */
  if (!self) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  self_lock();
  g_self.affinity_update_mode = flags;
  self_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI GetProcessDEPPolicy(HANDLE process, LPDWORD flags, PBOOL permanent) {
  BOOL self;
  if (!flags || !permanent) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!process_handle_is(process, &self)) return FALSE;
  *flags = 1;      /* PROCESS_DEP_ENABLE: a 64-bit process always has it */
  *permanent = TRUE;
  return TRUE;
}
EXPORT BOOL WINAPI SetProcessDEPPolicy(DWORD flags) { (void)flags; set_error(ERROR_NOT_SUPPORTED); return FALSE; }      /* (as on 64-bit Windows) */
EXPORT DWORD WINAPI GetSystemDEPPolicy(void) { return 2; }      /* DEP_SYSTEM_POLICY_TYPE_OPTIN */
EXPORT BOOL WINAPI IsProcessCritical(HANDLE process, PBOOL critical) {
  BOOL self;
  if (!critical) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!process_handle_is(process, &self)) return FALSE;
  *critical = FALSE;
  return TRUE;
}

/* Mitigation policies: the sizes are those of the structures of the PROCESS_MITIGATION_POLICY values; a setting, once made, stays. */
static int mitigation_size(int policy) {
  if (policy == 0) return 8;      /* ProcessDEPPolicy */
  if (policy >= 1 && policy <= 18 && policy != 5) return 4;      /* (5, the options mask, is not a policy of its own) */
  return 0;
}
EXPORT BOOL WINAPI GetProcessMitigationPolicy(HANDLE process, int policy, PVOID buffer, SIZE_T length) {
  BOOL self;
  if (!buffer || mitigation_size(policy) == 0 || length != (SIZE_T)mitigation_size(policy)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!process_handle_is(process, &self)) return FALSE;
  memset(buffer, 0, length);
  if (policy == 0) { ((DWORD *)buffer)[0] = 1; ((BYTE *)buffer)[4] = 1; return TRUE; }
  if (self) { self_lock(); ((DWORD *)buffer)[0] = g_self.mitigation[policy][0]; self_unlock(); }
  return TRUE;
}
EXPORT BOOL WINAPI SetProcessMitigationPolicy(int policy, PVOID buffer, SIZE_T length) {
  if (!buffer || mitigation_size(policy) == 0 || length != (SIZE_T)mitigation_size(policy) || policy == 0 || policy == 7 /* ControlFlowGuard: set by the image */) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  self_lock();
  g_self.mitigation[policy][0] |= ((DWORD *)buffer)[0];
  self_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetProcessDynamicEHContinuationTargets(HANDLE process, USHORT count, PVOID targets) { (void)targets; BOOL self; if (!process_handle_is(process, &self)) return FALSE; if (!count) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return TRUE; }
EXPORT BOOL WINAPI SetProcessDynamicEnforcedCetCompatibleRanges(HANDLE process, USHORT count, PVOID ranges) { return SetProcessDynamicEHContinuationTargets(process, count, ranges); }
EXPORT BOOL WINAPI IsUserCetAvailableInEnvironment(DWORD environment) { (void)environment; return FALSE; }
EXPORT BOOL WINAPI GetMachineTypeAttributes(USHORT machine, DWORD *attributes) {
  if (!attributes) { set_error(ERROR_INVALID_PARAMETER); return (BOOL)0x80070057; }
  *attributes = machine == 0x8664 ? 3u : 0u;      /* UserEnabled | KernelEnabled for the native machine */
  return machine == 0x8664 ? 0 : (BOOL)0x80070057;
}
EXPORT BOOL WINAPI IsWow64GuestMachineSupported(USHORT machine, BOOL *supported) { if (!supported) return (BOOL)0x80070057; *supported = FALSE; (void)machine; return 0; }

/* SetProcessInformation / GetProcessInformation (the classes of PROCESS_INFORMATION_CLASS other than the machine type). */
static BOOL process_information(HANDLE process, int klass, LPVOID information, DWORD size, BOOL set) {
  BOOL self;
  if (!information) { set_error(ERROR_NOACCESS); return FALSE; }
  if (!process_handle_is(process, &self)) return FALSE;
  DWORD expected;
  switch (klass) {
    case 0: case 7: expected = 4; break;      /* ProcessMemoryPriority, ProcessProtectionLevelInfo */
    case 4: expected = 12; break;      /* ProcessPowerThrottling */
    case 8: expected = 8; break;      /* ProcessLeapSecondInfo */
    case 1: case 2: case 3: case 5: case 6: expected = 0; break;      /* (kinds that exist on Windows and mean nothing here) */
    default: set_error(ERROR_INVALID_PARAMETER); return FALSE;
  }
  if (expected == 0) { set_error(set ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED); return FALSE; }
  if (size != expected) { set_error(24 /* ERROR_BAD_LENGTH */); return FALSE; }
  if (set && !self) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  DWORD *v = (DWORD *)information;
  self_lock();
  BOOL ok = TRUE;
  switch (klass) {
    case 0:
      if (set) { if (v[0] < 1 || v[0] > 7) ok = FALSE; else g_self.memory_priority = v[0]; }
      else v[0] = self ? g_self.memory_priority : 5;
      break;
    case 7:
      if (set) { if (v[0] > 3 && v[0] != 0xFFFFFFFFu) ok = FALSE; else g_self.protection_level = v[0]; }
      else v[0] = g_self.protection_level;
      break;
    case 4:
      if (set) {
        if (v[0] != 1 || (v[1] & ~(DWORD)3) || (v[2] & ~v[1])) ok = FALSE;      /* version 1, EXECUTION_SPEED and IGNORE_TIMER_RESOLUTION */
        else { g_self.throttle_control = v[1]; g_self.throttle_state = v[2]; }
      } else { v[0] = 1; v[1] = g_self.throttle_control; v[2] = g_self.throttle_state; }
      break;
    case 8:
      if (set) { if (v[0] & ~(DWORD)1 || v[1] != 0) ok = FALSE; else g_self.leap_second_flags = v[0]; }
      else { v[0] = g_self.leap_second_flags; v[1] = 0; }
      break;
  }
  self_unlock();
  if (!ok) set_error(ERROR_INVALID_PARAMETER);
  return ok;
}
EXPORT BOOL WINAPI SetProcessInformation(HANDLE process, int klass, LPVOID information, DWORD size) { return process_information(process, klass, information, size, TRUE); }

/* Threads: ideal processor, memory priority, power throttling, selected CPU sets. */
typedef struct { DWORD id; BYTE ideal; BYTE cpu_set_count; BYTE has_ideal; BYTE pad; DWORD memory_priority, absolute_cpu_priority, dynamic_code, throttle_control, throttle_state; DWORD cpu_sets[16]; } ThreadSettings;
static ThreadSettings g_thread_settings[64];
static volatile LONG g_thread_settings_lock;
static ThreadSettings *thread_settings(HANDLE thread) {      /* (with the lock held) */
  const DWORD id = GetThreadId(thread);
  if (!id) return NULL;
  ThreadSettings *free_slot = NULL;
  for (int i = 0; i < 64; ++i) {
    if (g_thread_settings[i].id == id) return &g_thread_settings[i];
    if (!g_thread_settings[i].id && !free_slot) free_slot = &g_thread_settings[i];
  }
  if (!free_slot) { set_error(ERROR_NO_SYSTEM_RESOURCES); return NULL; }
  memset(free_slot, 0, sizeof(*free_slot));
  free_slot->id = id;
  free_slot->memory_priority = 5;
  return free_slot;
}
static void thread_settings_lock(void) { while (__sync_lock_test_and_set(&g_thread_settings_lock, 1)) { while (g_thread_settings_lock) __asm__ volatile("pause"); } }
static void thread_settings_unlock(void) { __sync_lock_release(&g_thread_settings_lock); }
static BOOL thread_information(HANDLE thread, int klass, LPVOID information, DWORD size, BOOL set) {
  DWORD expected;
  if (!information) { set_error(ERROR_NOACCESS); return FALSE; }
  switch (klass) {
    case 0: case 1: case 2: expected = 4; break;      /* ThreadMemoryPriority, ThreadAbsoluteCpuPriority, ThreadDynamicCodePolicy */
    case 3: expected = 12; break;      /* ThreadPowerThrottling */
    default: set_error(ERROR_INVALID_PARAMETER); return FALSE;
  }
  if (size != expected) { set_error(24); return FALSE; }
  thread_settings_lock();
  ThreadSettings *t = thread_settings(thread);
  BOOL ok = t != NULL;
  DWORD *v = (DWORD *)information;
  if (t) switch (klass) {
    case 0: if (set) { if (v[0] < 1 || v[0] > 7) { ok = FALSE; set_error(ERROR_INVALID_PARAMETER); } else t->memory_priority = v[0]; } else v[0] = t->memory_priority; break;
    case 1: if (set) t->absolute_cpu_priority = v[0]; else v[0] = t->absolute_cpu_priority; break;
    case 2: if (set) t->dynamic_code = v[0]; else v[0] = t->dynamic_code; break;
    case 3:
      if (set) {
        if (v[0] != 1 || (v[1] & ~(DWORD)1) || (v[2] & ~v[1])) { ok = FALSE; set_error(ERROR_INVALID_PARAMETER); }
        else { t->throttle_control = v[1]; t->throttle_state = v[2]; }
      } else { v[0] = 1; v[1] = t->throttle_control; v[2] = t->throttle_state; }
      break;
  }
  thread_settings_unlock();
  return ok;
}
EXPORT BOOL WINAPI GetThreadInformation(HANDLE thread, int klass, LPVOID information, DWORD size) { return thread_information(thread, klass, information, size, FALSE); }
EXPORT BOOL WINAPI SetThreadInformation(HANDLE thread, int klass, LPVOID information, DWORD size) { return thread_information(thread, klass, information, size, TRUE); }
EXPORT BOOL WINAPI SetThreadIdealProcessorEx(HANDLE thread, PROCESSOR_NUMBER_ *ideal, PROCESSOR_NUMBER_ *previous) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  if (!ideal || ideal->Group != 0 || ideal->Number >= si.dwNumberOfProcessors || ideal->Reserved) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  thread_settings_lock();
  ThreadSettings *t = thread_settings(thread);
  if (t) {
    if (previous) { previous->Group = 0; previous->Number = t->has_ideal ? t->ideal : 0; previous->Reserved = 0; }
    t->ideal = ideal->Number;
    t->has_ideal = 1;
  }
  thread_settings_unlock();
  if (!t) return FALSE;
  SetThreadIdealProcessor(thread, ideal->Number);
  return TRUE;
}
EXPORT BOOL WINAPI GetThreadIdealProcessorEx(HANDLE thread, PROCESSOR_NUMBER_ *ideal) {
  if (!ideal) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  thread_settings_lock();
  ThreadSettings *t = thread_settings(thread);
  if (t) { ideal->Group = 0; ideal->Number = t->has_ideal ? t->ideal : 0; ideal->Reserved = 0; }
  thread_settings_unlock();
  return t != NULL;
}

/* CPU sets: ids are 256 + the logical processor's index, as on Windows. A set of nothing (the default) lets the thread use every processor. */
static BOOL cpu_set_ids_valid(const ULONG *ids, ULONG count) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  for (ULONG i = 0; i < count; ++i) if (ids[i] < 256 || ids[i] - 256 >= si.dwNumberOfProcessors) return FALSE;
  return TRUE;
}
static BOOL cpu_sets_get(const DWORD *stored, ULONG stored_count, PULONG ids, ULONG capacity, PULONG required) {
  if (!required) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *required = stored_count;
  if (capacity < stored_count) { if (ids || capacity) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; } return TRUE; }
  for (ULONG i = 0; i < stored_count; ++i) ids[i] = stored[i];
  return TRUE;
}
EXPORT BOOL WINAPI GetProcessDefaultCpuSets(HANDLE process, PULONG ids, ULONG capacity, PULONG required) {
  BOOL self;
  if (!process_handle_is(process, &self)) return FALSE;
  self_lock();
  const BOOL ok = cpu_sets_get(g_self.default_cpu_sets, self ? g_self.default_cpu_set_count : 0, ids, capacity, required);
  self_unlock();
  return ok;
}
EXPORT BOOL WINAPI SetProcessDefaultCpuSets(HANDLE process, const ULONG *ids, ULONG count) {
  BOOL self;
  if (!process_handle_is(process, &self)) return FALSE;
  if (count > 64 || (count && !ids) || (count && !cpu_set_ids_valid(ids, count))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!self) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  self_lock();
  for (ULONG i = 0; i < count; ++i) g_self.default_cpu_sets[i] = ids[i];
  g_self.default_cpu_set_count = count;
  self_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI GetThreadSelectedCpuSets(HANDLE thread, PULONG ids, ULONG capacity, PULONG required) {
  thread_settings_lock();
  ThreadSettings *t = thread_settings(thread);
  const BOOL ok = t && cpu_sets_get(t->cpu_sets, t->cpu_set_count, ids, capacity, required);
  thread_settings_unlock();
  return ok;
}
EXPORT BOOL WINAPI SetThreadSelectedCpuSets(HANDLE thread, const ULONG *ids, ULONG count) {
  if (count > 16 || (count && !ids) || (count && !cpu_set_ids_valid(ids, count))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  thread_settings_lock();
  ThreadSettings *t = thread_settings(thread);
  if (t) { for (ULONG i = 0; i < count; ++i) t->cpu_sets[i] = ids[i]; t->cpu_set_count = (BYTE)count; }
  thread_settings_unlock();
  return t != NULL;
}
/* The masks form: every id becomes a bit of group 0. */
static ULONG cpu_set_masks(const DWORD *ids, ULONG count, GROUP_AFFINITY *masks, USHORT capacity, PUSHORT_ required) {
  if (!required) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *required = count ? 1 : 0;
  if (capacity < *required) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  if (count) {
    masks[0].Mask = 0; masks[0].Group = 0; masks[0].Reserved[0] = masks[0].Reserved[1] = masks[0].Reserved[2] = 0;
    for (ULONG i = 0; i < count; ++i) masks[0].Mask |= (ULONG_PTR)1 << (ids[i] - 256);
  }
  return TRUE;
}
EXPORT BOOL WINAPI GetProcessDefaultCpuSetMasks(HANDLE process, GROUP_AFFINITY *masks, USHORT capacity, PUSHORT_ required) {
  BOOL self;
  if (!process_handle_is(process, &self)) return FALSE;
  self_lock();
  const BOOL ok = cpu_set_masks(g_self.default_cpu_sets, self ? g_self.default_cpu_set_count : 0, masks, capacity, required);
  self_unlock();
  return ok;
}
EXPORT BOOL WINAPI SetProcessDefaultCpuSetMasks(HANDLE process, GROUP_AFFINITY *masks, USHORT count) {
  ULONG ids[64], n = 0;
  for (USHORT i = 0; i < count; ++i) {
    if (!masks || masks[i].Group != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (int bit = 0; bit < 64; ++bit) if ((masks[i].Mask >> bit) & 1) { if (n == 64) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } ids[n++] = 256 + (ULONG)bit; }
  }
  return SetProcessDefaultCpuSets(process, ids, n);
}
EXPORT BOOL WINAPI GetThreadSelectedCpuSetMasks(HANDLE thread, GROUP_AFFINITY *masks, USHORT capacity, PUSHORT_ required) {
  thread_settings_lock();
  ThreadSettings *t = thread_settings(thread);
  const BOOL ok = t && cpu_set_masks(t->cpu_sets, t->cpu_set_count, masks, capacity, required);
  thread_settings_unlock();
  return ok;
}
EXPORT BOOL WINAPI SetThreadSelectedCpuSetMasks(HANDLE thread, GROUP_AFFINITY *masks, USHORT count) {
  ULONG ids[16], n = 0;
  for (USHORT i = 0; i < count; ++i) {
    if (!masks || masks[i].Group != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (int bit = 0; bit < 64; ++bit) if ((masks[i].Mask >> bit) & 1) { if (n == 16) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } ids[n++] = 256 + (ULONG)bit; }
  }
  return SetThreadSelectedCpuSets(thread, ids, n);
}

/* ---- packages (this OS has none: a process is never packaged) -------------------------------------------------------------------------------------------------- */

#define NO_PACKAGE(call) { (void)0; return ERROR_APPMODEL_NO_PACKAGE; }
EXPORT LONG WINAPI GetCurrentPackageFullName(UINT32 *length, PWSTR name) { (void)name; if (!length) return ERROR_INVALID_PARAMETER; return ERROR_APPMODEL_NO_PACKAGE; }
EXPORT LONG WINAPI GetCurrentPackageFamilyName(UINT32 *length, PWSTR name) { (void)name; if (!length) return ERROR_INVALID_PARAMETER; return ERROR_APPMODEL_NO_PACKAGE; }
EXPORT LONG WINAPI GetCurrentPackageId(UINT32 *length, BYTE *buffer) { (void)buffer; if (!length) return ERROR_INVALID_PARAMETER; return ERROR_APPMODEL_NO_PACKAGE; }
EXPORT LONG WINAPI GetCurrentPackagePath(UINT32 *length, PWSTR path) { (void)path; if (!length) return ERROR_INVALID_PARAMETER; return ERROR_APPMODEL_NO_PACKAGE; }
EXPORT LONG WINAPI GetCurrentPackageInfo(UINT32 flags, UINT32 *length, BYTE *buffer, UINT32 *count) { (void)flags; (void)buffer; if (count) *count = 0; if (!length) return ERROR_INVALID_PARAMETER; return ERROR_APPMODEL_NO_PACKAGE; }
EXPORT LONG WINAPI GetCurrentApplicationUserModelId(UINT32 *length, PWSTR id) { (void)id; if (!length) return ERROR_INVALID_PARAMETER; return ERROR_APPMODEL_NO_PACKAGE; }
EXPORT LONG WINAPI GetApplicationUserModelId(HANDLE process, UINT32 *length, PWSTR id) {
  BOOL self;
  (void)id;
  if (!length) return ERROR_INVALID_PARAMETER;
  if (!process_handle_is(process, &self)) return ERROR_INVALID_HANDLE;
  return ERROR_APPMODEL_NO_PACKAGE;
}
EXPORT LONG WINAPI GetPackageFullName(HANDLE process, UINT32 *length, PWSTR name) { return GetApplicationUserModelId(process, length, name); }
EXPORT LONG WINAPI GetPackageFamilyName(HANDLE process, UINT32 *length, PWSTR name) { return GetApplicationUserModelId(process, length, name); }
EXPORT LONG WINAPI GetPackageId(HANDLE process, UINT32 *length, BYTE *buffer) { return GetApplicationUserModelId(process, length, (PWSTR)buffer); }
EXPORT LONG WINAPI GetPackagePath(const void *id, UINT32 reserved, UINT32 *length, PWSTR path) { (void)reserved; (void)path; if (!id || !length) return ERROR_INVALID_PARAMETER; return ERROR_NOT_FOUND; }
EXPORT LONG WINAPI GetPackagePathByFullName(PCWSTR name, UINT32 *length, PWSTR path) { (void)path; if (!name || !length) return ERROR_INVALID_PARAMETER; return ERROR_NOT_FOUND; }
EXPORT LONG WINAPI GetStagedPackagePathByFullName(PCWSTR name, UINT32 *length, PWSTR path) { return GetPackagePathByFullName(name, length, path); }
EXPORT LONG WINAPI OpenPackageInfoByFullName(PCWSTR name, UINT32 reserved, void **reference) { (void)reserved; if (!name || !reference) return ERROR_INVALID_PARAMETER; *reference = NULL; return ERROR_NOT_FOUND; }
EXPORT LONG WINAPI ClosePackageInfo(void *reference) { (void)reference; return ERROR_INVALID_PARAMETER; }      /* (no reference is ever handed out) */
EXPORT LONG WINAPI GetPackageInfo(void *reference, UINT32 flags, UINT32 *length, BYTE *buffer, UINT32 *count) { (void)flags; (void)buffer; if (count) *count = 0; if (!reference || !length) return ERROR_INVALID_PARAMETER; return ERROR_INVALID_PARAMETER; }
EXPORT LONG WINAPI GetPackageApplicationIds(void *reference, UINT32 *length, BYTE *buffer, UINT32 *count) { (void)buffer; if (count) *count = 0; if (!reference || !length) return ERROR_INVALID_PARAMETER; return ERROR_INVALID_PARAMETER; }
EXPORT LONG WINAPI GetPackagesByPackageFamily(PCWSTR family, UINT32 *count, PWSTR *names, UINT32 *buffer_length, PWSTR buffer) {
  (void)names; (void)buffer;
  if (!family || !count || !buffer_length) return ERROR_INVALID_PARAMETER;
  *count = 0; *buffer_length = 0;
  return 0;
}
EXPORT LONG WINAPI FindPackagesByPackageFamily(PCWSTR family, UINT32 flags, UINT32 *count, PWSTR *names, UINT32 *buffer_length, PWSTR buffer, UINT32 *properties) {
  (void)flags; (void)properties;
  return GetPackagesByPackageFamily(family, count, names, buffer_length, buffer);
}
/* Parsing and formatting of names: "Name_Version_Architecture_ResourceId_PublisherId" (full), "Name_PublisherId" (family), "Family!ApplicationId" (user model id). */
static LONG copy_out(const WCHAR *text, SIZE_T chars, UINT32 *length, PWSTR out) {
  if (*length < chars + 1) { *length = (UINT32)chars + 1; return ERROR_INSUFFICIENT_BUFFER; }
  if (!out) return ERROR_INVALID_PARAMETER;
  memcpy(out, text, chars * sizeof(WCHAR));
  out[chars] = 0;
  *length = (UINT32)chars + 1;
  return 0;
}
static const WCHAR *wide_find(const WCHAR *s, WCHAR c, int nth_from_start) {
  for (; *s; ++s) if (*s == c && nth_from_start-- == 0) return s;
  return NULL;
}
EXPORT LONG WINAPI PackageFamilyNameFromFullName(PCWSTR full, UINT32 *length, PWSTR family) {
  if (!full || !length) return ERROR_INVALID_PARAMETER;
  const WCHAR *u1 = wide_find(full, '_', 0), *u2 = wide_find(full, '_', 1), *u3 = wide_find(full, '_', 2), *u4 = wide_find(full, '_', 3);
  if (!u1 || !u2 || !u3 || !u4 || u1 == full || wide_find(full, '_', 4) || !u4[1]) return ERROR_INVALID_PARAMETER;
  WCHAR text[PATH_CHARS];
  const SIZE_T name = (SIZE_T)(u1 - full), publisher = wcslen(u4 + 1);
  if (name + 1 + publisher >= PATH_CHARS) return ERROR_INVALID_PARAMETER;
  memcpy(text, full, name * sizeof(WCHAR));
  text[name] = '_';
  memcpy(text + name + 1, u4 + 1, (publisher + 1) * sizeof(WCHAR));
  return copy_out(text, name + 1 + publisher, length, family);
}
EXPORT LONG WINAPI PackageFamilyNameFromId(const void *id, UINT32 *length, PWSTR family) {
  if (!id || !length) return ERROR_INVALID_PARAMETER;
  const WCHAR *name = *(const WCHAR *const *)((const BYTE *)id + 16), *publisher_id = *(const WCHAR *const *)((const BYTE *)id + 32);      /* PACKAGE_ID: name, publisher, resourceId, publisherId */
  if (!name || !publisher_id) return ERROR_INVALID_PARAMETER;
  WCHAR text[PATH_CHARS];
  const SIZE_T n = wcslen(name), p = wcslen(publisher_id);
  if (n + 1 + p >= PATH_CHARS) return ERROR_INVALID_PARAMETER;
  memcpy(text, name, n * sizeof(WCHAR));
  text[n] = '_';
  memcpy(text + n + 1, publisher_id, (p + 1) * sizeof(WCHAR));
  return copy_out(text, n + 1 + p, length, family);
}
EXPORT LONG WINAPI PackageNameAndPublisherIdFromFamilyName(PCWSTR family, UINT32 *name_length, PWSTR name, UINT32 *publisher_length, PWSTR publisher) {
  if (!family || !name_length || !publisher_length) return ERROR_INVALID_PARAMETER;
  const WCHAR *u = wide_find(family, '_', 0);
  if (!u || u == family || !u[1] || wide_find(family, '_', 1)) return ERROR_INVALID_PARAMETER;
  LONG error = copy_out(family, (SIZE_T)(u - family), name_length, name);
  const LONG second = copy_out(u + 1, wcslen(u + 1), publisher_length, publisher);
  return error ? error : second;
}
EXPORT LONG WINAPI ParseApplicationUserModelId(PCWSTR id, UINT32 *family_length, PWSTR family, UINT32 *app_length, PWSTR app) {
  if (!id || !family_length || !app_length) return ERROR_INVALID_PARAMETER;
  const WCHAR *bang = wide_find(id, '!', 0);
  if (!bang || bang == id || !bang[1] || wide_find(id, '!', 1)) return ERROR_INVALID_PARAMETER;
  const LONG a = copy_out(id, (SIZE_T)(bang - id), family_length, family), b = copy_out(bang + 1, wcslen(bang + 1), app_length, app);
  return a ? a : b;
}
EXPORT LONG WINAPI FormatApplicationUserModelId(PCWSTR family, PCWSTR app, UINT32 *length, PWSTR out) {
  if (!family || !app || !length || !*family || !*app) return ERROR_INVALID_PARAMETER;
  WCHAR text[PATH_CHARS];
  const SIZE_T f = wcslen(family), a = wcslen(app);
  if (f + 1 + a >= PATH_CHARS) return ERROR_INVALID_PARAMETER;
  memcpy(text, family, f * sizeof(WCHAR));
  text[f] = '!';
  memcpy(text + f + 1, app, (a + 1) * sizeof(WCHAR));
  return copy_out(text, f + 1 + a, length, out);
}
EXPORT PVOID WINAPI GetCurrentPackageVirtualizationContext(void) { return NULL; }
EXPORT PVOID WINAPI CreatePackageVirtualizationContext(PCWSTR family) { (void)family; set_error(ERROR_APPMODEL_NO_PACKAGE); return NULL; }
EXPORT PVOID WINAPI DuplicatePackageVirtualizationContext(PVOID context) { (void)context; set_error(ERROR_INVALID_PARAMETER); return NULL; }
EXPORT void WINAPI ReleasePackageVirtualizationContext(PVOID context) { (void)context; }
EXPORT PVOID WINAPI ActivatePackageVirtualizationContext(PVOID context) { (void)context; set_error(ERROR_INVALID_PARAMETER); return NULL; }
EXPORT void WINAPI DeactivatePackageVirtualizationContext(PVOID cookie) { (void)cookie; }
EXPORT BOOL WINAPI GetProcessesInVirtualizationContext(PCWSTR family, PULONG count, PULONG_PTR ids) { (void)family; (void)ids; if (count) *count = 0; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
/* The policies of a desktop (unpackaged) program. */
#define APP_POLICY(name, value) EXPORT LONG WINAPI name(HANDLE token, int *policy) { (void)token; if (!policy) return ERROR_INVALID_PARAMETER; *policy = (value); return 0; }
APP_POLICY(AppPolicyGetClrCompat, 2)              /* AppPolicyClrCompat_ClassicDesktop */
APP_POLICY(AppPolicyGetCreateFileAccess, 0)       /* AppPolicyCreateFileAccess_Full */
APP_POLICY(AppPolicyGetLifecycleManagement, 0)    /* AppPolicyLifecycleManagement_Unmanaged */
APP_POLICY(AppPolicyGetMediaFoundationCodecLoading, 0)      /* ..._All */
APP_POLICY(AppPolicyGetProcessTerminationMethod, 0)         /* ..._ExitProcess */
APP_POLICY(AppPolicyGetShowDeveloperDiagnostic, 0)          /* ..._None */
APP_POLICY(AppPolicyGetThreadInitializationType, 0)         /* ..._None */
APP_POLICY(AppPolicyGetWindowingModel, 2)         /* AppPolicyWindowingModel_ClassicDesktop */
EXPORT LONG WINAPI AppXGetOSMaxVersionTested(PCWSTR path, ULONGLONG *version) { (void)path; if (version) *version = 0; return ERROR_APPMODEL_NO_PACKAGE; }
EXPORT LONG WINAPI OpenState(void **state) { if (state) *state = NULL; return (LONG)0x80073D54; }      /* HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE) */
EXPORT LONG WINAPI OpenStateExplicit(PCWSTR family, void **state) { (void)family; if (state) *state = NULL; return (LONG)0x80073D54; }
EXPORT void WINAPI CloseState(void *state) { (void)state; }
EXPORT LONG WINAPI GetStateFolder(int folder, void *state, PWSTR path, UINT32 *length) { (void)folder; (void)state; (void)path; (void)length; return (LONG)0x80073D54; }
EXPORT LONG WINAPI GetSystemAppDataKey(PCWSTR name, UINT32 *length, void *buffer) { (void)name; (void)buffer; if (length) *length = 0; return (LONG)0x80073D54; }

/* ---- application recovery and restart, Windows Error Reporting registrations -------------------------------------------------------------------------------------- */

#define HR_E_NOTFOUND ((HRESULT)0x80070490)
typedef struct { PVOID callback, parameter; DWORD ping, flags; BOOL registered; WCHAR restart[1024]; DWORD restart_flags; BOOL restart_registered; } RecoveryState;
static RecoveryState g_recovery;
EXPORT HRESULT WINAPI RegisterApplicationRecoveryCallback(PVOID callback, PVOID parameter, DWORD ping, DWORD flags) {
  if (!callback || flags || ping > 5000 * 60 || (ping && ping < 100 /* a ping interval that short is not accepted */)) return (HRESULT)0x80070057;
  self_lock();
  g_recovery.callback = callback; g_recovery.parameter = parameter; g_recovery.ping = ping ? ping : 5000; g_recovery.flags = flags; g_recovery.registered = TRUE;
  self_unlock();
  return 0;
}
EXPORT HRESULT WINAPI UnregisterApplicationRecoveryCallback(void) { self_lock(); g_recovery.registered = FALSE; g_recovery.callback = NULL; self_unlock(); return 0; }
EXPORT HRESULT WINAPI GetApplicationRecoveryCallback(HANDLE process, PVOID *callback, PVOID *parameter, PDWORD ping, PDWORD flags) {
  BOOL self;
  if (!callback || !parameter || !ping || !flags) return (HRESULT)0x80070057;
  if (!process_handle_is(process, &self)) return (HRESULT)0x80070006;
  if (!self) return HR_E_NOTFOUND;      /* (another process's registrations are not in reach) */
  HRESULT result = HR_E_NOTFOUND;
  self_lock();
  if (g_recovery.registered) { *callback = g_recovery.callback; *parameter = g_recovery.parameter; *ping = g_recovery.ping; *flags = g_recovery.flags; result = 0; }
  self_unlock();
  return result;
}
EXPORT HRESULT WINAPI ApplicationRecoveryInProgress(PBOOL canceled) { if (canceled) *canceled = FALSE; return HR_E_NOTFOUND; }      /* (no crash report is being made) */
EXPORT void WINAPI ApplicationRecoveryFinished(BOOL success) { (void)success; }
EXPORT HRESULT WINAPI RegisterApplicationRestart(PCWSTR command_line, DWORD flags) {
  if (flags > 0xF || (command_line && wcslen(command_line) >= 1024)) return (HRESULT)0x80070057;
  self_lock();
  if (command_line) memcpy(g_recovery.restart, command_line, (wcslen(command_line) + 1) * sizeof(WCHAR)); else g_recovery.restart[0] = 0;
  g_recovery.restart_flags = flags;
  g_recovery.restart_registered = TRUE;
  self_unlock();
  return 0;
}
EXPORT HRESULT WINAPI UnregisterApplicationRestart(void) { self_lock(); g_recovery.restart_registered = FALSE; self_unlock(); return 0; }
EXPORT HRESULT WINAPI GetApplicationRestartSettings(HANDLE process, PWSTR command_line, PDWORD size, PDWORD flags) {
  BOOL self;
  if (!size || (*size && !command_line)) return (HRESULT)0x80070057;
  if (!process_handle_is(process, &self)) return (HRESULT)0x80070006;
  if (!self) return HR_E_NOTFOUND;
  HRESULT result = HR_E_NOTFOUND;
  self_lock();
  if (g_recovery.restart_registered) {
    const DWORD need = (DWORD)wcslen(g_recovery.restart) + 1;
    if (*size < need) { *size = need; result = (HRESULT)0x8007007A; /* ERROR_INSUFFICIENT_BUFFER as an HRESULT */ }
    else { memcpy(command_line, g_recovery.restart, need * sizeof(WCHAR)); *size = need; if (flags) *flags = g_recovery.restart_flags; result = 0; }
  }
  self_unlock();
  return result;
}
EXPORT HRESULT WINAPI RegisterWaitUntilOOBECompleted(PVOID callback, PVOID context, PVOID *handle) { (void)callback; (void)context; if (handle) *handle = NULL; return (HRESULT)0x80070032; }
/* Windows Error Reporting: this OS makes no reports, so what a program registers for one is accepted and dropped. */
#define WER_OK1(name, a) EXPORT HRESULT WINAPI name a { return 0; }
EXPORT HRESULT WINAPI WerRegisterFile(PCWSTR file, int type, DWORD flags) { (void)type; (void)flags; return file ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerUnregisterFile(PCWSTR file) { return file ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerRegisterFileWorker(PCWSTR file, int type, DWORD flags) { return WerRegisterFile(file, type, flags); }
EXPORT HRESULT WINAPI WerUnregisterFileWorker(PCWSTR file) { return WerUnregisterFile(file); }
EXPORT HRESULT WINAPI WerRegisterMemoryBlock(PVOID address, DWORD size) { return address && size ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerUnregisterMemoryBlock(PVOID address) { return address ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerRegisterMemoryBlockWorker(PVOID address, DWORD size) { return WerRegisterMemoryBlock(address, size); }
EXPORT HRESULT WINAPI WerUnregisterMemoryBlockWorker(PVOID address) { return WerUnregisterMemoryBlock(address); }
EXPORT HRESULT WINAPI WerRegisterExcludedMemoryBlock(const void *address, DWORD size) { return address && size ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerUnregisterExcludedMemoryBlock(const void *address) { return address ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerRegisterAdditionalProcess(DWORD pid, DWORD thread) { (void)thread; return pid ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerUnregisterAdditionalProcess(DWORD pid) { return pid ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerRegisterAppLocalDump(PCWSTR directory) { return directory ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerUnregisterAppLocalDump(void) { return 0; }
EXPORT HRESULT WINAPI WerRegisterCustomMetadata(PCWSTR key, PCWSTR value) { return key && value ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerUnregisterCustomMetadata(PCWSTR key) { return key ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerRegisterRuntimeExceptionModule(PCWSTR module, PVOID context) { (void)context; return module ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerUnregisterRuntimeExceptionModule(PCWSTR module, PVOID context) { (void)context; return module ? 0 : (HRESULT)0x80070057; }
EXPORT HRESULT WINAPI WerRegisterRuntimeExceptionModuleWorker(PCWSTR module, PVOID context) { return WerRegisterRuntimeExceptionModule(module, context); }
EXPORT HRESULT WINAPI WerUnregisterRuntimeExceptionModuleWorker(PCWSTR module, PVOID context) { return WerUnregisterRuntimeExceptionModule(module, context); }
static DWORD g_wer_flags;
EXPORT HRESULT WINAPI WerSetFlags(DWORD flags) { if (flags & ~(DWORD)0x1F) return (HRESULT)0x80070057; g_wer_flags = flags; return 0; }
EXPORT HRESULT WINAPI WerGetFlags(HANDLE process, PDWORD flags) { BOOL self; if (!flags) return (HRESULT)0x80070057; if (!process_handle_is(process, &self)) return (HRESULT)0x80070006; *flags = self ? g_wer_flags : 0; return 0; }
EXPORT HRESULT WINAPI WerSetFlagsWorker(DWORD flags) { return WerSetFlags(flags); }
EXPORT HRESULT WINAPI WerGetFlagsWorker(HANDLE process, PDWORD flags) { return WerGetFlags(process, flags); }

/* RaiseFailFastException: the process ends at once, with the code of the exception it was given (or STATUS_FAIL_FAST_EXCEPTION), with no handler, filter or debugger-less report
 * running; a debugger is told about the exception first. */
EXPORT void WINAPI RaiseFailFastException(EXCEPTION_RECORD *record, CONTEXT *context, DWORD flags) {
  (void)flags;
  if (IsDebuggerPresent() && record) RtlRaiseException(record);
  (void)context;
  NtTerminateProcess(NtCurrentProcess(), record ? (NTSTATUS)record->ExceptionCode : (NTSTATUS)0xC0000602);
  for (;;) {}
}

/* ---- power ------------------------------------------------------------------------------------------------------------------------------------------------ */

#define ES_CONTINUOUS_ 0x80000000u
EXPORT DWORD WINAPI SetThreadExecutionState(DWORD state) {
  if (state & ~(ES_CONTINUOUS_ | 0x1 | 0x2 | 0x4 | 0x40)) return 0;      /* ES_SYSTEM_REQUIRED, DISPLAY_REQUIRED, AWAYMODE_REQUIRED, USER_PRESENT */
  self_lock();
  const DWORD previous = g_self.execution_state;
  if (state & ES_CONTINUOUS_) g_self.execution_state = state;
  self_unlock();
  return previous;
}
EXPORT HANDLE WINAPI PowerCreateRequest(const void *context) {
  if (!context) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  const DWORD version = *(const DWORD *)context, flags = *(const DWORD *)((const BYTE *)context + 4);
  if (version > 1 || (flags != 1 /* POWER_REQUEST_CONTEXT_SIMPLE_STRING */ && flags != 2 /* DETAILED_STRING */)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  HANDLE h = CreateEventW(NULL, TRUE, FALSE, NULL);      /* the request is a handle that nothing but being closed gives up */
  return h ? h : INVALID_HANDLE_VALUE;
}
EXPORT BOOL WINAPI PowerSetRequest(HANDLE request, int type) { DWORD flags; if (type < 0 || type > 3) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return GetHandleInformation(request, &flags); }
EXPORT BOOL WINAPI PowerClearRequest(HANDLE request, int type) { return PowerSetRequest(request, type); }
EXPORT BOOL WINAPI GetDevicePowerState(HANDLE device, PBOOL on) { if (!on) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } if (GetFileType(device) == 0 && GetLastError()) return FALSE; *on = TRUE; return TRUE; }
EXPORT BOOL WINAPI SetSystemPowerState(BOOL suspend, BOOL force) { (void)suspend; (void)force; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI IsSystemResumeAutomatic(void) { return FALSE; }
EXPORT BOOL WINAPI RequestWakeupLatency(int latency) { return latency == 0 || latency == 1; }
EXPORT BOOL WINAPI RequestDeviceWakeup(HANDLE device) { (void)device; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT BOOL WINAPI CancelDeviceWakeupRequest(HANDLE device) { (void)device; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT BOOL WINAPI SetMessageWaitingIndicator(HANDLE indicator, ULONG count) { (void)indicator; (void)count; set_error(ERROR_INVALID_PARAMETER); return FALSE; }

/* ---- tape drives (there are none: a handle that is not a tape answers like a file does) --------------------------------------------------------------------------- */

#define TAPE_ERROR(name, args) EXPORT DWORD WINAPI name args { return ERROR_INVALID_FUNCTION; }
TAPE_ERROR(GetTapeParameters, (HANDLE h, DWORD op, LPDWORD size, LPVOID info))
TAPE_ERROR(SetTapeParameters, (HANDLE h, DWORD op, LPVOID info))
TAPE_ERROR(GetTapePosition, (HANDLE h, DWORD type, LPDWORD partition, LPDWORD low, LPDWORD high))
TAPE_ERROR(SetTapePosition, (HANDLE h, DWORD method, DWORD partition, DWORD low, DWORD high, BOOL immediate))
TAPE_ERROR(GetTapeStatus, (HANDLE h))
TAPE_ERROR(PrepareTape, (HANDLE h, DWORD op, BOOL immediate))
TAPE_ERROR(EraseTape, (HANDLE h, DWORD type, BOOL immediate))
TAPE_ERROR(WriteTapemark, (HANDLE h, DWORD type, DWORD count, BOOL immediate))
TAPE_ERROR(CreateTapePartition, (HANDLE h, DWORD method, DWORD count, DWORD size))

/* ---- firmware environment variables (the UEFI variables are not reachable from a program) ------------------------------------------------------------------------------ */

EXPORT BOOL WINAPI GetFirmwareType(DWORD *type) { if (!type) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *type = 2; /* FirmwareTypeUefi */ return TRUE; }
EXPORT DWORD WINAPI GetFirmwareEnvironmentVariableExW(LPCWSTR name, LPCWSTR guid, PVOID buffer, DWORD size, PDWORD attributes) { (void)name; (void)guid; (void)buffer; (void)size; (void)attributes; set_error(ERROR_PRIVILEGE_NOT_HELD); return 0; }
EXPORT DWORD WINAPI GetFirmwareEnvironmentVariableW(LPCWSTR name, LPCWSTR guid, PVOID buffer, DWORD size) { return GetFirmwareEnvironmentVariableExW(name, guid, buffer, size, NULL); }
EXPORT DWORD WINAPI GetFirmwareEnvironmentVariableExA(LPCSTR name, LPCSTR guid, PVOID buffer, DWORD size, PDWORD attributes) { (void)name; (void)guid; (void)buffer; (void)size; (void)attributes; set_error(ERROR_PRIVILEGE_NOT_HELD); return 0; }
EXPORT DWORD WINAPI GetFirmwareEnvironmentVariableA(LPCSTR name, LPCSTR guid, PVOID buffer, DWORD size) { return GetFirmwareEnvironmentVariableExA(name, guid, buffer, size, NULL); }
EXPORT BOOL WINAPI SetFirmwareEnvironmentVariableExW(LPCWSTR name, LPCWSTR guid, PVOID buffer, DWORD size, DWORD attributes) { (void)name; (void)guid; (void)buffer; (void)size; (void)attributes; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI SetFirmwareEnvironmentVariableW(LPCWSTR name, LPCWSTR guid, PVOID buffer, DWORD size) { return SetFirmwareEnvironmentVariableExW(name, guid, buffer, size, 0); }
EXPORT BOOL WINAPI SetFirmwareEnvironmentVariableExA(LPCSTR name, LPCSTR guid, PVOID buffer, DWORD size, DWORD attributes) { (void)name; (void)guid; (void)buffer; (void)size; (void)attributes; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI SetFirmwareEnvironmentVariableA(LPCSTR name, LPCSTR guid, PVOID buffer, DWORD size) { return SetFirmwareEnvironmentVariableExA(name, guid, buffer, size, 0); }
EXPORT BOOL WINAPI IsNativeVhdBoot(PBOOL native) { if (!native) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *native = FALSE; return TRUE; }
EXPORT BOOL WINAPI GetOsSafeBootMode(PDWORD mode) { if (!mode) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *mode = 0; return TRUE; }
EXPORT BOOL WINAPI GetMemoryErrorHandlingCapabilities(PULONG capabilities) { if (!capabilities) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *capabilities = 0; return TRUE; }
EXPORT PVOID WINAPI RegisterBadMemoryNotification(PVOID callback) { (void)callback; set_error(ERROR_NOT_SUPPORTED); return NULL; }
EXPORT BOOL WINAPI UnregisterBadMemoryNotification(PVOID handle) { (void)handle; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI GetSystemFileCacheSize(PSIZE_T minimum, PSIZE_T maximum, PDWORD flags) {
  if (!minimum || !maximum || !flags) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *minimum = 0; *maximum = 0x40000000; *flags = 0;
  return TRUE;
}
EXPORT BOOL WINAPI SetSystemFileCacheSize(SIZE_T minimum, SIZE_T maximum, DWORD flags) { (void)minimum; (void)maximum; (void)flags; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI GetProcessorSystemCycleTime(USHORT group, PVOID information, PDWORD length) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const DWORD need = si.dwNumberOfProcessors * 16;
  if (group != 0 || !length) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (*length < need || !information) { *length = need; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memset(information, 0, need);      /* (cycles are not counted per processor) */
  *length = need;
  return TRUE;
}
EXPORT BOOL WINAPI CheckTokenCapability(HANDLE token, PVOID capability, PBOOL has) { (void)token; (void)capability; if (!has) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *has = FALSE; return TRUE; }
EXPORT BOOL WINAPI GetThreadSelectorEntry(HANDLE thread, DWORD selector, PVOID entry) { (void)thread; (void)selector; (void)entry; set_error(ERROR_NOT_SUPPORTED); return FALSE; }      /* (no descriptor tables to read on x64) */
EXPORT BOOL WINAPI Wow64DisableWow64FsRedirection(PVOID *old) { (void)old; set_error(ERROR_INVALID_FUNCTION); return FALSE; }      /* (a 64-bit process, as on 64-bit Windows) */
EXPORT BOOLEAN WINAPI Wow64EnableWow64FsRedirection(BOOLEAN enable) { (void)enable; set_error(ERROR_INVALID_FUNCTION); return FALSE; }
EXPORT BOOL WINAPI Wow64RevertWow64FsRedirection(PVOID old) { (void)old; set_error(ERROR_INVALID_FUNCTION); return FALSE; }
EXPORT BOOL WINAPI Wow64GetThreadContext(HANDLE thread, PVOID context) { (void)thread; (void)context; set_error(ERROR_INVALID_FUNCTION); return FALSE; }
EXPORT BOOL WINAPI Wow64SetThreadContext(HANDLE thread, const void *context) { (void)thread; (void)context; set_error(ERROR_INVALID_FUNCTION); return FALSE; }
EXPORT BOOL WINAPI Wow64GetThreadSelectorEntry(HANDLE thread, DWORD selector, PVOID entry) { (void)thread; (void)selector; (void)entry; set_error(ERROR_INVALID_FUNCTION); return FALSE; }
EXPORT DWORD WINAPI Wow64SuspendThread(HANDLE thread) { (void)thread; set_error(ERROR_INVALID_FUNCTION); return (DWORD)-1; }

/* ---- vectored continue handlers, small forwards ---------------------------------------------------------------------------------------------------------------- */

IMPORT PVOID NTAPI RtlAddVectoredContinueHandler(ULONG, PVOID);
IMPORT ULONG NTAPI RtlRemoveVectoredContinueHandler(PVOID);
EXPORT PVOID WINAPI AddVectoredContinueHandler(ULONG first, PVOID handler) { return RtlAddVectoredContinueHandler(first, handler); }
EXPORT ULONG WINAPI RemoveVectoredContinueHandler(PVOID handler) { return RtlRemoveVectoredContinueHandler(handler); }
EXPORT DWORD WINAPI GetTempPath2W(DWORD capacity, LPWSTR buffer) { return GetTempPathW(capacity, buffer); }
EXPORT DWORD WINAPI GetTempPath2A(DWORD capacity, LPSTR buffer) { return GetTempPathA(capacity, buffer); }
EXPORT BOOL WINAPI AreShortNamesEnabled(HANDLE root, PBOOL enabled) { (void)root; if (!enabled) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *enabled = FALSE; return TRUE; }
EXPORT BOOL WINAPI SetStdHandleEx(DWORD which, HANDLE handle, PHANDLE previous) {
  HANDLE old = GetStdHandle(which);
  if (old == INVALID_HANDLE_VALUE && GetLastError()) return FALSE;
  if (!SetStdHandle(which, handle)) return FALSE;
  if (previous) *previous = old;
  return TRUE;
}
EXPORT UINT WINAPI SetHandleCount(UINT count) { return count; }
EXPORT LPVOID WINAPI TlsGetValue2(DWORD index) { const DWORD saved = GetLastError(); LPVOID v = TlsGetValue(index); set_error(saved); return v; }
EXPORT PVOID WINAPI FlsGetValue2(DWORD index) { const DWORD saved = GetLastError(); PVOID v = FlsGetValue(index); set_error(saved); return v; }
EXPORT void WINAPI RestoreLastError(DWORD error) { set_error(error); }
EXPORT BOOL WINAPI InitAtomTable(DWORD size) { (void)size; return TRUE; }
EXPORT ATOM WINAPI GlobalAddAtomExW(LPCWSTR name, DWORD flags) { (void)flags; return GlobalAddAtomW(name); }
EXPORT ATOM WINAPI GlobalAddAtomExA(LPCSTR name, DWORD flags) { (void)flags; return GlobalAddAtomA(name); }
/* The memory functions of 16-bit Windows: memory does not move, so there is nothing to fix, wire or compact. */
EXPORT void WINAPI GlobalFix(HGLOBAL m) { (void)m; }
EXPORT void WINAPI GlobalUnfix(HGLOBAL m) { (void)m; }
EXPORT LPVOID WINAPI GlobalWire(HGLOBAL m) { return GlobalLock(m); }
EXPORT BOOL WINAPI GlobalUnWire(HGLOBAL m) { return GlobalUnlock(m); }
EXPORT SIZE_T WINAPI GlobalCompact(DWORD minimum) { return minimum; }
EXPORT SIZE_T WINAPI LocalCompact(UINT minimum) { return minimum; }
EXPORT SIZE_T WINAPI LocalShrink(HLOCAL m, UINT size) { (void)m; return size; }
EXPORT BOOL WINAPI IsBadHugeReadPtr(const void *p, UINT_PTR length) { return IsBadReadPtr(p, length); }
EXPORT BOOL WINAPI IsBadHugeWritePtr(LPVOID p, UINT_PTR length) { return IsBadWritePtr(p, length); }
EXPORT PVOID WINAPI HeapCreateTagsW(HANDLE heap_, DWORD flags, PCWSTR tag, PCWSTR name) { (void)heap_; (void)flags; (void)tag; (void)name; return NULL; }
EXPORT PCWSTR WINAPI HeapQueryTagW(HANDLE heap_, DWORD flags, WORD tag, BOOL reset, PVOID usage) { (void)heap_; (void)flags; (void)tag; (void)reset; (void)usage; return NULL; }
EXPORT BOOL WINAPI HeapExtend(HANDLE heap_, DWORD flags, PVOID base, DWORD size) { (void)heap_; (void)flags; (void)base; (void)size; set_error(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
EXPORT BOOL WINAPI HeapUsage(HANDLE heap_, DWORD flags, DWORD allocation, SIZE_T size, PVOID usage) { (void)heap_; (void)flags; (void)allocation; (void)size; (void)usage; set_error(ERROR_CALL_NOT_IMPLEMENTED); return FALSE; }
EXPORT UINT WINAPI LoadModule(LPCSTR name, LPVOID parameters) {
  /* PARAMETERBLOCK: wEnvAddress (WORD), lpCmdLine (LPSTR), lpCmdShow, dwReserved */
  if (!name) return 2;
  char line[1024];
  size_t_ n = 0;
  for (; name[n] && n < sizeof(line) - 2; ++n) line[n] = name[n];
  const char *args = parameters ? *(const char *const *)((const BYTE *)parameters + 8) : NULL;
  if (args && *args && args[0] != 0) {      /* (a command line counted as Pascal strings in the 16-bit days is a plain string here: first byte = its length) */
    const unsigned len = (unsigned char)args[0];
    line[n++] = ' ';
    for (unsigned i = 0; i < len && n < sizeof(line) - 1; ++i) line[n++] = args[1 + i];
  }
  line[n] = 0;
  return WinExec(line, 1);
}

/* ---- the machine's name, set by an administrator only ----------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI SetComputerNameExW(int type, LPCWSTR name) {
  if (!name || type < 0 || type > 8) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  set_error(ERROR_ACCESS_DENIED);      /* (the name is the machine's, which a program that is not the system cannot change) */
  return FALSE;
}
EXPORT BOOL WINAPI SetComputerNameW(LPCWSTR name) { return SetComputerNameExW(5 /* ComputerNamePhysicalNetBIOS */, name); }
EXPORT BOOL WINAPI SetComputerNameExA(int type, LPCSTR name) { if (!name) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return SetComputerNameExW(type, L""); }
EXPORT BOOL WINAPI SetComputerNameA(LPCSTR name) { return SetComputerNameExA(5, name); }
EXPORT BOOL WINAPI SetComputerNameEx2W(int type, DWORD flags, LPCWSTR name) { (void)flags; return SetComputerNameExW(type, name); }
EXPORT BOOL WINAPI SetLocalPrimaryComputerNameW(DWORD reserved, LPCWSTR name) { (void)reserved; return SetComputerNameExW(0, name); }
EXPORT BOOL WINAPI SetLocalPrimaryComputerNameA(DWORD reserved, LPCSTR name) { (void)reserved; return SetComputerNameExA(0, name); }
EXPORT BOOL WINAPI AddLocalAlternateComputerNameW(LPCWSTR name, ULONG flags) { (void)flags; return SetComputerNameExW(0, name); }
EXPORT BOOL WINAPI AddLocalAlternateComputerNameA(LPCSTR name, ULONG flags) { (void)flags; return SetComputerNameExA(0, name); }
EXPORT BOOL WINAPI RemoveLocalAlternateComputerNameW(LPCWSTR name, ULONG flags) { (void)flags; return SetComputerNameExW(0, name); }
EXPORT BOOL WINAPI RemoveLocalAlternateComputerNameA(LPCSTR name, ULONG flags) { (void)flags; return SetComputerNameExA(0, name); }
EXPORT BOOL WINAPI EnumerateLocalComputerNamesW(int type, DWORD reserved, LPWSTR buffer, LPDWORD size) {
  WCHAR name[16];
  DWORD n = 16;
  (void)reserved;
  if (!size || type < 0 || type > 8) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetComputerNameExW(type, name, &n)) return FALSE;
  if (*size < n + 2 || !buffer) { *size = n + 2; set_error(ERROR_MORE_DATA); return FALSE; }
  memcpy(buffer, name, n * sizeof(WCHAR));
  buffer[n] = 0; buffer[n + 1] = 0;
  *size = n + 2;
  return TRUE;
}
EXPORT BOOL WINAPI EnumerateLocalComputerNamesA(int type, DWORD reserved, LPSTR buffer, LPDWORD size) {
  WCHAR wide[40];
  DWORD n = 40;
  if (!EnumerateLocalComputerNamesW(type, reserved, wide, &n)) { if (size) *size = n; return FALSE; }
  if (!size || *size < n || !buffer) { if (size) *size = n; set_error(ERROR_MORE_DATA); return FALSE; }
  for (DWORD i = 0; i < n; ++i) buffer[i] = (char)wide[i];
  *size = n;
  return TRUE;
}
/* A DNS host name as a NetBIOS name: its first label, in upper case, of at most 15 characters. */
EXPORT BOOL WINAPI DnsHostnameToComputerNameExW(LPCWSTR host, LPWSTR name, LPDWORD size) {
  if (!host || !size) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  DWORD n = 0;
  while (host[n] && host[n] != '.' && n < 15) ++n;
  if (!name || *size < n + 1) { *size = n + 1; set_error(ERROR_MORE_DATA); return FALSE; }
  for (DWORD i = 0; i < n; ++i) name[i] = wide_upper(host[i]);
  name[n] = 0;
  *size = n;
  return TRUE;
}
EXPORT BOOL WINAPI DnsHostnameToComputerNameW(LPCWSTR host, LPWSTR name, LPDWORD size) { return DnsHostnameToComputerNameExW(host, name, size); }
EXPORT BOOL WINAPI DnsHostnameToComputerNameA(LPCSTR host, LPSTR name, LPDWORD size) {
  WCHAR *w = host ? to_wide(host) : NULL;
  WCHAR out[16];
  DWORD n = 16;
  if (!w) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const BOOL ok = DnsHostnameToComputerNameExW(w, out, &n);
  free_wide(w);
  if (!size) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!ok || !name || *size < n + 1) { *size = n + 1; if (ok) set_error(ERROR_MORE_DATA); return FALSE; }
  for (DWORD i = 0; i <= n; ++i) name[i] = (char)out[i];
  *size = n;
  return TRUE;
}

/* ---- console: aliases and command history ----------------------------------------------------------------------------------------------------------------------- */

typedef struct { WCHAR exe[64]; WCHAR source[64]; WCHAR target[256]; } ConsoleAlias;
static ConsoleAlias g_aliases[64];
static DWORD g_history_commands = 50, g_history_mode;
static volatile LONG g_alias_lock;
static void alias_lock(void) { while (__sync_lock_test_and_set(&g_alias_lock, 1)) { while (g_alias_lock) __asm__ volatile("pause"); } }
static void alias_unlock(void) { __sync_lock_release(&g_alias_lock); }
static BOOL alias_match(const WCHAR *a, const WCHAR *b) { return equals_ignore_case(a, b); }
EXPORT BOOL WINAPI AddConsoleAliasW(LPWSTR source, LPWSTR target, LPWSTR exe) {
  if (!source || !*source || !exe || !*exe || wcslen(source) >= 64 || wcslen(exe) >= 64 || (target && wcslen(target) >= 256)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BOOL ok = TRUE;
  alias_lock();
  int slot = -1, free_slot = -1;
  for (int i = 0; i < 64; ++i) {
    if (!g_aliases[i].source[0]) { if (free_slot < 0) free_slot = i; continue; }
    if (alias_match(g_aliases[i].exe, exe) && alias_match(g_aliases[i].source, source)) slot = i;
  }
  if (!target || !*target) {      /* an empty target removes the alias */
    if (slot >= 0) g_aliases[slot].source[0] = 0;
  } else {
    if (slot < 0) slot = free_slot;
    if (slot < 0) { ok = FALSE; set_error(ERROR_NOT_ENOUGH_MEMORY); }
    else {
      memcpy(g_aliases[slot].exe, exe, (wcslen(exe) + 1) * sizeof(WCHAR));
      memcpy(g_aliases[slot].source, source, (wcslen(source) + 1) * sizeof(WCHAR));
      memcpy(g_aliases[slot].target, target, (wcslen(target) + 1) * sizeof(WCHAR));
    }
  }
  alias_unlock();
  return ok;
}
EXPORT DWORD WINAPI GetConsoleAliasW(LPWSTR source, LPWSTR buffer, DWORD bytes, LPWSTR exe) {
  if (!source || !buffer || !exe) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  DWORD result = 0;
  BOOL found = FALSE;
  alias_lock();
  for (int i = 0; i < 64 && !found; ++i) {
    if (!g_aliases[i].source[0] || !alias_match(g_aliases[i].exe, exe) || !alias_match(g_aliases[i].source, source)) continue;
    found = TRUE;
    const DWORD need = (DWORD)(wcslen(g_aliases[i].target) + 1) * sizeof(WCHAR);
    if (bytes < need) set_error(ERROR_INSUFFICIENT_BUFFER);
    else { memcpy(buffer, g_aliases[i].target, need); result = need; }
  }
  alias_unlock();
  if (!found) set_error(1 /* alias not found: ERROR_INVALID_FUNCTION on Windows */);
  return result;
}
/* The aliases of a program as "source=target\0" strings in a row. */
static DWORD alias_list(LPCWSTR exe, WCHAR *buffer, DWORD chars) {
  DWORD used = 0;
  alias_lock();
  for (int i = 0; i < 64; ++i) {
    if (!g_aliases[i].source[0] || !alias_match(g_aliases[i].exe, exe)) continue;
    const DWORD s = (DWORD)wcslen(g_aliases[i].source), t = (DWORD)wcslen(g_aliases[i].target);
    if (buffer && used + s + 1 + t + 1 <= chars) {
      memcpy(buffer + used, g_aliases[i].source, s * sizeof(WCHAR));
      buffer[used + s] = '=';
      memcpy(buffer + used + s + 1, g_aliases[i].target, (t + 1) * sizeof(WCHAR));
    }
    used += s + 1 + t + 1;
  }
  alias_unlock();
  return used;
}
EXPORT DWORD WINAPI GetConsoleAliasesLengthW(LPWSTR exe) { if (!exe) { set_error(ERROR_INVALID_PARAMETER); return 0; } return alias_list(exe, NULL, 0) * sizeof(WCHAR); }
EXPORT DWORD WINAPI GetConsoleAliasesW(LPWSTR buffer, DWORD bytes, LPWSTR exe) {
  if (!buffer || !exe) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const DWORD need = alias_list(exe, NULL, 0) * (DWORD)sizeof(WCHAR);
  if (need > bytes) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return alias_list(exe, buffer, bytes / sizeof(WCHAR)) * (DWORD)sizeof(WCHAR);
}
static DWORD alias_exes(WCHAR *buffer, DWORD chars) {
  DWORD used = 0;
  alias_lock();
  for (int i = 0; i < 64; ++i) {
    if (!g_aliases[i].source[0]) continue;
    BOOL seen = FALSE;
    for (int j = 0; j < i; ++j) if (g_aliases[j].source[0] && alias_match(g_aliases[j].exe, g_aliases[i].exe)) seen = TRUE;
    if (seen) continue;
    const DWORD n = (DWORD)wcslen(g_aliases[i].exe) + 1;
    if (buffer && used + n <= chars) memcpy(buffer + used, g_aliases[i].exe, n * sizeof(WCHAR));
    used += n;
  }
  alias_unlock();
  return used;
}
EXPORT DWORD WINAPI GetConsoleAliasExesLengthW(void) { return alias_exes(NULL, 0) * sizeof(WCHAR); }
EXPORT DWORD WINAPI GetConsoleAliasExesW(LPWSTR buffer, DWORD bytes) {
  if (!buffer) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const DWORD need = alias_exes(NULL, 0) * (DWORD)sizeof(WCHAR);
  if (need > bytes) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return alias_exes(buffer, bytes / sizeof(WCHAR)) * (DWORD)sizeof(WCHAR);
}
/* The ANSI forms convert the arguments and the answer. */
EXPORT BOOL WINAPI AddConsoleAliasA(LPSTR source, LPSTR target, LPSTR exe) {
  WCHAR *s = source ? to_wide(source) : NULL, *t = target ? to_wide(target) : NULL, *e = exe ? to_wide(exe) : NULL;
  const BOOL ok = AddConsoleAliasW(s, t, e);
  free_wide(s); free_wide(t); free_wide(e);
  return ok;
}
EXPORT DWORD WINAPI GetConsoleAliasA(LPSTR source, LPSTR buffer, DWORD bytes, LPSTR exe) {
  WCHAR *s = source ? to_wide(source) : NULL, *e = exe ? to_wide(exe) : NULL, wide[256];
  DWORD result = 0;
  if (s && e && buffer) {
    const DWORD got = GetConsoleAliasW(s, wide, sizeof(wide), e);
    if (got) {
      const int n = WideCharToMultiByte(CP_ACP, 0, wide, -1, buffer, (int)bytes, NULL, NULL);
      if (n > 0) result = (DWORD)n; else set_error(ERROR_INSUFFICIENT_BUFFER);
    }
  } else set_error(ERROR_INVALID_PARAMETER);
  free_wide(s); free_wide(e);
  return result;
}
static DWORD alias_list_fill(WCHAR *buffer, DWORD chars, LPCWSTR exe) { return alias_list(exe, buffer, chars); }
static DWORD alias_text_a(DWORD (*fill)(WCHAR *, DWORD, LPCWSTR), LPCWSTR exe, LPSTR buffer, DWORD bytes, BOOL length_only) {
  const DWORD chars = fill(NULL, 0, exe);
  WCHAR *wide = chars ? RtlAllocateHeap(heap(), 0, (SIZE_T)chars * sizeof(WCHAR)) : NULL;
  if (chars && !wide) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  if (wide) fill(wide, chars, exe);
  const int n = chars ? WideCharToMultiByte(CP_ACP, 0, wide, (int)chars, NULL, 0, NULL, NULL) : 0;
  DWORD result = (DWORD)n;
  if (!length_only && chars) {
    if ((DWORD)n > bytes) { set_error(ERROR_INSUFFICIENT_BUFFER); result = 0; }
    else WideCharToMultiByte(CP_ACP, 0, wide, (int)chars, buffer, (int)bytes, NULL, NULL);
  }
  if (wide) RtlFreeHeap(heap(), 0, wide);
  return result;
}
EXPORT DWORD WINAPI GetConsoleAliasesLengthA(LPSTR exe) { WCHAR *e = exe ? to_wide(exe) : NULL; if (!e) { set_error(ERROR_INVALID_PARAMETER); return 0; } const DWORD n = alias_text_a(alias_list_fill, e, NULL, 0, TRUE); free_wide(e); return n; }
EXPORT DWORD WINAPI GetConsoleAliasesA(LPSTR buffer, DWORD bytes, LPSTR exe) { WCHAR *e = exe ? to_wide(exe) : NULL; if (!e || !buffer) { free_wide(e); set_error(ERROR_INVALID_PARAMETER); return 0; } const DWORD n = alias_text_a(alias_list_fill, e, buffer, bytes, FALSE); free_wide(e); return n; }
static DWORD alias_exes_fill(WCHAR *buffer, DWORD chars, LPCWSTR unused) { (void)unused; return alias_exes(buffer, chars); }
EXPORT DWORD WINAPI GetConsoleAliasExesLengthA(void) { return alias_text_a(alias_exes_fill, NULL, NULL, 0, TRUE); }
EXPORT DWORD WINAPI GetConsoleAliasExesA(LPSTR buffer, DWORD bytes) { if (!buffer) { set_error(ERROR_INVALID_PARAMETER); return 0; } return alias_text_a(alias_exes_fill, NULL, buffer, bytes, FALSE); }
/* Command history: line editing is the terminal's, so no history is kept here; the settings can be made and read back. */
EXPORT DWORD WINAPI GetConsoleCommandHistoryLengthW(LPWSTR exe) { if (!exe) { set_error(ERROR_INVALID_PARAMETER); return 0; } return 0; }
EXPORT DWORD WINAPI GetConsoleCommandHistoryLengthA(LPSTR exe) { if (!exe) { set_error(ERROR_INVALID_PARAMETER); return 0; } return 0; }
EXPORT DWORD WINAPI GetConsoleCommandHistoryW(LPWSTR buffer, DWORD bytes, LPWSTR exe) { (void)bytes; if (!buffer || !exe) { set_error(ERROR_INVALID_PARAMETER); return 0; } return 0; }
EXPORT DWORD WINAPI GetConsoleCommandHistoryA(LPSTR buffer, DWORD bytes, LPSTR exe) { (void)bytes; if (!buffer || !exe) { set_error(ERROR_INVALID_PARAMETER); return 0; } return 0; }
EXPORT void WINAPI ExpungeConsoleCommandHistoryW(LPWSTR exe) { (void)exe; }
EXPORT void WINAPI ExpungeConsoleCommandHistoryA(LPSTR exe) { (void)exe; }
EXPORT BOOL WINAPI SetConsoleNumberOfCommandsW(DWORD count, LPWSTR exe) { if (!exe || count > 0xFFFF) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } g_history_commands = count; return TRUE; }
EXPORT BOOL WINAPI SetConsoleNumberOfCommandsA(DWORD count, LPSTR exe) { if (!exe || count > 0xFFFF) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } g_history_commands = count; return TRUE; }
EXPORT BOOL WINAPI SetConsoleCommandHistoryMode(DWORD mode) { if (mode & ~(DWORD)1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } g_history_mode = mode; return TRUE; }
EXPORT BOOL WINAPI GetConsoleKeyboardLayoutNameW(LPWSTR name) { if (!name) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } static const WCHAR us[] = {'0', '0', '0', '0', '0', '4', '0', '9', 0}; memcpy(name, us, sizeof(us)); return TRUE; }
EXPORT BOOL WINAPI GetConsoleKeyboardLayoutNameA(LPSTR name) { if (!name) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } memcpy(name, "00000409", 9); return TRUE; }
EXPORT BOOL WINAPI ShowConsoleCursor(HANDLE console, BOOL show) {
  CONSOLE_CURSOR_INFO_ info;
  if (!GetConsoleCursorInfo(console, &info)) return FALSE;
  info.visible = show;
  return SetConsoleCursorInfo(console, &info);
}
EXPORT BOOL WINAPI SetConsoleIcon(HANDLE icon) { (void)icon; return TRUE; }      /* (the terminal draws its own) */
EXPORT void WINAPI SetLastConsoleEventActive(void) {}
EXPORT BOOL WINAPI VerifyConsoleIoHandle(HANDLE h) { DWORD mode; return GetConsoleMode(h, &mode); }
EXPORT BOOL WINAPI AllocConsoleWithOptions(const void *options, DWORD *result) {
  if (result) *result = 0;      /* ALLOC_CONSOLE_RESULT_NO_CONSOLE */
  (void)options;
  const BOOL ok = AllocConsole();
  if (ok && result) *result = 1;
  return ok;
}

/* ---- volume mount points, volume label (one drive, C:, with no other volume mounted in it) ------------------------------------------------------------------------- */

EXPORT HANDLE WINAPI FindFirstVolumeMountPointW(LPCWSTR root, LPWSTR name, DWORD capacity) {
  (void)name; (void)capacity;
  if (!root) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (GetDriveTypeW(root) <= 1) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  set_error(ERROR_NO_MORE_FILES);
  return INVALID_HANDLE_VALUE;
}
EXPORT HANDLE WINAPI FindFirstVolumeMountPointA(LPCSTR root, LPSTR name, DWORD capacity) {
  WCHAR *w = root ? to_wide(root) : NULL;
  if (!w) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  HANDLE h = FindFirstVolumeMountPointW(w, NULL, capacity);
  free_wide(w);
  (void)name;
  return h;
}
EXPORT BOOL WINAPI FindNextVolumeMountPointW(HANDLE h, LPWSTR name, DWORD capacity) { (void)h; (void)name; (void)capacity; set_error(ERROR_NO_MORE_FILES); return FALSE; }
EXPORT BOOL WINAPI FindNextVolumeMountPointA(HANDLE h, LPSTR name, DWORD capacity) { (void)h; (void)name; (void)capacity; set_error(ERROR_NO_MORE_FILES); return FALSE; }
EXPORT BOOL WINAPI FindVolumeMountPointClose(HANDLE h) { (void)h; set_error(ERROR_INVALID_HANDLE); return FALSE; }      /* (no search is ever opened) */
EXPORT BOOL WINAPI SetVolumeMountPointW(LPCWSTR path, LPCWSTR volume) { if (!path || !volume) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } set_error(ERROR_ACCESS_DENIED); return FALSE; }
EXPORT BOOL WINAPI SetVolumeMountPointA(LPCSTR path, LPCSTR volume) { if (!path || !volume) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } set_error(ERROR_ACCESS_DENIED); return FALSE; }
EXPORT BOOL WINAPI DeleteVolumeMountPointW(LPCWSTR path) { if (!path) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } set_error(ERROR_ACCESS_DENIED); return FALSE; }
EXPORT BOOL WINAPI DeleteVolumeMountPointA(LPCSTR path) { if (!path) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } set_error(ERROR_ACCESS_DENIED); return FALSE; }
EXPORT BOOL WINAPI SetVolumeLabelW(LPCWSTR root, LPCWSTR label) {
  if (root && GetDriveTypeW(root) <= 1) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
  if (label && wcslen(label) > 32) { set_error(ERROR_LABEL_TOO_LONG); return FALSE; }
  set_error(ERROR_ACCESS_DENIED);      /* (the label of the system volume is the system's) */
  return FALSE;
}
EXPORT BOOL WINAPI SetVolumeLabelA(LPCSTR root, LPCSTR label) {
  WCHAR *r = root ? to_wide(root) : NULL, *l = label ? to_wide(label) : NULL;
  const BOOL ok = SetVolumeLabelW(r, l);
  free_wide(r); free_wide(l);
  return ok;
}
EXPORT BOOL WINAPI SetFileShortNameW(HANDLE file, LPCWSTR name) { (void)file; (void)name; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI SetFileShortNameA(HANDLE file, LPCSTR name) { (void)file; (void)name; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI SetFileIoOverlappedRange(HANDLE file, PUCHAR start, ULONG length) { (void)file; (void)start; (void)length; set_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
EXPORT BOOL WINAPI GetFileBandwidthReservation(HANDLE file, LPDWORD period, LPDWORD bytes, PBOOL discardable, LPDWORD transfer, LPDWORD outstanding) {
  (void)period; (void)bytes; (void)discardable; (void)transfer; (void)outstanding;
  if (GetFileType(file) == 0 && GetLastError()) return FALSE;
  set_error(ERROR_INVALID_FUNCTION);      /* (the volume does not reserve bandwidth) */
  return FALSE;
}
EXPORT BOOL WINAPI SetFileBandwidthReservation(HANDLE file, DWORD period, DWORD bytes, BOOL discardable, LPDWORD transfer, LPDWORD outstanding) {
  (void)period; (void)bytes; (void)discardable; (void)transfer; (void)outstanding;
  if (GetFileType(file) == 0 && GetLastError()) return FALSE;
  set_error(ERROR_INVALID_FUNCTION);
  return FALSE;
}
EXPORT BOOL WINAPI GetThreadIOPendingFlag(HANDLE thread, PBOOL pending) { if (!pending) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } if (!GetThreadId(thread)) return FALSE; *pending = FALSE; return TRUE; }
EXPORT BOOL WINAPI GetNamedPipeAttribute(HANDLE pipe, int attribute, PSTR name, PVOID value, PSIZE_T length) { (void)name; (void)value; (void)length; if (GetFileType(pipe) == 0 && GetLastError()) return FALSE; set_error(attribute >= 0 && attribute <= 2 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI SetNamedPipeAttribute(HANDLE pipe, int attribute, PSTR name, PVOID value, SIZE_T length) { (void)name; (void)value; (void)length; (void)attribute; if (GetFileType(pipe) == 0 && GetLastError()) return FALSE; set_error(ERROR_INVALID_PARAMETER); return FALSE; }

/* ---- files: transacted forms, names of a file, backup streams, directory changes -------------------------------------------------------------------------------- */

/* There is no transaction manager here: a handle to a transaction cannot be made, so a call that names one has an invalid handle. */
static BOOL transaction_ok(HANDLE transaction) {
  if (!transaction || transaction == INVALID_HANDLE_VALUE) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  set_error(6701 /* ERROR_TRANSACTIONAL_CONFLICT: the handle is not one of a transaction */);
  return FALSE;
}
#define TRANSACTED_W(ret, fail, name, params, args) EXPORT ret WINAPI name params { if (!transaction_ok(transaction)) return fail; return 0; }
EXPORT HANDLE WINAPI CreateFileTransactedW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file, HANDLE transaction, PUSHORT_ mini_version, PVOID extended) {
  (void)path; (void)access; (void)share; (void)sa; (void)disposition; (void)flags; (void)template_file; (void)mini_version; (void)extended;
  transaction_ok(transaction);
  return INVALID_HANDLE_VALUE;
}
EXPORT HANDLE WINAPI CreateFileTransactedA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file, HANDLE transaction, PUSHORT_ mini_version, PVOID extended) {
  (void)path; (void)access; (void)share; (void)sa; (void)disposition; (void)flags; (void)template_file; (void)mini_version; (void)extended;
  transaction_ok(transaction);
  return INVALID_HANDLE_VALUE;
}
EXPORT BOOL WINAPI CreateDirectoryTransactedW(LPCWSTR tmpl, LPCWSTR path, LPSECURITY_ATTRIBUTES sa, HANDLE transaction) { (void)tmpl; (void)path; (void)sa; return transaction_ok(transaction); }
EXPORT BOOL WINAPI CreateDirectoryTransactedA(LPCSTR tmpl, LPCSTR path, LPSECURITY_ATTRIBUTES sa, HANDLE transaction) { (void)tmpl; (void)path; (void)sa; return transaction_ok(transaction); }
EXPORT BOOL WINAPI RemoveDirectoryTransactedW(LPCWSTR path, HANDLE transaction) { (void)path; return transaction_ok(transaction); }
EXPORT BOOL WINAPI RemoveDirectoryTransactedA(LPCSTR path, HANDLE transaction) { (void)path; return transaction_ok(transaction); }
EXPORT BOOL WINAPI DeleteFileTransactedW(LPCWSTR path, HANDLE transaction) { (void)path; return transaction_ok(transaction); }
EXPORT BOOL WINAPI DeleteFileTransactedA(LPCSTR path, HANDLE transaction) { (void)path; return transaction_ok(transaction); }
EXPORT BOOL WINAPI CopyFileTransactedW(LPCWSTR from, LPCWSTR to, PVOID progress, LPVOID data, LPBOOL cancel, DWORD flags, HANDLE transaction) { (void)from; (void)to; (void)progress; (void)data; (void)cancel; (void)flags; return transaction_ok(transaction); }
EXPORT BOOL WINAPI CopyFileTransactedA(LPCSTR from, LPCSTR to, PVOID progress, LPVOID data, LPBOOL cancel, DWORD flags, HANDLE transaction) { (void)from; (void)to; (void)progress; (void)data; (void)cancel; (void)flags; return transaction_ok(transaction); }
EXPORT BOOL WINAPI MoveFileTransactedW(LPCWSTR from, LPCWSTR to, PVOID progress, LPVOID data, DWORD flags, HANDLE transaction) { (void)from; (void)to; (void)progress; (void)data; (void)flags; return transaction_ok(transaction); }
EXPORT BOOL WINAPI MoveFileTransactedA(LPCSTR from, LPCSTR to, PVOID progress, LPVOID data, DWORD flags, HANDLE transaction) { (void)from; (void)to; (void)progress; (void)data; (void)flags; return transaction_ok(transaction); }
EXPORT DWORD WINAPI GetFileAttributesTransactedW(LPCWSTR path, int level, LPVOID info, HANDLE transaction) { (void)path; (void)level; (void)info; return transaction_ok(transaction) ? 0 : 0; }
EXPORT DWORD WINAPI GetFileAttributesTransactedA(LPCSTR path, int level, LPVOID info, HANDLE transaction) { (void)path; (void)level; (void)info; return transaction_ok(transaction) ? 0 : 0; }
EXPORT BOOL WINAPI SetFileAttributesTransactedW(LPCWSTR path, DWORD attributes, HANDLE transaction) { (void)path; (void)attributes; return transaction_ok(transaction); }
EXPORT BOOL WINAPI SetFileAttributesTransactedA(LPCSTR path, DWORD attributes, HANDLE transaction) { (void)path; (void)attributes; return transaction_ok(transaction); }
EXPORT DWORD WINAPI GetCompressedFileSizeTransactedW(LPCWSTR path, LPDWORD high, HANDLE transaction) { (void)path; (void)high; transaction_ok(transaction); return 0xFFFFFFFFu; }
EXPORT DWORD WINAPI GetCompressedFileSizeTransactedA(LPCSTR path, LPDWORD high, HANDLE transaction) { (void)path; (void)high; transaction_ok(transaction); return 0xFFFFFFFFu; }
EXPORT DWORD WINAPI GetFullPathNameTransactedW(LPCWSTR path, DWORD size, LPWSTR buffer, LPWSTR *part, HANDLE transaction) { (void)path; (void)size; (void)buffer; (void)part; transaction_ok(transaction); return 0; }
EXPORT DWORD WINAPI GetFullPathNameTransactedA(LPCSTR path, DWORD size, LPSTR buffer, LPSTR *part, HANDLE transaction) { (void)path; (void)size; (void)buffer; (void)part; transaction_ok(transaction); return 0; }
EXPORT DWORD WINAPI GetLongPathNameTransactedW(LPCWSTR path, LPWSTR buffer, DWORD size, HANDLE transaction) { (void)path; (void)buffer; (void)size; transaction_ok(transaction); return 0; }
EXPORT DWORD WINAPI GetLongPathNameTransactedA(LPCSTR path, LPSTR buffer, DWORD size, HANDLE transaction) { (void)path; (void)buffer; (void)size; transaction_ok(transaction); return 0; }
EXPORT BOOL WINAPI CreateHardLinkTransactedW(LPCWSTR link, LPCWSTR target, LPSECURITY_ATTRIBUTES sa, HANDLE transaction) { (void)link; (void)target; (void)sa; return transaction_ok(transaction); }
EXPORT BOOL WINAPI CreateHardLinkTransactedA(LPCSTR link, LPCSTR target, LPSECURITY_ATTRIBUTES sa, HANDLE transaction) { (void)link; (void)target; (void)sa; return transaction_ok(transaction); }
EXPORT BOOLEAN WINAPI CreateSymbolicLinkTransactedW(LPCWSTR link, LPCWSTR target, DWORD flags, HANDLE transaction) { (void)link; (void)target; (void)flags; return (BOOLEAN)transaction_ok(transaction); }
EXPORT BOOLEAN WINAPI CreateSymbolicLinkTransactedA(LPCSTR link, LPCSTR target, DWORD flags, HANDLE transaction) { (void)link; (void)target; (void)flags; return (BOOLEAN)transaction_ok(transaction); }
EXPORT HANDLE WINAPI FindFirstFileTransactedW(LPCWSTR path, int level, LPVOID data, int op, LPVOID filter, DWORD flags, HANDLE transaction) { (void)path; (void)level; (void)data; (void)op; (void)filter; (void)flags; transaction_ok(transaction); return INVALID_HANDLE_VALUE; }
EXPORT HANDLE WINAPI FindFirstFileTransactedA(LPCSTR path, int level, LPVOID data, int op, LPVOID filter, DWORD flags, HANDLE transaction) { (void)path; (void)level; (void)data; (void)op; (void)filter; (void)flags; transaction_ok(transaction); return INVALID_HANDLE_VALUE; }
EXPORT HANDLE WINAPI FindFirstFileNameTransactedW(LPCWSTR path, DWORD flags, LPDWORD length, PWSTR name, HANDLE transaction) { (void)path; (void)flags; (void)length; (void)name; transaction_ok(transaction); return INVALID_HANDLE_VALUE; }
EXPORT HANDLE WINAPI FindFirstStreamTransactedW(LPCWSTR path, int level, LPVOID data, DWORD flags, HANDLE transaction) { (void)path; (void)level; (void)data; (void)flags; transaction_ok(transaction); return INVALID_HANDLE_VALUE; }

/* The names of a file: FindFirstFileNameW lists every hard link of it as a path from the root of the volume. A file with one link has one name; for more the volume is walked for
 * entries with the same file index. The names are kept in the buffer of a FindState (FindClose frees it). */
typedef struct { HANDLE directory; BYTE buffer[4096]; ULONG offset, used; BOOLEAN started, finished; WCHAR pattern[MAX_PATH]; } NameSearch;
static BOOL name_add(NameSearch *s, const WCHAR *path) {
  const ULONG bytes = (ULONG)(wcslen(path) + 1) * sizeof(WCHAR);
  if (s->used + bytes > sizeof(s->buffer)) return FALSE;
  memcpy(s->buffer + s->used, path, bytes);
  s->used += bytes;
  return TRUE;
}
static void names_walk(NameSearch *s, WCHAR *dir, SIZE_T dir_length, ULONGLONG index, DWORD volume, int depth) {
  WCHAR pattern[MAX_PATH + 4];
  if (depth > 16 || dir_length + 3 >= MAX_PATH) return;
  memcpy(pattern, dir, dir_length * sizeof(WCHAR));
  pattern[dir_length] = '*'; pattern[dir_length + 1] = 0;
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW(pattern, &entry);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    const SIZE_T n = wcslen(entry.cFileName);
    if ((n == 1 && entry.cFileName[0] == '.') || (n == 2 && entry.cFileName[0] == '.' && entry.cFileName[1] == '.') || dir_length + n + 2 >= MAX_PATH) continue;
    memcpy(dir + dir_length, entry.cFileName, (n + 1) * sizeof(WCHAR));
    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!(entry.dwFileAttributes & 0x400 /* FILE_ATTRIBUTE_REPARSE_POINT */)) { dir[dir_length + n] = '\\'; dir[dir_length + n + 1] = 0; names_walk(s, dir, dir_length + n + 1, index, volume, depth + 1); }
    } else {
      HANDLE f = CreateFileW(dir, 0, 7, NULL, 3, 0x02000000 /* FILE_FLAG_BACKUP_SEMANTICS */, NULL);
      if (f != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info;
        if (GetFileInformationByHandle(f, &info) && info.dwVolumeSerialNumber == volume && (((ULONGLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow) == index) name_add(s, dir + 2);
        CloseHandle(f);
      }
    }
    dir[dir_length] = 0;
  } while (FindNextFileW(find, &entry));
  FindClose(find);
}
EXPORT HANDLE WINAPI FindFirstFileNameW(LPCWSTR path, DWORD flags, LPDWORD length, PWSTR name) {
  if (!path || flags || !length || (*length && !name)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  HANDLE f = CreateFileW(path, 0, 7, NULL, 3, 0x02000000, NULL);
  if (f == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
  BY_HANDLE_FILE_INFORMATION info;
  const BOOL got = GetFileInformationByHandle(f, &info);
  CloseHandle(f);
  if (!got) return INVALID_HANDLE_VALUE;
  NameSearch *s = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(NameSearch));
  if (!s) { set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  WCHAR full[MAX_PATH];
  const DWORD n = GetFullPathNameW(path, MAX_PATH, full, NULL);
  if (!n || n >= MAX_PATH) { RtlFreeHeap(heap(), 0, s); set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (info.nNumberOfLinks <= 1 || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    name_add(s, full + 2);
  } else {
    WCHAR root[MAX_PATH] = {full[0], ':', '\\', 0};
    names_walk(s, root, 3, ((ULONGLONG)info.nFileIndexHigh << 32) | info.nFileIndexLow, info.dwVolumeSerialNumber, 0);
    if (!s->used) name_add(s, full + 2);
  }
  s->started = TRUE;
  const WCHAR *first = (const WCHAR *)s->buffer;
  const DWORD need = (DWORD)wcslen(first) + 1;
  s->offset = need * sizeof(WCHAR);
  if (*length < need) { *length = need; set_error(ERROR_MORE_DATA); s->offset = 0; if (!s->used) {} RtlFreeHeap(heap(), 0, s); return INVALID_HANDLE_VALUE; }
  memcpy(name, first, need * sizeof(WCHAR));
  *length = need;
  return s;
}
EXPORT BOOL WINAPI FindNextFileNameW(HANDLE h, LPDWORD length, PWSTR name) {
  NameSearch *s = h;
  if (!h || h == INVALID_HANDLE_VALUE || !length) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (s->offset >= s->used) { set_error(ERROR_HANDLE_EOF); return FALSE; }
  const WCHAR *next = (const WCHAR *)(s->buffer + s->offset);
  const DWORD need = (DWORD)wcslen(next) + 1;
  if (*length < need || !name) { *length = need; set_error(ERROR_MORE_DATA); return FALSE; }
  memcpy(name, next, need * sizeof(WCHAR));
  *length = need;
  s->offset += need * sizeof(WCHAR);
  return TRUE;
}

/* GetFileInformationByName (Windows 11): FileStatBasicByNameInfo only. */
EXPORT BOOL WINAPI GetFileInformationByName(LPCWSTR path, int klass, PVOID info, ULONG size) {
  if (!path || !info || klass < 0 || klass > 1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (size < 0x68) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  HANDLE f = CreateFileW(path, 0x80 /* FILE_READ_ATTRIBUTES */, 7, NULL, 3, 0x02000000, NULL);
  if (f == INVALID_HANDLE_VALUE) return FALSE;
  BY_HANDLE_FILE_INFORMATION h;
  const BOOL ok = GetFileInformationByHandle(f, &h);
  CloseHandle(f);
  if (!ok) return FALSE;
  BYTE *o = info;
  memset(o, 0, 0x68);
  const ULONGLONG file_size = ((ULONGLONG)h.nFileSizeHigh << 32) | h.nFileSizeLow;
  *(ULONGLONG *)(o + 0x00) = ((ULONGLONG)h.nFileIndexHigh << 32) | h.nFileIndexLow;      /* FileId */
  *(ULONGLONG *)(o + 0x08) = ((ULONGLONG)h.ftCreationTime.dwHighDateTime << 32) | h.ftCreationTime.dwLowDateTime;
  *(ULONGLONG *)(o + 0x10) = ((ULONGLONG)h.ftLastAccessTime.dwHighDateTime << 32) | h.ftLastAccessTime.dwLowDateTime;
  *(ULONGLONG *)(o + 0x18) = ((ULONGLONG)h.ftLastWriteTime.dwHighDateTime << 32) | h.ftLastWriteTime.dwLowDateTime;
  *(ULONGLONG *)(o + 0x20) = *(ULONGLONG *)(o + 0x18);      /* ChangeTime */
  *(ULONGLONG *)(o + 0x28) = (file_size + 4095) & ~(ULONGLONG)4095;      /* AllocationSize */
  *(ULONGLONG *)(o + 0x30) = file_size;
  *(DWORD *)(o + 0x38) = h.dwFileAttributes;
  *(DWORD *)(o + 0x40) = h.nNumberOfLinks;
  *(DWORD *)(o + 0x44) = 7 /* FILE_DEVICE_DISK_FILE_SYSTEM... a disk */;
  *(DWORD *)(o + 0x58) = h.dwVolumeSerialNumber;
  return TRUE;
}
/* GetFileMUIPath: this OS keeps no language-specific resource files (".mui") next to its programs. */
EXPORT BOOL WINAPI GetFileMUIPath(DWORD flags, LPCWSTR path, LPWSTR language, PULONG language_length, LPWSTR file_mui, PULONG file_mui_length, PULONGLONG enumerator) {
  (void)language; (void)file_mui;
  if (!path || !language_length || !file_mui_length || !enumerator || (flags & ~(DWORD)0x1F)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  set_error(ERROR_FILE_NOT_FOUND);
  return FALSE;
}

/* BackupRead / BackupWrite / BackupSeek: the data stream of a file as a WIN32_STREAM_ID header (BACKUP_DATA) followed by the bytes; other streams (security, links, ...) do not exist
 * here, and a stream written is restored if it is BACKUP_DATA and skipped otherwise. */
typedef struct { int phase; ULONGLONG remaining; BYTE header[20]; DWORD header_done; DWORD stream_name_left; } BackupContext;
EXPORT BOOL WINAPI BackupRead(HANDLE file, BYTE * buffer, DWORD to_read, LPDWORD read, BOOL abort, BOOL security, LPVOID *context) {
  (void)security;
  if (read) *read = 0;
  if (!context) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BackupContext *c = *context;
  if (abort) { if (c) { RtlFreeHeap(heap(), 0, c); *context = NULL; } return TRUE; }
  if (!buffer || !read) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!c) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) return FALSE;
    c = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    *(DWORD *)(c->header + 0) = 1;      /* BACKUP_DATA */
    *(ULONGLONG *)(c->header + 8) = (ULONGLONG)size.QuadPart;
    c->remaining = (ULONGLONG)size.QuadPart;
    *context = c;
  }
  DWORD done = 0;
  while (done < to_read && c->phase < 2) {
    if (c->phase == 0) {
      const DWORD n = (to_read - done) < (20 - c->header_done) ? (to_read - done) : (20 - c->header_done);
      memcpy(buffer + done, c->header + c->header_done, n);
      c->header_done += n; done += n;
      if (c->header_done == 20) c->phase = c->remaining ? 1 : 2;
    } else {
      DWORD want = to_read - done;
      if (want > c->remaining) want = (DWORD)c->remaining;
      DWORD got = 0;
      if (!ReadFile(file, buffer + done, want, &got, NULL)) { *read = done; return FALSE; }
      if (got == 0) { c->phase = 2; break; }
      done += got; c->remaining -= got;
      if (!c->remaining) c->phase = 2;
    }
  }
  *read = done;
  return TRUE;
}
EXPORT BOOL WINAPI BackupSeek(HANDLE file, DWORD low, DWORD high, LPDWORD seeked_low, LPDWORD seeked_high, LPVOID *context) {
  if (!context || !seeked_low || !seeked_high) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BackupContext *c = *context;
  *seeked_low = *seeked_high = 0;
  if (!c || c->phase != 1) return FALSE;      /* (only inside the data stream) */
  ULONGLONG n = ((ULONGLONG)high << 32) | low;
  if (n > c->remaining) n = c->remaining;
  LARGE_INTEGER d, now;
  d.QuadPart = (LONGLONG)n;
  if (!SetFilePointerEx(file, d, &now, 1)) return FALSE;
  c->remaining -= n;
  if (!c->remaining) c->phase = 2;
  *seeked_low = (DWORD)n; *seeked_high = (DWORD)(n >> 32);
  return TRUE;
}
EXPORT BOOL WINAPI BackupWrite(HANDLE file, BYTE * buffer, DWORD to_write, LPDWORD written, BOOL abort, BOOL security, LPVOID *context) {
  (void)security;
  if (written) *written = 0;
  if (!context) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BackupContext *c = *context;
  if (abort) { if (c) { RtlFreeHeap(heap(), 0, c); *context = NULL; } return TRUE; }
  if (!buffer || !written) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!c) {
    c = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    *context = c;
  }
  DWORD done = 0;
  while (done < to_write) {
    if (c->phase == 0 || c->phase == 2) {      /* reading a header (2: the one after a stream) */
      const DWORD n = (to_write - done) < (20 - c->header_done) ? (to_write - done) : (20 - c->header_done);
      memcpy(c->header + c->header_done, buffer + done, n);
      c->header_done += n; done += n;
      if (c->header_done < 20) break;
      c->header_done = 0;
      c->remaining = *(ULONGLONG *)(c->header + 8);
      c->stream_name_left = *(DWORD *)(c->header + 16);
      c->phase = *(DWORD *)(c->header + 0) == 1 ? 1 : 3;      /* data, or a stream to skip */
      if (c->stream_name_left) { c->phase = c->phase == 1 ? 4 : 5; }
      else if (!c->remaining) c->phase = 2;
    } else if (c->phase >= 4) {      /* the name of the stream is skipped */
      DWORD n = to_write - done;
      if (n > c->stream_name_left) n = c->stream_name_left;
      done += n; c->stream_name_left -= n;
      if (!c->stream_name_left) c->phase = c->phase == 4 ? 1 : 3;
      if (!c->stream_name_left && !c->remaining) c->phase = 2;
    } else {
      DWORD n = to_write - done;
      if (n > c->remaining) n = (DWORD)c->remaining;
      if (c->phase == 1) {
        DWORD put = 0;
        if (!WriteFile(file, buffer + done, n, &put, NULL)) { *written = done; return FALSE; }
      }
      done += n; c->remaining -= n;
      if (!c->remaining) c->phase = 2;
    }
  }
  *written = done;
  return TRUE;
}

/* ReadDirectoryChangesExW: ReadDirectoryNotifyInformation (1) is ReadDirectoryChangesW; ReadDirectoryNotifyExtendedInformation (2) is the same notifications with the file's size
 * and attributes added, made here from the standard ones when the call is synchronous. */
EXPORT BOOL WINAPI ReadDirectoryChangesExW(HANDLE directory, LPVOID buffer, DWORD length, BOOL subtree, DWORD filter, LPDWORD returned, LPOVERLAPPED overlapped, LPVOID completion, int klass) {
  if (klass == 1) return ReadDirectoryChangesW(directory, buffer, length, subtree, filter, returned, overlapped, completion);
  if (klass != 2 || !buffer) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (overlapped || completion) { set_error(ERROR_NOT_SUPPORTED); return FALSE; }
  BYTE standard[4096];
  DWORD got = 0;
  if (!ReadDirectoryChangesW(directory, standard, sizeof(standard) < length ? sizeof(standard) : length, subtree, filter, &got, NULL, NULL)) return FALSE;
  WCHAR base[MAX_PATH];
  const DWORD base_length = GetFinalPathNameByHandleW(directory, base, MAX_PATH - 2, 0);
  DWORD in = 0, out = 0, previous = (DWORD)-1;
  while (in < got) {
    const BYTE *r = standard + in;
    const DWORD next = *(const DWORD *)r, action = *(const DWORD *)(r + 4), name_bytes = *(const DWORD *)(r + 8);
    const WCHAR *name = (const WCHAR *)(r + 12);
    const DWORD record = (0x58 + name_bytes + 7) & ~7u;      /* FILE_NOTIFY_EXTENDED_INFORMATION: 0x54 bytes, then the name */
    if (out + record > length) { set_error(1022); return FALSE; }
    BYTE *o = (BYTE *)buffer + out;
    memset(o, 0, record);
    *(DWORD *)(o + 4) = action;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (base_length && base_length + 1 + name_bytes / 2 < MAX_PATH) {
      WCHAR path[MAX_PATH];
      memcpy(path, base, base_length * sizeof(WCHAR));
      path[base_length] = '\\';
      memcpy(path + base_length + 1, name, name_bytes);
      path[base_length + 1 + name_bytes / 2] = 0;
      if (GetFileAttributesExW(path, 0, &data)) {
        *(ULONGLONG *)(o + 0x08) = ((ULONGLONG)data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
        *(ULONGLONG *)(o + 0x10) = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
        *(ULONGLONG *)(o + 0x18) = *(ULONGLONG *)(o + 0x10);
        *(ULONGLONG *)(o + 0x20) = ((ULONGLONG)data.ftLastAccessTime.dwHighDateTime << 32) | data.ftLastAccessTime.dwLowDateTime;
        *(ULONGLONG *)(o + 0x28) = (((ULONGLONG)data.nFileSizeHigh << 32 | data.nFileSizeLow) + 4095) & ~(ULONGLONG)4095;
        *(ULONGLONG *)(o + 0x30) = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        *(DWORD *)(o + 0x38) = data.dwFileAttributes;
      }
    }
    *(DWORD *)(o + 0x50) = name_bytes;
    memcpy(o + 0x54, name, name_bytes);
    if (previous != (DWORD)-1) *(DWORD *)((BYTE *)buffer + previous) = out - previous;
    previous = out;
    out += record;
    if (!next) break;
    in += next;
  }
  if (returned) *returned = out;
  return TRUE;
}

/* ---- thread pool: I/O completion, wait and timer variants, stack size --------------------------------------------------------------------------------------------- */

typedef void (WINAPI *TP_WIN32_IO_CALLBACK_)(PVOID instance, PVOID context, PVOID overlapped, ULONG result, ULONG_PTR bytes, PVOID io);
typedef struct TpIo {
  HANDLE file, port, thread;
  TP_WIN32_IO_CALLBACK_ callback;
  PVOID context;
  volatile LONG pending, running;
} TpIo;
#define TPIO_QUIT ((ULONG_PTR)0xB10CB10C)
static DWORD WINAPI tpio_thread(LPVOID parameter) {
  TpIo *io = parameter;
  for (;;) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED overlapped = NULL;
    GetQueuedCompletionStatus(io->port, &bytes, &key, &overlapped, INFINITE);
    if (key == TPIO_QUIT && !overlapped) break;
    if (!overlapped) continue;
    __sync_fetch_and_add(&io->running, 1);
    if (io->pending > 0) __sync_fetch_and_sub(&io->pending, 1);
    io->callback(NULL, io->context, overlapped, RtlNtStatusToDosError((NTSTATUS)overlapped->Internal), bytes, io);
    __sync_fetch_and_sub(&io->running, 1);
  }
  return 0;
}
EXPORT PVOID WINAPI CreateThreadpoolIo(HANDLE file, PVOID callback, PVOID context, PVOID environment) {
  (void)environment;
  if (!callback) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  if (!file || file == INVALID_HANDLE_VALUE) { set_error(ERROR_INVALID_HANDLE); return NULL; }
  if (!overlapped_known(file)) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  TpIo *io = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(*io));
  if (!io) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  io->file = file; io->callback = callback; io->context = context;
  io->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
  if (!io->port || !CreateIoCompletionPort(file, io->port, 1, 0)) {
    const DWORD error = GetLastError();
    if (io->port) CloseHandle(io->port);
    RtlFreeHeap(heap(), 0, io);
    set_error(error);
    return NULL;
  }
  io->thread = CreateThread(NULL, 0, tpio_thread, io, 0, NULL);
  if (!io->thread) { CloseHandle(io->port); RtlFreeHeap(heap(), 0, io); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  return io;
}
EXPORT void WINAPI StartThreadpoolIo(PVOID io) { __sync_fetch_and_add(&((TpIo *)io)->pending, 1); }
EXPORT void WINAPI CancelThreadpoolIo(PVOID io) { TpIo *t = io; if (t->pending > 0) __sync_fetch_and_sub(&t->pending, 1); }
EXPORT void WINAPI WaitForThreadpoolIoCallbacks(PVOID io, BOOL cancel_pending) {
  TpIo *t = io;
  if (cancel_pending) t->pending = 0;
  while (t->pending > 0 || t->running > 0) Sleep(1);
}
EXPORT void WINAPI CloseThreadpoolIo(PVOID io) {
  TpIo *t = io;
  PostQueuedCompletionStatus(t->port, 0, TPIO_QUIT, NULL);
  WaitForSingleObject(t->thread, INFINITE);
  CloseHandle(t->thread);
  CloseHandle(t->port);
  RtlFreeHeap(heap(), 0, t);
}
EXPORT BOOL WINAPI SetThreadpoolTimerEx(PVOID timer, PFILETIME due, DWORD period, DWORD window) {
  const BOOL was_set = IsThreadpoolTimerSet(timer);
  SetThreadpoolTimer(timer, due, period, window);
  return was_set;
}
EXPORT BOOL WINAPI SetThreadpoolWaitEx(PVOID wait, HANDLE handle, PFILETIME timeout, PVOID reserved) {
  (void)reserved;
  const BOOL was_set = ((TpWait *)wait)->thread != NULL;
  SetThreadpoolWait(wait, handle, timeout);
  return was_set;
}
EXPORT HANDLE WINAPI SetTimerQueueTimer(HANDLE queue, WAITORTIMERCALLBACK_ callback, PVOID parameter, DWORD due, DWORD period, BOOL prefer_io) {
  HANDLE timer = NULL;
  if (!CreateTimerQueueTimer(&timer, queue, callback, parameter, due, period, prefer_io ? 0x00000008 /* WT_EXECUTEINIOTHREAD */ : 0)) return NULL;
  return timer;
}
EXPORT HANDLE WINAPI RegisterWaitForSingleObjectEx(HANDLE object, WAITORTIMERCALLBACK_ callback, PVOID context, ULONG timeout, ULONG flags) {
  HANDLE wait = NULL;
  if (!RegisterWaitForSingleObject(&wait, object, callback, context, timeout, flags)) return NULL;
  return wait;
}
typedef struct { SIZE_T reserve, commit; } TP_POOL_STACK_INFORMATION_;
static TP_POOL_STACK_INFORMATION_ g_pool_stack = {0x100000, 0x2000};
EXPORT BOOL WINAPI SetThreadpoolStackInformation(PVOID pool, TP_POOL_STACK_INFORMATION_ *information) {
  (void)pool;
  if (!information || information->commit > information->reserve) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  g_pool_stack = *information;
  return TRUE;
}
EXPORT BOOL WINAPI QueryThreadpoolStackInformation(PVOID pool, TP_POOL_STACK_INFORMATION_ *information) {
  (void)pool;
  if (!information) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *information = g_pool_stack;
  return TRUE;
}

/* ---- private namespaces and boundary descriptors ----------------------------------------------------------------------------------------------------------------------- */

/* A boundary descriptor is its name (the SIDs and integrity labels that fence a namespace off are checked by the object manager of a system with users, which this has one of). A
 * private namespace is a directory of the session's object directory with the alias for a name; names given as "Alias\Object" land in it. */
#define BOUNDARY_MAGIC 0x444E5542u
typedef struct { DWORD magic; DWORD sids, labels; WCHAR name[64]; } BoundaryDescriptor;
EXPORT HANDLE WINAPI CreateBoundaryDescriptorW(LPCWSTR name, ULONG flags) {
  if (!name || flags || !*name || wcslen(name) >= 64) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  BoundaryDescriptor *b = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(*b));
  if (!b) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  b->magic = BOUNDARY_MAGIC;
  memcpy(b->name, name, (wcslen(name) + 1) * sizeof(WCHAR));
  return b;
}
EXPORT HANDLE WINAPI CreateBoundaryDescriptorA(LPCSTR name, ULONG flags) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (!w) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE h = CreateBoundaryDescriptorW(w, flags);
  free_wide(w);
  return h;
}
static BoundaryDescriptor *boundary_of(HANDLE h) { BoundaryDescriptor *b = h; return b && b->magic == BOUNDARY_MAGIC ? b : NULL; }
EXPORT BOOL WINAPI AddSIDToBoundaryDescriptor(HANDLE *descriptor, PVOID sid) {
  BoundaryDescriptor *b = descriptor ? boundary_of(*descriptor) : NULL;
  if (!b || !sid) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  ++b->sids;
  return TRUE;
}
EXPORT BOOL WINAPI AddIntegrityLabelToBoundaryDescriptor(HANDLE *descriptor, PVOID sid) {
  BoundaryDescriptor *b = descriptor ? boundary_of(*descriptor) : NULL;
  if (!b || !sid) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  ++b->labels;
  return TRUE;
}
EXPORT void WINAPI DeleteBoundaryDescriptor(HANDLE descriptor) {
  BoundaryDescriptor *b = boundary_of(descriptor);
  if (!b) return;
  b->magic = 0;
  RtlFreeHeap(heap(), 0, b);
}
EXPORT HANDLE WINAPI CreatePrivateNamespaceW(LPSECURITY_ATTRIBUTES sa, LPVOID boundary, LPCWSTR alias) {
  if (!boundary_of(boundary) || !alias || !*alias) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  for (const WCHAR *c = alias; *c; ++c) if (*c == '\\') { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  NamedAttributes n;
  POBJECT_ATTRIBUTES a = named_attributes_sa(&n, alias, sa);
  if (!a) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  a->Attributes &= ~(ULONG)0x80;      /* (a namespace that is there already is an error) */
  HANDLE h = NULL;
  const NTSTATUS status = NtCreateDirectoryObject(&h, 0x000F000F /* DIRECTORY_ALL_ACCESS */, a);
  if (status == (NTSTATUS)0xC0000035 /* STATUS_OBJECT_NAME_COLLISION */) { set_error(ERROR_ALREADY_EXISTS); return NULL; }
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return h;
}
EXPORT HANDLE WINAPI CreatePrivateNamespaceA(LPSECURITY_ATTRIBUTES sa, LPVOID boundary, LPCSTR alias) {
  WCHAR *w = alias ? to_wide(alias) : NULL;
  if (!w) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE h = CreatePrivateNamespaceW(sa, boundary, w);
  free_wide(w);
  return h;
}
EXPORT HANDLE WINAPI OpenPrivateNamespaceW(LPVOID boundary, LPCWSTR alias) {
  if (!boundary_of(boundary) || !alias || !*alias) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  NamedAttributes n;
  POBJECT_ATTRIBUTES a = named_attributes(&n, alias, FALSE);
  if (!a) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  a->Attributes &= ~(ULONG)0x80;
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenDirectoryObject(&h, 0x000F000F, a);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return h;
}
EXPORT HANDLE WINAPI OpenPrivateNamespaceA(LPVOID boundary, LPCSTR alias) {
  WCHAR *w = alias ? to_wide(alias) : NULL;
  if (!w) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE h = OpenPrivateNamespaceW(boundary, w);
  free_wide(w);
  return h;
}
EXPORT BOOLEAN WINAPI ClosePrivateNamespace(HANDLE ns, ULONG flags) {
  if (flags & ~(ULONG)1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return (BOOLEAN)CloseHandle(ns);
}

/* ---- memory resource notification: an event that is set while memory is plentiful (High) or short (Low); this machine reports plenty ----------------------------------- */

EXPORT HANDLE WINAPI CreateMemoryResourceNotification(int type) {
  if (type != 0 && type != 1) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  return CreateEventW(NULL, TRUE, type == 1 /* HighMemoryResourceNotification */, NULL);
}
EXPORT BOOL WINAPI QueryMemoryResourceNotification(HANDLE h, PBOOL state) {
  if (!state) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD r = WaitForSingleObject(h, 0);
  if (r == 0xFFFFFFFFu) return FALSE;
  *state = r == 0;
  return TRUE;
}

/* ---- psapi ----------------------------------------------------------------------------------------------------------------------------------------------------------- */

static DWORD psapi_protection(DWORD p) {
  switch (p & 0xFF) { case 0x01: return 0; case 0x02: return 1; case 0x10: return 2; case 0x20: return 3; case 0x04: return 4; case 0x08: return 5; case 0x40: return 6; case 0x80: return 7; default: return 0; }
}
#define USER_TOP ((ULONG_PTR)0x00007FFFFFFF0000ull)
EXPORT BOOL WINAPI K32QueryWorkingSet(HANDLE process, PVOID buffer, DWORD size) {
  if (!buffer || size < sizeof(ULONG_PTR)) { set_error(24 /* ERROR_BAD_LENGTH */); return FALSE; }
  ULONG_PTR *out = buffer;
  ULONG_PTR count = 0;
  const ULONG_PTR capacity = (size - sizeof(ULONG_PTR)) / sizeof(ULONG_PTR);
  MEMORY_BASIC_INFORMATION info;
  for (ULONG_PTR at = 0; at < USER_TOP;) {
    if (!VirtualQueryEx(process, (LPCVOID)at, &info, sizeof(info))) break;
    if (info.State == 0x1000) {
      for (ULONG_PTR page = (ULONG_PTR)info.BaseAddress; page < (ULONG_PTR)info.BaseAddress + info.RegionSize; page += 0x1000) {
        if (count < capacity) out[1 + count] = (page & ~(ULONG_PTR)0xFFF) | psapi_protection(info.Protect) | (info.Type == 0x40000 /* MEM_MAPPED */ ? 0x100 : 0);
        ++count;
      }
    }
    const ULONG_PTR next = (ULONG_PTR)info.BaseAddress + info.RegionSize;
    if (next <= at) break;
    at = next;
  }
  out[0] = count;
  if (count > capacity) { set_error(24); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI K32QueryWorkingSetEx(HANDLE process, PVOID buffer, DWORD size) {
  if (!buffer || size % 16) { set_error(24); return FALSE; }
  ULONG_PTR (*entries)[2] = buffer;
  MEMORY_BASIC_INFORMATION info;
  for (DWORD i = 0; i < size / 16; ++i) {
    ULONG_PTR attributes = 0;
    if (VirtualQueryEx(process, (LPCVOID)entries[i][0], &info, sizeof(info)) && info.State == 0x1000)
      attributes = 1 | ((ULONG_PTR)info.Protect << 4) | (info.Type == 0x40000 ? (ULONG_PTR)1 << 15 : 0);
    entries[i][1] = attributes;
  }
  return TRUE;
}
EXPORT BOOL WINAPI K32InitializeProcessForWsWatch(HANDLE process) { BOOL self; return process_handle_is(process, &self); }
EXPORT BOOL WINAPI K32GetWsChanges(HANDLE process, PVOID watch, DWORD size) {
  BOOL self;
  if (!watch || size < 16) { set_error(24); return FALSE; }
  if (!process_handle_is(process, &self)) return FALSE;
  memset(watch, 0, 16);      /* (an empty list: the entry with no program counter ends it) */
  return TRUE;
}
EXPORT BOOL WINAPI K32GetWsChangesEx(HANDLE process, PVOID watch, PDWORD size) { if (!size) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return K32GetWsChanges(process, watch, *size); }
EXPORT BOOL WINAPI K32EnumPageFilesW(PVOID callback, LPVOID context) { (void)context; if (!callback) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return TRUE; }      /* (no paging file) */
EXPORT BOOL WINAPI K32EnumPageFilesA(PVOID callback, LPVOID context) { return K32EnumPageFilesW(callback, context); }
EXPORT BOOL WINAPI K32EnumDeviceDrivers(LPVOID *bases, DWORD size, LPDWORD needed) { (void)bases; (void)size; if (!needed) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *needed = 0; return TRUE; }
EXPORT DWORD WINAPI K32GetDeviceDriverBaseNameW(LPVOID base, LPWSTR name, DWORD size) { (void)base; (void)name; (void)size; set_error(ERROR_INVALID_PARAMETER); return 0; }
EXPORT DWORD WINAPI K32GetDeviceDriverBaseNameA(LPVOID base, LPSTR name, DWORD size) { (void)base; (void)name; (void)size; set_error(ERROR_INVALID_PARAMETER); return 0; }
EXPORT DWORD WINAPI K32GetDeviceDriverFileNameW(LPVOID base, LPWSTR name, DWORD size) { (void)base; (void)name; (void)size; set_error(ERROR_INVALID_PARAMETER); return 0; }
EXPORT DWORD WINAPI K32GetDeviceDriverFileNameA(LPVOID base, LPSTR name, DWORD size) { (void)base; (void)name; (void)size; set_error(ERROR_INVALID_PARAMETER); return 0; }
EXPORT BOOL WINAPI K32GetPerformanceInfo(PVOID info, DWORD size) {
  MEMORYSTATUSEX m;
  if (!info || size < 104) { set_error(24); return FALSE; }
  m.dwLength = sizeof(m);
  if (!GlobalMemoryStatusEx(&m)) return FALSE;
  DWORD ids[1024], needed = 0;
  K32EnumProcesses(ids, sizeof(ids), &needed);
  SIZE_T *p = (SIZE_T *)((BYTE *)info + 8);
  memset(info, 0, 104);
  *(DWORD *)info = 104;
  p[0] = (SIZE_T)((m.ullTotalPageFile - m.ullAvailPageFile) / 4096);      /* CommitTotal */
  p[1] = (SIZE_T)(m.ullTotalPageFile / 4096);                            /* CommitLimit */
  p[2] = p[0];                                                           /* CommitPeak */
  p[3] = (SIZE_T)(m.ullTotalPhys / 4096);
  p[4] = (SIZE_T)(m.ullAvailPhys / 4096);
  p[9] = 4096;                                                           /* PageSize */
  p[11] = needed / sizeof(DWORD);                                        /* ProcessCount */
  return TRUE;
}
/* The file a view is of: a program image is known by the loader; a data view of a file is not reachable from here. */
EXPORT DWORD WINAPI K32GetMappedFileNameW(HANDLE process, LPVOID address, LPWSTR name, DWORD size) {
  MEMORY_BASIC_INFORMATION info;
  if (!name || !VirtualQueryEx(process, address, &info, sizeof(info))) { if (!name) set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (info.Type != 0x1000000 /* MEM_IMAGE */) { set_error(1006 /* ERROR_FILE_INVALID: not a view of a file that can be named */); return 0; }
  WCHAR path[MAX_PATH];
  const DWORD n = K32GetModuleFileNameExW(process, (HMODULE)info.AllocationBase, path, MAX_PATH);
  if (!n) { set_error(1006); return 0; }
  static const WCHAR prefix[] = {'\\', 'D', 'e', 'v', 'i', 'c', 'e', '\\', 'H', 'a', 'r', 'd', 'd', 'i', 's', 'k', 'V', 'o', 'l', 'u', 'm', 'e', '1', 0};
  const DWORD total = (DWORD)wcslen(prefix) + (n >= 2 && path[1] == ':' ? n - 2 : n);
  if (size <= total) { if (size) { memcpy(name, prefix, (size - 1 < wcslen(prefix) ? size - 1 : wcslen(prefix)) * sizeof(WCHAR)); name[size - 1] = 0; } set_error(ERROR_INSUFFICIENT_BUFFER); return size; }
  memcpy(name, prefix, wcslen(prefix) * sizeof(WCHAR));
  memcpy(name + wcslen(prefix), path + (n >= 2 && path[1] == ':' ? 2 : 0), (n - (n >= 2 && path[1] == ':' ? 2 : 0) + 1) * sizeof(WCHAR));
  return total;
}
EXPORT DWORD WINAPI K32GetMappedFileNameA(HANDLE process, LPVOID address, LPSTR name, DWORD size) {
  WCHAR wide[MAX_PATH + 32];
  const DWORD n = K32GetMappedFileNameW(process, address, wide, MAX_PATH + 32);
  if (!n) return 0;
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide, (int)n + 1, name, (int)size, NULL, NULL);
  if (!bytes) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return (DWORD)bytes - 1;
}

/* ---- processor state: CONTEXT with extended state, XSAVE features ---------------------------------------------------------------------------------------------------------- */

#define CONTEXT_AMD64_ 0x00100000u
#define CONTEXT_XSTATE_ (CONTEXT_AMD64_ | 0x40u)
#define CONTEXT_SIZE_ 0x4D0u
static DWORD64 xstate_enabled(void) {
  DWORD a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  if (!(c & (1u << 27)) || !(c & (1u << 28))) return 3;      /* no OSXSAVE or AVX: x87 and SSE */
  DWORD lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (lo & 6) == 6 ? 7 : 3;
}
EXPORT DWORD64 WINAPI GetEnabledXStateFeatures(void) { return xstate_enabled(); }
EXPORT DWORD64 WINAPI GetThreadEnabledXStateFeatures(void) { return xstate_enabled(); }
EXPORT BOOL WINAPI EnableProcessOptionalXStateFeatures(DWORD64 features) { if (features & ~xstate_enabled()) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return TRUE; }
/* The buffer: [CONTEXT][CONTEXT_EX: six DWORDs, the chunks All, Legacy, XState (offset from the CONTEXT_EX, length)] then the XSTATE area (header of 64 bytes, then the AVX registers). */
static BOOL context_layout(DWORD flags, DWORD *length, DWORD *xstate_offset, DWORD *xstate_length) {
  if ((flags & CONTEXT_AMD64_) != CONTEXT_AMD64_ || (flags & ~(DWORD)0x1000007F & ~CONTEXT_AMD64_ & 0x7FFFFFFF)) return FALSE;
  DWORD total = CONTEXT_SIZE_ + 24, xo = 0, xl = 0;
  if ((flags & CONTEXT_XSTATE_) == CONTEXT_XSTATE_) {
    total = (total + 63) & ~63u;
    xo = total - CONTEXT_SIZE_;
    xl = 64 + ((xstate_enabled() & 4) ? 256 : 0);
    total += xl;
  }
  *length = total + 15;      /* (room to align the CONTEXT to 16 bytes) */
  *xstate_offset = xo;
  *xstate_length = xl;
  return TRUE;
}
EXPORT BOOL WINAPI GetExtendedContextLength(DWORD flags, PDWORD length) {
  DWORD total, xo, xl;
  if (!length || !context_layout(flags, &total, &xo, &xl)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *length = total;
  return TRUE;
}
EXPORT BOOL WINAPI InitializeContext2(PVOID buffer, DWORD flags, CONTEXT **context, PDWORD length, DWORD64 compaction_mask) {
  DWORD total, xo, xl;
  (void)compaction_mask;
  if (!length || !context_layout(flags, &total, &xo, &xl)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (*length < total || !buffer) { *length = total; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  *length = total;
  BYTE *aligned = (BYTE *)(((ULONG_PTR)buffer + 15) & ~(ULONG_PTR)15);
  memset(aligned, 0, total - (DWORD)(aligned - (BYTE *)buffer));
  *(DWORD *)(aligned + 0x30) = flags;      /* CONTEXT.ContextFlags */
  DWORD *ex = (DWORD *)(aligned + CONTEXT_SIZE_);
  ex[0] = (DWORD)-(LONG)CONTEXT_SIZE_; ex[1] = total - 15;      /* All */
  ex[2] = (DWORD)-(LONG)CONTEXT_SIZE_; ex[3] = CONTEXT_SIZE_;   /* Legacy */
  ex[4] = xo; ex[5] = xl;                                       /* XState (offset relative to the CONTEXT_EX) */
  if (xo) *(DWORD64 *)(aligned + CONTEXT_SIZE_ + xo + 8) = 0x8000000000000000ull;      /* header: compaction mask with the compacted-format bit */
  if (context) *context = (CONTEXT *)aligned;
  return TRUE;
}
EXPORT BOOL WINAPI InitializeContext(PVOID buffer, DWORD flags, CONTEXT **context, PDWORD length) { return InitializeContext2(buffer, flags, context, length, 0); }
static BYTE *context_xstate(CONTEXT *context, DWORD *length) {
  DWORD *ex = (DWORD *)((BYTE *)context + CONTEXT_SIZE_);
  if (!ex[5]) return NULL;
  *length = ex[5];
  return (BYTE *)ex + (LONG)ex[4];
}
EXPORT BOOL WINAPI GetXStateFeaturesMask(CONTEXT *context, DWORD64 *mask) {
  DWORD length;
  if (!context || !mask) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BYTE *x = context_xstate(context, &length);
  *mask = x ? *(DWORD64 *)x : 0;
  if (*(DWORD *)((BYTE *)context + 0x30) & 0x8) *mask |= 3;      /* (the legacy floating point state is the CONTEXT's own) */
  return TRUE;
}
EXPORT BOOL WINAPI SetXStateFeaturesMask(CONTEXT *context, DWORD64 mask) {
  DWORD length;
  if (!context || (mask & ~xstate_enabled())) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BYTE *x = context_xstate(context, &length);
  if (!x) { if (mask & ~(DWORD64)3) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return TRUE; }
  *(DWORD64 *)x = mask & ~(DWORD64)3;
  return TRUE;
}
EXPORT PVOID WINAPI LocateXStateFeature(CONTEXT *context, DWORD feature, PDWORD length) {
  DWORD xl;
  if (!context) return NULL;
  if (feature == 0) { if (length) *length = 160; return (BYTE *)context + 0x100; }
  if (feature == 1) { if (length) *length = 256; return (BYTE *)context + 0x1A0; }
  if (feature != 2) return NULL;
  BYTE *x = context_xstate(context, &xl);
  if (!x || xl < 64 + 256) return NULL;
  if (length) *length = 256;
  return x + 64;
}
EXPORT BOOL WINAPI CopyContext(CONTEXT *destination, DWORD flags, CONTEXT *source) {
  if (!destination || !source || (flags & CONTEXT_AMD64_) != CONTEXT_AMD64_) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BYTE *d = (BYTE *)destination, *s = (BYTE *)source;
  if (flags & 1) { memcpy(d + 0x38, s + 0x38, 2); memcpy(d + 0x42, s + 0x42, 6); memcpy(d + 0x98, s + 0x98, 8); memcpy(d + 0xF8, s + 0xF8, 8); }      /* control: SegCs, SegSs, EFlags, Rsp, Rip */
  if (flags & 2) { memcpy(d + 0x78, s + 0x78, 0x98 - 0x78); memcpy(d + 0xA0, s + 0xA0, 0xF8 - 0xA0); }      /* integer */
  if (flags & 4) memcpy(d + 0x3A, s + 0x3A, 8);      /* segments */
  if (flags & 8) { memcpy(d + 0x34, s + 0x34, 4); memcpy(d + 0x100, s + 0x100, 512); }      /* floating point, MxCsr */
  if (flags & 0x10) memcpy(d + 0x48, s + 0x48, 0x78 - 0x48);      /* debug registers */
  DWORD dl, sl;
  BYTE *dx = ((flags & CONTEXT_XSTATE_) == CONTEXT_XSTATE_) ? context_xstate(destination, &dl) : NULL, *sx = dx ? context_xstate(source, &sl) : NULL;
  if (dx && sx) memcpy(dx, sx, dl < sl ? dl : sl);
  return TRUE;
}

/* ---- features that need hardware or services this machine does not have: enclaves, I/O rings, user-mode scheduling, thread profiling ------------------------------------------------ */

#define HR_NOT_SUPPORTED ((HRESULT)0x80070032)
EXPORT BOOL WINAPI IsEnclaveTypeSupported(DWORD type) { (void)type; return FALSE; }
EXPORT PVOID WINAPI CreateEnclave(HANDLE process, PVOID base, SIZE_T size, SIZE_T initial, DWORD type, const VOID *info, DWORD info_length, PDWORD error) { (void)process; (void)base; (void)size; (void)initial; (void)type; (void)info; (void)info_length; if (error) *error = ERROR_NOT_SUPPORTED; set_error(ERROR_NOT_SUPPORTED); return NULL; }
EXPORT BOOL WINAPI LoadEnclaveData(HANDLE process, PVOID base, const VOID *buffer, SIZE_T size, DWORD protect, const VOID *info, DWORD info_length, PSIZE_T written, PDWORD error) { (void)process; (void)base; (void)buffer; (void)size; (void)protect; (void)info; (void)info_length; if (written) *written = 0; if (error) *error = ERROR_NOT_SUPPORTED; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT BOOL WINAPI InitializeEnclave(HANDLE process, PVOID base, const VOID *info, DWORD info_length, PDWORD error) { (void)process; (void)base; (void)info; (void)info_length; if (error) *error = ERROR_NOT_SUPPORTED; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT HRESULT WINAPI QueryIoRingCapabilities(PVOID capabilities) { (void)capabilities; return HR_NOT_SUPPORTED; }
EXPORT BOOL WINAPI IsIoRingOpSupported(PVOID ring, int op) { (void)ring; (void)op; return FALSE; }
EXPORT HRESULT WINAPI CreateIoRing(int version, ULONG flags, UINT32 submission, UINT32 completion, PVOID *ring) { (void)version; (void)flags; (void)submission; (void)completion; if (ring) *ring = NULL; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI GetIoRingInfo(PVOID ring, PVOID info) { (void)ring; (void)info; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI SubmitIoRing(PVOID ring, UINT32 wait, UINT32 milliseconds, UINT32 *submitted) { (void)ring; (void)wait; (void)milliseconds; if (submitted) *submitted = 0; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI PopIoRingCompletion(PVOID ring, PVOID entry) { (void)ring; (void)entry; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI CloseIoRing(PVOID ring) { (void)ring; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI SetIoRingCompletionEvent(PVOID ring, HANDLE event) { (void)ring; (void)event; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI BuildIoRingCancelRequest(PVOID ring, ...) { (void)ring; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI BuildIoRingReadFile(PVOID ring, ...) { (void)ring; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI BuildIoRingRegisterBuffers(PVOID ring, ...) { (void)ring; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI BuildIoRingRegisterFileHandles(PVOID ring, ...) { (void)ring; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI BuildIoRingFlushFile(PVOID ring, ...) { (void)ring; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI BuildIoRingReadFileScatter(PVOID ring, ...) { (void)ring; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI BuildIoRingWriteFile(PVOID ring, ...) { (void)ring; return HR_NOT_SUPPORTED; }
EXPORT HRESULT WINAPI BuildIoRingWriteFileGather(PVOID ring, ...) { (void)ring; return HR_NOT_SUPPORTED; }
#define UMS_UNSUPPORTED_BOOL(name, params) EXPORT BOOL WINAPI name params { set_error(ERROR_NOT_SUPPORTED); return FALSE; }
UMS_UNSUPPORTED_BOOL(CreateUmsCompletionList, (PVOID *list))
UMS_UNSUPPORTED_BOOL(DeleteUmsCompletionList, (PVOID list))
UMS_UNSUPPORTED_BOOL(DequeueUmsCompletionListItems, (PVOID list, DWORD wait, PVOID *items))
UMS_UNSUPPORTED_BOOL(GetUmsCompletionListEvent, (PVOID list, PHANDLE event))
UMS_UNSUPPORTED_BOOL(ExecuteUmsThread, (PVOID context))
UMS_UNSUPPORTED_BOOL(UmsThreadYield, (PVOID scheduler))
UMS_UNSUPPORTED_BOOL(CreateUmsThreadContext, (PVOID *context))
UMS_UNSUPPORTED_BOOL(DeleteUmsThreadContext, (PVOID context))
UMS_UNSUPPORTED_BOOL(EnterUmsSchedulingMode, (PVOID attributes))
UMS_UNSUPPORTED_BOOL(QueryUmsThreadInformation, (PVOID context, int klass, PVOID info, ULONG length, PULONG returned))
UMS_UNSUPPORTED_BOOL(SetUmsThreadInformation, (PVOID context, int klass, PVOID info, ULONG length))
EXPORT PVOID WINAPI GetCurrentUmsThread(void) { set_error(ERROR_NOT_SUPPORTED); return NULL; }
EXPORT PVOID WINAPI GetNextUmsListItem(PVOID context) { (void)context; set_error(ERROR_NOT_SUPPORTED); return NULL; }
EXPORT BOOL WINAPI GetUmsSystemThreadInformation(HANDLE thread, PVOID info) { (void)thread; (void)info; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT DWORD WINAPI EnableThreadProfiling(HANDLE thread, DWORD flags, DWORD64 affinity, PHANDLE handle) { (void)thread; (void)flags; (void)affinity; if (handle) *handle = NULL; return ERROR_NOT_SUPPORTED; }
EXPORT DWORD WINAPI DisableThreadProfiling(HANDLE handle) { (void)handle; return ERROR_INVALID_HANDLE; }
EXPORT DWORD WINAPI QueryThreadProfiling(HANDLE thread, PBOOLEAN enabled) { (void)thread; if (enabled) *enabled = FALSE; return 0; }
EXPORT DWORD WINAPI ReadThreadProfilingData(HANDLE handle, DWORD flags, PVOID data) { (void)handle; (void)flags; (void)data; return ERROR_INVALID_HANDLE; }
EXPORT BOOL WINAPI QueryIoRateControlInformationJobObject(HANDLE job, PCWSTR volume, PVOID *information, ULONG *count) { (void)job; (void)volume; if (information) *information = NULL; if (count) *count = 0; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT DWORD WINAPI SetIoRateControlInformationJobObject(HANDLE job, PVOID information) { (void)job; (void)information; set_error(ERROR_NOT_SUPPORTED); return 0; }
EXPORT void WINAPI FreeMemoryJobObject(PVOID buffer) { (void)buffer; }
/* Protected policies (per-process settings of the Protected Process Light kind) are kept as a small table. */
static struct { BYTE guid[16]; ULONG_PTR value; BOOL used; } g_protected_policy[8];
EXPORT BOOL WINAPI SetProtectedPolicy(const BYTE *guid, ULONG_PTR value, PULONG_PTR old) {
  if (!guid) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  int free_slot = -1;
  self_lock();
  for (int i = 0; i < 8; ++i) {
    if (g_protected_policy[i].used && !memcmp(g_protected_policy[i].guid, guid, 16)) {
      if (old) *old = g_protected_policy[i].value;
      self_unlock();
      set_error(ERROR_ACCESS_DENIED);      /* (a policy, once set, is final) */
      return FALSE;
    }
    if (!g_protected_policy[i].used && free_slot < 0) free_slot = i;
  }
  if (free_slot >= 0) { memcpy(g_protected_policy[free_slot].guid, guid, 16); g_protected_policy[free_slot].value = value; g_protected_policy[free_slot].used = TRUE; }
  self_unlock();
  if (free_slot < 0) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  if (old) *old = 0;
  return TRUE;
}
EXPORT BOOL WINAPI QueryProtectedPolicy(const BYTE *guid, PULONG_PTR value) {
  if (!guid || !value) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BOOL found = FALSE;
  self_lock();
  for (int i = 0; i < 8; ++i) if (g_protected_policy[i].used && !memcmp(g_protected_policy[i].guid, guid, 16)) { *value = g_protected_policy[i].value; found = TRUE; }
  self_unlock();
  if (!found) set_error(1168 /* ERROR_NOT_FOUND */);
  return found;
}
EXPORT BOOL WINAPI GetCachedSigningLevel(HANDLE file, PULONG flags, PULONG level, PUCHAR thumbprint, PULONG thumbprint_size, PULONG algorithm) { (void)file; (void)flags; (void)level; (void)thumbprint; (void)thumbprint_size; (void)algorithm; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT BOOL WINAPI SetCachedSigningLevel(PHANDLE sources, ULONG count, ULONG flags, HANDLE target) { (void)sources; (void)count; (void)flags; (void)target; set_error(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT BOOL WINAPI OOBEComplete(PBOOL complete) { if (!complete) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *complete = TRUE; return TRUE; }
EXPORT BOOL WINAPI CeipIsOptedIn(void) { return FALSE; }

/* ---- languages: durations, UI languages, geographic names -------------------------------------------------------------------------------------------------------------------- */

static int duration_digits(WCHAR *out, int capacity, int at, ULONGLONG value, int width) {
  WCHAR text[24];
  int n = 0;
  do { text[n++] = (WCHAR)('0' + value % 10); value /= 10; } while (value);
  while (n < width && n < 23) text[n++] = '0';
  for (int i = n - 1; i >= 0; --i) { if (out && at < capacity) out[at] = text[i]; ++at; }
  return at;
}
EXPORT int WINAPI GetDurationFormatEx(LPCWSTR locale_name, DWORD flags, const SYSTEMTIME *duration, ULONGLONG ticks, LPCWSTR format, LPWSTR out, int capacity) {
  static const WCHAR default_format[] = {'h', ':', 'm', 'm', ':', 's', 's', 0};
  if ((flags & ~(DWORD)0x80000000u) || capacity < 0 || (capacity && !out)) { set_error(ERROR_INVALID_FLAGS); return 0; }
  if (locale_name && !IsValidLocaleName(locale_name) && !(locale_name[0] == 0)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (duration) {
    if (duration->wHour > 23 || duration->wMinute > 59 || duration->wSecond > 59 || duration->wMilliseconds > 999) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    ticks = (((ULONGLONG)duration->wHour * 60 + duration->wMinute) * 60 + duration->wSecond) * 10000000ull + (ULONGLONG)duration->wMilliseconds * 10000ull;
  }
  if (!format) format = default_format;
  const ULONGLONG DAY = 864000000000ull, HOUR = 36000000000ull, MINUTE = 600000000ull, SECOND = 10000000ull;
  BOOL has_d = FALSE, has_h = FALSE, has_m = FALSE, has_s = FALSE;
  for (const WCHAR *c = format; *c; ++c) {
    if (*c == '\'') { for (++c; *c && *c != '\''; ++c) {} if (!*c) break; continue; }
    if (*c == 'd') has_d = TRUE; else if (*c == 'h') has_h = TRUE; else if (*c == 'm') has_m = TRUE; else if (*c == 's') has_s = TRUE;
  }
  const ULONGLONG days = ticks / DAY, after_days = has_d ? ticks % DAY : ticks;
  const ULONGLONG hours = after_days / HOUR, after_hours = has_h ? after_days % HOUR : after_days;
  const ULONGLONG minutes = after_hours / MINUTE, after_minutes = has_m ? after_hours % MINUTE : after_hours;
  const ULONGLONG seconds = after_minutes / SECOND, fraction = has_s ? after_minutes % SECOND : after_minutes % SECOND;
  int at = 0;
  for (const WCHAR *c = format; *c;) {
    const WCHAR ch = *c;
    if (ch == '\'') {
      ++c;
      while (*c && *c != '\'') { if (out && at < capacity) out[at] = *c; ++at; ++c; }
      if (*c == '\'') ++c;
      continue;
    }
    if (ch == 'd' || ch == 'h' || ch == 'm' || ch == 's' || ch == 'f') {
      int run = 0;
      while (c[run] == ch) ++run;
      if (ch == 'f') {
        ULONGLONG f = fraction;
        int shown = run > 7 ? 7 : run;
        for (int i = 0; i < 7 - shown; ++i) f /= 10;
        at = duration_digits(out, capacity, at, f, shown);
        for (int i = 7; i < run; ++i) { if (out && at < capacity) out[at] = '0'; ++at; }
      } else {
        const ULONGLONG v = ch == 'd' ? days : ch == 'h' ? hours : ch == 'm' ? minutes : seconds;
        at = duration_digits(out, capacity, at, v, run >= 2 ? 2 : 1);
      }
      c += run;
      continue;
    }
    if (out && at < capacity) out[at] = ch;
    ++at;
    ++c;
  }
  (void)has_s;
  if (capacity == 0) return at + 1;
  if (at + 1 > capacity) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  out[at] = 0;
  return at + 1;
}
EXPORT int WINAPI GetDurationFormat(LCID locale, DWORD flags, const SYSTEMTIME *duration, ULONGLONG ticks, LPCWSTR format, LPWSTR out, int capacity) {
  if (!IsValidLocale(ConvertDefaultLocale(locale), 1)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  return GetDurationFormatEx(NULL, flags, duration, ticks, format, out, capacity);
}
/* UI languages: this system has one, English (United States). */
EXPORT BOOL WINAPI IsValidUILanguage(LANGID language) { return language == 0x0409; }
EXPORT BOOL WINAPI GetUILanguageInfo(DWORD flags, PCZZWSTR_ languages, PZZWSTR_ fallback, PDWORD fallback_size, PDWORD attributes) {
  const DWORD kinds = flags & (0x4 | 0x8), modifiers = flags & ~(DWORD)(0x4 | 0x8);
  if (!languages || !attributes || (kinds == (0x4 | 0x8)) || (modifiers & ~(DWORD)(0x1 | 0x2 | 0x10)) || ((modifiers & 1) && (modifiers & 2))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  static const WCHAR full_name[] = {'e', 'n', '-', 'U', 'S', 0, 0}, full_id[] = {'0', '4', '0', '9', 0, 0}, parent_name[] = {'e', 'n', 0, 0};
  const WCHAR *l = languages;
  const BOOL by_id = kinds == 0x4;
  /* each language of the list is checked: en-US, en and 0409 are the ones this system has */
  for (; *l; l += wcslen(l) + 1) {
    const BOOL known = equals_ignore_case(l, (const WCHAR *)L"en-US") || equals_ignore_case(l, (const WCHAR *)L"en") || equals_ignore_case(l, (const WCHAR *)L"0409") || equals_ignore_case(l, (const WCHAR *)L"409");
    if (!known) { *attributes = 0; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  }
  const WCHAR *text = (modifiers & 2) ? parent_name : (by_id ? full_id : full_name);
  const DWORD need = (DWORD)(by_id ? 6 : ((modifiers & 2) ? 4 : 7));
  *attributes = 0;      /* MUI_FULL_LANGUAGE */
  if (!fallback_size) return TRUE;
  if (!fallback) { *fallback_size = need; return TRUE; }
  if (*fallback_size < need) { *fallback_size = need; set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memcpy(fallback, text, need * sizeof(WCHAR));
  *fallback_size = need;
  return TRUE;
}
EXPORT BOOL WINAPI ValidateLocale(DWORD locale) { return IsValidLocale(ConvertDefaultLocale(locale), 1) ? TRUE : FALSE; }
EXPORT DWORD WINAPI VerLanguageNameW(DWORD language, LPWSTR out, DWORD capacity) {
  WCHAR name[96];
  const int n = GetLocaleInfoW(language & 0xFFFF, 0x72 /* LOCALE_SENGLISHDISPLAYNAME */, name, 96);
  if (n <= 0) {
    static const WCHAR neutral[] = {'L', 'a', 'n', 'g', 'u', 'a', 'g', 'e', ' ', 'N', 'e', 'u', 't', 'r', 'a', 'l', 0};
    memcpy(name, neutral, sizeof(neutral));
  }
  const DWORD length = (DWORD)wcslen(name);
  if (!out || !capacity) return length;
  const DWORD copy = length < capacity - 1 ? length : capacity - 1;
  memcpy(out, name, copy * sizeof(WCHAR));
  out[copy] = 0;
  return copy;
}
EXPORT DWORD WINAPI VerLanguageNameA(DWORD language, LPSTR out, DWORD capacity) {
  WCHAR wide[96];
  const DWORD n = VerLanguageNameW(language, wide, 96);
  if (!out || !capacity) return n;
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide, (int)n + 1, out, (int)capacity, NULL, NULL);
  if (bytes <= 0) { out[capacity - 1] = 0; return capacity - 1; }
  return (DWORD)bytes - 1;
}
EXPORT BOOL WINAPI SetCalendarInfoW(LCID locale, DWORD calendar, DWORD type, LPCWSTR value) {
  if (!IsValidLocale(ConvertDefaultLocale(locale), 1) || !value) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  (void)calendar; (void)type;
  set_error(ERROR_ACCESS_DENIED);      /* (the calendar settings are the system's) */
  return FALSE;
}
EXPORT BOOL WINAPI SetCalendarInfoA(LCID locale, DWORD calendar, DWORD type, LPCSTR value) {
  WCHAR *w = value ? to_wide(value) : NULL;
  const BOOL ok = SetCalendarInfoW(locale, calendar, type, w);
  free_wide(w);
  return ok;
}
typedef BOOL (CALLBACK *GEO_ENUMNAMEPROC_)(PWSTR, LPARAM);
EXPORT BOOL WINAPI EnumSystemGeoNames(DWORD geo_class, GEO_ENUMNAMEPROC_ callback, LPARAM parameter) {
  if (!callback || (geo_class != 16 /* GEOCLASS_NATION */ && geo_class != 14 /* GEOCLASS_REGION */)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (geo_class == 16) {
    for (int i = 0; i < NLS_GEO_COUNT; ++i) {
      WCHAR name[3] = {(WCHAR)(unsigned char)k_geo_nations[i].iso2[0], (WCHAR)(unsigned char)k_geo_nations[i].iso2[1], 0};
      if (!callback(name, parameter)) break;
    }
  }
  return TRUE;
}
EXPORT int WINAPI ConvertNLSDayOfWeekToWin32DayOfWeek(int day) { return (day + 1) % 7; }      /* NLS: Monday = 0; Win32: Sunday = 0 */
EXPORT LONG64 WINAPI MultiplyExtract128(LONG64 a, LONG64 b, BYTE shift) { return (LONG64)(((__int128)a * (__int128)b) >> (shift & 127)); }
EXPORT ULONG64 WINAPI UnsignedMultiplyExtract128(ULONG64 a, ULONG64 b, BYTE shift) { return (ULONG64)(((unsigned __int128)a * (unsigned __int128)b) >> (shift & 127)); }
EXPORT VOID WINAPI RtlSecureZeroMemory(PVOID p, SIZE_T n) { volatile BYTE *b = p; while (n--) *b++ = 0; }
EXPORT BOOLEAN WINAPI RtlIsEcCode(ULONG64 address) { (void)address; return FALSE; }      /* (no emulation-compatible code in a native process) */
typedef BOOL (WINAPI *SECURE_MEMORY_CACHE_CALLBACK_)(PVOID, SIZE_T);
static SECURE_MEMORY_CACHE_CALLBACK_ g_secure_callbacks[8];
EXPORT BOOL WINAPI AddSecureMemoryCacheCallback(SECURE_MEMORY_CACHE_CALLBACK_ callback) {
  if (!callback) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  self_lock();
  int slot = -1;
  for (int i = 0; i < 8; ++i) if (!g_secure_callbacks[i]) { slot = i; break; }
  if (slot >= 0) g_secure_callbacks[slot] = callback;
  self_unlock();
  if (slot < 0) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI RemoveSecureMemoryCacheCallback(SECURE_MEMORY_CACHE_CALLBACK_ callback) {
  BOOL found = FALSE;
  self_lock();
  for (int i = 0; i < 8; ++i) if (callback && g_secure_callbacks[i] == callback) { g_secure_callbacks[i] = NULL; found = TRUE; break; }
  self_unlock();
  if (!found) set_error(ERROR_INVALID_PARAMETER);
  return found;
}
/* CreateProcessInternal*: CreateProcess with the token of another user, which only a system with accounts to switch to can use; without a token it is CreateProcess. */
EXPORT BOOL WINAPI CreateProcessInternalW(HANDLE token, LPCWSTR application, LPWSTR command_line, LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information, PHANDLE new_token) {
  if (new_token) *new_token = NULL;
  if (token) { set_error(ERROR_NOT_SUPPORTED); return FALSE; }
  return CreateProcessW(application, command_line, process_attributes, thread_attributes, inherit, flags, environment, directory, startup, information);
}
EXPORT BOOL WINAPI CreateProcessInternalA(HANDLE token, LPCSTR application, LPSTR command_line, LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment, LPCSTR directory, LPSTARTUPINFOA startup, LPPROCESS_INFORMATION information, PHANDLE new_token) {
  if (new_token) *new_token = NULL;
  if (token) { set_error(ERROR_NOT_SUPPORTED); return FALSE; }
  return CreateProcessA(application, command_line, process_attributes, thread_attributes, inherit, flags, environment, directory, startup, information);
}
