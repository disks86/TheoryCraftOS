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
  if (!strcmp(what, "pid")) return (int)(GetCurrentProcessId() & 0x7F);
  if (!strcmp(what, "spin")) { volatile unsigned long n = 0; for (;;) n = n + 1; }
  if (!strcmp(what, "waitevent")) {
    HANDLE e = OpenEventA(SYNCHRONIZE, FALSE, "TcosProcessEvent");
    if (!e) return 2;
    return WaitForSingleObject(e, 20000) == WAIT_OBJECT_0 ? 3 : 4;
  }
  return 101;
}
