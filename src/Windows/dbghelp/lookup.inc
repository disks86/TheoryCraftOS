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

/* Part of dbghelp.c: finding and enumerating symbols (SymFromAddr, SymFromName, SymEnumSymbols, the IMAGEHLP_SYMBOL64 forms), the function table, and the calls that need
 * debug information this library does not read (lines, types, sources: they fail the way a module without any does). */

static BOOL undecorate(const char *mangled, char *out, SIZE_T size, DWORD flags);

/* The name of a symbol as the caller sees it: undecorated if SYMOPT_UNDNAME. */
static SIZE_T display_name(const MODULE *m, const SYM *s, char *out, SIZE_T size) {
  const char *raw = sym_name(m, s);
  if ((g_options & SYMOPT_UNDNAME) && raw[0] == '?' && undecorate(raw, out, size, UNDNAME_NAME_ONLY)) return slen(out);
  return scopy(out, size, raw);
}

static DWORD sym_flags(const SYM *s) {
  DWORD f = 0;
  if (s->kind == SYMKIND_EXPORT) f |= SYMFLAG_EXPORT;
  if (s->kind == SYMKIND_USER) f |= SYMFLAG_VIRTUAL;
  if (s->tag == SymTagFunction) f |= SYMFLAG_FUNCTION;
  return f;
}

/* Fills a SYMBOL_INFO (or the wide form) for a symbol; max_name is the capacity in characters. */
static void fill_symbol_info(const MODULE *m, const SYM *s, DWORD index, void *info, BOOL wide) {
  char name[SYM_NAME_MAX];
  const SIZE_T length = display_name(m, s, name, sizeof(name));
  SYMBOL_INFO *a = info;
  SYMBOL_INFOW *w = info;
  DWORD max_name = wide ? w->MaxNameLen : a->MaxNameLen;
  const DWORD size_of_struct = wide ? w->SizeOfStruct : a->SizeOfStruct;
  if (wide) {
    mzero(w, sizeof(*w) - sizeof(WCHAR));
    w->SizeOfStruct = size_of_struct;
    w->MaxNameLen = max_name;
    w->Index = index;
    w->Size = s->size;
    w->ModBase = m->base;
    w->Flags = sym_flags(s);
    w->Address = s->address;
    w->Tag = s->tag;
    if (max_name) { w->NameLen = (ULONG)widen_into(w->Name, max_name, name); if (w->NameLen >= max_name) w->NameLen = max_name - 1; }
  } else {
    mzero(a, sizeof(*a) - 1);
    a->SizeOfStruct = size_of_struct;
    a->MaxNameLen = max_name;
    a->Index = index;
    a->Size = s->size;
    a->ModBase = m->base;
    a->Flags = sym_flags(s);
    a->Address = s->address;
    a->Tag = s->tag;
    if (max_name) { a->NameLen = (ULONG)scopy(a->Name, max_name, name); if (a->NameLen >= max_name) a->NameLen = max_name - 1; }
  }
  (void)length;
}

/* The last symbol at or below an address (the first of several at the same address); returns its index or -1. */
static long find_by_address(const MODULE *m, DWORD64 address) {
  long lo = 0, hi = (long)m->count - 1, found = -1;
  while (lo <= hi) {
    const long mid = lo + (hi - lo) / 2;
    if (m->syms[mid].address <= address) { found = mid; lo = mid + 1; } else hi = mid - 1;
  }
  if (found < 0) return -1;
  while (found > 0 && m->syms[found - 1].address == m->syms[found].address) found--;
  return found;
}

static BOOL locate_address(HANDLE process, DWORD64 address, MODULE **module, long *index, SYMPROC **proc) {
  SYMPROC *p = find_process(process);
  if (!p) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  MODULE *m = module_for_address(p, address, TRUE);
  const long i = m ? find_by_address(m, address) : -1;
  if (i < 0) { SetLastError(ERR_MOD_NOT_FOUND); return FALSE; }
  *module = m;
  *index = i;
  if (proc) *proc = p;
  return TRUE;
}

static BOOL symbol_from_address(HANDLE process, DWORD64 address, DWORD64 *displacement, void *info, BOOL wide) {
  if (!info || (wide ? ((SYMBOL_INFOW *)info)->SizeOfStruct : ((SYMBOL_INFO *)info)->SizeOfStruct) != (wide ? sizeof(SYMBOL_INFOW) : sizeof(SYMBOL_INFO))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  MODULE *m;
  long i;
  LOCK;
  if (!locate_address(process, address, &m, &i, NULL)) { UNLOCK; return FALSE; }
  fill_symbol_info(m, &m->syms[i], (DWORD)i + 1, info, wide);
  if (displacement) *displacement = address - m->syms[i].address;
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymFromAddr(HANDLE process, DWORD64 address, PDWORD64 displacement, PSYMBOL_INFO info) { return symbol_from_address(process, address, displacement, info, FALSE); }
EXPORT BOOL WINAPI SymFromAddrW(HANDLE process, DWORD64 address, PDWORD64 displacement, PSYMBOL_INFOW info) { return symbol_from_address(process, address, displacement, info, TRUE); }

/* "module!name" (the module part may be empty) */
static const char *split_module(const char *name, char *module, SIZE_T capacity) {
  const char *bang = NULL;
  for (const char *p = name; *p; p++) if (*p == '!') { bang = p; break; }
  if (!bang) { module[0] = 0; return name; }
  SIZE_T n = (SIZE_T)(bang - name);
  if (n >= capacity) n = capacity - 1;
  mcopy(module, name, n);
  module[n] = 0;
  return bang + 1;
}

static BOOL symbol_from_name(HANDLE process, const char *name, void *info, BOOL wide) {
  if (!name || !info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  LOCK;
  SYMPROC *p = find_process(process);
  if (!p) { UNLOCK; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  char module[64], display[SYM_NAME_MAX];
  const char *wanted = split_module(name, module, sizeof(module));
  const MODULE *best_module = NULL;
  const SYM *best = NULL;
  for (MODULE *m = p->modules; m; m = m->next) {
    if (module[0] && sicmp(module, m->name)) continue;
    for (DWORD i = 0; i < m->count; i++) {
      display_name(m, &m->syms[i], display, sizeof(display));
      if (!scmp(display, wanted) || !scmp(sym_name(m, &m->syms[i]), wanted)) {
        if (!best || (best->kind == SYMKIND_EXPORT && m->syms[i].kind != SYMKIND_EXPORT)) { best = &m->syms[i]; best_module = m; }
      }
    }
    if (best && module[0]) break;
  }
  if (!best) { UNLOCK; SetLastError(ERR_MOD_NOT_FOUND); return FALSE; }
  fill_symbol_info(best_module, best, (DWORD)(best - best_module->syms) + 1, info, wide);
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymFromName(HANDLE process, PCSTR name, PSYMBOL_INFO info) { return symbol_from_name(process, name, info, FALSE); }
EXPORT BOOL WINAPI SymFromNameW(HANDLE process, PCWSTR name, PSYMBOL_INFOW info) {
  char *a = to_ansi(name);
  const BOOL r = symbol_from_name(process, a, info, TRUE);
  xfree(a);
  return r;
}

static BOOL symbol_from_index(HANDLE process, ULONG64 base, DWORD index, void *info, BOOL wide) {
  LOCK;
  SYMPROC *p = find_process(process);
  MODULE *m = p ? module_at_base(p, base) : NULL;
  if (!m || index == 0 || index > m->count) { UNLOCK; SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE); return FALSE; }
  fill_symbol_info(m, &m->syms[index - 1], index, info, wide);
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymFromIndex(HANDLE process, ULONG64 base, DWORD index, PSYMBOL_INFO info) { return symbol_from_index(process, base, index, info, FALSE); }
EXPORT BOOL WINAPI SymFromIndexW(HANDLE process, ULONG64 base, DWORD index, PSYMBOL_INFOW info) { return symbol_from_index(process, base, index, info, TRUE); }

/* The next or previous symbol of the one a SYMBOL_INFO describes. */
static BOOL symbol_step(HANDLE process, void *info, BOOL wide, int step) {
  LOCK;
  SYMPROC *p = find_process(process);
  const DWORD64 base = wide ? ((SYMBOL_INFOW *)info)->ModBase : ((SYMBOL_INFO *)info)->ModBase;
  const DWORD index = wide ? ((SYMBOL_INFOW *)info)->Index : ((SYMBOL_INFO *)info)->Index;
  MODULE *m = p ? module_at_base(p, base) : NULL;
  const long next = (long)index - 1 + step;
  if (!m || index == 0 || next < 0 || next >= (long)m->count) { UNLOCK; SetLastError(p ? ERROR_NO_MORE_ITEMS : ERROR_INVALID_HANDLE); return FALSE; }
  fill_symbol_info(m, &m->syms[next], (DWORD)next + 1, info, wide);
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymNext(HANDLE process, PSYMBOL_INFO info) { return symbol_step(process, info, FALSE, 1); }
EXPORT BOOL WINAPI SymPrev(HANDLE process, PSYMBOL_INFO info) { return symbol_step(process, info, FALSE, -1); }
EXPORT BOOL WINAPI SymNextW(HANDLE process, PSYMBOL_INFOW info) { return symbol_step(process, info, TRUE, 1); }
EXPORT BOOL WINAPI SymPrevW(HANDLE process, PSYMBOL_INFOW info) { return symbol_step(process, info, TRUE, -1); }

/* Enumeration. */
typedef struct { void *info; BOOL wide; } ENUMBUF;
static BOOL enumerate_symbols(HANDLE process, ULONG64 base, const char *mask, BOOL wide, void *callback, PVOID context, DWORD64 only_address, DWORD tag) {
  LOCK;
  SYMPROC *p = find_process(process);
  if (!p) { UNLOCK; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  char module_pattern[64], display[SYM_NAME_MAX];
  const char *pattern = mask ? split_module(mask, module_pattern, sizeof(module_pattern)) : "";
  if (!mask) module_pattern[0] = 0;
  if (!*pattern) pattern = "*";
  const SIZE_T capacity = wide ? sizeof(SYMBOL_INFOW) + SYM_NAME_MAX * sizeof(WCHAR) : sizeof(SYMBOL_INFO) + SYM_NAME_MAX;
  BYTE *buffer = xalloc(capacity);
  if (!buffer) { UNLOCK; SetLastError(ERROR_OUTOFMEMORY); return FALSE; }
  BOOL found_module = FALSE;
  for (MODULE *m = p->modules; m; m = m->next) {
    if (base && m->base != base) continue;
    if (module_pattern[0] && !match_pattern(module_pattern, m->name, FALSE)) continue;
    found_module = TRUE;
    for (DWORD i = 0; i < m->count; i++) {
      const SYM *s = &m->syms[i];
      if (only_address && s->address != only_address) continue;
      if (tag && s->tag != tag) continue;
      display_name(m, s, display, sizeof(display));
      if (!match_pattern(pattern, display, (g_options & SYMOPT_CASE_INSENSITIVE) == 0)) continue;
      if (wide) { ((SYMBOL_INFOW *)buffer)->SizeOfStruct = sizeof(SYMBOL_INFOW); ((SYMBOL_INFOW *)buffer)->MaxNameLen = SYM_NAME_MAX; }
      else { ((SYMBOL_INFO *)buffer)->SizeOfStruct = sizeof(SYMBOL_INFO); ((SYMBOL_INFO *)buffer)->MaxNameLen = SYM_NAME_MAX; }
      fill_symbol_info(m, s, i + 1, buffer, wide);
      const ULONG size = s->size;
      const BOOL more = wide ? ((PSYM_ENUMERATESYMBOLS_CALLBACKW)callback)((PSYMBOL_INFOW)buffer, size, context) : ((PSYM_ENUMERATESYMBOLS_CALLBACK)callback)((PSYMBOL_INFO)buffer, size, context);
      if (!more) { xfree(buffer); UNLOCK; return TRUE; }
    }
  }
  xfree(buffer);
  UNLOCK;
  if (!found_module) { SetLastError(ERR_MOD_NOT_FOUND); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI SymEnumSymbols(HANDLE process, ULONG64 base, PCSTR mask, PSYM_ENUMERATESYMBOLS_CALLBACK callback, PVOID context) {
  return enumerate_symbols(process, base, mask, FALSE, callback, context, 0, 0);
}
EXPORT BOOL WINAPI SymEnumSymbolsEx(HANDLE process, ULONG64 base, PCSTR mask, PSYM_ENUMERATESYMBOLS_CALLBACK callback, PVOID context, DWORD options) {
  return enumerate_symbols(process, base, mask, FALSE, callback, context, 0, 0);
}
EXPORT BOOL WINAPI SymEnumSym(HANDLE process, ULONG64 base, PSYM_ENUMERATESYMBOLS_CALLBACK callback, PVOID context) {
  return enumerate_symbols(process, base, NULL, FALSE, callback, context, 0, 0);
}
EXPORT BOOL WINAPI SymEnumSymbolsW(HANDLE process, ULONG64 base, PCWSTR mask, PSYM_ENUMERATESYMBOLS_CALLBACKW callback, PVOID context) {
  char *a = to_ansi(mask);
  const BOOL r = enumerate_symbols(process, base, a, TRUE, callback, context, 0, 0);
  xfree(a);
  return r;
}
EXPORT BOOL WINAPI SymEnumSymbolsExW(HANDLE process, ULONG64 base, PCWSTR mask, PSYM_ENUMERATESYMBOLS_CALLBACKW callback, PVOID context, DWORD options) {
  return SymEnumSymbolsW(process, base, mask, callback, context);
}
static BOOL enumerate_for_address(HANDLE process, DWORD64 address, BOOL wide, void *callback, PVOID context) {
  MODULE *m;
  long i;
  LOCK;
  if (!locate_address(process, address, &m, &i, NULL)) { UNLOCK; return FALSE; }
  const DWORD64 at = m->syms[i].address;
  const DWORD64 base = m->base;
  UNLOCK;
  return enumerate_symbols(process, base, NULL, wide, callback, context, at, 0);
}
EXPORT BOOL WINAPI SymEnumSymbolsForAddr(HANDLE process, DWORD64 address, PSYM_ENUMERATESYMBOLS_CALLBACK callback, PVOID context) { return enumerate_for_address(process, address, FALSE, callback, context); }
EXPORT BOOL WINAPI SymEnumSymbolsForAddrW(HANDLE process, DWORD64 address, PSYM_ENUMERATESYMBOLS_CALLBACKW callback, PVOID context) { return enumerate_for_address(process, address, TRUE, callback, context); }
EXPORT BOOL WINAPI SymSearch(HANDLE process, ULONG64 base, DWORD index, DWORD tag, PCSTR mask, DWORD64 address, PSYM_ENUMERATESYMBOLS_CALLBACK callback, PVOID context, DWORD options) {
  return enumerate_symbols(process, base, mask, FALSE, callback, context, address, tag);
}
EXPORT BOOL WINAPI SymSearchW(HANDLE process, ULONG64 base, DWORD index, DWORD tag, PCWSTR mask, DWORD64 address, PSYM_ENUMERATESYMBOLS_CALLBACKW callback, PVOID context, DWORD options) {
  char *a = to_ansi(mask);
  const BOOL r = enumerate_symbols(process, base, a, TRUE, callback, context, address, tag);
  xfree(a);
  return r;
}

/* The IMAGEHLP_SYMBOL64 forms. */
static void fill_old_symbol(const MODULE *m, const SYM *s, IMAGEHLP_SYMBOL64 *out) {
  const DWORD max = out->MaxNameLength;
  out->Address = s->address;
  out->Size = s->size;
  out->Flags = sym_flags(s);
  if (max) { char name[SYM_NAME_MAX]; display_name(m, s, name, sizeof(name)); scopy(out->Name, max, name); }
}
EXPORT BOOL WINAPI SymGetSymFromAddr64(HANDLE process, DWORD64 address, PDWORD64 displacement, PIMAGEHLP_SYMBOL64 out) {
  MODULE *m;
  long i;
  if (!out || out->SizeOfStruct < sizeof(IMAGEHLP_SYMBOL64)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  LOCK;
  if (!locate_address(process, address, &m, &i, NULL)) { UNLOCK; return FALSE; }
  fill_old_symbol(m, &m->syms[i], out);
  if (displacement) *displacement = address - m->syms[i].address;
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymGetSymFromAddr(HANDLE process, DWORD address, PDWORD displacement, PIMAGEHLP_SYMBOL out) {
  BYTE buffer[sizeof(IMAGEHLP_SYMBOL64) + 1024];
  IMAGEHLP_SYMBOL64 *s = (IMAGEHLP_SYMBOL64 *)buffer;
  DWORD64 d = 0;
  if (!out || out->SizeOfStruct < sizeof(IMAGEHLP_SYMBOL)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  mzero(buffer, sizeof(buffer));
  s->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
  s->MaxNameLength = out->MaxNameLength < 1024 ? out->MaxNameLength : 1024;
  if (!SymGetSymFromAddr64(process, address, &d, s)) return FALSE;
  out->Address = (DWORD)s->Address;
  out->Size = s->Size;
  out->Flags = s->Flags;
  if (out->MaxNameLength) scopy(out->Name, out->MaxNameLength, s->Name);
  if (displacement) *displacement = (DWORD)d;
  return TRUE;
}
EXPORT BOOL WINAPI SymGetSymFromName64(HANDLE process, PCSTR name, PIMAGEHLP_SYMBOL64 out) {
  BYTE buffer[sizeof(SYMBOL_INFO) + 1024];
  SYMBOL_INFO *si = (SYMBOL_INFO *)buffer;
  if (!out || out->SizeOfStruct < sizeof(IMAGEHLP_SYMBOL64)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  mzero(buffer, sizeof(buffer));
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = 1024;
  if (!SymFromName(process, name, si)) return FALSE;
  out->Address = si->Address;
  out->Size = si->Size;
  out->Flags = si->Flags;
  if (out->MaxNameLength) scopy(out->Name, out->MaxNameLength, si->Name);
  return TRUE;
}
EXPORT BOOL WINAPI SymGetSymFromName(HANDLE process, PCSTR name, PIMAGEHLP_SYMBOL out) {
  BYTE buffer[sizeof(IMAGEHLP_SYMBOL64) + 1024];
  IMAGEHLP_SYMBOL64 *s = (IMAGEHLP_SYMBOL64 *)buffer;
  if (!out || out->SizeOfStruct < sizeof(IMAGEHLP_SYMBOL)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  mzero(buffer, sizeof(buffer));
  s->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
  s->MaxNameLength = out->MaxNameLength < 1024 ? out->MaxNameLength : 1024;
  if (!SymGetSymFromName64(process, name, s)) return FALSE;
  out->Address = (DWORD)s->Address;
  out->Size = s->Size;
  out->Flags = s->Flags;
  if (out->MaxNameLength) scopy(out->Name, out->MaxNameLength, s->Name);
  return TRUE;
}
static BOOL old_symbol_step(HANDLE process, PIMAGEHLP_SYMBOL64 s, int step) {
  MODULE *m;
  long i;
  if (!s || s->SizeOfStruct < sizeof(IMAGEHLP_SYMBOL64)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  LOCK;
  if (!locate_address(process, s->Address, &m, &i, NULL)) { UNLOCK; return FALSE; }
  /* the symbol at the address itself (the first of several at it) */
  const long next = i + step;
  if (next < 0 || next >= (long)m->count) { UNLOCK; SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
  fill_old_symbol(m, &m->syms[next], s);
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymGetSymNext64(HANDLE process, PIMAGEHLP_SYMBOL64 s) { return old_symbol_step(process, s, 1); }
EXPORT BOOL WINAPI SymGetSymPrev64(HANDLE process, PIMAGEHLP_SYMBOL64 s) { return old_symbol_step(process, s, -1); }

EXPORT BOOL WINAPI SymEnumerateSymbols64(HANDLE process, ULONG64 base, PSYM_ENUMSYMBOLS_CALLBACK64 callback, PVOID context) {
  LOCK;
  SYMPROC *p = find_process(process);
  MODULE *m = p ? module_at_base(p, base) : NULL;
  if (!m) { UNLOCK; SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE); return FALSE; }
  char name[SYM_NAME_MAX];
  for (DWORD i = 0; i < m->count; i++) {
    display_name(m, &m->syms[i], name, sizeof(name));
    if (!callback(name, m->syms[i].address, m->syms[i].size, context)) break;
  }
  UNLOCK;
  return TRUE;
}
EXPORT BOOL WINAPI SymEnumerateSymbolsW64(HANDLE process, ULONG64 base, PSYM_ENUMSYMBOLS_CALLBACK64W callback, PVOID context) {
  LOCK;
  SYMPROC *p = find_process(process);
  MODULE *m = p ? module_at_base(p, base) : NULL;
  if (!m) { UNLOCK; SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE); return FALSE; }
  char name[SYM_NAME_MAX];
  WCHAR wide[SYM_NAME_MAX];
  for (DWORD i = 0; i < m->count; i++) {
    display_name(m, &m->syms[i], name, sizeof(name));
    widen_into(wide, SYM_NAME_MAX, name);
    if (!callback(wide, m->syms[i].address, m->syms[i].size, context)) break;
  }
  UNLOCK;
  return TRUE;
}

/* User symbols. */
static BOOL add_symbol(HANDLE process, DWORD64 base, const char *name, DWORD64 address, DWORD size) {
  LOCK;
  SYMPROC *p = find_process(process);
  MODULE *m = p ? module_at_base(p, base) : NULL;
  if (!m) { UNLOCK; SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE); return FALSE; }
  const BOOL ok = name && *name && module_add(m, address, size, SYMKIND_USER, name, slen(name), SymTagPublicSymbol);
  if (ok) module_sort(m);
  UNLOCK;
  if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
  return ok;
}
EXPORT BOOL WINAPI SymAddSymbol(HANDLE process, ULONG64 base, PCSTR name, DWORD64 address, DWORD size, DWORD flags) { return add_symbol(process, base, name, address, size); }
EXPORT BOOL WINAPI SymAddSymbolW(HANDLE process, ULONG64 base, PCWSTR name, DWORD64 address, DWORD size, DWORD flags) {
  char *a = to_ansi(name);
  const BOOL r = add_symbol(process, base, a, address, size);
  xfree(a);
  return r;
}
static BOOL delete_symbol(HANDLE process, DWORD64 base, const char *name, DWORD64 address) {
  LOCK;
  SYMPROC *p = find_process(process);
  MODULE *m = p ? module_at_base(p, base) : NULL;
  if (!m) { UNLOCK; SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE); return FALSE; }
  for (DWORD i = 0; i < m->count; i++) {
    if (m->syms[i].kind != SYMKIND_USER) continue;
    if ((name && !scmp(sym_name(m, &m->syms[i]), name)) || (!name && m->syms[i].address == address)) {
      for (DWORD j = i; j + 1 < m->count; j++) m->syms[j] = m->syms[j + 1];
      m->count--;
      UNLOCK;
      return TRUE;
    }
  }
  UNLOCK;
  SetLastError(ERROR_NOT_FOUND);
  return FALSE;
}
EXPORT BOOL WINAPI SymDeleteSymbol(HANDLE process, ULONG64 base, PCSTR name, DWORD64 address, DWORD flags) { return delete_symbol(process, base, name, address); }
EXPORT BOOL WINAPI SymDeleteSymbolW(HANDLE process, ULONG64 base, PCWSTR name, DWORD64 address, DWORD flags) {
  char *a = to_ansi(name);
  const BOOL r = delete_symbol(process, base, a, address);
  xfree(a);
  return r;
}

/* ---- the function table (the x64 unwind data) ---- */
EXPORT PVOID WINAPI SymFunctionTableAccess64(HANDLE process, DWORD64 address) {
  LOCK;
  SYMPROC *p = find_process(process);
  MODULE *m = p ? module_for_address(p, address, TRUE) : NULL;
  PVOID result = NULL;
  if (m && m->pdata && address >= m->base) {
    const DWORD64 rva64 = address - m->base;
    if (rva64 <= 0xFFFFFFFFu) {
      const DWORD rva = (DWORD)rva64;
      long lo = 0, hi = (long)m->pdata_count - 1;
      while (lo <= hi) {
        const long mid = lo + (hi - lo) / 2;
        const RUNTIME_FUNCTION *f = &m->pdata[mid];
        if (rva < f->BeginAddress) hi = mid - 1;
        else if (rva >= f->EndAddress) lo = mid + 1;
        else { result = (PVOID)f; break; }
      }
    }
  }
  UNLOCK;
  if (!result) SetLastError(p ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE);
  return result;
}
EXPORT PVOID WINAPI SymFunctionTableAccess64AccessRoutines(HANDLE process, DWORD64 address, PREAD_PROCESS_MEMORY_ROUTINE64 read, PGET_MODULE_BASE_ROUTINE64 base) {
  return SymFunctionTableAccess64(process, address);
}

/* ---- what needs debug information that is not read here ---- */
EXPORT BOOL WINAPI SymGetLineFromAddr64(HANDLE process, DWORD64 address, PDWORD displacement, PIMAGEHLP_LINE64 line) {
  SetLastError(find_process(process) ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI SymGetLineFromAddrW64(HANDLE process, DWORD64 address, PDWORD displacement, PIMAGEHLP_LINEW64 line) {
  SetLastError(find_process(process) ? ERR_MOD_NOT_FOUND : ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI SymGetTypeInfo(HANDLE process, DWORD64 base, ULONG type, IMAGEHLP_SYMBOL_TYPE_INFO info, PVOID out) {
  SetLastError(find_process(process) ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI SymEnumTypes(HANDLE process, ULONG64 base, PSYM_ENUMERATESYMBOLS_CALLBACK callback, PVOID context) {
  SetLastError(find_process(process) ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI SymEnumTypesW(HANDLE process, ULONG64 base, PSYM_ENUMERATESYMBOLS_CALLBACKW callback, PVOID context) {
  SetLastError(find_process(process) ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI SymGetTypeFromName(HANDLE process, ULONG64 base, PCSTR name, PSYMBOL_INFO info) {
  SetLastError(find_process(process) ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI SymGetSymbolFile(HANDLE process, PCSTR path, PCSTR image, DWORD type, PSTR symbol_file, SIZE_T symbol_size, PSTR dbg_file, SIZE_T dbg_size) {
  SetLastError(ERROR_FILE_NOT_FOUND);
  return FALSE;
}
