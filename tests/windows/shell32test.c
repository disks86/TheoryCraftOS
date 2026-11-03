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

/* shell32.dll and ole32.dll: command lines, special folders, item id lists and shell folders, shell items, file operations with the recycle bin, ShellExecute and file
 * associations, shortcuts, SHGetFileInfo, shared memory, and the COM basics (mingw gcc; see tests/windows/build.sh). ShellExecute starts procchild.exe, which must be in
 * C:\Windows\System32 (-DTCOS_USERMODE_DEMO_FILES=...). Prints "shell32test: N checks, M failed"; the exit code is the number of failures. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* not in the headers */
void WINAPI CheckEscapesW(LPWSTR string, DWORD capacity);
int WINAPI SheRemoveQuotesW(LPWSTR string);
HRESULT WINAPI DllGetVersion(DLLVERSIONINFO *info);

static int g_checks, g_failed;
static void check_(int ok, const char *what, int line) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL line %d: %s\n", line, what);
}
#define check(ok, what) check_((ok) ? 1 : 0, what, __LINE__)

static const WCHAR kDir[] = L"C:\\Temp\\s32test";
static void make_file(const WCHAR *path, const char *text) {
  HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD done;
  WriteFile(f, text, (DWORD)strlen(text), &done, NULL);
  CloseHandle(f);
}
static BOOL exists(const WCHAR *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
static BOOL is_dir(const WCHAR *path) { const DWORD a = GetFileAttributesW(path); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void path_in(WCHAR *out, const WCHAR *name) { swprintf(out, MAX_PATH, L"%ls\\%ls", kDir, name); }
/* a double-NUL list from up to four names (each under kDir unless it starts with a drive) */
static void list_of(WCHAR *out, const WCHAR *a, const WCHAR *b, const WCHAR *c) {
  WCHAR *p = out;
  const WCHAR *names[3] = {a, b, c};
  for (int i = 0; i < 3; ++i) {
    if (!names[i]) continue;
    if (names[i][1] == ':') wcscpy(p, names[i]); else swprintf(p, MAX_PATH, L"%ls\\%ls", kDir, names[i]);
    p += wcslen(p) + 1;
  }
  *p = 0;
}
static int fileop(UINT func, FILEOP_FLAGS flags, const WCHAR *from, const WCHAR *to, BOOL *aborted) {
  SHFILEOPSTRUCTW op;
  memset(&op, 0, sizeof(op));
  op.wFunc = func;
  op.fFlags = flags | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMMKDIR;
  op.pFrom = from;
  op.pTo = to;
  const int r = SHFileOperationW(&op);
  if (aborted) *aborted = op.fAnyOperationsAborted;
  return r;
}

static void test_command_lines(void) {
  struct { const WCHAR *line; int count; const WCHAR *args[6]; } cases[] = {
    {L"exe a b", 3, {L"exe", L"a", L"b"}},
    {L"exe \"a b\" c", 3, {L"exe", L"a b", L"c"}},
    {L"exe a\\\\\\\"b", 2, {L"exe", L"a\\\"b"}},
    {L"exe \"a\\\\\" b", 3, {L"exe", L"a\\", L"b"}},
    {L"exe three\"\"\"quotes next", 3, {L"exe", L"three\"quotes", L"next"}},
    {L"\"spaced exe\" arg", 2, {L"spaced exe", L"arg"}},
    {L" \"arg\"one argtwo", 3, {L"", L"argone", L"argtwo"}},
    {L"exe\"file\"path arg1", 2, {L"exe\"file\"path", L"arg1"}},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    int n = -5;
    LPWSTR *argv = CommandLineToArgvW(cases[i].line, &n);
    int ok = argv && n == cases[i].count && argv[n] == NULL;
    for (int k = 0; ok && k < n; ++k) ok = !wcscmp(argv[k], cases[i].args[k]);
    check(ok, "CommandLineToArgvW case");
    if (argv) LocalFree(argv);
  }
  int n = 0;
  LPWSTR *argv = CommandLineToArgvW(L"", &n);
  WCHAR me[MAX_PATH];
  GetModuleFileNameW(NULL, me, MAX_PATH);
  check(argv && n == 1 && !wcscmp(argv[0], me), "empty command line is the program");
  if (argv) LocalFree(argv);
  SetLastError(0);
  check(CommandLineToArgvW(L"x", NULL) == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "no count is an error");

  SetEnvironmentVariableW(L"S32VAR", L"value");
  WCHAR s[64] = L"a %S32VAR% b %NOPE% c";
  const DWORD r = DoEnvironmentSubstW(s, 64);
  check(!wcscmp(s, L"a value b %NOPE% c") && LOWORD(r) == wcslen(s), "DoEnvironmentSubst");
  WCHAR e[32] = L"two words";
  CheckEscapesW(e, 32);
  check(!wcscmp(e, L"\"two words\""), "CheckEscapes quotes a name with a space");
  WCHAR q[32] = L"\"quoted\"";
  check(SheRemoveQuotesW(q) == 6 && !wcscmp(q, L"quoted"), "SheRemoveQuotes");
}

static void test_known_folders(void) {
  WCHAR path[MAX_PATH];
  static const int csidls[] = {CSIDL_DESKTOPDIRECTORY, CSIDL_PERSONAL, CSIDL_APPDATA, CSIDL_LOCAL_APPDATA, CSIDL_COMMON_APPDATA, CSIDL_PROFILE, CSIDL_FAVORITES, CSIDL_STARTMENU, CSIDL_PROGRAMS,
                               CSIDL_MYMUSIC, CSIDL_MYPICTURES, CSIDL_MYVIDEO, CSIDL_PROGRAM_FILES, CSIDL_WINDOWS, CSIDL_SYSTEM, CSIDL_FONTS};
  int ok = 1;
  for (size_t i = 0; i < sizeof(csidls) / sizeof(csidls[0]); ++i) {
    path[0] = 0;
    const HRESULT hr = SHGetFolderPathW(NULL, csidls[i] | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, path);
    if (hr != S_OK || !path[0] || !is_dir(path)) { printf("  csidl 0x%x: hr %lx path '%ls'\n", csidls[i], (unsigned long)hr, path); ok = 0; }
  }
  check(ok, "the file system folders exist after CSIDL_FLAG_CREATE");
  WCHAR desktop[MAX_PATH], documents[MAX_PATH];
  SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop);
  SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, documents);
  check(wcsstr(desktop, L"Desktop") != NULL && wcsstr(documents, L"Documents") != NULL, "folder names");
  check(SHGetFolderPathW(NULL, CSIDL_DRIVES, NULL, 0, path) == E_INVALIDARG, "virtual folders have no path (E_INVALIDARG)");
  check(SHGetFolderPathW(NULL, 0x7F, NULL, 0, path) == E_INVALIDARG, "unknown CSIDL");
  PWSTR known = NULL;
  check(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &known) == S_OK && known && !wcscmp(known, documents), "KnownFolderPath matches the CSIDL");
  CoTaskMemFree(known);
  known = (PWSTR)1;
  check(SHGetKnownFolderPath(&FOLDERID_ComputerFolder, 0, NULL, &known) != S_OK && known == NULL, "virtual known folder fails");
  GUID bogus = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
  check(SHGetKnownFolderPath(&bogus, 0, NULL, &known) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "unknown known folder");
  {
    static const struct { int csidl; const char *variable; } pairs[] = {{CSIDL_PROGRAM_FILES, "ProgramFiles"}, {CSIDL_APPDATA, "APPDATA"}, {CSIDL_PROFILE, "USERPROFILE"}, {CSIDL_WINDOWS, "SystemRoot"},
                                                                         {CSIDL_WINDOWS, "windir"}, {CSIDL_PROGRAM_FILES_COMMON, "CommonProgramFiles"}};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
      char folder_path[MAX_PATH], value[MAX_PATH];
      const DWORD n = GetEnvironmentVariableA(pairs[i].variable, value, MAX_PATH);
      if (!SHGetSpecialFolderPathA(NULL, folder_path, pairs[i].csidl, FALSE) || !n) continue;
      if (_stricmp(folder_path, value)) printf("  %s is '%s', the folder is '%s'\n", pairs[i].variable, value, folder_path);
      check(!_stricmp(folder_path, value), "a folder matches its environment variable");
    }
  }
  char a[MAX_PATH];
  check(SHGetSpecialFolderPathA(NULL, a, CSIDL_WINDOWS, FALSE) && !_stricmp(a, "C:\\Windows"), "SHGetSpecialFolderPathA(CSIDL_WINDOWS)");
  /* the sub directory form */
  check(SHGetFolderPathAndSubDirW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, L"S32Test\\Sub", path) == S_OK && is_dir(path) && wcsstr(path, L"S32Test\\Sub"), "folder and sub directory");
  RemoveDirectoryW(path);
  *wcsrchr(path, '\\') = 0;
  RemoveDirectoryW(path);
  /* redirecting a folder */
  WCHAR redirected[MAX_PATH];
  path_in(redirected, L"redirected_music");
  check(SHSetKnownFolderPath(&FOLDERID_Music, 0, NULL, redirected) == S_OK && is_dir(redirected), "SHSetKnownFolderPath creates the folder");
  known = NULL;
  check(SHGetKnownFolderPath(&FOLDERID_Music, 0, NULL, &known) == S_OK && !wcscmp(known, redirected), "the redirection is used");
  CoTaskMemFree(known);
  known = NULL;
  check(SHGetKnownFolderPath(&FOLDERID_Music, KF_FLAG_DEFAULT_PATH, NULL, &known) == S_OK && wcscmp(known, redirected) != 0, "KF_FLAG_DEFAULT_PATH ignores it");
  CoTaskMemFree(known);
  LPITEMIDLIST pidl = NULL;
  check(SHGetSpecialFolderLocation(NULL, CSIDL_DRIVES, &pidl) == S_OK && pidl && ILGetSize(pidl) > 2, "My Computer as a list");
  ILFree(pidl);
  check(SHGetSpecialFolderLocation(NULL, CSIDL_DESKTOP, &pidl) == S_OK && pidl && ILGetSize(pidl) == 2, "Desktop is the empty list");
  ILFree(pidl);
}

static void test_id_lists(void) {
  WCHAR file[MAX_PATH], back[MAX_PATH];
  CreateDirectoryW(kDir, NULL);
  path_in(file, L"item.txt");
  make_file(file, "hello");
  LPITEMIDLIST pidl = ILCreateFromPathW(file);
  check(pidl != NULL, "ILCreateFromPath");
  check(pidl && SHGetPathFromIDListW(pidl, back) && !wcsicmp(back, file), "list to path and back");
  check(ILCreateFromPathW(L"C:\\Temp\\s32test\\nonexistent.xyz") == NULL, "no list for a missing file");
  LPITEMIDLIST simple = SHSimpleIDListFromPath(L"C:\\Temp\\s32test\\nonexistent.xyz");
  check(simple != NULL, "a simple list for a missing file");
  ILFree(simple);
  if (pidl) {
    LPITEMIDLIST last = ILFindLastID(pidl), clone = ILClone(pidl), first = ILCloneFirst(pidl);
    check(ILIsEqual(pidl, clone) && last && ILGetSize(last) < ILGetSize(pidl), "clone and last item");
    check(first && ILGetSize(first) == pidl->mkid.cb + 2, "first item");
    LPITEMIDLIST parent = ILClone(pidl);
    check(ILRemoveLastID(parent) && ILIsParent(parent, pidl, TRUE) && ILIsParent(parent, pidl, FALSE) && !ILIsParent(pidl, parent, FALSE), "parent");
    check(ILFindChild(parent, pidl) == (LPITEMIDLIST)((BYTE *)pidl + ILGetSize(parent) - 2), "ILFindChild");
    LPITEMIDLIST joined = ILCombine(parent, last);
    check(joined && ILIsEqual(joined, pidl), "combine");
    ILFree(joined); ILFree(parent); ILFree(first); ILFree(clone);
    /* through a stream */
    IStream *stream = NULL;
    if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) {
      LARGE_INTEGER zero = {0};
      check(SUCCEEDED(ILSaveToStream(stream, pidl)), "ILSaveToStream");
      IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
      LPITEMIDLIST loaded = NULL;
      check(SUCCEEDED(ILLoadFromStream(stream, &loaded)) && loaded && ILIsEqual(loaded, pidl), "ILLoadFromStream");
      ILFree(loaded);
      IStream_Release(stream);
    } else check(0, "CreateStreamOnHGlobal");
    /* binding to the folder */
    IShellFolder *folder = NULL;
    LPCITEMIDLIST child = NULL;
    check(SUCCEEDED(SHBindToParent(pidl, &IID_IShellFolder, (void **)&folder, &child)) && folder && child, "SHBindToParent");
    if (folder) {
      STRRET s;
      WCHAR name[MAX_PATH];
      check(SUCCEEDED(IShellFolder_GetDisplayNameOf(folder, child, SHGDN_FORPARSING, &s)) && SUCCEEDED(StrRetToBufW(&s, child, name, MAX_PATH)) && !wcsicmp(name, file), "display name for parsing is the path");
      check(SUCCEEDED(IShellFolder_GetDisplayNameOf(folder, child, SHGDN_INFOLDER, &s)) && SUCCEEDED(StrRetToBufW(&s, child, name, MAX_PATH)) && !wcsicmp(name, L"item.txt"), "display name in the folder");
      SFGAOF attrs = SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_STREAM | SFGAO_CANDELETE;
      check(SUCCEEDED(IShellFolder_GetAttributesOf(folder, 1, &child, &attrs)) && (attrs & SFGAO_FILESYSTEM) && (attrs & SFGAO_STREAM) && !(attrs & SFGAO_FOLDER), "attributes of a file");
      /* rename */
      LPITEMIDLIST renamed = NULL;
      check(SUCCEEDED(IShellFolder_SetNameOf(folder, NULL, child, L"renamed.txt", SHGDN_NORMAL, &renamed)) && renamed, "SetNameOf");
      WCHAR r[MAX_PATH];
      path_in(r, L"renamed.txt");
      check(exists(r) && !exists(file), "the file was renamed");
      MoveFileW(r, file);
      ILFree(renamed);
      /* the folder's contents */
      WCHAR sub[MAX_PATH];
      path_in(sub, L"enumdir");
      CreateDirectoryW(sub, NULL);
      WCHAR f1[MAX_PATH], f2[MAX_PATH], d1[MAX_PATH];
      swprintf(f1, MAX_PATH, L"%ls\\b.txt", sub); swprintf(f2, MAX_PATH, L"%ls\\a.txt", sub); swprintf(d1, MAX_PATH, L"%ls\\folder", sub);
      make_file(f1, "b"); make_file(f2, "a"); CreateDirectoryW(d1, NULL);
      LPITEMIDLIST subpidl = ILCreateFromPathW(sub);
      IShellFolder *subfolder = NULL;
      check(subpidl && SUCCEEDED(SHBindToObject(NULL, subpidl, NULL, &IID_IShellFolder, (void **)&subfolder)), "bind to a directory");
      if (subfolder) {
        IEnumIDList *en = NULL;
        int files = 0, dirs = 0;
        check(SUCCEEDED(IShellFolder_EnumObjects(subfolder, NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &en)) && en, "EnumObjects");
        LPITEMIDLIST item;
        ULONG got;
        LPITEMIDLIST items[3] = {0};
        while (en && IEnumIDList_Next(en, 1, &item, &got) == S_OK) {
          if (files + dirs < 3) items[files + dirs] = ILClone(item);
          SFGAOF a = SFGAO_FOLDER;
          IShellFolder_GetAttributesOf(subfolder, 1, (LPCITEMIDLIST *)&item, &a);
          if (a & SFGAO_FOLDER) ++dirs; else ++files;
          ILFree(item);
        }
        check(files == 2 && dirs == 1, "two files and a folder");
        if (items[0] && items[1]) {
          const HRESULT c = IShellFolder_CompareIDs(subfolder, 0, items[0], items[1]);
          check((short)HRESULT_CODE(c) != 0, "CompareIDs tells two items apart");
        }
        for (int i = 0; i < 3; ++i) ILFree(items[i]);
        if (en) IEnumIDList_Release(en);
        /* a data object for the files */
        IEnumIDList *en2 = NULL;
        IShellFolder_EnumObjects(subfolder, NULL, SHCONTF_NONFOLDERS, &en2);
        LPITEMIDLIST sel[2] = {0};
        for (int i = 0; en2 && i < 2 && IEnumIDList_Next(en2, 1, &sel[i], &got) == S_OK; ++i) {}
        IDataObject *data = NULL;
        check(sel[0] && sel[1] && SUCCEEDED(IShellFolder_GetUIObjectOf(subfolder, NULL, 2, (LPCITEMIDLIST *)sel, &IID_IDataObject, NULL, (void **)&data)) && data, "GetUIObjectOf(IDataObject)");
        if (data) {
          FORMATETC fmt = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
          STGMEDIUM m;
          check(SUCCEEDED(IDataObject_GetData(data, &fmt, &m)), "GetData(CF_HDROP)");
          if (m.hGlobal) {
            HDROP drop = (HDROP)m.hGlobal;
            check(DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) == 2, "DragQueryFile counts");
            WCHAR dropped[MAX_PATH];
            check(DragQueryFileW(drop, 0, dropped, MAX_PATH) > 0 && wcsstr(dropped, L"enumdir") != NULL, "DragQueryFile gives a path");
            char narrow[MAX_PATH];
            check(DragQueryFileA(drop, 1, narrow, MAX_PATH) > 0 && strstr(narrow, "enumdir") != NULL, "DragQueryFileA");
            GlobalFree(m.hGlobal);
          }
          IDataObject_Release(data);
        }
        for (int i = 0; i < 2; ++i) ILFree(sel[i]);
        if (en2) IEnumIDList_Release(en2);
        IShellFolder_Release(subfolder);
      }
      ILFree(subpidl);
      DeleteFileW(f1); DeleteFileW(f2); RemoveDirectoryW(d1); RemoveDirectoryW(sub);
      IShellFolder_Release(folder);
    }
    ILFree(pidl);
  }
  /* parsing by name */
  PIDLIST_ABSOLUTE parsed = NULL;
  SFGAOF have = 0;
  check(SUCCEEDED(SHParseDisplayName(file, NULL, &parsed, SFGAO_FOLDER, &have)) && parsed && !(have & SFGAO_FOLDER), "SHParseDisplayName");
  PWSTR text = NULL;
  check(parsed && SUCCEEDED(SHGetNameFromIDList(parsed, SIGDN_FILESYSPATH, &text)) && !wcsicmp(text, file), "SHGetNameFromIDList(FILESYSPATH)");
  CoTaskMemFree(text);
  text = NULL;
  check(parsed && SUCCEEDED(SHGetNameFromIDList(parsed, SIGDN_PARENTRELATIVEPARSING, &text)) && !wcsicmp(text, L"item.txt"), "SHGetNameFromIDList(PARENTRELATIVEPARSING)");
  CoTaskMemFree(text);
  ILFree(parsed);
  check(FAILED(SHParseDisplayName(L"C:\\Temp\\s32test\\nonexistent.xyz", NULL, &parsed, 0, NULL)), "parsing a missing file fails");
  /* My Computer lists the drive */
  IShellFolder *desktop = NULL;
  check(SUCCEEDED(SHGetDesktopFolder(&desktop)), "desktop folder");
  if (desktop) {
    LPITEMIDLIST computer = NULL;
    SHGetSpecialFolderLocation(NULL, CSIDL_DRIVES, &computer);
    IShellFolder *mycomputer = NULL;
    check(computer && SUCCEEDED(IShellFolder_BindToObject(desktop, computer, NULL, &IID_IShellFolder, (void **)&mycomputer)), "bind My Computer");
    if (mycomputer) {
      IEnumIDList *en = NULL;
      int drives = 0;
      if (SUCCEEDED(IShellFolder_EnumObjects(mycomputer, NULL, SHCONTF_FOLDERS, &en)) && en) {
        LPITEMIDLIST item;
        ULONG got;
        while (IEnumIDList_Next(en, 1, &item, &got) == S_OK) { ++drives; ILFree(item); }
        IEnumIDList_Release(en);
      }
      check(drives >= 1, "My Computer has a drive");
      IShellFolder_Release(mycomputer);
    }
    ILFree(computer);
    IShellFolder_Release(desktop);
  }
  DeleteFileW(file);
}

static void test_shell_items(void) {
  WCHAR file[MAX_PATH], folder[MAX_PATH];
  path_in(file, L"shellitem.txt");
  make_file(file, "data");
  IShellItem *item = NULL;
  check(SUCCEEDED(SHCreateItemFromParsingName(file, NULL, &IID_IShellItem, (void **)&item)) && item, "SHCreateItemFromParsingName");
  if (item) {
    PWSTR text = NULL;
    check(SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &text)) && !wcsicmp(text, file), "item path");
    CoTaskMemFree(text);
    text = NULL;
    check(SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_NORMALDISPLAY, &text)) && !wcsicmp(text, L"shellitem.txt"), "item display name");
    CoTaskMemFree(text);
    SFGAOF a = 0;
    check(IShellItem_GetAttributes(item, SFGAO_FOLDER | SFGAO_STREAM, &a) == S_FALSE && a == SFGAO_STREAM, "item attributes (a file is a stream, not a folder)");
    IShellItem *parent = NULL;
    check(SUCCEEDED(IShellItem_GetParent(item, &parent)) && parent, "item parent");
    if (parent) {
      path_in(folder, L"");
      folder[wcslen(folder) - 1] = 0;
      PWSTR ptext = NULL;
      check(SUCCEEDED(IShellItem_GetDisplayName(parent, SIGDN_FILESYSPATH, &ptext)) && !wcsicmp(ptext, kDir), "the parent is the directory");
      CoTaskMemFree(ptext);
      IShellItem_Release(parent);
    }
    IShellItem *same = NULL;
    int order = 5;
    SHCreateItemFromParsingName(file, NULL, &IID_IShellItem, (void **)&same);
    check(same && IShellItem_Compare(item, same, SICHINT_DISPLAY, &order) == S_OK && order == 0, "equal items compare equal");
    if (same) IShellItem_Release(same);
    /* an array */
    PIDLIST_ABSOLUTE pidl = NULL;
    SHGetIDListFromObject((IUnknown *)item, &pidl);
    check(pidl != NULL, "SHGetIDListFromObject");
    IShellItemArray *array = NULL;
    check(pidl && SUCCEEDED(SHCreateShellItemArrayFromIDLists(1, (PCIDLIST_ABSOLUTE_ARRAY)&pidl, &array)) && array, "item array");
    if (array) {
      DWORD n = 0;
      IShellItemArray_GetCount(array, &n);
      check(n == 1, "array count");
      IShellItemArray_Release(array);
    }
    ILFree(pidl);
    IShellItem_Release(item);
  }
  IShellItem *docs = NULL;
  check(SUCCEEDED(SHGetKnownFolderItem(&FOLDERID_Documents, KF_FLAG_CREATE, NULL, &IID_IShellItem, (void **)&docs)) && docs, "known folder as an item");
  if (docs) {
    SFGAOF a = 0;
    check(IShellItem_GetAttributes(docs, SFGAO_FOLDER, &a) == S_OK, "Documents is a folder");
    IShellItem_Release(docs);
  }
  DeleteFileW(file);
}

static void test_file_operations(void) {
  WCHAR from[1024], to[1024], a[MAX_PATH], b[MAX_PATH];
  BOOL aborted = TRUE;
  path_in(a, L"src.txt");
  make_file(a, "content");
  list_of(from, L"src.txt", NULL, NULL);
  list_of(to, L"copy.txt", NULL, NULL);
  check(fileop(FO_COPY, 0, from, to, &aborted) == 0 && !aborted, "copy a file");
  path_in(b, L"copy.txt");
  check(exists(a) && exists(b), "both files exist after the copy");
  list_of(to, L"moved.txt", NULL, NULL);
  list_of(from, L"copy.txt", NULL, NULL);
  check(fileop(FO_MOVE, 0, from, to, NULL) == 0, "move a file");
  path_in(b, L"moved.txt");
  check(exists(b), "the moved file");
  list_of(from, L"moved.txt", NULL, NULL);
  list_of(to, L"renamed.txt", NULL, NULL);
  check(fileop(FO_RENAME, 0, from, to, NULL) == 0, "rename");
  path_in(b, L"renamed.txt");
  check(exists(b), "the renamed file");
  /* a directory tree */
  WCHAR d[MAX_PATH], sub[MAX_PATH], leaf[MAX_PATH];
  path_in(d, L"tree"); path_in(sub, L"tree\\inner"); path_in(leaf, L"tree\\inner\\leaf.txt");
  CreateDirectoryW(d, NULL); CreateDirectoryW(sub, NULL); make_file(leaf, "leaf");
  list_of(from, L"tree", NULL, NULL);
  list_of(to, L"tree2", NULL, NULL);
  check(fileop(FO_COPY, 0, from, to, NULL) == 0, "copy a tree");
  path_in(leaf, L"tree2\\inner\\leaf.txt");
  check(exists(leaf), "the tree was copied with its content");
  /* into an existing directory */
  path_in(sub, L"dest");
  CreateDirectoryW(sub, NULL);
  list_of(to, L"dest", NULL, NULL);
  list_of(from, L"tree2", L"renamed.txt", NULL);
  check(fileop(FO_COPY, 0, from, to, NULL) == 0, "copy two things into a directory");
  path_in(leaf, L"dest\\tree2\\inner\\leaf.txt");
  path_in(b, L"dest\\renamed.txt");
  check(exists(leaf) && exists(b), "both are inside");
  /* collisions */
  list_of(from, L"src.txt", NULL, NULL);
  list_of(to, L"dest", NULL, NULL);
  fileop(FO_COPY, 0, from, to, NULL);
  check(fileop(FO_COPY, FOF_RENAMEONCOLLISION, from, to, NULL) == 0, "copy with rename on collision");
  path_in(b, L"dest\\src.txt");
  path_in(a, L"dest\\Copy of src.txt");
  check(exists(b) && exists(a), "the collision got a new name");
  /* errors */
  list_of(from, L"missing.txt", NULL, NULL);
  check(fileop(FO_DELETE, 0, from, NULL, NULL) == ERROR_FILE_NOT_FOUND, "delete of a missing file");
  list_of(from, L"missing_dir\\x.txt", NULL, NULL);
  check(fileop(FO_DELETE, 0, from, NULL, NULL) == 0x7C, "delete in a missing directory is DE_INVALIDFILES");
  check(fileop(FO_COPY, 0, L"C:\\Temp\\s32test\\src.txt\0", NULL, NULL) != 0, "copy without a target fails");
  list_of(from, L"src.txt", NULL, NULL);
  check(fileop(FO_COPY, 0, from, from, NULL) == 0x71, "copy onto itself is DE_SAMEFILE");
  list_of(from, L"tree", NULL, NULL);
  list_of(to, L"tree\\inner", NULL, NULL);
  check(fileop(FO_COPY, 0, from, to, NULL) == 0x76, "copy a tree into itself is DE_DESTSUBTREE");
  check(fileop(99, 0, from, to, NULL) == ERROR_INVALID_PARAMETER, "unknown function");
  /* wildcards */
  path_in(a, L"w1.log"); make_file(a, "1");
  path_in(a, L"w2.log"); make_file(a, "2");
  path_in(a, L"w.txt"); make_file(a, "3");
  list_of(from, L"w*.log", NULL, NULL);
  check(fileop(FO_DELETE, 0, from, NULL, NULL) == 0, "delete by wildcard");
  path_in(a, L"w1.log"); path_in(b, L"w.txt");
  check(!exists(a) && exists(b), "only the matching files are gone");
  DeleteFileW(b);
  /* the recycle bin */
  path_in(a, L"recycled.txt");
  make_file(a, "bin me");
  list_of(from, L"recycled.txt", NULL, NULL);
  SHEmptyRecycleBinW(NULL, L"C:\\", SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
  SHQUERYRBINFO info;
  memset(&info, 0, sizeof(info));
  info.cbSize = sizeof(info);
  check(SUCCEEDED(SHQueryRecycleBinW(L"C:\\", &info)) && info.i64NumItems == 0, "the bin starts empty");
  check(fileop(FO_DELETE, FOF_ALLOWUNDO, from, NULL, NULL) == 0 && !exists(a), "delete to the bin");
  info.cbSize = sizeof(info);
  check(SUCCEEDED(SHQueryRecycleBinW(L"C:\\", &info)) && info.i64NumItems == 1 && info.i64Size == 6, "one item of six bytes in the bin");
  check(SUCCEEDED(SHEmptyRecycleBinW(NULL, L"C:\\", SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND)), "empty the bin");
  info.cbSize = sizeof(info);
  check(SUCCEEDED(SHQueryRecycleBinW(L"C:\\", &info)) && info.i64NumItems == 0, "the bin is empty again");
  /* directories */
  WCHAR deep[MAX_PATH];
  path_in(deep, L"x\\y\\z");
  check(SHCreateDirectoryExW(NULL, deep, NULL) == ERROR_SUCCESS && is_dir(deep), "SHCreateDirectoryEx makes parents");
  check(SHCreateDirectoryExW(NULL, deep, NULL) == ERROR_ALREADY_EXISTS, "again is ERROR_ALREADY_EXISTS");
  check(SHCreateDirectoryExW(NULL, L"relative", NULL) == ERROR_BAD_PATHNAME, "a relative path");
  list_of(from, L"x", L"dest", L"tree");
  fileop(FO_DELETE, 0, from, NULL, NULL);
  list_of(from, L"tree2", L"src.txt", L"renamed.txt");
  fileop(FO_DELETE, 0, from, NULL, NULL);
  path_in(a, L"x"); path_in(b, L"dest");
  check(!exists(a) && !exists(b), "deleted trees");
  /* names */
  WCHAR unique[MAX_PATH];
  path_in(a, L"unique.txt");
  make_file(a, "u");
  check(PathYetAnotherMakeUniqueName(unique, a, NULL, L"unique.txt") && wcsstr(unique, L"unique (2).txt"), "unique name");
  DeleteFileW(a);
  WCHAR spec[32] = L"a:b*c?d.txt";
  check((PathCleanupSpec(NULL, spec) & PCS_REPLACEDCHAR) && !wcspbrk(spec, L":*?"), "PathCleanupSpec");
  WCHAR q[MAX_PATH] = L"relative.txt";
  PathQualify(q);
  check(q[1] == ':', "PathQualify makes a full path");
  check(PathIsExe(L"C:\\x\\prog.exe") && !PathIsExe(L"C:\\x\\doc.txt"), "PathIsExe");
}

static void set_class_command(const WCHAR *extension, const WCHAR *class_name, const WCHAR *command) {
  HKEY key;
  RegCreateKeyExW(HKEY_CLASSES_ROOT, extension, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
  RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)class_name, (DWORD)((wcslen(class_name) + 1) * 2));
  RegCloseKey(key);
  WCHAR path[200];
  swprintf(path, 200, L"%ls\\shell\\open\\command", class_name);
  RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
  RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)command, (DWORD)((wcslen(command) + 1) * 2));
  RegCloseKey(key);
}
static void test_execute(void) {
  WCHAR doc[MAX_PATH];
  path_in(doc, L"doc.tcosx");
  make_file(doc, "document");
  const BOOL have_child = exists(L"C:\\Windows\\System32\\procchild.exe");
  /* nothing is associated yet */
  SHELLEXECUTEINFOW info;
  memset(&info, 0, sizeof(info));
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  info.lpFile = doc;
  info.nShow = SW_HIDE;
  check(!ShellExecuteExW(&info) && (INT_PTR)info.hInstApp == SE_ERR_NOASSOC, "no association for an unknown extension");
  HINSTANCE r = ShellExecuteW(NULL, NULL, L"C:\\Temp\\s32test\\missing.exe", NULL, NULL, SW_HIDE);
  check((INT_PTR)r == SE_ERR_FNF, "a missing file");
  r = ShellExecuteW(NULL, NULL, L"C:\\Temp\\nodir\\missing.exe", NULL, NULL, SW_HIDE);
  check((INT_PTR)r == SE_ERR_PNF, "a missing directory");
  WCHAR found[MAX_PATH];
  check((INT_PTR)FindExecutableW(doc, NULL, found) == SE_ERR_NOASSOC, "FindExecutable without an association");
  if (!have_child) { printf("  (no procchild.exe in System32: the programs are not started)\n"); DeleteFileW(doc); return; }
  /* a program, run by name with parameters */
  memset(&info, 0, sizeof(info));
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  info.lpFile = L"C:\\Windows\\System32\\procchild.exe";
  info.lpParameters = L"exit 23";
  info.nShow = SW_HIDE;
  check(ShellExecuteExW(&info) && (INT_PTR)info.hInstApp > 32 && info.hProcess, "ShellExecuteEx runs a program");
  if (info.hProcess) {
    DWORD code = 0;
    WaitForSingleObject(info.hProcess, 20000);
    GetExitCodeProcess(info.hProcess, &code);
    check(code == 23, "its exit code");
    CloseHandle(info.hProcess);
  }
  /* an association */
  set_class_command(L".tcosx", L"tcosxfile", L"\"C:\\Windows\\System32\\procchild.exe\" exit 17 \"%1\"");
  memset(&info, 0, sizeof(info));
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  info.lpFile = doc;
  info.nShow = SW_HIDE;
  check(ShellExecuteExW(&info) && (INT_PTR)info.hInstApp > 32 && info.hProcess, "a file runs through its association");
  if (info.hProcess) {
    DWORD code = 0;
    WaitForSingleObject(info.hProcess, 20000);
    GetExitCodeProcess(info.hProcess, &code);
    check(code == 17, "the associated program ran");
    CloseHandle(info.hProcess);
  }
  check((INT_PTR)FindExecutableW(doc, NULL, found) > 32 && !wcsicmp(found, L"C:\\Windows\\System32\\procchild.exe"), "FindExecutable finds the program");
  WCHAR type[80];
  SHFILEINFOW sfi;
  memset(&sfi, 0, sizeof(sfi));
  SHGetFileInfoW(doc, 0, &sfi, sizeof(sfi), SHGFI_TYPENAME | SHGFI_DISPLAYNAME);
  wcscpy(type, sfi.szTypeName);
  check(!wcscmp(sfi.szDisplayName, L"doc.tcosx") && type[0], "SHGetFileInfo names");
  SHDeleteKeyW(HKEY_CLASSES_ROOT, L".tcosx");
  SHDeleteKeyW(HKEY_CLASSES_ROOT, L"tcosxfile");
  DeleteFileW(doc);
}

static void test_file_info(void) {
  WCHAR me[MAX_PATH];
  GetModuleFileNameW(NULL, me, MAX_PATH);
  SHFILEINFOW sfi;
  memset(&sfi, 0xcf, sizeof(sfi));
  const DWORD_PTR exetype = SHGetFileInfoW(me, 0, &sfi, sizeof(sfi), SHGFI_EXETYPE);
  check(LOWORD(exetype) == ('P' | ('E' << 8)), "this program is a PE");
  memset(&sfi, 0, sizeof(sfi));
  check(SHGetFileInfoW(L"C:\\Temp", 0, &sfi, sizeof(sfi), SHGFI_TYPENAME | SHGFI_DISPLAYNAME | SHGFI_ATTRIBUTES) != 0 && !wcscmp(sfi.szTypeName, L"File folder") && !wcscmp(sfi.szDisplayName, L"Temp") &&
        (sfi.dwAttributes & SFGAO_FOLDER), "a folder");
  check(SHGetFileInfoW(L"C:\\Temp\\s32test\\nonexistent", 0, &sfi, sizeof(sfi), SHGFI_TYPENAME) == 0, "a missing file");
  check(SHGetFileInfoW(L"C:\\Temp\\s32test\\nonexistent.txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) != 0 && sfi.szTypeName[0], "SHGFI_USEFILEATTRIBUTES does not look at the disk");
  DWORD binary = 99;
  check(GetBinaryTypeW(me, &binary) && binary == SCS_64BIT_BINARY, "GetBinaryType of this program");
  WCHAR text[MAX_PATH];
  path_in(text, L"text.txt");
  make_file(text, "not a program");
  check(!GetBinaryTypeW(text, &binary) && GetLastError() == ERROR_BAD_EXE_FORMAT, "GetBinaryType of a text file");
  DeleteFileW(text);
  check(IsUserAnAdmin() == IsUserAnAdmin(), "IsUserAnAdmin answers");
  check(DriveType(2) == DRIVE_FIXED && DriveType(30) == DRIVE_NO_ROOT_DIR, "DriveType");
  check(IsLFNDriveW(L"C:\\"), "IsLFNDrive");
  HRESULT hr = SetCurrentProcessExplicitAppUserModelID(L"TCOS.Test.App");
  PWSTR id = NULL;
  check(hr == S_OK && SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&id)) && id && !wcscmp(id, L"TCOS.Test.App"), "AppUserModelID");
  CoTaskMemFree(id);
  DLLVERSIONINFO v = {sizeof(v)};
  { HRESULT (WINAPI *get)(DLLVERSIONINFO *) = (void *)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "DllGetVersion"); check(get && get(&v) == S_OK && v.dwMajorVersion >= 6, "DllGetVersion"); }
}

static void test_shared_and_notify(void) {
  const char text[] = "shared data";
  HANDLE block = SHAllocShared(text, sizeof(text), GetCurrentProcessId());
  check(block != NULL, "SHAllocShared");
  if (block) {
    char *p = SHLockShared(block, GetCurrentProcessId());
    check(p && !strcmp(p, text), "SHLockShared reads the data");
    if (p) { p[0] = 'S'; SHUnlockShared(p); }
    p = SHLockShared(block, GetCurrentProcessId());
    check(p && p[0] == 'S', "and the change is kept");
    if (p) SHUnlockShared(p);
    check(SHFreeShared(block, GetCurrentProcessId()), "SHFreeShared");
  }
  IMalloc *malloc_ = NULL;
  check(SHGetMalloc(&malloc_) == S_OK && malloc_, "SHGetMalloc");
  if (malloc_) {
    void *m = IMalloc_Alloc(malloc_, 100);
    check(m && IMalloc_GetSize(malloc_, m) >= 100, "IMalloc");
    IMalloc_Free(malloc_, m);
  }
  /* change notifications and notification icons do not fail without a window system */
  SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH, L"C:\\Temp", NULL);
  NOTIFYICONDATAW nid;
  memset(&nid, 0, sizeof(nid));
  nid.cbSize = sizeof(nid);
  nid.hWnd = (HWND)0x1234;
  nid.uID = 1;
  nid.uFlags = NIF_TIP;
  wcscpy(nid.szTip, L"tip");
  check(Shell_NotifyIconW(NIM_ADD, &nid), "NIM_ADD");
  check(!Shell_NotifyIconW(NIM_ADD, &nid), "NIM_ADD twice fails");
  check(Shell_NotifyIconW(NIM_MODIFY, &nid), "NIM_MODIFY");
  check(Shell_NotifyIconW(NIM_DELETE, &nid), "NIM_DELETE");
  check(!Shell_NotifyIconW(NIM_DELETE, &nid), "NIM_DELETE of nothing fails");
}

static void test_links_and_com(void) {
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  check(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) == S_FALSE, "CoInitialize twice");
  CoUninitialize();
  check(CoInitializeEx(NULL, COINIT_MULTITHREADED) == RPC_E_CHANGED_MODE, "another mode is refused");
  WCHAR text[64];
  check(StringFromGUID2(&CLSID_ShellLink, text, 64) == 39 && !wcscmp(text, L"{00021401-0000-0000-C000-000000000046}"), "StringFromGUID2");
  CLSID c;
  check(SUCCEEDED(CLSIDFromString(text, &c)) && IsEqualGUID(&c, &CLSID_ShellLink), "CLSIDFromString");
  check(CLSIDFromString(L"{not a guid}", &c) == CO_E_CLASSSTRING, "a bad string");
  GUID g1, g2;
  CoCreateGuid(&g1); CoCreateGuid(&g2);
  check(!IsEqualGUID(&g1, &g2) && (g1.Data3 >> 12) == 4, "CoCreateGuid is a random GUID");
  IBindCtx *bc = NULL;
  check(SUCCEEDED(CreateBindCtx(0, &bc)) && bc, "CreateBindCtx");
  if (bc) {
    BIND_OPTS2 o;
    memset(&o, 0, sizeof(o));
    o.cbStruct = sizeof(o);
    check(SUCCEEDED(IBindCtx_GetBindOptions(bc, (BIND_OPTS *)&o)) && o.grfMode == STGM_READWRITE, "bind options");
    IBindCtx_Release(bc);
  }
  IStream *s = NULL;
  check(SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &s)) && s, "CreateStreamOnHGlobal");
  if (s) {
    ULONG done = 0;
    char buf[16] = {0};
    LARGE_INTEGER zero = {0};
    IStream_Write(s, "streamed", 8, &done);
    IStream_Seek(s, zero, STREAM_SEEK_SET, NULL);
    check(done == 8 && SUCCEEDED(IStream_Read(s, buf, 15, &done)) && done == 8 && !strcmp(buf, "streamed"), "stream write and read");
    IStream_Release(s);
  }
  /* a shortcut */
  WCHAR lnk[MAX_PATH], target[MAX_PATH];
  path_in(lnk, L"link.lnk");
  path_in(target, L"link_target.txt");
  make_file(target, "target");
  IShellLinkW *link = NULL;
  HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
  check(hr == S_OK && link, "CoCreateInstance(CLSID_ShellLink)");
  if (link) {
    IPersistFile *file = NULL;
    check(SUCCEEDED(IShellLinkW_SetPath(link, target)) && SUCCEEDED(IShellLinkW_SetArguments(link, L"-x \"y z\"")) && SUCCEEDED(IShellLinkW_SetDescription(link, L"A test link")) &&
          SUCCEEDED(IShellLinkW_SetWorkingDirectory(link, L"C:\\Temp")) && SUCCEEDED(IShellLinkW_SetShowCmd(link, SW_SHOWMINIMIZED)) && SUCCEEDED(IShellLinkW_SetIconLocation(link, L"C:\\Windows\\System32\\shell32.dll", 7)), "set link fields");
    check(SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)) && file, "link is persistent");
    if (file) {
      check(SUCCEEDED(IPersistFile_Save(file, lnk, TRUE)) && exists(lnk), "save the link");
      IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
  }
  link = NULL;
  CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
  if (link) {
    IPersistFile *file = NULL;
    IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
    check(file && SUCCEEDED(IPersistFile_Load(file, lnk, STGM_READ)), "load the link");
    WCHAR out[MAX_PATH];
    int show = 0, icon = 0;
    check(SUCCEEDED(IShellLinkW_GetPath(link, out, MAX_PATH, NULL, 0)) && !wcsicmp(out, target), "link target");
    check(SUCCEEDED(IShellLinkW_GetArguments(link, out, MAX_PATH)) && !wcscmp(out, L"-x \"y z\""), "link arguments");
    check(SUCCEEDED(IShellLinkW_GetDescription(link, out, MAX_PATH)) && !wcscmp(out, L"A test link"), "link description");
    check(SUCCEEDED(IShellLinkW_GetWorkingDirectory(link, out, MAX_PATH)) && !wcsicmp(out, L"C:\\Temp"), "link working directory");
    check(SUCCEEDED(IShellLinkW_GetShowCmd(link, &show)) && show == SW_SHOWMINIMIZED, "link show command");
    check(SUCCEEDED(IShellLinkW_GetIconLocation(link, out, MAX_PATH, &icon)) && icon == 7 && wcsstr(out, L"shell32.dll"), "link icon");
    check(IShellLinkW_Resolve(link, NULL, 0) == S_OK, "the target resolves");
    IShellLinkA *linka = NULL;
    char narrow[MAX_PATH];
    check(SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IShellLinkA, (void **)&linka)) && linka && SUCCEEDED(IShellLinkA_GetDescription(linka, narrow, MAX_PATH)) && !strcmp(narrow, "A test link"), "the ANSI interface");
    if (linka) IShellLinkA_Release(linka);
    if (file) IPersistFile_Release(file);
    IShellLinkW_Release(link);
  }
  /* garbage is refused, not crashed on */
  WCHAR garbage[MAX_PATH];
  path_in(garbage, L"garbage.lnk");
  make_file(garbage, "this is not a shortcut at all, only text, long enough to hold a header");
  CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
  if (link) {
    IPersistFile *file = NULL;
    IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
    check(file && FAILED(IPersistFile_Load(file, garbage, STGM_READ)), "a text file is not a link");
    if (file) IPersistFile_Release(file);
    IShellLinkW_Release(link);
  }
  /* every prefix of a good link is refused too (truncation) */
  HANDLE f = CreateFileW(lnk, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f != INVALID_HANDLE_VALUE) {
    BYTE data[1024];
    DWORD got = 0;
    ReadFile(f, data, sizeof(data), &got, NULL);
    CloseHandle(f);
    int refused = 0, tried = 0;
    for (DWORD cut = 0; cut + 1 < got; cut += 7) {
      WCHAR cutfile[MAX_PATH];
      path_in(cutfile, L"cut.lnk");
      HANDLE w = CreateFileW(cutfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
      DWORD done;
      WriteFile(w, data, cut, &done, NULL);
      CloseHandle(w);
      CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
      IPersistFile *file = NULL;
      IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
      ++tried;
      if (FAILED(IPersistFile_Load(file, cutfile, STGM_READ))) ++refused;
      IPersistFile_Release(file);
      IShellLinkW_Release(link);
      DeleteFileW(cutfile);
    }
    check(tried > 5 && refused == tried, "truncated links are refused");
    /* bit flips must not crash (whether they are refused or not) */
    for (DWORD at = 0; at < got; at += 3) {
      BYTE copy[1024];
      memcpy(copy, data, got);
      copy[at] ^= 0xFF;
      WCHAR flipped[MAX_PATH];
      path_in(flipped, L"flip.lnk");
      HANDLE w = CreateFileW(flipped, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
      DWORD done;
      WriteFile(w, copy, got, &done, NULL);
      CloseHandle(w);
      CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
      IPersistFile *file = NULL;
      IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
      IPersistFile_Load(file, flipped, STGM_READ);
      IPersistFile_Release(file);
      IShellLinkW_Release(link);
      DeleteFileW(flipped);
    }
    check(1, "bit flips did not crash");
  }
  DeleteFileW(lnk); DeleteFileW(target); DeleteFileW(garbage);
  CoUninitialize();
}

int main(void) {
  CreateDirectoryW(L"C:\\Temp", NULL);
  CreateDirectoryW(kDir, NULL);
  test_command_lines();
  test_known_folders();
  test_id_lists();
  test_shell_items();
  test_file_operations();
  test_execute();
  test_file_info();
  test_shared_and_notify();
  test_links_and_com();
  RemoveDirectoryW(kDir);
  printf("shell32test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
