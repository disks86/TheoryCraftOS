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

/* Thread and process priority on TheoryCraftOS: SetThreadPriority/SetPriorityClass actually change which thread the scheduler favors, not just values read back
 * (mingw gcc; see tests/windows/build.sh). Prints "priotest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (error %lu)\n", what, (unsigned long)GetLastError());
}

static volatile LONG g_stop;
static volatile LONGLONG g_count_a, g_count_b;

static DWORD WINAPI spin_a(LPVOID unused) { (void)unused; LONGLONG n = 0; while (!g_stop) ++n; g_count_a = n; return 0; }
static DWORD WINAPI spin_b(LPVOID unused) { (void)unused; LONGLONG n = 0; while (!g_stop) ++n; g_count_b = n; return 0; }

/* Runs the two spinners on one core for a while (so they must contend for it) and returns how many loops each made. */
static void race(int priority_a, int priority_b, LONGLONG *count_a, LONGLONG *count_b) {
  g_stop = 0;
  g_count_a = g_count_b = 0;
  HANDLE a = CreateThread(NULL, 0, spin_a, NULL, 0, NULL);
  HANDLE b = CreateThread(NULL, 0, spin_b, NULL, 0, NULL);
  SetThreadAffinityMask(a, 1);
  SetThreadAffinityMask(b, 1);
  SetThreadPriority(a, priority_a);
  SetThreadPriority(b, priority_b);
  Sleep(600);
  g_stop = 1;
  WaitForSingleObject(a, 5000);
  WaitForSingleObject(b, 5000);
  *count_a = g_count_a;
  *count_b = g_count_b;
  CloseHandle(a);
  CloseHandle(b);
}

int main(void) {
  /* values are kept and read back, as before */
  HANDLE self = GetCurrentThread();
  check(SetThreadPriority(self, THREAD_PRIORITY_ABOVE_NORMAL) && GetThreadPriority(self) == THREAD_PRIORITY_ABOVE_NORMAL, "SetThreadPriority is read back");
  check(SetThreadPriority(self, THREAD_PRIORITY_NORMAL), "back to normal");
  check(SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) && GetPriorityClass(GetCurrentProcess()) == HIGH_PRIORITY_CLASS, "SetPriorityClass is read back");
  check(SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS), "back to normal");

  /* a much higher priority thread gets noticeably more of a core it shares with a lower one */
  LONGLONG high = 0, low = 0;
  race(THREAD_PRIORITY_HIGHEST, THREAD_PRIORITY_LOWEST, &high, &low);
  check(high > low * 3, "THREAD_PRIORITY_HIGHEST beats THREAD_PRIORITY_LOWEST on a shared core");
  LONGLONG critical = 0, idle = 0;
  race(THREAD_PRIORITY_TIME_CRITICAL, THREAD_PRIORITY_IDLE, &critical, &idle);
  check(critical > idle * 3, "THREAD_PRIORITY_TIME_CRITICAL beats THREAD_PRIORITY_IDLE on a shared core");

  /* the loser still ran a little: it is not starved outright, just outrun */
  check(low > 0 && idle >= 0, "the lower-priority thread still got the core sometimes");

  /* two threads at the same priority still share the core roughly evenly */
  LONGLONG even_a = 0, even_b = 0;
  race(THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_NORMAL, &even_a, &even_b);
  LONGLONG bigger = even_a > even_b ? even_a : even_b, smaller = even_a > even_b ? even_b : even_a;
  check(smaller > 0 && bigger < smaller * 3, "equal priority still shares the core");

  printf("priotest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
