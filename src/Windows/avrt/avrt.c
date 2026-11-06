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

/* avrt.dll for TheoryCraftOS: the Multimedia Class Scheduler (MMCSS) calls that audio and game threads make. Windows moves a registered thread into a scheduling class of the
 * MMCSS service; here a thread joins one of the standard tasks (the names of HKLM\...\Multimedia\SystemProfile\Tasks on a fresh Windows) and gets a raised thread priority
 * for as long as it is registered, which is the part a program can notice. Not done: the thread-ordering groups (AvRt*ThreadOrderingGroup* stay stubs). Built freestanding
 * over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <avrt.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define ERROR_INVALID_TASK_NAME_ 1550

/* The registrations: the thread and the priority it had before. A handle is 0x4156'0000 + the slot, checked against the table (a handle the program made up is refused). */
typedef struct { DWORD thread; int previous_priority; } Registration;
#define MAX_REGISTRATIONS 256
#define HANDLE_BASE 0x41560000u
static Registration g_registrations[MAX_REGISTRATIONS];      /* thread 0: free */
static SRWLOCK g_lock = SRWLOCK_INIT;

static const char *const kTasks[] = {"Audio", "Capture", "Distribution", "Games", "Low Latency", "Playback", "Pro Audio", "Window Manager"};
static LONG g_next_index = 1;

static int ascii_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static BOOL known_task_w(LPCWSTR name) {
  for (SIZE_T t = 0; t < sizeof kTasks / sizeof kTasks[0]; ++t) {
    SIZE_T i = 0;
    while (kTasks[t][i] && name[i] && ascii_lower(name[i]) == ascii_lower(kTasks[t][i])) ++i;
    if (!kTasks[t][i] && !name[i]) return TRUE;
  }
  return FALSE;
}
static BOOL known_task_a(LPCSTR name) {
  WCHAR wide[64];
  SIZE_T i = 0;
  for (; name[i] && i < 63; ++i) wide[i] = (BYTE)name[i];
  if (name[i]) return FALSE;
  wide[i] = 0;
  return known_task_w(wide);
}

/* The slot of a handle, or MAX_REGISTRATIONS. */
static UINT slot_of(HANDLE handle) {
  const ULONG_PTR h = (ULONG_PTR)handle;
  if (h < HANDLE_BASE || h >= HANDLE_BASE + MAX_REGISTRATIONS) return MAX_REGISTRATIONS;
  const UINT slot = (UINT)(h - HANDLE_BASE);
  AcquireSRWLockShared(&g_lock);
  const BOOL used = g_registrations[slot].thread != 0;
  ReleaseSRWLockShared(&g_lock);
  return used ? slot : MAX_REGISTRATIONS;
}

static HANDLE join(BOOL known, LPDWORD index) {
  if (!index) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (!known) { SetLastError(ERROR_INVALID_TASK_NAME_); return NULL; }
  AcquireSRWLockExclusive(&g_lock);
  UINT slot = 0;
  while (slot < MAX_REGISTRATIONS && g_registrations[slot].thread) ++slot;
  if (slot == MAX_REGISTRATIONS) { ReleaseSRWLockExclusive(&g_lock); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  g_registrations[slot].thread = GetCurrentThreadId();
  g_registrations[slot].previous_priority = GetThreadPriority(GetCurrentThread());
  ReleaseSRWLockExclusive(&g_lock);
  if (*index == 0) *index = (DWORD)InterlockedIncrement(&g_next_index) - 1;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  return (HANDLE)(ULONG_PTR)(HANDLE_BASE + slot);
}

EXPORT HANDLE WINAPI AvSetMmThreadCharacteristicsW(LPCWSTR task, LPDWORD index) { return join(task && known_task_w(task), index); }
EXPORT HANDLE WINAPI AvSetMmThreadCharacteristicsA(LPCSTR task, LPDWORD index) { return join(task && known_task_a(task), index); }
EXPORT HANDLE WINAPI AvSetMmMaxThreadCharacteristicsW(LPCWSTR first, LPCWSTR second, LPDWORD index) {
  return join((first && known_task_w(first)) || (second && known_task_w(second)), index);
}
EXPORT HANDLE WINAPI AvSetMmMaxThreadCharacteristicsA(LPCSTR first, LPCSTR second, LPDWORD index) {
  return join((first && known_task_a(first)) || (second && known_task_a(second)), index);
}

EXPORT BOOL WINAPI AvRevertMmThreadCharacteristics(HANDLE handle) {
  const ULONG_PTR h = (ULONG_PTR)handle;
  AcquireSRWLockExclusive(&g_lock);      /* (the check and the clearing together: two reverts of one handle cannot both succeed) */
  const UINT slot = h >= HANDLE_BASE && h < HANDLE_BASE + MAX_REGISTRATIONS ? (UINT)(h - HANDLE_BASE) : MAX_REGISTRATIONS;
  if (slot == MAX_REGISTRATIONS || !g_registrations[slot].thread) { ReleaseSRWLockExclusive(&g_lock); SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  const Registration r = g_registrations[slot];
  g_registrations[slot].thread = 0;
  ReleaseSRWLockExclusive(&g_lock);
  if (r.thread == GetCurrentThreadId()) SetThreadPriority(GetCurrentThread(), r.previous_priority);
  return TRUE;
}

/* AVRT_PRIORITY_VERYLOW (-2) ... AVRT_PRIORITY_CRITICAL (2) */
EXPORT BOOL WINAPI AvSetMmThreadPriority(HANDLE handle, AVRT_PRIORITY priority) {
  static const int kPriorities[] = {THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_HIGHEST, THREAD_PRIORITY_TIME_CRITICAL};
  const UINT slot = slot_of(handle);
  if (slot == MAX_REGISTRATIONS) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if ((int)priority < -2 || (int)priority > 2) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  AcquireSRWLockShared(&g_lock);
  const DWORD thread = g_registrations[slot].thread;
  ReleaseSRWLockShared(&g_lock);
  if (thread != GetCurrentThreadId()) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  return SetThreadPriority(GetCurrentThread(), kPriorities[(int)priority + 2]);
}

/* The share of the processor kept for other work: the Windows default (20 percent). */
EXPORT BOOL WINAPI AvQuerySystemResponsiveness(HANDLE handle, PULONG responsiveness) {
  if (slot_of(handle) == MAX_REGISTRATIONS) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (!responsiveness) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *responsiveness = 20;
  return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module; (void)reason; (void)reserved;
  return TRUE;
}
