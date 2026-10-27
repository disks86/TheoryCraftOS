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

/* Windows threads without structured exceptions, for running on the CPU emulator (tests/emulated_windows.sh): CreateThread, waits, critical sections, events, TLS,
 * TerminateThread, ExitThread. Prints "emuthreads: N checks, M failed". */
#include <windows.h>
#include <stdio.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (!ok) { ++g_failed; printf("FAIL: %s\n", what); }
}

#define WORKERS 4
#define ROUNDS 2000
static CRITICAL_SECTION g_cs;
static volatile LONG g_atomic;
static LONG g_plain;
static DWORD g_tls;
static HANDLE g_start, g_done;
static DWORD g_ids[WORKERS];

static DWORD WINAPI worker(LPVOID param) {
  const DWORD index = (DWORD)(ULONG_PTR)param;
  g_ids[index] = GetCurrentThreadId();
  TlsSetValue(g_tls, (LPVOID)(ULONG_PTR)(index + 100));
  WaitForSingleObject(g_start, INFINITE);
  for (int i = 0; i < ROUNDS; ++i) {
    EnterCriticalSection(&g_cs);
    g_plain = g_plain + 1;
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_atomic);
  }
  return (DWORD)(ULONG_PTR)TlsGetValue(g_tls);
}

static DWORD WINAPI sleeper(LPVOID param) {
  (void)param;
  Sleep(30);
  return 5;
}

static DWORD WINAPI forever(LPVOID param) {
  (void)param;
  for (;;) Sleep(5);
  return 0;
}

static DWORD WINAPI exits_early(LPVOID param) {
  (void)param;
  ExitThread(77);
  return 0;
}

static DWORD WINAPI signaller(LPVOID param) {
  Sleep(10);
  SetEvent((HANDLE)param);
  return 0;
}

int main(void) {
  InitializeCriticalSection(&g_cs);
  g_tls = TlsAlloc();
  g_start = CreateEventA(NULL, TRUE, FALSE, NULL);
  g_done = CreateEventA(NULL, FALSE, FALSE, NULL);
  check(g_tls != TLS_OUT_OF_INDEXES && g_start && g_done, "setup");

  HANDLE threads[WORKERS];
  for (int i = 0; i < WORKERS; ++i) threads[i] = CreateThread(NULL, 0, worker, (LPVOID)(ULONG_PTR)i, 0, NULL);
  int created = 0;
  for (int i = 0; i < WORKERS; ++i) created += threads[i] != NULL;
  check(created == WORKERS, "CreateThread x4");
  SetEvent(g_start);
  check(WaitForMultipleObjects(WORKERS, threads, TRUE, 20000) == WAIT_OBJECT_0, "WaitForMultipleObjects all");
  check(g_plain == WORKERS * ROUNDS, "critical section keeps the counter exact");
  check(g_atomic == WORKERS * ROUNDS, "InterlockedIncrement is atomic");
  int tls_ok = 1, distinct = 1;
  for (int i = 0; i < WORKERS; ++i) {
    DWORD code = 0;
    GetExitCodeThread(threads[i], &code);
    if (code != (DWORD)(i + 100)) tls_ok = 0;
    for (int j = 0; j < i; ++j) if (g_ids[i] == g_ids[j]) distinct = 0;
    if (g_ids[i] == 0 || g_ids[i] == GetCurrentThreadId()) distinct = 0;
    CloseHandle(threads[i]);
  }
  check(tls_ok, "thread-local storage is per thread");
  check(distinct, "thread ids differ");

  HANDLE s = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
  check(WaitForSingleObject(s, 10000) == WAIT_OBJECT_0, "a thread that sleeps ends");
  DWORD code = 0;
  GetExitCodeThread(s, &code);
  check(code == 5, "exit code 5");
  CloseHandle(s);

  HANDLE e = CreateThread(NULL, 0, exits_early, NULL, 0, NULL);
  WaitForSingleObject(e, 10000);
  GetExitCodeThread(e, &code);
  check(code == 77, "ExitThread(77)");
  CloseHandle(e);

  HANDLE f = CreateThread(NULL, 0, forever, NULL, 0, NULL);
  Sleep(20);
  GetExitCodeThread(f, &code);
  check(code == STILL_ACTIVE, "a running thread is STILL_ACTIVE");
  check(WaitForSingleObject(f, 20) == WAIT_TIMEOUT, "waiting for it times out");
  check(TerminateThread(f, 9), "TerminateThread");
  check(WaitForSingleObject(f, 10000) == WAIT_OBJECT_0, "the terminated thread ends");
  GetExitCodeThread(f, &code);
  check(code == 9, "its exit code is 9");
  CloseHandle(f);

  HANDLE g = CreateThread(NULL, 0, signaller, g_done, 0, NULL);
  check(WaitForSingleObject(g_done, 10000) == WAIT_OBJECT_0, "an event set by another thread wakes the waiter");
  CloseHandle(g);

  printf("emuthreads: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
