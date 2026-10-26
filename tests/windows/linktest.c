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

/* Hard and symbolic links on TheoryCraftOS (mingw gcc; see tests/windows/build.sh). Prints "linktest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (error %lu)\n", what, (unsigned long)GetLastError());
}

static int read_text(const char *path, char *buffer, DWORD capacity) {
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  DWORD got = 0;
  BOOL ok = ReadFile(h, buffer, capacity - 1, &got, NULL);
  CloseHandle(h);
  if (!ok) return -1;
  buffer[got] = 0;
  return (int)got;
}
static BOOL write_text(const char *path, const char *text) {
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) return FALSE;
  DWORD put = 0;
  const BOOL ok = WriteFile(h, text, (DWORD)strlen(text), &put, NULL) && put == strlen(text);
  CloseHandle(h);
  return ok;
}

int main(void) {
  char buffer[128];

  /* hard links */
  CreateDirectoryA("C:\\Temp\\links", NULL);
  check(write_text("C:\\Temp\\links\\original.txt", "hello"), "write the original file");
  check(CreateHardLinkA("C:\\Temp\\links\\also.txt", "C:\\Temp\\links\\original.txt", NULL), "CreateHardLink");
  check(!CreateHardLinkA("C:\\Temp\\links\\also.txt", "C:\\Temp\\links\\original.txt", NULL) && GetLastError() == ERROR_ALREADY_EXISTS, "a second one fails");
  check(!CreateHardLinkA("C:\\Temp\\links\\x.txt", "C:\\Temp\\links\\nosuch.txt", NULL) && GetLastError() == ERROR_FILE_NOT_FOUND, "linking to nothing");
  check(read_text("C:\\Temp\\links\\also.txt", buffer, sizeof(buffer)) == 5 && !strcmp(buffer, "hello"), "the new name reads the same file");
  check(write_text("C:\\Temp\\links\\also.txt", "HELLO WORLD"), "write through the new name");
  check(read_text("C:\\Temp\\links\\original.txt", buffer, sizeof(buffer)) == 11 && !strcmp(buffer, "HELLO WORLD"), "the original name sees it");
  check(DeleteFileA("C:\\Temp\\links\\original.txt"), "delete the original name");
  check(read_text("C:\\Temp\\links\\also.txt", buffer, sizeof(buffer)) == 11 && !strcmp(buffer, "HELLO WORLD"), "the data lived on through the other name");
  DWORD attr = GetFileAttributesA("C:\\Temp\\links\\also.txt");
  check(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY), "GetFileAttributes of a hard-linked file");

  /* symbolic links: relative and absolute */
  check(write_text("C:\\Temp\\links\\target.txt", "world"), "write the target file");
  check(CreateSymbolicLinkA("C:\\Temp\\links\\rel.lnk", "target.txt", 0), "CreateSymbolicLink, relative");
  check(CreateSymbolicLinkA("C:\\Temp\\links\\abs.lnk", "C:\\Temp\\links\\target.txt", 0), "CreateSymbolicLink, absolute");
  check(read_text("C:\\Temp\\links\\rel.lnk", buffer, sizeof(buffer)) == 5 && !strcmp(buffer, "world"), "reading through the relative link");
  check(read_text("C:\\Temp\\links\\abs.lnk", buffer, sizeof(buffer)) == 5 && !strcmp(buffer, "world"), "reading through the absolute link");
  check(write_text("C:\\Temp\\links\\rel.lnk", "WORLD!"), "writing through the relative link");
  check(read_text("C:\\Temp\\links\\target.txt", buffer, sizeof(buffer)) == 6 && !strcmp(buffer, "WORLD!"), "the target sees it");
  attr = GetFileAttributesA("C:\\Temp\\links\\rel.lnk");
  check(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY), "GetFileAttributes follows the link");

  /* a symbolic link to a directory can be traversed into */
  CreateDirectoryA("C:\\Temp\\links\\sub", NULL);
  check(write_text("C:\\Temp\\links\\sub\\inside.txt", "deep"), "a file inside the subdirectory");
  check(CreateSymbolicLinkA("C:\\Temp\\links\\sublink", "sub", SYMBOLIC_LINK_FLAG_DIRECTORY), "CreateSymbolicLink, a directory");
  check(read_text("C:\\Temp\\links\\sublink\\inside.txt", buffer, sizeof(buffer)) == 4 && !strcmp(buffer, "deep"), "traversing through a directory link");
  attr = GetFileAttributesA("C:\\Temp\\links\\sublink");
  check(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY), "it looks like a directory");

  /* a dangling symbolic link */
  check(CreateSymbolicLinkA("C:\\Temp\\links\\dangling.lnk", "nowhere.txt", 0), "a link to nothing yet");
  check(GetFileAttributesA("C:\\Temp\\links\\dangling.lnk") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND, "nothing there to follow it to");
  check(CreateFileA("C:\\Temp\\links\\dangling.lnk", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE, "opening it for data fails the same way");

  /* directory listing shows the link itself (FindFirstFile on an exact name does not follow) */
  WIN32_FIND_DATAA fd;
  HANDLE find = FindFirstFileA("C:\\Temp\\links\\rel.lnk", &fd);
  check(find != INVALID_HANDLE_VALUE && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "FindFirstFile shows the reparse bit");
  if (find != INVALID_HANDLE_VALUE) FindClose(find);

  printf("linktest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
