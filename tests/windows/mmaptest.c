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

/* File mappings on TheoryCraftOS: CreateFileMapping / MapViewOfFile on memory, by name between processes, and on files (mingw gcc; see tests/windows/build.sh; procchild.exe
 * has to be in C:\Windows\System32). Prints "mmaptest: N checks, M failed"; the exit code is the number of failures. */
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

static int mapped(const void *p) {
  MEMORY_BASIC_INFORMATION info;
  return VirtualQuery(p, &info, sizeof(info)) && info.State != MEM_FREE;
}

static volatile LONG g_go, g_seen;
static DWORD WINAPI watcher(LPVOID p) {
  volatile char *view = p;
  while (!g_go) Sleep(1);
  for (int i = 0; i < 2000 && view[100] != 'Z'; ++i) Sleep(1);
  g_seen = view[100] == 'Z';
  view[101] = 'Y';
  return 0;
}

int main(void) {
  /* memory, two views of one mapping */
  HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
  check(m != NULL, "CreateFileMapping on memory");
  char *a = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  char *b = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  check(a && b && a != b, "two views of it");
  check(a && a[0] == 0 && a[65535] == 0, "memory starts as zeros");
  memcpy(a, "shared!", 8);
  check(b && !memcmp(b, "shared!", 8), "what one view gets, the other has");
  char *part = MapViewOfFile(m, FILE_MAP_READ, 0, 4096, 4096);
  check(part != NULL, "a view of part of it (from offset 4096)");
  a[4096] = 'q';
  check(part && part[0] == 'q', "which is the same memory");
  check(UnmapViewOfFile(part), "UnmapViewOfFile");
  check(!mapped(part), "the pages are gone");
  SetLastError(0);
  check(!UnmapViewOfFile(a + 8) && GetLastError() == ERROR_INVALID_ADDRESS, "unmapping something that is no view: ERROR_INVALID_ADDRESS");
  check(UnmapViewOfFile(b), "unmap the second view");
  check(a[0] == 's', "the first view is still there");
  /* a thread on another core sees the writes, and the other way round */
  g_go = 0;
  HANDLE t = CreateThread(NULL, 0, watcher, a, 0, NULL);
  g_go = 1;
  Sleep(50);
  a[100] = 'Z';
  WaitForSingleObject(t, 20000);
  check(g_seen && a[101] == 'Y', "a thread sees a write to the view and writes back");
  CloseHandle(t);
  check(UnmapViewOfFile(a), "unmap the first");
  CloseHandle(m);

  /* by name, between processes */
  HANDLE named = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "TcosMap");
  HANDLE same = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "TcosMap");
  check(named && same && GetLastError() == ERROR_ALREADY_EXISTS, "creating a mapping by a name that exists: the same one, ERROR_ALREADY_EXISTS");
  CloseHandle(same);
  char *view = MapViewOfFile(named, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  memcpy(view, "TCOSMAP!", 8);
  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  char command[] = "procchild.exe mapcheck";
  check(CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "start a child that opens the mapping");
  WaitForSingleObject(pi.hProcess, 20000);
  DWORD code = 99;
  GetExitCodeProcess(pi.hProcess, &code);
  check(code == 0, "the child found the data (exit code 0)");
  check(!memcmp(view + 64, "childack", 8), "and its answer is in the parent's view");
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  UnmapViewOfFile(view);
  CloseHandle(named);
  SetLastError(0);
  check(OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "TcosMap") == NULL && GetLastError() == ERROR_FILE_NOT_FOUND, "when the last handle and view are gone, so is the name");

  /* a file */
  CreateDirectoryA("C:\\Temp", NULL);
  HANDLE f = CreateFileA("C:\\Temp\\map.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  char data[5000];
  for (int i = 0; i < 5000; ++i) data[i] = (char)(i * 7);
  DWORD n = 0;
  WriteFile(f, data, sizeof(data), &n, NULL);
  HANDLE fm = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 0, NULL);
  check(fm != NULL, "CreateFileMapping on a file");
  unsigned char *fv = MapViewOfFile(fm, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  check(fv != NULL && !memcmp(fv, data, 5000), "the view has the file's contents");
  fv[0] = 0x55;
  fv[4999] = 0x66;
  check(FlushViewOfFile(fv, 0), "FlushViewOfFile");
  char back[5000];
  SetFilePointer(f, 0, NULL, FILE_BEGIN);
  ReadFile(f, back, sizeof(back), &n, NULL);
  check(n == 5000 && (unsigned char)back[0] == 0x55 && (unsigned char)back[4999] == 0x66 && back[1] == data[1], "and the file has what the view got");
  fv[10] = 0x77;
  UnmapViewOfFile(fv);
  CloseHandle(fm);
  SetFilePointer(f, 0, NULL, FILE_BEGIN);
  ReadFile(f, back, sizeof(back), &n, NULL);
  check((unsigned char)back[10] == 0x77, "closing the last view and the mapping writes the file back");
  CloseHandle(f);

  /* a copy-on-write view changes nothing else */
  f = CreateFileA("C:\\Temp\\map.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  fm = CreateFileMappingA(f, NULL, PAGE_WRITECOPY, 0, 0, NULL);
  unsigned char *cv = MapViewOfFile(fm, FILE_MAP_COPY, 0, 0, 0);
  check(cv != NULL, "a copy-on-write view");
  cv[20] = 0x11;
  UnmapViewOfFile(cv);
  CloseHandle(fm);
  SetFilePointer(f, 0, NULL, FILE_BEGIN);
  ReadFile(f, back, sizeof(back), &n, NULL);
  check((unsigned char)back[20] == (unsigned char)data[20], "its changes do not reach the file");
  /* a read-only mapping does not give a writable view */
  fm = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
  SetLastError(0);
  check(MapViewOfFile(fm, FILE_MAP_WRITE, 0, 0, 0) == NULL && GetLastError() == ERROR_ACCESS_DENIED, "a writable view of a read-only mapping: ERROR_ACCESS_DENIED");
  unsigned char *ro = MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0);
  check(ro != NULL && ro[1] == (unsigned char)data[1], "but a read-only view works");
  UnmapViewOfFile(ro);
  CloseHandle(fm);
  /* a mapping bigger than the file makes the file bigger */
  fm = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 12000, NULL);
  unsigned char *big = MapViewOfFile(fm, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  check(big != NULL && big[11999] == 0 && big[100] == (unsigned char)data[100], "a mapping of 12000 bytes over a 5000 byte file");
  big[11999] = 0x42;
  UnmapViewOfFile(big);
  CloseHandle(fm);
  check(GetFileSize(f, NULL) == 12000, "makes the file 12000 bytes");
  CloseHandle(f);
  DeleteFileA("C:\\Temp\\map.bin");

  /* the Ex functions for this process */
  LPVOID block = VirtualAllocEx(GetCurrentProcess(), NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  check(block != NULL && VirtualFreeEx(GetCurrentProcess(), block, 0, MEM_RELEASE), "VirtualAllocEx and VirtualFreeEx on this process");
  printf("mmaptest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
