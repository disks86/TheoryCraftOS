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

/* LoadLibrary, FreeLibrary and friends on TheoryCraftOS (mingw gcc; see tests/windows/build.sh). Needs loadme.dll, loaddep.dll, failinit.dll and tlsdll.dll in
 * C:\Windows\System32 (-DTCOS_USERMODE_DEMO_FILES=...). Prints "loadtest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

typedef int (*AddFn)(int, int);
typedef int (*Add3Fn)(int, int, int);

static HANDLE g_loaded, g_threads;
/* how many DLL_PROCESS_ATTACHes are not yet matched by a DETACH */
static LONG live(void) {
  LONG previous = 0;
  ReleaseSemaphore(g_loaded, 1, &previous);
  WaitForSingleObject(g_loaded, 0);
  return previous;
}
static int mapped(HMODULE m) {
  MEMORY_BASIC_INFORMATION info;
  return VirtualQuery(m, &info, sizeof(info)) && info.State != MEM_FREE;
}
static DWORD WINAPI idle_thread(LPVOID p) { (void)p; return 0; }

static int (*volatile g_tls_get)(void), (*volatile g_tls_tail)(void);
static DWORD WINAPI tls_thread(LPVOID p) { (void)p; return (DWORD)(g_tls_get() == 42 && g_tls_tail() == 0 ? 42 : 1); }

static volatile LONG g_stop, g_errors, g_what;
static DWORD WINAPI churn(LPVOID p) {
  (void)p;
  for (int i = 0; i < 40 && !g_stop; ++i) {
    HMODULE m = LoadLibraryA("loadme.dll");
    if (!m) { InterlockedIncrement(&g_errors); g_what = 1000 + (LONG)GetLastError(); continue; }
    AddFn add = (AddFn)GetProcAddress(m, "add");
    if (!add || add(i, 1) != i + 1) { InterlockedIncrement(&g_errors); g_what = 2; }
    if (!FreeLibrary(m)) { InterlockedIncrement(&g_errors); g_what = 3000 + (LONG)GetLastError(); }
  }
  return 0;
}

int main(void) {
  g_loaded = CreateSemaphoreA(NULL, 0, 1000, "TcosLoadCount");
  g_threads = CreateSemaphoreA(NULL, 0, 1000, "TcosThreadAttach");

  HMODULE m = LoadLibraryA("loadme.dll");
  if (!m) printf("LoadLibrary(loadme.dll) failed: error %lu\n", (unsigned long)GetLastError());
  check(m != NULL, "LoadLibrary finds loadme.dll in System32");
  check(m && mapped(m), "its image is mapped");
  AddFn add = m ? (AddFn)GetProcAddress(m, "add") : NULL;
  check(add && add(2, 3) == 5, "GetProcAddress and a call");
  check(GetModuleHandleA("loadme.dll") == m && GetModuleHandleA("LOADME") == m, "GetModuleHandle finds it by name");
  check(live() == 1, "DllMain ran once with DLL_PROCESS_ATTACH");
  WCHAR name[MAX_PATH];
  check(GetModuleFileNameW(m, name, MAX_PATH) > 12, "GetModuleFileName");
  check(!GetProcAddress(m, "nosuchfunction") && GetLastError() == ERROR_PROC_NOT_FOUND, "a missing export");

  /* a second use */
  check(LoadLibraryA("loadme.dll") == m, "loading it again gives the same handle");
  check(live() == 1, "and does not start it again");
  check(FreeLibrary(m) && GetModuleHandleA("loadme.dll") == m && live() == 1 && add(1, 1) == 2, "one FreeLibrary leaves it loaded");
  check(FreeLibrary(m), "the second FreeLibrary");
  check(GetModuleHandleA("loadme.dll") == NULL, "now it is gone");
  check(!mapped(m), "its pages are unmapped");
  check(live() == 0, "DLL_PROCESS_DETACH was called");
  SetLastError(0);
  check(!FreeLibrary(m) && GetLastError() == ERROR_INVALID_HANDLE, "freeing it a third time fails");

  /* it can come back */
  HMODULE again = LoadLibraryA("C:\\Windows\\System32\\loadme.dll");
  check(again != NULL && GetProcAddress(again, "add") != NULL, "loading by full path");
  /* threads started while it is loaded are told */
  LONG before = 0;
  ReleaseSemaphore(g_threads, 1, &before);
  WaitForSingleObject(g_threads, 0);
  HANDLE t = CreateThread(NULL, 0, idle_thread, NULL, 0, NULL);
  WaitForSingleObject(t, 10000);
  CloseHandle(t);
  LONG after = 0;
  ReleaseSemaphore(g_threads, 1, &after);
  WaitForSingleObject(g_threads, 0);
  check(after == before + 1, "DLL_THREAD_ATTACH for a new thread");
  /* GetModuleHandleEx counts a use unless told not to */
  HMODULE ex = NULL;
  check(GetModuleHandleExA(0, "loadme.dll", &ex) && ex == again, "GetModuleHandleEx");
  FreeLibrary(again);
  check(GetModuleHandleA("loadme.dll") == again, "so one FreeLibrary is not enough");
  check(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)GetProcAddress(again, "add"), &ex) && ex == again, "FROM_ADDRESS");
  check(FreeLibrary(again) && GetModuleHandleA("loadme.dll") == NULL, "and the last one frees it");

  /* a library that imports another */
  HMODULE dep = LoadLibraryA("loaddep.dll");
  if (!dep) printf("LoadLibrary(loaddep.dll) failed: error %lu\n", (unsigned long)GetLastError());
  check(dep != NULL, "loaddep.dll loads");
  Add3Fn add3 = dep ? (Add3Fn)GetProcAddress(dep, "add_three") : NULL;
  check(add3 && add3(1, 2, 3) == 6, "a call through the imported library");
  HMODULE imported = GetModuleHandleA("loadme.dll");
  check(imported != NULL && live() == 1, "loadme.dll came along");
  check(FreeLibrary(dep), "FreeLibrary of the importing one");
  check(GetModuleHandleA("loaddep.dll") == NULL && GetModuleHandleA("loadme.dll") == NULL && live() == 0, "takes the imported one with it");
  /* ... unless somebody else holds it */
  HMODULE own = LoadLibraryA("loadme.dll");
  dep = LoadLibraryA("loaddep.dll");
  check(own != NULL && dep != NULL && GetModuleHandleA("loadme.dll") == own, "loadme.dll was loaded already when loaddep.dll came");
  FreeLibrary(dep);
  check(GetModuleHandleA("loaddep.dll") == NULL && GetModuleHandleA("loadme.dll") == own && live() == 1, "freeing the importer keeps one that is used otherwise");
  FreeLibrary(own);
  check(GetModuleHandleA("loadme.dll") == NULL, "freeing the last use frees it");

  /* things that fail */
  SetLastError(0);
  check(LoadLibraryA("nosuchlibrary.dll") == NULL && GetLastError() == ERROR_MOD_NOT_FOUND, "a missing library: ERROR_MOD_NOT_FOUND");
  SetLastError(0);
  HMODULE fi = LoadLibraryA("failinit.dll");
  const DWORD fi_error = GetLastError();
  if (fi_error != 1114) printf("failinit.dll: handle %p, error %lu\n", (void *)fi, (unsigned long)fi_error);
  check(fi == NULL && fi_error == 1114, "an entry point that refuses: ERROR_DLL_INIT_FAILED");
  check(GetModuleHandleA("failinit.dll") == NULL, "which is not left loaded");
  SetLastError(0);
  check(LoadLibraryExA("loadme.dll", NULL, LOAD_LIBRARY_AS_DATAFILE) == NULL && GetLastError() == ERROR_NOT_SUPPORTED, "the data-file mode is refused");

  /* a DLL with thread-local data of its own: every thread, the ones that exist and the ones made later, has a copy */
  HMODULE tls = LoadLibraryA("tlsdll.dll");
  check(tls != NULL, "tlsdll.dll (thread-local data of its own) loads");
  if (tls) {
    typedef int (*GetFn)(void);
    typedef void (*SetFn)(int);
    GetFn get = (GetFn)GetProcAddress(tls, "tlsdll_get"), tail = (GetFn)GetProcAddress(tls, "tlsdll_buffer_tail"), second = (GetFn)GetProcAddress(tls, "tlsdll_second");
    SetFn set = (SetFn)GetProcAddress(tls, "tlsdll_set");
    check(get && set && get() == 42 && second() == 7 && tail() == 0, "the main thread starts with the initial values");
    set(9);
    check(get() == 9 && second() == 10 && tail() == 9, "and has its own copy");
    g_tls_get = get; g_tls_tail = tail;
    HANDLE helper = CreateThread(NULL, 0, tls_thread, NULL, 0, NULL);
    WaitForSingleObject(helper, 10000);
    DWORD saw = 0;
    GetExitCodeThread(helper, &saw);
    CloseHandle(helper);
    check(saw == 42 && get() == 9, "a thread made afterwards has a copy of its own, and the main thread's is untouched");
    check(FreeLibrary(tls) && GetModuleHandleA("tlsdll.dll") == NULL, "freeing it");
  }

  /* a copy elsewhere, found through SetDllDirectory */
  CreateDirectoryA("C:\\Temp", NULL);
  check(CopyFileA("C:\\Windows\\System32\\loadme.dll", "C:\\Temp\\copyme.dll", FALSE), "copy a DLL to C:\\Temp");
  check(LoadLibraryA("copyme.dll") == NULL, "which is not on the search path");
  check(SetDllDirectoryA("C:\\Temp"), "SetDllDirectory");
  char directory[MAX_PATH];
  check(GetDllDirectoryA(sizeof(directory), directory) == 7 && !strcmp(directory, "C:\\Temp"), "GetDllDirectory");
  HMODULE copy = LoadLibraryA("copyme.dll");
  check(copy != NULL && GetProcAddress(copy, "add") != NULL, "now it is");
  check(FreeLibrary(copy) && GetModuleHandleA("copyme.dll") == NULL, "and goes again");
  SetDllDirectoryA(NULL);
  DeleteFileA("C:\\Temp\\copyme.dll");

  /* several threads loading and freeing at once */
  HANDLE workers[4];
  for (int i = 0; i < 4; ++i) workers[i] = CreateThread(NULL, 0, churn, NULL, 0, NULL);
  check(WaitForMultipleObjects(4, workers, TRUE, 120000) == WAIT_OBJECT_0, "four threads load and free at the same time");
  g_stop = 1;
  if (g_errors) printf("churn: %ld errors, last %ld\n", (long)g_errors, (long)g_what);
  check(g_errors == 0, "without a failure");
  check(GetModuleHandleA("loadme.dll") == NULL && live() == 0, "and leave nothing loaded");
  for (int i = 0; i < 4; ++i) CloseHandle(workers[i]);

  /* a library still loaded when the process ends is told (the semaphore is not checked after: the process is gone) */
  HMODULE last = LoadLibraryA("loadme.dll");
  check(last != NULL && live() == 1, "one library stays loaded to the end");
  printf("loadtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
