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

/* What Chromium's shared memory regions (base::PlatformSharedMemoryRegion, mojo) do with a section on Windows: create it, query it with NtQuerySection, hand it on with
 * DuplicateHandle through a real handle of the process (same access, or reduced to read-only), map it, and check that a read-only handle cannot be mapped for writing.
 * Prints "shmregiontest: N checks, M failed". */
#include <windows.h>
#include <stdio.h>

typedef struct { PVOID BaseAddress; ULONG Attributes; LARGE_INTEGER Size; } SECTION_BASIC_INFORMATION_;
typedef LONG (WINAPI *NtQuerySectionFn)(HANDLE, int, PVOID, SIZE_T, PSIZE_T);
static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("shmregiontest: line %d failed: %s (error %lu)\n", __LINE__, #c, GetLastError()); } } while (0)

static LONG query(NtQuerySectionFn q, HANDLE h, SECTION_BASIC_INFORMATION_* out) {
  SIZE_T got = 0;
  memset(out, 0, sizeof(*out));
  return q(h, 0, out, sizeof(*out), &got);
}

int main(void) {
  NtQuerySectionFn nt_query_section = (NtQuerySectionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySection");
  CHECK(nt_query_section != NULL);
  const SIZE_T size = 0x12345;      /* (not a whole number of pages) */
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)size, NULL);
  CHECK(section != NULL);
  SECTION_BASIC_INFORMATION_ info;
  CHECK(query(nt_query_section, section, &info) == 0);
  printf("shmregiontest: attributes %#lx size %#llx\n", info.Attributes, (unsigned long long)info.Size.QuadPart);
  CHECK((info.Attributes & (SEC_IMAGE | 0x800000 /* SEC_FILE */)) == 0 && (info.Attributes & SEC_COMMIT));
  CHECK((ULONGLONG)info.Size.QuadPart >= size);

  /* How Chromium makes a writable region: the section's handle is duplicated with only read, write and query rights, and the first closed. */
  HANDLE reduced = NULL;
  CHECK(DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &reduced, FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY, FALSE, 0));
  CHECK(query(nt_query_section, reduced, &info) == 0);
  BYTE* reduced_view = MapViewOfFile(reduced, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
  CHECK(reduced_view != NULL);
  if (reduced_view) UnmapViewOfFile(reduced_view);
  HANDLE reduced_moved = NULL;      /* (and handed on with the same rights, closing the source) */
  CHECK(DuplicateHandle(GetCurrentProcess(), reduced, GetCurrentProcess(), &reduced_moved, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE));
  CHECK(query(nt_query_section, reduced_moved, &info) == 0);
  CloseHandle(reduced_moved);

  /* Chromium's sections have an empty DACL, and it tells a writable region from a read-only one by asking for a write handle: granted when the handle has that right
     already (no access check then), refused when it has not (the empty DACL grants nothing more). */
  {
    ACL dacl;
    SECURITY_DESCRIPTOR sd;
    CHECK(InitializeAcl(&dacl, sizeof dacl, ACL_REVISION) && InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&sd, TRUE, &dacl, FALSE));
    SECURITY_ATTRIBUTES sa = {sizeof sa, &sd, FALSE};
    HANDLE locked = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 0x10000, NULL);
    CHECK(locked != NULL);
    HANDLE writable_region = NULL, read_only_region = NULL, probe = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), locked, GetCurrentProcess(), &writable_region, FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY, FALSE, 0));
    CHECK(DuplicateHandle(GetCurrentProcess(), writable_region, GetCurrentProcess(), &probe, FILE_MAP_WRITE, FALSE, 0));
    if (probe) CloseHandle(probe);
    probe = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), writable_region, GetCurrentProcess(), &read_only_region, FILE_MAP_READ | SECTION_QUERY, FALSE, 0));
    SetLastError(0);
    CHECK(!DuplicateHandle(GetCurrentProcess(), read_only_region, GetCurrentProcess(), &probe, FILE_MAP_WRITE, FALSE, 0) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(query(nt_query_section, read_only_region, &info) == 0);
    CloseHandle(read_only_region);
    CloseHandle(writable_region);
    CloseHandle(locked);
  }

  /* Through a real handle of this process, as Mojo hands handles to a process. */
  HANDLE self = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
  CHECK(self != NULL);
  HANDLE same = NULL;
  CHECK(DuplicateHandle(GetCurrentProcess(), section, self, &same, 0, FALSE, DUPLICATE_SAME_ACCESS));
  CHECK(query(nt_query_section, same, &info) == 0 && (ULONGLONG)info.Size.QuadPart >= size);
  BYTE* view = MapViewOfFile(same, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
  CHECK(view != NULL);
  if (view) {
    view[size - 1] = 0x5A;
    CHECK(((ULONG_PTR)view & 0xFFFF) == 0);      /* views begin on the allocation granularity */
    CHECK(UnmapViewOfFile(view));
  }
  /* Read-only: the handle may not be mapped for writing, only for reading. */
  HANDLE read_only = NULL;
  CHECK(DuplicateHandle(GetCurrentProcess(), section, self, &read_only, FILE_MAP_READ | SECTION_QUERY, FALSE, 0));
  CHECK(query(nt_query_section, read_only, &info) == 0);
  SetLastError(0);
  void* writable = MapViewOfFile(read_only, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0);
  CHECK(writable == NULL && GetLastError() == ERROR_ACCESS_DENIED);
  BYTE* readable = MapViewOfFile(read_only, FILE_MAP_READ, 0, 0, size);
  CHECK(readable != NULL && readable[size - 1] == 0x5A);
  if (readable) UnmapViewOfFile(readable);
  /* With DUPLICATE_CLOSE_SOURCE (how a handle leaves a process). */
  HANDLE moved = NULL;
  CHECK(DuplicateHandle(GetCurrentProcess(), same, self, &moved, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE));
  CHECK(moved != NULL && query(nt_query_section, moved, &info) == 0);
  /* Two views a page apart in address order unmap one at a time (programs unmap by the 64 KiB-aligned address below a view). */
  BYTE* a = MapViewOfFile(moved, FILE_MAP_READ, 0, 0, 4096);
  BYTE* b = MapViewOfFile(moved, FILE_MAP_READ, 0, 0, 4096);
  CHECK(a && b && ((ULONG_PTR)a & 0xFFFF) == 0 && ((ULONG_PTR)b & 0xFFFF) == 0);
  CHECK(UnmapViewOfFile(b) && a[0] == 0);      /* (a is still there) */
  CHECK(UnmapViewOfFile(a));
  CloseHandle(moved);
  CloseHandle(read_only);
  CloseHandle(section);
  CloseHandle(self);
  /* The random bytes the tokens of the regions are made of (base::UnguessableToken: all zero is no token). */
  typedef BOOL (WINAPI *ProcessPrngFn)(PBYTE, SIZE_T);
  HMODULE primitives = LoadLibraryW(L"bcryptprimitives.dll");
  ProcessPrngFn prng = primitives ? (ProcessPrngFn)GetProcAddress(primitives, "ProcessPrng") : NULL;
  CHECK(prng != NULL);
  if (prng) {
    BYTE random[32] = {0};
    CHECK(prng(random, sizeof random));
    int nonzero = 0;
    for (int i = 0; i < 32; ++i) nonzero += random[i] != 0;
    printf("shmregiontest: random %02x%02x%02x%02x%02x%02x%02x%02x...\n", random[0], random[1], random[2], random[3], random[4], random[5], random[6], random[7]);
    CHECK(nonzero > 16);
  }
  printf("shmregiontest: %d checks, %d failed\n", checks, failed);
  return failed;
}
