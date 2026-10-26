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

/* Static thread-local data of a DLL and of the program, side by side, in several threads (clang; the DLL is tlsdll.dll, which must sit next to the program). Prints
 * "tlsdlltest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>

__declspec(dllimport) int tlsdll_get(void);
__declspec(dllimport) void tlsdll_set(int);
__declspec(dllimport) int tlsdll_second(void);
__declspec(dllimport) int tlsdll_buffer_tail(void);
__declspec(dllimport) int tlsdll_buffer_head(void);

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}

__declspec(thread) int t_program = 100;

static DWORD WINAPI worker(LPVOID parameter) {
  const int mine = (int)(INT_PTR)parameter;
  DWORD bad = 0;      /* which part is wrong, as bits */
  if (tlsdll_get() != 42) bad |= 1;      /* a fresh copy */
  if (tlsdll_second() != 7) bad |= 2;
  if (tlsdll_buffer_tail() != 0 || tlsdll_buffer_head() != 0) bad |= 4;
  if (t_program != 100) { bad |= 8; printf("t_program starts as %d\n", t_program); }
  tlsdll_set(mine);
  t_program = mine * 3;
  for (int i = 0; i < 30; ++i) SwitchToThread();
  if (tlsdll_get() != mine) bad |= 16;
  if (tlsdll_second() != mine + 1) bad |= 32;
  if (tlsdll_buffer_tail() != (char)mine) bad |= 64;
  if (t_program != mine * 3) bad |= 128;
  return bad;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tlsdlltest starting (the program's thread-local starts as %d)\n", t_program);
  check(tlsdll_get() == 42 && tlsdll_second() == 7 && tlsdll_buffer_tail() == 0, "the DLL's data starts from its template in the main thread");
  tlsdll_set(5);
  t_program = 9;
  check(tlsdll_get() == 5 && tlsdll_second() == 6 && tlsdll_buffer_tail() == 5, "the main thread changes its copy");
  HANDLE h[4];
  for (int i = 0; i < 4; ++i) h[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)(i + 10), 0, NULL);
  WaitForMultipleObjects(4, h, TRUE, INFINITE);
  int ok = 1;
  for (int i = 0; i < 4; ++i) { DWORD code = 0; GetExitCodeThread(h[i], &code); ok &= code == 0; if (code) printf("thread %d wrong parts 0x%lx\n", i, code); CloseHandle(h[i]); }
  check(ok, "four threads each have their own copy of the DLL's and the program's data");
  check(tlsdll_get() == 5 && tlsdll_second() == 6 && t_program == 9, "the main thread's copies are untouched");
  printf("tlsdlltest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
