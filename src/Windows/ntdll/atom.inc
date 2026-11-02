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
/* ntdll.dll: atom tables (RtlCreateAtomTable and the others, after the user-mode part of Wine's atom.c) and the global atom services built on one. Included by ntdll.c
   after rtl3.inc. */

NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs);
#define ATOM_MAXINT 0xC000u
#define ATOM_MAX_LEN 255
#define ATOM_MAX_HANDLES 0x4000
#define ATOM_SIGNATURE 0x6d6f7441u      /* 'Atom' */
#define ATOM_IS_INT(p) (((ULONG_PTR)(p) >> 16) == 0)
#define ATOM_STATUS_MORE_ENTRIES ((NTSTATUS)0x00000105)
#define ATOM_STATUS_WAS_LOCKED ((NTSTATUS)0x40000019)
#define ATOM_STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)

typedef struct AtomEntry {
  struct AtomEntry *HashLink;
  USHORT HandleIndex, Atom, ReferenceCount;
  UCHAR Flags;
  UCHAR Pad;
  USHORT NameLength;      /* in characters */
  WCHAR Name[1];
} ATOM_ENTRY;
typedef struct AtomTable {
  ULONG Signature;
  RTL_CRITICAL_SECTION CriticalSection;
  RTL_HANDLE_TABLE_ HandleTable;
  ULONG NumberOfBuckets;
  ATOM_ENTRY *Buckets[1];
} ATOM_TABLE;
typedef struct { RTL_HANDLE_ hdr; ATOM_ENTRY *entry; } ATOM_HANDLE;

static NTSTATUS atom_lock(ATOM_TABLE *table) {
  if (!table || table->Signature != ATOM_SIGNATURE) return STATUS_INVALID_PARAMETER;
  RtlEnterCriticalSection(&table->CriticalSection);
  return STATUS_SUCCESS;
}
static void atom_unlock(ATOM_TABLE *table) { RtlLeaveCriticalSection(&table->CriticalSection); }
static ULONG atom_hash(ATOM_TABLE *table, const WCHAR *name, ULONG len) {
  UNICODE_STRING str;
  ULONG hash = 0;
  str.Length = str.MaximumLength = (USHORT)(len * 2);
  str.Buffer = (PWSTR)name;
  RtlHashUnicodeString(&str, TRUE, 1, &hash);
  return hash % table->NumberOfBuckets;
}
static ATOM_ENTRY *atom_find_entry(ATOM_TABLE *table, const WCHAR *name, ULONG len, ULONG hash) {
  ATOM_ENTRY *entry = table->Buckets[hash];
  while (entry && RtlCompareUnicodeStrings(entry->Name, entry->NameLength, name, len, TRUE)) entry = entry->HashLink;
  return entry;
}
static NTSTATUS atom_add_entry(ATOM_TABLE *table, const WCHAR *name, ULONG len, USHORT *atom) {
  ULONG index = 0, hash = atom_hash(table, name, len);
  ATOM_ENTRY *entry = atom_find_entry(table, name, len, hash);
  if (entry) { entry->ReferenceCount++; *atom = entry->Atom; return STATUS_SUCCESS; }
  ATOM_HANDLE *handle = (ATOM_HANDLE *)RtlAllocateHandle(&table->HandleTable, &index);
  if (!handle) return STATUS_NO_MEMORY;
  entry = RtlAllocateHeap(RtlProcessHeap(), 0, sizeof(ATOM_ENTRY) + (SIZE_T)len * 2);
  if (!entry) { RtlFreeHandle(&table->HandleTable, &handle->hdr); return STATUS_NO_MEMORY; }
  entry->HandleIndex = (USHORT)index;
  entry->Atom = (USHORT)(ATOM_MAXINT + index);
  entry->ReferenceCount = 1;
  entry->Flags = 0;
  entry->NameLength = (USHORT)len;
  entry->HashLink = table->Buckets[hash];
  memcpy(entry->Name, name, (SIZE_T)len * 2);
  handle->hdr.Next = (RTL_HANDLE_ *)1;
  table->Buckets[hash] = entry;
  handle->entry = entry;
  *atom = entry->Atom;
  return STATUS_SUCCESS;
}
/* STATUS_SUCCESS: an integer atom (*out is filled); STATUS_INVALID_PARAMETER: too long or 0; STATUS_MORE_ENTRIES: a name. */
static NTSTATUS atom_integral(const WCHAR *str, SIZE_T len, USHORT *out) {
  ULONG atom;
  if (!ATOM_IS_INT(str)) {
    const WCHAR *p = str;
    if (!len) return STATUS_OBJECT_NAME_INVALID;
    if (*p++ == '#') {
      atom = 0;
      while (p < str + len && *p >= '0' && *p <= '9') atom = ((atom * 10 + *p++ - '0') & 0xFFFF);
      if (p > str + 1 && p == str + len) goto done;
    }
    if (len > ATOM_MAX_LEN) return STATUS_INVALID_PARAMETER;
    return ATOM_STATUS_MORE_ENTRIES;
  }
  if ((atom = (USHORT)(ULONG_PTR)str) >= ATOM_MAXINT) return STATUS_INVALID_PARAMETER;
done:
  if (atom >= ATOM_MAXINT) atom = 0;
  if (!(*out = (USHORT)atom)) return STATUS_INVALID_PARAMETER;
  return STATUS_SUCCESS;
}
static SIZE_T atom_wcslen(const WCHAR *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }

EXPORT NTSTATUS NTAPI RtlCreateAtomTable(ULONG size, ATOM_TABLE **ret) {
  if (*ret) return size ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS;
  if (size < 4 || size > 0x200) size = 37;
  ATOM_TABLE *table = RtlAllocateHeap(RtlProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ATOM_TABLE) + (SIZE_T)size * sizeof(ATOM_ENTRY *));
  if (!table) return STATUS_NO_MEMORY;
  table->Signature = ATOM_SIGNATURE;
  table->NumberOfBuckets = size;
  RtlInitializeCriticalSection(&table->CriticalSection);
  RtlInitializeHandleTable(ATOM_MAX_HANDLES, sizeof(ATOM_HANDLE), &table->HandleTable);
  *ret = table;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlEmptyAtomTable(ATOM_TABLE *table, BOOLEAN delete_pinned) {
  const NTSTATUS status = atom_lock(table);
  if (status) return status;
  for (ULONG i = 0; i < table->NumberOfBuckets; ++i) {
    ATOM_ENTRY **ptr = &table->Buckets[i];
    while (*ptr) {
      if (!delete_pinned && (*ptr)->Flags) { ptr = &(*ptr)->HashLink; continue; }
      ATOM_ENTRY *entry = *ptr;
      RTL_HANDLE_ *handle;
      *ptr = entry->HashLink;
      if (RtlIsValidIndexHandle(&table->HandleTable, entry->HandleIndex, &handle)) RtlFreeHandle(&table->HandleTable, handle);
      RtlFreeHeap(RtlProcessHeap(), 0, entry);
    }
  }
  atom_unlock(table);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDestroyAtomTable(ATOM_TABLE *table) {
  if (!table || table->Signature != ATOM_SIGNATURE) return STATUS_INVALID_PARAMETER;
  RtlEmptyAtomTable(table, TRUE);
  RtlDestroyHandleTable(&table->HandleTable);
  RtlDeleteCriticalSection(&table->CriticalSection);
  table->Signature = 0;
  RtlFreeHeap(RtlProcessHeap(), 0, table);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAddAtomToAtomTable(ATOM_TABLE *table, const WCHAR *name, USHORT *atom) {
  NTSTATUS status = atom_lock(table);
  if (status) return status;
  const SIZE_T len = ATOM_IS_INT(name) ? 0 : atom_wcslen(name);
  /* (Windows reads such a "pointer" as a name and the fault is turned into a status) */
  if (ATOM_IS_INT(name) && (ULONG_PTR)name >= ATOM_MAXINT) status = 0xC0000005;
  else status = atom_integral(name, len, atom);
  if (status == ATOM_STATUS_MORE_ENTRIES) status = atom_add_entry(table, name, (ULONG)len, atom);
  atom_unlock(table);
  return status;
}
EXPORT NTSTATUS NTAPI RtlLookupAtomInAtomTable(ATOM_TABLE *table, const WCHAR *name, USHORT *atom) {
  NTSTATUS status = atom_lock(table);
  if (status) return status;
  const SIZE_T len = ATOM_IS_INT(name) ? 0 : atom_wcslen(name);
  status = atom_integral(name, len, atom);
  if (status == ATOM_STATUS_MORE_ENTRIES) {
    ATOM_ENTRY *entry = atom_find_entry(table, name, (ULONG)len, atom_hash(table, name, (ULONG)len));
    if (entry) { *atom = entry->Atom; status = STATUS_SUCCESS; }
    else status = STATUS_OBJECT_NAME_NOT_FOUND;
  }
  atom_unlock(table);
  return status;
}
EXPORT NTSTATUS NTAPI RtlDeleteAtomFromAtomTable(ATOM_TABLE *table, USHORT atom) {
  ATOM_HANDLE *handle;
  NTSTATUS status = atom_lock(table);
  if (status) return status;
  if (!atom) status = STATUS_INVALID_HANDLE;
  else if (atom < ATOM_MAXINT) status = STATUS_SUCCESS;
  else if (RtlIsValidIndexHandle(&table->HandleTable, atom - ATOM_MAXINT, (RTL_HANDLE_ **)&handle)) {
    if (handle->entry->Flags) status = ATOM_STATUS_WAS_LOCKED;
    else if (!--handle->entry->ReferenceCount) {
      ATOM_ENTRY **ptr = &table->Buckets[atom_hash(table, handle->entry->Name, handle->entry->NameLength)];
      while (*ptr != handle->entry) ptr = &(*ptr)->HashLink;
      *ptr = (*ptr)->HashLink;
      RtlFreeHeap(RtlProcessHeap(), 0, handle->entry);
      RtlFreeHandle(&table->HandleTable, &handle->hdr);
    }
  } else status = STATUS_INVALID_HANDLE;
  atom_unlock(table);
  return status;
}
EXPORT NTSTATUS NTAPI RtlPinAtomInAtomTable(ATOM_TABLE *table, USHORT atom) {
  ATOM_HANDLE *handle;
  const NTSTATUS status = atom_lock(table);
  if (status) return status;
  if (atom >= ATOM_MAXINT && RtlIsValidIndexHandle(&table->HandleTable, atom - ATOM_MAXINT, (RTL_HANDLE_ **)&handle)) handle->entry->Flags = 1;
  atom_unlock(table);
  return STATUS_SUCCESS;
}
/* The name "#<atom>" of an integer atom, cut to len bytes (len 0: only its size is wanted). Returns the length in bytes. */
static ULONG atom_integral_name(WCHAR *buffer, ULONG len, USHORT atom) {
  WCHAR tmp[8], rev[8];
  ULONG n = 0, digits = 0, v = atom, room = len / 2;
  do { rev[digits++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
  tmp[n++] = '#';
  while (digits) tmp[n++] = rev[--digits];
  if (!len) return n * 2;
  if (room <= n) n = room - 1;
  memcpy(buffer, tmp, (SIZE_T)n * 2);
  buffer[n] = 0;
  return n * 2;
}
EXPORT NTSTATUS NTAPI RtlQueryAtomInAtomTable(ATOM_TABLE *table, USHORT atom, ULONG *ref, ULONG *pin, WCHAR *name, ULONG *len) {
  ATOM_HANDLE *handle;
  ULONG wlen = 0;
  NTSTATUS status;
  if (!atom) return STATUS_INVALID_PARAMETER;
  status = atom_lock(table);
  if (status) return status;
  if (atom < ATOM_MAXINT) {
    if (len) wlen = atom_integral_name(name, *len, atom);
    if (ref) *ref = 1;
    if (pin) *pin = 1;
  } else if (RtlIsValidIndexHandle(&table->HandleTable, atom - ATOM_MAXINT, (RTL_HANDLE_ **)&handle)) {
    wlen = handle->entry->NameLength * 2u;
    if (ref) *ref = handle->entry->ReferenceCount;
    if (pin) *pin = handle->entry->Flags;
    if (len && *len) {
      if (*len - 2 < wlen) wlen = *len - 2;
      if (name) { memcpy(name, handle->entry->Name, wlen); name[wlen / 2] = 0; }
    }
  } else status = STATUS_INVALID_HANDLE;
  atom_unlock(table);
  if (status == STATUS_SUCCESS && len) {
    if (!*len) status = ATOM_STATUS_BUFFER_TOO_SMALL;
    *len = wlen;
  }
  return status;
}

/* ---- global atoms ------------------------------------------------------------------------------------------------------------------------------------------ */

/* (One table per process, like the one kernel32's global atom calls keep; a system wide table would need the kernel.) */
static ATOM_TABLE *g_global_atoms;
static ATOM_TABLE *global_atom_table(void) {
  if (!g_global_atoms) RtlCreateAtomTable(37, &g_global_atoms);
  return g_global_atoms;
}
/* Copies a counted name (bytes) to a terminated buffer; an integer atom comes in as the pointer itself. */
static NTSTATUS global_atom_name(const WCHAR *name, ULONG bytes, WCHAR *buf, const WCHAR **out) {
  if (ATOM_IS_INT(name)) { *out = name; return STATUS_SUCCESS; }
  if (!bytes || (bytes & 1)) return STATUS_OBJECT_NAME_INVALID;
  if (bytes / 2 > ATOM_MAX_LEN) return STATUS_INVALID_PARAMETER;
  memcpy(buf, name, bytes);
  buf[bytes / 2] = 0;
  *out = buf;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI NtAddAtom(const WCHAR *name, ULONG bytes, USHORT *atom) {
  WCHAR buf[ATOM_MAX_LEN + 1];
  const WCHAR *n;
  const NTSTATUS status = global_atom_name(name, bytes, buf, &n);
  if (status) return status;
  return RtlAddAtomToAtomTable(global_atom_table(), n, atom);
}
EXPORT NTSTATUS NTAPI NtFindAtom(const WCHAR *name, ULONG bytes, USHORT *atom) {
  WCHAR buf[ATOM_MAX_LEN + 1];
  const WCHAR *n;
  const NTSTATUS status = global_atom_name(name, bytes, buf, &n);
  if (status) return status;
  return RtlLookupAtomInAtomTable(global_atom_table(), n, atom);
}
EXPORT NTSTATUS NTAPI NtDeleteAtom(USHORT atom) { return RtlDeleteAtomFromAtomTable(global_atom_table(), atom); }
typedef struct { USHORT UsageCount, Flags, NameLength; WCHAR Name[1]; } ATOM_BASIC_INFO_;
EXPORT NTSTATUS NTAPI NtQueryInformationAtom(USHORT atom, ULONG cls, void *info, ULONG length, ULONG *ret_length) {
  if (cls != 0) return STATUS_INVALID_INFO_CLASS;
  if (length < sizeof(ATOM_BASIC_INFO_)) return STATUS_INVALID_PARAMETER;
  ATOM_BASIC_INFO_ *abi = info;
  const ULONG name_len = length - sizeof(ATOM_BASIC_INFO_);
  ULONG ref = 0, pin = 0, total = sizeof(WCHAR) * (ATOM_MAX_LEN + 2);
  WCHAR tmp[ATOM_MAX_LEN + 2];
  if (!atom) return STATUS_INVALID_HANDLE;
  const NTSTATUS status = RtlQueryAtomInAtomTable(global_atom_table(), atom, &ref, &pin, tmp, &total);
  if (status) return status;
  abi->UsageCount = (USHORT)ref;
  abi->Flags = (USHORT)pin;
  if (name_len == 0 && total) {
    abi->NameLength = (USHORT)total;
    abi->Name[0] = 0;
    if (ret_length) *ret_length = sizeof(ATOM_BASIC_INFO_);
    return ATOM_STATUS_BUFFER_TOO_SMALL;
  }
  if (name_len < total) total = name_len & ~1u;
  abi->NameLength = (USHORT)total;
  memcpy(abi->Name, tmp, total);
  *(WCHAR *)((BYTE *)abi->Name + total) = 0;
  if (ret_length) *ret_length = sizeof(ATOM_BASIC_INFO_) + total;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI ZwAddAtom(const WCHAR *name, ULONG bytes, USHORT *atom) { return NtAddAtom(name, bytes, atom); }
EXPORT NTSTATUS NTAPI ZwFindAtom(const WCHAR *name, ULONG bytes, USHORT *atom) { return NtFindAtom(name, bytes, atom); }
EXPORT NTSTATUS NTAPI ZwDeleteAtom(USHORT atom) { return NtDeleteAtom(atom); }
EXPORT NTSTATUS NTAPI ZwQueryInformationAtom(USHORT atom, ULONG cls, void *info, ULONG length, ULONG *ret_length) { return NtQueryInformationAtom(atom, cls, info, length, ret_length); }
