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

/* Byte-range locks on TheoryCraftOS: LockFile / LockFileEx between handles, threads and processes (mingw gcc; see tests/windows/build.sh; procchild.exe has to be in
 * C:\Windows\System32). Prints "locktest: N checks, M failed"; the exit code is the number of failures. */
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

static HANDLE open_file(void) { return CreateFileA("C:\\Temp\\lock.bin", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL); }

static volatile LONG g_got, g_started;
static DWORD WINAPI blocked_locker(LPVOID p) {
  HANDLE f = (HANDLE)p;
  OVERLAPPED o;
  memset(&o, 0, sizeof(o));
  g_started = 1;
  if (LockFileEx(f, LOCKFILE_EXCLUSIVE_LOCK, 0, 10, 0, &o)) g_got = 1;      /* waits until the range is free */
  return 0;
}

int main(void) {
  CreateDirectoryA("C:\\Temp", NULL);
  HANDLE a = open_file(), b = open_file();
  char zeros[1000] = {0};
  DWORD n = 0;
  WriteFile(a, zeros, sizeof(zeros), &n, NULL);

  check(LockFile(a, 0, 0, 100, 0), "LockFile");
  SetLastError(0);
  check(!LockFile(b, 50, 0, 10, 0) && GetLastError() == ERROR_LOCK_VIOLATION, "another handle cannot lock inside it: ERROR_LOCK_VIOLATION");
  check(LockFile(b, 100, 0, 10, 0), "but can lock next to it");
  SetLastError(0);
  check(!UnlockFile(b, 0, 0, 100, 0) && GetLastError() == ERROR_NOT_LOCKED, "a lock is undone by the handle that made it: ERROR_NOT_LOCKED");
  check(UnlockFile(a, 0, 0, 100, 0), "UnlockFile");
  check(LockFile(b, 50, 0, 10, 0), "and then the range is free");
  UnlockFile(b, 50, 0, 10, 0);
  UnlockFile(b, 100, 0, 10, 0);

  /* shared and exclusive */
  OVERLAPPED o;
  memset(&o, 0, sizeof(o));
  o.Offset = 500;
  check(LockFileEx(a, LOCKFILE_FAIL_IMMEDIATELY, 0, 100, 0, &o) && LockFileEx(b, LOCKFILE_FAIL_IMMEDIATELY, 0, 100, 0, &o), "two shared locks over the same bytes");
  SetLastError(0);
  HANDLE c = open_file();
  check(!LockFileEx(c, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 10, 0, &o) && GetLastError() == ERROR_LOCK_VIOLATION, "an exclusive lock over shared ones fails");
  UnlockFileEx(a, 0, 100, 0, &o);
  UnlockFileEx(b, 0, 100, 0, &o);
  check(LockFileEx(c, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 10, 0, &o), "when they are gone it works");
  UnlockFileEx(c, 0, 10, 0, &o);
  CloseHandle(c);

  /* a lock that waits */
  LockFile(a, 0, 0, 20, 0);
  g_got = g_started = 0;
  HANDLE t = CreateThread(NULL, 0, blocked_locker, b, 0, NULL);
  while (!g_started) Sleep(1);
  Sleep(150);
  check(g_got == 0, "a lock that cannot be had waits");
  UnlockFile(a, 0, 0, 20, 0);
  WaitForSingleObject(t, 10000);
  check(g_got == 1, "and goes on when the range is freed");
  CloseHandle(t);
  memset(&o, 0, sizeof(o));
  UnlockFileEx(b, 0, 10, 0, &o);

  /* locks go with the handle */
  LockFile(a, 0, 0, 100, 0);
  CloseHandle(a);
  check(LockFile(b, 0, 0, 100, 0), "closing the handle gives its locks up");
  UnlockFile(b, 0, 0, 100, 0);
  a = open_file();

  /* a process */
  LockFile(a, 0, 0, 100, 0);
  memset(&o, 0, sizeof(o));
  o.Offset = 300;
  LockFileEx(a, 0, 0, 100, 0, &o);
  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  char command[] = "procchild.exe lockcheck";
  check(CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "start a child that tries locks");
  WaitForSingleObject(pi.hProcess, 20000);
  DWORD code = 99;
  GetExitCodeProcess(pi.hProcess, &code);
  check(code == 0, "its attempts on this process's locks fail or succeed as they should (exit code 0)");
  printf("child exit code %lu\n", (unsigned long)code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(a);
  CloseHandle(b);
  DeleteFileA("C:\\Temp\\lock.bin");
  printf("locktest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
