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

/* Threads on TheoryCraftOS's kernel32.dll (see tests/windows/build.sh; built with clang for __declspec(thread) and __try). Prints "threadtest: N checks, M failed"; the exit
 * code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
static volatile LONG g_check_lock;
static void check(int ok, const char *what) {
  while (InterlockedExchange(&g_check_lock, 1)) SwitchToThread();
  ++g_checks;
  if (!ok) { ++g_failed; printf("FAIL: %s\n", what); }
  InterlockedExchange(&g_check_lock, 0);
}

static volatile LONG g_ran, g_counter, g_atomic_counter;
static DWORD g_main_id;
static CRITICAL_SECTION g_cs;

static DWORD WINAPI set_flag(LPVOID parameter) {
  *(volatile int *)parameter = 42;
  g_ran = 1;
  return 7;
}
static void test_basic(void) {
  volatile int value = 0;
  DWORD id = 0;
  HANDLE h = CreateThread(NULL, 0, set_flag, (LPVOID)&value, 0, &id);
  check(h != NULL, "CreateThread");
  if (!h) return;
  check(WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0, "WaitForSingleObject on a thread");
  check(value == 42 && g_ran == 1, "the thread ran with its parameter");
  DWORD code = 0;
  check(GetExitCodeThread(h, &code) && code == 7, "GetExitCodeThread");
  check(id != 0 && id != g_main_id, "the thread has an id of its own");
  check(GetThreadId(h) == id, "GetThreadId");
  CloseHandle(h);
}

static DWORD WINAPI id_reporter(LPVOID parameter) { *(DWORD *)parameter = GetCurrentThreadId(); return 0; }
static DWORD WINAPI count_with_section(LPVOID parameter) {
  (void)parameter;
  for (int i = 0; i < 2000; ++i) {
    EnterCriticalSection(&g_cs);
    LONG v = g_counter;
    if (i % 100 == 0) SwitchToThread();      /* give the other threads a chance to collide */
    g_counter = v + 1;
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_atomic_counter);
  }
  return 0;
}
static void test_shared_counter(void) {
  HANDLE threads[8];
  for (int i = 0; i < 8; ++i) threads[i] = CreateThread(NULL, 0, count_with_section, NULL, 0, NULL);
  int ok = 1;
  for (int i = 0; i < 8; ++i) ok &= threads[i] != NULL;
  check(ok, "eight threads");
  check(WaitForMultipleObjects(8, threads, TRUE, INFINITE) == WAIT_OBJECT_0, "WaitForMultipleObjects (all) on threads");
  check(g_counter == 16000, "the critical section keeps the count exact");
  check(g_atomic_counter == 16000, "InterlockedIncrement keeps the count exact");
  for (int i = 0; i < 8; ++i) if (threads[i]) CloseHandle(threads[i]);

  DWORD ids[4] = {0};
  HANDLE t4[4];
  for (int i = 0; i < 4; ++i) t4[i] = CreateThread(NULL, 0, id_reporter, &ids[i], 0, NULL);
  WaitForMultipleObjects(4, t4, TRUE, INFINITE);
  int distinct = 1;
  for (int i = 0; i < 4; ++i) { for (int j = i + 1; j < 4; ++j) distinct &= ids[i] != ids[j]; distinct &= ids[i] != 0 && ids[i] != g_main_id; CloseHandle(t4[i]); }
  check(distinct, "every thread has its own id");
}

static __declspec(thread) int t_value = 5;
static __declspec(thread) char t_buffer[64];
static DWORD g_tls_index;
static DWORD WINAPI tls_thread(LPVOID parameter) {
  const int mine = (int)(INT_PTR)parameter;
  const int initial = t_value;      /* each thread starts from the template */
  t_value = mine * 10;
  memset(t_buffer, mine, sizeof(t_buffer));
  TlsSetValue(g_tls_index, (LPVOID)(INT_PTR)mine);
  for (int i = 0; i < 50; ++i) SwitchToThread();      /* the others get to write theirs */
  const int ok = initial == 5 && t_value == mine * 10 && (int)(INT_PTR)TlsGetValue(g_tls_index) == mine && t_buffer[0] == (char)mine && t_buffer[63] == (char)mine;
  return ok ? 1 : 0;
}
static void test_tls(void) {
  g_tls_index = TlsAlloc();
  check(g_tls_index != TLS_OUT_OF_INDEXES, "TlsAlloc");
  t_value = 99;
  TlsSetValue(g_tls_index, (LPVOID)(INT_PTR)1234);
  HANDLE h[4];
  for (int i = 0; i < 4; ++i) h[i] = CreateThread(NULL, 0, tls_thread, (LPVOID)(INT_PTR)(i + 1), 0, NULL);
  WaitForMultipleObjects(4, h, TRUE, INFINITE);
  int ok = 1;
  for (int i = 0; i < 4; ++i) { DWORD code = 0; GetExitCodeThread(h[i], &code); ok &= code == 1; CloseHandle(h[i]); }
  check(ok, "__declspec(thread) data and TlsGetValue are per thread");
  check(t_value == 99 && (int)(INT_PTR)TlsGetValue(g_tls_index) == 1234, "the main thread's own values are untouched");
  TlsFree(g_tls_index);
}

static DWORD WINAPI exits_early(LPVOID parameter) {
  (void)parameter;
  ExitThread(33);
  return 1;
}
static void test_suspended_and_exit(void) {
  g_ran = 0;
  volatile int value = 0;
  HANDLE h = CreateThread(NULL, 0, set_flag, (LPVOID)&value, CREATE_SUSPENDED, NULL);
  check(h != NULL, "CreateThread suspended");
  Sleep(20);
  check(g_ran == 0 && value == 0, "a suspended thread does not run");
  DWORD code = 0;
  GetExitCodeThread(h, &code);
  check(code == STILL_ACTIVE, "STILL_ACTIVE for a thread that has not run");
  check(WaitForSingleObject(h, 10) == WAIT_TIMEOUT, "waiting for it times out");
  check(ResumeThread(h) == 1, "ResumeThread returns the previous count");
  check(WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0 && g_ran == 1 && value == 42, "it runs after ResumeThread");
  CloseHandle(h);

  h = CreateThread(NULL, 0, exits_early, NULL, 0, NULL);
  WaitForSingleObject(h, INFINITE);
  GetExitCodeThread(h, &code);
  check(code == 33, "ExitThread sets the exit code");
  CloseHandle(h);
}

static DWORD WINAPI quick(LPVOID parameter) { return (DWORD)(INT_PTR)parameter; }
static void test_many(void) {
  int ok = 1;
  for (int i = 0; i < 100; ++i) {
    HANDLE h = CreateThread(NULL, 0, quick, (LPVOID)(INT_PTR)i, 0, NULL);
    if (!h) { ok = 0; break; }
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(h, &code);
    ok &= code == (DWORD)i;
    CloseHandle(h);
  }
  check(ok, "a hundred threads, one after the other (their stacks are given back)");
}

static DWORD WINAPI child_thread(LPVOID parameter) { InterlockedIncrement((volatile LONG *)parameter); return 0; }
static DWORD WINAPI spawner(LPVOID parameter) {
  HANDLE h = CreateThread(NULL, 0, child_thread, parameter, 0, NULL);
  if (!h) return 1;
  WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
  return 0;
}
static void test_nested(void) {
  volatile LONG n = 0;
  HANDLE h = CreateThread(NULL, 0, spawner, (LPVOID)&n, 0, NULL);
  WaitForSingleObject(h, INFINITE);
  check(n == 1, "a thread can create a thread");
  CloseHandle(h);
}

static HANDLE g_event, g_mutex;
static volatile LONG g_stage;
static DWORD WINAPI waits_for_event(LPVOID parameter) {
  (void)parameter;
  g_stage = 1;
  WaitForSingleObject(g_event, INFINITE);
  g_stage = 2;
  return 0;
}
static DWORD WINAPI mutex_user(LPVOID parameter) {
  (void)parameter;
  for (int i = 0; i < 200; ++i) {
    WaitForSingleObject(g_mutex, INFINITE);
    LONG v = g_counter;
    if (i % 50 == 0) Sleep(1);
    g_counter = v + 1;
    ReleaseMutex(g_mutex);
  }
  return 0;
}
static void test_sync_objects(void) {
  g_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  g_stage = 0;
  HANDLE h = CreateThread(NULL, 0, waits_for_event, NULL, 0, NULL);
  while (g_stage < 1) Sleep(1);
  Sleep(20);
  check(g_stage == 1, "the thread waits for the event");
  SetEvent(g_event);
  check(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && g_stage == 2, "SetEvent releases the waiting thread");
  CloseHandle(h);
  CloseHandle(g_event);

  g_mutex = CreateMutexA(NULL, FALSE, NULL);
  g_counter = 0;
  HANDLE m[4];
  for (int i = 0; i < 4; ++i) m[i] = CreateThread(NULL, 0, mutex_user, NULL, 0, NULL);
  WaitForMultipleObjects(4, m, TRUE, INFINITE);
  check(g_counter == 800, "a mutex keeps the count exact");
  for (int i = 0; i < 4; ++i) CloseHandle(m[i]);
  CloseHandle(g_mutex);

  HANDLE s = CreateSemaphoreA(NULL, 0, 10, NULL);
  LONG previous = -1;
  check(s != NULL && ReleaseSemaphore(s, 3, &previous) && previous == 0, "semaphore");
  check(WaitForSingleObject(s, 0) == WAIT_OBJECT_0 && WaitForSingleObject(s, 0) == WAIT_OBJECT_0 && WaitForSingleObject(s, 0) == WAIT_OBJECT_0 && WaitForSingleObject(s, 0) == WAIT_TIMEOUT, "a semaphore counts");
  CloseHandle(s);
}

static SRWLOCK g_srw = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static int g_queue[16], g_head, g_tail;
static DWORD WINAPI consumer(LPVOID parameter) {
  long sum = 0;
  for (int taken = 0; taken < 10; ++taken) {
    AcquireSRWLockExclusive(&g_srw);
    while (g_head == g_tail) SleepConditionVariableSRW(&g_cv, &g_srw, INFINITE, 0);
    sum += g_queue[g_head++ % 16];
    ReleaseSRWLockExclusive(&g_srw);
  }
  *(long *)parameter = sum;
  return 0;
}
static void test_condition_variable(void) {
  long sum = 0;
  HANDLE h = CreateThread(NULL, 0, consumer, &sum, 0, NULL);
  for (int i = 1; i <= 10; ++i) {
    AcquireSRWLockExclusive(&g_srw);
    g_queue[g_tail++ % 16] = i;
    ReleaseSRWLockExclusive(&g_srw);
    WakeConditionVariable(&g_cv);
    Sleep(1);
  }
  check(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && sum == 55, "a condition variable passes work to another thread");
  CloseHandle(h);
}

static DWORD WINAPI allocator_thread(LPVOID parameter) {
  void *blocks[64];
  for (int round = 0; round < 40; ++round) {
    for (int i = 0; i < 64; ++i) { blocks[i] = malloc(16 + (i * 37 + round) % 700); if (blocks[i]) memset(blocks[i], i, 16); }
    for (int i = 0; i < 64; ++i) { if (blocks[i] && ((unsigned char *)blocks[i])[0] != (unsigned char)i) return 1; free(blocks[i]); }
  }
  (void)parameter;
  return 0;
}
static void test_heap(void) {
  HANDLE h[6];
  for (int i = 0; i < 6; ++i) h[i] = CreateThread(NULL, 0, allocator_thread, NULL, 0, NULL);
  WaitForMultipleObjects(6, h, TRUE, INFINITE);
  int ok = 1;
  for (int i = 0; i < 6; ++i) { DWORD code = 9; GetExitCodeThread(h[i], &code); ok &= code == 0; CloseHandle(h[i]); }
  check(ok, "six threads allocate and free at once");
}

static __attribute__((noinline)) void write_to_null(void) { *(volatile int *)0 = 1; }
static DWORD WINAPI faulting(LPVOID parameter) {
  (void)parameter;
  DWORD code = 0;
  __try {
    write_to_null();      /* (clang puts only calls, not single instructions, in the scope of a __try) */
  } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
  }
  return code == EXCEPTION_ACCESS_VIOLATION ? 1 : 0;
}
static DWORD WINAPI printer(LPVOID parameter) {
  for (int i = 0; i < 5; ++i) printf("thread %d line %d\n", (int)(INT_PTR)parameter, i);
  errno = (int)(INT_PTR)parameter;
  Sleep(5);
  return errno == (int)(INT_PTR)parameter ? 1 : 0;
}
static void test_exceptions_and_crt(void) {
  HANDLE h = CreateThread(NULL, 0, faulting, NULL, 0, NULL);
  WaitForSingleObject(h, INFINITE);
  DWORD code = 0;
  GetExitCodeThread(h, &code);
  check(code == 1, "a fault in a thread is handled by the thread's own __try");
  CloseHandle(h);
  HANDLE p[3];
  for (int i = 0; i < 3; ++i) p[i] = CreateThread(NULL, 0, printer, (LPVOID)(INT_PTR)(i + 1), 0, NULL);
  WaitForMultipleObjects(3, p, TRUE, INFINITE);
  int ok = 1;
  for (int i = 0; i < 3; ++i) { GetExitCodeThread(p[i], &code); ok &= code == 1; CloseHandle(p[i]); }
  check(ok, "errno is a variable of each thread");
}

static DWORD WINAPI spinner(LPVOID parameter) { for (;;) { InterlockedIncrement((volatile LONG *)parameter); } }
static DWORD WINAPI sleeper(LPVOID parameter) { (void)parameter; Sleep(1000000); return 0; }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  g_main_id = GetCurrentThreadId();
  InitializeCriticalSection(&g_cs);
  printf("threadtest starting\n");
  test_basic();
  test_shared_counter();
  test_tls();
  test_suspended_and_exit();
  test_many();
  test_nested();
  test_sync_objects();
  test_condition_variable();
  test_heap();
  test_exceptions_and_crt();
  printf("threadtest: %d checks, %d failed\n", g_checks, g_failed);
  /* the process ends with threads still running: one spinning, one asleep */
  static volatile LONG spin;
  CreateThread(NULL, 0, spinner, (LPVOID)&spin, 0, NULL);
  CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
  Sleep(10);
  return g_failed;
}
