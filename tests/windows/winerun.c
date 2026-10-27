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

/* Runs the test programs listed in C:\Windows\System32\winerun.txt (one name per line) one after the other, each in a process of its own with this process's standard
 * handles, and prints "=== <name> exit <code>" (or "timeout") after each. The last line is "WINERUN DONE", which scripts/winetest.sh waits for. A program that does not end
 * within 30 s is terminated. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  char list[16384];
  HANDLE f = CreateFileA("C:\\Windows\\System32\\winerun.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  DWORD got = 0;
  if (f == INVALID_HANDLE_VALUE || !ReadFile(f, list, sizeof(list) - 1, &got, NULL)) {
    printf("winerun: no list\nWINERUN DONE\n");
    return 1;
  }
  CloseHandle(f);
  list[got] = 0;
  /* The debug console is slow: a test that fails a thousand checks would not end in time. After 30 failures a test only counts. */
  SetEnvironmentVariableA("WINETEST_MUTE_THRESHOLD", "30");
  for (char *line = strtok(list, "\r\n"); line; line = strtok(NULL, "\r\n")) {
    if (line[0] == '!') {      /* "!NAME=value" sets an environment variable for the programs after it */
      char *eq = strchr(line, '=');
      if (eq) { *eq = 0; SetEnvironmentVariableA(line + 1, eq + 1); }
      continue;
    }
    char command[260];
    snprintf(command, sizeof(command), "C:\\Windows\\System32\\%s", line);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    printf("=== %s start\n", line);
    fflush(stdout);
    if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
      printf("=== %s did not start (error %lu)\n", line, GetLastError());
      continue;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    if (waited != WAIT_OBJECT_0) {
      /* Where is it stuck? The thread's registers and the top of its stack (symbolize with objdump). */
      const DWORD suspended = SuspendThread(pi.hThread);
      DWORD thread_code = 0;
      GetExitCodeThread(pi.hThread, &thread_code);
      printf("=== %s: suspend count %lu, thread code %lx, thread wait %lu\n", line, suspended, thread_code, WaitForSingleObject(pi.hThread, 0));
      CONTEXT context;
      memset(&context, 0, sizeof(context));
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      const BOOL have_context = GetThreadContext(pi.hThread, &context);
      if (!have_context) printf("=== %s: GetThreadContext failed, error %lu\n", line, GetLastError());
      if (have_context) {
        printf("=== %s stuck at rip=%llx rsp=%llx\n", line, (unsigned long long)context.Rip, (unsigned long long)context.Rsp);
        unsigned long long stack[24];
        SIZE_T got = 0;
        if (ReadProcessMemory(pi.hProcess, (LPCVOID)context.Rsp, stack, sizeof(stack), &got)) {
          printf("    stack:");
          for (SIZE_T k = 0; k < got / 8; ++k) printf(" %llx", stack[k]);
          printf("\n");
        }
      }
      TerminateProcess(pi.hProcess, 1);
      printf("=== %s timeout\n", line);
    } else {
      GetExitCodeProcess(pi.hProcess, &code);
      printf("=== %s exit 0x%lx\n", line, code);
    }
    fflush(stdout);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  printf("WINERUN DONE\n");
  return 0;
}
