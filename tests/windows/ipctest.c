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

/* Two processes sharing synchronization objects by name (run twice at the same time: "ipctest.exe server" and "ipctest.exe client", see TCOS_USERMODE_DEMO_PAIR). They find
 * each other by creating-or-opening the same names. They prove: events (manual and auto reset) wake the other process; a named mutex keeps a counter in a file exact when both
 * add to it; a semaphore carries counts across; WaitForMultipleObjects across processes; and a mutex the server still owns when it ends is abandoned for the client.
 * Prints "ipctest <role>: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
static const char *g_role;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL (%s): %s\n", g_role, what);
}

static HANDLE open_or_create_event(const char *name, BOOL manual) { return CreateEventA(NULL, manual, FALSE, name); }

static const char *kCounterFile = "C:\\ipc_counter.txt";
static void add_to_counter(void) {
  char text[32] = {0};
  DWORD n = 0;
  HANDLE f = CreateFileA(kCounterFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  int value = 0;
  if (f != INVALID_HANDLE_VALUE) {
    ReadFile(f, text, sizeof(text) - 1, &n, NULL);
    CloseHandle(f);
    value = atoi(text);
  }
  Sleep(0);      /* give the other process the chance to get in between, if the mutex let it */
  snprintf(text, sizeof(text), "%d", value + 1);
  f = CreateFileA(kCounterFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (f != INVALID_HANDLE_VALUE) {
    WriteFile(f, text, (DWORD)strlen(text), &n, NULL);
    CloseHandle(f);
  }
}
static int read_counter(void) {
  char text[32] = {0};
  DWORD n = 0;
  HANDLE f = CreateFileA(kCounterFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return -1;
  ReadFile(f, text, sizeof(text) - 1, &n, NULL);
  CloseHandle(f);
  return atoi(text);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  g_role = argc > 1 ? argv[1] : "";
  if (strcmp(g_role, "server") != 0 && strcmp(g_role, "client") != 0) {      /* started on its own: there is nobody to talk to */
    printf("ipctest: run it as \"server\" and \"client\" at the same time\n");
    return 0;
  }
  const int server = strcmp(g_role, "client") != 0;
  printf("ipctest %s starting\n", g_role);

  HANDLE ready = open_or_create_event("Local\\TcosIpc.Ready", TRUE);
  HANDLE ping = open_or_create_event("TcosIpc.Ping", FALSE);
  HANDLE pong = open_or_create_event("TcosIpc.Pong", FALSE);
  HANDLE both_done = open_or_create_event("TcosIpc.Done", TRUE);
  HANDLE lock = CreateMutexA(NULL, FALSE, "TcosIpc.Lock");
  HANDLE semaphore = CreateSemaphoreA(NULL, 0, 100, "Global\\TcosIpc.Semaphore");
  check(ready && ping && pong && both_done && lock && semaphore, "the objects are made (or opened)");
  HANDLE abandon = NULL;

  if (server) {
    /* the server makes the abandoned-mutex object and keeps it */
    abandon = CreateMutexA(NULL, TRUE, "TcosIpc.Abandon");
    check(abandon != NULL && GetLastError() != ERROR_ALREADY_EXISTS, "the server makes the mutex it will not release");
    DeleteFileA(kCounterFile);
    SetEvent(ready);
  } else {
    check(WaitForSingleObject(ready, 30000) == WAIT_OBJECT_0, "the client sees the server's event");
    abandon = OpenMutexA(SYNCHRONIZE, FALSE, "TcosIpc.Abandon");
    check(abandon != NULL, "the client opens the server's mutex by name");
  }

  /* events: ping-pong, each wake carries one side to the other */
  int pongs = 0;
  for (int i = 0; i < 20; ++i) {
    if (server) {
      SetEvent(ping);
      if (WaitForSingleObject(pong, 20000) == WAIT_OBJECT_0) ++pongs;
    } else {
      if (WaitForSingleObject(ping, 20000) == WAIT_OBJECT_0) ++pongs;
      SetEvent(pong);
    }
  }
  check(pongs == 20, "twenty round trips of an auto-reset event");

  /* the mutex keeps a shared counter exact */
  for (int i = 0; i < 40; ++i) {
    DWORD w = WaitForSingleObject(lock, 20000);
    check(w == WAIT_OBJECT_0, "the shared mutex is acquired");
    add_to_counter();
    ReleaseMutex(lock);
  }

  /* the semaphore: the server releases counts, the client takes them */
  if (server) {
    LONG previous = -1;
    check(ReleaseSemaphore(semaphore, 7, &previous), "the server releases the semaphore");
  } else {
    int taken = 0;
    for (int i = 0; i < 7; ++i) if (WaitForSingleObject(semaphore, 20000) == WAIT_OBJECT_0) ++taken;
    check(taken == 7, "the client takes all seven counts");
    check(WaitForSingleObject(semaphore, 0) == WAIT_TIMEOUT, "and no more");
  }

  /* wait for any / all across processes */
  HANDLE manual = open_or_create_event("TcosIpc.Manual", TRUE);
  HANDLE other = open_or_create_event("TcosIpc.Other", TRUE);
  if (server) {
    Sleep(50);
    SetEvent(other);
    Sleep(50);
    SetEvent(manual);
  } else {
    HANDLE both[2] = {manual, other};
    DWORD any = WaitForMultipleObjects(2, both, FALSE, 20000);
    check(any == WAIT_OBJECT_0 + 1, "wait for any: the second event came first");
    check(WaitForMultipleObjects(2, both, TRUE, 20000) == WAIT_OBJECT_0, "wait for all: both are set in the end");
  }

  /* both sides finished their counting: the total is right */
  if (server) {
    SetEvent(both_done);
  } else {
    check(WaitForSingleObject(both_done, 30000) == WAIT_OBJECT_0, "the server is done");
  }
  /* (each wrote 40; the other may still be in its last turn, so the check waits for the sum) */
  int counter = 0;
  for (int i = 0; i < 400 && (counter = read_counter()) != 80; ++i) Sleep(50);
  check(counter == 80, "both processes' additions all counted (80)");

  if (!server) {
    /* the server leaves without releasing the mutex it owns: it is abandoned for the client */
    DWORD w = WaitForSingleObject(abandon, 30000);
    check(w == WAIT_ABANDONED_0, "the mutex of the process that ended is abandoned");
    ReleaseMutex(abandon);
  }

  printf("ipctest %s: %d checks, %d failed\n", g_role, g_checks, g_failed);
  return g_failed;      /* (the server ends while still owning `abandon`) */
}
