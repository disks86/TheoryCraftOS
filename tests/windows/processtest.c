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

/* CreateProcess on TheoryCraftOS: starts procchild.exe in several ways and checks what it did (mingw gcc; see tests/windows/build.sh; procchild.exe has to be in
 * C:\Windows\System32: -DTCOS_USERMODE_DEMO_FILES=...). Prints "processtest: N checks, M failed"; the exit code is the number of failures. */
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

/* Runs the child with a command line and returns its exit code (-1: it did not start or did not end). */
static int run(const char *command_line, LPVOID environment, const char *directory, DWORD *pid_out) {
  char buffer[256];
  strcpy(buffer, command_line);
  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));
  if (!CreateProcessA(NULL, buffer, NULL, NULL, FALSE, 0, environment, directory, &si, &pi)) return -1;
  if (pid_out) *pid_out = pi.dwProcessId;
  const DWORD waited = WaitForSingleObject(pi.hProcess, 20000);
  DWORD code = (DWORD)-1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return waited == WAIT_OBJECT_0 ? (int)code : -1;
}

int main(void) {
  DWORD pid = 0;
  check(run("procchild.exe exit 7", NULL, NULL, &pid) == 7, "exit code 7");
  check(pid != 0 && pid != GetCurrentProcessId(), "the child has an id of its own");
  check(run("procchild exit 0", NULL, NULL, NULL) == 0, "the name without .exe");
  check(run("\"procchild.exe\" args \"a b\" c", NULL, NULL, NULL) == 42, "arguments, one in quotes");
  check(run("C:\\Windows\\System32\\procchild.exe exit 12", NULL, NULL, NULL) == 12, "a full path");
  SetEnvironmentVariableA("TCOS_CHILD", "yes");
  check(run("procchild.exe env", NULL, NULL, NULL) == 0, "the environment is inherited");
  static const char block[] = "ONLY=1\0";      /* (the second NUL ends the block) */
  check(run("procchild.exe env2", (LPVOID)block, NULL, NULL) == 0, "an environment of its own");
  check(run("procchild.exe cwd", NULL, "C:\\Windows", NULL) == 0, "the current directory");
  check(run("procchild.exe print", NULL, NULL, NULL) == 0, "output on the shared console");

  /* a program that is not there */
  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  char missing[] = "nosuchprogram.exe";
  SetLastError(0);
  check(!CreateProcessA(NULL, missing, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) && GetLastError() == ERROR_FILE_NOT_FOUND, "a missing program fails with ERROR_FILE_NOT_FOUND");

  /* started suspended */
  char suspended[] = "procchild.exe exit 9";
  memset(&pi, 0, sizeof(pi));
  check(CreateProcessA(NULL, suspended, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi), "CREATE_SUSPENDED");
  DWORD code = 0;
  check(WaitForSingleObject(pi.hProcess, 300) == WAIT_TIMEOUT, "a suspended process does not run");
  check(GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE, "STILL_ACTIVE");
  check(GetProcessId(pi.hProcess) == pi.dwProcessId, "GetProcessId");
  check(ResumeThread(pi.hThread) == 1, "ResumeThread returns the old count");
  check(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 9, "it runs after ResumeThread");
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  /* TerminateProcess of a child that never stops */
  char spin[] = "procchild.exe spin";
  memset(&pi, 0, sizeof(pi));
  check(CreateProcessA(NULL, spin, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "start a spinning child");
  Sleep(100);
  check(WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT && GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE, "it spins");
  check(TerminateProcess(pi.hProcess, 55), "TerminateProcess");
  check(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 55, "its exit code is the one given");
  check(!TerminateProcess(pi.hProcess, 1), "an ended process cannot be terminated again");
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  /* an event shared with the child by name */
  HANDLE event = CreateEventA(NULL, TRUE, FALSE, "TcosProcessEvent");
  char waiting[] = "procchild.exe waitevent";
  memset(&pi, 0, sizeof(pi));
  check(event && CreateProcessA(NULL, waiting, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "start a child that waits for an event");
  Sleep(200);
  check(WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT, "it waits");
  SetEvent(event);
  check(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 3, "the event wakes it");
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(event);

  /* several at once, waited for together */
  HANDLE children[4];
  for (int i = 0; i < 4; ++i) {
    char line[64];
    sprintf(line, "procchild.exe exit %d", 20 + i);
    memset(&pi, 0, sizeof(pi));
    check(CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "start one of four");
    children[i] = pi.hProcess;
    CloseHandle(pi.hThread);
  }
  check(WaitForMultipleObjects(4, children, TRUE, 30000) == WAIT_OBJECT_0, "WaitForMultipleObjects for four processes");
  for (int i = 0; i < 4; ++i) {
    check(GetExitCodeProcess(children[i], &code) && code == (DWORD)(20 + i), "each has its own exit code");
    CloseHandle(children[i]);
  }
  printf("processtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
