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

/* TLS callbacks (and DllMain-style notifications) on TheoryCraftOS: the program's TLS callbacks hear about the process starting and ending and about every thread
 * starting and ending, in the thread concerned. Built with the usual mingw start-up code, which has its own TLS callbacks too (see tests/windows/build.sh). Prints
 * "tlscbtest: N checks, M failed"; the exit code is the number of failures. */
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

static volatile LONG g_process_attach, g_thread_attach, g_thread_detach, g_process_detach;
static volatile LONG g_attach_in_right_thread, g_detach_in_right_thread, g_process_attach_before_main;
static volatile DWORD g_last_attach_thread, g_last_detach_thread;
static DWORD g_main_thread;
static volatile LONG g_main_started;

static void NTAPI callback(PVOID module, DWORD reason, PVOID reserved) {
  (void)module;
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      InterlockedIncrement(&g_process_attach);
      g_process_attach_before_main = !g_main_started;
      break;
    case DLL_THREAD_ATTACH:
      InterlockedIncrement(&g_thread_attach);
      g_last_attach_thread = GetCurrentThreadId();
      InterlockedIncrement(&g_attach_in_right_thread);
      break;
    case DLL_THREAD_DETACH:
      InterlockedIncrement(&g_thread_detach);
      g_last_detach_thread = GetCurrentThreadId();
      InterlockedIncrement(&g_detach_in_right_thread);
      break;
    case DLL_PROCESS_DETACH: {
      InterlockedIncrement(&g_process_detach);
      /* the process is ending (reserved says so): straight to the console, the C library is gone */
      const char line[] = "tlscbtest: the process-detach callback ran\n";
      DWORD n;
      WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, sizeof(line) - 1, &n, NULL);
      (void)reserved;
      break;
    }
  }
}
/* (the section the C runtime gathers the TLS callbacks from) */
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK g_callback = callback;
/* a thread-local variable makes sure the program has a TLS directory */
__thread int g_thread_local = 3;

static DWORD g_thread_id_seen_by_thread[4];
static DWORD WINAPI worker(LPVOID parameter) {
  const int index = (int)(INT_PTR)parameter;
  g_thread_id_seen_by_thread[index] = GetCurrentThreadId();
  Sleep(5);
  return (DWORD)index;
}
static DWORD WINAPI exits_by_itself(LPVOID parameter) {
  (void)parameter;
  ExitThread(9);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  g_main_thread = GetCurrentThreadId();
  g_main_started = 1;
  printf("tlscbtest starting\n");
  check(g_process_attach == 1, "the process-attach callback ran once");
  check(g_process_attach_before_main, "and before main");
  check(g_thread_attach == 0, "the main thread gets no thread-attach");

  HANDLE threads[4];
  for (int i = 0; i < 4; ++i) threads[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
  WaitForMultipleObjects(4, threads, TRUE, INFINITE);
  for (int i = 0; i < 4; ++i) CloseHandle(threads[i]);
  check(g_thread_attach == 4, "four thread-attach callbacks");
  check(g_thread_detach == 4, "four thread-detach callbacks, finished before the threads count as ended");
  check(g_last_attach_thread != 0 && g_last_attach_thread != g_main_thread, "attach ran in the new thread, not in main");
  check(g_last_detach_thread != 0 && g_last_detach_thread != g_main_thread, "detach ran in the ending thread");

  HANDLE h = CreateThread(NULL, 0, exits_by_itself, NULL, 0, NULL);
  WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
  check(g_thread_attach == 5 && g_thread_detach == 5, "ExitThread also announces the end");

  /* a thread started suspended is announced when it starts to run */
  const LONG before = g_thread_attach;
  h = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)0, CREATE_SUSPENDED, NULL);
  Sleep(20);
  check(g_thread_attach == before, "no attach callback for a thread that has not started");
  ResumeThread(h);
  WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
  check(g_thread_attach == before + 1 && g_thread_detach == before + 1, "it comes when the thread runs");

  /* thread-local data still works with callbacks in play */
  g_thread_local = 11;
  check(g_thread_local == 11, "a thread-local variable");

  printf("tlscbtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;      /* (the process-detach callback runs after this) */
}
