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

/* A 32-bit Windows program built the ordinary way (i686-w64-mingw32-gcc, with its C runtime), for tests/wow64_qemu.sh: it runs on the 32-bit
 * ntdll.dll, kernel32.dll and msvcrt.dll of C:\Windows\SysWOW64 (docs/Wow64.md) and checks what a program expects of them - the C runtime's start-up,
 * the heap, files, the environment, thread-local storage, critical sections, threads and waiting. Ends with exit code 42 if every check passed.
 *   i686-w64-mingw32-gcc -O1 -o crt32.exe tests/windows/crt32.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "crt32: ok: %s\n" : "crt32: FAILED: %s\n", what);
  if (ok) ++g_passed; else ++g_failed;
}

static __thread int t_value = 5;      /* thread-local storage of the image (its TLS directory) */
static CRITICAL_SECTION g_lock;
static volatile LONG g_counter;
static DWORD g_slot;

static DWORD WINAPI worker(LPVOID parameter) {
  const int number = (int)(INT_PTR)parameter;
  int fresh = t_value == 5;      /* every thread starts with the image's initial value */
  t_value = 100 + number;
  TlsSetValue(g_slot, (LPVOID)(INT_PTR)(number + 1));
  for (int i = 0; i < 1000; ++i) {
    EnterCriticalSection(&g_lock);
    ++g_counter;
    LeaveCriticalSection(&g_lock);
  }
  /* 10 + the thread's number if all is well; else 100, 200 or 400 (and their sums) say what was not */
  return 10u + (DWORD)number + (fresh ? 0 : 100) + (t_value == 100 + number ? 0 : 200) + ((int)(INT_PTR)TlsGetValue(g_slot) == number + 1 ? 0 : 400);
}

int main(int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[1], "child") == 0) return 7;      /* (started by itself, below: a 32-bit program starting a 32-bit program) */
  if (argc >= 3 && strcmp(argv[1], "event") == 0) return SetEvent((HANDLE)(UINT_PTR)strtoul(argv[2], NULL, 10)) ? 9 : 8;      /* (and with a handle it inherits) */
  printf("crt32: a 32-bit program with a C runtime is running\n");
  check(sizeof(void *) == 4 && argc >= 1 && argv[0] && strstr(argv[0], "crt32") != NULL, "32-bit pointers, and the program's name in argv[0]");

  /* The heap. */
  char *text = malloc(64);
  check(text != NULL && (UINT_PTR)text < 0x7FFF0000u, "malloc gives memory below 2 GiB");
  strcpy(text, "heap");
  text = realloc(text, 100000);
  check(text != NULL && strcmp(text, "heap") == 0, "realloc keeps the contents");
  memset(text, 'x', 99999);
  text[99999] = 0;
  check(strlen(text) == 99999, "a large block can be written");
  free(text);

  /* Formatting. */
  char line[128];
  snprintf(line, sizeof(line), "%d %u %s %5.2f %llx %c", -7, 42u, "str", 3.14159, 0x123456789ABCDEFull, 'z');
  check(strcmp(line, "-7 42 str  3.14 123456789abcdef z") == 0, "snprintf");

  /* The environment, the module, the system. */
  char buffer[MAX_PATH];
  check(GetModuleFileNameA(NULL, buffer, sizeof(buffer)) > 0 && strstr(buffer, "crt32") != NULL, "GetModuleFileName names the program");
  check(SetEnvironmentVariableA("CRT32_TEST", "value") && getenv("PATH") != NULL && GetEnvironmentVariableA("CRT32_TEST", buffer, sizeof(buffer)) == 5 &&
        strcmp(buffer, "value") == 0, "environment variables");
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  check(info.dwPageSize == 4096 && info.dwNumberOfProcessors >= 1, "GetSystemInfo");
  check(GetCurrentProcessId() != 0 && GetCurrentThreadId() != 0, "process and thread ids");
  MEMORY_BASIC_INFORMATION memory;
  check(VirtualQuery(&g_passed, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT && (UINT_PTR)memory.BaseAddress <= (UINT_PTR)&g_passed,
        "VirtualQuery describes the program's data");
  void *pages = VirtualAlloc(NULL, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  check(pages != NULL && (UINT_PTR)pages < 0x7FFF0000u, "VirtualAlloc");
  if (pages) { memset(pages, 1, 0x20000); check(VirtualFree(pages, 0, MEM_RELEASE), "VirtualFree"); }

  /* A file: written, read back, deleted. */
  const char *path = "C:\\Windows\\Temp\\crt32.txt";
  FILE *file = fopen(path, "w");
  check(file != NULL, "fopen for writing");
  if (file) {
    fprintf(file, "line %d\nline %d\n", 1, 2);
    fclose(file);
    file = fopen(path, "r");
    char first[32] = "", second[32] = "";
    const int read = file && fgets(first, sizeof(first), file) && fgets(second, sizeof(second), file);
    check(read && strcmp(first, "line 1\n") == 0 && strcmp(second, "line 2\n") == 0, "the file reads back");
    if (file) fclose(file);
    check(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && DeleteFileA(path) && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES, "the file is there, then deleted");
  }

  /* Threads: each has its own thread-local storage; they share a counter under a lock. */
  InitializeCriticalSection(&g_lock);
  g_slot = TlsAlloc();
  HANDLE threads[4];
  int started = 1;
  for (int i = 0; i < 4; ++i) {
    threads[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
    started = started && threads[i] != NULL;
  }
  check(started, "CreateThread");
  if (started) {
    check(WaitForMultipleObjects(4, threads, TRUE, 20000) == WAIT_OBJECT_0, "WaitForMultipleObjects for the threads");
    int codes = 1;
    for (int i = 0; i < 4; ++i) {
      DWORD code = 0;
      const BOOL got = GetExitCodeThread(threads[i], &code);
      if (!got || code != 10u + (DWORD)i) printf("crt32: thread %d: exit code %lu (asked: %d, error %lu)\n", i, (unsigned long)code, (int)got, (unsigned long)GetLastError());
      codes = codes && got && code == 10u + (DWORD)i;
      CloseHandle(threads[i]);
    }
    check(codes, "each thread had its own thread-local storage and its own exit code");
    check(g_counter == 4000 && t_value == 5, "the counter is whole, and this thread's storage untouched");
  }
  HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
  check(event != NULL && WaitForSingleObject(event, 10) == WAIT_TIMEOUT && SetEvent(event) && WaitForSingleObject(event, 10) == WAIT_OBJECT_0, "an event");
  DWORD before = GetTickCount();
  Sleep(30);
  check(GetTickCount() - before >= 20, "Sleep");

  /* A library loaded while the program runs, by its full path, from a folder that is not the system's (plug32.dll: tests/windows/plug32.c). */
  CreateDirectoryA("C:\\Windows\\Temp\\crt32.tmp", NULL);
  check(CopyFileA("C:\\plug32.dll", "C:\\Windows\\Temp\\crt32.tmp\\plug32.dll", FALSE), "the library is copied to a temporary folder");
  HMODULE plugin = LoadLibraryExA("C:\\Windows\\Temp\\crt32.tmp\\plug32.dll", NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  check(plugin != NULL, "LoadLibraryEx by full path");
  if (plugin) {
    void (__cdecl *entry)(HWND, int, char *, void **, void *) = (void (__cdecl *)(HWND, int, char *, void **, void *))(void *)GetProcAddress(plugin, "_");
    int (__stdcall *twice)(int) = (int (__stdcall *)(int))(void *)GetProcAddress(plugin, "Twice@4");
    int answer = 0;
    if (entry) entry(NULL, 24, NULL, NULL, &answer);
    check(entry != NULL && answer == 1025, "its cdecl function is found by GetProcAddress and called (and its entry point had run)");
    check(twice != NULL && twice(21) == 42, "and its stdcall function");
    check(FreeLibrary(plugin), "FreeLibrary");
  }
  DeleteFileA("C:\\Windows\\Temp\\crt32.tmp\\plug32.dll");
  RemoveDirectoryA("C:\\Windows\\Temp\\crt32.tmp");

  /* Renaming: a file that was just written, onto a new name and over an existing file (the request names the file with a handle in it). */
  CreateDirectoryA("C:\\Windows\\Temp\\crt32.move", NULL);
  FILE *moved = fopen("C:\\Windows\\Temp\\crt32.move\\a.tmp", "wb");
  if (moved) { fputs("renamed", moved); fclose(moved); }
  check(moved != NULL && MoveFileA("C:\\Windows\\Temp\\crt32.move\\a.tmp", "C:\\Windows\\Temp\\crt32.move\\b.bin") &&
        GetFileAttributesA("C:\\Windows\\Temp\\crt32.move\\a.tmp") == INVALID_FILE_ATTRIBUTES && GetFileAttributesA("C:\\Windows\\Temp\\crt32.move\\b.bin") != INVALID_FILE_ATTRIBUTES,
        "MoveFile renames a file");
  moved = fopen("C:\\Windows\\Temp\\crt32.move\\c.tmp", "wb");
  if (moved) { fputs("second", moved); fclose(moved); }
  char moved_text[16] = {0};
  const BOOL replaced = MoveFileExA("C:\\Windows\\Temp\\crt32.move\\c.tmp", "C:\\Windows\\Temp\\crt32.move\\b.bin", MOVEFILE_REPLACE_EXISTING);
  moved = fopen("C:\\Windows\\Temp\\crt32.move\\b.bin", "rb");
  if (moved) { fread(moved_text, 1, 15, moved); fclose(moved); }
  check(replaced && strcmp(moved_text, "second") == 0, "MoveFileEx replaces an existing file");
  DeleteFileA("C:\\Windows\\Temp\\crt32.move\\b.bin");
  RemoveDirectoryA("C:\\Windows\\Temp\\crt32.move");

  /* A system library asked for by the path of System32 is the 32-bit one (of SysWOW64). */
  HMODULE system_library = LoadLibraryA("C:\\Windows\\System32\\version.dll");
  check(system_library != NULL && GetProcAddress(system_library, "GetFileVersionInfoSizeW") != NULL, "LoadLibrary of a System32 path loads the 32-bit library");
  if (system_library) FreeLibrary(system_library);

  /* %p is a 32-bit pointer. */
  char pointer_text[32];
  sprintf(pointer_text, "%p", (void *)0x1234ABCD);
  check(strcmp(pointer_text, "1234ABCD") == 0, "printf's %p prints eight digits");

  /* Another program: itself, with an argument. */
  char own_path[MAX_PATH], command_line[MAX_PATH + 16];
  STARTUPINFOA startup;
  PROCESS_INFORMATION child;
  DWORD child_code = 0;
  memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  GetModuleFileNameA(NULL, own_path, MAX_PATH);
  sprintf(command_line, "\"%s\" child", own_path);
  const BOOL child_started = CreateProcessA(NULL, command_line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child);
  check(child_started && WaitForSingleObject(child.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &child_code) && child_code == 7,
        "CreateProcess starts a 32-bit program, which gets its argument and gives its exit code");
  if (child_started) { CloseHandle(child.hThread); CloseHandle(child.hProcess); }

  /* What Chromium's programs do when they start (Google's updater stopped at each of these). The loader lock is where a 32-bit program looks for it: its own
     PEB at FS:[0x30], offset 0xA0, a critical section whose owner can be read. */
  {
    const BYTE *peb = (const BYTE *)__readfsdword(0x30);
    const RTL_CRITICAL_SECTION *loader_lock = *(RTL_CRITICAL_SECTION *const *)(peb + 0xA0);
    check(loader_lock != NULL && (DWORD)(UINT_PTR)loader_lock->OwningThread != GetCurrentThreadId(), "the PEB names the loader lock, which this thread does not hold");
  }
  /* A named pipe read says how many bytes it read (the status block is the 32-bit one, 8 bytes: nothing behind it is written). */
  {
    const HANDLE server = CreateNamedPipeA("\\\\.\\pipe\\crt32-test", PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, 1, 512, 512, 0, NULL);
    const HANDLE client = CreateFileA("\\\\.\\pipe\\crt32-test", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    struct { DWORD read; DWORD guard; } counted = {0, 0x5AFE5AFE};
    char message[36], answer[36];
    DWORD written = 0, mode = PIPE_READMODE_MESSAGE;
    memset(message, 'm', sizeof(message));
    memset(answer, 0, sizeof(answer));
    const BOOL sent = server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE && SetNamedPipeHandleState(client, &mode, NULL, NULL) &&
                      WriteFile(client, message, sizeof(message), &written, NULL) && written == sizeof(message);
    const BOOL got = sent && ReadFile(server, answer, sizeof(answer), &counted.read, NULL);
    check(got && counted.read == sizeof(message) && counted.guard == 0x5AFE5AFE && memcmp(answer, message, sizeof(message)) == 0, "a pipe read gives the message and its length");
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
  }
  /* A handle opened to append (and not to write elsewhere) writes at the end, whatever the file's other handles did. */
  {
    char path[MAX_PATH], back[16];
    DWORD done = 0;
    GetTempPathA(MAX_PATH, path);
    strcat(path, "crt32-append.txt");
    HANDLE first = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    BOOL ok = first != INVALID_HANDLE_VALUE && WriteFile(first, "one", 3, &done, NULL);
    HANDLE append = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    ok = ok && append != INVALID_HANDLE_VALUE && WriteFile(append, "two", 3, &done, NULL) && WriteFile(first, "1", 1, &done, NULL) && WriteFile(append, "3", 1, &done, NULL);
    if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
    if (append != INVALID_HANDLE_VALUE) CloseHandle(append);
    first = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    memset(back, 0, sizeof(back));
    ok = ok && first != INVALID_HANDLE_VALUE && ReadFile(first, back, sizeof(back) - 1, &done, NULL);
    /* ("one"; "two" appended; "1" by the first handle at its own place, over the 't'; "3" appended) */
    check(ok && done == 7 && strcmp(back, "one1wo3") == 0, "a handle opened to append writes at the end");
    if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
    DeleteFileA(path);
  }
  /* A child that inherits only the handles of a list (how a crash handler is started): it starts, and gets the handle. */
  {
    SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), NULL, TRUE};
    HANDLE listed = CreateEventA(&inheritable, TRUE, FALSE, NULL);
    SIZE_T size = 0;
    struct { STARTUPINFOA info; LPPROC_THREAD_ATTRIBUTE_LIST list; } extended;
    PROCESS_INFORMATION made;
    DWORD code = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = malloc(size);
    memset(&extended, 0, sizeof(extended));
    extended.info.cb = sizeof(extended);
    extended.list = list;
    sprintf(command_line, "\"%s\" event %lu", own_path, (unsigned long)(UINT_PTR)listed);
    const BOOL started = list && InitializeProcThreadAttributeList(list, 1, 0, &size) && UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &listed, sizeof(listed), NULL, NULL) &&
                         CreateProcessA(NULL, command_line, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &extended.info, &made);
    check(started && WaitForSingleObject(made.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(made.hProcess, &code) && code == 9 &&
          WaitForSingleObject(listed, 0) == WAIT_OBJECT_0, "CreateProcess with a list of handles to inherit: the child sets the event it was handed");
    if (started) { CloseHandle(made.hThread); CloseHandle(made.hProcess); }
    free(list);
    CloseHandle(listed);
  }

  /* What a 32-bit program is told about itself and the machine: it runs "under WOW64" on a 64-bit processor. */
  BOOL wow = FALSE;
  SYSTEM_INFO own, native;
  GetSystemInfo(&own);
  GetNativeSystemInfo(&native);
  check(IsWow64Process(GetCurrentProcess(), &wow) && wow, "IsWow64Process says so");
  check(own.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL && native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 &&
        (UINT_PTR)own.lpMaximumApplicationAddress < 0x80000000u && own.dwPageSize == 4096, "GetSystemInfo is a 32-bit processor's, GetNativeSystemInfo the real one's");

  printf(g_failed == 0 ? "crt32: all %d checks passed\n" : "crt32: %d checks passed, some FAILED\n", g_passed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
