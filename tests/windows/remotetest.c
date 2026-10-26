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

/* The memory and threads of another process on TheoryCraftOS: ReadProcessMemory, WriteProcessMemory, VirtualAllocEx/FreeEx/ProtectEx/QueryEx and CreateRemoteThread (mingw gcc;
 * see tests/windows/build.sh; procchild.exe has to be in System32). Prints "remotetest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (error %lu)\n", what, (unsigned long)GetLastError());
}

static BOOL start(const char *command, DWORD flags, PROCESS_INFORMATION *pi) {
  STARTUPINFOA si;
  char line[256];
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  strcpy(line, command);
  return CreateProcessA(NULL, line, NULL, NULL, FALSE, flags, NULL, NULL, &si, pi);
}

static DWORD remote_run(HANDLE process, const unsigned char *code, size_t length, DWORD flags, DWORD wait_ms, DWORD *result, HANDLE *thread_out) {
  LPVOID at = VirtualAllocEx(process, NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!at) return 1;
  SIZE_T done = 0;
  if (!WriteProcessMemory(process, at, code, length, &done) || done != length) return 2;
  DWORD id = 0;
  HANDLE thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)at, NULL, flags, &id);
  if (!thread) return 3;
  if (thread_out) *thread_out = thread;
  if (wait_ms) {
    if (WaitForSingleObject(thread, wait_ms) != WAIT_OBJECT_0) return 4;
    GetExitCodeThread(thread, result);
    CloseHandle(thread);
  }
  return 0;
}

int main(void) {
  PROCESS_INFORMATION child;
  /* memory of a process that has not run yet, and a program that reads it */
  check(start("procchild.exe memexpect", CREATE_SUSPENDED, &child), "start a child suspended");
  LPVOID fixed = VirtualAllocEx(child.hProcess, (LPVOID)0x30000000, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  check(fixed == (LPVOID)0x30000000, "VirtualAllocEx at a given address");
  SIZE_T done = 0;
  check(WriteProcessMemory(child.hProcess, fixed, "TCOSREMOTE", 10, &done) && done == 10, "WriteProcessMemory");
  char back[16] = {0};
  check(ReadProcessMemory(child.hProcess, fixed, back, 10, &done) && done == 10 && memcmp(back, "TCOSREMOTE", 10) == 0, "ReadProcessMemory gives it back");
  MEMORY_BASIC_INFORMATION mbi;
  check(VirtualQueryEx(child.hProcess, (char *)fixed + 100, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.BaseAddress == fixed && mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize == 8192, "VirtualQueryEx");
  check(VirtualQueryEx(child.hProcess, (LPCVOID)0x50000000, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_FREE, "VirtualQueryEx of free memory");
  DWORD old = 0;
  check(VirtualProtectEx(child.hProcess, fixed, 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE, "VirtualProtectEx");
  check(VirtualQueryEx(child.hProcess, fixed, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.Protect == PAGE_READONLY && mbi.RegionSize == 4096, "the new protection");
  check(WriteProcessMemory(child.hProcess, (char *)fixed + 20, "xyz", 3, &done) && done == 3, "a write works on a read-only page (the debugger way)");
  check(VirtualProtectEx(child.hProcess, fixed, 4096, PAGE_READWRITE, &old) && old == PAGE_READONLY, "back to read and write");
  /* reading past the committed pages stops there */
  char big[9000];
  check(!ReadProcessMemory(child.hProcess, fixed, big, 9000, &done) && GetLastError() == ERROR_PARTIAL_COPY && done == 8192, "a read that runs past the allocation copies what is there");
  check(!ReadProcessMemory(child.hProcess, (LPCVOID)0x50000000, big, 16, &done) && GetLastError() == ERROR_PARTIAL_COPY && done == 0, "reading unmapped memory");
  check(!VirtualProtectEx(child.hProcess, (LPVOID)0x50000000, 4096, PAGE_READONLY, &old), "protecting unmapped memory fails");
  /* the program reads what was written and answers */
  ResumeThread(child.hThread);
  check(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0, "the child ended");
  DWORD code = 99;
  GetExitCodeProcess(child.hProcess, &code);
  check(code == 0, "it found the text in its own memory");
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);

  /* threads in another process */
  check(start("procchild.exe spin", 0, &child), "start a spinning child");
  Sleep(50);
  DWORD result = 0;
  const unsigned char returns_123[] = {0xB8, 0x7B, 0x00, 0x00, 0x00, 0xC3};      /* mov eax, 123; ret */
  check(remote_run(child.hProcess, returns_123, sizeof(returns_123), 0, 10000, &result, NULL) == 0 && result == 123, "code written into the child ran as a thread and returned 123");
  /* a thread that writes to the child's memory */
  LPVOID mem = VirtualAllocEx(child.hProcess, (LPVOID)0x30000000, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  check(mem == (LPVOID)0x30000000, "memory in the running child");
  const unsigned char writes[] = {0xC7, 0x04, 0x25, 0x00, 0x01, 0x00, 0x30, 0xFE, 0xCA, 0x00, 0x00, 0x33, 0xC0, 0xC3};      /* mov dword [0x30000100], 0xCAFE; xor eax,eax; ret */
  check(remote_run(child.hProcess, writes, sizeof(writes), 0, 10000, &result, NULL) == 0 && result == 0, "a thread that writes to the child's memory");
  DWORD value = 0;
  check(ReadProcessMemory(child.hProcess, (LPCVOID)0x30000100, &value, 4, &done) && value == 0xCAFE, "the parent reads what the thread wrote");
  /* a suspended thread */
  HANDLE suspended = NULL;
  check(remote_run(child.hProcess, returns_123, sizeof(returns_123), CREATE_SUSPENDED, 0, NULL, &suspended) == 0 && suspended, "a thread created suspended");
  Sleep(50);
  check(GetExitCodeThread(suspended, &result) && result == STILL_ACTIVE && WaitForSingleObject(suspended, 50) == WAIT_TIMEOUT, "it has not run");
  check(ResumeThread(suspended) == 1, "ResumeThread says it was suspended once");
  check(WaitForSingleObject(suspended, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(suspended, &result) && result == 123, "it runs after the resume");
  const DWORD thread_id = GetThreadId(suspended);
  check(thread_id != 0, "GetThreadId of a remote thread");
  CloseHandle(suspended);
  /* a thread that never ends, and TerminateThread */
  const unsigned char forever[] = {0xEB, 0xFE};      /* jmp $ */
  HANDLE endless = NULL;
  check(remote_run(child.hProcess, forever, sizeof(forever), 0, 0, NULL, &endless) == 0 && endless, "a thread that loops");
  Sleep(50);
  check(WaitForSingleObject(endless, 50) == WAIT_TIMEOUT, "it is still running");
  check(TerminateThread(endless, 55), "TerminateThread on it");
  check(WaitForSingleObject(endless, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(endless, &result) && result == 55, "it ended with the code given");
  CloseHandle(endless);
  /* release */
  check(VirtualFreeEx(child.hProcess, mem, 0, MEM_RELEASE), "VirtualFreeEx");
  check(!ReadProcessMemory(child.hProcess, mem, &value, 4, &done) && done == 0, "the memory is gone");
  check(!ReadProcessMemory((HANDLE)0x7777, mem, &value, 4, &done) && GetLastError() == ERROR_INVALID_HANDLE, "a bad process handle");
  /* this process through a handle of its own */
  HANDLE self = OpenProcess(PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId());
  int local = 41;
  check(self && ReadProcessMemory(self, &local, &value, 4, &done) && value == 41, "a handle to this process reads this process");
  local = 5;
  value = 77;
  check(WriteProcessMemory(self, &local, &value, 4, &done) && local == 77, "and writes it");
  if (self) CloseHandle(self);
  TerminateProcess(child.hProcess, 0);
  WaitForSingleObject(child.hProcess, 5000);
  check(!ReadProcessMemory(child.hProcess, (LPCVOID)0x30000100, &value, 4, &done), "memory of an ended process cannot be read");
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  printf("remotetest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
