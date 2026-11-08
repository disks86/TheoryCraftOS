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

/* The channel Chromium's processes talk over (mojo's PlatformChannel and ChannelWin): a byte-mode named pipe made as the first instance with FILE_FLAG_OVERLAPPED, its
 * client end opened by the parent and inherited by the child through a handle list, each end tied to an I/O completion port, and overlapped reads and writes finished
 * through GetQueuedCompletionStatus. The parent sends "ping" messages, the child answers "pong", several rounds, with a read always pending on each side.
 * Prints "mojopipetest: N checks, M failed". */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("mojopipetest: line %d failed: %s (error %lu)\n", __LINE__, #c, GetLastError()); } } while (0)

enum { kRounds = 20, kKey = 77 };

typedef struct {
  OVERLAPPED read_ov, write_ov;
  char read_buffer[256];
} Io;

/* Waits for the next completion on the port; returns which operation (1 read, 2 write) and the bytes moved, or 0 after the time ran out or on an error. */
static int next_completion(HANDLE port, Io* io, DWORD* bytes, const char* who) {
  ULONG_PTR key = 0;
  OVERLAPPED* ov = NULL;
  const BOOL ok = GetQueuedCompletionStatus(port, bytes, &key, &ov, 10000);
  if (!ok && !ov) { printf("mojopipetest: %s: no completion (error %lu)\n", who, GetLastError()); return 0; }
  if (!ok) { printf("mojopipetest: %s: completion failed (error %lu)\n", who, GetLastError()); return 0; }
  if (key != kKey) { printf("mojopipetest: %s: wrong key %llu\n", who, (unsigned long long)key); return 0; }
  return ov == &io->read_ov ? 1 : ov == &io->write_ov ? 2 : 0;
}

static BOOL start_read(HANDLE pipe, Io* io) {
  memset(&io->read_ov, 0, sizeof io->read_ov);
  memset(io->read_buffer, 0, sizeof io->read_buffer);
  return ReadFile(pipe, io->read_buffer, sizeof io->read_buffer - 1, NULL, &io->read_ov) || GetLastError() == ERROR_IO_PENDING;
}

static BOOL start_write(HANDLE pipe, Io* io, const char* text) {
  memset(&io->write_ov, 0, sizeof io->write_ov);
  return WriteFile(pipe, text, (DWORD)strlen(text), NULL, &io->write_ov) || GetLastError() == ERROR_IO_PENDING;
}

/* Talks over `pipe` until `rounds` messages went each way: the parent writes first, the child answers each message it reads. */
static int converse(HANDLE pipe, BOOL parent) {
  const char* who = parent ? "parent" : "child";
  HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (!port || CreateIoCompletionPort(pipe, port, kKey, 0) != port) { printf("mojopipetest: %s: no completion port (error %lu)\n", who, GetLastError()); return 10; }
  Io io;
  int received = 0, sent = 0, writing = 0;
  char text[64];
  if (!start_read(pipe, &io)) { printf("mojopipetest: %s: read failed (error %lu)\n", who, GetLastError()); return 11; }
  if (parent) {
    snprintf(text, sizeof text, "ping %d", sent);
    if (!start_write(pipe, &io, text)) return 12;
    writing = 1;
  }
  while (received < kRounds || writing) {
    DWORD bytes = 0;
    const int which = next_completion(port, &io, &bytes, who);
    if (which == 0) return 13;
    if (which == 2) {
      writing = 0;
      ++sent;
      continue;
    }
    char expected[64];
    snprintf(expected, sizeof expected, parent ? "pong %d" : "ping %d", received);
    if (bytes != strlen(expected) || memcmp(io.read_buffer, expected, bytes) != 0) {
      printf("mojopipetest: %s: read \"%.*s\" (%lu bytes), wanted \"%s\"\n", who, (int)bytes, io.read_buffer, bytes, expected);
      return 14;
    }
    ++received;
    if (received < kRounds || !parent) {
      if (received < kRounds && !start_read(pipe, &io)) return 15;
      if (writing) return 16;      /* (the other side waits for each answer: only one write at a time) */
      snprintf(text, sizeof text, parent ? "ping %d" : "pong %d", parent ? received : received - 1);
      if (!start_write(pipe, &io, text)) return 17;
      writing = 1;
    }
  }
  CloseHandle(port);
  return 0;
}

int main(void) {
  const wchar_t* line = GetCommandLineW();
  const wchar_t* at = wcsstr(line, L"--mojo-platform-channel-handle=");
  if (at) return converse((HANDLE)(ULONG_PTR)wcstoull(at + 31, NULL, 10), FALSE);

  wchar_t name[128];
  swprintf(name, 128, L"\\\\.\\pipe\\mojo.%lu.%lu.%llu", GetCurrentProcessId(), GetCurrentThreadId(), (unsigned long long)GetTickCount64() * 2654435761ull);
  HANDLE server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 4096, 4096, 5000, NULL);
  CHECK(server != INVALID_HANDLE_VALUE);
  SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
  HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, &inherit, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS | FILE_FLAG_OVERLAPPED, NULL);
  CHECK(client != INVALID_HANDLE_VALUE);
  OVERLAPPED connect;
  memset(&connect, 0, sizeof connect);
  const BOOL connected = ConnectNamedPipe(server, &connect);
  CHECK(!connected && GetLastError() == ERROR_PIPE_CONNECTED);      /* (the client is there already) */

  wchar_t self[MAX_PATH];
  CHECK(GetModuleFileNameW(NULL, self, MAX_PATH) > 0);
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
  LPPROC_THREAD_ATTRIBUTE_LIST list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, bytes);
  CHECK(list && InitializeProcThreadAttributeList(list, 1, 0, &bytes));
  HANDLE inherited[1] = {client};
  CHECK(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, NULL, NULL));
  STARTUPINFOEXW info;
  memset(&info, 0, sizeof info);
  info.StartupInfo.cb = sizeof info;
  info.lpAttributeList = list;
  wchar_t command[512];
  swprintf(command, 512, L"\"%ls\" --type=renderer --mojo-platform-channel-handle=%llu", self, (unsigned long long)(ULONG_PTR)client);
  PROCESS_INFORMATION process;
  memset(&process, 0, sizeof process);
  CHECK(CreateProcessW(NULL, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &info.StartupInfo, &process));
  CloseHandle(client);      /* (the child has its own) */

  const DWORD start = GetTickCount();
  const int talked = converse(server, TRUE);
  printf("mojopipetest: %d rounds each way in %lu ms (parent %d)\n", kRounds, GetTickCount() - start, talked);
  CHECK(talked == 0);
  CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
  DWORD code = 99;
  CHECK(GetExitCodeProcess(process.hProcess, &code) && code == 0);
  printf("mojopipetest: the child ended with %lu\n", code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(server);
  printf("mojopipetest: %d checks, %d failed\n", checks, failed);
  return failed;
}
