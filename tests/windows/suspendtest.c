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

/* SuspendThread, GetThreadContext, SetThreadContext and TerminateThread on TheoryCraftOS (mingw gcc; see tests/windows/build.sh). Prints "suspendtest: N checks, M failed";
 * the exit code is the number of failures. */
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

static volatile LONG g_counter;
static volatile LONG g_escaped;

/* a thread that only computes in user mode */
static DWORD WINAPI spinner(LPVOID parameter) {
  (void)parameter;
  for (;;) InterlockedIncrement(&g_counter);
}
static DWORD WINAPI spinner_end(LPVOID parameter) { return (DWORD)(ULONG_PTR)parameter; }      /* (marks the end of the code above) */
/* where a suspended thread is sent with SetThreadContext */
static void escape(void) {
  g_escaped = 1;
  ExitThread(77);
}

static HANDLE g_event, g_mutex;
static DWORD WINAPI waits_forever(LPVOID parameter) { (void)parameter; WaitForSingleObject(g_event, INFINITE); return 1; }
static DWORD WINAPI holds_mutex(LPVOID parameter) {
  (void)parameter;
  WaitForSingleObject(g_mutex, INFINITE);
  Sleep(1000000);
  return 2;
}
static DWORD WINAPI sleeps(LPVOID parameter) { (void)parameter; Sleep(1000000); return 3; }
static HANDLE g_main_thread;
static DWORD WINAPI resumes_main(LPVOID parameter) {
  (void)parameter;
  Sleep(40);
  ResumeThread(g_main_thread);
  return 0;
}

static LONG settle(void) {      /* the counter after the thread had time to stop */
  Sleep(30);
  return g_counter;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("suspendtest starting\n");

  HANDLE spin = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
  Sleep(30);
  const LONG running = g_counter;
  check(running > 0, "the thread computes");
  check(SuspendThread(spin) == 0, "SuspendThread returns the previous count (0)");
  const LONG at_stop = settle();
  Sleep(40);
  check(g_counter == at_stop && at_stop >= running, "a suspended thread does not run");
  CONTEXT context;
  memset(&context, 0, sizeof(context));
  context.ContextFlags = CONTEXT_FULL;
  check(GetThreadContext(spin, &context), "GetThreadContext");
  check(context.Rip >= (DWORD64)(ULONG_PTR)spinner && context.Rip < (DWORD64)(ULONG_PTR)spinner_end, "its Rip is inside the thread's code");
  check(context.Rsp != 0 && context.SegCs == 0x33, "with a stack pointer and the user code segment");

  check(SuspendThread(spin) == 1, "a second SuspendThread: previous count 1");
  check(ResumeThread(spin) == 2, "ResumeThread: previous count 2");
  Sleep(30);
  check(g_counter == at_stop, "still suspended after one resume");

  /* send it somewhere else */
  context.Rip = (DWORD64)(ULONG_PTR)escape;
  context.Rsp = ((context.Rsp - 512) & ~(DWORD64)15) - 8;
  check(SetThreadContext(spin, &context), "SetThreadContext");
  check(ResumeThread(spin) == 1, "ResumeThread: previous count 1");
  check(WaitForSingleObject(spin, 5000) == WAIT_OBJECT_0, "the thread went to the new Rip and ended");
  DWORD code = 0;
  GetExitCodeThread(spin, &code);
  check(g_escaped == 1 && code == 77, "it ran escape() and exited with 77");
  CloseHandle(spin);

  /* a thread that has not started: the registers it will start with */
  HANDLE idle = CreateThread(NULL, 0, spinner_end, (LPVOID)(ULONG_PTR)5, CREATE_SUSPENDED, NULL);
  memset(&context, 0, sizeof(context));
  context.ContextFlags = CONTEXT_FULL;
  check(GetThreadContext(idle, &context) && context.Rcx == (DWORD64)(ULONG_PTR)spinner_end && context.Rdx == 5, "an unstarted thread: Rcx and Rdx hold its function and parameter");
  context.Rdx = 6;
  check(SetThreadContext(idle, &context), "its parameter can be changed");
  check(ResumeThread(idle) == 1, "ResumeThread starts it");
  WaitForSingleObject(idle, 5000);
  GetExitCodeThread(idle, &code);
  check(code == 6, "and it runs with the changed parameter");
  CloseHandle(idle);

  /* TerminateThread: a thread in user mode, one waiting in the kernel, one sleeping, one holding a mutex */
  spin = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
  Sleep(20);
  check(TerminateThread(spin, 55), "TerminateThread");
  check(WaitForSingleObject(spin, 5000) == WAIT_OBJECT_0, "the spinning thread ends");
  GetExitCodeThread(spin, &code);
  check(code == 55, "with the exit code given");
  CloseHandle(spin);

  g_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  HANDLE waiter = CreateThread(NULL, 0, waits_forever, NULL, 0, NULL);
  Sleep(20);
  TerminateThread(waiter, 66);
  check(WaitForSingleObject(waiter, 5000) == WAIT_OBJECT_0, "a thread waiting for ever in the kernel ends");
  GetExitCodeThread(waiter, &code);
  check(code == 66, "with its code");
  CloseHandle(waiter);

  HANDLE sleeper = CreateThread(NULL, 0, sleeps, NULL, 0, NULL);
  Sleep(20);
  TerminateThread(sleeper, 4);
  check(WaitForSingleObject(sleeper, 5000) == WAIT_OBJECT_0, "a sleeping thread ends");
  CloseHandle(sleeper);

  g_mutex = CreateMutexA(NULL, FALSE, NULL);
  HANDLE holder = CreateThread(NULL, 0, holds_mutex, NULL, 0, NULL);
  Sleep(20);
  check(WaitForSingleObject(g_mutex, 0) == WAIT_TIMEOUT, "the other thread holds the mutex");
  TerminateThread(holder, 8);
  WaitForSingleObject(holder, 5000);
  check(WaitForSingleObject(g_mutex, 1000) == WAIT_ABANDONED_0, "its mutex is abandoned when the thread is terminated");
  ReleaseMutex(g_mutex);
  CloseHandle(holder);

  /* a thread may suspend itself: another resumes it */
  HANDLE self = NULL;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
  g_main_thread = self;
  HANDLE helper = CreateThread(NULL, 0, resumes_main, NULL, 0, NULL);
  const DWORD t0 = GetTickCount();
  check(SuspendThread(self) == 0, "a thread suspends itself");
  check(GetTickCount() - t0 >= 30, "and was held until the other thread resumed it");
  WaitForSingleObject(helper, 5000);
  CloseHandle(helper);
  CloseHandle(self);

  printf("suspendtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
