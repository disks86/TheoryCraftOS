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

/* What a process writes into a pipe just before it ends must still reach the reader, as Chromium's GPU process relies on: it sends its host
 * "DidFailInitialize" over mojo (an overlapped write on a named pipe) and terminates itself at once. The child writes several messages, overlapped
 * and plain, then calls TerminateProcess on itself (or ExitProcess); the parent must read every byte, then see the pipe broken.
 * Build: x86_64-w64-mingw32-gcc -O1 -o pipeexittest.exe pipeexittest.c
 * Prints "pipeexittest: N checks, M failed". */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("pipeexittest: line %d failed: %s (error %lu)\n", __LINE__, #c, GetLastError()); } } while (0)

enum { kMessages = 4, kMessageBytes = 900 };      /* (all of it fits in the pipe's 4096-byte buffer: Windows keeps such writes; a write still waiting for room is cancelled with its process) */

static int child(const char *name, int terminate) {
  HANDLE pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
  if (pipe == INVALID_HANDLE_VALUE) return 2;
  static char message[kMessageBytes];
  OVERLAPPED ov[kMessages];
  for (int i = 0; i < kMessages; ++i) {
    memset(message, 'a' + i, sizeof message);
    memset(&ov[i], 0, sizeof ov[i]);
    ov[i].hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD done = 0;
    if (!WriteFile(pipe, message, sizeof message, &done, &ov[i]) && GetLastError() != ERROR_IO_PENDING) return 3;      /* (not waited for: the process ends next) */
  }
  if (terminate) TerminateProcess(GetCurrentProcess(), 0);
  ExitProcess(0);
}

static void run(const char *self, int terminate) {
  char name[64];
  snprintf(name, sizeof name, "\\\\.\\pipe\\tcos-pipeexit-%lu-%d", GetCurrentProcessId(), terminate);
  HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
  CHECK(server != INVALID_HANDLE_VALUE);
  char command[MAX_PATH + 128];
  snprintf(command, sizeof command, "\"%s\" child %s %d", self, name, terminate);
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof si);
  si.cb = sizeof si;
  CHECK(CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
  OVERLAPPED connect;
  memset(&connect, 0, sizeof connect);
  connect.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (!ConnectNamedPipe(server, &connect)) {
    const DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) CHECK(WaitForSingleObject(connect.hEvent, 20000) == WAIT_OBJECT_0);
    else CHECK(e == ERROR_PIPE_CONNECTED);
  }
  CHECK(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0);      /* (the writer is gone before anything is read) */
  static char buffer[kMessages * kMessageBytes];
  DWORD total = 0;
  for (;;) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD got = 0;
    BOOL ok = ReadFile(server, buffer + total, sizeof buffer - total, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(server, &ov, &got, TRUE);
    CloseHandle(ov.hEvent);
    if (!ok || got == 0) { CHECK(GetLastError() == ERROR_BROKEN_PIPE); break; }
    total += got;
    if (total == sizeof buffer) break;
  }
  printf("pipeexittest: %s: %lu of %u bytes after the writer ended\n", terminate ? "TerminateProcess" : "ExitProcess", total, (unsigned)sizeof buffer);
  CHECK(total == sizeof buffer);
  int in_order = 1;
  for (DWORD i = 0; i < total; ++i) if (buffer[i] != 'a' + (int)(i / kMessageBytes)) in_order = 0;
  CHECK(in_order);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(server);
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "child") == 0) return child(argv[2], atoi(argv[3]));
  char self[MAX_PATH];
  GetModuleFileNameA(NULL, self, MAX_PATH);
  run(self, 1);
  run(self, 0);
  printf("pipeexittest: %d checks, %d failed\n", checks, failed);
  return failed;
}
