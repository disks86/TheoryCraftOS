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

/* version.dll for TheoryCraftOS: the version resource of a file. GetFileVersionInfo* read the PE headers and the resource section of the file (not the whole image: a
 * chrome.dll is hundreds of megabytes), walk the resource directory to the first RT_VERSION (16) entry and copy it; VerQueryValue* walk the VS_VERSIONINFO tree
 * ("\\" is the VS_FIXEDFILEINFO, "\\VarFileInfo\\Translation", "\\StringFileInfo\\040904b0\\ProductVersion" ...). Like Windows, the size given is twice the resource
 * plus 4: the second half is where VerQueryValueA puts the ANSI copies of the strings, and "FE2X" marks the block as one of this format. Everything read from the file
 * or the block is checked against the bounds it came with, so a damaged file gives an error, not a wild read. The 16-bit (ANSI) version resources of NE files are not
 * read; VerFindFile/VerInstallFile/VerLanguageName stay stubs. Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

#define MAX_RESOURCE_SECTION (64u << 20)      /* a resource section larger than this is not read */

static void copy_bytes(void *to, const void *from, SIZE_T n) {
  BYTE *d = to;
  const BYTE *s = from;
  while (n--) *d++ = *s++;
}
static WORD rd16(const BYTE *p) { return (WORD)(p[0] | p[1] << 8); }
static DWORD rd32(const BYTE *p) { return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }

/* ------------------------------------------------------------------------------------------------------------------- reading the file */

static BOOL read_at(HANDLE file, DWORD offset, void *buffer, DWORD size) {
  LARGE_INTEGER at;
  at.QuadPart = offset;
  if (!SetFilePointerEx(file, at, NULL, FILE_BEGIN)) return FALSE;
  DWORD done = 0;
  while (done < size) {
    DWORD got = 0;
    if (!ReadFile(file, (BYTE *)buffer + done, size - done, &got, NULL) || got == 0) return FALSE;
    done += got;
  }
  return TRUE;
}

static HANDLE open_file(LPCWSTR name) {
  HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file != INVALID_HANDLE_VALUE) return file;
  /* A bare file name is looked for like a library (Windows loads it with LoadLibraryEx): the search path. */
  BOOL bare = TRUE;
  for (LPCWSTR p = name; *p; ++p)
    if (*p == '\\' || *p == '/' || *p == ':') bare = FALSE;
  if (!bare) return INVALID_HANDLE_VALUE;
  WCHAR found[MAX_PATH];
  const DWORD n = SearchPathW(NULL, name, NULL, MAX_PATH, found, NULL);
  if (n == 0 || n >= MAX_PATH) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  return CreateFileW(found, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* The entry of a resource directory (at dir, inside section[0..size)) with the id wanted, or the first one for id 0xFFFFFFFF. Gives the offset it points at (with the
 * subdirectory flag still set). */
static BOOL directory_entry(const BYTE *section, DWORD size, DWORD dir, DWORD id, DWORD *target) {
  if (dir > size || size - dir < 16) return FALSE;
  const DWORD named = rd16(section + dir + 12), ids = rd16(section + dir + 14), count = named + ids;
  if (count > (size - dir - 16) / 8) return FALSE;
  for (DWORD i = 0; i < count; ++i) {
    const BYTE *e = section + dir + 16 + 8 * i;
    const DWORD name = rd32(e);
    if (id == 0xFFFFFFFFu || (!(name & 0x80000000u) && name == id)) { *target = rd32(e + 4); return TRUE; }
  }
  return FALSE;
}

/* The RT_VERSION resource of an open PE file, copied into a new heap block (*out, *out_size). The error is set on failure. */
static BOOL read_version_resource(HANDLE file, BYTE **out, DWORD *out_size) {
  BYTE header[0x400];
  LARGE_INTEGER file_size;
  if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0x40) { SetLastError(ERROR_BAD_FORMAT); return FALSE; }
  const DWORD header_size = file_size.QuadPart < (LONGLONG)sizeof header ? (DWORD)file_size.QuadPart : (DWORD)sizeof header;
  if (!read_at(file, 0, header, header_size)) return FALSE;
  if (header[0] != 'M' || header[1] != 'Z') { SetLastError(ERROR_BAD_FORMAT); return FALSE; }
  const DWORD pe = rd32(header + 0x3C);
  if (pe > header_size - 24 || rd32(header + pe) != 0x00004550u) { SetLastError(ERROR_BAD_FORMAT); return FALSE; }
  const DWORD sections = rd16(header + pe + 6), optional_size = rd16(header + pe + 20), opt = pe + 24;
  if (optional_size > header_size - opt || optional_size < 2) { SetLastError(ERROR_BAD_FORMAT); return FALSE; }
  const WORD magic = rd16(header + opt);
  const DWORD dirs = opt + (magic == 0x20B ? 112 : 96), resource_entry = dirs + 2 * 8;
  if (magic != 0x20B && magic != 0x10B) { SetLastError(ERROR_BAD_FORMAT); return FALSE; }
  if (resource_entry + 8 > opt + optional_size) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return FALSE; }
  const DWORD resource_rva = rd32(header + resource_entry);
  if (!resource_rva) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return FALSE; }
  const DWORD table = opt + optional_size;
  if (sections > (header_size - table) / 40) { SetLastError(ERROR_BAD_FORMAT); return FALSE; }
  /* The section that holds the resource directory. */
  DWORD va = 0, raw = 0, raw_size = 0;
  BOOL found = FALSE;
  for (DWORD i = 0; i < sections && !found; ++i) {
    const BYTE *s = header + table + 40 * i;
    const DWORD s_vsize = rd32(s + 8), s_va = rd32(s + 12), s_raw_size = rd32(s + 16), s_raw = rd32(s + 20);
    const DWORD extent = s_vsize > s_raw_size ? s_vsize : s_raw_size;
    if (resource_rva >= s_va && resource_rva - s_va < extent) { va = s_va; raw = s_raw; raw_size = s_raw_size; found = TRUE; }
  }
  if (!found || raw_size == 0) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return FALSE; }
  if ((ULONGLONG)raw + raw_size > (ULONGLONG)file_size.QuadPart) raw_size = (DWORD)(file_size.QuadPart - (raw < file_size.QuadPart ? raw : file_size.QuadPart));
  if (raw_size == 0 || raw_size > MAX_RESOURCE_SECTION) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); return FALSE; }
  BYTE *section = HeapAlloc(GetProcessHeap(), 0, raw_size);
  if (!section) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  BOOL ok = FALSE;
  if (read_at(file, raw, section, raw_size)) {
    const DWORD root = resource_rva - va;
    DWORD type = 0, name = 0, language = 0;
    /* type 16 -> the first name -> the first language -> the data entry (RVA, size). */
    if (!directory_entry(section, raw_size, root, 16, &type) || !(type & 0x80000000u) ||
        !directory_entry(section, raw_size, root + (type & 0x7FFFFFFFu), 0xFFFFFFFFu, &name) || !(name & 0x80000000u) ||
        !directory_entry(section, raw_size, root + (name & 0x7FFFFFFFu), 0xFFFFFFFFu, &language) || (language & 0x80000000u)) {
      SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
    } else {
      const DWORD entry = root + language;
      if (entry < root || entry > raw_size || raw_size - entry < 16) {
        SetLastError(ERROR_BAD_FORMAT);
      } else {
        const DWORD data_rva = rd32(section + entry), data_size = rd32(section + entry + 4);
        const DWORD data = data_rva - va;
        if (data_rva < va || data > raw_size || data_size > raw_size - data || data_size < 6 || data_size > 0xFFFF) {
          SetLastError(ERROR_BAD_FORMAT);
        } else if ((*out = HeapAlloc(GetProcessHeap(), 0, data_size)) == NULL) {
          SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        } else {
          copy_bytes(*out, section + data, data_size);
          *out_size = data_size;
          ok = TRUE;
        }
      }
    }
  }
  HeapFree(GetProcessHeap(), 0, section);
  return ok;
}

/* The version resource of a file by name: a heap block and its size, the size its own header says (no larger than the resource). */
static BOOL load_version(LPCWSTR name, BYTE **out, DWORD *size) {
  *out = NULL;
  *size = 0;
  if (!name || !*name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  HANDLE file = open_file(name);
  if (file == INVALID_HANDLE_VALUE) return FALSE;
  const BOOL ok = read_version_resource(file, out, size);
  CloseHandle(file);
  if (!ok) return FALSE;
  /* A 32-bit VS_VERSIONINFO: wLength, wValueLength, wType, L"VS_VERSION_INFO". */
  static const char kKey[] = "VS_VERSION_INFO";
  BOOL valid = *size >= 6 + sizeof kKey * 2;
  for (DWORD i = 0; valid && i < sizeof kKey; ++i)
    if (rd16(*out + 6 + 2 * i) != (WORD)(BYTE)kKey[i]) valid = FALSE;
  const DWORD length = valid ? rd16(*out) : 0;
  if (!valid || length < 6 + sizeof kKey * 2 || length > *size) {
    HeapFree(GetProcessHeap(), 0, *out);
    *out = NULL;
    SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return FALSE;
  }
  *size = length;
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------------------------- GetFileVersionInfo */

EXPORT DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR name, LPDWORD handle) {
  (void)flags;
  if (handle) *handle = 0;
  BYTE *block;
  DWORD size;
  if (!load_version(name, &block, &size)) return 0;
  HeapFree(GetProcessHeap(), 0, block);
  SetLastError(ERROR_SUCCESS);
  return size * 2 + 4;
}

EXPORT BOOL WINAPI GetFileVersionInfoExW(DWORD flags, LPCWSTR name, DWORD handle, DWORD capacity, LPVOID data) {
  (void)flags;
  (void)handle;
  if (!data || capacity == 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  BYTE *block;
  DWORD size;
  if (!load_version(name, &block, &size)) return FALSE;
  const DWORD n = size < capacity ? size : capacity;
  copy_bytes(data, block, n);
  if (n >= 2 && n < size) {      /* a cut block says how long it is now, so that VerQueryValue stays inside it */
    ((BYTE *)data)[0] = (BYTE)n;
    ((BYTE *)data)[1] = (BYTE)(n >> 8);
  }
  if (capacity >= size + 4) copy_bytes((BYTE *)data + size, "FE2X", 4);      /* (Windows' mark of a converted Unicode block) */
  HeapFree(GetProcessHeap(), 0, block);
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

EXPORT DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR name, LPDWORD handle) { return GetFileVersionInfoSizeExW(0, name, handle); }
EXPORT BOOL WINAPI GetFileVersionInfoW(LPCWSTR name, DWORD handle, DWORD capacity, LPVOID data) { return GetFileVersionInfoExW(0, name, handle, capacity, data); }

/* The ANSI file names: converted with the ANSI code page. */
static WCHAR *wide_of(LPCSTR text) {
  if (!text) return NULL;
  const int n = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
  if (n <= 0) return NULL;
  WCHAR *w = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(WCHAR));
  if (w && MultiByteToWideChar(CP_ACP, 0, text, -1, w, n) <= 0) { HeapFree(GetProcessHeap(), 0, w); w = NULL; }
  return w;
}

EXPORT DWORD WINAPI GetFileVersionInfoSizeExA(DWORD flags, LPCSTR name, LPDWORD handle) {
  if (handle) *handle = 0;
  WCHAR *w = wide_of(name);
  if (!w) { SetLastError(name ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return 0; }
  const DWORD r = GetFileVersionInfoSizeExW(flags, w, handle);
  HeapFree(GetProcessHeap(), 0, w);
  return r;
}
EXPORT BOOL WINAPI GetFileVersionInfoExA(DWORD flags, LPCSTR name, DWORD handle, DWORD capacity, LPVOID data) {
  WCHAR *w = wide_of(name);
  if (!w) { SetLastError(name ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return FALSE; }
  const BOOL r = GetFileVersionInfoExW(flags, w, handle, capacity, data);
  HeapFree(GetProcessHeap(), 0, w);
  return r;
}
EXPORT DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR name, LPDWORD handle) { return GetFileVersionInfoSizeExA(0, name, handle); }
EXPORT BOOL WINAPI GetFileVersionInfoA(LPCSTR name, DWORD handle, DWORD capacity, LPVOID data) { return GetFileVersionInfoExA(0, name, handle, capacity, data); }

/* ------------------------------------------------------------------------------------------------------------------- VerQueryValue */

/* One node of the tree at offset at of the block (whose bounds are [0, end)): its length, its key's position and length, the value's offset and byte size, where the
 * children start. */
typedef struct {
  DWORD length, value_length, type;
  DWORD key, key_chars;
  DWORD value, value_bytes;
  DWORD children;
} Node;

static DWORD align4(DWORD x) { return (x + 3) & ~3u; }

static BOOL read_node(const BYTE *block, DWORD end, DWORD at, Node *n) {
  if (at > end || end - at < 6) return FALSE;
  n->length = rd16(block + at);
  n->value_length = rd16(block + at + 2);
  n->type = rd16(block + at + 4);
  if (n->length < 6 || n->length > end - at) return FALSE;
  const DWORD node_end = at + n->length;
  n->key = at + 6;
  n->key_chars = 0;
  for (;;) {
    const DWORD p = n->key + 2 * n->key_chars;
    if (p + 2 > node_end) return FALSE;
    if (rd16(block + p) == 0) break;
    ++n->key_chars;
  }
  n->value = align4(n->key + 2 * n->key_chars + 2);
  if (n->value > node_end) n->value = node_end;
  n->value_bytes = n->type == 1 ? n->value_length * 2u : n->value_length;
  if (n->value_bytes > node_end - n->value) n->value_bytes = node_end - n->value;
  n->children = align4(n->value + n->value_bytes);
  if (n->children > node_end) n->children = node_end;
  return TRUE;
}

static WCHAR fold(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }

/* Whether the key of a node is the path component [part, part + chars). */
static BOOL key_is(const BYTE *block, const Node *n, LPCWSTR part, DWORD chars) {
  if (n->key_chars != chars) return FALSE;
  for (DWORD i = 0; i < chars; ++i)
    if (fold(rd16(block + n->key + 2 * i)) != fold(part[i])) return FALSE;
  return TRUE;
}

/* The node a sub-block path names (components between backslashes; empty ones are skipped), at *found. */
static BOOL find_path(const BYTE *block, LPCWSTR path, Node *found, DWORD *found_at) {
  const DWORD end = rd16(block);
  Node node;
  DWORD at = 0;
  if (!read_node(block, end, 0, &node)) return FALSE;
  LPCWSTR p = path;
  for (;;) {
    while (*p == '\\') ++p;
    if (!*p) break;
    LPCWSTR part = p;
    while (*p && *p != '\\') ++p;
    const DWORD chars = (DWORD)(p - part);
    /* Look through the children of the current node. */
    const DWORD node_end = at + node.length;
    DWORD child = node.children;
    BOOL matched = FALSE;
    Node c;
    while (child < node_end && read_node(block, node_end, child, &c)) {
      if (key_is(block, &c, part, chars)) { matched = TRUE; break; }
      const DWORD next = align4(child + c.length);
      if (next <= child) break;
      child = next;
    }
    if (!matched) return FALSE;
    node = c;
    at = child;
  }
  *found = node;
  *found_at = at;
  return TRUE;
}

EXPORT BOOL WINAPI VerQueryValueW(LPCVOID data, LPCWSTR sub_block, LPVOID *buffer, PUINT length) {
  if (buffer) *buffer = NULL;
  if (length) *length = 0;
  if (!data || !buffer) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const BYTE *block = data;
  Node node;
  DWORD at;
  if (!find_path(block, sub_block ? sub_block : L"\\", &node, &at)) { SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND); return FALSE; }
  (void)at;
  *buffer = (LPVOID)(block + node.value);
  if (length) *length = node.type == 1 ? node.value_bytes / 2 : node.value_bytes;
  return TRUE;
}

EXPORT BOOL WINAPI VerQueryValueA(LPCVOID data, LPCSTR sub_block, LPVOID *buffer, PUINT length) {
  if (buffer) *buffer = NULL;
  if (length) *length = 0;
  if (!data || !buffer) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR *path = wide_of(sub_block ? sub_block : "\\");
  if (!path) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  const BYTE *block = data;
  Node node;
  DWORD at;
  const BOOL found = find_path(block, path, &node, &at);
  HeapFree(GetProcessHeap(), 0, path);
  if (!found) { SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND); return FALSE; }
  (void)at;
  if (node.type != 1) {      /* binary values are the same in both forms */
    *buffer = (LPVOID)(block + node.value);
    if (length) *length = node.value_bytes;
    return TRUE;
  }
  /* A string: converted into the second half of the block (offset size + 4 + value/2, which keeps the copies of different strings apart), when the block has one. */
  const DWORD size = rd16(block);
  if (rd32(block + size) != 0x58324546u /* "FE2X" */) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
  char *to = (char *)block + size + 4 + node.value / 2;
  const DWORD chars = node.value_bytes / 2;
  if (chars == 0) { to[0] = 0; *buffer = to; return TRUE; }
  /* Room: chars bytes (the string's terminator, if any, is among them). */
  int n = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)(block + node.value), (int)chars, to, (int)chars, NULL, NULL);
  if (n <= 0) n = 0;
  if ((DWORD)n >= chars) n = (int)chars - 1;
  to[n] = 0;
  *buffer = to;
  if (length) *length = (UINT)(n + (node.value_bytes / 2 > 0 && rd16(block + node.value + node.value_bytes - 2) == 0 ? 1 : 0));
  return TRUE;
}
