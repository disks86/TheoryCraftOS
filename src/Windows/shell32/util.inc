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

/* Memory, strings, paths, the registry, IMalloc and shared memory: what the other parts of shell32.dll are built from. Included by shell32.c. */

/* ---- memory ---------------------------------------------------------------------------------------------------------------------------------------------- */

static void *mem_alloc(SIZE_T bytes) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes ? bytes : 1); }
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static void *mem_realloc(void *p, SIZE_T bytes) {
  if (!p) return mem_alloc(bytes);
  return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, bytes ? bytes : 1);
}

/* ---- strings --------------------------------------------------------------------------------------------------------------------------------------------- */

static size_t wlen(const WCHAR *s) { size_t n = 0; while (s[n]) ++n; return n; }
static int wicmp(const WCHAR *a, const WCHAR *b) { return lstrcmpiW(a, b); }
static WCHAR wup(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
static int wnicmp(const WCHAR *a, const WCHAR *b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const WCHAR x = wup(a[i]), y = wup(b[i]);
    if (x != y) return x < y ? -1 : 1;
    if (!x) return 0;
  }
  return 0;
}
/* Does `s` start with `prefix` (ignoring case)? */
static BOOL w_starts(const WCHAR *s, const WCHAR *prefix) { return wnicmp(s, prefix, wlen(prefix)) == 0; }
static int wcmp(const WCHAR *a, const WCHAR *b) { return lstrcmpW(a, b); }

/* Copies into a buffer of `capacity` characters; FALSE (and an empty or cut string) if it does not fit. */
static BOOL w_copy(WCHAR *dest, size_t capacity, const WCHAR *src) {
  if (!capacity) return FALSE;
  size_t i = 0;
  for (; src[i]; ++i) {
    if (i + 1 >= capacity) { dest[i] = 0; return FALSE; }
    dest[i] = src[i];
  }
  dest[i] = 0;
  return TRUE;
}
static BOOL w_cat(WCHAR *dest, size_t capacity, const WCHAR *src) {
  const size_t n = wlen(dest);
  if (n >= capacity) return FALSE;
  return w_copy(dest + n, capacity - n, src);
}
static BOOL w_copy_n(WCHAR *dest, size_t capacity, const WCHAR *src, size_t n) {
  if (!capacity) return FALSE;
  if (n >= capacity) { dest[0] = 0; return FALSE; }
  for (size_t i = 0; i < n; ++i) dest[i] = src[i];
  dest[n] = 0;
  return TRUE;
}
static WCHAR *w_dup(const WCHAR *s) {
  if (!s) return NULL;
  const size_t n = wlen(s);
  WCHAR *r = mem_alloc((n + 1) * sizeof(WCHAR));
  if (r) for (size_t i = 0; i <= n; ++i) r[i] = s[i];
  return r;
}
static WCHAR *w_dup_n(const WCHAR *s, size_t n) {
  WCHAR *r = mem_alloc((n + 1) * sizeof(WCHAR));
  if (r) { for (size_t i = 0; i < n; ++i) r[i] = s[i]; r[n] = 0; }
  return r;
}
static const WCHAR *w_find(const WCHAR *s, WCHAR c) { for (; *s; ++s) if (*s == c) return s; return NULL; }
static const WCHAR *w_find_last(const WCHAR *s, WCHAR c) { const WCHAR *r = NULL; for (; *s; ++s) if (*s == c) r = s; return r; }
static BOOL w_is_space(WCHAR c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
/* A copy of `src` in a string the caller frees with CoTaskMemFree (SHAlloc's allocator). */
static WCHAR *w_task_dup(const WCHAR *s) {
  const size_t n = wlen(s);
  WCHAR *r = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));
  if (r) for (size_t i = 0; i <= n; ++i) r[i] = s[i];
  return r;
}

/* ANSI -> wide (the ANSI code page), a new string; NULL for NULL. */
static WCHAR *a_to_w(const char *s) {
  if (!s) return NULL;
  const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
  WCHAR *r = mem_alloc((size_t)(n > 0 ? n : 1) * sizeof(WCHAR));
  if (r && n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, r, n);
  return r;
}
/* Wide -> ANSI into a buffer; FALSE if it does not fit. */
static BOOL w_to_a(const WCHAR *s, char *out, int capacity) {
  if (capacity <= 0) return FALSE;
  const int n = WideCharToMultiByte(CP_ACP, 0, s, -1, out, capacity, NULL, NULL);
  if (n <= 0) { out[0] = 0; return FALSE; }
  return TRUE;
}
/* A wide string for an ANSI argument that may be an atom or a resource id (high word zero) is left alone by the callers; this is for real strings. */

static HRESULT hr_last_error(void) {
  const DWORD e = GetLastError();
  return e ? HRESULT_FROM_WIN32(e) : E_FAIL;
}

/* ---- paths ----------------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL is_sep(WCHAR c) { return c == '\\' || c == '/'; }
static BOOL is_drive_letter(WCHAR c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static BOOL has_drive(const WCHAR *p) { return is_drive_letter(p[0]) && p[1] == ':'; }
static BOOL is_unc(const WCHAR *p) { return is_sep(p[0]) && is_sep(p[1]) && p[2] != '?' && p[2] != '.'; }
static BOOL is_relative(const WCHAR *p) { return !has_drive(p) && !is_sep(p[0]); }
static BOOL is_fully_qualified(const WCHAR *p) { return (has_drive(p) && is_sep(p[2])) || is_unc(p) || (is_sep(p[0]) && p[1] == '?'); }

/* The part of a path after the last separator (or after "X:"). */
static const WCHAR *path_file_name(const WCHAR *p) {
  const WCHAR *r = p;
  if (has_drive(p)) r = p + 2;
  for (const WCHAR *s = r; *s; ++s) if (is_sep(*s)) r = s + 1;
  return r;
}
/* The extension with its dot; the end of the string if there is none. */
static const WCHAR *path_extension(const WCHAR *p) {
  const WCHAR *name = path_file_name(p), *dot = NULL;
  for (const WCHAR *s = name; *s; ++s) if (*s == '.') dot = s;
  return dot ? dot : p + wlen(p);
}
static void path_strip_trailing_sep(WCHAR *p) {
  size_t n = wlen(p);
  while (n > 3 && is_sep(p[n - 1])) p[--n] = 0;
  if (n == 1 && is_sep(p[0])) return;
}
/* Appends a backslash unless there is one (or nothing before it). */
static BOOL path_add_sep(WCHAR *p, size_t capacity) {
  const size_t n = wlen(p);
  if (!n || is_sep(p[n - 1])) return TRUE;
  if (n + 2 > capacity) return FALSE;
  p[n] = '\\'; p[n + 1] = 0;
  return TRUE;
}
/* directory + name into out (the directory may be empty). */
static BOOL path_join(WCHAR *out, size_t capacity, const WCHAR *dir, const WCHAR *name) {
  if (!w_copy(out, capacity, dir)) return FALSE;
  if (!path_add_sep(out, capacity)) return FALSE;
  while (is_sep(name[0])) ++name;
  return w_cat(out, capacity, name);
}
static BOOL path_attributes(const WCHAR *p, DWORD *attributes) {
  const DWORD a = GetFileAttributesW(p);
  if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
  if (attributes) *attributes = a;
  return TRUE;
}
static BOOL path_is_dir(const WCHAR *p) { DWORD a; return path_attributes(p, &a) && (a & FILE_ATTRIBUTE_DIRECTORY); }
static BOOL path_exists(const WCHAR *p) { return path_attributes(p, NULL); }
/* The length of the root of a path: "C:\" -> 3, "C:" -> 2, "\\server\share\" -> all of that, "\" -> 1, none -> 0. */
static size_t path_root_length(const WCHAR *p) {
  if (has_drive(p)) return is_sep(p[2]) ? 3 : 2;
  if (is_unc(p)) {
    size_t i = 2, seps = 0;
    while (p[i]) { if (is_sep(p[i])) { if (++seps == 2) return i + 1; } ++i; }
    return i;
  }
  return is_sep(p[0]) ? 1 : 0;
}
static BOOL path_is_root(const WCHAR *p) {
  const size_t r = path_root_length(p);
  if (!r) return FALSE;
  if (is_unc(p)) {
    /* "\\server" and "\\server\share" (with or without a closing separator) are roots */
    size_t i = 2, seps = 0;
    for (; p[i]; ++i) if (is_sep(p[i])) { ++seps; if (seps == 2 && p[i + 1]) return FALSE; }
    return TRUE;
  }
  return p[r] == 0;
}
/* A copy without a leading and trailing quote ("name" -> name). */
static void path_unquote(WCHAR *p) {
  size_t n = wlen(p);
  if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
    for (size_t i = 1; i + 1 < n; ++i) p[i - 1] = p[i];
    p[n - 2] = 0;
  }
}
/* Does the string need quotes around it as a command (a space in it)? */
static BOOL needs_quotes(const WCHAR *p) { for (; *p; ++p) if (*p == ' ') return TRUE; return FALSE; }

/* The full path of a name (relative to the current directory). */
static BOOL path_full(const WCHAR *name, WCHAR *out, size_t capacity) {
  const DWORD n = GetFullPathNameW(name, (DWORD)capacity, out, NULL);
  return n && n < capacity;
}
/* Does a file name match a pattern with * and ? (case-insensitive)? */
static BOOL pattern_match(const WCHAR *pattern, const WCHAR *name) {
  while (*pattern) {
    if (*pattern == '*') {
      while (*pattern == '*') ++pattern;
      if (!*pattern) return TRUE;
      for (; *name; ++name) if (pattern_match(pattern, name)) return TRUE;
      return pattern_match(pattern, name);
    }
    if (!*name) return FALSE;
    if (*pattern != '?' && wup(*pattern) != wup(*name)) return FALSE;
    ++pattern; ++name;
  }
  return *name == 0;
}
static BOOL has_wildcards(const WCHAR *p) { for (; *p; ++p) if (*p == '*' || *p == '?') return TRUE; return FALSE; }

/* ---- the registry ---------------------------------------------------------------------------------------------------------------------------------------- */

/* Reads a string value (REG_SZ or REG_EXPAND_SZ, expanded). `value` NULL is the default value. */
static BOOL reg_get_string(HKEY root, const WCHAR *subkey, const WCHAR *value, WCHAR *out, DWORD capacity) {
  HKEY key;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return FALSE;
  WCHAR raw[MAX_PATH_EX];
  DWORD type = 0, size = sizeof(raw) - sizeof(WCHAR);
  LONG r = RegQueryValueExW(key, value, NULL, &type, (BYTE *)raw, &size);
  RegCloseKey(key);
  if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return FALSE;
  raw[size / sizeof(WCHAR)] = 0;
  if (type == REG_EXPAND_SZ) {
    const DWORD n = ExpandEnvironmentStringsW(raw, out, capacity);
    return n && n <= capacity;
  }
  return w_copy(out, capacity, raw);
}
static BOOL reg_set_string(HKEY root, const WCHAR *subkey, const WCHAR *value, const WCHAR *data, DWORD type) {
  HKEY key;
  if (RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return FALSE;
  const LONG r = RegSetValueExW(key, value, 0, type, (const BYTE *)data, (DWORD)((wlen(data) + 1) * sizeof(WCHAR)));
  RegCloseKey(key);
  return r == ERROR_SUCCESS;
}
static BOOL reg_key_exists(HKEY root, const WCHAR *subkey) {
  HKEY key;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return FALSE;
  RegCloseKey(key);
  return TRUE;
}

/* ---- IMalloc, SHAlloc -------------------------------------------------------------------------------------------------------------------------------------- */

/* Allocations of the shell are on the process heap, the same one ole32's CoTaskMemAlloc uses. */
EXPORT void *WINAPI SHAlloc(SIZE_T bytes) { return HeapAlloc(GetProcessHeap(), 0, bytes ? bytes : 1); }
EXPORT void WINAPI SHFree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static HRESULT STDMETHODCALLTYPE malloc_query(IMalloc *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMalloc)) { *out = self; return S_OK; }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE malloc_addref(IMalloc *self) { (void)self; return 1; }
static ULONG STDMETHODCALLTYPE malloc_release(IMalloc *self) { (void)self; return 1; }
static void *STDMETHODCALLTYPE malloc_alloc(IMalloc *self, SIZE_T n) { (void)self; return SHAlloc(n); }
static void *STDMETHODCALLTYPE malloc_realloc(IMalloc *self, void *p, SIZE_T n) {
  (void)self;
  if (!p) return SHAlloc(n);
  if (!n) { SHFree(p); return NULL; }
  return HeapReAlloc(GetProcessHeap(), 0, p, n);
}
static void STDMETHODCALLTYPE malloc_free(IMalloc *self, void *p) { (void)self; SHFree(p); }
static SIZE_T STDMETHODCALLTYPE malloc_size(IMalloc *self, void *p) { (void)self; return p ? HeapSize(GetProcessHeap(), 0, p) : (SIZE_T)-1; }
static int STDMETHODCALLTYPE malloc_did_alloc(IMalloc *self, void *p) { (void)self; return p ? (HeapValidate(GetProcessHeap(), 0, p) ? 1 : 0) : -1; }
static void STDMETHODCALLTYPE malloc_minimize(IMalloc *self) { (void)self; }
static const IMallocVtbl g_malloc_vtbl = {malloc_query, malloc_addref, malloc_release, malloc_alloc, malloc_realloc, malloc_free, malloc_size, malloc_did_alloc, malloc_minimize};
static IMalloc g_malloc = {(IMallocVtbl *)&g_malloc_vtbl};

EXPORT HRESULT WINAPI SHGetMalloc(LPMALLOC *out) {
  if (!out) return E_INVALIDARG;
  *out = &g_malloc;
  return S_OK;
}

/* ---- COM object helpers ---------------------------------------------------------------------------------------------------------------------------------- */

/* Every object of this DLL counts itself so that DllCanUnloadNow can answer. */
static LONG g_objects;
static void objects_add(void) { InterlockedIncrement(&g_objects); }
static void objects_remove(void) { InterlockedDecrement(&g_objects); }

/* ---- shared memory (SHAllocShared ...) ------------------------------------------------------------------------------------------------------------------------ */

/* A block of memory passed between processes: a file mapping whose first 16 bytes say how large the data after them is; the handle is duplicated into the other process. */
typedef struct { DWORD magic, size; DWORD64 reserved; } SharedHeader;
#define SHARED_MAGIC 0x53484D31u      /* "SHM1" */

EXPORT HANDLE WINAPI SHAllocShared(const void *data, DWORD size, DWORD process_id) {
  if (size > 0x7FFFFFF0u) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size + sizeof(SharedHeader), NULL);
  if (!mapping) return NULL;
  SharedHeader *view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, size + sizeof(SharedHeader));
  if (!view) { CloseHandle(mapping); return NULL; }
  view->magic = SHARED_MAGIC;
  view->size = size;
  if (data) CopyMemory(view + 1, data, size);
  UnmapViewOfFile(view);
  if (process_id == GetCurrentProcessId()) return mapping;
  HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, process_id), result = NULL;
  if (process) {
    if (!DuplicateHandle(GetCurrentProcess(), mapping, process, &result, 0, FALSE, DUPLICATE_SAME_ACCESS)) result = NULL;
    CloseHandle(process);
  }
  CloseHandle(mapping);
  return result;
}
/* The data of a block (in this process: the handle belongs to it, or is taken from `process_id`). */
EXPORT void *WINAPI SHLockShared(HANDLE block, DWORD process_id) {
  HANDLE mine = block;
  if (!block) return NULL;
  if (process_id != GetCurrentProcessId()) {
    HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, process_id);
    if (!process) return NULL;
    const BOOL ok = DuplicateHandle(process, block, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(process);
    if (!ok) return NULL;
  }
  SharedHeader *view = MapViewOfFile(mine, FILE_MAP_WRITE, 0, 0, 0);
  if (mine != block) CloseHandle(mine);
  if (!view) return NULL;
  if (view->magic != SHARED_MAGIC) { UnmapViewOfFile(view); return NULL; }
  return view + 1;
}
EXPORT BOOL WINAPI SHUnlockShared(void *data) {
  if (!data) return FALSE;
  return UnmapViewOfFile((SharedHeader *)data - 1);
}
EXPORT BOOL WINAPI SHFreeShared(HANDLE block, DWORD process_id) {
  if (!block) return FALSE;
  if (process_id == GetCurrentProcessId()) return CloseHandle(block);
  HANDLE process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, process_id);
  if (!process) return FALSE;
  HANDLE unused = NULL;
  const BOOL ok = DuplicateHandle(process, block, GetCurrentProcess(), &unused, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
  if (ok && unused) CloseHandle(unused);
  CloseHandle(process);
  return ok;
}
