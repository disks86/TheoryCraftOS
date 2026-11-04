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

/* Part of dbghelp.c: directories and files (MakeSureDirectoryPathExists, SearchTreeForFile, EnumDirTree, FindFileInPath, FindExecutableImage, FindDebugInfoFile) and
 * the enumeration of the modules a process has loaded (EnumerateLoadedModules). */

EXPORT BOOL WINAPI MakeSureDirectoryPathExists(PCSTR path) {
  char buffer[MAX_PATH];
  if (!path || !*path || slen(path) >= MAX_PATH) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  scopy(buffer, sizeof(buffer), path);
  char *p = buffer;
  if (p[0] && p[1] == ':') p += 2;
  else if (is_sep(p[0]) && is_sep(p[1])) {      /* \\server\share\ : the first two components are not directories to make */
    p += 2;
    for (int skipped = 0; *p && skipped < 2; p++) if (is_sep(*p)) skipped++;
    p--;
  }
  if (is_sep(*p)) p++;
  /* every component that ends with a separator is a directory; the last, if it has none, is a file name */
  for (; *p; p++) {
    if (!is_sep(*p)) continue;
    const char saved = *p;
    *p = 0;
    if (!CreateDirectoryA(buffer, NULL)) {
      const DWORD error = GetLastError();
      const DWORD attributes = GetFileAttributesA(buffer);
      if (error != ERROR_ALREADY_EXISTS && !(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))) { *p = saved; return FALSE; }
    }
    *p = saved;
  }
  return TRUE;
}

/* Looks for the files matching a pattern in a directory and (depth first) below it; calls back with the full path of each. */
typedef BOOL (*TREE_VISIT)(const char *path, void *context);
static int tree_search(const char *root, const char *pattern, TREE_VISIT visit, void *context, int depth) {
  char spec[MAX_PATH + 2];
  const SIZE_T rl = slen(root);
  if (rl + 2 >= MAX_PATH || depth > 64) return 1;
  scopy(spec, sizeof(spec), root);
  SIZE_T n = rl;
  if (n && !is_sep(spec[n - 1])) spec[n++] = '\\';
  /* the files of this directory */
  {
    WIN32_FIND_DATAA fd;
    char mask[MAX_PATH + 2];
    scopy(mask, sizeof(mask), spec);
    if (n + slen(pattern) >= MAX_PATH) return 1;
    scopy(mask + n, sizeof(mask) - n, pattern);
    HANDLE h = FindFirstFileA(mask, &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char full[MAX_PATH];
        if (n + slen(fd.cFileName) >= MAX_PATH) continue;
        scopy(full, sizeof(full), spec);
        scopy(full + n, sizeof(full) - n, fd.cFileName);
        if (!visit(full, context)) { FindClose(h); return 0; }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
  }
  /* then the subdirectories */
  {
    WIN32_FIND_DATAA fd;
    char mask[MAX_PATH + 2];
    scopy(mask, sizeof(mask), spec);
    if (n + 1 >= MAX_PATH) return 1;
    mask[n] = '*';
    mask[n + 1] = 0;
    HANDLE h = FindFirstFileA(mask, &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !scmp(fd.cFileName, ".") || !scmp(fd.cFileName, "..")) continue;
        char sub[MAX_PATH];
        if (n + slen(fd.cFileName) >= MAX_PATH) continue;
        scopy(sub, sizeof(sub), spec);
        scopy(sub + n, sizeof(sub) - n, fd.cFileName);
        if (!tree_search(sub, pattern, visit, context, depth + 1)) { FindClose(h); return 0; }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
  }
  return 1;
}

typedef struct { char *out; BOOL found; PENUMDIRTREE_CALLBACK callback; void *context; } TREE_CONTEXT;
static BOOL first_match(const char *path, void *context) {
  TREE_CONTEXT *t = context;
  scopy(t->out, MAX_PATH, path);
  t->found = TRUE;
  return FALSE;
}
static BOOL each_match(const char *path, void *context) {
  TREE_CONTEXT *t = context;
  t->found = TRUE;
  scopy(t->out, MAX_PATH, path);
  return t->callback(path, t->context);
}

EXPORT BOOL WINAPI SearchTreeForFile(PCSTR root, PCSTR input, PSTR output) {
  if (!root || !input || !output) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  TREE_CONTEXT t = {output, FALSE, NULL, NULL};
  output[0] = 0;
  tree_search(root, input, first_match, &t, 0);
  return t.found;
}
EXPORT BOOL WINAPI SearchTreeForFileW(PCWSTR root, PCWSTR input, PWSTR output) {
  char *r = to_ansi(root), *i = to_ansi(input);
  char out[MAX_PATH];
  BOOL ok = FALSE;
  if (r && i) {
    ok = SearchTreeForFile(r, i, out);
    if (ok) widen_into(output, MAX_PATH, out); else if (output) output[0] = 0;
  }
  xfree(r);
  xfree(i);
  return ok;
}
EXPORT BOOL WINAPI EnumDirTree(HANDLE process, PCSTR root, PCSTR input, PSTR output, PENUMDIRTREE_CALLBACK callback, PVOID context) {
  char out[MAX_PATH], dot[] = ".";
  if (!input) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  TREE_CONTEXT t = {out, FALSE, callback, context};
  out[0] = 0;
  if (callback) tree_search(root ? root : dot, input, each_match, &t, 0);
  else tree_search(root ? root : dot, input, first_match, &t, 0);
  if (t.found && output) scopy(output, MAX_PATH, out);
  return t.found;
}
EXPORT BOOL WINAPI EnumDirTreeW(HANDLE process, PCWSTR root, PCWSTR input, PWSTR output, PENUMDIRTREE_CALLBACKW callback, PVOID context) {
  /* the wide callback gets the path widened: collect the paths first */
  char *r = to_ansi(root), *i = to_ansi(input);
  char out[MAX_PATH];
  BOOL ok = FALSE;
  if (i) {
    TREE_CONTEXT t = {out, FALSE, NULL, NULL};
    out[0] = 0;
    char dot[] = ".";
    tree_search(r ? r : dot, i, first_match, &t, 0);
    ok = t.found;
    if (ok) {
      WCHAR w[MAX_PATH];
      widen_into(w, MAX_PATH, out);
      if (callback) callback(w, context);
      if (output) widen_into(output, MAX_PATH, out);
    }
  }
  xfree(r);
  xfree(i);
  return ok;
}

/* Searches the directories of a ';'-separated list for a file, checking the time stamp and size of an image if they are given. */
static BOOL image_matches(const char *path, DWORD timestamp, DWORD size) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return FALSE;
  BYTE head[0x1000];
  DWORD got = 0;
  PEINFO pe;
  const BOOL ok = ReadFile(f, head, sizeof(head), &got, NULL) && pe_parse(head, got, &pe) && (!timestamp || pe.timestamp == timestamp) && (!size || pe.size_of_image == size);
  CloseHandle(f);
  return ok;
}
static BOOL file_exists(const char *path) {
  const DWORD a = GetFileAttributesA(path);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static BOOL find_in_list(const char *list, const char *name, DWORD timestamp, DWORD size, char *found, PFINDFILEINPATHCALLBACK callback, void *context) {
  const BOOL absolute = (name[0] && name[1] == ':') || is_sep(name[0]);
  const char *file_only = base_name(name);
  const char *p = absolute ? "" : list;
  for (int pass = 0; pass < 2; pass++) {
    while (absolute ? pass == 0 : *p) {
      char dir[MAX_PATH], full[MAX_PATH];
      SIZE_T len = 0;
      if (absolute) { dir[0] = 0; }
      else {
        while (p[len] && p[len] != ';') len++;
        mcopy(dir, p, len < MAX_PATH ? len : MAX_PATH - 1);
        dir[len < MAX_PATH ? len : MAX_PATH - 1] = 0;
        p += len + (p[len] ? 1 : 0);
        if (!dir[0]) continue;
      }
      if (absolute) scopy(full, sizeof(full), name);
      else {
        scopy(full, sizeof(full), dir);
        SIZE_T n = slen(full);
        if (n && !is_sep(full[n - 1]) && n + 1 < sizeof(full)) full[n++] = '\\';
        scopy(full + n, sizeof(full) - n, file_only);
      }
      if (file_exists(full) && ((!timestamp && !size) || image_matches(full, timestamp, size)) && (!callback || callback(full, context))) {
        scopy(found, MAX_PATH, full);
        return TRUE;
      }
      if (absolute) break;
    }
    break;
  }
  return FALSE;
}

static BOOL find_file_in_path(HANDLE process, PCSTR search_path, PCSTR name, PVOID id, DWORD two, DWORD three, DWORD flags, PSTR found, PFINDFILEINPATHCALLBACK callback, PVOID context) {
  if (!name || !found) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  DWORD timestamp = 0;
  if (flags & SSRVOPT_DWORDPTR) { if (id) timestamp = *(DWORD *)id; }
  else if (flags & SSRVOPT_DWORD) timestamp = (DWORD)(ULONG_PTR)id;
  const char *list = search_path;
  char *own = NULL;
  if (!list) {
    LOCK;
    SYMPROC *p = find_process(process);
    if (p && p->search_path) own = sdup(p->search_path);
    UNLOCK;
    list = own ? own : ".";
  }
  const BOOL r = find_in_list(list, name, timestamp, two, found, callback, context);
  xfree(own);
  if (!r) SetLastError(ERROR_FILE_NOT_FOUND);
  return r;
}
EXPORT BOOL WINAPI FindFileInPath(HANDLE process, PCSTR path, PCSTR name, PVOID id, DWORD two, DWORD three, DWORD flags, PSTR found, PFINDFILEINPATHCALLBACK callback, PVOID context) {
  return find_file_in_path(process, path, name, id, two, three, flags, found, callback, context);
}
EXPORT BOOL WINAPI SymFindFileInPath(HANDLE process, PCSTR path, PCSTR name, PVOID id, DWORD two, DWORD three, DWORD flags, PSTR found, PFINDFILEINPATHCALLBACK callback, PVOID context) {
  return find_file_in_path(process, path, name, id, two, three, flags, found, callback, context);
}
EXPORT BOOL WINAPI SymFindFileInPathW(HANDLE process, PCWSTR path, PCWSTR name, PVOID id, DWORD two, DWORD three, DWORD flags, PWSTR found, PFINDFILEINPATHCALLBACKW callback, PVOID context) {
  char *p = to_ansi(path), *n = to_ansi(name);
  char out[MAX_PATH];
  BOOL ok = FALSE;
  if (n) {
    ok = find_file_in_path(process, p, n, id, two, three, flags, out, NULL, NULL);
    if (ok) {
      WCHAR w[MAX_PATH];
      widen_into(w, MAX_PATH, out);
      if (callback && !callback(w, context)) { ok = FALSE; SetLastError(ERROR_FILE_NOT_FOUND); }
      else if (found) widen_into(found, MAX_PATH, out);
    }
  }
  xfree(p);
  xfree(n);
  return ok;
}

/* The executable that a name and a search path give: the handle of the file opened for reading, its path in the buffer. */
static HANDLE find_image(const char *name, const char *path, char *out, int kind) {
  char found[MAX_PATH];
  if (!name || !out) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  char list[2048];
  scopy(list, sizeof(list), path ? path : ".");
  if (kind == 1) {      /* a debug file is in <path>\<extension>\<name>.dbg, or <path>\<name>.dbg */
    char stem[MAX_PATH], dbg[MAX_PATH * 2];
    scopy(stem, sizeof(stem), base_name(name));
    char *dot = NULL;
    for (char *s = stem; *s; s++) if (*s == '.') dot = s;
    char ext[16];
    ext[0] = 0;
    if (dot) { scopy(ext, sizeof(ext), dot + 1); *dot = 0; }
    dbg[0] = 0;
    for (const char *s = list; *s;) {
      SIZE_T len = 0;
      while (s[len] && s[len] != ';') len++;
      char dir[MAX_PATH];
      mcopy(dir, s, len < MAX_PATH ? len : MAX_PATH - 1);
      dir[len < MAX_PATH ? len : MAX_PATH - 1] = 0;
      s += len + (s[len] ? 1 : 0);
      if (!dir[0]) continue;
      const char *shapes[2] = {ext, ""};
      for (int k = 0; k < 2; k++) {
        scopy(dbg, sizeof(dbg), dir);
        und_cat(dbg, "\\");
        if (shapes[k][0]) { und_cat(dbg, shapes[k]); und_cat(dbg, "\\"); }
        und_cat(dbg, stem);
        und_cat(dbg, ".dbg");
        if (file_exists(dbg)) {
          HANDLE h = CreateFileA(dbg, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
          if (h != INVALID_HANDLE_VALUE) { scopy(out, MAX_PATH, dbg); return h; }
        }
      }
    }
    SetLastError(ERROR_FILE_NOT_FOUND);
    return NULL;
  }
  if (!find_in_list(list, name, 0, 0, found, NULL, NULL)) { SetLastError(ERROR_FILE_NOT_FOUND); return NULL; }
  HANDLE h = CreateFileA(found, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  scopy(out, MAX_PATH, found);
  return h;
}
EXPORT HANDLE WINAPI FindExecutableImage(PCSTR name, PCSTR path, PSTR out) { return find_image(name, path, out, 0); }
EXPORT HANDLE WINAPI FindExecutableImageEx(PCSTR name, PCSTR path, PSTR out, PFIND_EXE_FILE_CALLBACK callback, PVOID context) {
  HANDLE h = find_image(name, path, out, 0);
  if (h && callback && !callback(h, out, context)) { CloseHandle(h); SetLastError(ERROR_FILE_NOT_FOUND); return NULL; }
  return h;
}
EXPORT HANDLE WINAPI FindExecutableImageExW(PCWSTR name, PCWSTR path, PWSTR out, PFIND_EXE_FILE_CALLBACKW callback, PVOID context) {
  char *n = to_ansi(name), *p = to_ansi(path);
  char found[MAX_PATH];
  HANDLE h = n ? find_image(n, p, found, 0) : NULL;
  if (h) {
    WCHAR w[MAX_PATH];
    widen_into(w, MAX_PATH, found);
    if (callback && !callback(h, w, context)) { CloseHandle(h); h = NULL; SetLastError(ERROR_FILE_NOT_FOUND); }
    else if (out) widen_into(out, MAX_PATH, found);
  }
  xfree(n);
  xfree(p);
  return h;
}
EXPORT HANDLE WINAPI FindDebugInfoFile(PCSTR name, PCSTR path, PSTR out) { return find_image(name, path, out, 1); }
EXPORT HANDLE WINAPI FindDebugInfoFileEx(PCSTR name, PCSTR path, PSTR out, PFIND_DEBUG_FILE_CALLBACK callback, PVOID context) {
  HANDLE h = find_image(name, path, out, 1);
  if (h && callback && !callback(h, out, context)) { CloseHandle(h); SetLastError(ERROR_FILE_NOT_FOUND); return NULL; }
  return h;
}
EXPORT HANDLE WINAPI FindDebugInfoFileExW(PCWSTR name, PCWSTR path, PWSTR out, PFIND_DEBUG_FILE_CALLBACKW callback, PVOID context) {
  char *n = to_ansi(name), *p = to_ansi(path);
  char found[MAX_PATH];
  HANDLE h = n ? find_image(n, p, found, 1) : NULL;
  if (h) {
    WCHAR w[MAX_PATH];
    widen_into(w, MAX_PATH, found);
    if (callback && !callback(h, w, context)) { CloseHandle(h); h = NULL; SetLastError(ERROR_FILE_NOT_FOUND); }
    else if (out) widen_into(out, MAX_PATH, found);
  }
  xfree(n);
  xfree(p);
  return h;
}

/* ---- the loaded modules of a process ---- */
typedef struct { DWORD64 base; DWORD size; char path[MAX_PATH]; } LOADED;
static LOADED *loaded_modules(HANDLE process, DWORD *count) {
  HMODULE modules[1024];
  DWORD needed = 0;
  *count = 0;
  if (!K32EnumProcessModules(process, modules, sizeof(modules), &needed)) return NULL;
  const DWORD n = needed / sizeof(HMODULE) < 1024 ? needed / sizeof(HMODULE) : 1024;
  LOADED *list = xalloc((SIZE_T)(n ? n : 1) * sizeof(LOADED));
  if (!list) return NULL;
  for (DWORD i = 0; i < n; i++) {
    MODULEINFO mi;
    WCHAR wpath[MAX_PATH];
    list[i].base = (DWORD64)(ULONG_PTR)modules[i];
    if (K32GetModuleInformation(process, modules[i], &mi, sizeof(mi))) list[i].size = mi.SizeOfImage;
    if (K32GetModuleFileNameExW(process, modules[i], wpath, MAX_PATH)) {
      char *a = to_ansi(wpath);
      if (a) { scopy(list[i].path, MAX_PATH, a); xfree(a); }
    }
  }
  *count = n;
  return list;
}
EXPORT BOOL WINAPI EnumerateLoadedModules64(HANDLE process, PENUMLOADED_MODULES_CALLBACK64 callback, PVOID context) {
  DWORD n;
  LOADED *l = loaded_modules(process, &n);
  if (!l) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  for (DWORD i = 0; i < n; i++) if (!callback(l[i].path, l[i].base, l[i].size, context)) break;
  xfree(l);
  return TRUE;
}
EXPORT BOOL WINAPI EnumerateLoadedModulesEx(HANDLE process, PENUMLOADED_MODULES_CALLBACK64 callback, PVOID context) { return EnumerateLoadedModules64(process, callback, context); }
EXPORT BOOL WINAPI EnumerateLoadedModulesW64(HANDLE process, PENUMLOADED_MODULES_CALLBACKW64 callback, PVOID context) {
  DWORD n;
  LOADED *l = loaded_modules(process, &n);
  if (!l) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  for (DWORD i = 0; i < n; i++) {
    WCHAR w[MAX_PATH];
    widen_into(w, MAX_PATH, l[i].path);
    if (!callback(w, l[i].base, l[i].size, context)) break;
  }
  xfree(l);
  return TRUE;
}
EXPORT BOOL WINAPI EnumerateLoadedModulesExW(HANDLE process, PENUMLOADED_MODULES_CALLBACKW64 callback, PVOID context) { return EnumerateLoadedModulesW64(process, callback, context); }
EXPORT BOOL WINAPI EnumerateLoadedModules(HANDLE process, PENUMLOADED_MODULES_CALLBACK callback, PVOID context) {
  DWORD n;
  LOADED *l = loaded_modules(process, &n);
  if (!l) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  for (DWORD i = 0; i < n; i++) if (!callback(l[i].path, (ULONG)l[i].base, l[i].size, context)) break;
  xfree(l);
  return TRUE;
}
