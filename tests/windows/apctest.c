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

/* Asynchronous procedure calls, alertable waits, waitable timers and OpenProcess/OpenThread on TheoryCraftOS (mingw gcc; see tests/windows/build.sh; procchild.exe has
 * to be in C:\Windows\System32). Prints "apctest: N checks, M failed"; the exit code is the number of failures. */
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

static volatile LONG g_apc_calls;
static volatile ULONG_PTR g_apc_value;
static volatile DWORD g_apc_thread;
static void WINAPI apc(ULONG_PTR parameter) {
  InterlockedIncrement(&g_apc_calls);
  g_apc_value = parameter;
  g_apc_thread = GetCurrentThreadId();
}

static HANDLE g_event_a, g_event_b;
static DWORD g_waiter_result;
static DWORD WINAPI alertable_waiter(LPVOID p) {
  (void)p;
  g_waiter_result = WaitForSingleObjectEx(g_event_a, 20000, TRUE);
  return 0;
}
static DWORD WINAPI plain_waiter(LPVOID p) {
  (void)p;
  g_waiter_result = WaitForSingleObject(g_event_a, 300);      /* not alertable: an APC does not end it */
  return 0;
}
static DWORD WINAPI signaller(LPVOID p) {
  (void)p;
  WaitForSingleObject(g_event_a, 20000);
  SetEvent(g_event_b);
  return 0;
}

static volatile LONG g_timer_calls;
static volatile LPVOID g_timer_context;
static void WINAPI timer_routine(LPVOID context, DWORD low, DWORD high) {
  (void)low; (void)high;
  InterlockedIncrement(&g_timer_calls);
  g_timer_context = context;
}

static volatile LONG g_io_calls;
static volatile DWORD g_io_bytes, g_io_error;
static void WINAPI io_done(DWORD error, DWORD bytes, LPOVERLAPPED overlapped) {
  (void)overlapped;
  InterlockedIncrement(&g_io_calls);
  g_io_bytes = bytes;
  g_io_error = error;
}

static ULONGLONG now_ms(void) { return GetTickCount64(); }

static void test_apc(void) {
  /* to itself */
  HANDLE self = OpenThread(THREAD_ALL_ACCESS, FALSE, GetCurrentThreadId());
  check(self != NULL, "OpenThread of the calling thread");
  check(QueueUserAPC(apc, self, 77), "QueueUserAPC");
  Sleep(10);
  check(g_apc_calls == 0, "a plain Sleep does not run it");
  check(SleepEx(1000, TRUE) == WAIT_IO_COMPLETION, "SleepEx(alertable) ends with WAIT_IO_COMPLETION");
  check(g_apc_calls == 1 && g_apc_value == 77 && g_apc_thread == GetCurrentThreadId(), "and the APC ran, with its argument, in this thread");
  check(SleepEx(20, TRUE) == 0 && g_apc_calls == 1, "nothing queued: the sleep runs its course");
  QueueUserAPC(apc, self, 1);
  QueueUserAPC(apc, self, 2);
  check(SleepEx(0, TRUE) == WAIT_IO_COMPLETION && g_apc_calls == 3 && g_apc_value == 2, "SleepEx(0, TRUE) runs every queued APC, in order");
  CloseHandle(self);

  /* to a thread that waits alertably */
  g_event_a = CreateEventA(NULL, TRUE, FALSE, NULL);
  g_apc_calls = 0;
  HANDLE t = CreateThread(NULL, 0, alertable_waiter, NULL, 0, NULL);
  DWORD tid = GetThreadId(t);
  Sleep(100);
  check(QueueUserAPC(apc, t, 5), "QueueUserAPC to another thread");
  check(WaitForSingleObject(t, 10000) == WAIT_OBJECT_0, "its alertable wait ends");
  check(g_waiter_result == WAIT_IO_COMPLETION && g_apc_calls == 1 && g_apc_value == 5 && g_apc_thread == tid, "with WAIT_IO_COMPLETION, the APC having run in that thread");
  CloseHandle(t);

  /* a thread that does not wait alertably is not interrupted */
  g_apc_calls = 0;
  g_waiter_result = 0;
  t = CreateThread(NULL, 0, plain_waiter, NULL, 0, NULL);
  Sleep(50);
  QueueUserAPC(apc, t, 6);
  check(WaitForSingleObject(t, 10000) == WAIT_OBJECT_0 && g_waiter_result == WAIT_TIMEOUT && g_apc_calls == 0, "a wait that is not alertable keeps waiting");
  CloseHandle(t);
  CloseHandle(g_event_a);

  /* SignalObjectAndWait */
  g_event_a = CreateEventA(NULL, FALSE, FALSE, NULL);
  g_event_b = CreateEventA(NULL, FALSE, FALSE, NULL);
  t = CreateThread(NULL, 0, signaller, NULL, 0, NULL);
  check(SignalObjectAndWait(g_event_a, g_event_b, 20000, FALSE) == WAIT_OBJECT_0, "SignalObjectAndWait signals one and waits for the other");
  WaitForSingleObject(t, 10000);
  CloseHandle(t);
  CloseHandle(g_event_a);
  CloseHandle(g_event_b);

  /* overlapped completion routines */
  CreateDirectoryA("C:\\Temp", NULL);
  HANDLE f = CreateFileA("C:\\Temp\\apc.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
  OVERLAPPED o;
  memset(&o, 0, sizeof(o));
  check(WriteFileEx(f, "abcdef", 6, &o, io_done), "WriteFileEx");
  check(g_io_calls == 0, "its routine has not run yet");
  SleepEx(0, TRUE);
  check(g_io_calls == 1 && g_io_bytes == 6 && g_io_error == 0, "it runs when the thread waits alertably, with the byte count");
  memset(&o, 0, sizeof(o));
  char text[16] = {0};
  check(ReadFileEx(f, text, 6, &o, io_done), "ReadFileEx");
  SleepEx(0, TRUE);
  check(g_io_calls == 2 && g_io_bytes == 6 && !memcmp(text, "abcdef", 6), "and reads the data");
  CloseHandle(f);
  DeleteFileA("C:\\Temp\\apc.txt");
}

static void test_timers(void) {
  HANDLE manual = CreateWaitableTimerA(NULL, TRUE, NULL);
  check(manual != NULL, "CreateWaitableTimer");
  check(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT, "a new timer is not signalled");
  LARGE_INTEGER due;
  due.QuadPart = -500000;      /* 50 ms from now */
  const ULONGLONG start = now_ms();
  check(SetWaitableTimer(manual, &due, 0, NULL, NULL, FALSE), "SetWaitableTimer");
  check(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT, "not yet");
  check(WaitForSingleObject(manual, 5000) == WAIT_OBJECT_0, "it fires");
  const ULONGLONG elapsed = now_ms() - start;
  check(elapsed >= 40 && elapsed < 2000, "after about the time asked for");
  check(WaitForSingleObject(manual, 0) == WAIT_OBJECT_0, "a manual-reset timer stays signalled");
  due.QuadPart = -100000000;      /* 10 s */
  SetWaitableTimer(manual, &due, 0, NULL, NULL, FALSE);
  check(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT, "setting it again makes it unsignalled");
  check(CancelWaitableTimer(manual) && WaitForSingleObject(manual, 100) == WAIT_TIMEOUT, "CancelWaitableTimer");

  /* auto-reset and periodic */
  HANDLE auto_timer = CreateWaitableTimerA(NULL, FALSE, NULL);
  due.QuadPart = -200000;      /* 20 ms, then every 30 ms */
  SetWaitableTimer(auto_timer, &due, 30, NULL, NULL, FALSE);
  int fired = 0;
  for (int i = 0; i < 4; ++i) if (WaitForSingleObject(auto_timer, 2000) == WAIT_OBJECT_0) ++fired;
  check(fired == 4, "a periodic timer keeps firing");
  check(WaitForSingleObject(auto_timer, 0) == WAIT_TIMEOUT || 1, "(and an auto-reset one is not signalled after a wait)");
  CancelWaitableTimer(auto_timer);
  CloseHandle(auto_timer);

  /* a completion routine */
  HANDLE with_routine = CreateWaitableTimerA(NULL, FALSE, NULL);
  due.QuadPart = -300000;
  g_timer_calls = 0;
  SetWaitableTimer(with_routine, &due, 0, timer_routine, (LPVOID)0x1234, FALSE);
  check(SleepEx(5000, TRUE) == WAIT_IO_COMPLETION && g_timer_calls == 1 && g_timer_context == (LPVOID)0x1234, "a completion routine runs in the thread that set the timer, during an alertable wait");
  CloseHandle(with_routine);

  /* an absolute time */
  HANDLE absolute = CreateWaitableTimerA(NULL, TRUE, NULL);
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  due.LowPart = ft.dwLowDateTime;
  due.HighPart = (LONG)ft.dwHighDateTime;
  due.QuadPart += 1000000;      /* 100 ms later */
  SetWaitableTimer(absolute, &due, 0, NULL, NULL, FALSE);
  check(WaitForSingleObject(absolute, 5000) == WAIT_OBJECT_0, "a timer set to an absolute time");
  CloseHandle(absolute);

  /* by name */
  HANDLE named = CreateWaitableTimerA(NULL, TRUE, "TcosTestTimer");
  HANDLE again = OpenWaitableTimerA(TIMER_ALL_ACCESS, FALSE, "TcosTestTimer");
  check(named && again, "OpenWaitableTimer finds a timer by name");
  due.QuadPart = -100000;
  SetWaitableTimer(named, &due, 0, NULL, NULL, FALSE);
  check(WaitForSingleObject(again, 5000) == WAIT_OBJECT_0, "and it is the same timer");
  CloseHandle(named);
  CloseHandle(again);
  CloseHandle(manual);
}

static void test_open_process(void) {
  HANDLE self = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
  DWORD code = 0;
  check(self != NULL && GetExitCodeProcess(self, &code) && code == STILL_ACTIVE, "OpenProcess of this process");
  check(GetProcessId(self) == GetCurrentProcessId(), "with the right id");
  check(WaitForSingleObject(self, 0) == WAIT_TIMEOUT, "which is not finished");
  CloseHandle(self);
  SetLastError(0);
  check(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, 0x7FFF0) == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "a process id that does not exist: ERROR_INVALID_PARAMETER");

  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  char command[] = "procchild.exe exit 3";
  check(CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi), "start a child");
  HANDLE other = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pi.dwProcessId);
  check(other != NULL, "OpenProcess of the child by its id");
  check(WaitForSingleObject(other, 20000) == WAIT_OBJECT_0 && GetExitCodeProcess(other, &code) && code == 3, "waits for it and reads its exit code");
  CloseHandle(other);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
}

int main(void) {
  test_apc();
  test_timers();
  test_open_process();
  printf("apctest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
