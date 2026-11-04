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

/* Part of dbghelp.c: the image helpers (imagehlp's ImageNtHeader family), version information. */

EXPORT PIMAGE_NT_HEADERS WINAPI ImageNtHeader(PVOID base) {
  const BYTE *b = base;
  if (!b || b[0] != 'M' || b[1] != 'Z') return NULL;
  const DWORD e_lfanew = *(const DWORD *)(b + 0x3c);
  if (e_lfanew >= 0x10000000u) return NULL;
  PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(b + e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE ? nt : NULL;
}

EXPORT PIMAGE_SECTION_HEADER WINAPI ImageRvaToSection(PIMAGE_NT_HEADERS nt, PVOID base, ULONG rva) {
  (void)base;
  PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
    if (rva >= s->VirtualAddress && rva - s->VirtualAddress < s->SizeOfRawData) return s;
  return NULL;
}

EXPORT PVOID WINAPI ImageRvaToVa(PIMAGE_NT_HEADERS nt, PVOID base, ULONG rva, PIMAGE_SECTION_HEADER *last) {
  PIMAGE_SECTION_HEADER s = last ? *last : NULL;
  if (!s || rva < s->VirtualAddress || rva - s->VirtualAddress >= s->SizeOfRawData) {
    s = ImageRvaToSection(nt, base, rva);
    if (!s) return NULL;
    if (last) *last = s;
  }
  return (PVOID)((BYTE *)base + (rva - s->VirtualAddress) + s->PointerToRawData);
}

EXPORT PVOID WINAPI ImageDirectoryEntryToDataEx(PVOID base, BOOLEAN mapped_as_image, USHORT directory, PULONG size, PIMAGE_SECTION_HEADER *section) {
  if (section) *section = NULL;
  if ((ULONG_PTR)base & 1) {      /* (a base with the low bit set was mapped as data: the caller has the sections laid out as in the file) */
    base = (PVOID)((ULONG_PTR)base & ~(ULONG_PTR)3);
    mapped_as_image = FALSE;
  }
  PIMAGE_NT_HEADERS nt = ImageNtHeader(base);
  if (!nt) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  const IMAGE_DATA_DIRECTORY *dirs;
  DWORD count;
  if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    const IMAGE_OPTIONAL_HEADER64 *o = (const IMAGE_OPTIONAL_HEADER64 *)&nt->OptionalHeader;
    dirs = o->DataDirectory;
    count = o->NumberOfRvaAndSizes;
  } else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    const IMAGE_OPTIONAL_HEADER32 *o = (const IMAGE_OPTIONAL_HEADER32 *)&nt->OptionalHeader;
    dirs = o->DataDirectory;
    count = o->NumberOfRvaAndSizes;
  } else { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (directory >= count || !dirs[directory].VirtualAddress) { if (size) *size = 0; return NULL; }
  if (size) *size = dirs[directory].Size;
  if (mapped_as_image || dirs[directory].VirtualAddress < nt->OptionalHeader.SizeOfHeaders) return (BYTE *)base + dirs[directory].VirtualAddress;
  return ImageRvaToVa(nt, base, dirs[directory].VirtualAddress, section);
}

EXPORT PVOID WINAPI ImageDirectoryEntryToData(PVOID base, BOOLEAN mapped_as_image, USHORT directory, PULONG size) {
  return ImageDirectoryEntryToDataEx(base, mapped_as_image, directory, size, NULL);
}

EXPORT DWORD WINAPI GetTimestampForLoadedLibrary(HMODULE module) {
  PIMAGE_NT_HEADERS nt = ImageNtHeader(module);
  return nt ? nt->FileHeader.TimeDateStamp : 0;
}

static API_VERSION g_api_version = {4, 0, 5, 0};
EXPORT LPAPI_VERSION WINAPI ImagehlpApiVersion(void) { return &g_api_version; }
EXPORT LPAPI_VERSION WINAPI ImagehlpApiVersionEx(LPAPI_VERSION v) {
  if (!v) return NULL;
  *v = g_api_version;
  return v;
}
EXPORT LPAPI_VERSION WINAPI ExtensionApiVersion(void) {
  static API_VERSION v = {6, 0, 10, 0};
  return &v;
}
EXPORT void WINAPI SetCheckUserInterruptShared(PVOID callback) { (void)callback; }
