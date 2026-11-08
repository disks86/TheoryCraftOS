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

/* How Chromium starts its child processes (base::LaunchProcess): a STARTUPINFOEX whose attribute list names the only handles the child inherits
 * (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), inheritance on, the program's own path quoted in a long command line, and a job object the child is put in. The child (this program
 * again, with --type=child and the handle's value) writes through the inherited pipe and checks that a handle that was not listed is not there.
 * Prints "launchtest: N checks, M failed". */
#include <windows.h>
#include <versionhelpers.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("launchtest: line %d failed: %s (error %lu)\n", __LINE__, #c, GetLastError()); } } while (0)

static int child(const wchar_t* line) {
  const wchar_t* pipe_at = wcsstr(line, L"--pipe=");
  const wchar_t* other_at = wcsstr(line, L"--other=");
  if (!pipe_at || !other_at) return 5;
  HANDLE pipe = (HANDLE)(ULONG_PTR)wcstoull(pipe_at + 7, NULL, 10);
  HANDLE other = (HANDLE)(ULONG_PTR)wcstoull(other_at + 8, NULL, 10);
  DWORD flags = 0;
  const BOOL other_there = GetHandleInformation(other, &flags);      /* (an inheritable handle that was not in the list must not be inherited) */
  char message[64];
  const int n = snprintf(message, sizeof message, "hello from the child %d", other_there ? 1 : 0);
  DWORD wrote = 0;
  if (!GetHandleInformation(pipe, &flags)) {      /* (the listed handle did not arrive) */
    const DWORD error = GetLastError();
    printf("launchtest child: handle %p is not there (error %lu); command line: %ls\n", pipe, error, line);
    return 1000 + (int)error;
  }
  return WriteFile(pipe, message, (DWORD)n, &wrote, NULL) && wrote == (DWORD)n ? 0 : 2000 + (int)GetLastError();
}

int main(void) {
  const wchar_t* line = GetCommandLineW();
  if (wcsstr(line, L"--type=child")) return child(line);

  wchar_t self[MAX_PATH];
  CHECK(GetModuleFileNameW(NULL, self, MAX_PATH) > 0);
  SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
  HANDLE read_end = NULL, write_end = NULL, other_read = NULL, other_write = NULL;
  CHECK(CreatePipe(&read_end, &write_end, &inherit, 0));
  CHECK(CreatePipe(&other_read, &other_write, &inherit, 0));      /* inheritable, but not listed */
  CHECK(SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0));

  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
  CHECK(bytes > 0);
  LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, bytes);
  CHECK(attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &bytes));
  HANDLE inherited[1] = {write_end};
  CHECK(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, NULL, NULL));

  STARTUPINFOEXW info;
  ZeroMemory(&info, sizeof info);
  info.StartupInfo.cb = sizeof info;
  info.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
  info.StartupInfo.wShowWindow = SW_HIDE;
  info.lpAttributeList = attributes;
  wchar_t command[2048];
  swprintf(command, 2048, L"\"%ls\" --type=child --utility-sub-type=network.mojom.NetworkService --lang=en-US --service-sandbox-type=none --no-sandbox "
           L"--mojo-platform-channel-handle=%llu --field-trial-handle=1234,i,5678,9012,262144 --pipe=%llu --other=%llu /prefetch:11",
           self, (unsigned long long)(ULONG_PTR)write_end, (unsigned long long)(ULONG_PTR)write_end, (unsigned long long)(ULONG_PTR)other_write);
  HANDLE job = CreateJobObjectW(NULL, NULL);
  CHECK(job != NULL);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
  ZeroMemory(&limits, sizeof limits);
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits));

  PROCESS_INFORMATION process;
  ZeroMemory(&process, sizeof process);
  const BOOL started = CreateProcessW(NULL, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, NULL, NULL,
                                      &info.StartupInfo, &process);
  CHECK(started);
  if (started) {
    CHECK(AssignProcessToJobObject(job, process.hProcess));
    CHECK(ResumeThread(process.hThread) == 1);
    CloseHandle(write_end);
    write_end = NULL;
    char got[64] = {0};
    DWORD n = 0;
    CHECK(ReadFile(read_end, got, sizeof got - 1, &n, NULL) && n > 0);
    printf("launchtest: child said \"%s\"\n", got);
    CHECK(strcmp(got, "hello from the child 0") == 0);
    CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code = 99;
    CHECK(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    printf("launchtest: the child's exit code %lu\n", code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  DeleteProcThreadAttributeList(attributes);

  /* With the standard handles in the list as well (Chromium passes its stdout and stderr when it logs to them). */
  {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    printf("launchtest: standard handles %p %p\n", out, err);
    CHECK(SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) && SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));      /* (as base::LaunchProcess does) */
    HANDLE r2 = NULL, w2 = NULL;
    CHECK(CreatePipe(&r2, &w2, &inherit, 0));
    SetHandleInformation(r2, HANDLE_FLAG_INHERIT, 0);
    bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, bytes);
    CHECK(list && InitializeProcThreadAttributeList(list, 1, 0, &bytes));
    HANDLE three[3] = {w2, out, err};
    const SIZE_T listed = (out && out != INVALID_HANDLE_VALUE && err && err != INVALID_HANDLE_VALUE) ? 3 : 1;
    CHECK(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, three, listed * sizeof(HANDLE), NULL, NULL));
    STARTUPINFOEXW info2;
    ZeroMemory(&info2, sizeof info2);
    info2.StartupInfo.cb = sizeof info2;
    info2.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    info2.StartupInfo.hStdOutput = out;
    info2.StartupInfo.hStdError = err;
    info2.lpAttributeList = list;
    swprintf(command, 2048, L"\"%ls\" --type=child --pipe=%llu --other=%llu", self, (unsigned long long)(ULONG_PTR)w2, (unsigned long long)(ULONG_PTR)other_write);
    PROCESS_INFORMATION p2;
    ZeroMemory(&p2, sizeof p2);
    const BOOL ok2 = CreateProcessW(NULL, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &info2.StartupInfo, &p2);
    CHECK(ok2);
    if (ok2) {
      CloseHandle(w2);
      char got[64] = {0};
      DWORD n = 0;
      CHECK(ReadFile(r2, got, sizeof got - 1, &n, NULL) && n > 0);
      WaitForSingleObject(p2.hProcess, 10000);
      DWORD code = 99;
      GetExitCodeProcess(p2.hProcess, &code);
      printf("launchtest: with standard handles the child said \"%s\" and ended with %lu\n", got, code);
      CloseHandle(p2.hThread);
      CloseHandle(p2.hProcess);
    }
    DeleteProcThreadAttributeList(list);
  }
  /* A window program started the way Chromium starts its children: no STARTF_USESTDHANDLES, a list, and a mitigation policy. Its standard handles must be none or
     usable (its C runtime takes a handle that is there, of a known type, for an open stream). */
  {
    wchar_t gui[MAX_PATH];
    wcscpy(gui, self);
    wchar_t* slash = wcsrchr(gui, L'\\');
    if (slash) wcscpy(slash + 1, L"kernel32_launchgui.exe");
    HANDLE r4 = NULL, w4 = NULL;
    CHECK(CreatePipe(&r4, &w4, &inherit, 0));
    SetHandleInformation(r4, HANDLE_FLAG_INHERIT, 0);
    bytes = 0;
    InitializeProcThreadAttributeList(NULL, 2, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, bytes);
    CHECK(list && InitializeProcThreadAttributeList(list, 2, 0, &bytes));
    DWORD64 mitigations[2] = {0, 0x20000000};
    CHECK(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, mitigations, sizeof mitigations, NULL, NULL));
    HANDLE one[1] = {w4};
    CHECK(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, one, sizeof one, NULL, NULL));
    STARTUPINFOEXW info4;
    ZeroMemory(&info4, sizeof info4);
    info4.StartupInfo.cb = sizeof info4;
    info4.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
    info4.lpAttributeList = list;
    swprintf(command, 2048, L"\"%ls\" --pipe=%llu", gui, (unsigned long long)(ULONG_PTR)w4);
    PROCESS_INFORMATION p4;
    ZeroMemory(&p4, sizeof p4);
    const BOOL ok4 = CreateProcessW(NULL, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &info4.StartupInfo, &p4);
    CHECK(ok4);
    if (ok4) {
      CloseHandle(w4);
      char got[400] = {0};
      DWORD n = 0;
      ReadFile(r4, got, sizeof got - 1, &n, NULL);
      printf("launchtest: the window program said \"%s\"\n", got);
      CHECK(strstr(got, "usable") != NULL && strstr(got, "not usable") == NULL);
      CHECK(strstr(got, "not attached") != NULL || strstr(got, "wrote 1") != NULL);      /* (attaching to the console works only if opening CONOUT$ then does too) */
      WaitForSingleObject(p4.hProcess, 10000);
      CloseHandle(p4.hThread);
      CloseHandle(p4.hProcess);
    }
    DeleteProcThreadAttributeList(list);
  }

  /* A named pipe made inheritable by its security attributes, with a descriptor of its own and as the first instance (how Crashpad makes the pipe its handler
     inherits): the handle has to be inheritable. */
  {
    SECURITY_DESCRIPTOR sd;
    CHECK(InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE));
    SECURITY_ATTRIBUTES sa = {sizeof sa, &sd, TRUE};
    HANDLE named = CreateNamedPipeW(L"\\\\.\\pipe\\launchtest_crashpad", PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 2, 512, 512, 20, &sa);
    CHECK(named != INVALID_HANDLE_VALUE);
    DWORD handle_flags = 0;
    CHECK(GetHandleInformation(named, &handle_flags) && (handle_flags & HANDLE_FLAG_INHERIT));
    printf("launchtest: the named pipe's handle flags %lu\n", handle_flags);
    CloseHandle(named);
  }

  /* The version checks programs make with VerifyVersionInfoW (Crashpad makes its pipe inheritable only on Vista or later). */
  printf("launchtest: Vista or later %d, 8 or later %d, 10 or later %d, version %#lx\n", IsWindowsVistaOrGreater(), IsWindows8OrGreater(), IsWindows10OrGreater(), GetVersion());
  {
    const ULONGLONG mask = VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_MINORVERSION, VER_GREATER_EQUAL),
                                               VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
    OSVERSIONINFOEXW want = {sizeof want, 6, 0, 0, 0, {0}, 0, 0, 0, 0, 0};
    typedef LONG (WINAPI *RtlVerifyFn)(OSVERSIONINFOEXW*, ULONG, ULONGLONG);
    RtlVerifyFn rtl = (RtlVerifyFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlVerifyVersionInfo");
    printf("launchtest: condition mask %#llx, RtlVerifyVersionInfo %#lx\n", mask, rtl ? rtl(&want, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, mask) : -1L);
    CHECK(mask == 0x1801B);
  }
  CHECK(IsWindowsVistaOrGreater() && IsWindows7OrGreater() && IsWindows8OrGreater());

  /* Without a list: every inheritable handle goes along. */
  {
    HANDLE r3 = NULL, w3 = NULL;
    CHECK(CreatePipe(&r3, &w3, &inherit, 0));
    SetHandleInformation(r3, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW plain;
    ZeroMemory(&plain, sizeof plain);
    plain.cb = sizeof plain;
    swprintf(command, 2048, L"\"%ls\" --type=child --pipe=%llu --other=%llu", self, (unsigned long long)(ULONG_PTR)w3, (unsigned long long)(ULONG_PTR)other_write);
    PROCESS_INFORMATION p3;
    ZeroMemory(&p3, sizeof p3);
    CHECK(CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &plain, &p3));
    CloseHandle(w3);
    char got[64] = {0};
    DWORD n = 0;
    ReadFile(r3, got, sizeof got - 1, &n, NULL);
    WaitForSingleObject(p3.hProcess, 10000);
    DWORD code = 99;
    GetExitCodeProcess(p3.hProcess, &code);
    printf("launchtest: inheriting everything the child said \"%s\" and ended with %lu\n", got, code);
    CHECK(strcmp(got, "hello from the child 1") == 0 && code == 0);      /* (the unlisted handle is inherited too here) */
  }
  CloseHandle(job);
  printf("launchtest: %d checks, %d failed\n", checks, failed);
  return failed;
}
