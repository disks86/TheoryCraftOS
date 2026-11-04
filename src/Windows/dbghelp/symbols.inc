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

/* Part of dbghelp.c: the symbol handler's process and module calls (SymInitialize ... SymEnumerateModules), options, search path, callbacks. */

#define LOCK EnterCriticalSection(&g_lock)
#define UNLOCK LeaveCriticalSection(&g_lock)

EXPORT DWORD WINAPI SymSetOptions(DWORD options) { g_options = options; return g_options; }
EXPORT DWORD WINAPI SymGetOptions(void) { return g_options; }

static char *default_search_path(void) {
  char buffer[2048];
  SIZE_T n = scopy(buffer, sizeof(buffer), ".");
  static const char *vars[] = {"_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH"};
  for (int i = 0; i < 2; i++) {
    char value[1024];
    const DWORD got = GetEnvironmentVariableA(vars[i], value, sizeof(value));
    if (got && got < sizeof(value) && n + 1 + got < sizeof(buffer)) { buffer[n++] = ';'; scopy(buffer + n, sizeof(buffer) - n, value); n += got; }
  }
  return sdup(buffer);
}

static BOOL initialize(HANDLE process, const char *search, BOOL invade) {
  LOCK;
  if (find_process(process)) { UNLOCK; SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SYMPROC *p = xalloc(sizeof(*p));
  if (!p) { UNLOCK; SetLastError(ERROR_OUTOFMEMORY); return FALSE; }
  p->handle = process;
  p->search_path = search ? sdup(search) : default_search_path();
  p->next = g_processes;
  g_processes = p;
  if (invade) load_process_modules(p);      /* (a process whose modules cannot be listed has none) */
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymInitialize(HANDLE process, PCSTR search, BOOL invade) { return initialize(process, search, invade); }
EXPORT BOOL WINAPI SymInitializeW(HANDLE process, PCWSTR search, BOOL invade) {
  char *a = to_ansi(search);
  const BOOL r = initialize(process, a, invade);
  xfree(a);
  return r;
}

EXPORT BOOL WINAPI SymCleanup(HANDLE process) {
  LOCK;
  for (SYMPROC **pp = &g_processes; *pp; pp = &(*pp)->next) {
    SYMPROC *p = *pp;
    if (p->handle != process) continue;
    *pp = p->next;
    while (p->modules) { MODULE *m = p->modules; p->modules = m->next; module_free(m); }
    xfree(p->search_path);
    xfree(p);
    UNLOCK;
    return TRUE;
  }
  UNLOCK;
  SetLastError(ERROR_INVALID_HANDLE);
  return FALSE;
}

EXPORT BOOL WINAPI SymRefreshModuleList(HANDLE process) {
  LOCK;
  SYMPROC *p = find_process(process);
  const BOOL ok = p && load_process_modules(p);
  const DWORD error = GetLastError();
  UNLOCK;
  if (!p) SetLastError(ERROR_INVALID_HANDLE); else if (!ok) SetLastError(error);
  return ok;
}

typedef BOOL(CALLBACK *PSYM_ENUMPROCESSES_CALLBACK)(HANDLE, ULONG64);
EXPORT BOOL WINAPI SymEnumProcesses(PSYM_ENUMPROCESSES_CALLBACK callback, PVOID context) {
  LOCK;
  for (SYMPROC *p = g_processes; p; p = p->next) if (!callback(p->handle, (ULONG64)(ULONG_PTR)context)) break;
  UNLOCK;
  return TRUE;
}

static DWORD64 load_module(HANDLE process, const char *image, const char *name, DWORD64 base, DWORD size, DWORD flags) {
  LOCK;
  SYMPROC *p = find_process(process);
  if (!p) { UNLOCK; SetLastError(ERROR_INVALID_HANDLE); return 0; }
  if (base && module_at_base(p, base)) { UNLOCK; SetLastError(ERROR_SUCCESS); return 0; }
  if (!base && !image && !name) { UNLOCK; SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if ((flags & SLMFLAG_VIRTUAL) && (!base || !size)) { UNLOCK; SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  MODULE *m = module_load(p, base, size, image, name, (flags & SLMFLAG_VIRTUAL) != 0);
  if (m && (flags & SLMFLAG_VIRTUAL)) m->type = SymVirtual;
  const DWORD64 result = m ? m->base : 0;
  if (m && !module_link(p, m)) { UNLOCK; return 0; }
  if (m) SetLastError(ERROR_SUCCESS);
  UNLOCK;
  return result;
}
EXPORT DWORD64 WINAPI SymLoadModuleEx(HANDLE process, HANDLE file, PCSTR image, PCSTR name, DWORD64 base, DWORD size, PMODLOAD_DATA data, DWORD flags) {
  return load_module(process, image, name, base, size, flags);
}
EXPORT DWORD64 WINAPI SymLoadModuleExW(HANDLE process, HANDLE file, PCWSTR image, PCWSTR name, DWORD64 base, DWORD size, PMODLOAD_DATA data, DWORD flags) {
  char *i = to_ansi(image), *n = to_ansi(name);
  const DWORD64 r = load_module(process, i, n, base, size, flags);
  xfree(i);
  xfree(n);
  return r;
}
EXPORT DWORD64 WINAPI SymLoadModule64(HANDLE process, HANDLE file, PCSTR image, PCSTR name, DWORD64 base, DWORD size) {
  return load_module(process, image, name, base, size, 0);
}
EXPORT DWORD WINAPI SymLoadModule(HANDLE process, HANDLE file, PCSTR image, PCSTR name, DWORD base, DWORD size) {
  return (DWORD)load_module(process, image, name, base, size, 0);
}

EXPORT BOOL WINAPI SymUnloadModule64(HANDLE process, DWORD64 base) {
  LOCK;
  SYMPROC *p = find_process(process);
  if (p) {
    for (MODULE **mp = &p->modules; *mp; mp = &(*mp)->next) {
      if ((*mp)->base != base) continue;
      MODULE *m = *mp;
      *mp = m->next;
      module_free(m);
      UNLOCK;
      return TRUE;
    }
  }
  UNLOCK;
  SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI SymUnloadModule(HANDLE process, DWORD base) { return SymUnloadModule64(process, base); }

EXPORT DWORD64 WINAPI SymGetModuleBase64(HANDLE process, DWORD64 address) {
  LOCK;
  SYMPROC *p = find_process(process);
  MODULE *m = p ? module_for_address(p, address, TRUE) : NULL;
  const DWORD64 r = m ? m->base : 0;
  UNLOCK;
  if (!r) SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE);
  return r;
}
EXPORT DWORD WINAPI SymGetModuleBase(HANDLE process, DWORD address) { return (DWORD)SymGetModuleBase64(process, address); }

static DWORD count_syms(const MODULE *m) { return m->count; }

static BOOL module_info64(HANDLE process, DWORD64 address, IMAGEHLP_MODULEW64 *w, IMAGEHLP_MODULE64 *a, DWORD size_of_struct) {
  LOCK;
  SYMPROC *p = find_process(process);
  MODULE *m = p ? module_for_address(p, address, TRUE) : NULL;
  if (!m) { UNLOCK; SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE); return FALSE; }
  if (a) {
    IMAGEHLP_MODULE64 r;
    mzero(&r, sizeof(r));
    r.SizeOfStruct = size_of_struct;
    r.BaseOfImage = m->base;
    r.ImageSize = m->size;
    r.TimeDateStamp = m->timestamp;
    r.CheckSum = m->checksum;
    r.NumSyms = count_syms(m);
    r.SymType = m->type;
    scopy(r.ModuleName, sizeof(r.ModuleName), m->name);
    scopy(r.ImageName, sizeof(r.ImageName), m->image);
    scopy(r.LoadedImageName, sizeof(r.LoadedImageName), m->loaded);
    r.Publics = m->count != 0;
    r.MachineType = m->machine;
    mcopy(a, &r, size_of_struct < sizeof(r) ? size_of_struct : sizeof(r));
  } else {
    IMAGEHLP_MODULEW64 r;
    mzero(&r, sizeof(r));
    r.SizeOfStruct = size_of_struct;
    r.BaseOfImage = m->base;
    r.ImageSize = m->size;
    r.TimeDateStamp = m->timestamp;
    r.CheckSum = m->checksum;
    r.NumSyms = count_syms(m);
    r.SymType = m->type;
    widen_into(r.ModuleName, 32, m->name);
    widen_into(r.ImageName, 256, m->image);
    widen_into(r.LoadedImageName, 256, m->loaded);
    r.Publics = m->count != 0;
    r.MachineType = m->machine;
    mcopy(w, &r, size_of_struct < sizeof(r) ? size_of_struct : sizeof(r));
  }
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymGetModuleInfo64(HANDLE process, DWORD64 address, PIMAGEHLP_MODULE64 info) {
  /* the structure grew over the versions; the oldest one ends with LoadedImageName */
  if (!info || info->SizeOfStruct < offsetof(IMAGEHLP_MODULE64, LoadedPdbName)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return module_info64(process, address, NULL, info, info->SizeOfStruct);
}
EXPORT BOOL WINAPI SymGetModuleInfoW64(HANDLE process, DWORD64 address, PIMAGEHLP_MODULEW64 info) {
  if (!info || info->SizeOfStruct < offsetof(IMAGEHLP_MODULEW64, LoadedPdbName)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return module_info64(process, address, info, NULL, info->SizeOfStruct);
}
EXPORT BOOL WINAPI SymGetModuleInfo(HANDLE process, DWORD address, PIMAGEHLP_MODULE info) {
  IMAGEHLP_MODULE64 r;
  if (!info || info->SizeOfStruct < sizeof(IMAGEHLP_MODULE)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  mzero(&r, sizeof(r));
  if (!module_info64(process, address, NULL, &r, sizeof(r))) return FALSE;
  info->BaseOfImage = (DWORD)r.BaseOfImage;
  info->ImageSize = r.ImageSize;
  info->TimeDateStamp = r.TimeDateStamp;
  info->CheckSum = r.CheckSum;
  info->NumSyms = r.NumSyms;
  info->SymType = r.SymType;
  mcopy(info->ModuleName, r.ModuleName, sizeof(info->ModuleName));
  mcopy(info->ImageName, r.ImageName, sizeof(info->ImageName));
  mcopy(info->LoadedImageName, r.LoadedImageName, sizeof(info->LoadedImageName));
  return TRUE;
}
EXPORT BOOL WINAPI SymGetModuleInfoW(HANDLE process, DWORD address, PIMAGEHLP_MODULEW info) {
  IMAGEHLP_MODULEW64 r;
  if (!info || info->SizeOfStruct < sizeof(IMAGEHLP_MODULEW)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  mzero(&r, sizeof(r));
  if (!module_info64(process, address, &r, NULL, sizeof(r))) return FALSE;
  info->BaseOfImage = (DWORD)r.BaseOfImage;
  info->ImageSize = r.ImageSize;
  info->TimeDateStamp = r.TimeDateStamp;
  info->CheckSum = r.CheckSum;
  info->NumSyms = r.NumSyms;
  info->SymType = r.SymType;
  mcopy(info->ModuleName, r.ModuleName, sizeof(info->ModuleName));
  mcopy(info->ImageName, r.ImageName, sizeof(info->ImageName));
  mcopy(info->LoadedImageName, r.LoadedImageName, sizeof(info->LoadedImageName));
  return TRUE;
}

/* The enumerations copy the list first: a callback may call back into the library. */
typedef struct { DWORD64 base; char name[64]; } MODCOPY;
static MODCOPY *copy_modules(SYMPROC *p, DWORD *count) {
  DWORD n = 0;
  for (MODULE *m = p->modules; m; m = m->next) n++;
  MODCOPY *c = xalloc((SIZE_T)(n ? n : 1) * sizeof(MODCOPY));
  *count = 0;
  if (!c) return NULL;
  /* the list is newest first: report in load order */
  for (MODULE *m = p->modules; m; m = m->next) { c[n - 1 - *count].base = m->base; scopy(c[n - 1 - *count].name, 64, m->name); (*count)++; }
  return c;
}
EXPORT BOOL WINAPI SymEnumerateModules64(HANDLE process, PSYM_ENUMMODULES_CALLBACK64 callback, PVOID context) {
  LOCK;
  SYMPROC *p = find_process(process);
  DWORD n = 0;
  MODCOPY *c = p ? copy_modules(p, &n) : NULL;
  UNLOCK;
  if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  for (DWORD i = 0; i < n; i++) if (!callback(c[i].name, c[i].base, context)) break;
  xfree(c);
  return TRUE;
}
EXPORT BOOL WINAPI SymEnumerateModulesW64(HANDLE process, PSYM_ENUMMODULES_CALLBACKW64 callback, PVOID context) {
  LOCK;
  SYMPROC *p = find_process(process);
  DWORD n = 0;
  MODCOPY *c = p ? copy_modules(p, &n) : NULL;
  UNLOCK;
  if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  for (DWORD i = 0; i < n; i++) {
    WCHAR w[64];
    widen_into(w, 64, c[i].name);
    if (!callback(w, c[i].base, context)) break;
  }
  xfree(c);
  return TRUE;
}
EXPORT BOOL WINAPI SymEnumerateModules(HANDLE process, PSYM_ENUMMODULES_CALLBACK callback, PVOID context) {
  LOCK;
  SYMPROC *p = find_process(process);
  DWORD n = 0;
  MODCOPY *c = p ? copy_modules(p, &n) : NULL;
  UNLOCK;
  if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  for (DWORD i = 0; i < n; i++) if (!callback(c[i].name, (DWORD)c[i].base, context)) break;
  xfree(c);
  return TRUE;
}

/* ---- search path, home directory, callback, context ---- */
static BOOL set_search_path(HANDLE process, const char *path) {
  LOCK;
  SYMPROC *p = find_process(process);
  if (p) { char *copy = path ? sdup(path) : default_search_path(); xfree(p->search_path); p->search_path = copy; }
  UNLOCK;
  if (!p) SetLastError(ERROR_INVALID_HANDLE);
  return p != NULL;
}
EXPORT BOOL WINAPI SymSetSearchPath(HANDLE process, PCSTR path) { return set_search_path(process, path); }
EXPORT BOOL WINAPI SymSetSearchPathW(HANDLE process, PCWSTR path) {
  char *a = to_ansi(path);
  const BOOL r = set_search_path(process, a);
  xfree(a);
  return r;
}
EXPORT BOOL WINAPI SymGetSearchPath(HANDLE process, PSTR buffer, DWORD size) {
  LOCK;
  SYMPROC *p = find_process(process);
  if (p && buffer && size) scopy(buffer, size, p->search_path ? p->search_path : "");
  UNLOCK;
  if (!p) SetLastError(ERROR_INVALID_HANDLE);
  return p != NULL;
}
EXPORT BOOL WINAPI SymGetSearchPathW(HANDLE process, PWSTR buffer, DWORD size) {
  LOCK;
  SYMPROC *p = find_process(process);
  if (p && buffer && size) widen_into(buffer, size, p->search_path ? p->search_path : "");
  UNLOCK;
  if (!p) SetLastError(ERROR_INVALID_HANDLE);
  return p != NULL;
}

EXPORT PCHAR WINAPI SymSetHomeDirectory(HANDLE process, PCSTR dir) {
  LOCK;
  xfree(g_home);
  g_home = dir ? sdup(dir) : NULL;
  UNLOCK;
  return g_home;
}
EXPORT PWSTR WINAPI SymSetHomeDirectoryW(HANDLE process, PCWSTR dir) {
  static WCHAR wide[MAX_PATH];
  char *a = to_ansi(dir);
  SymSetHomeDirectory(process, a);
  xfree(a);
  widen_into(wide, MAX_PATH, g_home ? g_home : "");
  return wide;
}
EXPORT PCHAR WINAPI SymGetHomeDirectory(DWORD type, PSTR dir, size_t size) {
  if (dir && size) scopy(dir, size, g_home ? g_home : "");
  return dir;
}
EXPORT PWSTR WINAPI SymGetHomeDirectoryW(DWORD type, PWSTR dir, size_t size) {
  if (dir && size) widen_into(dir, size, g_home ? g_home : "");
  return dir;
}

EXPORT BOOL WINAPI SymRegisterCallback64(HANDLE process, PSYMBOL_REGISTERED_CALLBACK64 callback, ULONG64 context) {
  LOCK;
  SYMPROC *p = find_process(process);
  if (p) { p->callback = callback; p->callback_context = context; p->callback_wide = FALSE; }
  UNLOCK;
  if (!p) SetLastError(ERROR_INVALID_HANDLE);
  return p != NULL;
}
EXPORT BOOL WINAPI SymRegisterCallbackW64(HANDLE process, PSYMBOL_REGISTERED_CALLBACK64 callback, ULONG64 context) {
  const BOOL r = SymRegisterCallback64(process, callback, context);
  if (r) { LOCK; SYMPROC *p = find_process(process); if (p) p->callback_wide = TRUE; UNLOCK; }
  return r;
}
EXPORT BOOL WINAPI SymSetContext(HANDLE process, PIMAGEHLP_STACK_FRAME frame, PIMAGEHLP_CONTEXT context) {
  LOCK;
  SYMPROC *p = find_process(process);
  BOOL same = FALSE;
  if (p && frame) {
    same = p->frame.InstructionOffset == frame->InstructionOffset;
    p->frame = *frame;
  }
  UNLOCK;
  if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (same) SetLastError(ERROR_SUCCESS);
  return !same;
}
EXPORT BOOL WINAPI SymSetParentWindow(HWND window) { return TRUE; }

/* ---- the pattern functions ---- */
EXPORT BOOL WINAPI SymMatchStringA(PCSTR string, PCSTR expression, BOOL case_sensitive) {
  if (!string || !expression) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return match_pattern(expression, string, case_sensitive);
}
EXPORT BOOL WINAPI SymMatchString(PCSTR string, PCSTR expression, BOOL case_sensitive) { return SymMatchStringA(string, expression, case_sensitive); }
EXPORT BOOL WINAPI SymMatchStringW(PCWSTR string, PCWSTR expression, BOOL case_sensitive) {
  char *s = to_ansi(string), *e = to_ansi(expression);
  const BOOL r = s && e ? match_pattern(e, s, case_sensitive) : FALSE;
  xfree(s);
  xfree(e);
  return r;
}
EXPORT BOOL WINAPI SymMatchFileName(PCSTR name, PCSTR match, PSTR *name_start, PSTR *match_start) {
  const SIZE_T nl = slen(name), ml = slen(match);
  SIZE_T i = nl, j = ml;
  while (i > 0 && j > 0) {
    const char a = lc(name[i - 1]), b = lc(match[j - 1]);
    if (a != b && !(is_sep(a) && is_sep(b))) break;
    i--;
    j--;
  }
  if (j > 0) {
    /* the whole of the match must be there, unless the rest of the name is a path in front of a separator */
    return FALSE;
  }
  if (name_start) *name_start = (PSTR)name + i;
  if (match_start) *match_start = (PSTR)match + j;
  return TRUE;
}
EXPORT BOOL WINAPI SymMatchFileNameW(PCWSTR name, PCWSTR match, PWSTR *name_start, PWSTR *match_start) {
  const SIZE_T nl = wlen(name), ml = wlen(match);
  SIZE_T i = nl, j = ml;
  while (i > 0 && j > 0) {
    WCHAR a = name[i - 1], b = match[j - 1];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b && !((a == '\\' || a == '/') && (b == '\\' || b == '/'))) break;
    i--;
    j--;
  }
  if (j > 0) return FALSE;
  if (name_start) *name_start = (PWSTR)name + i;
  if (match_start) *match_start = (PWSTR)match + j;
  return TRUE;
}
