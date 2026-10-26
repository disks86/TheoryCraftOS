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

/* Toolhelp32 and psapi on TheoryCraftOS: the lists of processes, threads, modules and heaps, HeapWalk and the module and memory functions, for this process and for a child
 * (mingw gcc; see tests/windows/build.sh; procchild.exe has to be in System32). Prints "toolhelptest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD);
BOOL WINAPI K32GetModuleInformation(HANDLE, HMODULE, LPMODULEINFO, DWORD);
DWORD WINAPI K32GetModuleBaseNameA(HANDLE, HMODULE, LPSTR, DWORD);
DWORD WINAPI K32GetModuleFileNameExA(HANDLE, HMODULE, LPSTR, DWORD);
BOOL WINAPI K32GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
DWORD WINAPI K32GetProcessImageFileNameA(HANDLE, LPSTR, DWORD);

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (error %lu)\n", what, (unsigned long)GetLastError());
}

static HANDLE g_stop;
static DWORD WINAPI waiter(LPVOID unused) { (void)unused; WaitForSingleObject(g_stop, 20000); return 0; }

int main(void) {
  const DWORD me = GetCurrentProcessId();
  g_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
  HANDLE worker = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
  Sleep(20);

  /* processes and threads */
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
  check(snap != INVALID_HANDLE_VALUE, "snapshot of processes and threads");
  PROCESSENTRY32 pe;
  pe.dwSize = sizeof(pe);
  int mine = 0;
  DWORD my_threads = 0;
  for (BOOL more = Process32First(snap, &pe); more; more = Process32Next(snap, &pe)) if (pe.th32ProcessID == me) { mine = 1; my_threads = pe.cntThreads; }
  check(mine && my_threads >= 2, "this process is listed with at least two threads");
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  int owned = 0, found_current = 0;
  for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != me) continue;
    ++owned;
    if (te.th32ThreadID == GetCurrentThreadId()) found_current = 1;
  }
  check((DWORD)owned == my_threads && found_current, "the threads of this process, with the current one");
  check(!Thread32Next(snap, &te) && GetLastError() == ERROR_NO_MORE_FILES, "end of the thread list");
  CloseHandle(snap);

  /* modules */
  snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
  check(snap != INVALID_HANDLE_VALUE, "snapshot of modules");
  MODULEENTRY32 me32;
  me32.dwSize = sizeof(me32);
  int first = 1, program_ok = 0, kernel32 = 0, ntdll = 0, count = 0;
  for (BOOL more = Module32First(snap, &me32); more; more = Module32Next(snap, &me32)) {
    ++count;
    if (first) { program_ok = me32.modBaseAddr == (BYTE *)GetModuleHandleA(NULL) && me32.modBaseSize > 0 && me32.th32ProcessID == me; first = 0; }
    if (!stricmp(me32.szModule, "kernel32.dll")) kernel32 = (me32.modBaseAddr == (BYTE *)GetModuleHandleA("kernel32.dll"));
    if (!stricmp(me32.szModule, "ntdll.dll")) ntdll = 1;
  }
  check(count >= 3 && program_ok, "the program is the first module");
  check(kernel32 && ntdll, "kernel32.dll and ntdll.dll are listed with their bases");
  CloseHandle(snap);

  /* heaps */
  HANDLE extra = HeapCreate(0, 0, 0);
  void *block = HeapAlloc(extra, 0, 1000);
  snap = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, 0);
  check(snap != INVALID_HANDLE_VALUE, "snapshot of heaps");
  HEAPLIST32 hl;
  hl.dwSize = sizeof(hl);
  int heaps = 0, default_ok = 0, extra_ok = 0, block_seen = 0;
  for (BOOL more = Heap32ListFirst(snap, &hl); more; more = Heap32ListNext(snap, &hl)) {
    ++heaps;
    if ((hl.dwFlags & HF32_DEFAULT) && hl.th32HeapID == (ULONG_PTR)GetProcessHeap()) default_ok = 1;
    if (hl.th32HeapID == (ULONG_PTR)extra) {
      extra_ok = 1;
      HEAPENTRY32 he;
      he.dwSize = sizeof(he);
      for (BOOL ok = Heap32First(&he, me, hl.th32HeapID); ok; ok = Heap32Next(&he))
        if (he.dwAddress == (ULONG_PTR)block && (he.dwFlags & LF32_FIXED) && he.dwBlockSize >= 1000) block_seen = 1;
    }
  }
  check(heaps >= 2 && default_ok && extra_ok, "the process heap and the new heap are listed");
  check(block_seen, "Heap32First/Next find the allocated block");
  CloseHandle(snap);
  PROCESS_HEAP_ENTRY walk;
  memset(&walk, 0, sizeof(walk));
  int busy = 0, saw = 0;
  while (HeapWalk(extra, &walk)) if (walk.wFlags & PROCESS_HEAP_ENTRY_BUSY) { ++busy; if (walk.lpData == block) saw = 1; }
  check(busy >= 1 && saw && GetLastError() == ERROR_NO_MORE_ITEMS, "HeapWalk finds the block and ends");
  HeapFree(extra, 0, block);
  HeapDestroy(extra);

  /* psapi, this process */
  HMODULE modules[64];
  DWORD needed = 0;
  HANDLE self = GetCurrentProcess();
  check(K32EnumProcessModules(self, modules, sizeof(modules), &needed) && needed >= 3 * sizeof(HMODULE) && modules[0] == GetModuleHandleA(NULL), "EnumProcessModules, the program first");
  char name[MAX_PATH];
  check(K32GetModuleBaseNameA(self, GetModuleHandleA("kernel32.dll"), name, sizeof(name)) > 0 && !stricmp(name, "kernel32.dll"), "GetModuleBaseName");
  check(K32GetModuleFileNameExA(self, NULL, name, sizeof(name)) > 0 && strstr(name, ".exe") != NULL, "GetModuleFileNameEx of the program");
  MODULEINFO mi;
  check(K32GetModuleInformation(self, GetModuleHandleA(NULL), &mi, sizeof(mi)) && mi.lpBaseOfDll == (LPVOID)GetModuleHandleA(NULL) && mi.SizeOfImage > 0, "GetModuleInformation");
  PROCESS_MEMORY_COUNTERS pmc;
  check(K32GetProcessMemoryInfo(self, &pmc, sizeof(pmc)) && pmc.WorkingSetSize > 0 && pmc.PagefileUsage > 0, "GetProcessMemoryInfo");
  check(K32GetProcessImageFileNameA(self, name, sizeof(name)) > 0, "GetProcessImageFileName");

  /* another process */
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  char command[] = "procchild.exe spin";
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  check(CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "start a child");
  Sleep(100);
  snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, pi.dwProcessId);
  check(snap != INVALID_HANDLE_VALUE, "snapshot of the child");
  int child_modules = 0, child_threads = 0, child_listed = 0;
  MODULEENTRY32 cm;
  cm.dwSize = sizeof(cm);
  for (BOOL more = Module32First(snap, &cm); more; more = Module32Next(snap, &cm)) { if (cm.th32ProcessID == pi.dwProcessId) ++child_modules; }
  te.dwSize = sizeof(te);
  for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) if (te.th32OwnerProcessID == pi.dwProcessId) ++child_threads;
  pe.dwSize = sizeof(pe);
  for (BOOL more = Process32First(snap, &pe); more; more = Process32Next(snap, &pe)) if (pe.th32ProcessID == pi.dwProcessId && pe.th32ParentProcessID == me) child_listed = 1;
  check(child_modules >= 3, "the child's modules");
  check(child_threads >= 1, "the child's threads");
  check(child_listed, "the child is listed with this process as its parent");
  CloseHandle(snap);
  check(K32EnumProcessModules(pi.hProcess, modules, sizeof(modules), &needed) && needed >= 3 * sizeof(HMODULE), "EnumProcessModules of the child");
  check(K32GetProcessMemoryInfo(pi.hProcess, &pmc, sizeof(pmc)) && pmc.WorkingSetSize > 0, "GetProcessMemoryInfo of the child");
  check(CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, pi.dwProcessId) == INVALID_HANDLE_VALUE, "the heaps of another process cannot be listed");
  check(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 99999) == INVALID_HANDLE_VALUE, "no such process");
  TerminateProcess(pi.hProcess, 0);
  WaitForSingleObject(pi.hProcess, 5000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  SetEvent(g_stop);
  WaitForSingleObject(worker, 2000);
  CloseHandle(worker);
  printf("toolhelptest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
