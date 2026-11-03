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

/* The kernel32 batch that completed the export table: WaitOnAddress, resource enumeration, VirtualAlloc2 and NUMA, working set and mitigation settings, CPU sets, packages,
 * application recovery, console aliases, file names, backup streams, thread pool I/O, private namespaces, durations, extended contexts (mingw gcc; see tests/windows/build.sh).
 * Prints "batchtest: N checks, M failed"; the exit code is the number of failures. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

LONG WINAPI GetCurrentPackageFullName(UINT32 *, PWSTR);
LONG WINAPI GetCurrentPackageFamilyName(UINT32 *, PWSTR);
LONG WINAPI PackageFamilyNameFromFullName(PCWSTR, UINT32 *, PWSTR);
LONG WINAPI PackageNameAndPublisherIdFromFamilyName(PCWSTR, UINT32 *, PWSTR, UINT32 *, PWSTR);
LONG WINAPI FormatApplicationUserModelId(PCWSTR, PCWSTR, UINT32 *, PWSTR);
LONG WINAPI ParseApplicationUserModelId(PCWSTR, UINT32 *, PWSTR, UINT32 *, PWSTR);
HRESULT WINAPI WerRegisterMemoryBlock(PVOID, DWORD);
HRESULT WINAPI WerUnregisterMemoryBlock(PVOID);
BOOL WINAPI GetConsoleKeyboardLayoutNameW(LPWSTR);
BOOL WINAPI IsValidUILanguage(LANGID);
BOOL WINAPI ValidateLocale(DWORD);
int WINAPI ConvertNLSDayOfWeekToWin32DayOfWeek(int);
static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (error %lu)\n", what, GetLastError());
}

/* ---- WaitOnAddress ---- */
static volatile LONG g_word;
static DWORD WINAPI waker(LPVOID parameter) {
  (void)parameter;
  Sleep(50);
  g_word = 1;
  WakeByAddressSingle((PVOID)&g_word);
  return 0;
}
static volatile LONG g_woken;
static DWORD WINAPI sleeper(LPVOID parameter) {
  LONG undesired = 0;
  while (*(volatile LONG *)parameter == 0) WaitOnAddress(parameter, &undesired, sizeof(undesired), INFINITE);
  InterlockedIncrement(&g_woken);
  return 0;
}
static void test_wait_on_address(void) {
  LONG undesired = 1;
  check(WaitOnAddress(&g_word, &undesired, sizeof(LONG), 1000), "WaitOnAddress returns at once when the value differs");
  undesired = 0;
  const DWORD t0 = GetTickCount();
  LARGE_INTEGER q0, q1, qf;
  QueryPerformanceCounter(&q0);
  SetLastError(0);
  const BOOL waited = WaitOnAddress(&g_word, &undesired, sizeof(LONG), 60);
  const DWORD waited_error = GetLastError();
  QueryPerformanceCounter(&q1);
  QueryPerformanceFrequency(&qf);
  const LONGLONG elapsed_ms = (q1.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart;
  if (waited || waited_error != ERROR_TIMEOUT || elapsed_ms < 40) printf("  (wait returned %d, error %lu, %lld ms by the counter, %lu ms by the tick count)\n", waited, waited_error, elapsed_ms, GetTickCount() - t0);
  check(!waited && waited_error == ERROR_TIMEOUT && elapsed_ms >= 40, "WaitOnAddress times out");
  HANDLE t = CreateThread(NULL, 0, waker, NULL, 0, NULL);
  check(WaitOnAddress(&g_word, &undesired, sizeof(LONG), 5000) && g_word == 1, "WaitOnAddress is woken by WakeByAddressSingle");
  WaitForSingleObject(t, 5000);
  CloseHandle(t);
  check(!WaitOnAddress(&g_word, &undesired, 3, 0) && GetLastError() == ERROR_INVALID_PARAMETER, "a size of 3 is refused");
  static volatile LONG gate;
  HANDLE s[3];
  for (int i = 0; i < 3; ++i) s[i] = CreateThread(NULL, 0, sleeper, (LPVOID)&gate, 0, NULL);
  Sleep(100);
  gate = 1;
  WakeByAddressAll((PVOID)&gate);
  WaitForMultipleObjects(3, s, TRUE, 5000);
  check(g_woken == 3, "WakeByAddressAll wakes every waiter");
  for (int i = 0; i < 3; ++i) CloseHandle(s[i]);
  BYTE small = 5, other = 5;
  check(!WaitOnAddress(&small, &other, 1, 10) && GetLastError() == ERROR_TIMEOUT, "one byte waits");
}

/* ---- resources ---- */
static int g_types, g_names, g_langs;
static int g_saw_rcdata, g_saw_testdata, g_saw_alpha, g_saw_seven, g_saw_english, g_saw_german;
static BOOL CALLBACK type_proc(HMODULE m, LPWSTR type, LONG_PTR p) {
  (void)m; (void)p; ++g_types;
  if (IS_INTRESOURCE(type) && (ULONG_PTR)type == 10) g_saw_rcdata = 1;
  if (!IS_INTRESOURCE(type) && !wcscmp(type, L"TESTDATA")) g_saw_testdata = 1;
  return TRUE;
}
static BOOL CALLBACK name_proc(HMODULE m, LPCWSTR type, LPWSTR name, LONG_PTR p) {
  (void)m; (void)type; (void)p; ++g_names;
  if (IS_INTRESOURCE(name) && (ULONG_PTR)name == 7) g_saw_seven = 1;
  if (!IS_INTRESOURCE(name) && !wcscmp(name, L"ALPHA")) g_saw_alpha = 1;
  return TRUE;
}
static BOOL CALLBACK lang_proc(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR p) {
  (void)m; (void)type; (void)name; (void)p; ++g_langs;
  if (lang == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) g_saw_english = 1;
  if (lang == MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN)) g_saw_german = 1;
  return TRUE;
}
static int g_stopped;
static BOOL CALLBACK stop_proc(HMODULE m, LPWSTR type, LONG_PTR p) { (void)m; (void)type; (void)p; ++g_stopped; return FALSE; }
static BOOL CALLBACK name_proc_a(HMODULE m, LPCSTR type, LPSTR name, LONG_PTR p) {
  (void)m; (void)type;
  if (!IS_INTRESOURCE(name) && !strcmp(name, "ALPHA")) ++*(int *)p;
  return TRUE;
}
static void test_resources(void) {
  HMODULE self = GetModuleHandleW(NULL);
  check(EnumResourceTypesW(self, type_proc, 0) && g_types >= 2 && g_saw_rcdata && g_saw_testdata, "EnumResourceTypes finds the numbered and the named type");
  check(EnumResourceNamesW(self, MAKEINTRESOURCEW(10), name_proc, 0) && g_names == 2 && g_saw_alpha && g_saw_seven, "EnumResourceNames finds a name and a number");
  check(EnumResourceLanguagesW(self, MAKEINTRESOURCEW(10), L"ALPHA", lang_proc, 0) && g_langs == 2 && g_saw_english && g_saw_german, "EnumResourceLanguages finds both languages");
  check(EnumResourceTypesW(self, stop_proc, 0) && g_stopped == 1, "a callback that returns FALSE ends the walk");
  int seen = 0;
  check(EnumResourceNamesA(self, (LPCSTR)10, name_proc_a, (LONG_PTR)&seen) && seen == 1, "the ANSI form converts the names");
  check(EnumResourceNamesExW(self, L"TESTDATA", name_proc, 0, 0, 0) && g_names == 3, "EnumResourceNamesEx by a type name");
  SetLastError(0);
  check(!EnumResourceNamesW(self, L"NOSUCHTYPE", name_proc, 0) && GetLastError() == ERROR_RESOURCE_TYPE_NOT_FOUND, "a missing type");
  check(!EnumResourceLanguagesW(self, MAKEINTRESOURCEW(10), L"NOSUCH", lang_proc, 0) && GetLastError() == ERROR_RESOURCE_NAME_NOT_FOUND, "a missing name");
  check(!EnumResourceTypesW(self, NULL, 0) && GetLastError() == ERROR_INVALID_PARAMETER, "a NULL callback is refused");
}

/* ---- memory ---- */
static void test_memory(void) {
  MEM_ADDRESS_REQUIREMENTS req = {0};
  MEM_EXTENDED_PARAMETER param = {0};
  req.Alignment = 0x200000;
  param.Type = MemExtendedParameterAddressRequirements;
  param.Pointer = &req;
  PVOID p = VirtualAlloc2(GetCurrentProcess(), NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &param, 1);
  check(p != NULL && ((ULONG_PTR)p & 0x1FFFFF) == 0, "VirtualAlloc2 honours a 2 MiB alignment");
  if (p) { *(volatile int *)p = 7; check(*(volatile int *)p == 7, "the aligned memory is usable"); VirtualFree(p, 0, MEM_RELEASE); }
  p = VirtualAlloc2(GetCurrentProcess(), NULL, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, NULL, 0);
  check(p != NULL, "VirtualAlloc2 without parameters");
  if (p) VirtualFree(p, 0, MEM_RELEASE);
  param.Type = MemExtendedParameterNumaNode;
  param.ULong = 0;
  p = VirtualAlloc2(GetCurrentProcess(), NULL, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &param, 1);
  check(p != NULL, "a NUMA node 0 parameter is accepted");
  if (p) VirtualFree(p, 0, MEM_RELEASE);
  p = VirtualAllocExNuma(GetCurrentProcess(), NULL, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0);
  check(p != NULL, "VirtualAllocExNuma node 0");
  if (p) {
    check(OfferVirtualMemory(p, 0x1000, VmOfferPriorityNormal) == 0, "OfferVirtualMemory");
    check(ReclaimVirtualMemory(p, 0x1000) == 0, "ReclaimVirtualMemory");
    check(DiscardVirtualMemory(p, 0x1000) == 0, "DiscardVirtualMemory");
    VirtualFree(p, 0, MEM_RELEASE);
  }
  check(VirtualAllocExNuma(GetCurrentProcess(), NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE, 5) == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "NUMA node 5 does not exist");
  PVOID w = VirtualAlloc(NULL, 0x4000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
  PVOID pages[8];
  ULONG_PTR count = 8;
  DWORD granularity = 0;
  SetLastError(0);
  const UINT watched = w ? GetWriteWatch(0, w, 0x4000, pages, &count, &granularity) : 99;
  if (watched != 0 || granularity != 0x1000) {
    MEMORY_BASIC_INFORMATION mbi = {0};
    VirtualQuery(w, &mbi, sizeof(mbi));
    printf("  (GetWriteWatch %u, error %lu, count %lu, granularity %lu; region %p allocation base %p size %zu type %lx)\n", watched, GetLastError(), (unsigned long)count, granularity, w, mbi.AllocationBase, (size_t)mbi.RegionSize, mbi.Type);
  }
  check(watched == 0 && granularity == 0x1000 && count >= 1, "GetWriteWatch lists the pages of a watched region");
  check(w && ResetWriteWatch(w, 0x4000) == 0, "ResetWriteWatch");
  count = 8;
  PVOID plain = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  check(plain && GetWriteWatch(0, plain, 0x1000, pages, &count, &granularity) == (UINT)-1, "a region without MEM_WRITE_WATCH is refused");
  if (w) VirtualFree(w, 0, MEM_RELEASE);
  if (plain) VirtualFree(plain, 0, MEM_RELEASE);
  check(GetLargePageMinimum() >= 0x1000, "GetLargePageMinimum");
  ULONGLONG mask = 0;
  USHORT node = 99;
  check(GetNumaNodeProcessorMask(0, &mask) && mask != 0 && GetNumaHighestNodeNumber((PULONG)&(ULONG){0}) && GetNumaProximityNodeEx(0, &node) && node == 0, "NUMA node 0");
  ULONGLONG other = 0;
  check(!GetNumaNodeProcessorMask(3, &other), "NUMA node 3 does not exist");
  GROUP_AFFINITY ga;
  check(GetNumaNodeProcessorMaskEx(0, &ga) && ga.Group == 0 && ga.Mask == mask, "GetNumaNodeProcessorMaskEx");
  ULONGLONG avail = 0;
  check(GetNumaAvailableMemoryNode(0, &avail) && avail > 0, "GetNumaAvailableMemoryNode");
  PROCESSOR_NUMBER pn;
  GetCurrentProcessorNumberEx(&pn);
  check(pn.Group == 0, "GetCurrentProcessorNumberEx");
  USHORT groups[2], n = 2;
  check(GetProcessGroupAffinity(GetCurrentProcess(), &n, groups) && n == 1 && groups[0] == 0, "GetProcessGroupAffinity");
  HANDLE m = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0x2000, NULL);
  check(m != NULL, "CreateFileMappingFromApp");
  PVOID v = m ? MapViewOfFileFromApp(m, FILE_MAP_WRITE, 0, 0x1000) : NULL;
  check(v != NULL, "MapViewOfFileFromApp");
  if (v) { *(int *)v = 3; check(UnmapViewOfFileEx(v, 0), "UnmapViewOfFileEx"); }
  if (m) CloseHandle(m);
  HANDLE sem = CreateSemaphoreExA(NULL, 1, 2, "batchtest-semaphore", 0, SEMAPHORE_ALL_ACCESS);
  check(sem != NULL, "CreateSemaphoreExA");
  if (sem) CloseHandle(sem);
  HANDLE low = CreateMemoryResourceNotification(LowMemoryResourceNotification), high = CreateMemoryResourceNotification(HighMemoryResourceNotification);
  BOOL state = TRUE;
  check(low && QueryMemoryResourceNotification(low, &state) && !state, "memory is not low");
  check(high && QueryMemoryResourceNotification(high, &state) && state, "memory is plentiful");
  if (low) CloseHandle(low);
  if (high) CloseHandle(high);
}

/* ---- process and thread settings ---- */
static void test_settings(void) {
  SIZE_T lo = 0, hi = 0;
  DWORD flags = 0;
  check(GetProcessWorkingSetSizeEx(GetCurrentProcess(), &lo, &hi, &flags) && lo >= 4096 && hi >= lo, "GetProcessWorkingSetSizeEx");
  check(SetProcessWorkingSetSize(GetCurrentProcess(), 1 << 20, 4 << 20) && GetProcessWorkingSetSize(GetCurrentProcess(), &lo, &hi) && lo == (1 << 20) && hi == (4 << 20), "the working set size is kept");
  check(!SetProcessWorkingSetSize(GetCurrentProcess(), 8 << 20, 4 << 20), "a minimum above the maximum is refused");
  IO_COUNTERS io;
  check(GetProcessIoCounters(GetCurrentProcess(), &io), "GetProcessIoCounters");
  DWORD level, f;
  check(GetProcessShutdownParameters(&level, &f) && level == 0x280, "GetProcessShutdownParameters");
  DWORD mode = 7;
  check(QueryProcessAffinityUpdateMode(GetCurrentProcess(), &mode) && mode == 0 && SetProcessAffinityUpdateMode(GetCurrentProcess(), 1) && QueryProcessAffinityUpdateMode(GetCurrentProcess(), &mode) && mode == 1, "the affinity update mode");
  DWORD dep = 0;
  BOOL permanent = FALSE;
  check(GetProcessDEPPolicy(GetCurrentProcess(), &dep, &permanent) && (dep & 1) && permanent, "GetProcessDEPPolicy");
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dc = {0};
  check(GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDynamicCodePolicy, &dc, sizeof(dc)) && dc.ProhibitDynamicCode == 0, "GetProcessMitigationPolicy");
  PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY sh = {0};
  sh.RaiseExceptionOnInvalidHandleReference = 1;
  check(SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &sh, sizeof(sh)), "SetProcessMitigationPolicy");
  PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY got = {0};
  check(GetProcessMitigationPolicy(GetCurrentProcess(), ProcessStrictHandleCheckPolicy, &got, sizeof(got)) && got.RaiseExceptionOnInvalidHandleReference == 1, "the policy stays set");
  check(!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &sh, 3), "a wrong size is refused");
  MEMORY_PRIORITY_INFORMATION mp = {MEMORY_PRIORITY_LOW};
  check(SetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &mp, sizeof(mp)), "SetProcessInformation memory priority");
  MEMORY_PRIORITY_INFORMATION back = {0};
  check(GetProcessInformation(GetCurrentProcess(), ProcessMemoryPriority, &back, sizeof(back)) && back.MemoryPriority == MEMORY_PRIORITY_LOW, "GetProcessInformation memory priority");
  PROCESS_POWER_THROTTLING_STATE pt = {PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_EXECUTION_SPEED};
  PROCESS_POWER_THROTTLING_STATE ptback = {0};
  check(SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt)) && GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &ptback, sizeof(ptback)) && ptback.StateMask == pt.StateMask, "process power throttling");
  THREAD_POWER_THROTTLING_STATE tp = {THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, THREAD_POWER_THROTTLING_EXECUTION_SPEED}, tpback = {0};
  check(SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tp, sizeof(tp)) && GetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tpback, sizeof(tpback)) && tpback.StateMask == tp.StateMask, "thread power throttling");
  PROCESSOR_NUMBER want = {0, 0, 0}, old;
  check(SetThreadIdealProcessorEx(GetCurrentThread(), &want, &old) && GetThreadIdealProcessorEx(GetCurrentThread(), &old) && old.Number == 0, "ideal processor");
  ULONG ids[4], need = 0;
  check(GetProcessDefaultCpuSets(GetCurrentProcess(), NULL, 0, &need) && need == 0, "no default CPU sets");
  ids[0] = 256;
  check(SetProcessDefaultCpuSets(GetCurrentProcess(), ids, 1) && GetProcessDefaultCpuSets(GetCurrentProcess(), ids, 4, &need) && need == 1 && ids[0] == 256, "default CPU sets");
  GROUP_AFFINITY masks[1];
  USHORT mn = 0;
  check(GetProcessDefaultCpuSetMasks(GetCurrentProcess(), masks, 1, &mn) && mn == 1 && masks[0].Mask == 1, "CPU set masks");
  ULONG bad = 100000;
  check(!SetThreadSelectedCpuSets(GetCurrentThread(), &bad, 1), "a CPU set id that does not exist is refused");
  SetProcessDefaultCpuSets(GetCurrentProcess(), NULL, 0);
  DWORD64 features = GetEnabledXStateFeatures();
  check((features & 3) == 3, "x87 and SSE state are enabled");
  DWORD length = 0;
  check(!InitializeContext(NULL, CONTEXT_FULL | CONTEXT_XSTATE, NULL, &length) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && length > sizeof(CONTEXT), "InitializeContext asks for space");
  void *buffer = malloc(length);
  PCONTEXT ctx = NULL;
  check(buffer && InitializeContext(buffer, CONTEXT_FULL | CONTEXT_XSTATE, &ctx, &length) && ctx && ((ULONG_PTR)ctx & 15) == 0, "InitializeContext");
  DWORD flen = 0;
  check(ctx && LocateXStateFeature(ctx, XSTATE_LEGACY_SSE, &flen) == (BYTE *)ctx + 0x1A0 && flen == 256, "the SSE state is in the CONTEXT");
  DWORD64 mask = 99;
  check(ctx && GetXStateFeaturesMask(ctx, &mask), "GetXStateFeaturesMask");
  CONTEXT a = {0}, b = {0};
  a.Rax = 5; a.Rip = 0x1234;
  check(CopyContext(&b, CONTEXT_INTEGER, &a) && b.Rax == 5 && b.Rip == 0, "CopyContext copies only the parts asked for");
  free(buffer);
  check(SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) == ES_CONTINUOUS, "SetThreadExecutionState gives the old state back");
  check(SetThreadExecutionState(ES_CONTINUOUS) == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED), "and the new one is kept");
}

/* ---- packages, recovery, error reporting ---- */
static DWORD WINAPI recovery(PVOID p) { (void)p; return 0; }
static void test_packages(void) {
  UINT32 length = 0;
  check(GetCurrentPackageFullName(&length, NULL) == APPMODEL_ERROR_NO_PACKAGE, "this process has no package");
  check(GetCurrentPackageFamilyName(&length, NULL) == APPMODEL_ERROR_NO_PACKAGE, "and no family");
  WCHAR family[64];
  length = 64;
  check(PackageFamilyNameFromFullName(L"Microsoft.Foo_1.2.3.4_x64__8wekyb3d8bbwe", &length, family) == 0 && !wcscmp(family, L"Microsoft.Foo_8wekyb3d8bbwe"), "PackageFamilyNameFromFullName");
  WCHAR name[32], publisher[32];
  UINT32 nl = 32, pl = 32;
  check(PackageNameAndPublisherIdFromFamilyName(L"Microsoft.Foo_8wekyb3d8bbwe", &nl, name, &pl, publisher) == 0 && !wcscmp(name, L"Microsoft.Foo") && !wcscmp(publisher, L"8wekyb3d8bbwe"), "PackageNameAndPublisherIdFromFamilyName");
  WCHAR aumid[64];
  length = 64;
  check(FormatApplicationUserModelId(L"Fam_abc", L"App", &length, aumid) == 0 && !wcscmp(aumid, L"Fam_abc!App"), "FormatApplicationUserModelId");
  WCHAR fam[32], app[32];
  UINT32 fl = 32, al = 32;
  check(ParseApplicationUserModelId(L"Fam_abc!App", &fl, fam, &al, app) == 0 && !wcscmp(fam, L"Fam_abc") && !wcscmp(app, L"App"), "ParseApplicationUserModelId");
  check(RegisterApplicationRecoveryCallback((APPLICATION_RECOVERY_CALLBACK)recovery, NULL, 5000, 0) == S_OK, "RegisterApplicationRecoveryCallback");
  APPLICATION_RECOVERY_CALLBACK cb = NULL;
  PVOID param = NULL;
  DWORD ping = 0, flags = 0;
  check(GetApplicationRecoveryCallback(GetCurrentProcess(), &cb, &param, &ping, &flags) == S_OK && cb == (APPLICATION_RECOVERY_CALLBACK)recovery && ping == 5000, "GetApplicationRecoveryCallback");
  check(UnregisterApplicationRecoveryCallback() == S_OK && GetApplicationRecoveryCallback(GetCurrentProcess(), &cb, &param, &ping, &flags) != S_OK, "UnregisterApplicationRecoveryCallback");
  WCHAR cmd[64];
  DWORD size = 64;
  check(RegisterApplicationRestart(L"/again", RESTART_NO_CRASH) == S_OK && GetApplicationRestartSettings(GetCurrentProcess(), cmd, &size, &flags) == S_OK && !wcscmp(cmd, L"/again") && flags == RESTART_NO_CRASH, "RegisterApplicationRestart and read back");
  check(UnregisterApplicationRestart() == S_OK, "UnregisterApplicationRestart");
  check(WerRegisterMemoryBlock(&cmd, 16) == S_OK && WerUnregisterMemoryBlock(&cmd) == S_OK, "Wer memory blocks");
}

/* ---- console aliases ---- */
static void test_console_aliases(void) {
  check(AddConsoleAliasW((LPWSTR)L"ll", (LPWSTR)L"dir /w $*", (LPWSTR)L"cmd.exe"), "AddConsoleAlias");
  WCHAR target[64];
  check(GetConsoleAliasW((LPWSTR)L"LL", target, sizeof(target), (LPWSTR)L"CMD.EXE") == 20 && !wcscmp(target, L"dir /w $*"), "GetConsoleAlias is case-insensitive");
  check(AddConsoleAliasA("gs", "git status", "cmd.exe"), "the ANSI form");
  check(GetConsoleAliasesLengthW((LPWSTR)L"cmd.exe") == (3 + 10 + 3 + 11) * 2, "GetConsoleAliasesLength");
  WCHAR all[64];
  check(GetConsoleAliasesW(all, sizeof(all), (LPWSTR)L"cmd.exe") == (3 + 10 + 3 + 11) * 2 && !wcsncmp(all, L"ll=dir", 6), "GetConsoleAliases");
  check(GetConsoleAliasExesLengthW() == 8 * 2 && GetConsoleAliasExesA(NULL, 0) == 0, "the programs that have aliases");
  check(AddConsoleAliasW((LPWSTR)L"ll", NULL, (LPWSTR)L"cmd.exe") && GetConsoleAliasW((LPWSTR)L"ll", target, sizeof(target), (LPWSTR)L"cmd.exe") == 0, "an empty target removes the alias");
  AddConsoleAliasW((LPWSTR)L"gs", NULL, (LPWSTR)L"cmd.exe");
  WCHAR layout[KL_NAMELENGTH];
  check(GetConsoleKeyboardLayoutNameW(layout) && !wcscmp(layout, L"00000409"), "GetConsoleKeyboardLayoutName");
}

/* ---- files ---- */
static void test_files(void) {
  CreateDirectoryA("C:\\Temp", NULL);
  DeleteFileA("C:\\Temp\\bt1.txt");
  DeleteFileA("C:\\Temp\\bt2.txt");
  HANDLE f = CreateFileA("C:\\Temp\\bt1.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  DWORD done;
  WriteFile(f, "hello world", 11, &done, NULL);
  CloseHandle(f);
  WCHAR name[MAX_PATH];
  DWORD len = MAX_PATH;
  HANDLE find = FindFirstFileNameW(L"C:\\Temp\\bt1.txt", 0, &len, name);
  check(find != INVALID_HANDLE_VALUE && !wcscmp(name, L"\\Temp\\bt1.txt"), "FindFirstFileNameW names a file");
  len = MAX_PATH;
  check(find != INVALID_HANDLE_VALUE && !FindNextFileNameW(find, &len, name) && GetLastError() == ERROR_HANDLE_EOF, "a file with one link has one name");
  if (find != INVALID_HANDLE_VALUE) FindClose(find);
  if (CreateHardLinkA("C:\\Temp\\bt2.txt", "C:\\Temp\\bt1.txt", NULL)) {
    int seen = 0;
    len = MAX_PATH;
    find = FindFirstFileNameW(L"C:\\Temp\\bt1.txt", 0, &len, name);
    if (find != INVALID_HANDLE_VALUE) {
      do { ++seen; len = MAX_PATH; } while (FindNextFileNameW(find, &len, name));
      FindClose(find);
    }
    check(seen == 2, "both hard links are named");
    DeleteFileA("C:\\Temp\\bt2.txt");
  } else check(1, "(hard links are not available on this volume)");
  /* backup streams */
  f = CreateFileA("C:\\Temp\\bt1.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  BYTE image[64];
  LPVOID context = NULL;
  DWORD got = 0, total = 0;
  while (BackupRead(f, image + total, (DWORD)sizeof(image) - total, &got, FALSE, FALSE, &context) && got) total += got;
  BackupRead(f, NULL, 0, &got, TRUE, FALSE, &context);
  CloseHandle(f);
  check(total == 20 + 11 && *(DWORD *)image == 1 && *(ULONGLONG *)(image + 8) == 11 && !memcmp(image + 20, "hello world", 11), "BackupRead gives a stream header and the data");
  f = CreateFileA("C:\\Temp\\bt3.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  context = NULL;
  check(BackupWrite(f, image, total, &got, FALSE, FALSE, &context) && got == total, "BackupWrite takes it back");
  BackupWrite(f, NULL, 0, &got, TRUE, FALSE, &context);
  CloseHandle(f);
  char text[16] = {0};
  f = CreateFileA("C:\\Temp\\bt3.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
  ReadFile(f, text, 15, &got, NULL);
  CloseHandle(f);
  check(got == 11 && !strcmp(text, "hello world"), "the data is restored");
  DeleteFileA("C:\\Temp\\bt3.txt");
  /* transacted forms refuse a handle that is no transaction */
  SetLastError(0);
  check(CreateFileTransactedA("C:\\Temp\\bt1.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL, NULL, NULL, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_HANDLE, "CreateFileTransacted without a transaction");
  check(!DeleteFileTransactedA("C:\\Temp\\bt1.txt", NULL), "DeleteFileTransacted without a transaction");
  BOOL enabled = TRUE;
  check(AreShortNamesEnabled(NULL, &enabled) && !enabled, "short names are off");
  check(AreFileApisANSI(), "the file APIs are ANSI");
  SetFileApisToOEM();
  check(!AreFileApisANSI(), "SetFileApisToOEM");
  SetFileApisToANSI();
  DWORD type = 99;
  char exe[MAX_PATH];
  GetModuleFileNameA(NULL, exe, MAX_PATH);
  check(GetBinaryTypeA(exe, &type) && type == SCS_64BIT_BINARY, "GetBinaryType of this program");
  check(!GetBinaryTypeA("C:\\Temp\\bt1.txt", &type), "GetBinaryType of a text file");
  DeleteFileA("C:\\Temp\\bt1.txt");
  check(!SetVolumeMountPointA("C:\\Temp\\mnt\\", "C:\\") , "SetVolumeMountPoint is refused");
  check(FindFirstVolumeMountPointA("C:\\", (LPSTR)exe, MAX_PATH) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_NO_MORE_FILES, "no volume mount points");
  char *env = "A=1\0B=2\0";
  check(SetEnvironmentStringsA(env), "SetEnvironmentStringsA");
  char value[8];
  check(GetEnvironmentVariableA("A", value, sizeof(value)) == 1 && value[0] == '1', "and the variables are there");
}

/* ---- thread pool I/O, namespaces ---- */
static volatile LONG g_io_done;
static VOID CALLBACK io_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PVOID overlapped, ULONG result, ULONG_PTR bytes, PTP_IO io) {
  (void)instance; (void)context; (void)overlapped; (void)io;
  if (result == 0 && bytes == 5) InterlockedIncrement(&g_io_done);
}
static void test_pool_and_namespaces(void) {
  HANDLE f = CreateFileA("C:\\Temp\\bt4.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
  PTP_IO io = f != INVALID_HANDLE_VALUE ? CreateThreadpoolIo(f, io_callback, NULL, NULL) : NULL;
  check(io != NULL, "CreateThreadpoolIo");
  if (io) {
    OVERLAPPED ov = {0};
    StartThreadpoolIo(io);
    DWORD written = 0;
    BOOL ok = WriteFile(f, "12345", 5, &written, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) CancelThreadpoolIo(io);
    WaitForThreadpoolIoCallbacks(io, FALSE);
    check(g_io_done == 1, "the callback ran when the write finished");
    CloseThreadpoolIo(io);
  }
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  DeleteFileA("C:\\Temp\\bt4.txt");
  HANDLE boundary = CreateBoundaryDescriptorW(L"batchtest-boundary", 0);
  check(boundary != NULL, "CreateBoundaryDescriptor");
  HANDLE ns = boundary ? CreatePrivateNamespaceW(NULL, boundary, L"BatchNs") : NULL;
  check(ns != NULL, "CreatePrivateNamespace");
  HANDLE ev = ns ? CreateEventW(NULL, TRUE, FALSE, L"BatchNs\\Evt") : NULL;
  check(ev != NULL, "an event is made in the namespace");
  HANDLE open = ns ? OpenPrivateNamespaceW(boundary, L"BatchNs") : NULL;
  check(open != NULL, "OpenPrivateNamespace");
  if (ns) check(CreatePrivateNamespaceW(NULL, boundary, L"BatchNs") == NULL && GetLastError() == ERROR_ALREADY_EXISTS, "a namespace that exists is refused");
  HANDLE same = ev ? OpenEventW(EVENT_ALL_ACCESS, FALSE, L"BatchNs\\Evt") : NULL;
  check(same != NULL, "the event can be opened by its name in the namespace");
  if (same) CloseHandle(same);
  if (open) ClosePrivateNamespace(open, 0);
  if (ev) CloseHandle(ev);
  if (ns) ClosePrivateNamespace(ns, 0);
  if (boundary) DeleteBoundaryDescriptor(boundary);
  HANDLE timer = SetTimerQueueTimer(NULL, (WAITORTIMERCALLBACK)NULL, NULL, 10, 0, FALSE);
  (void)timer;
}

/* ---- languages and small things ---- */
static void test_languages(void) {
  WCHAR out[64];
  ULONGLONG ticks = (ULONGLONG)(3600 * 2 + 60 * 5 + 7) * 10000000ull;
  check(GetDurationFormat(LOCALE_USER_DEFAULT, 0, NULL, ticks, L"h:mm:ss", out, 64) == 8 && !wcscmp(out, L"2:05:07"), "GetDurationFormat h:mm:ss");
  check(GetDurationFormat(LOCALE_USER_DEFAULT, 0, NULL, ticks + 1234567, L"hh 'hours' mm 'min' ss.ffff", out, 64) > 0 && !wcscmp(out, L"02 hours 05 min 07.1234"), "literals and fractions");
  check(GetDurationFormat(LOCALE_USER_DEFAULT, 0, NULL, (ULONGLONG)90000 * 10000000ull, L"d 'days' h", out, 64) > 0 && !wcscmp(out, L"1 days 1"), "days");
  check(GetDurationFormatEx(L"en-US", 0, NULL, ticks, NULL, NULL, 0) == 8, "the length is asked for");
  check(IsValidUILanguage(0x0409) && !IsValidUILanguage(0x0407), "IsValidUILanguage");
  WCHAR lang[16];
  DWORD size = 16, attributes = 99;
  check(GetUILanguageInfo(MUI_LANGUAGE_NAME | MUI_FULL_LANGUAGE, L"en-US\0", lang, &size, &attributes) && !wcscmp(lang, L"en-US"), "GetUILanguageInfo");
  check(ValidateLocale(0x0409), "ValidateLocale");
  WCHAR vl[64];
  check(VerLanguageNameW(0x0409, vl, 64) > 0 && wcsstr(vl, L"English"), "VerLanguageName");
  int geo = 0;
  BOOL CALLBACK count_geo(PWSTR n, LPARAM p) { (void)n; ++*(int *)p; return TRUE; }
  check(EnumSystemGeoNames(GEOCLASS_NATION, count_geo, (LPARAM)&geo) && geo > 100, "EnumSystemGeoNames");
  check(ConvertNLSDayOfWeekToWin32DayOfWeek(0) == 1 && ConvertNLSDayOfWeekToWin32DayOfWeek(6) == 0, "day of week numbering");
  check(MultiplyExtract128(1LL << 40, 1LL << 40, 40) == (1LL << 40), "MultiplyExtract128");
  WCHAR dns[16];
  DWORD n = 16;
  check(DnsHostnameToComputerNameW(L"my-host.example.com", dns, &n) && !wcscmp(dns, L"MY-HOST") && n == 7, "DnsHostnameToComputerName");
  check(!SetComputerNameExW(ComputerNamePhysicalNetBIOS, L"NEWNAME") && GetLastError() == ERROR_ACCESS_DENIED, "the computer name cannot be changed");
  DWORD fw = 0;
  check(GetFirmwareType((PFIRMWARE_TYPE)&fw) && fw == FirmwareTypeUefi, "GetFirmwareType");
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  check(GetTapeStatus(INVALID_HANDLE_VALUE) != 0, "no tape drive");
}

int main(void) {
  test_wait_on_address();
  test_resources();
  test_memory();
  test_settings();
  test_packages();
  test_console_aliases();
  test_files();
  test_pool_and_namespaces();
  test_languages();
  printf("batchtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
