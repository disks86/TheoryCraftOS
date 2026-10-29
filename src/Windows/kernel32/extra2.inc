/* More of kernel32 (included by kernel32.c after extra.inc): names, volumes, searching, file information, copying, initialization files, resources. */

/* ---- computer name, volumes, search path ---------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI GetComputerNameExW(int type, LPWSTR name, LPDWORD size) {
  static const WCHAR upper[] = {'T', 'C', 'O', 'S', 0}, lower[] = {'t', 'c', 'o', 's', 0};
  const WCHAR *text = (type == 1 || type == 4 || type == 5 || type == 7) ? lower : upper;      /* ComputerNameDnsHostname/Domain/Fqdn/PhysicalDnsHostname are lower case here */
  const DWORD n = 4;
  if (!size || *size < n + 1 || !name) { if (size) *size = n + 1; set_error(ERROR_MORE_DATA); return FALSE; }
  if (name) memcpy(name, text, (n + 1) * 2);
  *size = n;
  return TRUE;
}
EXPORT BOOL WINAPI GetComputerNameExA(int type, LPSTR name, LPDWORD size) {
  WCHAR wide[16];
  DWORD n = 16;
  if (!GetComputerNameExW(type, wide, &n)) { if (size) *size = 5; return FALSE; }
  if (!size || *size < n + 1) { if (size) *size = n + 1; set_error(ERROR_MORE_DATA); return FALSE; }
  for (DWORD i = 0; i <= n; ++i) name[i] = (char)wide[i];
  *size = n;
  return TRUE;
}

/* A handle to the root folder of a drive, for the questions about the volume. */
static HANDLE open_volume(LPCWSTR root) {
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(root && *root ? root : (const WCHAR *)L"C:\\", buffer, &name, &attributes)) return NULL;
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtOpenFile(&h, 0x80100000u /* FILE_READ_ATTRIBUTES | SYNCHRONIZE */, &attributes, &iosb, 7, 0x20 | 0x1 /* FILE_DIRECTORY_FILE */);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return h;
}
EXPORT BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes) {
  HANDLE h = open_volume(directory);
  if (!h) return FALSE;
  IO_STATUS_BLOCK iosb;
  BYTE info[32];
  const NTSTATUS status = NtQueryVolumeInformationFile(h, &iosb, info, sizeof(info), 7);      /* FileFsFullSizeInformation */
  NtClose(h);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  const ULONGLONG unit = (ULONGLONG)get32(info + 24) * get32(info + 28);
  if (total) total->QuadPart = get64(info) * unit;
  if (available) available->QuadPart = get64(info + 8) * unit;
  if (free_bytes) free_bytes->QuadPart = get64(info + 16) * unit;
  return TRUE;
}
EXPORT BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR directory, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes) {
  WCHAR *w = directory ? to_wide(directory) : NULL;
  if (directory && !w) return FALSE;
  const BOOL ok = GetDiskFreeSpaceExW(w, available, total, free_bytes);
  free_wide(w);
  return ok;
}
EXPORT BOOL WINAPI GetDiskFreeSpaceW(LPCWSTR root, LPDWORD sectors_per_cluster, LPDWORD bytes_per_sector, LPDWORD free_clusters, LPDWORD total_clusters) {
  HANDLE h = open_volume(root);
  if (!h) return FALSE;
  IO_STATUS_BLOCK iosb;
  BYTE info[24];
  const NTSTATUS status = NtQueryVolumeInformationFile(h, &iosb, info, sizeof(info), 3);      /* FileFsSizeInformation */
  NtClose(h);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (total_clusters) *total_clusters = (DWORD)get64(info);
  if (free_clusters) *free_clusters = (DWORD)get64(info + 8);
  if (sectors_per_cluster) *sectors_per_cluster = get32(info + 16);
  if (bytes_per_sector) *bytes_per_sector = get32(info + 20);
  return TRUE;
}
EXPORT BOOL WINAPI GetDiskFreeSpaceA(LPCSTR root, LPDWORD sectors_per_cluster, LPDWORD bytes_per_sector, LPDWORD free_clusters, LPDWORD total_clusters) {
  WCHAR *w = root ? to_wide(root) : NULL;
  if (root && !w) return FALSE;
  const BOOL ok = GetDiskFreeSpaceW(w, sectors_per_cluster, bytes_per_sector, free_clusters, total_clusters);
  free_wide(w);
  return ok;
}
EXPORT BOOL WINAPI GetVolumeInformationW(LPCWSTR root, LPWSTR label, DWORD label_size, LPDWORD serial, LPDWORD max_component, LPDWORD flags, LPWSTR file_system, DWORD file_system_size) {
  HANDLE h = open_volume(root);
  if (!h) return FALSE;
  IO_STATUS_BLOCK iosb;
  BYTE volume[18 + 2 * 64], attributes[12 + 2 * 32];
  NTSTATUS status = NtQueryVolumeInformationFile(h, &iosb, volume, sizeof(volume), 1);      /* FileFsVolumeInformation */
  if (NT_SUCCESS(status)) status = NtQueryVolumeInformationFile(h, &iosb, attributes, sizeof(attributes), 5);      /* FileFsAttributeInformation */
  NtClose(h);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  const DWORD label_bytes = get32(volume + 12), name_bytes = get32(attributes + 8);
  if (label) { if (label_size * 2 < label_bytes + 2) { set_error(ERROR_MORE_DATA); return FALSE; } memcpy(label, volume + 18, label_bytes); label[label_bytes / 2] = 0; }
  if (file_system) { if (file_system_size * 2 < name_bytes + 2) { set_error(ERROR_MORE_DATA); return FALSE; } memcpy(file_system, attributes + 12, name_bytes); file_system[name_bytes / 2] = 0; }
  if (serial) *serial = get32(volume + 8);
  if (max_component) *max_component = get32(attributes + 4);
  if (flags) *flags = get32(attributes);
  return TRUE;
}
EXPORT BOOL WINAPI GetVolumeInformationA(LPCSTR root, LPSTR label, DWORD label_size, LPDWORD serial, LPDWORD max_component, LPDWORD flags, LPSTR file_system, DWORD file_system_size) {
  WCHAR *w = root ? to_wide(root) : NULL;
  if (root && !w) return FALSE;
  WCHAR wide_label[64], wide_system[32];
  const BOOL ok = GetVolumeInformationW(w, wide_label, 64, serial, max_component, flags, wide_system, 32);
  free_wide(w);
  if (!ok) return FALSE;
  if (label && label_size) { if (WideCharToMultiByte(CP_UTF8, 0, wide_label, -1, label, (int)label_size, NULL, NULL) == 0) { set_error(ERROR_MORE_DATA); return FALSE; } }
  if (file_system && file_system_size) { if (WideCharToMultiByte(CP_UTF8, 0, wide_system, -1, file_system, (int)file_system_size, NULL, NULL) == 0) { set_error(ERROR_MORE_DATA); return FALSE; } }
  return TRUE;
}
EXPORT BOOL WINAPI GetVolumePathNameW(LPCWSTR file, LPWSTR buffer, DWORD capacity) {
  WCHAR full[PATH_CHARS];
  if (!file || full_path_w(file, full, PATH_CHARS) == 0) return FALSE;
  if (capacity < 4) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  buffer[0] = full[0]; buffer[1] = ':'; buffer[2] = '\\'; buffer[3] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetVolumePathNameA(LPCSTR file, LPSTR buffer, DWORD capacity) {
  WCHAR *w = file ? to_wide(file) : NULL;
  if (!w) return FALSE;
  WCHAR out[8];
  const BOOL ok = GetVolumePathNameW(w, out, 8);
  free_wide(w);
  if (!ok || capacity < 4) return FALSE;
  buffer[0] = (char)out[0]; buffer[1] = ':'; buffer[2] = '\\'; buffer[3] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetVolumeNameForVolumeMountPointW(LPCWSTR mount_point, LPWSTR name, DWORD capacity) {
  (void)mount_point;
  static const WCHAR volume[] = {'\\', '\\', '?', '\\', 'V', 'o', 'l', 'u', 'm', 'e', '{', '0', '0', '0', '0', '0', '0', '0', '0', '-', '0', '0', '0', '0', '-', '0', '0', '0', '0', '-', '0', '0', '0', '0', '-', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '1', '}', '\\', 0};
  if (capacity < sizeof(volume) / 2) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
  memcpy(name, volume, sizeof(volume));
  return TRUE;
}

/* SearchPath: `path` is a list of folders separated by ';' (NULL: the usual places). */
EXPORT DWORD WINAPI SearchPathW(LPCWSTR path, LPCWSTR file, LPCWSTR extension, DWORD capacity, LPWSTR buffer, LPWSTR *file_part) {
  WCHAR found[PATH_CHARS];
  BOOL ok = FALSE;
  if (!file) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  static const WCHAR none[] = {0};
  if (!path) {
    ok = find_file(file, extension ? extension : none, NULL, found);
  } else {
    WCHAR candidate[PATH_CHARS];
    WCHAR with_extension[MAX_PATH];
    BOOL has_extension = FALSE;
    for (LPCWSTR c = file; *c; ++c) if (*c == '.') has_extension = TRUE; else if (*c == '\\' || *c == '/') has_extension = FALSE;
    const size_t_ fl = wcslen(file), el = extension ? wcslen(extension) : 0;
    if (fl + el + 1 > MAX_PATH) { set_error(ERROR_FILENAME_EXCED_RANGE); return 0; }
    memcpy(with_extension, file, (fl + 1) * 2);
    if (!has_extension && el) memcpy(with_extension + fl, extension, (el + 1) * 2);
    for (LPCWSTR start = path; *start && !ok;) {
      LPCWSTR end = start;
      while (*end && *end != ';') ++end;
      if (end > start && (size_t_)(end - start) < PATH_CHARS) { memcpy(candidate, start, (size_t_)(end - start) * 2); ok = try_candidate(candidate, (size_t_)(end - start), with_extension, found); }
      start = *end ? end + 1 : end;
    }
  }
  if (!ok) { set_error(ERROR_FILE_NOT_FOUND); return 0; }
  const DWORD n = (DWORD)wcslen(found);
  if (n + 1 > capacity) return n + 1;
  memcpy(buffer, found, (n + 1) * 2);
  if (file_part) { LPWSTR slash = buffer + n; while (slash > buffer && *(slash - 1) != '\\') --slash; *file_part = slash; }
  return n;
}
EXPORT DWORD WINAPI SearchPathA(LPCSTR path, LPCSTR file, LPCSTR extension, DWORD capacity, LPSTR buffer, LPSTR *file_part) {
  WCHAR *wp = path ? to_wide(path) : NULL, *wf = file ? to_wide(file) : NULL, *we = extension ? to_wide(extension) : NULL;
  DWORD result = 0;
  if (wf) {
    WCHAR found[PATH_CHARS];
    const DWORD n = SearchPathW(wp, wf, we, PATH_CHARS, found, NULL);
    if (n != 0 && n < PATH_CHARS) {
      result = wide_result_to_ansi(found, n, buffer, capacity);
      if (result < capacity && file_part) { LPSTR slash = buffer + result; while (slash > buffer && *(slash - 1) != '\\') --slash; *file_part = slash; }
    }
  }
  free_wide(wp); free_wide(wf); free_wide(we);
  return result;
}

/* ---- file information by handle -------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI GetFileInformationByHandleEx(HANDLE h, int info_class, LPVOID info, DWORD size) {
  IO_STATUS_BLOCK iosb;
  NTSTATUS status;
  switch (info_class) {
    case 0: status = NtQueryInformationFile(h, &iosb, info, size, 4); break;       /* FileBasicInfo */
    case 1: status = NtQueryInformationFile(h, &iosb, info, size, 5); break;       /* FileStandardInfo */
    case 2: status = NtQueryInformationFile(h, &iosb, info, size, 9); break;       /* FileNameInfo */
    case 9: {                                                                       /* FileAttributeTagInfo { FileAttributes, ReparseTag } */
      BYTE basic[40];
      if (size < 8) { set_error(ERROR_BAD_LENGTH); return FALSE; }
      status = NtQueryInformationFile(h, &iosb, basic, sizeof(basic), 4);
      if (NT_SUCCESS(status)) { ((DWORD *)info)[0] = get32(basic + 32); ((DWORD *)info)[1] = 0; }
      break;
    }
    default: set_error(ERROR_INVALID_PARAMETER); return FALSE;
  }
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI SetFileInformationByHandle(HANDLE h, int info_class, LPVOID info, DWORD size) {
  IO_STATUS_BLOCK iosb;
  ULONG nt_class;
  switch (info_class) {
    case 0: nt_class = 4; break;       /* FileBasicInfo */
    case 3: nt_class = 10; break;      /* FileRenameInfo (the same layout as NT's) */
    case 4: nt_class = 13; break;      /* FileDispositionInfo */
    case 5: nt_class = 19; break;      /* FileAllocationInfo */
    case 6: nt_class = 20; break;      /* FileEndOfFileInfo */
    default: set_error(ERROR_INVALID_PARAMETER); return FALSE;
  }
  const NTSTATUS status = NtSetInformationFile(h, &iosb, info, size, nt_class);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* ---- copying and moving with a progress routine ------------------------------------------------------------------------------------------------------------- */

typedef DWORD (WINAPI *LPPROGRESS_ROUTINE_)(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID);
EXPORT BOOL WINAPI CopyFileExW(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE_ progress, LPVOID data, LPBOOL cancel, DWORD flags) {
  return copy_file_core(from, to, (flags & 1 /* COPY_FILE_FAIL_IF_EXISTS */) != 0, (COPY_PROGRESS_)progress, data, cancel);
}
typedef struct { DWORD dwSize, dwCopyFlags; BOOL *pfCancel; PVOID pProgressRoutine; PVOID pvCallbackContext; } COPYFILE2_PARAMETERS_;
/* CopyFile2: the same copy, answering an HRESULT (and leaving the Win32 error behind). The progress routine of its own kind is not called. */
EXPORT LONG WINAPI CopyFile2(LPCWSTR from, LPCWSTR to, const COPYFILE2_PARAMETERS_ *parameters) {
  const BOOL fail_if_exists = parameters && (parameters->dwCopyFlags & 1);
  if (copy_file_core(from, to, fail_if_exists, NULL, NULL, parameters ? parameters->pfCancel : NULL)) return 0;
  const DWORD error = GetLastError();
  return (LONG)(error == 0 ? 0u : ((error & 0xFFFF) | 0x80070000u));
}
EXPORT BOOL WINAPI CopyFileExA(LPCSTR from, LPCSTR to, LPPROGRESS_ROUTINE_ progress, LPVOID data, LPBOOL cancel, DWORD flags) {
  if (!from || !to) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
  WCHAR *a = from ? to_wide(from) : NULL, *b = to ? to_wide(to) : NULL;
  const BOOL ok = a && b ? CopyFileExW(a, b, progress, data, cancel, flags) : FALSE;
  free_wide(a); free_wide(b);
  return ok;
}
EXPORT BOOL WINAPI MoveFileWithProgressW(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE_ progress, LPVOID data, DWORD flags) { (void)progress; (void)data; return MoveFileExW(from, to, flags); }
EXPORT BOOL WINAPI MoveFileWithProgressA(LPCSTR from, LPCSTR to, LPPROGRESS_ROUTINE_ progress, LPVOID data, DWORD flags) {
  WCHAR *a = from ? to_wide(from) : NULL, *b = to ? to_wide(to) : NULL;
  const BOOL ok = a ? MoveFileWithProgressW(a, b, progress, data, flags) : FALSE;
  free_wide(a); free_wide(b);
  return ok;
}
EXPORT BOOL WINAPI ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, LPVOID exclude, LPVOID reserved) {
  (void)flags; (void)exclude; (void)reserved;
  if (GetFileAttributesW(replacement) == INVALID_FILE_ATTRIBUTES) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
  if (GetFileAttributesW(replaced) == INVALID_FILE_ATTRIBUTES) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
  if (backup && !MoveFileExW(replaced, backup, 1 /* MOVEFILE_REPLACE_EXISTING */)) return FALSE;
  return MoveFileExW(replacement, replaced, 1);
}
EXPORT BOOL WINAPI ReplaceFileA(LPCSTR replaced, LPCSTR replacement, LPCSTR backup, DWORD flags, LPVOID exclude, LPVOID reserved) {
  WCHAR *a = replaced ? to_wide(replaced) : NULL, *b = replacement ? to_wide(replacement) : NULL, *c = backup ? to_wide(backup) : NULL;
  const BOOL ok = a && b ? ReplaceFileW(a, b, c, flags, exclude, reserved) : FALSE;
  free_wide(a); free_wide(b); free_wide(c);
  return ok;
}
EXPORT BOOL WINAPI CreateDirectoryExW(LPCWSTR template_directory, LPCWSTR path, LPSECURITY_ATTRIBUTES sa) { (void)template_directory; return CreateDirectoryW(path, sa); }
EXPORT BOOL WINAPI CreateDirectoryExA(LPCSTR template_directory, LPCSTR path, LPSECURITY_ATTRIBUTES sa) { (void)template_directory; return CreateDirectoryA(path, sa); }

/* ---- initialization (.ini) files --------------------------------------------------------------------------------------------------------------------------- */

/* The file is read whole, changed in memory as text (the code page is UTF-8) and written back. */
static char *to_ansi_alloc(LPCWSTR text) {      /* UTF-8, from the process heap */
  const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
  char *out = n > 0 ? RtlAllocateHeap(heap(), 0, (SIZE_T)n + 1) : NULL;
  if (out) WideCharToMultiByte(CP_UTF8, 0, text, -1, out, n, NULL, NULL);
  return out;
}
/* The win.ini of the system */
static const WCHAR g_win_ini[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
typedef struct { char *text; size_t_ size; } IniText;
/* A file name without a folder is in the Windows folder; only a name with a folder (even ".\\x.ini") is looked for where it says. */
static LPCWSTR ini_path(LPCWSTR name, WCHAR *buffer, size_t_ capacity) {
  for (LPCWSTR c = name; *c; ++c) if (*c == '\\' || *c == '/' || *c == ':') return name;
  static const WCHAR kFolder[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 0};
  const size_t_ folder = wcslen(kFolder), n = wcslen(name);
  if (folder + n + 1 > capacity) return name;
  memcpy(buffer, kFolder, folder * 2);
  memcpy(buffer + folder, name, (n + 1) * 2);
  return buffer;
}
static BOOL ini_read(LPCWSTR given, IniText *ini) {
  WCHAR resolved[PATH_CHARS];
  const LPCWSTR path = ini_path(given, resolved, PATH_CHARS);
  ini->text = NULL;
  ini->size = 0;
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return FALSE;
  const DWORD size = GetFileSize(h, NULL);
  ini->text = RtlAllocateHeap(heap(), 0, (SIZE_T)size + 2);
  DWORD got = 0;
  if (!ini->text || (size && !ReadFile(h, ini->text, size, &got, NULL))) { CloseHandle(h); if (ini->text) RtlFreeHeap(heap(), 0, ini->text); ini->text = NULL; return FALSE; }
  CloseHandle(h);
  size_t_ start = 0;
  if (got >= 3 && (BYTE)ini->text[0] == 0xEF && (BYTE)ini->text[1] == 0xBB && (BYTE)ini->text[2] == 0xBF) start = 3;      /* a byte order mark */
  if (start) { memmove(ini->text, ini->text + start, got - start); got -= (DWORD)start; }
  ini->text[got] = 0;
  ini->size = got;
  return TRUE;
}
static void ini_free(IniText *ini) { if (ini->text) RtlFreeHeap(heap(), 0, ini->text); ini->text = NULL; }
static int ini_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static char ini_no_section[1];      /* the section of lines before the first header: no section, not the one called "" */
static BOOL ini_equal(const char *a, size_t_ alen, const char *b, size_t_ blen) {
  if (a == ini_no_section || b == ini_no_section) return FALSE;
  while (alen && (*a == ' ' || *a == '\t')) { ++a; --alen; }
  while (alen && (a[alen - 1] == ' ' || a[alen - 1] == '\t')) --alen;
  while (blen && (*b == ' ' || *b == '\t')) { ++b; --blen; }
  while (blen && (b[blen - 1] == ' ' || b[blen - 1] == '\t')) --blen;
  if (alen != blen) return FALSE;
  for (size_t_ i = 0; i < alen; ++i) if (ini_lower((BYTE)a[i]) != ini_lower((BYTE)b[i])) return FALSE;
  return TRUE;
}
/* Walks the lines; for each, calls back with the line and whether it is a section header ("[name]") or a "key=value". `section` is the current section's name. */
typedef struct { const char *line; size_t_ length; const char *section; size_t_ section_length; const char *key; size_t_ key_length; const char *value; size_t_ value_length; BOOL header, entry; size_t_ offset; } IniLine;
static BOOL ini_next(const IniText *ini, size_t_ *at, IniLine *l, const char **section, size_t_ *section_length) {
  if (*at >= ini->size) return FALSE;
  const char *start = ini->text + *at, *end = start;
  while ((size_t_)(end - ini->text) < ini->size && *end != '\n' && *end != '\r') ++end;      /* (a NUL inside the file is an ordinary byte; CR, LF and CR LF end a line) */
  l->offset = *at;
  size_t_ after = (size_t_)(end - ini->text);
  if (after < ini->size) { after += (*end == '\r' && after + 1 < ini->size && ini->text[after + 1] == '\n') ? 2 : 1; }
  *at = after;
  size_t_ length = (size_t_)(end - start);
  if (length && start[length - 1] == '\r') --length;
  l->line = start;
  l->length = length;
  l->header = l->entry = FALSE;
  const char *p = start;
  size_t_ n = length;
  while (n && (*p == ' ' || *p == '\t')) { ++p; --n; }
  if (n && *p == '[') {
    const char *close = p + 1;
    while ((size_t_)(close - p) < n && *close != ']') ++close;
    if ((size_t_)(close - p) < n) { l->header = TRUE; *section = p + 1; *section_length = (size_t_)(close - p - 1); }
  } else if (n && *p != ';') {
    const char *eq = p;
    while ((size_t_)(eq - p) < n && *eq != '=') ++eq;
    if ((size_t_)(eq - p) < n) {
      l->entry = TRUE;
      l->key = p; l->key_length = (size_t_)(eq - p);
      l->value = eq + 1; l->value_length = n - (size_t_)(eq - p) - 1;
    }
  }
  l->section = *section;
  l->section_length = *section_length;
  return TRUE;
}
/* The text a value has: spaces around it go, and a pair of quotes. */
static void ini_value_range(const char **v, size_t_ *n) {
  while (*n && (**v == ' ' || **v == '\t')) { ++*v; --*n; }
  while (*n && ((*v)[*n - 1] == ' ' || (*v)[*n - 1] == '\t')) --*n;
  if (*n >= 2 && (((*v)[0] == '"' && (*v)[*n - 1] == '"') || ((*v)[0] == '\'' && (*v)[*n - 1] == '\''))) { ++*v; *n -= 2; }
}
static DWORD ini_copy_out(const char *bytes, size_t_ n, LPSTR out, DWORD capacity) {
  if (capacity == 0) return 0;
  const DWORD room = capacity - 1;
  const DWORD copy = n > room ? room : (DWORD)n;
  memcpy(out, bytes, copy);
  out[copy] = 0;
  return copy;
}
/* The list of section names or of the keys of a section, each followed by a NUL, ended by an empty string. */
static DWORD ini_list(const IniText *ini, const char *section, size_t_ section_length, BOOL names, LPSTR out, DWORD capacity) {
  DWORD used = 0;
  BOOL truncated = FALSE;
  size_t_ at = 0, s_len = 0;
  const char *current = ini_no_section;
  IniLine l;
  if (capacity < 2) { if (capacity) out[0] = 0; return 0; }
  while (ini_next(ini, &at, &l, &current, &s_len)) {
    const char *piece = NULL;
    size_t_ piece_length = 0;
    if (names && l.header) { piece = current; piece_length = s_len; }
    else if (!names && l.entry && ini_equal(l.section, l.section_length, section, section_length)) { piece = l.key; piece_length = l.key_length; }
    if (!piece) continue;
    while (piece_length && (*piece == ' ' || *piece == '\t')) { ++piece; --piece_length; }
    while (piece_length && (piece[piece_length - 1] == ' ' || piece[piece_length - 1] == '\t')) --piece_length;
    if (used + piece_length + 1 > capacity - 2) {      /* what fits is copied, and the two end marks follow it */
      const DWORD room = capacity - 2 - used;
      memcpy(out + used, piece, piece_length < room ? piece_length : room);
      used += (DWORD)(piece_length < room ? piece_length : room);
      truncated = TRUE;
      break;
    }
    memcpy(out + used, piece, piece_length);
    used += (DWORD)piece_length;
    out[used++] = 0;
  }
  if (truncated) used = capacity - 2;
  out[used] = 0;
  out[used + 1] = 0;
  return used;
}
EXPORT DWORD WINAPI GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR default_value, LPSTR out, DWORD capacity, LPCSTR file) {
  WCHAR *w = file ? to_wide(file) : NULL;
  IniText ini = {NULL, 0};
  const BOOL have = file ? (w && ini_read(w, &ini)) : ini_read(g_win_ini, &ini);      /* (no file: win.ini) */
  free_wide(w);
  DWORD result = 0;
  if (!out || capacity == 0) { ini_free(&ini); return 0; }
  if (!have && (!section || !key)) { out[0] = 0; return 0; }      /* (a list from a file that is not there is one end mark) */
  if (have && !section) { result = ini_list(&ini, NULL, 0, TRUE, out, capacity); ini_free(&ini); return result; }
  if (have && !key) { result = ini_list(&ini, section, strlen(section), FALSE, out, capacity); ini_free(&ini); return result; }
  if (have) {
    size_t_ at = 0, s_len = 0;
    const char *current = ini_no_section;
    IniLine l;
    while (ini_next(&ini, &at, &l, &current, &s_len)) {
      if (!l.entry || !ini_equal(l.section, l.section_length, section, strlen(section)) || !ini_equal(l.key, l.key_length, key, strlen(key))) continue;
      const char *v = l.value;
      size_t_ n = l.value_length;
      ini_value_range(&v, &n);
      result = ini_copy_out(v, n, out, capacity);
      ini_free(&ini);
      return result;
    }
  }
  ini_free(&ini);
  const char *d = default_value ? default_value : "";
  size_t_ n = strlen(d);
  while (n && d[n - 1] == ' ') --n;      /* (trailing spaces of a default go) */
  return ini_copy_out(d, n, out, capacity);
}
EXPORT DWORD WINAPI GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR default_value, LPWSTR out, DWORD capacity, LPCWSTR file) {
  if (!out || capacity == 0) return 0;
  char *a = NULL, *k = NULL, *d = NULL, *f = NULL;
  #define NARROW(var, text) if (text) { const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL); var = RtlAllocateHeap(heap(), 0, (SIZE_T)n + 1); if (var) WideCharToMultiByte(CP_UTF8, 0, text, -1, var, n, NULL, NULL); }
  NARROW(a, section) NARROW(k, key) NARROW(d, default_value) NARROW(f, (file ? file : g_win_ini))
  #undef NARROW
  if ((!section || !key) && capacity >= 2 && (!section || a) && (!key || k) && f) {      /* a list: made whole, then cut to the wide buffer */
    char *whole = RtlAllocateHeap(heap(), 0, 32768);
    WCHAR *wide_whole = RtlAllocateHeap(heap(), 0, 32768 * 2);
    DWORD done = 0;
    if (whole && wide_whole) {
      const DWORD n = GetPrivateProfileStringA(a, k, d, whole, 32768 - 2, f);
      const int converted = n ? MultiByteToWideChar(CP_UTF8, 0, whole, (int)n, wide_whole, 32768) : 0;
      DWORD length = converted > 0 ? (DWORD)converted : 0;      /* (the names with their end marks, without the last) */
      if (length > capacity - 2) length = capacity - 2;      /* what does not fit is cut off */
      if (length) memcpy(out, wide_whole, (size_t_)length * 2);
      out[length] = 0;
      out[length + 1] = 0;
      done = length;
    } else {
      out[0] = out[1] = 0;
    }
    if (done == 0) { WCHAR first = out[0]; (void)first; }
    if (whole) RtlFreeHeap(heap(), 0, whole);
    if (wide_whole) RtlFreeHeap(heap(), 0, wide_whole);
    if (a) RtlFreeHeap(heap(), 0, a);
    if (k) RtlFreeHeap(heap(), 0, k);
    if (d) RtlFreeHeap(heap(), 0, d);
    if (f) RtlFreeHeap(heap(), 0, f);
    return done;
  }
  char *buffer = RtlAllocateHeap(heap(), 0, (SIZE_T)capacity * 4 + 4);
  DWORD result = 0;
  if (buffer && (!section || a) && (!key || k) && f) {
    const DWORD n = GetPrivateProfileStringA(a, k, d, buffer, capacity * 4, f);
    /* the characters (a list is NUL-separated and ends with two NULs) */
    const DWORD length = (!a || !k) ? n + 1 : n;
    const int wide = MultiByteToWideChar(CP_UTF8, 0, buffer, (int)length, out, (int)capacity - 1);
    if (wide > 0) {
      out[wide] = 0;
      result = (!a || !k) ? (DWORD)wide - 1 : (DWORD)wide;
    } else {
      out[0] = 0;
    }
  } else {
    out[0] = 0;
  }
  if (buffer) RtlFreeHeap(heap(), 0, buffer);
  if (a) RtlFreeHeap(heap(), 0, a);
  if (k) RtlFreeHeap(heap(), 0, k);
  if (d) RtlFreeHeap(heap(), 0, d);
  if (f) RtlFreeHeap(heap(), 0, f);
  return result;
}
EXPORT UINT WINAPI GetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT default_value, LPCSTR file) {
  char text[64];
  const DWORD length = GetPrivateProfileStringA(section, key, "", text, sizeof(text), file);
  if (length == 0) return (UINT)default_value;      /* (nothing there, or an empty value) */
  char *c = text;
  while (*c == ' ') ++c;
  const BOOL negative = *c == '-';
  if (*c == '-' || *c == '+') ++c;
  unsigned long long v = 0;
  while (*c >= '0' && *c <= '9') v = v * 10 + (unsigned)(*c++ - '0');
  return (UINT)(negative ? -(long long)v : (long long)v);
}
EXPORT UINT WINAPI GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT default_value, LPCWSTR file) {
  WCHAR wide[64];
  static const WCHAR empty[] = {0};
  const DWORD length = GetPrivateProfileStringW(section, key, empty, wide, 64, file);
  if (length == 0) return (UINT)default_value;
  WCHAR *c = wide;
  while (*c == ' ') ++c;
  const BOOL negative = *c == '-';
  if (*c == '-' || *c == '+') ++c;
  unsigned long long v = 0;
  while (*c >= '0' && *c <= '9') v = v * 10 + (unsigned)(*c++ - '0');
  return (UINT)(negative ? -(long long)v : (long long)v);
}
/* Writes: a key's value changed, added or removed (value NULL), or a whole section removed (key NULL). */
static BOOL ini_write(LPCWSTR given, const char *section, const char *key, const char *value) {
  WCHAR resolved[PATH_CHARS];
  const LPCWSTR path = ini_path(given, resolved, PATH_CHARS);
  IniText ini;
  if (!ini_read(path, &ini)) { ini.text = RtlAllocateHeap(heap(), 0, 2); if (!ini.text) return FALSE; ini.text[0] = 0; ini.size = 0; }
  const size_t_ section_length = strlen(section), key_length = key ? strlen(key) : 0, value_length = value ? strlen(value) : 0;
  char *out = RtlAllocateHeap(heap(), 0, ini.size + section_length + key_length + value_length + 16);
  if (!out) { ini_free(&ini); return FALSE; }
  size_t_ o = 0, at = 0, s_len = 0;
  const char *current = ini_no_section;
  BOOL in_section = FALSE, done = FALSE, section_seen = FALSE;
  IniLine l;
  while (ini_next(&ini, &at, &l, &current, &s_len)) {
    size_t_ end_of_line = at;      /* where the next line starts (the line, with its line end) */
    if (l.header) {
      if (in_section && !done && key && value) {      /* the section ends without the key: it goes at the end of the section */
        memcpy(out + o, key, key_length); o += key_length; out[o++] = '='; memcpy(out + o, value, value_length); o += value_length; out[o++] = '\r'; out[o++] = '\n';
        done = TRUE;
      }
      in_section = ini_equal(current, s_len, section, section_length);
      if (in_section) section_seen = TRUE;
      if (in_section && !key) continue;      /* (the whole section goes) */
    } else if (in_section && !key) {
      continue;
    } else if (in_section && l.entry && key && ini_equal(l.key, l.key_length, key, key_length)) {
      if (value) { memcpy(out + o, key, key_length); o += key_length; out[o++] = '='; memcpy(out + o, value, value_length); o += value_length; out[o++] = '\r'; out[o++] = '\n'; }
      done = TRUE;
      continue;
    }
    memcpy(out + o, ini.text + l.offset, end_of_line - l.offset);
    o += end_of_line - l.offset;
    if (end_of_line > 0 && ini.text[end_of_line - 1] != '\n' && ini.text[end_of_line - 1] != '\r' && l.length) { out[o++] = '\r'; out[o++] = '\n'; }
  }
  if (!done && value && key) {
    if (!section_seen) { out[o++] = '['; memcpy(out + o, section, section_length); o += section_length; out[o++] = ']'; out[o++] = '\r'; out[o++] = '\n'; }
    memcpy(out + o, key, key_length); o += key_length; out[o++] = '='; memcpy(out + o, value, value_length); o += value_length; out[o++] = '\r'; out[o++] = '\n';
  }
  ini_free(&ini);
  HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD wrote = 0;
  const BOOL ok = h != INVALID_HANDLE_VALUE && (o == 0 || WriteFile(h, out, (DWORD)o, &wrote, NULL));
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  RtlFreeHeap(heap(), 0, out);
  return ok;
}
EXPORT BOOL WINAPI WritePrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file) {
  if (!section) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }      /* (that is the answer, odd as it is) */
  if (!file) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR *w = to_wide(file);
  if (!w) return FALSE;
  const BOOL ok = ini_write(w, section, key, value);
  free_wide(w);
  return ok;
}
EXPORT BOOL WINAPI WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file) {
  if (!section) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
  if (!file) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  char s[256], k[256], v[1024];
  if (!WideCharToMultiByte(CP_UTF8, 0, section, -1, s, sizeof(s), NULL, NULL)) return FALSE;
  if (key && !WideCharToMultiByte(CP_UTF8, 0, key, -1, k, sizeof(k), NULL, NULL)) return FALSE;
  if (value && !WideCharToMultiByte(CP_UTF8, 0, value, -1, v, sizeof(v), NULL, NULL)) return FALSE;
  return ini_write(file, s, key ? k : NULL, value ? v : NULL);
}
/* The lines of a section as "key=value" strings, each ended by a NUL, the list ended by one more (blank lines and comments are left out). The result is the length without that
   last NUL; a buffer too small gets what fits and size - 2. An empty section is an empty list; a file that is not there is ERROR_FILE_NOT_FOUND. */
static DWORD ini_section_lines(LPCWSTR path, const char *section, LPSTR out, DWORD capacity) {
  IniText ini;
  if (!out || capacity < 2 || !section) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  out[0] = out[1] = 0;
  if (!ini_read(path, &ini)) { set_error(ERROR_FILE_NOT_FOUND); return 0; }
  DWORD used = 0;
  BOOL truncated = FALSE;
  const size_t_ section_length = strlen(section);
  size_t_ at = 0, s_len = 0;
  const char *current = ini_no_section;
  IniLine l;
  BOOL found = FALSE;
  while (ini_next(&ini, &at, &l, &current, &s_len)) {
    if (l.header && ini_equal(current, s_len, section, section_length)) found = TRUE;
    if (l.header || !ini_equal(l.section, l.section_length, section, section_length)) continue;
    const char *p = l.line;
    size_t_ n = l.length;
    while (n && (*p == ' ' || *p == '\t' || *p == 0)) { ++p; --n; }
    if (n == 0 || *p == ';') continue;
    if (used + n + 2 > capacity) {
      const DWORD room = capacity > used + 2 ? capacity - used - 2 : 0;
      memcpy(out + used, p, room);
      used += room;
      truncated = TRUE;
      break;
    }
    memcpy(out + used, p, n);
    used += (DWORD)n;
    out[used++] = 0;
  }
  ini_free(&ini);
  if (truncated) { out[capacity - 2] = out[capacity - 1] = 0; used = capacity - 2; }
  else out[used] = 0;
  if (used == 0) out[1] = 0;
  set_error(found ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
  return used;
}
EXPORT DWORD WINAPI GetPrivateProfileSectionA(LPCSTR section, LPSTR out, DWORD capacity, LPCSTR file) {
  if (!section) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *w = file ? to_wide(file) : NULL;
  if (!file) return ini_section_lines(g_win_ini, section, out, capacity);
  if (!w) return 0;
  const DWORD n = ini_section_lines(w, section, out, capacity);
  free_wide(w);
  return n;
}
EXPORT DWORD WINAPI GetPrivateProfileSectionW(LPCWSTR section, LPWSTR out, DWORD capacity, LPCWSTR file) {
  if (!section || !out || capacity < 2) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (!file) file = g_win_ini;
  char name[256];
  if (!WideCharToMultiByte(CP_UTF8, 0, section, -1, name, sizeof(name), NULL, NULL)) return 0;
  char *narrow = RtlAllocateHeap(heap(), 0, (SIZE_T)capacity * 4 + 4);
  if (!narrow) return 0;
  const DWORD n = ini_section_lines(file, name, narrow, capacity * 4);
  DWORD result = 0;
  out[0] = out[1] = 0;
  if (n) {
    const int wide = MultiByteToWideChar(CP_UTF8, 0, narrow, (int)n + 1, out, (int)capacity - 1);
    if (wide > 0) { out[wide] = 0; result = (DWORD)wide - 1; }
    if (result > capacity - 2) result = capacity - 2;
    out[result] = 0;
    out[result + 1] = 0;
  }
  RtlFreeHeap(heap(), 0, narrow);
  return result;
}
/* The section is replaced by the lines in `lines` (NUL-separated, ended by an empty one); none at all leaves an empty section. */
static BOOL ini_write_section(LPCWSTR given, const char *section, const char *lines) {
  if (!ini_write(given, section, NULL, NULL)) return FALSE;      /* the old section goes */
  WCHAR resolved[PATH_CHARS];
  const LPCWSTR path = ini_path(given, resolved, PATH_CHARS);
  IniText ini;
  if (!ini_read(path, &ini)) { ini.text = RtlAllocateHeap(heap(), 0, 2); if (!ini.text) return FALSE; ini.text[0] = 0; ini.size = 0; }
  size_t_ extra = strlen(section) + 8;
  for (const char *p = lines ? lines : ""; *p; p += strlen(p) + 1) extra += strlen(p) + 2;
  char *out = RtlAllocateHeap(heap(), 0, ini.size + extra + 4);
  if (!out) { ini_free(&ini); return FALSE; }
  size_t_ o = 0;
  memcpy(out, ini.text, ini.size);
  o = ini.size;
  if (o && out[o - 1] != '\n') { out[o++] = '\r'; out[o++] = '\n'; }
  out[o++] = '[';
  memcpy(out + o, section, strlen(section)); o += strlen(section);
  out[o++] = ']'; out[o++] = '\r'; out[o++] = '\n';
  for (const char *p = lines ? lines : ""; *p; p += strlen(p) + 1) { const size_t_ n = strlen(p); memcpy(out + o, p, n); o += n; out[o++] = '\r'; out[o++] = '\n'; }
  ini_free(&ini);
  HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD wrote = 0;
  const BOOL ok = h != INVALID_HANDLE_VALUE && WriteFile(h, out, (DWORD)o, &wrote, NULL);
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  RtlFreeHeap(heap(), 0, out);
  return ok;
}
EXPORT BOOL WINAPI WritePrivateProfileSectionA(LPCSTR section, LPCSTR lines, LPCSTR file) {
  if (!section || !file) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR *w = to_wide(file);
  if (!w) return FALSE;
  const BOOL ok = ini_write_section(w, section, lines);
  free_wide(w);
  return ok;
}
EXPORT BOOL WINAPI WritePrivateProfileSectionW(LPCWSTR section, LPCWSTR lines, LPCWSTR file) {
  if (!section || !file) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  char name[256];
  if (!WideCharToMultiByte(CP_UTF8, 0, section, -1, name, sizeof(name), NULL, NULL)) return FALSE;
  size_t_ units = 1;
  if (lines) { const WCHAR *p = lines; while (*p) p += wcslen(p) + 1; units = (size_t_)(p - lines) + 1; }
  char *narrow = lines ? RtlAllocateHeap(heap(), 0, units * 4 + 4) : NULL;
  if (lines && !narrow) return FALSE;
  if (narrow) { const int n = WideCharToMultiByte(CP_UTF8, 0, lines, (int)units, narrow, (int)units * 4, NULL, NULL); narrow[n] = 0; narrow[n + 1] = 0; }
  const BOOL ok = ini_write_section(file, name, narrow);
  if (narrow) RtlFreeHeap(heap(), 0, narrow);
  return ok;
}
EXPORT DWORD WINAPI GetProfileSectionA(LPCSTR section, LPSTR out, DWORD capacity) { return GetPrivateProfileSectionA(section, out, capacity, "C:\\Windows\\win.ini"); }
EXPORT DWORD WINAPI GetProfileSectionW(LPCWSTR section, LPWSTR out, DWORD capacity) { return GetPrivateProfileSectionW(section, out, capacity, g_win_ini); }
EXPORT BOOL WINAPI WriteProfileSectionA(LPCSTR section, LPCSTR lines) { return WritePrivateProfileSectionA(section, lines, "C:\\Windows\\win.ini"); }
EXPORT BOOL WINAPI WriteProfileSectionW(LPCWSTR section, LPCWSTR lines) { return WritePrivateProfileSectionW(section, lines, g_win_ini); }

/* A structure is kept as the hexadecimal digits of its bytes and a last byte, the sum of the others. */
EXPORT BOOL WINAPI WritePrivateProfileStructA(LPCSTR section, LPCSTR key, LPVOID data, UINT size, LPCSTR file) {
  if (!section || !file) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!data) return WritePrivateProfileStringA(section, key, NULL, file);
  char *text = RtlAllocateHeap(heap(), 0, (SIZE_T)size * 2 + 3);
  if (!text) return FALSE;
  static const char kDigits[] = "0123456789ABCDEF";
  BYTE sum = 0;
  for (UINT i = 0; i < size; i++) { const BYTE b = ((const BYTE *)data)[i]; sum = (BYTE)(sum + b); text[2 * i] = kDigits[b >> 4]; text[2 * i + 1] = kDigits[b & 15]; }
  text[2 * size] = kDigits[sum >> 4];
  text[2 * size + 1] = kDigits[sum & 15];
  text[2 * size + 2] = 0;
  const BOOL ok = WritePrivateProfileStringA(section, key, text, file);
  RtlFreeHeap(heap(), 0, text);
  return ok;
}
EXPORT BOOL WINAPI WritePrivateProfileStructW(LPCWSTR section, LPCWSTR key, LPVOID data, UINT size, LPCWSTR file) {
  if (!section || !file) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  char *a = to_ansi_alloc(section), *k = key ? to_ansi_alloc(key) : NULL, *f = to_ansi_alloc(file);
  BOOL ok = FALSE;
  if (a && f && (!key || k)) ok = WritePrivateProfileStructA(a, k, data, size, f);
  if (a) RtlFreeHeap(heap(), 0, a);
  if (k) RtlFreeHeap(heap(), 0, k);
  if (f) RtlFreeHeap(heap(), 0, f);
  return ok;
}
EXPORT BOOL WINAPI GetPrivateProfileStructA(LPCSTR section, LPCSTR key, LPVOID buffer, UINT size, LPCSTR file) {
  if (!section || !key || !file || !buffer) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  char text[2 * 1024 + 8];
  const DWORD n = GetPrivateProfileStringA(section, key, "", text, sizeof(text), file);
  if (n != 2 * (size + 1)) { set_error(ERROR_BAD_LENGTH); return FALSE; }
  BYTE sum = 0;
  for (UINT i = 0; i <= size; i++) {
    int v[2];
    for (int d = 0; d < 2; d++) {
      const char c = text[2 * i + d];
      v[d] = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (v[d] < 0) { set_error(ERROR_INVALID_DATA); return FALSE; }
    }
    const BYTE b = (BYTE)(v[0] << 4 | v[1]);
    if (i < size) { sum = (BYTE)(sum + b); ((BYTE *)buffer)[i] = b; }
    else if (b != sum) { set_error(ERROR_INVALID_DATA); return FALSE; }
  }
  return TRUE;
}
EXPORT BOOL WINAPI GetPrivateProfileStructW(LPCWSTR section, LPCWSTR key, LPVOID buffer, UINT size, LPCWSTR file) {
  if (!section || !key || !file || !buffer) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  char *a = to_ansi_alloc(section), *k = to_ansi_alloc(key), *f = to_ansi_alloc(file);
  BOOL ok = FALSE;
  if (a && k && f) ok = GetPrivateProfileStructA(a, k, buffer, size, f);
  if (a) RtlFreeHeap(heap(), 0, a);
  if (k) RtlFreeHeap(heap(), 0, k);
  if (f) RtlFreeHeap(heap(), 0, f);
  return ok;
}
EXPORT DWORD WINAPI GetPrivateProfileSectionNamesA(LPSTR out, DWORD capacity, LPCSTR file) { return GetPrivateProfileStringA(NULL, NULL, NULL, out, capacity, file); }
EXPORT DWORD WINAPI GetPrivateProfileSectionNamesW(LPWSTR out, DWORD capacity, LPCWSTR file) { return GetPrivateProfileStringW(NULL, NULL, NULL, out, capacity, file); }
/* The win.ini of the system */
EXPORT DWORD WINAPI GetProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR default_value, LPWSTR out, DWORD capacity) { return GetPrivateProfileStringW(section, key, default_value, out, capacity, g_win_ini); }
EXPORT DWORD WINAPI GetProfileStringA(LPCSTR section, LPCSTR key, LPCSTR default_value, LPSTR out, DWORD capacity) { return GetPrivateProfileStringA(section, key, default_value, out, capacity, "C:\\Windows\\win.ini"); }
EXPORT UINT WINAPI GetProfileIntW(LPCWSTR section, LPCWSTR key, INT default_value) { return GetPrivateProfileIntW(section, key, default_value, g_win_ini); }
EXPORT UINT WINAPI GetProfileIntA(LPCSTR section, LPCSTR key, INT default_value) { return GetPrivateProfileIntA(section, key, default_value, "C:\\Windows\\win.ini"); }
EXPORT BOOL WINAPI WriteProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value) { return WritePrivateProfileStringW(section, key, value, g_win_ini); }
EXPORT BOOL WINAPI WriteProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value) { return WritePrivateProfileStringA(section, key, value, "C:\\Windows\\win.ini"); }

/* ---- resources of a module (the resource directory of its PE file) ---------------------------------------------------------------------------------------- */

typedef struct { DWORD Name, OffsetToData; } ResourceEntry;
typedef struct { DWORD Characteristics, TimeDateStamp; WORD MajorVersion, MinorVersion, NumberOfNamedEntries, NumberOfIdEntries; } ResourceDirectory;
typedef struct { DWORD OffsetToData, Size, CodePage, Reserved; } ResourceData;
/* A library loaded only for its data is a mapped view with a tag in the low bits of the handle: 1 for the file as it is (a relative address has to be turned into a file
 * offset through the section table), 2 for the file mapped as an image (relative addresses are good as they are). */
static const BYTE *resource_base(HMODULE module) { return module ? (const BYTE *)((ULONG_PTR)module & ~(ULONG_PTR)3) : (const BYTE *)NtCurrentPeb()->ImageBaseAddress; }
static const BYTE *resource_address(HMODULE module, DWORD rva) {
  const BYTE *base = resource_base(module);
  if (((ULONG_PTR)module & 3) != 1) return base + rva;
  const ULONG pe = *(const ULONG *)(base + 0x3C);
  const WORD sections = *(const WORD *)(base + pe + 6), optional = *(const WORD *)(base + pe + 20);
  const BYTE *table = base + pe + 24 + optional;
  for (unsigned i = 0; i < sections; ++i) {
    const BYTE *sec = table + 40u * i;
    const DWORD virtual_size = *(const DWORD *)(sec + 8), va = *(const DWORD *)(sec + 12), raw_size = *(const DWORD *)(sec + 16), raw = *(const DWORD *)(sec + 20);
    const DWORD span = virtual_size > raw_size ? virtual_size : raw_size;
    if (rva >= va && rva - va < span) return base + raw + (rva - va);
  }
  return base + rva;
}
static const ResourceDirectory *resource_root(HMODULE module, DWORD *size) {
  const BYTE *base = resource_base(module);
  const ULONG pe = *(const ULONG *)(base + 0x3C);
  const ULONG rva = *(const ULONG *)(base + pe + 0x18 + 0x70 + 2 * 8), bytes = *(const ULONG *)(base + pe + 0x18 + 0x70 + 2 * 8 + 4);
  if (!rva) return NULL;
  *size = bytes;
  return (const ResourceDirectory *)resource_address(module, rva);
}
/* An entry of a directory by id or by name (NULL if none). */
static const ResourceEntry *resource_find(const ResourceDirectory *root, const ResourceDirectory *dir, LPCWSTR name) {
  const ResourceEntry *entries = (const ResourceEntry *)(dir + 1);
  const unsigned count = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
  const BOOL by_id = ((ULONG_PTR)name >> 16) == 0;
  for (unsigned i = 0; i < count; ++i) {
    const ResourceEntry *e = &entries[i];
    if (by_id) {
      if (!(e->Name & 0x80000000u) && (e->Name & 0xFFFF) == (WORD)(ULONG_PTR)name) return e;
    } else if (e->Name & 0x80000000u) {
      const WORD *s = (const WORD *)((const BYTE *)root + (e->Name & 0x7FFFFFFF));
      const WORD length = s[0];
      size_t_ k = 0;
      while (k < length && name[k] && (name[k] == s[1 + k] || ((name[k] | 0x20) == (s[1 + k] | 0x20) && (name[k] | 0x20) >= 'a' && (name[k] | 0x20) <= 'z'))) ++k;
      if (k == length && !name[k]) return e;
    }
  }
  return NULL;
}
EXPORT HRSRC WINAPI FindResourceExW(HMODULE module, LPCWSTR type, LPCWSTR name, WORD language) {
  DWORD size = 0;
  const ResourceDirectory *root = resource_root(module, &size);
  if (!root) { set_error(1812 /* ERROR_RESOURCE_DATA_NOT_FOUND */); return NULL; }
  const ResourceEntry *te = resource_find(root, root, type);
  if (!te || !(te->OffsetToData & 0x80000000u)) { set_error(1813 /* ERROR_RESOURCE_TYPE_NOT_FOUND */); return NULL; }
  const ResourceDirectory *names = (const ResourceDirectory *)((const BYTE *)root + (te->OffsetToData & 0x7FFFFFFF));
  const ResourceEntry *ne = resource_find(root, names, name);
  if (!ne || !(ne->OffsetToData & 0x80000000u)) { set_error(1814 /* ERROR_RESOURCE_NAME_NOT_FOUND */); return NULL; }
  const ResourceDirectory *languages = (const ResourceDirectory *)((const BYTE *)root + (ne->OffsetToData & 0x7FFFFFFF));
  /* the language asked for; a neutral request takes the first there is; a language with no entry of its own falls back to the same primary language, then to the neutral one */
  const ResourceEntry *le = language ? resource_find(root, languages, (LPCWSTR)(ULONG_PTR)language) : NULL;
  const ResourceEntry *all = (const ResourceEntry *)(languages + 1);
  const unsigned count = languages->NumberOfNamedEntries + languages->NumberOfIdEntries;
  if (!le && !language && count) le = &all[0];
  if (!le && language) {
    for (unsigned i = 0; i < count && !le; ++i) if (!(all[i].Name & 0x80000000u) && (all[i].Name & 0x3FF) == (language & 0x3FF) && (all[i].Name & 0xFFFF) != 0) le = &all[i];
    if (!le) le = resource_find(root, languages, (LPCWSTR)(ULONG_PTR)0);
  }
  if (!le) { set_error(1815 /* ERROR_RESOURCE_LANG_NOT_FOUND */); return NULL; }
  if (le->OffsetToData & 0x80000000u) { set_error(1815); return NULL; }
  return (HRSRC)((const BYTE *)root + le->OffsetToData);
}
EXPORT HRSRC WINAPI FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type) { return FindResourceExW(module, type, name, 0); }
EXPORT HRSRC WINAPI FindResourceExA(HMODULE module, LPCSTR type, LPCSTR name, WORD language) {
  WCHAR *t = ((ULONG_PTR)type >> 16) ? to_wide(type) : (WCHAR *)type, *n = ((ULONG_PTR)name >> 16) ? to_wide(name) : (WCHAR *)name;
  HRSRC r = (t && n) ? FindResourceExW(module, t, n, language) : NULL;
  if (((ULONG_PTR)type >> 16)) free_wide(t);
  if (((ULONG_PTR)name >> 16)) free_wide(n);
  return r;
}
EXPORT HRSRC WINAPI FindResourceA(HMODULE module, LPCSTR name, LPCSTR type) { return FindResourceExA(module, type, name, 0); }
EXPORT HGLOBAL WINAPI LoadResource(HMODULE module, HRSRC resource) {
  if (!resource) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  return (HGLOBAL)resource_address(module, ((const ResourceData *)resource)->OffsetToData);
}
EXPORT DWORD WINAPI SizeofResource(HMODULE module, HRSRC resource) { (void)module; return resource ? ((const ResourceData *)resource)->Size : 0; }
EXPORT LPVOID WINAPI LockResource(HGLOBAL resource) { return resource; }
EXPORT BOOL WINAPI FreeResource(HGLOBAL resource) { (void)resource; return FALSE; }

/* ---- byte-range locks --------------------------------------------------------------------------------------------------------------------------------------- */

#define LOCKFILE_FAIL_IMMEDIATELY_ 1
#define LOCKFILE_EXCLUSIVE_LOCK_ 2
static BOOL lock_range(HANDLE h, ULONGLONG offset, ULONGLONG length, BOOL exclusive, BOOL fail_immediately) {
  IO_STATUS_BLOCK iosb;
  LARGE_INTEGER o, l;
  o.QuadPart = (LONGLONG)offset;
  l.QuadPart = (LONGLONG)length;
  const NTSTATUS status = NtLockFile(h, NULL, NULL, NULL, &iosb, &o, &l, 0, fail_immediately != 0, exclusive != 0);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
static BOOL unlock_range(HANDLE h, ULONGLONG offset, ULONGLONG length) {
  IO_STATUS_BLOCK iosb;
  LARGE_INTEGER o, l;
  o.QuadPart = (LONGLONG)offset;
  l.QuadPart = (LONGLONG)length;
  const NTSTATUS status = NtUnlockFile(h, &iosb, &o, &l, 0);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI LockFile(HANDLE h, DWORD offset_low, DWORD offset_high, DWORD length_low, DWORD length_high) {
  return lock_range(h, ((ULONGLONG)offset_high << 32) | offset_low, ((ULONGLONG)length_high << 32) | length_low, TRUE, TRUE);
}
EXPORT BOOL WINAPI UnlockFile(HANDLE h, DWORD offset_low, DWORD offset_high, DWORD length_low, DWORD length_high) {
  return unlock_range(h, ((ULONGLONG)offset_high << 32) | offset_low, ((ULONGLONG)length_high << 32) | length_low);
}
EXPORT BOOL WINAPI LockFileEx(HANDLE h, DWORD flags, DWORD reserved, DWORD length_low, DWORD length_high, LPOVERLAPPED overlapped) {
  (void)reserved;
  if (!overlapped) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const BOOL ok = lock_range(h, ((ULONGLONG)overlapped->OffsetHigh << 32) | overlapped->Offset, ((ULONGLONG)length_high << 32) | length_low, (flags & LOCKFILE_EXCLUSIVE_LOCK_) != 0,
                             (flags & LOCKFILE_FAIL_IMMEDIATELY_) != 0);
  complete_overlapped(overlapped, ok ? STATUS_SUCCESS : (NTSTATUS)0xC0000055, 0);
  return ok;
}
EXPORT BOOL WINAPI UnlockFileEx(HANDLE h, DWORD reserved, DWORD length_low, DWORD length_high, LPOVERLAPPED overlapped) {
  (void)reserved;
  if (!overlapped) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return unlock_range(h, ((ULONGLONG)overlapped->OffsetHigh << 32) | overlapped->Offset, ((ULONGLONG)length_high << 32) | length_low);
}
