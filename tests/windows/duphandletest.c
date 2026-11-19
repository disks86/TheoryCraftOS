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

/* File handles handed between processes with DuplicateHandle, as Chromium's mojo does when a sandboxed service asks the browser to open a file
 * (storage.mojom.Directory::OpenFile): the browser duplicates the file into the service's process with DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE.
 * The parent duplicates one file into the child (its value goes through the child's standard input), the child duplicates another out of the parent
 * (its value is on the command line); both share the file position with the parent's handle, as on Windows.
 * Build: x86_64-w64-mingw32-gcc -O1 -o duphandletest.exe duphandletest.c
 * Prints "duphandletest: N checks, M failed". */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; printf("duphandletest: line %d failed: %s (error %lu)\n", __LINE__, #c, GetLastError()); } } while (0)

static HANDLE make_file(const wchar_t* name, const char* text) {
  HANDLE f = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD done = 0;
  CHECK(f != INVALID_HANDLE_VALUE);
  CHECK(WriteFile(f, text, (DWORD)strlen(text), &done, NULL) && done == strlen(text));
  CHECK(SetFilePointer(f, 0, NULL, FILE_BEGIN) == 0);
  return f;
}

static int read_text(HANDLE f, char* out, DWORD capacity) {
  DWORD got = 0;
  if (!ReadFile(f, out, capacity - 1, &got, NULL)) return 0;
  out[got] = 0;
  return 1;
}

static int child(DWORD parent_pid, unsigned long long parent_handle) {
  char text[64];
  unsigned long long given = 0;
  DWORD got = 0;
  /* the one the parent put into this process: its value comes on standard input */
  CHECK(ReadFile(GetStdHandle(STD_INPUT_HANDLE), &given, sizeof(given), &got, NULL) && got == sizeof(given));
  HANDLE mine = (HANDLE)(ULONG_PTR)given;
  CHECK(read_text(mine, text, sizeof(text)) && strcmp(text, "first file") == 0);
  DWORD written = 0;
  CHECK(WriteFile(mine, " from child", 11, &written, NULL) && written == 11);      /* (the copy keeps the rights of the original: it can write) */
  CloseHandle(mine);
  /* the one taken out of the parent */
  HANDLE parent = OpenProcess(PROCESS_DUP_HANDLE, FALSE, parent_pid);
  CHECK(parent != NULL);
  HANDLE taken = NULL;
  CHECK(DuplicateHandle(parent, (HANDLE)(ULONG_PTR)parent_handle, GetCurrentProcess(), &taken, 0, FALSE, DUPLICATE_SAME_ACCESS));
  CHECK(read_text(taken, text, sizeof(text)) && strcmp(text, "second file") == 0);
  CloseHandle(taken);
  CloseHandle(parent);
  return failed;
}

int main(int argc, char** argv) {
  if (argc == 4 && strcmp(argv[1], "child") == 0) return child((DWORD)strtoul(argv[2], NULL, 10), strtoull(argv[3], NULL, 16));

  HANDLE first = make_file(L"C:\\duphandle1.txt", "first file");
  HANDLE second = make_file(L"C:\\duphandle2.txt", "second file");
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
  HANDLE read_end = NULL, write_end = NULL;
  CHECK(CreatePipe(&read_end, &write_end, &inherit, 0));
  SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);

  wchar_t self[MAX_PATH], command[MAX_PATH + 64];
  GetModuleFileNameW(NULL, self, MAX_PATH);
  _snwprintf(command, MAX_PATH + 64, L"\"%ls\" child %lu %llx", self, GetCurrentProcessId(), (unsigned long long)(ULONG_PTR)second);
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = read_end;
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  CHECK(CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi));
  CloseHandle(read_end);

  /* into the child, as mojo hands a file to a sandboxed process: the same rights, the source closed */
  HANDLE copy = first, in_child = NULL;
  CHECK(DuplicateHandle(GetCurrentProcess(), copy, pi.hProcess, &in_child, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE));
  unsigned long long value = (unsigned long long)(ULONG_PTR)in_child;
  DWORD written = 0;
  CHECK(WriteFile(write_end, &value, sizeof(value), &written, NULL) && written == sizeof(value));
  CloseHandle(write_end);

  DWORD code = 99;
  CHECK(WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0);
  CHECK(GetExitCodeProcess(pi.hProcess, &code) && code == 0);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  /* what the child wrote through its copy is in the file */
  HANDLE check = CreateFileW(L"C:\\duphandle1.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
  char text[64];
  CHECK(check != INVALID_HANDLE_VALUE && read_text(check, text, sizeof(text)) && strcmp(text, "first file from child") == 0);
  CloseHandle(check);
  /* the parent's own handle moved on with the child's read: they share one position */
  CHECK(SetFilePointer(second, 0, NULL, FILE_CURRENT) == 11);
  CloseHandle(second);
  DeleteFileW(L"C:\\duphandle1.txt");
  DeleteFileW(L"C:\\duphandle2.txt");
  printf("duphandletest: %d checks, %d failed\n", checks, failed);
  return failed;
}
