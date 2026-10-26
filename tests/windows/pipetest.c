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

/* Pipes on TheoryCraftOS: anonymous and named, between threads and between processes, and standard handles redirected into a child (mingw gcc; see tests/windows/build.sh;
 * procchild.exe has to be in C:\Windows\System32). Prints "pipetest: N checks, M failed"; the exit code is the number of failures. */
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

/* ---- anonymous pipes ---- */
static HANDLE g_big_write;
static DWORD WINAPI big_writer(LPVOID p) {
  (void)p;
  char block[1000];
  unsigned char value = 0;
  for (int i = 0; i < 100; ++i) {
    for (int k = 0; k < 1000; ++k) block[k] = (char)value++;
    DWORD n = 0;
    if (!WriteFile(g_big_write, block, sizeof(block), &n, NULL) || n != sizeof(block)) return 1;
  }
  CloseHandle(g_big_write);
  return 0;
}

static void test_anonymous(void) {
  HANDLE r, w;
  check(CreatePipe(&r, &w, NULL, 0), "CreatePipe");
  check(GetFileType(r) == FILE_TYPE_PIPE && GetFileType(w) == FILE_TYPE_PIPE, "GetFileType says pipe");
  DWORD n = 0;
  check(WriteFile(w, "hello", 5, &n, NULL) && n == 5, "write");
  DWORD available = 0, peeked = 0;
  char buffer[64];
  check(PeekNamedPipe(r, buffer, sizeof(buffer), &peeked, &available, NULL) && available == 5 && peeked == 5 && !memcmp(buffer, "hello", 5), "PeekNamedPipe sees the data and leaves it");
  memset(buffer, 0, sizeof(buffer));
  check(ReadFile(r, buffer, sizeof(buffer), &n, NULL) && n == 5 && !memcmp(buffer, "hello", 5), "read what was written");
  check(PeekNamedPipe(r, NULL, 0, NULL, &available, NULL) && available == 0, "and then nothing is left");
  /* the writer goes away: the reader gets what is there, then the end */
  WriteFile(w, "xyz", 3, &n, NULL);
  CloseHandle(w);
  check(ReadFile(r, buffer, sizeof(buffer), &n, NULL) && n == 3, "data written before the close is still there");
  SetLastError(0);
  check(!ReadFile(r, buffer, sizeof(buffer), &n, NULL) && GetLastError() == ERROR_BROKEN_PIPE, "then ERROR_BROKEN_PIPE");
  CloseHandle(r);
  /* the reader goes away: the writer is told */
  check(CreatePipe(&r, &w, NULL, 0), "another pipe");
  CloseHandle(r);
  SetLastError(0);
  check(!WriteFile(w, "x", 1, &n, NULL) && (GetLastError() == ERROR_NO_DATA || GetLastError() == ERROR_BROKEN_PIPE), "writing to a pipe nobody reads fails");
  CloseHandle(w);
  /* a lot of data through a small buffer, with the writer in a thread */
  check(CreatePipe(&r, &g_big_write, NULL, 4096), "a pipe with a 4 KiB buffer");
  HANDLE t = CreateThread(NULL, 0, big_writer, NULL, 0, NULL);
  DWORD total = 0;
  unsigned char expect = 0;
  int wrong = 0;
  char block[777];
  while (ReadFile(r, block, sizeof(block), &n, NULL) && n > 0) {
    for (DWORD i = 0; i < n; ++i) if ((unsigned char)block[i] != expect++) ++wrong;
    total += n;
  }
  check(total == 100000 && wrong == 0, "100000 bytes arrive in order through a 4 KiB pipe");
  WaitForSingleObject(t, 10000);
  DWORD code = 99;
  GetExitCodeThread(t, &code);
  check(code == 0, "the writer saw no error");
  CloseHandle(t);
  CloseHandle(r);
}

/* ---- named pipes ---- */
#define PIPE_NAME "\\\\.\\pipe\\TcosTestPipe"
static DWORD WINAPI echo_server(LPVOID p) {
  HANDLE server = (HANDLE)p;
  for (int round = 0; round < 2; ++round) {
    if (!ConnectNamedPipe(server, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) return 1;
    char buffer[256];
    DWORD n = 0, done = 0;
    while (ReadFile(server, buffer, sizeof(buffer), &n, NULL) && n > 0) {
      for (DWORD i = 0; i < n; ++i) if (buffer[i] >= 'a' && buffer[i] <= 'z') buffer[i] = (char)(buffer[i] - 32);
      if (!WriteFile(server, buffer, n, &done, NULL)) return 2;
    }
    FlushFileBuffers(server);
    DisconnectNamedPipe(server);
  }
  return 0;
}

static void test_named(void) {
  HANDLE server = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 2, 4096, 4096, 0, NULL);
  check(server != INVALID_HANDLE_VALUE, "CreateNamedPipe");
  SetLastError(0);
  check(CreateFileA("\\\\.\\pipe\\NoSuchPipe", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND, "a pipe that does not exist: ERROR_FILE_NOT_FOUND");
  HANDLE second = CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 2, 4096, 4096, 0, NULL);
  check(second != INVALID_HANDLE_VALUE, "a second instance of the same name");
  SetLastError(0);
  check(CreateNamedPipeA(PIPE_NAME, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 2, 4096, 4096, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY, "a third is over the limit: ERROR_PIPE_BUSY");
  CloseHandle(second);

  HANDLE t = CreateThread(NULL, 0, echo_server, server, 0, NULL);
  check(WaitNamedPipeA(PIPE_NAME, 5000), "WaitNamedPipe finds an instance");
  HANDLE client = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  check(client != INVALID_HANDLE_VALUE, "a client connects");
  char reply[64] = {0};
  DWORD n = 0;
  check(WriteFile(client, "ping pong", 9, &n, NULL) && n == 9, "the client writes");
  check(ReadFile(client, reply, sizeof(reply), &n, NULL) && n == 9 && !memcmp(reply, "PING PONG", 9), "and reads the server's answer");
  check(TransactNamedPipe(client, "again", 5, reply, sizeof(reply), &n, NULL) && n == 5 && !memcmp(reply, "AGAIN", 5), "TransactNamedPipe");
  DWORD flags = 0, out_size = 0, in_size = 0, max = 0;
  check(GetNamedPipeInfo(client, &flags, &out_size, &in_size, &max) && !(flags & PIPE_SERVER_END) && max == 2 && out_size == 4096 && in_size == 4096, "GetNamedPipeInfo on the client end");
  check(GetNamedPipeInfo(server, &flags, NULL, NULL, NULL) && (flags & PIPE_SERVER_END), "and on the server end");
  CloseHandle(client);      /* the server sees the end, disconnects and listens again */

  /* the instance serves a second client */
  check(CallNamedPipeA(PIPE_NAME, "call it", 7, reply, sizeof(reply), &n, 5000) && n == 7 && !memcmp(reply, "CALL IT", 7), "CallNamedPipe (wait, connect, transact, close)");
  WaitForSingleObject(t, 10000);
  DWORD code = 99;
  GetExitCodeThread(t, &code);
  check(code == 0, "the server thread saw no error");
  CloseHandle(t);
  CloseHandle(server);
  SetLastError(0);
  check(CreateFileA(PIPE_NAME, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND, "when the server is gone the name is too");

  /* a one-way pipe */
  HANDLE out = CreateNamedPipeA("\\\\.\\pipe\\TcosOneWay", PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE, 1, 1024, 1024, 0, NULL);
  check(out != INVALID_HANDLE_VALUE, "an outbound pipe");
  SetLastError(0);
  check(CreateFileA("\\\\.\\pipe\\TcosOneWay", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "a client cannot write to an outbound pipe");
  HANDLE reader = CreateFileA("\\\\.\\pipe\\TcosOneWay", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
  check(reader != INVALID_HANDLE_VALUE, "but can read");
  SetLastError(0);
  check(!ConnectNamedPipe(out, NULL) && GetLastError() == ERROR_PIPE_CONNECTED, "ConnectNamedPipe after the client came: ERROR_PIPE_CONNECTED");
  check(WriteFile(out, "one way", 7, &n, NULL) && ReadFile(reader, reply, sizeof(reply), &n, NULL) && n == 7 && !memcmp(reply, "one way", 7), "data goes from the server to the client");
  DWORD mode = PIPE_NOWAIT;
  check(SetNamedPipeHandleState(reader, &mode, NULL, NULL), "SetNamedPipeHandleState(PIPE_NOWAIT)");
  SetLastError(0);
  check(!ReadFile(reader, reply, sizeof(reply), &n, NULL) && GetLastError() == ERROR_NO_DATA, "a non-blocking read of an empty pipe: ERROR_NO_DATA");
  DWORD state = 0;
  check(GetNamedPipeHandleStateA(reader, &state, NULL, NULL, NULL, NULL, 0) && (state & PIPE_NOWAIT), "GetNamedPipeHandleState");
  CloseHandle(reader);
  CloseHandle(out);
}

/* ---- a child with its standard handles on pipes ---- */
static void test_child(void) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE out_read, out_write;
  check(CreatePipe(&out_read, &out_write, &sa, 0), "an inheritable pipe");
  DWORD flags = 0;
  check(GetHandleInformation(out_write, &flags) && (flags & HANDLE_FLAG_INHERIT), "its handles are inheritable");
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);      /* (the child must not hold the read end) */
  check(GetHandleInformation(out_read, &flags) && !(flags & HANDLE_FLAG_INHERIT), "SetHandleInformation clears it");

  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = out_write;
  si.hStdError = out_write;
  PROCESS_INFORMATION pi;
  char command[] = "procchild.exe hello_out";
  check(CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi), "CreateProcess with redirected output");
  CloseHandle(out_write);      /* (now only the child holds the write end) */
  char text[256];
  DWORD n = 0, total = 0;
  while (total < sizeof(text) - 1 && ReadFile(out_read, text + total, sizeof(text) - 1 - total, &n, NULL) && n > 0) total += n;
  text[total] = 0;
  check(strstr(text, "hello through a pipe, from process") != NULL, "the child's output arrives through the pipe");
  check(WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0, "the child ends");
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  check(code == 5, "with its exit code");
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(out_read);

  /* both directions: the child filters what it is sent */
  HANDLE in_read, in_write, back_read, back_write;
  CreatePipe(&in_read, &in_write, &sa, 0);
  CreatePipe(&back_read, &back_write, &sa, 0);
  SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(back_read, HANDLE_FLAG_INHERIT, 0);
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_read;
  si.hStdOutput = back_write;
  si.hStdError = back_write;
  char filter[] = "procchild.exe upper_in";
  check(CreateProcessA(NULL, filter, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi), "CreateProcess with redirected input and output");
  CloseHandle(in_read);
  CloseHandle(back_write);
  const char *message = "to the child and back";
  WriteFile(in_write, message, (DWORD)strlen(message), &n, NULL);
  CloseHandle(in_write);      /* the end of its input */
  total = 0;
  while (total < sizeof(text) - 1 && ReadFile(back_read, text + total, sizeof(text) - 1 - total, &n, NULL) && n > 0) total += n;
  text[total] = 0;
  check(!strcmp(text, "TO THE CHILD AND BACK"), "the child's filtered copy comes back");
  WaitForSingleObject(pi.hProcess, 20000);
  GetExitCodeProcess(pi.hProcess, &code);
  check(code == (DWORD)(strlen(message) & 0x7F), "the child counted what it read");
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(back_read);

  /* a handle that is not inheritable stays with the parent */
  HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
  check(GetHandleInformation(e, &flags) && !(flags & HANDLE_FLAG_INHERIT), "handles are not inheritable unless asked");
  CloseHandle(e);
  /* protect from close */
  HANDLE keep = CreateEventA(NULL, TRUE, FALSE, NULL);
  SetHandleInformation(keep, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE);
  SetLastError(0);
  check(!CloseHandle(keep), "a protected handle cannot be closed");
  SetHandleInformation(keep, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0);
  check(CloseHandle(keep), "until the protection is taken off");
}

int main(void) {
  test_anonymous();
  test_named();
  test_child();
  printf("pipetest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
