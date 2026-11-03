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

/* EnumResourceTypes, EnumResourceNames, EnumResourceLanguages (A, W and Ex): walk the resource directory (see "resources of a module" in extra2.inc) and call back for each entry;
 * the callback stops the walk by returning FALSE. And GetBinaryType. Included by kernel32.c after extra2.inc. */

typedef BOOL (CALLBACK *ENUMRESTYPEPROCW_)(HMODULE, LPWSTR, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESNAMEPROCW_)(HMODULE, LPCWSTR, LPWSTR, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESLANGPROCW_)(HMODULE, LPCWSTR, LPCWSTR, WORD, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESTYPEPROCA_)(HMODULE, LPSTR, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESNAMEPROCA_)(HMODULE, LPCSTR, LPSTR, LONG_PTR);
typedef BOOL (CALLBACK *ENUMRESLANGPROCA_)(HMODULE, LPCSTR, LPCSTR, WORD, LONG_PTR);

/* The name of a directory entry as the callback gets it: the id as a small number, or a terminated copy of the counted string. */
static LPWSTR resource_entry_name(const ResourceDirectory *root, const ResourceEntry *e, WCHAR *buffer, size_t_ capacity) {
  if (!(e->Name & 0x80000000u)) return (LPWSTR)(ULONG_PTR)(e->Name & 0xFFFF);
  const WORD *s = (const WORD *)((const BYTE *)root + (e->Name & 0x7FFFFFFF));
  const size_t_ n = s[0] < capacity - 1 ? s[0] : capacity - 1;
  for (size_t_ i = 0; i < n; ++i) buffer[i] = s[1 + i];
  buffer[n] = 0;
  return buffer;
}
/* A string as ANSI for the A callbacks (ids stay numbers). */
static LPSTR resource_ansi_name(LPCWSTR name, char *buffer, int capacity) {
  if (((ULONG_PTR)name >> 16) == 0) return (LPSTR)name;
  if (!WideCharToMultiByte(CP_ACP, 0, name, -1, buffer, capacity, NULL, NULL)) buffer[0] = 0;
  return buffer;
}
static const ResourceDirectory *resource_sub(const ResourceDirectory *root, const ResourceEntry *e) {
  if (!(e->OffsetToData & 0x80000000u)) return NULL;
  return (const ResourceDirectory *)((const BYTE *)root + (e->OffsetToData & 0x7FFFFFFF));
}

/* kind: 0 types, 1 names of `type`, 2 languages of `type`/`name`; `ansi` picks the callback form. */
static BOOL enumerate_resources(HMODULE module, LPCWSTR type, LPCWSTR name, void *proc, LONG_PTR param, int kind, BOOL ansi) {
  DWORD size = 0;
  if (!proc) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const ResourceDirectory *root = resource_root(module, &size);
  if (!root) { set_error(1812 /* ERROR_RESOURCE_DATA_NOT_FOUND */); return FALSE; }
  HMODULE callback_module = module;
  const ResourceDirectory *dir = root;
  if (kind >= 1) {
    const ResourceEntry *te = resource_find(root, root, type);
    const ResourceDirectory *names = te ? resource_sub(root, te) : NULL;
    if (!names) { set_error(1813 /* ERROR_RESOURCE_TYPE_NOT_FOUND */); return FALSE; }
    dir = names;
    if (kind == 2) {
      const ResourceEntry *ne = resource_find(root, names, name);
      const ResourceDirectory *langs = ne ? resource_sub(root, ne) : NULL;
      if (!langs) { set_error(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */); return FALSE; }
      dir = langs;
    }
  }
  const ResourceEntry *entries = (const ResourceEntry *)(dir + 1);
  const unsigned count = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
  for (unsigned i = 0; i < count; ++i) {
    WCHAR buffer[256];
    char narrow[256], narrow_type[256];
    BOOL keep_going;
    LPWSTR entry_name = resource_entry_name(root, &entries[i], buffer, 256);
    if (kind == 0) {
      keep_going = ansi ? ((ENUMRESTYPEPROCA_)proc)(callback_module, resource_ansi_name(entry_name, narrow, sizeof(narrow)), param) : ((ENUMRESTYPEPROCW_)proc)(callback_module, entry_name, param);
    } else if (kind == 1) {
      keep_going = ansi ? ((ENUMRESNAMEPROCA_)proc)(callback_module, resource_ansi_name(type, narrow_type, sizeof(narrow_type)), resource_ansi_name(entry_name, narrow, sizeof(narrow)), param)
                        : ((ENUMRESNAMEPROCW_)proc)(callback_module, type, entry_name, param);
    } else {
      const WORD language = (WORD)(ULONG_PTR)entry_name;
      char narrow_name[256];
      keep_going = ansi ? ((ENUMRESLANGPROCA_)proc)(callback_module, resource_ansi_name(type, narrow_type, sizeof(narrow_type)), resource_ansi_name(name, narrow_name, sizeof(narrow_name)), language, param)
                        : ((ENUMRESLANGPROCW_)proc)(callback_module, type, name, language, param);
    }
    if (!keep_going) break;
  }
  return TRUE;
}
static LPCWSTR resource_arg_wide(LPCSTR a, WCHAR **owned) { *owned = NULL; if (((ULONG_PTR)a >> 16) == 0) return (LPCWSTR)a; *owned = to_wide(a); return *owned; }

EXPORT BOOL WINAPI EnumResourceTypesExW(HMODULE module, ENUMRESTYPEPROCW_ proc, LONG_PTR param, DWORD flags, LANGID language) { (void)flags; (void)language; return enumerate_resources(module, NULL, NULL, (void *)proc, param, 0, FALSE); }
EXPORT BOOL WINAPI EnumResourceTypesW(HMODULE module, ENUMRESTYPEPROCW_ proc, LONG_PTR param) { return enumerate_resources(module, NULL, NULL, (void *)proc, param, 0, FALSE); }
EXPORT BOOL WINAPI EnumResourceTypesExA(HMODULE module, ENUMRESTYPEPROCA_ proc, LONG_PTR param, DWORD flags, LANGID language) { (void)flags; (void)language; return enumerate_resources(module, NULL, NULL, (void *)proc, param, 0, TRUE); }
EXPORT BOOL WINAPI EnumResourceTypesA(HMODULE module, ENUMRESTYPEPROCA_ proc, LONG_PTR param) { return enumerate_resources(module, NULL, NULL, (void *)proc, param, 0, TRUE); }
EXPORT BOOL WINAPI EnumResourceNamesExW(HMODULE module, LPCWSTR type, ENUMRESNAMEPROCW_ proc, LONG_PTR param, DWORD flags, LANGID language) { (void)flags; (void)language; return enumerate_resources(module, type, NULL, (void *)proc, param, 1, FALSE); }
EXPORT BOOL WINAPI EnumResourceNamesW(HMODULE module, LPCWSTR type, ENUMRESNAMEPROCW_ proc, LONG_PTR param) { return enumerate_resources(module, type, NULL, (void *)proc, param, 1, FALSE); }
EXPORT BOOL WINAPI EnumResourceNamesExA(HMODULE module, LPCSTR type, ENUMRESNAMEPROCA_ proc, LONG_PTR param, DWORD flags, LANGID language) {
  (void)flags; (void)language;
  WCHAR *owned;
  LPCWSTR t = resource_arg_wide(type, &owned);
  if (!t) return FALSE;
  const BOOL ok = enumerate_resources(module, t, NULL, (void *)proc, param, 1, TRUE);
  free_wide(owned);
  return ok;
}
EXPORT BOOL WINAPI EnumResourceNamesA(HMODULE module, LPCSTR type, ENUMRESNAMEPROCA_ proc, LONG_PTR param) { return EnumResourceNamesExA(module, type, proc, param, 0, 0); }
EXPORT BOOL WINAPI EnumResourceLanguagesExW(HMODULE module, LPCWSTR type, LPCWSTR name, ENUMRESLANGPROCW_ proc, LONG_PTR param, DWORD flags, LANGID language) { (void)flags; (void)language; return enumerate_resources(module, type, name, (void *)proc, param, 2, FALSE); }
EXPORT BOOL WINAPI EnumResourceLanguagesW(HMODULE module, LPCWSTR type, LPCWSTR name, ENUMRESLANGPROCW_ proc, LONG_PTR param) { return enumerate_resources(module, type, name, (void *)proc, param, 2, FALSE); }
EXPORT BOOL WINAPI EnumResourceLanguagesExA(HMODULE module, LPCSTR type, LPCSTR name, ENUMRESLANGPROCA_ proc, LONG_PTR param, DWORD flags, LANGID language) {
  (void)flags; (void)language;
  WCHAR *owned_type, *owned_name;
  LPCWSTR t = resource_arg_wide(type, &owned_type), n = resource_arg_wide(name, &owned_name);
  const BOOL ok = t && n ? enumerate_resources(module, t, n, (void *)proc, param, 2, TRUE) : FALSE;
  free_wide(owned_type); free_wide(owned_name);
  return ok;
}
EXPORT BOOL WINAPI EnumResourceLanguagesA(HMODULE module, LPCSTR type, LPCSTR name, ENUMRESLANGPROCA_ proc, LONG_PTR param) { return EnumResourceLanguagesExA(module, type, name, proc, param, 0, 0); }

/* ---- GetBinaryType --------------------------------------------------------------------------------------------------------------------------------------------- */

/* What kind of program a file is: a 32- or 64-bit Windows program, a DOS program, a 16-bit Windows or OS/2 program (the executable headers decide, as the loader would). */
EXPORT BOOL WINAPI GetBinaryTypeW(LPCWSTR path, LPDWORD type) {
  if (!path || !type) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return FALSE;
  BYTE mz[64];
  DWORD got = 0;
  BOOL ok = FALSE;
  set_error(ERROR_BAD_EXE_FORMAT);
  if (ReadFile(f, mz, sizeof(mz), &got, NULL) && got == sizeof(mz) && mz[0] == 'M' && mz[1] == 'Z') {
    const DWORD offset = *(const DWORD *)(mz + 0x3C);
    BYTE header[0x18 + 2];
    DWORD result = 1;      /* SCS_DOS_BINARY */
    if (offset >= sizeof(mz) && offset < 0x10000000 && SetFilePointer(f, (LONG)offset, NULL, FILE_BEGIN) == offset && ReadFile(f, header, sizeof(header), &got, NULL) && got == sizeof(header)) {
      if (header[0] == 'P' && header[1] == 'E' && !header[2] && !header[3]) {
        const WORD machine = *(const WORD *)(header + 4);
        result = machine == 0x8664 || machine == 0xAA64 ? 6 : 0;      /* SCS_64BIT_BINARY, SCS_32BIT_BINARY */
        if (machine != 0x8664 && machine != 0xAA64 && machine != 0x14C && machine != 0x1C4 && machine != 0x1C0) result = 0xFFFFFFFFu;
      } else if (header[0] == 'N' && header[1] == 'E') result = 2;      /* SCS_WOW_BINARY */
      else if (header[0] == 'L' && (header[1] == 'E' || header[1] == 'X')) result = 1;
    }
    if (result != 0xFFFFFFFFu) { *type = result; ok = TRUE; }
  }
  CloseHandle(f);
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
