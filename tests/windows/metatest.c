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

/* File attributes and times on TheoryCraftOS: SetFileAttributes (the read-only flag) and SetFileTime really change the file (mingw gcc; see tests/windows/build.sh). Prints
 * "metatest: N checks, M failed"; the exit code is the number of failures. */
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

static ULONGLONG as64(FILETIME t) { return ((ULONGLONG)t.dwHighDateTime << 32) | t.dwLowDateTime; }

int main(void) {
  CreateDirectoryA("C:\\Temp", NULL);
  const char *path = "C:\\Temp\\meta.txt";
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD n = 0;
  check(h != INVALID_HANDLE_VALUE && WriteFile(h, "abc", 3, &n, NULL), "make a file");
  CloseHandle(h);
  check(!(GetFileAttributesA(path) & FILE_ATTRIBUTE_READONLY), "a new file is not read-only");

  check(SetFileAttributesA(path, FILE_ATTRIBUTE_READONLY), "SetFileAttributes(READONLY)");
  DWORD a = GetFileAttributesA(path);
  check(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY), "GetFileAttributes sees it");
  SetLastError(0);
  h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  check(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "a read-only file cannot be opened for writing");
  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  check(h != INVALID_HANDLE_VALUE, "but it can be read");
  CloseHandle(h);

  check(SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL), "SetFileAttributes(NORMAL)");
  check(!(GetFileAttributesA(path) & FILE_ATTRIBUTE_READONLY), "clears it");
  h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  check(h != INVALID_HANDLE_VALUE, "and writing works again");

  /* times: 2020-01-01 00:00:00 UTC is 132223104000000000 in 100 ns units since 1601 */
  FILETIME written, created;
  const ULONGLONG t1 = 132223104000000000ull, t2 = 131000000000000000ull;
  written.dwLowDateTime = (DWORD)t1; written.dwHighDateTime = (DWORD)(t1 >> 32);
  created.dwLowDateTime = (DWORD)t2; created.dwHighDateTime = (DWORD)(t2 >> 32);
  check(SetFileTime(h, &created, NULL, &written), "SetFileTime");
  FILETIME c, w;
  check(GetFileTime(h, &c, NULL, &w), "GetFileTime");
  check(as64(w) == t1, "the written time is the one set");
  check(as64(c) == t2, "the creation time is the one set");
  CloseHandle(h);
  WIN32_FILE_ATTRIBUTE_DATA data;
  check(GetFileAttributesExA(path, GetFileExInfoStandard, &data) && as64(data.ftLastWriteTime) == t1, "and so does a lookup by name");

  SetLastError(0);
  check(!SetFileAttributesA("C:\\Temp\\nosuchfile.txt", FILE_ATTRIBUTE_NORMAL) && GetLastError() == ERROR_FILE_NOT_FOUND, "a missing file: ERROR_FILE_NOT_FOUND");
  DeleteFileA(path);
  printf("metatest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
