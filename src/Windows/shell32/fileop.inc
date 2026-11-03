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

/* File operations: SHFileOperation (copy, move, rename, delete, with the recycle bin), making directories, the recycle bin, unique names and the path functions of the
 * shell. There are no dialogs: where Windows would ask (overwrite? delete a read-only file?) the answer is "yes", as with FOF_NOCONFIRMATION. Included by shell32.c. */

static void change_notify(LONG event, const WCHAR *first, const WCHAR *second);

/* ---- directories -------------------------------------------------------------------------------------------------------------------------------------------- */

/* Makes a directory and the ones above it; 0 or the Win32 error. */
static int create_directory_tree(const WCHAR *path, const SECURITY_ATTRIBUTES *attributes) {
  WCHAR p[MAX_PATH_EX];
  if (!w_copy(p, ARRAY_LEN(p), path)) return ERROR_FILENAME_EXCED_RANGE;
  path_strip_trailing_sep(p);
  DWORD a;
  if (path_attributes(p, &a)) return (a & FILE_ATTRIBUTE_DIRECTORY) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
  const size_t root = path_root_length(p);
  for (size_t i = root; p[i]; ++i) {
    if (!is_sep(p[i])) continue;
    p[i] = 0;
    if (i > root && !path_is_dir(p) && !CreateDirectoryW(p, (SECURITY_ATTRIBUTES *)attributes) && GetLastError() != ERROR_ALREADY_EXISTS) {
      const int e = (int)GetLastError();
      p[i] = '\\';
      return e;
    }
    p[i] = '\\';
  }
  if (!CreateDirectoryW(p, (SECURITY_ATTRIBUTES *)attributes)) return (int)GetLastError();
  change_notify(SHCNE_MKDIR, p, NULL);
  return 0;
}
EXPORT int WINAPI SHCreateDirectoryExW(HWND owner, LPCWSTR path, const SECURITY_ATTRIBUTES *attributes) {
  (void)owner;
  if (!path || !*path) return ERROR_INVALID_PARAMETER;
  if (is_relative(path) && !has_drive(path)) return ERROR_BAD_PATHNAME;
  if (wlen(path) >= MAX_PATH - 1 && !(path[0] == '\\' && path[1] == '\\' && path[2] == '?')) return ERROR_FILENAME_EXCED_RANGE;
  return create_directory_tree(path, attributes);
}
EXPORT int WINAPI SHCreateDirectoryExWWorker(HWND owner, LPCWSTR path, const SECURITY_ATTRIBUTES *attributes) { return SHCreateDirectoryExW(owner, path, attributes); }
EXPORT int WINAPI SHCreateDirectoryExA(HWND owner, LPCSTR path, const SECURITY_ATTRIBUTES *attributes) {
  WCHAR *w = a_to_w(path);
  const int r = w ? SHCreateDirectoryExW(owner, w, attributes) : ERROR_INVALID_PARAMETER;
  mem_free(w);
  return r;
}
EXPORT int WINAPI SHCreateDirectory(HWND owner, LPCWSTR path) { return SHCreateDirectoryExW(owner, path, NULL); }

/* Makes sure that the folder of a path (or the path itself) can be written to: SHPPFW_DIRCREATE makes it. */
EXPORT HRESULT WINAPI SHPathPrepareForWriteW(HWND owner, IUnknown *modeless, LPCWSTR path, DWORD flags) {
  (void)owner; (void)modeless;
  WCHAR dir[MAX_PATH_EX];
  if (!path) return E_INVALIDARG;
  if (!w_copy(dir, ARRAY_LEN(dir), path)) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  if (flags & SHPPFW_IGNOREFILENAME) *(WCHAR *)path_file_name(dir) = 0;
  path_strip_trailing_sep(dir);
  DWORD a;
  if (path_attributes(dir, &a)) {
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    if (!(flags & SHPPFW_NOWRITECHECK) && (a & FILE_ATTRIBUTE_READONLY) && FALSE) return HRESULT_FROM_WIN32(ERROR_WRITE_PROTECT);
    return S_OK;
  }
  if (!(flags & (SHPPFW_DIRCREATE | SHPPFW_ASKDIRCREATE))) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
  const int e = create_directory_tree(dir, NULL);
  return e == 0 || e == ERROR_ALREADY_EXISTS ? S_OK : HRESULT_FROM_WIN32((DWORD)e);
}
EXPORT HRESULT WINAPI SHPathPrepareForWriteA(HWND owner, IUnknown *modeless, LPCSTR path, DWORD flags) {
  WCHAR *w = a_to_w(path);
  const HRESULT hr = w ? SHPathPrepareForWriteW(owner, modeless, w, flags) : E_INVALIDARG;
  mem_free(w);
  return hr;
}

/* ---- the recycle bin ------------------------------------------------------------------------------------------------------------------------------------------- */

/* Each volume keeps \$Recycle.Bin\<user's SID>; a deleted item is two files there: $I<id><ext> (version 2: 8 bytes version, 8 size, 8 deletion time, 4 name length, the
 * original path) and $R<id><ext>, the item itself. */
static void user_sid_string(WCHAR *out, size_t capacity) {
  w_copy(out, capacity, L"S-1-5-21-0-0-0-1000");
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
  BYTE buffer[256];
  DWORD size = sizeof(buffer);
  if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
    WCHAR *text = NULL;
    if (ConvertSidToStringSidW(((TOKEN_USER *)buffer)->User.Sid, &text) && text) { w_copy(out, capacity, text); LocalFree(text); }
  }
  CloseHandle(token);
}
static BOOL recycle_directory(const WCHAR *drive_root, WCHAR *out, size_t capacity, BOOL create) {
  WCHAR sid[80];
  user_sid_string(sid, ARRAY_LEN(sid));
  WCHAR base[MAX_PATH_EX];
  w_copy_n(base, ARRAY_LEN(base), drive_root, 3);
  if (!w_cat(base, ARRAY_LEN(base), L"$Recycle.Bin")) return FALSE;
  if (!path_join(out, capacity, base, sid)) return FALSE;
  if (path_is_dir(out)) return TRUE;
  if (!create) return FALSE;
  if (!path_is_dir(base)) {
    if (!CreateDirectoryW(base, NULL)) return FALSE;
    SetFileAttributesW(base, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
  }
  return CreateDirectoryW(out, NULL) != 0;
}
typedef struct { ULONGLONG size, count; } BinTotals;
static ULONGLONG tree_size(const WCHAR *path) {
  WIN32_FIND_DATAW fd;
  DWORD a;
  if (!path_attributes(path, &a)) return 0;
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
    HANDLE f = FindFirstFileW(path, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    FindClose(f);
    return ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
  }
  ULONGLONG total = 0;
  WCHAR pattern[MAX_PATH_EX];
  if (!path_join(pattern, ARRAY_LEN(pattern), path, L"*")) return 0;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find == INVALID_HANDLE_VALUE) return 0;
  do {
    if (!wcmp(fd.cFileName, L".") || !wcmp(fd.cFileName, L"..")) continue;
    WCHAR child[MAX_PATH_EX];
    if (!path_join(child, ARRAY_LEN(child), path, fd.cFileName)) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += tree_size(child); else total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
  } while (FindNextFileW(find, &fd));
  FindClose(find);
  return total;
}
static BOOL delete_tree(const WCHAR *path, BOOL recursive);

/* Totals of the items in one volume's bin ($I files). */
static void bin_totals(const WCHAR *bin, BinTotals *t) {
  WCHAR pattern[MAX_PATH_EX];
  WIN32_FIND_DATAW fd;
  if (!path_join(pattern, ARRAY_LEN(pattern), bin, L"$I*")) return;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    WCHAR info[MAX_PATH_EX], item[MAX_PATH_EX];
    path_join(info, ARRAY_LEN(info), bin, fd.cFileName);
    w_copy(item, ARRAY_LEN(item), info);
    WCHAR *name = (WCHAR *)path_file_name(item);
    name[1] = 'R';
    if (!path_exists(item)) continue;
    t->count += 1;
    t->size += tree_size(item);
  } while (FindNextFileW(find, &fd));
  FindClose(find);
}
static void for_each_volume(void (*fn)(const WCHAR *root, void *arg), void *arg, const WCHAR *only) {
  if (only && *only) { fn(only, arg); return; }
  WCHAR drives[4 * 27 + 1];
  const DWORD n = GetLogicalDriveStringsW(ARRAY_LEN(drives) - 1, drives);
  for (DWORD i = 0; i < n && drives[i];) {
    if (GetDriveTypeW(drives + i) == DRIVE_FIXED) fn(drives + i, arg);
    i += (DWORD)wlen(drives + i) + 1;
  }
}
static void query_one(const WCHAR *root, void *arg) {
  WCHAR bin[MAX_PATH_EX];
  if (recycle_directory(root, bin, ARRAY_LEN(bin), FALSE)) bin_totals(bin, (BinTotals *)arg);
}
EXPORT HRESULT WINAPI SHQueryRecycleBinW(LPCWSTR root, SHQUERYRBINFO *info) {
  if (!info || info->cbSize < sizeof(SHQUERYRBINFO)) return E_INVALIDARG;
  BinTotals t = {0, 0};
  for_each_volume(query_one, &t, root);
  info->i64Size = (LONGLONG)t.size;
  info->i64NumItems = (LONGLONG)t.count;
  return S_OK;
}
EXPORT HRESULT WINAPI SHQueryRecycleBinA(LPCSTR root, SHQUERYRBINFO *info) {
  WCHAR *w = a_to_w(root);
  const HRESULT hr = SHQueryRecycleBinW(w, info);
  mem_free(w);
  return hr;
}
static void empty_one(const WCHAR *root, void *arg) {
  WCHAR bin[MAX_PATH_EX], pattern[MAX_PATH_EX];
  WIN32_FIND_DATAW fd;
  (void)arg;
  if (!recycle_directory(root, bin, ARRAY_LEN(bin), FALSE) || !path_join(pattern, ARRAY_LEN(pattern), bin, L"*")) return;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcmp(fd.cFileName, L".") || !wcmp(fd.cFileName, L"..")) continue;
    WCHAR item[MAX_PATH_EX];
    if (path_join(item, ARRAY_LEN(item), bin, fd.cFileName)) delete_tree(item, TRUE);
  } while (FindNextFileW(find, &fd));
  FindClose(find);
}
EXPORT HRESULT WINAPI SHEmptyRecycleBinW(HWND owner, LPCWSTR root, DWORD flags) {
  (void)owner; (void)flags;
  for_each_volume(empty_one, NULL, root);
  change_notify(SHCNE_UPDATEDIR, NULL, NULL);
  return S_OK;
}
EXPORT HRESULT WINAPI SHEmptyRecycleBinA(HWND owner, LPCSTR root, DWORD flags) {
  WCHAR *w = a_to_w(root);
  const HRESULT hr = SHEmptyRecycleBinW(owner, w, flags);
  mem_free(w);
  return hr;
}
EXPORT HRESULT WINAPI SHUpdateRecycleBinIcon(void) { return S_OK; }

/* Moves an item into the bin of its volume; FALSE if it cannot be (then the caller deletes it for good). */
static BOOL move_to_recycle_bin(const WCHAR *full) {
  WCHAR bin[MAX_PATH_EX];
  if (!has_drive(full) || GetDriveTypeW((WCHAR[]){full[0], ':', '\\', 0}) != DRIVE_FIXED) return FALSE;
  if (!recycle_directory(full, bin, ARRAY_LEN(bin), TRUE)) return FALSE;
  static LONG counter;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const DWORD seed = (now.dwLowDateTime ^ GetTickCount()) + (DWORD)InterlockedIncrement(&counter) * 2654435761u;
  WCHAR id[8];
  for (int i = 0; i < 6; ++i) id[i] = (WCHAR)("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[((seed >> (i * 5)) ^ (seed >> 3 * i)) % 36]);
  id[6] = 0;
  const WCHAR *ext = path_extension(full);
  WCHAR item[MAX_PATH_EX], info[MAX_PATH_EX], name[40];
  w_copy(name, ARRAY_LEN(name), L"$R");
  w_cat(name, ARRAY_LEN(name), id);
  if (!path_is_dir(full) || TRUE) {
    if (wlen(ext) < 20 && !path_is_dir(full)) w_cat(name, ARRAY_LEN(name), ext);
  }
  if (!path_join(item, ARRAY_LEN(item), bin, name)) return FALSE;
  w_copy(name, ARRAY_LEN(name), L"$I");
  w_cat(name, ARRAY_LEN(name), id);
  if (wlen(ext) < 20 && !path_is_dir(full)) w_cat(name, ARRAY_LEN(name), ext);
  if (!path_join(info, ARRAY_LEN(info), bin, name)) return FALSE;
  const ULONGLONG size = tree_size(full);
  if (!MoveFileExW(full, item, MOVEFILE_COPY_ALLOWED)) return FALSE;
  HANDLE f = CreateFileW(info, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f != INVALID_HANDLE_VALUE) {
    const DWORD64 version = 2;
    const DWORD length = (DWORD)wlen(full) + 1;
    DWORD done;
    WriteFile(f, &version, 8, &done, NULL);
    WriteFile(f, &size, 8, &done, NULL);
    WriteFile(f, &now, 8, &done, NULL);
    WriteFile(f, &length, 4, &done, NULL);
    WriteFile(f, full, length * sizeof(WCHAR), &done, NULL);
    CloseHandle(f);
  }
  return TRUE;
}

/* ---- the file operation ------------------------------------------------------------------------------------------------------------------------------------- */

#define DE_SAMEFILE_ 0x71
#define DE_MANYSRC1DEST_ 0x72
#define DE_DIFFDIR_ 0x73
#define DE_DESTSUBTREE_ 0x76
#define DE_INVALIDFILES_ 0x7C
#define DE_DESTSAMETREE_ 0x7D
#define DE_FLDDESTISFILE_ 0x7E
#define DE_FILEDESTISFLD_ 0x80

typedef struct { WCHAR *path; BOOL is_dir; } Source;
typedef struct { Source *items; size_t count, capacity; } SourceList;
static void sources_free(SourceList *l) { for (size_t i = 0; i < l->count; ++i) mem_free(l->items[i].path); mem_free(l->items); l->items = NULL; l->count = l->capacity = 0; }
static BOOL sources_add(SourceList *l, const WCHAR *path, BOOL is_dir) {
  if (l->count == l->capacity) {
    const size_t n = l->capacity ? l->capacity * 2 : 8;
    Source *grown = mem_realloc(l->items, n * sizeof(Source));
    if (!grown) return FALSE;
    l->items = grown;
    l->capacity = n;
  }
  l->items[l->count].path = w_dup(path);
  l->items[l->count].is_dir = is_dir;
  if (!l->items[l->count].path) return FALSE;
  ++l->count;
  return TRUE;
}
/* The strings of a double-NUL-terminated list. */
static size_t list_count(const WCHAR *list) { size_t n = 0; while (list && *list) { ++n; list += wlen(list) + 1; } return n; }
static const WCHAR *list_at(const WCHAR *list, size_t index) { while (index--) list += wlen(list) + 1; return list; }

/* Expands the sources (wildcards, relative names) and checks that they exist. 0 or the error to return; nothing is changed on an error. */
static int expand_sources(const WCHAR *from, DWORD flags, SourceList *out) {
  int error = 0;
  for (const WCHAR *s = from; *s; s += wlen(s) + 1) {
    WCHAR full[MAX_PATH_EX];
    if (!path_full(s, full, ARRAY_LEN(full))) return DE_INVALIDFILES_;
    const WCHAR *name = path_file_name(full);
    if (has_wildcards(name)) {
      WCHAR dir[MAX_PATH_EX];
      w_copy_n(dir, ARRAY_LEN(dir), full, (size_t)(name - full));
      WIN32_FIND_DATAW fd;
      HANDLE find = FindFirstFileW(full, &fd);
      size_t found = 0;
      if (find != INVALID_HANDLE_VALUE) {
        do {
          if (!wcmp(fd.cFileName, L".") || !wcmp(fd.cFileName, L"..")) continue;
          const BOOL d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
          if (d && (flags & FOF_FILESONLY)) continue;
          WCHAR child[MAX_PATH_EX];
          if (!path_join(child, ARRAY_LEN(child), dir, fd.cFileName)) continue;
          sources_add(out, child, d);
          ++found;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
      }
      if (!found && !error) {
        error = path_is_dir(dir) ? ERROR_FILE_NOT_FOUND : DE_INVALIDFILES_;
        if (path_is_dir(dir)) error = ERROR_FILE_NOT_FOUND;
      }
      continue;
    }
    DWORD a;
    if (!path_attributes(full, &a)) {
      WCHAR dir[MAX_PATH_EX];
      w_copy_n(dir, ARRAY_LEN(dir), full, (size_t)(name - full));
      path_strip_trailing_sep(dir);
      if (!error) error = path_is_dir(dir) ? ERROR_FILE_NOT_FOUND : DE_INVALIDFILES_;
      continue;
    }
    path_strip_trailing_sep(full);
    sources_add(out, full, (a & FILE_ATTRIBUTE_DIRECTORY) != 0);
  }
  return error;
}

static void clear_readonly(const WCHAR *path) {
  DWORD a;
  if (path_attributes(path, &a) && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
}
/* Deletes a file or a directory with what is in it. */
static BOOL delete_tree(const WCHAR *path, BOOL recursive) {
  DWORD a;
  if (!path_attributes(path, &a)) return FALSE;
  if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
    clear_readonly(path);
    if (a & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(path);
    return DeleteFileW(path);
  }
  WCHAR pattern[MAX_PATH_EX];
  WIN32_FIND_DATAW fd;
  if (!path_join(pattern, ARRAY_LEN(pattern), path, L"*")) return FALSE;
  HANDLE find = FindFirstFileW(pattern, &fd);
  BOOL ok = TRUE;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (!wcmp(fd.cFileName, L".") || !wcmp(fd.cFileName, L"..")) continue;
      WCHAR child[MAX_PATH_EX];
      if (!path_join(child, ARRAY_LEN(child), path, fd.cFileName)) { ok = FALSE; continue; }
      if (!delete_tree(child, recursive)) ok = FALSE;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
  }
  clear_readonly(path);
  return RemoveDirectoryW(path) && ok;
}

static BOOL names_equal(const WCHAR *a, const WCHAR *b) {
  WCHAR x[MAX_PATH_EX], y[MAX_PATH_EX];
  if (!path_full(a, x, ARRAY_LEN(x)) || !path_full(b, y, ARRAY_LEN(y))) return FALSE;
  path_strip_trailing_sep(x);
  path_strip_trailing_sep(y);
  return !wicmp(x, y);
}
/* Is `path` the same as `tree` or inside it? */
static BOOL inside_tree(const WCHAR *path, const WCHAR *tree) {
  WCHAR x[MAX_PATH_EX], y[MAX_PATH_EX];
  if (!path_full(path, x, ARRAY_LEN(x)) || !path_full(tree, y, ARRAY_LEN(y))) return FALSE;
  path_strip_trailing_sep(y);
  const size_t n = wlen(y);
  return wnicmp(x, y, n) == 0 && (x[n] == 0 || is_sep(x[n]));
}

/* The name for FOF_RENAMEONCOLLISION: "Copy of X", then "Copy (2) of X" ... */
static BOOL collision_name(const WCHAR *dir, const WCHAR *name, WCHAR *out, size_t capacity) {
  for (int n = 1; n < 1000; ++n) {
    WCHAR candidate[MAX_PATH_EX], prefix[40];
    if (n == 1) w_copy(prefix, ARRAY_LEN(prefix), L"Copy of ");
    else {
      WCHAR digits[12];
      int d = 0, v = n;
      WCHAR tmp[12];
      while (v) { tmp[d++] = (WCHAR)('0' + v % 10); v /= 10; }
      for (int i = 0; i < d; ++i) digits[i] = tmp[d - 1 - i];
      digits[d] = 0;
      w_copy(prefix, ARRAY_LEN(prefix), L"Copy (");
      w_cat(prefix, ARRAY_LEN(prefix), digits);
      w_cat(prefix, ARRAY_LEN(prefix), L") of ");
    }
    w_copy(candidate, ARRAY_LEN(candidate), prefix);
    w_cat(candidate, ARRAY_LEN(candidate), name);
    if (!path_join(out, capacity, dir, candidate)) return FALSE;
    if (!path_exists(out)) return TRUE;
  }
  return FALSE;
}

typedef struct { DWORD flags; BOOL move; int error; SHNAMEMAPPINGW *mappings; UINT mapping_count, mapping_capacity; BOOL want_mappings; } Operation;

static void record_mapping(Operation *op, const WCHAR *from, const WCHAR *to) {
  if (!op->want_mappings) return;
  if (op->mapping_count == op->mapping_capacity) {
    const UINT n = op->mapping_capacity ? op->mapping_capacity * 2 : 4;
    SHNAMEMAPPINGW *grown = LocalAlloc(LMEM_FIXED, n * sizeof(SHNAMEMAPPINGW));
    if (!grown) return;
    if (op->mappings) { CopyMemory(grown, op->mappings, op->mapping_count * sizeof(SHNAMEMAPPINGW)); LocalFree(op->mappings); }
    op->mappings = grown;
    op->mapping_capacity = n;
  }
  SHNAMEMAPPINGW *m = &op->mappings[op->mapping_count++];
  const size_t a = wlen(from) + 1, b = wlen(to) + 1;
  m->pszOldPath = LocalAlloc(LMEM_FIXED, a * sizeof(WCHAR));
  m->pszNewPath = LocalAlloc(LMEM_FIXED, b * sizeof(WCHAR));
  if (m->pszOldPath) CopyMemory(m->pszOldPath, from, a * sizeof(WCHAR));
  if (m->pszNewPath) CopyMemory(m->pszNewPath, to, b * sizeof(WCHAR));
  m->cchOldPath = (int)(a - 1);
  m->cchNewPath = (int)(b - 1);
}

/* Copies (or moves) one item: a file onto `to` (a name, not a folder); a folder with all that is in it. 0 or an error. */
static int transfer_item(Operation *op, const WCHAR *from, const WCHAR *to, BOOL is_dir) {
  if (!is_dir) {
    clear_readonly(to);
    if (op->move) {
      if (!MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) return (int)GetLastError();
      change_notify(SHCNE_RENAMEITEM, from, to);
      return 0;
    }
    if (!CopyFileW(from, to, FALSE)) return (int)GetLastError();
    change_notify(SHCNE_CREATE, to, NULL);
    return 0;
  }
  if (!path_is_dir(to)) {
    if (op->move && MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED)) { change_notify(SHCNE_RENAMEFOLDER, from, to); return 0; }
    if (!CreateDirectoryW(to, NULL)) return (int)GetLastError();
    change_notify(SHCNE_MKDIR, to, NULL);
    DWORD a;
    if (path_attributes(from, &a)) SetFileAttributesW(to, a & ~FILE_ATTRIBUTE_REPARSE_POINT);
  }
  if (op->flags & FOF_NORECURSION) { if (op->move) RemoveDirectoryW(from); return 0; }
  WCHAR pattern[MAX_PATH_EX];
  WIN32_FIND_DATAW fd;
  if (!path_join(pattern, ARRAY_LEN(pattern), from, L"*")) return ERROR_FILENAME_EXCED_RANGE;
  HANDLE find = FindFirstFileW(pattern, &fd);
  int result = 0;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (!wcmp(fd.cFileName, L".") || !wcmp(fd.cFileName, L"..")) continue;
      WCHAR src[MAX_PATH_EX], dst[MAX_PATH_EX];
      if (!path_join(src, ARRAY_LEN(src), from, fd.cFileName) || !path_join(dst, ARRAY_LEN(dst), to, fd.cFileName)) { result = ERROR_FILENAME_EXCED_RANGE; continue; }
      const int e = transfer_item(op, src, dst, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
      if (e && !result) result = e;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
  }
  if (op->move && !result) { clear_readonly(from); RemoveDirectoryW(from); }
  return result;
}

/* A path that was given as a destination, made absolute; `trailing` tells whether it ended in a separator. */
static BOOL destination_path(const WCHAR *to, WCHAR *out, size_t capacity, BOOL *trailing) {
  size_t n = wlen(to);
  *trailing = n > 0 && is_sep(to[n - 1]);
  return path_full(to, out, capacity);
}

static int copy_or_move(Operation *op, const SourceList *sources, const WCHAR *to_list, size_t to_count) {
  if (!to_list || !*to_list) return DE_DIFFDIR_;
  if (has_wildcards(to_list) && FALSE) return ERROR_INVALID_NAME;
  const BOOL multi_dest = (op->flags & FOF_MULTIDESTFILES) != 0;
  int result = 0;
  WCHAR dest_dir[MAX_PATH_EX];
  BOOL trailing = FALSE, dest_is_dir = FALSE;
  if (!multi_dest) {
    if (!destination_path(to_list, dest_dir, ARRAY_LEN(dest_dir), &trailing)) return DE_INVALIDFILES_;
    path_strip_trailing_sep(dest_dir);
    dest_is_dir = path_is_dir(dest_dir) || trailing || sources->count > 1;
    DWORD a;
    if (sources->count == 1 && sources->items[0].is_dir && path_attributes(dest_dir, &a) && !(a & FILE_ATTRIBUTE_DIRECTORY)) return DE_FLDDESTISFILE_;
    if (dest_is_dir && path_attributes(dest_dir, &a) && !(a & FILE_ATTRIBUTE_DIRECTORY)) return DE_FLDDESTISFILE_;
    if (sources->count > 0 && !dest_is_dir) {
      /* a file onto a name: the folder it is in must exist */
      WCHAR parent[MAX_PATH_EX];
      w_copy_n(parent, ARRAY_LEN(parent), dest_dir, (size_t)(path_file_name(dest_dir) - dest_dir));
      path_strip_trailing_sep(parent);
      if (parent[0] && !path_is_dir(parent) && !has_drive(parent)) return DE_INVALIDFILES_;
    }
  }
  for (size_t i = 0; i < sources->count; ++i) {
    const Source *s = &sources->items[i];
    WCHAR target[MAX_PATH_EX];
    BOOL into_folder = FALSE;
    if (multi_dest) {
      if (i >= to_count) { result = DE_DESTSAMETREE_; break; }
      BOOL t;
      if (!destination_path(list_at(to_list, i), target, ARRAY_LEN(target), &t)) { result = DE_INVALIDFILES_; break; }
      path_strip_trailing_sep(target);
    } else if (dest_is_dir) {
      if (!path_is_dir(dest_dir)) {
        const int e = create_directory_tree(dest_dir, NULL);
        if (e && e != ERROR_ALREADY_EXISTS) { result = e; break; }
      }
      if (!path_join(target, ARRAY_LEN(target), dest_dir, path_file_name(s->path))) { result = ERROR_FILENAME_EXCED_RANGE; break; }
      into_folder = TRUE;
    } else {
      w_copy(target, ARRAY_LEN(target), dest_dir);
    }
    if (names_equal(s->path, target)) {
      if (op->flags & FOF_RENAMEONCOLLISION) {
        WCHAR dir[MAX_PATH_EX], renamed[MAX_PATH_EX];
        w_copy_n(dir, ARRAY_LEN(dir), target, (size_t)(path_file_name(target) - target));
        path_strip_trailing_sep(dir);
        if (!collision_name(dir, path_file_name(target), renamed, ARRAY_LEN(renamed))) { result = DE_SAMEFILE_; break; }
        record_mapping(op, s->path, renamed);
        w_copy(target, ARRAY_LEN(target), renamed);
      } else { result = into_folder && !op->move ? DE_DESTSAMETREE_ : DE_SAMEFILE_; if (!into_folder || op->move) result = DE_SAMEFILE_; break; }
    } else if (s->is_dir && inside_tree(target, s->path)) {
      result = DE_DESTSUBTREE_;
      break;
    } else {
      DWORD a;
      if (path_attributes(target, &a)) {
        const BOOL target_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!s->is_dir && target_dir) { result = DE_FILEDESTISFLD_; break; }
        if (s->is_dir && !target_dir) { result = DE_FLDDESTISFILE_; break; }
        if (!s->is_dir && (op->flags & FOF_RENAMEONCOLLISION)) {
          WCHAR dir[MAX_PATH_EX], renamed[MAX_PATH_EX];
          w_copy_n(dir, ARRAY_LEN(dir), target, (size_t)(path_file_name(target) - target));
          path_strip_trailing_sep(dir);
          if (collision_name(dir, path_file_name(target), renamed, ARRAY_LEN(renamed))) { record_mapping(op, s->path, renamed); w_copy(target, ARRAY_LEN(target), renamed); }
        }
      }
    }
    const int e = transfer_item(op, s->path, target, s->is_dir);
    if (e) { result = e; break; }
  }
  return result;
}

static int delete_items(Operation *op, const SourceList *sources) {
  int result = 0;
  for (size_t i = 0; i < sources->count; ++i) {
    const Source *s = &sources->items[i];
    if (!path_exists(s->path)) { if (!result) result = DE_INVALIDFILES_; continue; }
    if ((op->flags & FOF_ALLOWUNDO) && move_to_recycle_bin(s->path)) { change_notify(s->is_dir ? SHCNE_RMDIR : SHCNE_DELETE, s->path, NULL); continue; }
    if (!delete_tree(s->path, TRUE)) { if (!result) result = (int)GetLastError(); if (!result) result = ERROR_ACCESS_DENIED; continue; }
    change_notify(s->is_dir ? SHCNE_RMDIR : SHCNE_DELETE, s->path, NULL);
  }
  return result;
}

static int rename_item(Operation *op, const SourceList *sources, const WCHAR *from_list, const WCHAR *to_list) {
  (void)op;
  if (!to_list) return ERROR_ACCESS_DENIED;
  if (!*from_list || list_count(from_list) > 1) return DE_MANYSRC1DEST_;
  const Source *s = &sources->items[0];
  if (!*to_list) return DE_DIFFDIR_;
  if (has_wildcards(to_list)) return ERROR_INVALID_NAME;
  WCHAR target[MAX_PATH_EX];
  BOOL trailing;
  if (!destination_path(to_list, target, ARRAY_LEN(target), &trailing)) return DE_INVALIDFILES_;
  WCHAR from_dir[MAX_PATH_EX], to_dir[MAX_PATH_EX];
  w_copy_n(from_dir, ARRAY_LEN(from_dir), s->path, (size_t)(path_file_name(s->path) - s->path));
  w_copy_n(to_dir, ARRAY_LEN(to_dir), target, (size_t)(path_file_name(target) - target));
  path_strip_trailing_sep(from_dir);
  path_strip_trailing_sep(to_dir);
  if (wicmp(from_dir, to_dir)) return DE_DIFFDIR_;
  DWORD a;
  if (path_attributes(target, &a)) {
    const BOOL target_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!s->is_dir && target_dir) return DE_FILEDESTISFLD_;
    if (s->is_dir && !target_dir) return DE_FLDDESTISFILE_;
  }
  Operation moving = *op;
  moving.move = TRUE;
  const int e = transfer_item(&moving, s->path, target, s->is_dir);
  return e;
}

static int file_operation(UINT function, const WCHAR *from, const WCHAR *to, DWORD flags, BOOL *aborted, HANDLETOMAPPINGS **mappings_out) {
  Operation op = {0};
  op.flags = flags;
  op.want_mappings = (flags & FOF_WANTMAPPINGHANDLE) != 0;
  if (function < FO_MOVE || function > FO_RENAME) return ERROR_INVALID_PARAMETER;
  if (!from) return ERROR_INVALID_PARAMETER;
  if (function == FO_RENAME && !to) {
    /* (a missing target is access denied once the source is known to be valid) */
  }
  if (aborted) *aborted = FALSE;
  if (!*from) {
    if (function == FO_RENAME) return DE_MANYSRC1DEST_;
    if (function == FO_DELETE) return ERROR_SUCCESS;
    return ERROR_SUCCESS;
  }
  SourceList sources = {0};
  int error = expand_sources(from, flags, &sources);
  int result;
  if (function == FO_RENAME && !to) result = ERROR_ACCESS_DENIED;
  else if (function == FO_RENAME && error) result = error;
  else if (error) result = error;
  else {
    switch (function) {
      case FO_DELETE: result = delete_items(&op, &sources); break;
      case FO_COPY: op.move = FALSE; result = copy_or_move(&op, &sources, to, list_count(to)); break;
      case FO_MOVE: op.move = TRUE; result = copy_or_move(&op, &sources, to, list_count(to)); break;
      default: result = rename_item(&op, &sources, from, to); break;
    }
  }
  sources_free(&sources);
  if (mappings_out) {
    *mappings_out = NULL;
    if (op.want_mappings && op.mapping_count) {
      HANDLETOMAPPINGS *h = LocalAlloc(LMEM_FIXED, sizeof(*h));
      if (h) { h->uNumberOfMappings = op.mapping_count; h->lpSM = op.mappings; *mappings_out = h; } else mappings_out = NULL;
    }
  }
  return result;
}

EXPORT int WINAPI SHFileOperationW(LPSHFILEOPSTRUCTW op) {
  if (!op) return ERROR_INVALID_PARAMETER;
  if (!op->pFrom) { return ERROR_INVALID_PARAMETER; }
  if (op->wFunc != FO_RENAME && op->wFunc < FO_MOVE) return ERROR_INVALID_PARAMETER;
  BOOL aborted = FALSE;
  HANDLETOMAPPINGS *mappings = NULL;
  const int r = file_operation(op->wFunc, op->pFrom, op->pTo, op->fFlags, &aborted, &mappings);
  if (op->wFunc >= FO_MOVE && op->wFunc <= FO_RENAME) op->fAnyOperationsAborted = aborted;
  if ((op->fFlags & FOF_WANTMAPPINGHANDLE)) op->hNameMappings = mappings;
  return r;
}
EXPORT int WINAPI SHFileOperation(LPSHFILEOPSTRUCTW op) { return SHFileOperationW(op); }

/* A double-NUL list of ANSI strings as wide ones. */
static WCHAR *list_a_to_w(const char *list) {
  if (!list) return NULL;
  size_t bytes = 0;
  while (list[bytes] || list[bytes + 1]) ++bytes;
  bytes += 2;
  const int n = MultiByteToWideChar(CP_ACP, 0, list, (int)bytes, NULL, 0);
  WCHAR *w = mem_alloc(((size_t)n + 2) * sizeof(WCHAR));
  if (w) MultiByteToWideChar(CP_ACP, 0, list, (int)bytes, w, n);
  return w;
}
EXPORT int WINAPI SHFileOperationA(LPSHFILEOPSTRUCTA op) {
  if (!op) return ERROR_INVALID_PARAMETER;
  SHFILEOPSTRUCTW w;
  ZeroMemory(&w, sizeof(w));
  w.hwnd = op->hwnd;
  w.wFunc = op->wFunc;
  w.fFlags = op->fFlags;
  w.pFrom = list_a_to_w(op->pFrom);
  w.pTo = list_a_to_w(op->pTo);
  w.lpszProgressTitle = (op->fFlags & FOF_SIMPLEPROGRESS) ? a_to_w(op->lpszProgressTitle) : NULL;
  if (!op->pFrom) { mem_free((void *)w.pTo); return ERROR_INVALID_PARAMETER; }
  w.fAnyOperationsAborted = op->fAnyOperationsAborted;
  const int r = SHFileOperationW(&w);
  if (op->wFunc >= FO_MOVE && op->wFunc <= FO_RENAME) op->fAnyOperationsAborted = w.fAnyOperationsAborted;
  if (op->fFlags & FOF_WANTMAPPINGHANDLE) {
    /* hand the caller the wide mappings: only the counts and wide strings are meaningful */
    op->hNameMappings = w.hNameMappings;
  }
  mem_free((void *)w.pFrom);
  mem_free((void *)w.pTo);
  mem_free((void *)w.lpszProgressTitle);
  return r;
}
EXPORT void WINAPI SHFreeNameMappings(HANDLE mappings) {
  HANDLETOMAPPINGS *h = mappings;
  if (!h) return;
  for (UINT i = 0; i < h->uNumberOfMappings; ++i) {
    if (h->lpSM[i].pszOldPath) LocalFree(h->lpSM[i].pszOldPath);
    if (h->lpSM[i].pszNewPath) LocalFree(h->lpSM[i].pszNewPath);
  }
  if (h->lpSM) LocalFree(h->lpSM);
  LocalFree(h);
}
EXPORT BOOL WINAPI Win32DeleteFile(LPCWSTR path) { return DeleteFileW(path); }

/* ---- unique names ------------------------------------------------------------------------------------------------------------------------------------------- */

/* name, name (2), name (3) ...: the number goes before the extension. */
static BOOL numbered_name(const WCHAR *dir, const WCHAR *name, WCHAR *out, size_t capacity) {
  WCHAR stem[MAX_PATH_EX], candidate[MAX_PATH_EX];
  const WCHAR *ext = path_extension(name);
  w_copy_n(stem, ARRAY_LEN(stem), name, (size_t)(ext - name));
  if (!path_join(out, capacity, dir, name)) return FALSE;
  if (!path_exists(out)) return TRUE;
  for (int n = 2; n < 10000; ++n) {
    WCHAR digits[12], tmp[12];
    int d = 0, v = n;
    while (v) { tmp[d++] = (WCHAR)('0' + v % 10); v /= 10; }
    for (int i = 0; i < d; ++i) digits[i] = tmp[d - 1 - i];
    digits[d] = 0;
    w_copy(candidate, ARRAY_LEN(candidate), stem);
    w_cat(candidate, ARRAY_LEN(candidate), L" (");
    w_cat(candidate, ARRAY_LEN(candidate), digits);
    w_cat(candidate, ARRAY_LEN(candidate), L")");
    w_cat(candidate, ARRAY_LEN(candidate), ext);
    if (!path_join(out, capacity, dir, candidate)) return FALSE;
    if (!path_exists(out)) return TRUE;
  }
  return FALSE;
}
EXPORT BOOL WINAPI PathMakeUniqueName(LPWSTR unique, UINT capacity, LPCWSTR short_template, LPCWSTR long_name, LPCWSTR dir) {
  const WCHAR *name = long_name && *long_name ? long_name : short_template;
  if (!unique || !name || !dir) return FALSE;
  return numbered_name(dir, name, unique, capacity);
}
EXPORT BOOL WINAPI PathYetAnotherMakeUniqueName(LPWSTR unique, LPCWSTR path, LPCWSTR short_name, LPCWSTR long_name) {
  WCHAR dir[MAX_PATH_EX];
  if (!unique || !path) return FALSE;
  const WCHAR *name = long_name && *long_name ? long_name : short_name;
  if (!name || !*name) {
    /* the path is the first candidate */
    if (!path_exists(path)) return w_copy(unique, MAX_PATH, path);
    name = path_file_name(path);
    w_copy_n(dir, ARRAY_LEN(dir), path, (size_t)(name - path));
    path_strip_trailing_sep(dir);
    return numbered_name(dir, name, unique, MAX_PATH);
  }
  w_copy_n(dir, ARRAY_LEN(dir), path, (size_t)(path_file_name(path) - path));
  if (path_is_dir(path)) w_copy(dir, ARRAY_LEN(dir), path);
  path_strip_trailing_sep(dir);
  return numbered_name(dir, name, unique, MAX_PATH);
}

/* ---- paths --------------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT void WINAPI PathQualify(LPWSTR path) {
  WCHAR full[MAX_PATH_EX];
  if (!path) return;
  if (path_full(path, full, ARRAY_LEN(full)) && wlen(full) < MAX_PATH) w_copy(path, MAX_PATH, full);
}
EXPORT void WINAPI PathGetShortPath(LPWSTR path) {
  WCHAR out[MAX_PATH_EX];
  if (!path) return;
  const DWORD n = GetShortPathNameW(path, out, ARRAY_LEN(out));
  if (n && n < ARRAY_LEN(out)) w_copy(path, MAX_PATH, out);
}

/* Replaces the characters a file name cannot have; returns PCS_* bits. */
EXPORT int WINAPI PathCleanupSpecW(LPCWSTR dir, LPWSTR spec) {
  int result = 0;
  if (!spec) return PCS_FATAL;
  if (dir && wlen(dir) + wlen(spec) + 1 > MAX_PATH - 1) result |= PCS_PATHTOOLONG;
  size_t out = 0;
  for (size_t i = 0; spec[i]; ++i) {
    WCHAR c = spec[i];
    if (c < 32 || c == '"' ) { spec[out++] = c == '"' ? '\'' : c; if (c == '"') result |= PCS_REPLACEDCHAR; continue; }
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|') {
      if (c == '\\' || c == '/' || c == ':') {}
      result |= PCS_REPLACEDCHAR;
      spec[out++] = '-';
      continue;
    }
    spec[out++] = c;
  }
  spec[out] = 0;
  /* trailing spaces and dots are not allowed */
  while (out && (spec[out - 1] == ' ' || spec[out - 1] == '.')) { spec[--out] = 0; result |= PCS_REMOVEDCHAR; }
  return result;
}
EXPORT int WINAPI PathCleanupSpec(LPCWSTR dir, LPWSTR spec) { return PathCleanupSpecW(dir, spec); }
EXPORT int WINAPI PathCleanupSpecWorker(LPCWSTR dir, LPWSTR spec) { return PathCleanupSpecW(dir, spec); }

/* Finds a file by a name along a list of directories (a NULL-ended array) and then the PATH. */
static BOOL find_on_path(WCHAR *path, const WCHAR *const *dirs, BOOL try_extensions) {
  static const WCHAR *const kExtensions[] = {L".pif", L".com", L".exe", L".bat", L".cmd", L".lnk"};
  WCHAR candidate[MAX_PATH_EX];
  for (int pass = 0; pass < 2; ++pass) {
    const WCHAR *const *d = dirs;
    WCHAR environment_path[MAX_PATH_EX * 4];
    const WCHAR *cursor = NULL;
    if (pass == 1) {
      const DWORD n = GetEnvironmentVariableW(L"PATH", environment_path, ARRAY_LEN(environment_path));
      if (!n || n >= ARRAY_LEN(environment_path)) break;
      cursor = environment_path;
      d = NULL;
    }
    for (;;) {
      WCHAR dir[MAX_PATH_EX];
      if (pass == 0) {
        if (!d || !*d) break;
        w_copy(dir, ARRAY_LEN(dir), *d++);
      } else {
        if (!*cursor) break;
        const WCHAR *end = w_find(cursor, ';');
        const size_t n = end ? (size_t)(end - cursor) : wlen(cursor);
        w_copy_n(dir, ARRAY_LEN(dir), cursor, n);
        cursor += n;
        if (*cursor == ';') ++cursor;
        if (!dir[0]) continue;
      }
      if (!path_join(candidate, ARRAY_LEN(candidate), dir, path)) continue;
      if (path_exists(candidate) && !path_is_dir(candidate)) { w_copy(path, MAX_PATH, candidate); return TRUE; }
      if (try_extensions) {
        for (size_t e = 0; e < ARRAY_LEN(kExtensions); ++e) {
          WCHAR with[MAX_PATH_EX];
          w_copy(with, ARRAY_LEN(with), candidate);
          w_cat(with, ARRAY_LEN(with), kExtensions[e]);
          if (path_exists(with)) { w_copy(path, MAX_PATH, with); return TRUE; }
        }
      }
    }
  }
  return FALSE;
}
EXPORT int WINAPI PathResolve(PWSTR path, PZPCWSTR dirs, UINT flags) {
  WCHAR work[MAX_PATH_EX];
  if (!path) return FALSE;
  const BOOL verify = (flags & PRF_VERIFYEXISTS) != 0, try_ext = (flags & PRF_TRYPROGRAMEXTENSIONS) == PRF_TRYPROGRAMEXTENSIONS;
  w_copy(work, ARRAY_LEN(work), path);
  path_unquote(work);
  const BOOL file_spec = !w_find(work, '\\') && !w_find(work, '/') && !w_find(work, ':');
  BOOL found = FALSE;
  if (file_spec) {
    WCHAR cwd[MAX_PATH_EX];
    const WCHAR *none[] = {NULL};
    const WCHAR *const *list = dirs ? dirs : none;
    if ((flags & PRF_FIRSTDIRDEF) && dirs && dirs[0]) {
      WCHAR candidate[MAX_PATH_EX];
      path_join(candidate, ARRAY_LEN(candidate), dirs[0], work);
      w_copy(work, ARRAY_LEN(work), candidate);
      found = !verify || path_exists(work);
      if (!found && try_ext) {
        for (int i = 0; i < 6 && !found; ++i) {
          static const WCHAR *const e[] = {L".pif", L".com", L".exe", L".bat", L".cmd", L".lnk"};
          WCHAR with[MAX_PATH_EX];
          w_copy(with, ARRAY_LEN(with), work); w_cat(with, ARRAY_LEN(with), e[i]);
          if (path_exists(with)) { w_copy(work, ARRAY_LEN(work), with); found = TRUE; }
        }
      }
    } else {
      found = find_on_path(work, list, try_ext);
      if (!found) {
        if (GetCurrentDirectoryW(ARRAY_LEN(cwd), cwd)) { const WCHAR *c[] = {cwd, NULL}; found = find_on_path(work, c, try_ext); }
      }
      if (!found && !verify) { path_full(work, cwd, ARRAY_LEN(cwd)); w_copy(work, ARRAY_LEN(work), cwd); found = TRUE; }
    }
  } else {
    WCHAR full[MAX_PATH_EX];
    if (path_full(work, full, ARRAY_LEN(full))) w_copy(work, ARRAY_LEN(work), full);
    found = !verify || path_exists(work);
    if (!found && try_ext) {
      static const WCHAR *const e[] = {L".pif", L".com", L".exe", L".bat", L".cmd", L".lnk"};
      for (int i = 0; i < 6 && !found; ++i) {
        WCHAR with[MAX_PATH_EX];
        w_copy(with, ARRAY_LEN(with), work); w_cat(with, ARRAY_LEN(with), e[i]);
        if (path_exists(with)) { w_copy(work, ARRAY_LEN(work), with); found = TRUE; }
      }
    }
  }
  if (found) { if (wlen(work) >= MAX_PATH) return FALSE; w_copy(path, MAX_PATH, work); return TRUE; }
  SetLastError(ERROR_FILE_NOT_FOUND);
  return FALSE;
}

/* A command line as the shell uses it: the program (found, qualified, quoted if it has spaces) and, with PPCF_ADDARGUMENTS, what follows it. Returns the length, or -1. */
EXPORT LONG WINAPI PathProcessCommand(LPCWSTR source, LPWSTR dest, int capacity, DWORD flags) {
  if (!source || !dest || capacity <= 0) return -1;
  WCHAR program[MAX_PATH_EX], rest[MAX_PATH_EX];
  rest[0] = 0;
  const WCHAR *s = source;
  while (*s == ' ') ++s;
  if (*s == '"') {
    const WCHAR *end = w_find(s + 1, '"');
    if (!end) end = s + wlen(s);
    w_copy_n(program, ARRAY_LEN(program), s + 1, (size_t)(end - s - 1));
    s = *end ? end + 1 : end;
    w_copy(rest, ARRAY_LEN(rest), s);
  } else {
    /* the longest start that names an existing file, else up to the first space */
    size_t n = wlen(s), best = 0;
    for (size_t i = 1; i <= n; ++i) {
      if (s[i] != 0 && s[i] != ' ') continue;
      WCHAR trial[MAX_PATH_EX];
      w_copy_n(trial, ARRAY_LEN(trial), s, i);
      if (path_exists(trial) && !path_is_dir(trial)) { best = i; if (!(flags & PPCF_LONGESTPOSSIBLE)) break; }
    }
    if (!best) { const WCHAR *sp = w_find(s, ' '); best = sp ? (size_t)(sp - s) : n; }
    w_copy_n(program, ARRAY_LEN(program), s, best);
    w_copy(rest, ARRAY_LEN(rest), s + best);
  }
  if ((flags & PPCF_FORCEQUALIFY) || (!is_relative(program) == FALSE && w_find(program, '\\') == NULL)) {
    WCHAR full[MAX_PATH_EX];
    if (path_full(program, full, ARRAY_LEN(full))) w_copy(program, ARRAY_LEN(program), full);
  }
  if ((flags & PPCF_NODIRECTORIES) && path_is_dir(program)) return -1;
  WCHAR out[MAX_PATH_EX];
  out[0] = 0;
  const BOOL quote = (flags & PPCF_ADDQUOTES) && needs_quotes(program);
  if (quote) w_cat(out, ARRAY_LEN(out), L"\"");
  w_cat(out, ARRAY_LEN(out), program);
  if (quote) w_cat(out, ARRAY_LEN(out), L"\"");
  if ((flags & PPCF_ADDARGUMENTS) == PPCF_ADDARGUMENTS) w_cat(out, ARRAY_LEN(out), rest);
  if (wlen(out) >= (size_t)capacity) return -1;
  w_copy(dest, (size_t)capacity, out);
  return (LONG)wlen(dest);
}

/* The name and place for a new shortcut to `target` in `dir`: "Shortcut to X.lnk", numbered if it exists. */
EXPORT BOOL WINAPI SHGetNewLinkInfoW(LPCWSTR link_to, LPCWSTR dir, LPWSTR name, BOOL *must_copy, UINT flags) {
  if (!link_to || !dir || !name) return FALSE;
  if (must_copy) *must_copy = FALSE;
  if (!path_exists(link_to) && !(flags & SHGNLI_NOUNIQUE) && !w_find(link_to, ':')) { SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
  WCHAR base[MAX_PATH_EX], stem[MAX_PATH_EX];
  const WCHAR *file = path_file_name(link_to);
  const WCHAR *ext = path_extension(file);
  w_copy_n(stem, ARRAY_LEN(stem), file, (size_t)(ext - file));
  if (!stem[0]) w_copy(stem, ARRAY_LEN(stem), file);
  if (flags & SHGNLI_PREFIXNAME) { w_copy(base, ARRAY_LEN(base), L"Shortcut to "); w_cat(base, ARRAY_LEN(base), stem); }
  else w_copy(base, ARRAY_LEN(base), stem);
  w_cat(base, ARRAY_LEN(base), L".lnk");
  if (flags & SHGNLI_NOUNIQUE) return path_join(name, MAX_PATH, dir, base);
  return numbered_name(dir, base, name, MAX_PATH);
}
EXPORT BOOL WINAPI SHGetNewLinkInfoA(LPCSTR link_to, LPCSTR dir, LPSTR name, BOOL *must_copy, UINT flags) {
  WCHAR *a = a_to_w(link_to), *b = a_to_w(dir);
  WCHAR out[MAX_PATH];
  const BOOL r = a && b && SHGetNewLinkInfoW(a, b, out, must_copy, flags) && w_to_a(out, name, MAX_PATH);
  mem_free(a); mem_free(b);
  return r;
}
EXPORT BOOL WINAPI SHGetNewLinkInfo(LPCWSTR link_to, LPCWSTR dir, LPWSTR name, BOOL *must_copy, UINT flags) { return SHGetNewLinkInfoW(link_to, dir, name, must_copy, flags); }
