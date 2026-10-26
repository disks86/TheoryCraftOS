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

/* A Windows program for kernel32.dll and ntdll.dll of TheoryCraftOS, built with mingw (see tests/windows/build.sh) against the usual import library:
 *   x86_64-w64-mingw32-gcc -nostdlib -O1 -Wl,-e,start -Wl,--subsystem,console -o k32test.exe k32test.c -lkernel32
 * It checks a good part of the Win32 API from the inside and prints "k32test: N checks, M failed". The exit code is the number of failures. */
#include <windows.h>

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *p = a, *q = b; for (; n; --n, ++p, ++q) if (*p != *q) return *p < *q ? -1 : 1; return 0; }

static int g_checks, g_failed;
static void out(const char *s) { DWORD n; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)lstrlenA(s), &n, NULL); }
static void number(unsigned long v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); out(b + i); }
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  out("FAIL: ");
  out(what);
  out(" (error ");
  number(GetLastError());
  out(")\n");
}
static int same(const char *a, const char *b) { return lstrcmpA(a, b) == 0; }

static void test_memory(void) {
  HANDLE heap = GetProcessHeap();
  unsigned char *p = HeapAlloc(heap, 0, 1000);
  check(p != NULL && HeapSize(heap, 0, p) >= 1000, "HeapAlloc");
  for (int i = 0; i < 1000; ++i) p[i] = (unsigned char)i;
  p = HeapReAlloc(heap, 0, p, 100000);
  int kept = p != NULL;
  for (int i = 0; kept && i < 1000; ++i) kept = p[i] == (unsigned char)i;
  check(kept, "HeapReAlloc keeps the contents");
  check(HeapFree(heap, 0, p), "HeapFree");
  void *blocks[200];
  for (int i = 0; i < 200; ++i) { blocks[i] = HeapAlloc(heap, HEAP_ZERO_MEMORY, 16 + (DWORD)i * 37); }
  int zeroed = 1, distinct = 1;
  for (int i = 0; i < 200; ++i) { unsigned char *b = blocks[i]; zeroed &= b && b[0] == 0 && b[16 + i * 37 - 1] == 0; for (int j = 0; j < i; ++j) distinct &= blocks[i] != blocks[j]; }
  check(zeroed && distinct, "200 zeroed, distinct allocations");
  for (int i = 0; i < 200; i += 2) HeapFree(heap, 0, blocks[i]);
  for (int i = 1; i < 200; i += 2) HeapFree(heap, 0, blocks[i]);
  void *big = HeapAlloc(heap, 0, 3 * 1024 * 1024);
  check(big != NULL, "a 3 MiB block (a segment of its own)");
  HeapFree(heap, 0, big);
  HANDLE mine = HeapCreate(0, 0, 0);
  void *q = HeapAlloc(mine, 0, 64);
  check(mine != NULL && q != NULL && HeapDestroy(mine), "HeapCreate, HeapAlloc, HeapDestroy");
  HLOCAL local = LocalAlloc(LMEM_ZEROINIT, 32);
  check(local != NULL && LocalFree(local) == NULL, "LocalAlloc, LocalFree");

  unsigned char *v = VirtualAlloc(NULL, 10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  check(v != NULL && ((ULONG_PTR)v & 0xFFFF) == 0, "VirtualAlloc, 64 KiB aligned");
  v[0] = 1; v[9999] = 2;
  MEMORY_BASIC_INFORMATION info;
  check(VirtualQuery(v, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE && info.RegionSize >= 10000, "VirtualQuery");
  DWORD old = 0;
  check(VirtualProtect(v, 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE, "VirtualProtect");
  check(VirtualQuery(v, &info, sizeof(info)) && info.Protect == PAGE_READONLY, "the new protection shows");
  check(VirtualFree(v, 0, MEM_RELEASE), "VirtualFree");
  check(VirtualQuery(v, &info, sizeof(info)) && info.State == MEM_FREE, "and the pages are free");
}

static void test_process(void) {
  check(GetCurrentProcessId() != 0 && GetCurrentThreadId() != 0, "process and thread ids");
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
  check(n > 4 && path[1] == ':', "GetModuleFileName");
  check(GetModuleHandleA(NULL) == (HMODULE)0x140000000ull, "the program's module handle is its image base");
  HMODULE k32 = GetModuleHandleA("KERNEL32.DLL");
  HMODULE nt = GetModuleHandleA("ntdll");
  check(k32 != NULL && nt != NULL && k32 != nt, "GetModuleHandle finds kernel32 and ntdll (any case, with or without .dll)");
  check(GetProcAddress(k32, "GetTickCount") == (FARPROC)GetTickCount, "GetProcAddress by name finds the very function the program imported");
  check(GetProcAddress(k32, "NoSuchFunction") == NULL && GetLastError() == ERROR_PROC_NOT_FOUND, "GetProcAddress of nothing");
  check(GetProcAddress(nt, "RtlUserThreadStart") != NULL, "an export of ntdll");
  check(LoadLibraryA("kernel32.dll") == k32 && LoadLibraryA("nothing.dll") == NULL && GetLastError() == ERROR_MOD_NOT_FOUND, "LoadLibrary of what is loaded, and of what is not");
  check(GetCommandLineA() != NULL && lstrlenA(GetCommandLineA()) > 4, "GetCommandLine");
  check(GetCommandLineW()[1] == (WCHAR)GetCommandLineA()[1], "the wide and the narrow command line agree");
  char value[200];
  check(GetEnvironmentVariableA("PATH", value, sizeof(value)) > 5, "GetEnvironmentVariable PATH");
  check(GetEnvironmentVariableA("SystemRoot", value, sizeof(value)) == 10 && same(value, "C:\\Windows"), "SystemRoot");
  check(GetEnvironmentVariableA("path", value, sizeof(value)) > 5, "variable names ignore case");
  check(GetEnvironmentVariableA("NO_SUCH_VARIABLE", value, sizeof(value)) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND, "a missing variable");
  check(SetEnvironmentVariableA("K32TEST", "hello world") && GetEnvironmentVariableA("K32TEST", value, sizeof(value)) == 11 && same(value, "hello world"), "SetEnvironmentVariable");
  check(SetEnvironmentVariableA("K32TEST", "again") && GetEnvironmentVariableA("K32TEST", value, sizeof(value)) == 5, "and replacing it");
  check(SetEnvironmentVariableA("K32TEST", NULL) && GetEnvironmentVariableA("K32TEST", value, sizeof(value)) == 0, "and removing it");
  check(GetEnvironmentVariableA("PATH", value, 3) > 3, "a buffer that is too small gets the size needed");
  char expanded[200];
  check(ExpandEnvironmentStringsA("%SystemRoot%\\x", expanded, sizeof(expanded)) == 13 && same(expanded, "C:\\Windows\\x"), "ExpandEnvironmentStrings");
  OSVERSIONINFOA version;
  version.dwOSVersionInfoSize = sizeof(version);
  check(GetVersionExA(&version) && version.dwMajorVersion == 10, "GetVersionEx");
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  check(si.dwPageSize == 4096 && si.dwNumberOfProcessors >= 1 && si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64, "GetSystemInfo");
  check(GetLastError() == GetLastError(), "last error is stable");
  SetLastError(1234);
  check(GetLastError() == 1234, "SetLastError");
  DWORD tls = TlsAlloc();
  check(tls != TLS_OUT_OF_INDEXES && TlsSetValue(tls, (LPVOID)0x1234) && TlsGetValue(tls) == (LPVOID)0x1234, "TLS");
  DWORD tls2 = TlsAlloc();
  check(tls2 != tls && TlsGetValue(tls2) == NULL && TlsFree(tls2) && TlsFree(tls), "more TLS");
  DWORD fls = FlsAlloc(NULL);
  check(fls != FLS_OUT_OF_INDEXES && FlsSetValue(fls, (PVOID)7) && FlsGetValue(fls) == (PVOID)7 && FlsFree(fls), "FLS");
  CRITICAL_SECTION cs;
  InitializeCriticalSection(&cs);
  EnterCriticalSection(&cs);
  EnterCriticalSection(&cs);
  LeaveCriticalSection(&cs);
  LeaveCriticalSection(&cs);
  DeleteCriticalSection(&cs);
  check(1, "critical sections");
  HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
  check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT && SetEvent(event) && WaitForSingleObject(event, 0) == WAIT_OBJECT_0 && CloseHandle(event), "events");
  LONG counter = 5;
  check(InterlockedIncrement(&counter) == 6 && InterlockedCompareExchange(&counter, 10, 6) == 6 && counter == 10, "Interlocked");
  check(CreateThread(NULL, 0, NULL, NULL, 0, NULL) == NULL, "threads are not available");
}

static void test_text(void) {
  WCHAR wide[64];
  char narrow[64];
  const char *text = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";      /* café € 😀 */
  int units = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 64);
  check(units == 10 && wide[3] == 0xE9 && wide[5] == 0x20AC && wide[7] == 0xD83D && wide[8] == 0xDE00, "MultiByteToWideChar (UTF-8)");
  int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow), NULL, NULL);
  check(bytes == lstrlenA(text) + 1 && same(narrow, text), "and back");
  check(MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0) == 10, "the size query");
  check(MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 3) == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "a buffer too small");
  check(lstrcmpiA("Hello", "hELLO") == 0 && lstrcmpA("a", "b") < 0 && CompareStringA(0, NORM_IGNORECASE, "ABC", -1, "abc", -1) == CSTR_EQUAL, "string comparison");
  char copy[16];
  lstrcpynA(copy, "0123456789ABCDEFGHIJ", 8);
  check(same(copy, "0123456"), "lstrcpyn");
  FILETIME ft;
  SYSTEMTIME st = {2024, 2, 0, 29, 13, 45, 30, 250}, back;
  check(SystemTimeToFileTime(&st, &ft) && FileTimeToSystemTime(&ft, &back), "SystemTimeToFileTime");
  check(back.wYear == 2024 && back.wMonth == 2 && back.wDay == 29 && back.wHour == 13 && back.wMinute == 45 && back.wSecond == 30 && back.wMilliseconds == 250 && back.wDayOfWeek == 4, "the leap day 2024-02-29 13:45:30.250 (a Thursday) survives the round trip");
  ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  check(t == 133536879302500000ull, "its FILETIME");
  LARGE_INTEGER a, b, freq;
  check(QueryPerformanceCounter(&a) && QueryPerformanceFrequency(&freq) && freq.QuadPart == 10000000, "QueryPerformanceFrequency");
  Sleep(20);
  QueryPerformanceCounter(&b);
  check(b.QuadPart - a.QuadPart >= 150000, "Sleep(20) takes at least 15 ms");
  DWORD t0 = GetTickCount();
  Sleep(30);
  check(GetTickCount() - t0 >= 25, "GetTickCount moves with Sleep");
  char message[128];
  check(FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, ERROR_FILE_NOT_FOUND, 0, message, sizeof(message), NULL) > 10 && message[0] == 'T', "FormatMessage");
}

static void test_files(void) {
  char cwd[MAX_PATH];
  check(GetCurrentDirectoryA(sizeof(cwd), cwd) >= 3 && cwd[1] == ':', "GetCurrentDirectory");
  check(CreateDirectoryA("C:\\k32dir", NULL), "CreateDirectory");
  check(!CreateDirectoryA("C:\\k32dir", NULL) && GetLastError() == ERROR_ALREADY_EXISTS, "CreateDirectory again");
  check(!CreateDirectoryA("C:\\nowhere\\sub", NULL) && GetLastError() == ERROR_PATH_NOT_FOUND, "CreateDirectory below nothing");
  check((GetFileAttributesA("C:\\k32dir") & FILE_ATTRIBUTE_DIRECTORY) != 0 && GetFileAttributesA("C:\\missing") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND, "GetFileAttributes");

  HANDLE f = CreateFileA("C:\\k32dir\\a.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  check(f != INVALID_HANDLE_VALUE, "CreateFile (new)");
  DWORD n = 0;
  check(WriteFile(f, "hello, ", 7, &n, NULL) && n == 7 && WriteFile(f, "files", 5, &n, NULL) && n == 5, "WriteFile twice");
  LARGE_INTEGER size;
  check(GetFileSizeEx(f, &size) && size.QuadPart == 12 && GetFileSize(f, NULL) == 12, "GetFileSize");
  check(SetFilePointer(f, 0, NULL, FILE_BEGIN) == 0, "SetFilePointer");
  char buffer[32];
  check(ReadFile(f, buffer, sizeof(buffer), &n, NULL) && n == 12 && memcmp(buffer, "hello, files", 12) == 0, "ReadFile reads what was written");
  check(ReadFile(f, buffer, sizeof(buffer), &n, NULL) && n == 0, "ReadFile at the end gives 0 bytes");
  LARGE_INTEGER to, at;
  to.QuadPart = -5;
  check(SetFilePointerEx(f, to, &at, FILE_END) && at.QuadPart == 7 && ReadFile(f, buffer, 5, &n, NULL) && n == 5 && memcmp(buffer, "files", 5) == 0, "SetFilePointerEx from the end");
  check(SetFilePointer(f, 5, NULL, FILE_BEGIN) == 5 && SetEndOfFile(f) && GetFileSize(f, NULL) == 5, "SetEndOfFile");
  BY_HANDLE_FILE_INFORMATION info;
  check(GetFileInformationByHandle(f, &info) && info.nFileSizeLow == 5 && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), "GetFileInformationByHandle");
  check(GetFileType(f) == FILE_TYPE_DISK && FlushFileBuffers(f) && CloseHandle(f), "GetFileType, FlushFileBuffers, CloseHandle");
  check(CreateFileA("C:\\k32dir\\a.txt", GENERIC_READ, 0, NULL, CREATE_NEW, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS, "CREATE_NEW on an existing file");
  f = CreateFileA("C:\\k32dir\\a.txt", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  SetFilePointer(f, 0, NULL, FILE_END);
  WriteFile(f, "!", 1, &n, NULL);
  CloseHandle(f);
  f = CreateFileA("C:\\k32dir\\a.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  check(ReadFile(f, buffer, sizeof(buffer), &n, NULL) && n == 6 && memcmp(buffer, "hello!", 6) == 0, "appended");
  CloseHandle(f);
  check(CreateFileA("C:\\k32dir\\none.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND, "OPEN_EXISTING on nothing");
  f = CreateFileA("C:\\k32dir\\b.txt", GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
  check(f != INVALID_HANDLE_VALUE && GetLastError() == 0, "OPEN_ALWAYS creates");
  CloseHandle(f);
  f = CreateFileA("C:\\k32dir\\b.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  check(f != INVALID_HANDLE_VALUE, "CREATE_ALWAYS replaces");
  CloseHandle(f);
  check(CreateFileA("C:\\k32dir", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE, "a directory is not opened like a file");

  check(CopyFileA("C:\\k32dir\\a.txt", "C:\\k32dir\\c.txt", TRUE), "CopyFile");
  check(!CopyFileA("C:\\k32dir\\a.txt", "C:\\k32dir\\c.txt", TRUE), "CopyFile onto a file");
  check(MoveFileA("C:\\k32dir\\c.txt", "C:\\k32dir\\d.txt") && GetFileAttributesA("C:\\k32dir\\c.txt") == INVALID_FILE_ATTRIBUTES && GetFileAttributesA("C:\\k32dir\\d.txt") != INVALID_FILE_ATTRIBUTES, "MoveFile");
  check(MoveFileExA("C:\\k32dir\\d.txt", "C:\\k32dir\\b.txt", MOVEFILE_REPLACE_EXISTING), "MoveFileEx replacing");

  /* directory listing */
  WIN32_FIND_DATAA fd;
  HANDLE find = FindFirstFileA("C:\\k32dir\\*", &fd);
  int names = 0, saw_a = 0, saw_b = 0, saw_dot = 0;
  if (find != INVALID_HANDLE_VALUE) {
    do { ++names; saw_a |= same(fd.cFileName, "a.txt"); saw_b |= same(fd.cFileName, "b.txt") && fd.nFileSizeLow == 6; saw_dot |= same(fd.cFileName, "."); } while (FindNextFileA(find, &fd));
    check(GetLastError() == ERROR_NO_MORE_FILES, "the listing ends with ERROR_NO_MORE_FILES");
    FindClose(find);
  }
  check(names == 4 && saw_a && saw_b && saw_dot, "FindFirstFile, FindNextFile: ., .., a.txt, b.txt");
  find = FindFirstFileA("C:\\k32dir\\A*.TXT", &fd);
  check(find != INVALID_HANDLE_VALUE && same(fd.cFileName, "a.txt") && !FindNextFileA(find, &fd), "a pattern (case does not matter)");
  FindClose(find);
  check(FindFirstFileA("C:\\k32dir\\*.zip", &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND, "no match");
  check(FindFirstFileA("C:\\nodir\\*", &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND, "no folder");
  WIN32_FIND_DATAW wfd;
  HANDLE wfind = FindFirstFileW(L"C:\\k32dir\\b.*", &wfd);
  check(wfind != INVALID_HANDLE_VALUE && wfd.cFileName[0] == 'b' && wfd.nFileSizeLow == 6, "FindFirstFileW");
  FindClose(wfind);

  /* the current directory and relative names */
  check(SetCurrentDirectoryA("C:\\k32dir") && GetCurrentDirectoryA(sizeof(cwd), cwd) == 9 && same(cwd, "C:\\k32dir"), "SetCurrentDirectory");
  f = CreateFileA("rel.txt", GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
  check(f != INVALID_HANDLE_VALUE, "a relative name");
  CloseHandle(f);
  check(GetFileAttributesA("C:\\k32dir\\rel.txt") != INVALID_FILE_ATTRIBUTES && GetFileAttributesA("..\\k32dir\\.\\rel.txt") != INVALID_FILE_ATTRIBUTES, "and the same file by a path with . and ..");
  char full[MAX_PATH];
  char *part = NULL;
  check(GetFullPathNameA("..\\x\\y.txt", sizeof(full), full, &part) == 10 && same(full, "C:\\x\\y.txt") && part == full + 5, "GetFullPathName");
  check(GetFullPathNameA("/up/../down.txt", sizeof(full), full, NULL) == 11 && same(full, "C:\\down.txt"), "and with slashes");
  check(SetCurrentDirectoryA("C:\\") && !SetCurrentDirectoryA("C:\\missing"), "back to the root");

  check(DeleteFileA("C:\\k32dir\\rel.txt") && DeleteFileA("C:\\k32dir\\a.txt") && DeleteFileA("C:\\k32dir\\b.txt"), "DeleteFile");
  check(!DeleteFileA("C:\\k32dir\\a.txt") && GetLastError() == ERROR_FILE_NOT_FOUND && !DeleteFileA("C:\\k32dir") , "DeleteFile of nothing, of a directory");
  check(RemoveDirectoryA("C:\\k32dir") && !RemoveDirectoryA("C:\\k32dir"), "RemoveDirectory");
  f = CreateFileA("C:\\k32temp.dat", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
  check(f != INVALID_HANDLE_VALUE && GetFileAttributesA("C:\\k32temp.dat") != INVALID_FILE_ATTRIBUTES, "a delete-on-close file exists");
  CloseHandle(f);
  check(GetFileAttributesA("C:\\k32temp.dat") == INVALID_FILE_ATTRIBUTES, "and is gone once closed");
  char tmp[MAX_PATH], name[MAX_PATH];
  check(GetTempPathA(sizeof(tmp), tmp) == 8 && GetTempFileNameA(tmp, "k32", 0, name) != 0 && GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES && DeleteFileA(name), "GetTempPath, GetTempFileName");
  /* wide names with non-ASCII letters */
  f = CreateFileW(L"C:\\caf\x00e9.txt", GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
  check(f != INVALID_HANDLE_VALUE, "a wide name with an accent");
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  WIN32_FIND_DATAA accent;
  HANDLE listing = FindFirstFileA("C:\\caf*", &accent);
  check(listing != INVALID_HANDLE_VALUE && same(accent.cFileName, "caf\xC3\xA9.txt"), "and it is listed in UTF-8");
  if (listing != INVALID_HANDLE_VALUE) FindClose(listing);
  check(DeleteFileW(L"C:\\caf\x00e9.txt"), "and deleted");
}

static void test_console(void) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE), o = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode;
  check(in != INVALID_HANDLE_VALUE && o != INVALID_HANDLE_VALUE && GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE, "standard handles");
  check(GetFileType(o) == FILE_TYPE_CHAR && GetFileType(in) == FILE_TYPE_CHAR, "they are character devices");
  mode = 0;
  check(GetConsoleMode(o, &mode) && mode == (ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT), "GetConsoleMode of the output");
  mode = 0;
  check(GetConsoleMode(in, &mode) && mode == (ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT), "GetConsoleMode of the input");
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  check(GetConsoleScreenBufferInfo(o, &csbi) && csbi.dwSize.X == 80, "GetConsoleScreenBufferInfo");
  DWORD n = 0;
  check(WriteConsoleA(o, "console A\n", 10, &n, NULL) && n == 10, "WriteConsoleA");
  check(WriteConsoleW(o, L"console W \x00e9\n", 12, &n, NULL) && n == 12, "WriteConsoleW");
  char line[64];
  check(ReadFile(in, line, sizeof(line), &n, NULL) && n > 0 && line[n - 1] == '\n', "ReadFile from the keyboard gives a line");
  line[n - 1] = 0;
  out("read: ");
  out(line);
  out("\n");
  check(same(line, "keyboard line"), "it is the line that was typed");
  check(ReadFile(in, line, sizeof(line), &n, NULL) && n == 0, "then end of file");
}

void start(void) {
  out("k32test starting\n");
  test_memory();
  test_process();
  test_text();
  test_files();
  test_console();
  out("k32test: ");
  number((unsigned long)g_checks);
  out(" checks, ");
  number((unsigned long)g_failed);
  out(" failed\n");
  ExitProcess((UINT)g_failed);
}
