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
  if (root && !root[0]) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
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
/* SearchPath: `path` is a list of folders separated by ';' (NULL: the usual places, which RtlGetSearchPath knows). A name with a drive or a root is looked for as it is. With no
 * path, the active activation context may name the file first. */
static BOOL actctx_redirect_key(LPCWSTR name, WCHAR *out, DWORD capacity, BOOL append_dll);
static BOOL search_list(LPCWSTR list, LPCWSTR file, WCHAR *found) {
  WCHAR candidate[PATH_CHARS];
  const size_t_ fl = wcslen(file);
  for (LPCWSTR start = list; *start;) {
    LPCWSTR end = start;
    while (*end && *end != ';') ++end;
    const size_t_ dl = (size_t_)(end - start);
    if (dl > 0 && dl + 1 + fl + 1 <= PATH_CHARS) {
      memcpy(candidate, start, dl * 2);
      candidate[dl] = '\\';
      memcpy(candidate + dl + 1, file, (fl + 1) * 2);
      WCHAR full[PATH_CHARS];
      if (full_path_w(candidate, full, PATH_CHARS) && is_program_file(full)) { lstrcpyW(found, full); return TRUE; }
    }
    start = *end ? end + 1 : end;
  }
  return FALSE;
}
EXPORT DWORD WINAPI SearchPathW(LPCWSTR path, LPCWSTR file, LPCWSTR extension, DWORD capacity, LPWSTR buffer, LPWSTR *file_part) {
  WCHAR found[PATH_CHARS];
  BOOL ok = FALSE;
  if (!file) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  {      /* (an empty name, or one of blanks only) */
    LPCWSTR c = file;
    while (*c == ' ') ++c;
    if (!*c) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  }
  BOOL has_extension = FALSE;
  for (LPCWSTR c = file; *c; ++c) if (*c == '.') has_extension = TRUE; else if (*c == '\\' || *c == '/') has_extension = FALSE;
  const size_t_ fl = wcslen(file), el = extension ? wcslen(extension) : 0;
  WCHAR with_extension[MAX_PATH];
  if (fl + el + 1 > MAX_PATH) { set_error(ERROR_FILENAME_EXCED_RANGE); return 0; }
  memcpy(with_extension, file, (fl + 1) * 2);
  if (!has_extension && el) memcpy(with_extension + fl, extension, (el + 1) * 2);
  const BOOL rooted = file[0] == '\\' || file[0] == '/' || (file[0] && file[1] == ':');
  if (rooted) {
    WCHAR full[PATH_CHARS];
    ok = (full_path_w(file, full, PATH_CHARS) && is_program_file(full) && (lstrcpyW(found, full), TRUE)) ||
         (with_extension[fl] && full_path_w(with_extension, full, PATH_CHARS) && is_program_file(full) && (lstrcpyW(found, full), TRUE));
  } else if (path && *path) {
    ok = search_list(path, with_extension, found);
  } else {
    BOOL bare = TRUE;
    for (LPCWSTR c = file; *c; ++c) if (*c == '\\' || *c == '/') bare = FALSE;
    if (bare && (actctx_redirect_key(file, found, PATH_CHARS, FALSE) || (with_extension[fl] && actctx_redirect_key(with_extension, found, PATH_CHARS, FALSE)))) {
      ok = TRUE;
    } else {
      PWSTR list = NULL;
      if (NT_SUCCESS(RtlGetSearchPath(&list))) {
        ok = search_list(list, with_extension, found);
        RtlReleasePath(list);
      }
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
  if (!file) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *wp = path ? to_wide(path) : NULL, *wf = to_wide(file), *we = extension ? to_wide(extension) : NULL;
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

/* The classes of FILE_INFO_BY_HANDLE_CLASS: how each is asked of the system (the class of NtQueryInformationFile or of NtSetInformationFile), and the smallest buffer it takes.
 * -1 means that the call has no such direction. */
typedef struct { signed char query, set; BYTE query_size; } FileInfoClass;
static const FileInfoClass g_file_info_classes[] = {
  {4, 4, 40},        /* 0 FileBasicInfo */
  {5, -1, 24},       /* 1 FileStandardInfo */
  {9, -1, 4},        /* 2 FileNameInfo */
  {-1, 10, 0},       /* 3 FileRenameInfo */
  {-1, 13, 0},       /* 4 FileDispositionInfo */
  {-1, 19, 0},       /* 5 FileAllocationInfo */
  {-1, 20, 0},       /* 6 FileEndOfFileInfo */
  {-1, -1, 0},       /* 7 FileStreamInfo */
  {-1, -1, 16},      /* 8 FileCompressionInfo */
  {-1, -1, 8},       /* 9 FileAttributeTagInfo */
  {37, -1, 104},     /* 10 FileIdBothDirectoryInfo */
  {37, -1, 104},     /* 11 FileIdBothDirectoryRestartInfo */
  {-1, 43, 0},       /* 12 FileIoPriorityHintInfo */
  {-1, -1, 0},       /* 13 FileRemoteProtocolInfo */
  {2, -1, 68},       /* 14 FileFullDirectoryInfo */
  {2, -1, 68},       /* 15 FileFullDirectoryRestartInfo */
};
EXPORT BOOL WINAPI GetFileInformationByHandleEx(HANDLE h, int info_class, LPVOID info, DWORD size) {
  IO_STATUS_BLOCK iosb;
  NTSTATUS status;
  if (info_class < 0 || info_class >= (int)(sizeof(g_file_info_classes) / sizeof(g_file_info_classes[0]))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (info_class == 8 || info_class == 9) {      /* FileCompressionInfo { CompressedFileSize, CompressionFormat, ... } and FileAttributeTagInfo { FileAttributes, ReparseTag } */
    BYTE basic[40], standard[24];
    if (size < (DWORD)g_file_info_classes[info_class].query_size) { set_error(ERROR_BAD_LENGTH); return FALSE; }
    if (!info) { set_error(ERROR_NOACCESS); return FALSE; }
    status = NtQueryInformationFile(h, &iosb, basic, sizeof(basic), 4);
    if (!NT_SUCCESS(status)) return fail_nt(status);
    if (info_class == 9) { ((DWORD *)info)[0] = get32(basic + 32); ((DWORD *)info)[1] = 0; return TRUE; }
    status = NtQueryInformationFile(h, &iosb, standard, sizeof(standard), 5);
    if (!NT_SUCCESS(status)) return fail_nt(status);
    memset(info, 0, 16);
    memcpy(info, standard + 8, 8);      /* the compressed size is the size: nothing is compressed */
    return TRUE;
  }
  const FileInfoClass *c = &g_file_info_classes[info_class];
  if (c->query < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (size < c->query_size) { set_error(ERROR_BAD_LENGTH); return FALSE; }
  if (!info) { set_error(ERROR_NOACCESS); return FALSE; }
  if (c->query == 37 || c->query == 2) {      /* a listing goes on where the last call stopped, the Restart classes begin again */
    UNICODE_STRING *no_pattern = NULL;
    status = NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, info, size, c->query, FALSE, no_pattern, info_class == 11 || info_class == 15);
    if (status == STATUS_NO_MORE_FILES) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
  } else {
    status = NtQueryInformationFile(h, &iosb, info, size, c->query);
  }
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI SetFileInformationByHandle(HANDLE h, int info_class, LPVOID info, DWORD size) {
  IO_STATUS_BLOCK iosb;
  if (info_class < 0 || info_class >= (int)(sizeof(g_file_info_classes) / sizeof(g_file_info_classes[0])) || g_file_info_classes[info_class].set < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const NTSTATUS status = NtSetInformationFile(h, &iosb, info, size, g_file_info_classes[info_class].set);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* ReOpenFile: another handle to the file of an open one, with the access, sharing and flags given (the flags are the FILE_FLAG_ ones; attributes make no sense for an open). A
 * directory cannot be reopened. */
EXPORT HANDLE WINAPI ReOpenFile(HANDLE original, DWORD access, DWORD share, DWORD flags) {
  if (flags & 0xFFFF) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  WCHAR path[PATH_CHARS + 64];
  DWORD n = 0;
  if (device_type(original) == 0x11) {      /* a pipe: another end of it, by its name */
    IO_STATUS_BLOCK iosb;
    BYTE info[4 + 2 * 256];
    if (NT_SUCCESS(NtQueryInformationFile(original, &iosb, info, sizeof(info), 9)) && get32(info) / 2 < 256) {
      static const WCHAR folder[] = {'\\', '\\', '.', '\\', 'p', 'i', 'p', 'e'};
      memcpy(path, folder, sizeof(folder));
      memcpy(path + 8, info + 4, get32(info));
      path[8 + get32(info) / 2] = 0;
      return CreateFileW(path, access, share, NULL, OPEN_EXISTING, flags, NULL);
    }
  }
  n = GetFinalPathNameByHandleW(original, path, PATH_CHARS + 64, 0);
  if (n == 0 || n >= PATH_CHARS + 64) {
    if (n == 0 && GetLastError() == ERROR_INVALID_HANDLE) return INVALID_HANDLE_VALUE;
    set_error(n ? ERROR_FILENAME_EXCED_RANGE : ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
  }
  const DWORD attributes = GetFileAttributesW(path);
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) { set_error(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
  return CreateFileW(path, access, share, NULL, OPEN_EXISTING, flags & ~(DWORD)FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

typedef struct { DWORD dwSize; DWORD Type; LONGLONG FileId; LONGLONG ExtendedHigh; } FILE_ID_DESCRIPTOR_;      /* (the union is 128 bits wide) */
/* OpenFileById: the file whose index (FileInternalInformation, the FileId of a directory listing) is given, looked for from the directory handle. */
EXPORT HANDLE WINAPI OpenFileById(HANDLE hint, const FILE_ID_DESCRIPTOR_ *descriptor, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD flags) {
  if (!descriptor || descriptor->dwSize != sizeof(*descriptor) || descriptor->Type != 0 /* FileIdType */) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  LONGLONG id = descriptor->FileId;
  UNICODE_STRING name;
  name.Buffer = (PWSTR)&id;
  name.Length = name.MaximumLength = sizeof(id);
  OBJECT_ATTRIBUTES attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = hint;
  attributes.ObjectName = &name;
  attributes.Attributes = 0x40 | ((sa && sa->bInheritHandle) ? 2 : 0);
  ULONG nt_access = (access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL)) | SYNCHRONIZE | FILE_READ_ATTRIBUTES;
  if (access & (GENERIC_READ | GENERIC_ALL)) nt_access |= FILE_READ_DATA;
  if (access & (GENERIC_WRITE | GENERIC_ALL)) nt_access |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES;
  if (access & (GENERIC_EXECUTE | GENERIC_ALL)) nt_access |= 0x20;
  ULONG options = 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */ | 0x2000 /* FILE_OPEN_BY_FILE_ID */;
  if (flags & FILE_FLAG_DELETE_ON_CLOSE) { options |= 0x1000; nt_access |= DELETE; }
  HANDLE handle = NULL;
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtCreateFile(&handle, nt_access, &attributes, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, share & 7, 1 /* FILE_OPEN */, options, NULL, 0);
  if (!NT_SUCCESS(status)) { fail_nt(status); return INVALID_HANDLE_VALUE; }
  overlapped_forget(handle);
  if (flags & 0x40000000u /* FILE_FLAG_OVERLAPPED */) overlapped_note(handle);
  return handle;
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
/* ReplaceFile: `replacement` takes the place (and the name) of `replaced`; `backup`, if given, becomes the old `replaced` (it keeps its times). Both files are opened first, the
 * way the move needs them (the replacement for good, exclusively; the replaced one for deleting, which a program that has it open may allow), and the rights are checked. */
EXPORT BOOL WINAPI ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, LPVOID exclude, LPVOID reserved) {
  (void)flags; (void)exclude; (void)reserved;
  if (!replaced || !replacement) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD replacement_attributes = GetFileAttributesW(replacement);
  if (replacement_attributes == INVALID_FILE_ATTRIBUTES) { if (GetLastError() != ERROR_PATH_NOT_FOUND) set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
  const DWORD replaced_attributes = GetFileAttributesW(replaced);
  if (replaced_attributes == INVALID_FILE_ATTRIBUTES) { if (GetLastError() != ERROR_PATH_NOT_FOUND) set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
  if ((replaced_attributes | replacement_attributes) & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  HANDLE hold_replacement = CreateFileW(replacement, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (hold_replacement == INVALID_HANDLE_VALUE) return FALSE;
  HANDLE hold_replaced = CreateFileW(replaced, DELETE, 7, NULL, OPEN_EXISTING, 0, NULL);
  if (hold_replaced == INVALID_HANDLE_VALUE) { const DWORD error = GetLastError(); CloseHandle(hold_replacement); set_error(error); return FALSE; }
  CloseHandle(hold_replaced);
  CloseHandle(hold_replacement);
  /* The old file leaves its name first (to the backup, or to a name of its own that goes away at once), as programs that still have it open may not let it be replaced. */
  WCHAR aside[PATH_CHARS];
  LPCWSTR away = backup;
  if (!backup) {
    static UINT counter;
    WCHAR full[PATH_CHARS];
    const DWORD n = full_path_w(replaced, full, PATH_CHARS - 24);
    if (n == 0) return FALSE;
    DWORD cut = n;
    while (cut > 0 && full[cut - 1] != '\\') --cut;
    memcpy(aside, full, cut * 2);
    static const WCHAR stem[] = {'.', 't', 'c', 'o', 's', '-', 'r', 'e', 'p', 'l', 'a', 'c', 'e', 'd', '-'};
    memcpy(aside + cut, stem, sizeof(stem));
    UINT id = ++counter;
    DWORD at = cut + (DWORD)(sizeof(stem) / 2);
    do { aside[at++] = (WCHAR)('0' + id % 10); id /= 10; } while (id);
    aside[at] = 0;
    away = aside;
  }
  if (!MoveFileExW(replaced, away, 1 /* MOVEFILE_REPLACE_EXISTING */)) return FALSE;
  if (!MoveFileExW(replacement, replaced, 1)) {
    const DWORD error = GetLastError();
    MoveFileExW(away, replaced, 1);      /* (the old file back where it was) */
    set_error(error);
    return FALSE;
  }
  if (!backup) DeleteFileW(away);
  return TRUE;
}
EXPORT BOOL WINAPI ReplaceFileA(LPCSTR replaced, LPCSTR replacement, LPCSTR backup, DWORD flags, LPVOID exclude, LPVOID reserved) {
  WCHAR *a = replaced ? to_wide(replaced) : NULL, *b = replacement ? to_wide(replacement) : NULL, *c = backup ? to_wide(backup) : NULL;
  const BOOL ok = a && b ? ReplaceFileW(a, b, c, flags, exclude, reserved) : FALSE;
  free_wide(a); free_wide(b); free_wide(c);
  return ok;
}
EXPORT BOOL WINAPI CreateDirectoryExW(LPCWSTR template_directory, LPCWSTR path, LPSECURITY_ATTRIBUTES sa) { (void)template_directory; return CreateDirectoryW(path, sa); }
EXPORT BOOL WINAPI CreateDirectoryExA(LPCSTR template_directory, LPCSTR path, LPSECURITY_ATTRIBUTES sa) { (void)template_directory; return CreateDirectoryA(path, sa); }


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

/* GetSystemPowerStatus: this machine is taken to run on mains power with no battery (nothing here reads one). */
EXPORT BOOL WINAPI GetSystemPowerStatus(BYTE *status) {      /* SYSTEM_POWER_STATUS { BYTE AC, BatteryFlag, BatteryLifePercent, SystemStatusFlag; DWORD BatteryLifeTime, BatteryFullLifeTime; } */
  if (!status) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  status[0] = 1;      /* AC_LINE_ONLINE */
  status[1] = 128;      /* BATTERY_FLAG_NO_BATTERY */
  status[2] = 255;      /* BATTERY_PERCENTAGE_UNKNOWN */
  status[3] = 0;
  const DWORD unknown = 0xFFFFFFFFu;      /* BATTERY_LIFE_UNKNOWN */
  memcpy(status + 4, &unknown, 4);
  memcpy(status + 8, &unknown, 4);
  return TRUE;
}

/* PrefetchVirtualMemory: a hint that the ranges are about to be used; everything here is in memory already, so the ranges are only checked to be there to be named. */
EXPORT BOOL WINAPI PrefetchVirtualMemory(HANDLE process, ULONG_PTR count, PVOID entries, ULONG flags) {
  (void)process;
  if (count == 0 || !entries || flags != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
