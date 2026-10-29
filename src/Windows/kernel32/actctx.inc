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

/* kernel32.dll: activation contexts (the work is ntdll's, src/Windows/ntdll/actctx.inc; this reads the manifest - from a file or from a module's resources - and passes
 * the Win32 calls on). A context handle is ntdll's context. */

IMPORT NTSTATUS NTAPI TcosCreateActivationContext(const BYTE *, SIZE_T, PCWSTR, PCWSTR, PCWSTR, ULONG, PVOID *);
IMPORT NTSTATUS NTAPI RtlActivateActivationContext(ULONG, PVOID, ULONG_PTR *);
IMPORT VOID NTAPI RtlDeactivateActivationContext(ULONG, ULONG_PTR);
IMPORT VOID NTAPI RtlAddRefActivationContext(PVOID);
IMPORT VOID NTAPI RtlReleaseActivationContext(PVOID);
IMPORT NTSTATUS NTAPI RtlZombifyActivationContext(PVOID);
IMPORT VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING, PCWSTR);
IMPORT NTSTATUS NTAPI RtlGetActiveActivationContext(PVOID *);
IMPORT NTSTATUS NTAPI RtlQueryInformationActivationContext(ULONG, PVOID, PVOID, ULONG, PVOID, SIZE_T, SIZE_T *);
IMPORT NTSTATUS NTAPI RtlFindActivationContextSectionString(ULONG, const void *, ULONG, PUNICODE_STRING, PVOID);
IMPORT NTSTATUS NTAPI RtlQueryActivationContextApplicationSettings(ULONG, PVOID, PCWSTR, PCWSTR, PWSTR, SIZE_T, SIZE_T *);
IMPORT NTSTATUS NTAPI RtlFindActivationContextSectionGuid(ULONG, const void *, ULONG, const void *, PVOID);

typedef struct ActCtxW {
  DWORD cbSize, dwFlags;
  LPCWSTR lpSource;
  WORD wProcessorArchitecture, wLangId;
  LPCWSTR lpAssemblyDirectory, lpResourceName, lpApplicationName;
  HMODULE hModule;
} ActCtxW;
typedef struct ActCtxA {
  DWORD cbSize, dwFlags;
  LPCSTR lpSource;
  WORD wProcessorArchitecture, wLangId;
  LPCSTR lpAssemblyDirectory, lpResourceName, lpApplicationName;
  HMODULE hModule;
} ActCtxA;

#define ACTCTX_ARCH_VALID 0x01
#define ACTCTX_LANG_VALID 0x02
#define ACTCTX_DIR_VALID 0x04
#define ACTCTX_RESOURCE_VALID 0x08
#define ACTCTX_PROCESS_DEFAULT 0x10
#define ACTCTX_APPNAME_VALID 0x20
#define ACTCTX_HMODULE_VALID 0x80

/* The whole file, or NULL with the error set. */
static BYTE *actctx_read_file(LPCWSTR path, DWORD *size) {
  HANDLE h = CreateFileW(path, 0x80000000u /* GENERIC_READ */, 7, NULL, 3 /* OPEN_EXISTING */, 0x80, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  const DWORD len = GetFileSize(h, NULL);
  BYTE *data = len == INVALID_FILE_SIZE || len > 0x10000000u ? NULL : RtlAllocateHeap(heap(), 0, (SIZE_T)len + 1);
  DWORD got = 0;
  if (!data) { CloseHandle(h); set_error(ERROR_INVALID_PARAMETER); return NULL; }
  if (len && (!ReadFile(h, data, len, &got, NULL) || got != len)) { CloseHandle(h); RtlFreeHeap(heap(), 0, data); return NULL; }
  CloseHandle(h);
  *size = len;
  return data;
}

/* The directory part of a path, with its closing backslash. */
static void actctx_dir_of(LPCWSTR path, LPWSTR out, SIZE_T capacity) {
  SIZE_T n = wcslen(path);
  while (n && path[n - 1] != '\\' && path[n - 1] != '/') --n;
  if (n >= capacity) n = 0;
  memmove(out, path, n * 2);
  out[n] = 0;
}

EXPORT HANDLE WINAPI CreateActCtxW(const ActCtxW *param) {
  ActCtxW p;
  memset(&p, 0, sizeof(p));
  if (!param || param->cbSize < 16) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  memcpy(&p, param, param->cbSize < sizeof(p) ? param->cbSize : sizeof(p));
  /* a flag needs its field to be inside the structure the caller says it passed */
  DWORD need = 16;
  if ((p.dwFlags & ACTCTX_ARCH_VALID) && need < 18) need = 18;
  if ((p.dwFlags & ACTCTX_LANG_VALID) && need < 20) need = 20;
  if ((p.dwFlags & ACTCTX_DIR_VALID) && need < 32) need = 32;
  if ((p.dwFlags & ACTCTX_RESOURCE_VALID) && need < 40) need = 40;
  if ((p.dwFlags & ACTCTX_APPNAME_VALID) && need < 48) need = 48;
  if ((p.dwFlags & ACTCTX_HMODULE_VALID) && need < 56) need = 56;
  if (param->cbSize < need || (p.dwFlags & ~0xBFu) || ((p.dwFlags & ACTCTX_RESOURCE_VALID) && !p.lpResourceName)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if ((p.dwFlags & ACTCTX_HMODULE_VALID) && !(p.dwFlags & ACTCTX_RESOURCE_VALID)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (!p.lpSource && !(p.dwFlags & ACTCTX_HMODULE_VALID)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }

  WCHAR full[MAX_PATH], search_dir[MAX_PATH], app_dir[MAX_PATH];
  full[0] = search_dir[0] = app_dir[0] = 0;
  BYTE *owned = NULL;
  const BYTE *manifest = NULL;
  DWORD size = 0;
  HMODULE loaded = NULL;
  const WCHAR *dir = (p.dwFlags & ACTCTX_DIR_VALID) ? p.lpAssemblyDirectory : NULL;
  if (dir && !dir[0]) dir = NULL;

  if (p.dwFlags & ACTCTX_RESOURCE_VALID) {      /* the manifest is a resource of a module (RT_MANIFEST = 24) */
    HMODULE module = (p.dwFlags & ACTCTX_HMODULE_VALID) ? p.hModule : NULL;
    if (!module) {
      if (!p.lpSource) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
      module = loaded = LoadLibraryExW(p.lpSource, NULL, 0x2 /* LOAD_LIBRARY_AS_DATAFILE */);
      if (!module) return INVALID_HANDLE_VALUE;
    }
    HRSRC r = FindResourceW(module, p.lpResourceName, (LPCWSTR)(ULONG_PTR)24);
    if (r) {
      HGLOBAL g = LoadResource(module, r);
      manifest = g ? LockResource(g) : NULL;
      size = SizeofResource(module, r);
    }
    if (!manifest) { const DWORD e = GetLastError(); if (loaded) FreeLibrary(loaded); set_error(e ? e : 1814); return INVALID_HANDLE_VALUE; }
    if (p.lpSource) GetFullPathNameW(p.lpSource, MAX_PATH, full, NULL);
    else GetModuleFileNameW(module, full, MAX_PATH);
    if (!dir) actctx_dir_of(full, search_dir, MAX_PATH);
  } else {      /* the manifest is a file; a relative name is looked for in the directory the caller gave */
    WCHAR joined[MAX_PATH * 2];
    LPCWSTR name = p.lpSource;
    if (dir && !(name[0] == '\\' || name[0] == '/' || (name[0] && name[1] == ':'))) {
      const SIZE_T dl = wcslen(dir), nl = wcslen(name);
      if (dl + nl + 2 >= MAX_PATH * 2) { set_error(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
      memcpy(joined, dir, dl * 2);
      SIZE_T o = dl;
      if (o && joined[o - 1] != '\\' && joined[o - 1] != '/') joined[o++] = '\\';
      memcpy(joined + o, name, (nl + 1) * 2);
      name = joined;
    }
    if (!GetFullPathNameW(name, MAX_PATH, full, NULL)) return INVALID_HANDLE_VALUE;
    owned = actctx_read_file(full, &size);
    if (!owned) return INVALID_HANDLE_VALUE;
    manifest = owned;
    actctx_dir_of(full, search_dir, MAX_PATH);
  }
  if (dir) {
    const SIZE_T dl = wcslen(dir);
    if (dl >= MAX_PATH) { if (owned) RtlFreeHeap(heap(), 0, owned); if (loaded) FreeLibrary(loaded); set_error(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
    memcpy(app_dir, dir, (dl + 1) * 2);
    memcpy(search_dir, dir, (dl + 1) * 2);
  } else if (GetModuleFileNameW(NULL, app_dir, MAX_PATH)) actctx_dir_of(app_dir, app_dir, MAX_PATH);

  PVOID ctx = NULL;
  const NTSTATUS status = TcosCreateActivationContext(manifest, size, full, search_dir, app_dir, p.dwFlags, &ctx);
  if (owned) RtlFreeHeap(heap(), 0, owned);
  if (loaded) FreeLibrary(loaded);
  if (!NT_SUCCESS(status)) { fail_nt(status); return INVALID_HANDLE_VALUE; }
  return ctx;
}

EXPORT HANDLE WINAPI CreateActCtxA(const ActCtxA *param) {
  if (!param || param->cbSize < 16) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  ActCtxA a;
  memset(&a, 0, sizeof(a));
  memcpy(&a, param, param->cbSize < sizeof(a) ? param->cbSize : sizeof(a));
  ActCtxW w;
  memset(&w, 0, sizeof(w));
  w.cbSize = param->cbSize;
  w.dwFlags = a.dwFlags;
  w.wProcessorArchitecture = a.wProcessorArchitecture;
  w.wLangId = a.wLangId;
  w.hModule = a.hModule;
  WCHAR *source = a.lpSource ? to_wide(a.lpSource) : NULL, *dir = (a.dwFlags & ACTCTX_DIR_VALID) && a.lpAssemblyDirectory ? to_wide(a.lpAssemblyDirectory) : NULL;
  WCHAR *app = (a.dwFlags & ACTCTX_APPNAME_VALID) && a.lpApplicationName ? to_wide(a.lpApplicationName) : NULL;
  WCHAR *res = NULL;
  const int res_string = (a.dwFlags & ACTCTX_RESOURCE_VALID) && ((ULONG_PTR)a.lpResourceName >> 16) != 0;
  if (res_string) res = to_wide(a.lpResourceName);
  w.lpSource = source;
  w.lpAssemblyDirectory = dir;
  w.lpApplicationName = app;
  w.lpResourceName = res_string ? res : (LPCWSTR)a.lpResourceName;
  HANDLE h = (a.lpSource && !source) || (res_string && !res) ? INVALID_HANDLE_VALUE : CreateActCtxW(&w);
  free_wide(source); free_wide(dir); free_wide(app); free_wide(res);
  return h;
}

EXPORT BOOL WINAPI ActivateActCtx(HANDLE h, ULONG_PTR *cookie) {
  const NTSTATUS s = RtlActivateActivationContext(0, h, cookie);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}
EXPORT BOOL WINAPI DeactivateActCtx(DWORD flags, ULONG_PTR cookie) { RtlDeactivateActivationContext(flags, cookie); return TRUE; }
EXPORT BOOL WINAPI GetCurrentActCtx(HANDLE *h) {
  const NTSTATUS s = RtlGetActiveActivationContext(h);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}
EXPORT void WINAPI AddRefActCtx(HANDLE h) { if (h && h != INVALID_HANDLE_VALUE) RtlAddRefActivationContext(h); }
EXPORT void WINAPI ReleaseActCtx(HANDLE h) { if (h && h != INVALID_HANDLE_VALUE) RtlReleaseActivationContext(h); }
EXPORT BOOL WINAPI ZombifyActCtx(HANDLE h) {
  if (!h || h == INVALID_HANDLE_VALUE) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const NTSTATUS s = RtlZombifyActivationContext(h);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}
EXPORT BOOL WINAPI QueryActCtxW(DWORD flags, HANDLE h, PVOID subinstance, ULONG class, PVOID buffer, SIZE_T length, SIZE_T *written) {
  const NTSTATUS s = RtlQueryInformationActivationContext(flags, h, subinstance, class, buffer, length, written);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}
EXPORT BOOL WINAPI FindActCtxSectionStringW(DWORD flags, const void *ext, ULONG kind, LPCWSTR string, PVOID data) {
  if (!string || !data) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  UNICODE_STRING us;
  RtlInitUnicodeString(&us, string);
  const NTSTATUS s = RtlFindActivationContextSectionString(flags, ext, kind, &us, data);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}
EXPORT BOOL WINAPI FindActCtxSectionStringA(DWORD flags, const void *ext, ULONG kind, LPCSTR string, PVOID data) {
  WCHAR *w = string && data ? to_wide(string) : NULL;
  if (!w) { set_error(string && data ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return FALSE; }
  const BOOL r = FindActCtxSectionStringW(flags, ext, kind, w, data);
  const DWORD e = GetLastError();
  free_wide(w);
  if (!r) set_error(e);
  return r;
}
EXPORT BOOL WINAPI FindActCtxSectionGuid(DWORD flags, const void *ext, ULONG kind, const void *guid, PVOID data) {
  if (!guid || !data) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const NTSTATUS s = RtlFindActivationContextSectionGuid(flags, ext, kind, guid, data);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}
EXPORT BOOL WINAPI QueryActCtxSettingsW(DWORD flags, HANDLE h, LPCWSTR ns, LPCWSTR name, LPWSTR buffer, SIZE_T size, SIZE_T *written) {
  const NTSTATUS s = RtlQueryActivationContextApplicationSettings(flags, h, ns, name, buffer, size, written);
  return NT_SUCCESS(s) ? TRUE : fail_nt(s);
}
