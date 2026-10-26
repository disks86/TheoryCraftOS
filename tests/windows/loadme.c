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

/* A DLL for loadtest.exe to load and free (mingw gcc; see tests/windows/build.sh). Its entry point counts what it is told in two named semaphores: "TcosLoadCount" goes up
 * for each DLL_PROCESS_ATTACH and down for each DLL_PROCESS_DETACH, "TcosThreadAttach" goes up for each DLL_THREAD_ATTACH. */
#include <windows.h>

static void count(const char *name, int up) {
  HANDLE s = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, name);
  if (!s) return;
  if (up) ReleaseSemaphore(s, 1, NULL); else WaitForSingleObject(s, 0);
  CloseHandle(s);
}

static int g_calls;

__declspec(dllexport) int add(int a, int b) { ++g_calls; return a + b; }
__declspec(dllexport) int calls(void) { return g_calls; }
__declspec(dllexport) DWORD process_id(void) { return GetCurrentProcessId(); }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) count("TcosLoadCount", 1);
  else if (reason == DLL_PROCESS_DETACH) count("TcosLoadCount", 0);
  else if (reason == DLL_THREAD_ATTACH) count("TcosThreadAttach", 1);
  return TRUE;
}
