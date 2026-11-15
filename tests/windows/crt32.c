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

  printf(g_failed == 0 ? "crt32: all %d checks passed\n" : "crt32: %d checks passed, some FAILED\n", g_passed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
