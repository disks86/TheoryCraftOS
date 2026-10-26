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

/* Threads of one process on different cores (clang, for __try; see tests/windows/build.sh): they run at the same time, interlocked operations and critical sections hold
 * up, and a page that one thread frees is gone for all the others the moment VirtualFree returns. Prints "smptest: N checks, M failed"; the exit code is the number of
 * failures. Needs more than one core (the machine has four in the test setup). */
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

static unsigned apic_id(void) {
  unsigned a = 1, b, c = 0, d;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  return b >> 24;
}

#define THREADS 3
static volatile LONG g_stop;
static volatile LONG g_ready;
static volatile LONG64 g_ids[THREADS];      /* bit n: the thread was seen on the core with APIC id n */
static volatile LONG64 g_progress[THREADS];

static DWORD WINAPI spinner(LPVOID parameter) {
  const int me = (int)(INT_PTR)parameter;
  InterlockedIncrement(&g_ready);
  while (!g_stop) {
    const unsigned id = apic_id();
    if (id < 63) InterlockedOr64(&g_ids[me], 1ll << id);
    InterlockedIncrement64(&g_progress[me]);
  }
  return 0;
}

static LONG64 popcount(LONG64 v) { LONG64 n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

/* ---- interlocked and critical sections ---- */
#define ROUNDS 20000
static volatile LONG g_atomic;
static LONG g_plain;      /* guarded by the critical section */
static CRITICAL_SECTION g_cs;
static DWORD WINAPI counter(LPVOID parameter) {
  (void)parameter;
  for (int i = 0; i < ROUNDS; ++i) {
    InterlockedIncrement(&g_atomic);
    EnterCriticalSection(&g_cs);
    LONG v = g_plain;
    if ((i & 255) == 0) Sleep(0);      /* (holds the lock across a reschedule now and then) */
    g_plain = v + 1;
    LeaveCriticalSection(&g_cs);
  }
  return 0;
}

/* ---- a page that is freed is gone everywhere ---- */
static char *volatile g_region;
static volatile LONG g_round, g_freed_round, g_stale, g_checked;
static volatile LONG g_touched[THREADS];
/* (clang only covers calls with a __try: the access is in a function of its own) */
__attribute__((noinline)) static void touch(char *p) { *(volatile char *)p = 1; }

static DWORD WINAPI toucher(LPVOID parameter) {
  const int me = (int)(INT_PTR)parameter;
  while (!g_stop) {
    const LONG round = g_round;
    char *p = g_region;
    if (!p || round == 0) continue;
    const LONG freed = g_freed_round;      /* (read before the access: if the page was freed by then, the access must fail) */
    int ok;
    __try {
      touch(p);
      ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = 0;
    }
    if (ok) g_touched[me] = round;
    if (freed >= round) {
      InterlockedIncrement(&g_checked);
      if (ok) InterlockedIncrement(&g_stale);
    }
  }
  return 0;
}

int main(void) {
  HANDLE threads[THREADS];
  /* four cores' worth of spinning threads */
  for (int i = 0; i < THREADS; ++i) threads[i] = CreateThread(NULL, 0, spinner, (LPVOID)(INT_PTR)i, 0, NULL);
  while (g_ready < THREADS) Sleep(1);
  Sleep(300);
  LONG64 seen = 0;
  for (int i = 0; i < THREADS; ++i) seen |= g_ids[i];
  printf("cores seen: %lld\n", popcount(seen));
  check(popcount(seen) >= 2, "the threads ran on at least two different cores");
  LONG64 before[THREADS];
  for (int i = 0; i < THREADS; ++i) before[i] = g_progress[i];
  Sleep(200);
  int moving = 0;
  for (int i = 0; i < THREADS; ++i) moving += g_progress[i] > before[i];
  check(moving == THREADS, "all of them make progress at the same time");
  g_stop = 1;
  WaitForMultipleObjects(THREADS, threads, TRUE, 10000);
  for (int i = 0; i < THREADS; ++i) CloseHandle(threads[i]);

  /* atomic and locked counting from four threads */
  InitializeCriticalSection(&g_cs);
  HANDLE workers[4];
  for (int i = 0; i < 4; ++i) workers[i] = CreateThread(NULL, 0, counter, NULL, 0, NULL);
  check(WaitForMultipleObjects(4, workers, TRUE, 60000) == WAIT_OBJECT_0, "four counting threads finish");
  check(g_atomic == 4 * ROUNDS, "InterlockedIncrement loses nothing");
  check(g_plain == 4 * ROUNDS, "a critical section protects a plain counter");
  for (int i = 0; i < 4; ++i) CloseHandle(workers[i]);

  /* freeing memory while other threads run on other cores */
  g_stop = 0;
  for (int i = 0; i < THREADS; ++i) threads[i] = CreateThread(NULL, 0, toucher, (LPVOID)(INT_PTR)i, 0, NULL);
  int touched_every_time = 1;
  for (LONG round = 1; round <= 300; ++round) {
    char *region = VirtualAlloc(NULL, 64 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!region) { check(0, "VirtualAlloc"); break; }
    g_region = region;
    g_round = round;
    DWORD waited = 0;
    for (int i = 0; i < THREADS;) {
      if (g_touched[i] == round) { ++i; continue; }
      Sleep(0);
      if (++waited > 200000) { touched_every_time = 0; break; }
    }
    if (!touched_every_time) break;
    VirtualFree(region, 0, MEM_RELEASE);
    InterlockedExchange(&g_freed_round, round);
    Sleep(0);
  }
  g_stop = 1;
  WaitForMultipleObjects(THREADS, threads, TRUE, 10000);
  for (int i = 0; i < THREADS; ++i) CloseHandle(threads[i]);
  check(touched_every_time, "every thread saw every new region");
  printf("accesses after free: %ld, stale: %ld\n", (long)g_checked, (long)g_stale);
  check(g_checked > 0, "threads kept testing the freed regions");
  check(g_stale == 0, "no thread can use a page after VirtualFree returned");

  printf("smptest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
