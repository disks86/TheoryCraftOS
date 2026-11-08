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

/* ---- SymSrvGetFileIndexInfo: what a symbol server indexes a file by ---- *
 * For an image: its time stamp and SizeOfImage, and from its CodeView debug record the PDB name with the GUID and age (RSDS) or signature and age (NB10). Only the
 * headers, the debug directory and the record are read from the file, each checked against what was read. */
static BOOL index_read_at(HANDLE file, DWORD offset, void *buffer, DWORD size, DWORD *got) {
  LARGE_INTEGER at;
  at.QuadPart = offset;
  *got = 0;
  if (!SetFilePointerEx(file, at, NULL, FILE_BEGIN)) return FALSE;
  while (*got < size) {
    DWORD n = 0;
    if (!ReadFile(file, (BYTE *)buffer + *got, size - *got, &n, NULL)) return FALSE;
    if (n == 0) break;
    *got += n;
  }
  return TRUE;
}
static DWORD index_rd32(const BYTE *p) { return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }

static BOOL file_index_info(HANDLE file, SYMSRV_INDEX_INFO *info) {
  BYTE header[4096];
  DWORD got;
  if (!index_read_at(file, 0, header, sizeof header, &got) || got < 0x40 || header[0] != 'M' || header[1] != 'Z') { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
  const DWORD pe = index_rd32(header + 0x3C);
  if (pe > got - 24 || index_rd32(header + pe) != IMAGE_NT_SIGNATURE) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
  const DWORD sections = *(const WORD *)(header + pe + 6), optional_size = *(const WORD *)(header + pe + 20), opt = pe + 24;
  if (optional_size < 2 || optional_size > got - opt) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
  const WORD magic = *(const WORD *)(header + opt);
  if (magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC && magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) { SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE; }
  info->timestamp = index_rd32(header + pe + 8);
  info->stripped = (*(const WORD *)(header + pe + 22) & IMAGE_FILE_DEBUG_STRIPPED) != 0;
  if (optional_size >= 60) info->size = index_rd32(header + opt + 56);      /* SizeOfImage */
  const DWORD dirs = opt + (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? 112 : 96), debug_entry = dirs + 8 * IMAGE_DIRECTORY_ENTRY_DEBUG;
  if (debug_entry + 8 > opt + optional_size) return TRUE;      /* no debug directory: what there is */
  const DWORD debug_rva = index_rd32(header + debug_entry), debug_size = index_rd32(header + debug_entry + 4);
  const DWORD table = opt + optional_size;
  if (!debug_rva || !debug_size || sections > (got - table) / 40) return TRUE;
  DWORD debug_offset = 0;
  BOOL mapped = FALSE;
  for (DWORD i = 0; i < sections && !mapped; i++) {
    const BYTE *s = header + table + 40 * i;
    const DWORD vsize = index_rd32(s + 8), va = index_rd32(s + 12), raw_size = index_rd32(s + 16), raw = index_rd32(s + 20);
    const DWORD extent = vsize > raw_size ? vsize : raw_size;
    if (debug_rva >= va && debug_rva - va < extent) { debug_offset = raw + (debug_rva - va); mapped = TRUE; }
  }
  if (!mapped) return TRUE;
  IMAGE_DEBUG_DIRECTORY entries[16];
  DWORD count = debug_size / sizeof entries[0];
  if (count > 16) count = 16;
  if (!index_read_at(file, debug_offset, entries, count * (DWORD)sizeof entries[0], &got)) return TRUE;
  count = got / (DWORD)sizeof entries[0];
  for (DWORD i = 0; i < count; i++) {
    if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[i].SizeOfData < 16) continue;
    BYTE record[24 + MAX_PATH + 1];
    const DWORD want = entries[i].SizeOfData < sizeof record ? entries[i].SizeOfData : (DWORD)sizeof record;
    if (!index_read_at(file, entries[i].PointerToRawData, record, want, &got) || got < 16) continue;
    DWORD name = 0;
    if (got >= 24 && record[0] == 'R' && record[1] == 'S' && record[2] == 'D' && record[3] == 'S') {
      mcopy(&info->guid, record + 4, sizeof info->guid);
      info->age = index_rd32(record + 20);
      name = 24;
    } else if (record[0] == 'N' && record[1] == 'B' && record[2] == '1' && record[3] == '0') {
      info->sig = index_rd32(record + 8);
      info->age = index_rd32(record + 12);
      name = 16;
    } else {
      continue;
    }
    /* the PDB's file name (the record holds the path the linker wrote) */
    DWORD end = name;
    while (end < got && record[end]) end++;
    DWORD start = end;
    while (start > name && record[start - 1] != '\\' && record[start - 1] != '/') start--;
    DWORD o = 0;
    for (DWORD k = start; k < end && o < MAX_PATH; k++) info->pdbfile[o++] = (CHAR)record[k];
    info->pdbfile[o] = 0;
    break;
  }
  return TRUE;
}

EXPORT BOOL WINAPI SymSrvGetFileIndexInfoW(PCWSTR path, PSYMSRV_INDEX_INFOW info, DWORD flags) {
  (void)flags;
  if (!path || !info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (info->sizeofstruct != sizeof *info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return FALSE;
  SYMSRV_INDEX_INFO *narrow = xalloc(sizeof *narrow);
  if (!narrow) { CloseHandle(file); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  const BOOL ok = file_index_info(file, narrow);
  CloseHandle(file);
  if (ok) {
    mzero((BYTE *)info + sizeof info->sizeofstruct, sizeof *info - sizeof info->sizeofstruct);
    SIZE_T n = wlen(path), start = n;
    while (start > 0 && path[start - 1] != '\\' && path[start - 1] != '/') start--;
    SIZE_T o = 0;
    for (SIZE_T k = start; k < n && o < MAX_PATH; k++) info->file[o++] = path[k];
    info->file[o] = 0;
    info->stripped = narrow->stripped;
    info->timestamp = narrow->timestamp;
    info->size = narrow->size;
    for (o = 0; narrow->pdbfile[o] && o < MAX_PATH; o++) info->pdbfile[o] = (WCHAR)(BYTE)narrow->pdbfile[o];
    info->pdbfile[o] = 0;
    info->guid = narrow->guid;
    info->sig = narrow->sig;
    info->age = narrow->age;
  }
  xfree(narrow);
  return ok;
}

EXPORT BOOL WINAPI SymSrvGetFileIndexInfo(PCSTR path, PSYMSRV_INDEX_INFO info, DWORD flags) {
  (void)flags;
  if (!path || !info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (info->sizeofstruct != sizeof *info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return FALSE;
  mzero((BYTE *)info + sizeof info->sizeofstruct, sizeof *info - sizeof info->sizeofstruct);
  const BOOL ok = file_index_info(file, info);
  CloseHandle(file);
  if (ok) {
    SIZE_T n = slen(path), start = n;
    while (start > 0 && path[start - 1] != '\\' && path[start - 1] != '/') start--;
    SIZE_T o = 0;
    for (SIZE_T k = start; k < n && o < MAX_PATH; k++) info->file[o++] = path[k];
    info->file[o] = 0;
  }
  return ok;
}
