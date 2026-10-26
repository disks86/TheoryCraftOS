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

/* The machine and the scheduling controls on TheoryCraftOS: processors, memory, affinity, priorities, times (mingw gcc; see tests/windows/build.sh). Prints
 * "systest: N checks, M failed"; the exit code is the number of failures. Needs at least two cores. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

static int bits(DWORD_PTR v) { int n = 0; while (v) { n += (int)(v & 1); v >>= 1; } return n; }

static volatile LONG g_stop, g_running;
static volatile LONG g_core[2];      /* the core the worker was last seen on */
static DWORD WINAPI worker(LPVOID parameter) {
  const int me = (int)(INT_PTR)parameter;
  InterlockedIncrement(&g_running);
  while (!g_stop) g_core[me] = (LONG)GetCurrentProcessorNumber();
  return 0;
}

/* Runs for a while and says whether every look at the core number was one of `allowed` (a bit mask). */
static int stays_on(volatile LONG *core, DWORD_PTR allowed) {
  for (int i = 0; i < 300; ++i) {
    if (!((allowed >> *core) & 1)) return 0;
    Sleep(1);
  }
  return 1;
}

int main(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  printf("processors: %lu, mask 0x%llx\n", (unsigned long)si.dwNumberOfProcessors, (unsigned long long)si.dwActiveProcessorMask);
  check(si.dwNumberOfProcessors >= 2, "GetSystemInfo reports more than one processor");
  check(bits(si.dwActiveProcessorMask) == (int)si.dwNumberOfProcessors, "the active mask has a bit for each processor");
  check(si.dwPageSize == 4096 && si.dwAllocationGranularity == 0x10000, "page size and allocation granularity");
  char number[16];
  check(GetEnvironmentVariableA("NUMBER_OF_PROCESSORS", number, sizeof(number)) && atoi(number) == (int)si.dwNumberOfProcessors, "NUMBER_OF_PROCESSORS is the real count");

  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  check(GlobalMemoryStatusEx(&ms), "GlobalMemoryStatusEx");
  printf("memory: %llu MiB, %llu MiB free\n", (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPhys >> 20));
  check(ms.ullTotalPhys >= (256ull << 20) && ms.ullAvailPhys > 0 && ms.ullAvailPhys <= ms.ullTotalPhys, "total and available memory make sense");
  ULONGLONG installed = 0;
  check(GetPhysicallyInstalledSystemMemory(&installed) && installed * 1024 == ms.ullTotalPhys, "GetPhysicallyInstalledSystemMemory");

  DWORD_PTR process_mask = 0, system_mask = 0;
  check(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask == system_mask && system_mask == si.dwActiveProcessorMask, "GetProcessAffinityMask");

  /* priorities */
  check(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL, "a thread starts at normal priority");
  check(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) && GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_HIGHEST, "SetThreadPriority reads back");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  check(GetPriorityClass(GetCurrentProcess()) == NORMAL_PRIORITY_CLASS, "normal priority class");
  check(SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) && GetPriorityClass(GetCurrentProcess()) == BELOW_NORMAL_PRIORITY_CLASS, "SetPriorityClass reads back");
  SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
  check(!SetPriorityClass(GetCurrentProcess(), 0x123) && GetLastError() == ERROR_INVALID_PARAMETER, "a bad priority class is refused");
  SetErrorMode(SEM_FAILCRITICALERRORS);
  check(GetErrorMode() == SEM_FAILCRITICALERRORS, "SetErrorMode reads back");
  SetErrorMode(0);

  /* affinity: a thread kept on one core, then on another */
  HANDLE threads[2];
  for (int i = 0; i < 2; ++i) threads[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
  while (g_running < 2) Sleep(1);
  DWORD_PTR old = SetThreadAffinityMask(threads[0], 1);
  check(old == system_mask, "SetThreadAffinityMask returns the old mask");
  Sleep(100);
  check(stays_on(&g_core[0], 1), "a thread kept on core 0 stays there");
  check(SetThreadAffinityMask(threads[0], 2) == 1, "the old mask is the one just set");
  Sleep(100);
  check(stays_on(&g_core[0], 2), "the same thread moved to core 1 stays there");
  SetLastError(0);
  check(SetThreadAffinityMask(threads[0], (DWORD_PTR)1 << 40) == 0 && GetLastError() == ERROR_INVALID_PARAMETER, "a mask with no existing core is refused");
  check(SetThreadAffinityMask(threads[0], system_mask) == 2, "back to every core");

  /* the whole process on one core */
  check(SetProcessAffinityMask(GetCurrentProcess(), 2), "SetProcessAffinityMask");
  Sleep(100);
  check(stays_on(&g_core[0], 2) && stays_on(&g_core[1], 2), "every thread of the process stays on core 1");
  int main_ok = 1;
  for (int i = 0; i < 300; ++i) { if (GetCurrentProcessorNumber() != 1) main_ok = 0; Sleep(1); }
  check(main_ok, "so does the main thread");
  check(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask == 2, "GetProcessAffinityMask after");
  check(SetProcessAffinityMask(GetCurrentProcess(), system_mask), "all cores again");
  g_stop = 1;
  WaitForMultipleObjects(2, threads, TRUE, 10000);

  /* times */
  FILETIME created, exited, kernel, user;
  for (volatile unsigned long i = 0; i < 40000000; ++i) {}
  check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "GetProcessTimes");
  ULONGLONG user_time = ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime, created_time = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
  check(user_time > 0, "the process has used CPU time");
  check(created_time > 116444736000000000ull, "the process has a creation time (after 1970)");
  check(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) && (((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime) > 0, "GetThreadTimes");

  /* handles and names */
  DWORD handles = 0;
  check(GetProcessHandleCount(GetCurrentProcess(), &handles) && handles >= 3, "GetProcessHandleCount");
  WCHAR name[MAX_PATH];
  DWORD size = MAX_PATH;
  check(QueryFullProcessImageNameW(GetCurrentProcess(), 0, name, &size) && size > 4 && !_wcsicmp(name + size - 4, L".exe"), "QueryFullProcessImageNameW");
  printf("systest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
