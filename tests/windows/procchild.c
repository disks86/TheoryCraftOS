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

/* The program processtest.exe starts with CreateProcess (mingw gcc; see tests/windows/build.sh). What it does is chosen by its first argument; the exit code is the answer. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc < 2) return 100;
  const char *what = argv[1];
  if (!strcmp(what, "exit")) return argc > 2 ? atoi(argv[2]) : 0;
  if (!strcmp(what, "args")) return argc == 4 && !strcmp(argv[2], "a b") && !strcmp(argv[3], "c") ? 42 : 1;
  if (!strcmp(what, "env")) { char v[16]; return GetEnvironmentVariableA("TCOS_CHILD", v, sizeof(v)) == 3 && !strcmp(v, "yes") ? 0 : 1; }
  if (!strcmp(what, "env2")) {
    char v[16];
    return GetEnvironmentVariableA("ONLY", v, sizeof(v)) == 1 && v[0] == '1' && GetEnvironmentVariableA("TCOS_CHILD", v, sizeof(v)) == 0 ? 0 : 1;
  }
  if (!strcmp(what, "cwd")) { char d[MAX_PATH]; DWORD n = GetCurrentDirectoryA(sizeof(d), d); return n == 10 && !strcmp(d, "C:\\Windows") ? 0 : 1; }
  if (!strcmp(what, "print")) { printf("child: hello from process %lu\n", (unsigned long)GetCurrentProcessId()); return 0; }
  if (!strcmp(what, "hello_out")) { printf("hello through a pipe, from process %lu\n", (unsigned long)GetCurrentProcessId()); fflush(stdout); return 5; }
  if (!strcmp(what, "upper_in")) {      /* standard input to upper case on standard output, until the end of the input */
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    char buffer[256];
    DWORD n, done, total = 0;
    while (ReadFile(in, buffer, sizeof(buffer), &n, NULL) && n > 0) {
      for (DWORD i = 0; i < n; ++i) if (buffer[i] >= 'a' && buffer[i] <= 'z') buffer[i] = (char)(buffer[i] - 32);
      WriteFile(out, buffer, n, &done, NULL);
      total += n;
    }
    return (int)(total & 0x7F);
  }
  if (!strcmp(what, "mapcheck")) {      /* the named mapping "TcosMap": check what the parent put there and answer in it */
    HANDLE m = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "TcosMap");
    if (!m) return 2;
    char *view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) return 3;
    if (memcmp(view, "TCOSMAP!", 8) != 0) return 4;
    memcpy(view + 64, "childack", 8);
    UnmapViewOfFile(view);
    CloseHandle(m);
    return 0;
  }
  if (!strcmp(what, "lockcheck")) {      /* the parent holds an exclusive lock on bytes 0..99 of C:\Temp\lock.bin and a shared one on 300..399 */
    HANDLE f = CreateFileA("C:\\Temp\\lock.bin", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 2;
    OVERLAPPED o;
    memset(&o, 0, sizeof(o));
    if (LockFileEx(f, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 50, 0, &o) || GetLastError() != ERROR_LOCK_VIOLATION) return 3;      /* inside the parent's exclusive lock */
    o.Offset = 200;
    if (!LockFileEx(f, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 50, 0, &o)) return 4;      /* free */
    o.Offset = 300;
    if (!LockFileEx(f, LOCKFILE_FAIL_IMMEDIATELY, 0, 50, 0, &o)) return 5;      /* shared with the parent's shared lock */
    o.Offset = 320;
    if (LockFileEx(f, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 10, 0, &o) || GetLastError() != ERROR_LOCK_VIOLATION) return 6;      /* exclusive over a shared one */
    return 0;
  }
  if (!strcmp(what, "grandchild")) {      /* starts a process that spins (the parent's job, if there is one, takes it too, unless asked to break away) and ends */
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    char command[] = "procchild.exe spin";
    DWORD flags = argc > 2 && !strcmp(argv[2], "breakaway") ? CREATE_BREAKAWAY_FROM_JOB : 0;
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi)) return GetLastError() == ERROR_ACCESS_DENIED ? 20 : GetLastError() == ERROR_NOT_ENOUGH_QUOTA ? 21 : 22;
    return 0;
  }
  if (!strcmp(what, "pid")) return (int)(GetCurrentProcessId() & 0x7F);
  if (!strcmp(what, "spin")) { volatile unsigned long n = 0; for (;;) n = n + 1; }
  if (!strcmp(what, "waitevent")) {
    HANDLE e = OpenEventA(SYNCHRONIZE, FALSE, "TcosProcessEvent");
    if (!e) return 2;
    return WaitForSingleObject(e, 20000) == WAIT_OBJECT_0 ? 3 : 4;
  }
  return 101;
}
