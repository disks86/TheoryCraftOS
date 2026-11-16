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

/* A 32-bit Windows program built against the old C runtime (msvcrt.dll itself, not the universal runtime's api sets), for tests/wow64_qemu.sh
 * (docs/Wow64.md). Such a program finds its standard streams by itself: stdout is 32 bytes after what __p__iob() gives it, stderr 64. Everything it
 * prints goes through that, so the lines the test looks for are themselves the first check. Ends with exit code 42 if every check passed.
 *   i686-w64-mingw32-gcc -O1 -mcrtdll=msvcrt-os -o old32.exe tests/windows/old32.c */
#include <stdio.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "old32: ok: %s\n" : "old32: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

int main(void) {
  /* (32 bytes in a 32-bit program; the same source built for x86-64 is a test of the 64-bit msvcrt.dll, where it is 48) */
  check(sizeof(FILE) == (sizeof(void *) == 8 ? 48 : 32) && (char *)stdout - (char *)stdin == sizeof(FILE) && (char *)stderr - (char *)stdin == 2 * sizeof(FILE),
        "the standard streams are one FILE apart");
  check(_fileno(stdin) == 0 && _fileno(stdout) == 1 && _fileno(stderr) == 2, "their file numbers are 0, 1 and 2");
  check(fprintf(stderr, "old32: (a line on stderr)\n") > 0 && !ferror(stderr), "fprintf to stderr");
  check(fputs("old32: (a line with fputs)\n", stdout) >= 0 && fflush(stdout) == 0, "fputs and fflush on stdout");

  /* a file through a stream of the runtime's own, beside the three */
  char path[MAX_PATH];
  GetTempPathA(MAX_PATH, path);
  strcat(path, "old32.txt");
  FILE *file = fopen(path, "w+");
  check(file != NULL && file != stdout && file != stderr, "fopen");
  char line[32] = "";
  check(file && fprintf(file, "%d %s\n", 1234, "words") == 11 && fseek(file, 0, SEEK_SET) == 0 && fgets(line, sizeof(line), file) && strcmp(line, "1234 words\n") == 0,
        "what was written to a file is read back");
  if (file) fclose(file);

  /* a Windows handle made into a file number: the handle is as wide as a pointer on the stack */
  const HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  const int number = handle != INVALID_HANDLE_VALUE ? _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY) : -1;
  char bytes[8] = "";
  check(number >= 3 && (HANDLE)_get_osfhandle(number) == handle && _read(number, bytes, 4) == 4 && memcmp(bytes, "1234", 4) == 0,
        "_open_osfhandle with its flags, _get_osfhandle and _read");
  if (number >= 0) _close(number);
  DeleteFileA(path);

  if (g_failed == 0) printf("old32: all %d checks passed\n", g_passed);
  else printf("old32: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
