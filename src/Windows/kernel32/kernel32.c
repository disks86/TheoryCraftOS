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

/* kernel32.dll for TheoryCraftOS: the Win32 API a console program needs, written over ntdll.dll's NT services. Single-threaded (CreateThread fails), the code page is UTF-8,
 * the only drive is C:. Built freestanding with the OS's mingw compiler. */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* From ntdll.dll (the NT services are declared in tcos_win.h). */
IMPORT PVOID NTAPI RtlAllocateHeap(PVOID, ULONG, SIZE_T);
IMPORT BOOLEAN NTAPI RtlFreeHeap(PVOID, ULONG, PVOID);
IMPORT BOOLEAN NTAPI RtlLockHeap(PVOID);
IMPORT BOOLEAN NTAPI RtlUnlockHeap(PVOID);
IMPORT PVOID NTAPI RtlReAllocateHeap(PVOID, ULONG, PVOID, SIZE_T);
IMPORT SIZE_T NTAPI RtlSizeHeap(PVOID, ULONG, PVOID);
IMPORT PVOID NTAPI RtlCreateHeap(ULONG, PVOID, SIZE_T, SIZE_T, PVOID, PVOID);
IMPORT PVOID NTAPI RtlDestroyHeap(PVOID);
IMPORT BOOLEAN NTAPI RtlValidateHeap(PVOID, ULONG, PVOID);
IMPORT PVOID NTAPI RtlProcessHeap(void);
IMPORT ULONG NTAPI RtlNtStatusToDosError(NTSTATUS);
IMPORT NTSTATUS NTAPI RtlMultiByteToUnicodeN(PWSTR, ULONG, PULONG, const char *, ULONG);
IMPORT NTSTATUS NTAPI RtlUnicodeToMultiByteN(char *, ULONG, PULONG, PCWSTR, ULONG);
IMPORT NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION);
IMPORT NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION);
IMPORT NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION);
IMPORT NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION);
IMPORT void NTAPI RtlExitUserProcess(NTSTATUS);
IMPORT void *memcpy(void *, const void *, size_t_);
IMPORT void *memset(void *, int, size_t_);
IMPORT void *memmove(void *, const void *, size_t_);
IMPORT int memcmp(const void *, const void *, size_t_);
IMPORT size_t_ strlen(const char *);
IMPORT size_t_ wcslen(const WCHAR *);

/* ---- small helpers ------------------------------------------------------------------------------------------------------------------------------------- */

static void set_error(DWORD error) { TEB_LAST_ERROR(NtCurrentTeb()) = error; }
/* Records the Win32 error of a failed NT call and returns FALSE. */
#ifndef ERROR_PRIVILEGE_NOT_HELD
#define ERROR_PRIVILEGE_NOT_HELD 1314
#endif
static int month_days(LONGLONG y, int m);
EXPORT void WINAPI GetLocalTime(LPSYSTEMTIME st);
static void complete_overlapped(LPOVERLAPPED overlapped, NTSTATUS status, ULONG_PTR bytes);
static DWORD wait_result(NTSTATUS status);
EXPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPOVERLAPPED overlapped);
EXPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPOVERLAPPED overlapped);
EXPORT DWORD WINAPI GetTickCount(void);
static BOOL console_write_hook(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, BOOL *handled);
static BOOL console_read_hook(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, BOOL *handled);
static BOOL con_input_ready(void);
static BOOL con_wait_polled(HANDLE h);
static void con_sync_signals(void);
static LONG con_unbound_kind(HANDLE h);
static BOOL con_attached(void);
/* A handle of a console that this process no longer belongs to is of no use. */
static BOOL con_dead_handle(HANDLE h) { return !con_attached() && con_unbound_kind(h) != 0; }
static void con_handle_closed(HANDLE h);
static BOOL con_is_input(HANDLE h);
static void con_alias_add(HANDLE h, BOOL input);
static void con_alias_forget(HANDLE h);
static BOOL pipe_nt_name(LPCWSTR name, WCHAR *buffer, UNICODE_STRING *us, OBJECT_ATTRIBUTES *attributes, BOOL inherit);
static BOOL mailslot_nt_name(LPCWSTR name, WCHAR *buffer, UNICODE_STRING *us, OBJECT_ATTRIBUTES *attributes, BOOL inherit);
EXPORT HANDLE WINAPI CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file);
static BOOL is_dos_device_name(const WCHAR *name);
static BOOL vol_prefix_ci(const WCHAR *s, const WCHAR *prefix);
static DWORD vol_len(const WCHAR *s);
EXPORT BOOL WINAPI CloseHandle(HANDLE h);
static void port_note(HANDLE file, LPOVERLAPPED overlapped, ULONG_PTR bytes);      /* (extra3.inc: a finished overlapped call is queued at the port its file is tied to) */
static void port_unlink(HANDLE file);
/* The files opened with FILE_FLAG_OVERLAPPED (every file is opened for synchronous calls underneath; this is what the flag said). */
static HANDLE volatile g_overlapped_files[256];
static void overlapped_note(HANDLE h) { for (int i = 0; i < 256; ++i) if (__sync_bool_compare_and_swap(&g_overlapped_files[i], (HANDLE)NULL, h)) return; }
static void unbuffered_forget(HANDLE h);
static void overlapped_forget(HANDLE h) { for (int i = 0; i < 256; ++i) __sync_bool_compare_and_swap(&g_overlapped_files[i], h, (HANDLE)NULL); unbuffered_forget(h); }
static BOOL overlapped_known(HANDLE h) { for (int i = 0; i < 256; ++i) if (g_overlapped_files[i] == h) return TRUE; return FALSE; }
/* Those of them opened without buffering as well: a call on one goes to the disk, so it is never answered at once. */
static HANDLE volatile g_unbuffered_files[256];
static void unbuffered_note(HANDLE h) { for (int i = 0; i < 256; ++i) if (__sync_bool_compare_and_swap(&g_unbuffered_files[i], (HANDLE)NULL, h)) return; }
static void unbuffered_forget(HANDLE h) { for (int i = 0; i < 256; ++i) __sync_bool_compare_and_swap(&g_unbuffered_files[i], h, (HANDLE)NULL); }
static BOOL unbuffered_known(HANDLE h) { for (int i = 0; i < 256; ++i) if (g_unbuffered_files[i] == h) return TRUE; return FALSE; }
EXPORT BOOL WINAPI GetFileAttributesExW(LPCWSTR path, int level, LPVOID data);
EXPORT DWORD WINAPI GetFinalPathNameByHandleW(HANDLE h, LPWSTR buffer, DWORD capacity, DWORD flags);
static BOOL fail_nt(NTSTATUS status) { set_error(RtlNtStatusToDosError(status)); return FALSE; }
static PVOID heap(void) { return RtlProcessHeap(); }
static WCHAR upper(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
static int equals_ignore_case(const WCHAR *a, const WCHAR *b) {
  for (;; ++a, ++b) {
    if (upper(*a) != upper(*b)) return 0;
    if (!*a) return 1;
  }
}

EXPORT DWORD WINAPI GetLastError(void) { return TEB_LAST_ERROR(NtCurrentTeb()); }
EXPORT void WINAPI SetLastError(DWORD error) { set_error(error); }
EXPORT void WINAPI SetLastErrorEx(DWORD error, DWORD type) { (void)type; set_error(error); }

/* ---- text conversion: the ANSI code page is UTF-8 ---------------------------------------------------------------------------------------------------------- */

#include "codepage.inc"
/* The lstr functions of Windows end quietly on an address that cannot be read; the pages are probed as the string crosses into them. */
static BOOL range_allowed(const void *p, SIZE_T length, BOOL write);
static int probe_length_a(const char *s) {
  int n = 0;
  for (;; ++n) {
    if ((n == 0 || (((ULONG_PTR)(s + n) & 0xFFF) == 0)) && !range_allowed(s + n, 1, FALSE)) return -1;
    if (!s[n]) return n;
  }
}
static int probe_length_w(const WCHAR *s) {
  int n = 0;
  for (;; ++n) {
    if ((n == 0 || (((ULONG_PTR)(s + n) & 0xFFF) < 2)) && !range_allowed(s + n, 2, FALSE)) return -1;
    if (!s[n]) return n;
  }
}
EXPORT int WINAPI lstrlenA(LPCSTR s) { if (!s) return 0; const int n = probe_length_a(s); return n < 0 ? 0 : n; }
EXPORT int WINAPI lstrlenW(LPCWSTR s) { if (!s) return 0; const int n = probe_length_w(s); return n < 0 ? 0 : n; }
EXPORT LPSTR WINAPI lstrcpyA(LPSTR d, LPCSTR s) {
  const int n = s ? probe_length_a(s) : -1;
  if (n < 0 || !d || !range_allowed(d, (UINT_PTR)n + 1, TRUE)) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  memcpy(d, s, (size_t_)n + 1);
  return d;
}
EXPORT LPWSTR WINAPI lstrcpyW(LPWSTR d, LPCWSTR s) {
  const int n = s ? probe_length_w(s) : -1;
  if (n < 0 || !d || !range_allowed(d, ((UINT_PTR)n + 1) * 2, TRUE)) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  memcpy(d, s, ((size_t_)n + 1) * 2);
  return d;
}
EXPORT LPSTR WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n) {
  if (n <= 0) return d;
  if (!d || !s || !range_allowed(d, (UINT_PTR)n, TRUE)) return NULL;
  int i = 0;
  for (; i < n - 1; ++i) {
    if ((i == 0 || (((ULONG_PTR)(s + i) & 0xFFF) == 0)) && !range_allowed(s + i, 1, FALSE)) break;
    if (!s[i]) break;
    d[i] = s[i];
  }
  d[i] = 0;
  return d;
}
EXPORT LPWSTR WINAPI lstrcpynW(LPWSTR d, LPCWSTR s, int n) {
  if (n <= 0) return d;
  if (!d || !s || !range_allowed(d, (UINT_PTR)n * 2, TRUE)) return NULL;
  int i = 0;
  for (; i < n - 1; ++i) {
    if ((i == 0 || (((ULONG_PTR)(s + i) & 0xFFF) < 2)) && !range_allowed(s + i, 2, FALSE)) break;
    if (!s[i]) break;
    d[i] = s[i];
  }
  d[i] = 0;
  return d;
}
EXPORT LPSTR WINAPI lstrcatA(LPSTR d, LPCSTR s) { const int n = d ? probe_length_a(d) : -1; if (n < 0 || !lstrcpyA(d + n, s)) return NULL; return d; }
EXPORT LPWSTR WINAPI lstrcatW(LPWSTR d, LPCWSTR s) { const int n = d ? probe_length_w(d) : -1; if (n < 0 || !lstrcpyW(d + n, s)) return NULL; return d; }
/* The comparisons use the sorting of the locale (nls.inc): "co-op" and "coop" are near, upper case follows lower case. */
EXPORT int WINAPI CompareStringW(DWORD locale, DWORD flags, LPCWSTR a, int an, LPCWSTR b, int bn);
EXPORT int WINAPI CompareStringA(DWORD locale, DWORD flags, LPCSTR a, int an, LPCSTR b, int bn);
EXPORT int WINAPI lstrcmpA(LPCSTR a, LPCSTR b) {
  if (!a || !b) return a == b ? 0 : !a ? -1 : 1;
  return CompareStringA(0x400, 0, a, -1, b, -1) - 2;
}
EXPORT int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b) {
  if (!a || !b) return a == b ? 0 : !a ? -1 : 1;
  return CompareStringW(0x400, 0, a, -1, b, -1) - 2;
}
EXPORT int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b) {
  if (!a || !b) return a == b ? 0 : !a ? -1 : 1;
  return CompareStringA(0x400, 1, a, -1, b, -1) - 2;
}
EXPORT int WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b) {
  if (!a || !b) return a == b ? 0 : !a ? -1 : 1;
  return CompareStringW(0x400, 1, a, -1, b, -1) - 2;
}
/* The bare (unsuffixed) lstr* names are their own real exports on a real kernel32.dll too, not just source-level UNICODE macros - always the ANSI ones. */
EXPORT int WINAPI lstrlen(LPCSTR s) { return lstrlenA(s); }
EXPORT LPSTR WINAPI lstrcpy(LPSTR d, LPCSTR s) { return lstrcpyA(d, s); }
EXPORT LPSTR WINAPI lstrcpyn(LPSTR d, LPCSTR s, int n) { return lstrcpynA(d, s, n); }
EXPORT LPSTR WINAPI lstrcat(LPSTR d, LPCSTR s) { return lstrcatA(d, s); }
EXPORT int WINAPI lstrcmp(LPCSTR a, LPCSTR b) { return lstrcmpA(a, b); }
EXPORT int WINAPI lstrcmpi(LPCSTR a, LPCSTR b) { return lstrcmpiA(a, b); }
/* Wide text from an ANSI argument, in memory from the process heap (the caller frees it). */
static WCHAR *to_wide(LPCSTR text) {
  if (!text) return NULL;
  const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  if (n <= 0) return NULL;
  WCHAR *w = RtlAllocateHeap(heap(), 0, (SIZE_T)n * 2);
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  MultiByteToWideChar(CP_UTF8, 0, text, -1, w, n);
  return w;
}
static void free_wide(WCHAR *w) { if (w) RtlFreeHeap(heap(), 0, w); }
/* Copies a wide result back as ANSI. Returns what the A functions return: length without the NUL, the size needed (with the NUL) if the buffer is too small, 0 on failure. */
static DWORD wide_result_to_ansi(const WCHAR *w, DWORD length, LPSTR buffer, DWORD capacity) {
  const int need = WideCharToMultiByte(CP_UTF8, 0, w, (int)length, NULL, 0, NULL, NULL);
  if (need < 0) return 0;
  if ((DWORD)need + 1 > capacity) return (DWORD)need + 1;
  WideCharToMultiByte(CP_UTF8, 0, w, (int)length, buffer, (int)capacity, NULL, NULL);
  buffer[need] = 0;
  return (DWORD)need;
}

/* ---- memory ------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT LPVOID WINAPI VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
  PVOID base = address;
  SIZE_T bytes = size;
  const NTSTATUS status = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &bytes, type, protect);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return base;
}
EXPORT BOOL WINAPI VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
  PVOID base = address;
  SIZE_T bytes = size;
  const NTSTATUS status = NtFreeVirtualMemory(NtCurrentProcess(), &base, &bytes, type);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
  if (!old) { set_error(ERROR_NOACCESS); return FALSE; }
  PVOID base = address;
  SIZE_T bytes = size;
  ULONG previous = 0;
  const NTSTATUS status = NtProtectVirtualMemory(NtCurrentProcess(), &base, &bytes, protect, &previous);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (old) *old = previous;
  return TRUE;
}
EXPORT SIZE_T WINAPI VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length) {
  SIZE_T returned = 0;
  const NTSTATUS status = NtQueryVirtualMemory(NtCurrentProcess(), (PVOID)address, 0, info, length, &returned);
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0; }
  return returned;
}
EXPORT BOOL WINAPI VirtualLock(LPVOID a, SIZE_T s) { (void)a; (void)s; return TRUE; }
EXPORT BOOL WINAPI VirtualUnlock(LPVOID a, SIZE_T s) { (void)a; (void)s; return TRUE; }

EXPORT HANDLE WINAPI GetProcessHeap(void) { return heap(); }
EXPORT HANDLE WINAPI HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum) {
  if (maximum && initial > maximum) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  PVOID h = RtlCreateHeap(options, NULL, maximum, initial, NULL, NULL);
  if (!h) set_error(ERROR_NOT_ENOUGH_MEMORY);
  return h;
}
EXPORT BOOL WINAPI HeapDestroy(HANDLE h) {
  if (RtlDestroyHeap(h) == NULL) return TRUE;
  set_error(ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT LPVOID WINAPI HeapAlloc(HANDLE h, DWORD flags, SIZE_T bytes) { return RtlAllocateHeap(h, flags, bytes); }
EXPORT LPVOID WINAPI HeapReAlloc(HANDLE h, DWORD flags, LPVOID memory, SIZE_T bytes) { return RtlReAllocateHeap(h, flags, memory, bytes); }
EXPORT BOOL WINAPI HeapFree(HANDLE h, DWORD flags, LPVOID memory) { return RtlFreeHeap(h, flags, memory) ? TRUE : FALSE; }
EXPORT SIZE_T WINAPI HeapSize(HANDLE h, DWORD flags, LPCVOID memory) { return RtlSizeHeap(h, flags, (PVOID)memory); }
EXPORT BOOL WINAPI HeapValidate(HANDLE h, DWORD flags, LPCVOID memory) { return RtlValidateHeap(h, flags, (PVOID)memory); }
IMPORT ULONG NTAPI RtlCompactHeap(PVOID heap, ULONG flags);
IMPORT NTSTATUS NTAPI RtlSetHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T length);
IMPORT NTSTATUS NTAPI RtlQueryHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T length, PSIZE_T returned);
EXPORT BOOL WINAPI HeapSetInformation(HANDLE h, int info_class, PVOID info, SIZE_T length) {
  const NTSTATUS status = RtlSetHeapInformation(h, (ULONG)info_class, info, length);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI HeapQueryInformation(HANDLE h, int info_class, PVOID info, SIZE_T length, PSIZE_T returned) {
  const NTSTATUS status = RtlQueryHeapInformation(h, (ULONG)info_class, info, length, returned);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT SIZE_T WINAPI HeapCompact(HANDLE h, DWORD flags) { return RtlCompactHeap(h, flags); }
EXPORT BOOL WINAPI HeapLock(HANDLE h) { return RtlLockHeap(h) ? TRUE : (set_error(ERROR_INVALID_PARAMETER), FALSE); }
EXPORT BOOL WINAPI HeapUnlock(HANDLE h) { return RtlUnlockHeap(h) ? TRUE : (set_error(ERROR_INVALID_PARAMETER), FALSE); }
#include "memory.inc"

/* ---- the process: ids, command line, environment, modules, version ---------------------------------------------------------------------------------------- */

EXPORT void WINAPI ExitProcess(UINT code) { RtlExitUserProcess((NTSTATUS)code); }
EXPORT BOOL WINAPI TerminateProcess(HANDLE process, UINT code) {
  if (!process) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (process != NtCurrentProcess()) {
    const NTSTATUS status = NtTerminateProcess(process, (NTSTATUS)code);
    return NT_SUCCESS(status) ? TRUE : fail_nt(status);
  }
  RtlExitUserProcess((NTSTATUS)code);
  return TRUE;
}
EXPORT void WINAPI ExitThread(DWORD code) { RtlExitUserThread((NTSTATUS)code); }
EXPORT void WINAPI FatalExit(int code) { RtlExitUserProcess((NTSTATUS)code); }
EXPORT HANDLE WINAPI GetCurrentProcess(void) { return NtCurrentProcess(); }
EXPORT HANDLE WINAPI GetCurrentThread(void) { return NtCurrentThread(); }
EXPORT DWORD WINAPI GetCurrentProcessId(void) { return (DWORD)TEB_CLIENT_PROCESS(NtCurrentTeb()); }
EXPORT DWORD WINAPI GetCurrentThreadId(void) { return (DWORD)TEB_CLIENT_THREAD(NtCurrentTeb()); }
EXPORT DWORD WINAPI GetProcessId(HANDLE h) {
  if (h == NtCurrentProcess()) return GetCurrentProcessId();
  ULONGLONG info[6];
  const NTSTATUS status = NtQueryInformationProcess(h, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0; }
  return (DWORD)info[4];
}
EXPORT BOOL WINAPI GetExitCodeProcess(HANDLE h, LPDWORD code) {
  if (h == NtCurrentProcess()) { *code = STILL_ACTIVE; return TRUE; }
  ULONGLONG info[6];
  const NTSTATUS status = NtQueryInformationProcess(h, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *code = (DWORD)info[0];
  return TRUE;
}
EXPORT BOOL WINAPI IsDebuggerPresent(void) { return NtCurrentPeb()->BeingDebugged != 0; }
EXPORT BOOL WINAPI CheckRemoteDebuggerPresent(HANDLE h, LPBOOL present) {
  if (!h || !present) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  ULONG_PTR port = 0;
  const NTSTATUS status = NtQueryInformationProcess(h, 7 /* ProcessDebugPort */, &port, sizeof(port), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *present = port != 0;
  return TRUE;
}
EXPORT void WINAPI DebugBreak(void) { __asm__ volatile("int3"); }
/* A debugger is told with a DBG_PRINTEXCEPTION_C exception (the length with the NUL, the text); without one nothing happens. */
EXPORT void WINAPI OutputDebugStringA(LPCSTR s) {
  if (!s || !NtCurrentPeb()->BeingDebugged) return;
  EXCEPTION_RECORD record;
  memset(&record, 0, sizeof(record));
  record.ExceptionCode = 0x40010006u;
  record.NumberParameters = 2;
  record.ExceptionInformation[0] = strlen(s) + 1;
  record.ExceptionInformation[1] = (ULONG_PTR)s;
  NtTcosDebugException(&record, TRUE, NULL);
}
EXPORT void WINAPI OutputDebugStringW(LPCWSTR s) {
  if (!s || !NtCurrentPeb()->BeingDebugged) return;
  char text[1024];
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, text, sizeof(text), NULL, NULL);
  if (n > 0) OutputDebugStringA(text); else { text[sizeof(text) - 1] = 0; OutputDebugStringA(text); }
}
IMPORT NTSTATUS NTAPI RtlSetThreadErrorMode(ULONG, ULONG *);
IMPORT ULONG NTAPI RtlGetThreadErrorMode(void);
static DWORD thread_error_mode_from_rtl(ULONG rtl) { return ((rtl & 0x10) ? 1u : 0u) | ((rtl & 0x20) ? 2u : 0u) | ((rtl & 0x40) ? 0x8000u : 0u); }
EXPORT DWORD WINAPI GetThreadErrorMode(void) { return thread_error_mode_from_rtl(RtlGetThreadErrorMode()); }
EXPORT BOOL WINAPI SetThreadErrorMode(DWORD mode, LPDWORD old) {
  if (mode & ~(DWORD)(1 | 2 | 0x8000)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  ULONG previous = 0;
  const ULONG rtl = ((mode & 1) ? 0x10u : 0u) | ((mode & 2) ? 0x20u : 0u) | ((mode & 0x8000) ? 0x40u : 0u);
  const NTSTATUS s = RtlSetThreadErrorMode(rtl, &previous);
  if (!NT_SUCCESS(s)) return fail_nt(s);
  if (old) *old = thread_error_mode_from_rtl(previous);
  return TRUE;
}
EXPORT UINT WINAPI SetErrorMode(UINT mode) {
  ULONG old = 0;
  NtQueryInformationProcess(NtCurrentProcess(), 12, &old, sizeof(old), NULL);
  ULONG value = mode;
  NtSetInformationProcess(NtCurrentProcess(), 12, &value, sizeof(value));
  return old;
}
EXPORT UINT WINAPI GetErrorMode(void) {
  ULONG mode = 0;
  NtQueryInformationProcess(NtCurrentProcess(), 12, &mode, sizeof(mode), NULL);
  return mode;
}
EXPORT BOOL WINAPI SetProcessShutdownParameters(DWORD level, DWORD flags) { (void)level; (void)flags; return TRUE; }
EXPORT BOOL WINAPI DisableThreadLibraryCalls(HMODULE m) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr) return FALSE;
  for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *module = (LDR_DATA_TABLE_ENTRY *)e;
    if (module->DllBase == (PVOID)m) { module->Flags |= 0x00040000u; return TRUE; }
  }
  set_error(ERROR_INVALID_HANDLE);
  return FALSE;
}

static RTL_USER_PROCESS_PARAMETERS *parameters(void) { return NtCurrentPeb()->ProcessParameters; }

/* (the address is kept from the first call: a program that clears the field in the parameters afterwards still gets its command line) */
EXPORT LPWSTR WINAPI GetCommandLineW(void) {
  static LPWSTR cached;
  if (!cached) cached = parameters()->CommandLine.Buffer;
  return cached;
}
EXPORT LPSTR WINAPI GetCommandLineA(void) {
  static LPSTR cached;
  if (!cached) {
    const UNICODE_STRING *line = &parameters()->CommandLine;
    const int n = WideCharToMultiByte(CP_UTF8, 0, line->Buffer, line->Length / 2, NULL, 0, NULL, NULL);
    cached = RtlAllocateHeap(heap(), 0, (SIZE_T)n + 1);
    if (!cached) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, line->Buffer, line->Length / 2, cached, n, NULL, NULL);
    cached[n] = 0;
  }
  return cached;
}

EXPORT LPWSTR WINAPI GetEnvironmentStringsW(void) { return parameters()->Environment; }
EXPORT BOOL WINAPI FreeEnvironmentStringsW(LPWSTR block) { (void)block; return TRUE; }
EXPORT LPSTR WINAPI GetEnvironmentStringsA(void) {
  const WCHAR *w = parameters()->Environment;
  size_t_ units = 0;
  while (w[units] || w[units + 1]) ++units;
  units += 2;
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)units, NULL, 0, NULL, NULL);
  LPSTR a = RtlAllocateHeap(heap(), 0, (SIZE_T)n);
  if (a) WideCharToMultiByte(CP_UTF8, 0, w, (int)units, a, n, NULL, NULL);
  return a;
}
EXPORT BOOL WINAPI FreeEnvironmentStringsA(LPSTR block) { return block ? RtlFreeHeap(heap(), 0, block) : TRUE; }
EXPORT LPSTR WINAPI GetEnvironmentStrings(void) { return GetEnvironmentStringsA(); }      /* its own real export too, always the ANSI one */

/* The value of NAME in an environment block ("NAME=value\0...\0\0"); the length goes to *length, NULL if there is no such variable. */
static const WCHAR *find_variable(const WCHAR *block, const WCHAR *name, size_t_ *length) {
  const size_t_ name_length = wcslen(name);
  for (const WCHAR *e = block; *e; e += wcslen(e) + 1) {
    size_t_ i = 0;
    while (i < name_length && e[i] && upper(e[i]) == upper(name[i])) ++i;
    if (i == name_length && e[i] == '=') {
      *length = wcslen(e + i + 1);
      return e + i + 1;
    }
  }
  return NULL;
}
EXPORT DWORD WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD capacity) {
  size_t_ length = 0;
  if (!name) { set_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
  const WCHAR *value = find_variable(parameters()->Environment, name, &length);
  if (!value) { set_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
  if (length + 1 > capacity) return (DWORD)length + 1;
  memcpy(buffer, value, (length + 1) * 2);
  return (DWORD)length;
}
EXPORT DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD capacity) {
  if (!name) { set_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
  WCHAR *w = to_wide(name);
  if (!w) return 0;
  size_t_ length = 0;
  const WCHAR *value = find_variable(parameters()->Environment, w, &length);
  free_wide(w);
  if (!value) { set_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
  if (length == 0) {      /* a variable with an empty value: no error, the size with the end mark is 1 */
    if (capacity == 0) return 1;
    buffer[0] = 0;
    return 0;
  }
  return wide_result_to_ansi(value, (DWORD)length, buffer, capacity);
}
/* The whole block of the process is replaced by `block` (NAME=value entries ended by an empty one); an entry needs a name before its first '='. */
EXPORT BOOL WINAPI SetEnvironmentStringsW(LPWSTR block) {
  if (!block) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  size_t_ units = 0;
  for (const WCHAR *e = block; *e; e += wcslen(e) + 1) {
    const WCHAR *eq = e + 1;      /* (a name may start with '=': "=C:=C:\") */
    while (*eq && *eq != '=') ++eq;
    if (!*eq || eq == e) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    units += wcslen(e) + 1;
  }
  WCHAR *fresh = RtlAllocateHeap(heap(), 0, (units + 2) * 2);
  if (!fresh) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  memcpy(fresh, block, units * 2);
  fresh[units] = 0;
  fresh[units + 1] = 0;
  parameters()->Environment = fresh;
  return TRUE;
}
/* Builds a new block with NAME set (or removed when value is NULL) and points the process parameters at it. */
IMPORT NTSTATUS NTAPI RtlSetEnvironmentVar(PWSTR *penv, PCWSTR name, SIZE_T name_len, PCWSTR value, SIZE_T value_len);
EXPORT BOOL WINAPI SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
  if (!name || !*name || wcslen(name) > 32767) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  for (const WCHAR *c = name; *c; ++c) if (*c == '=') { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const NTSTATUS status = RtlSetEnvironmentVar(NULL, name, wcslen(name), value, value ? wcslen(value) : 0);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
  WCHAR *n = to_wide(name), *v = value ? to_wide(value) : NULL;
  const BOOL ok = n ? SetEnvironmentVariableW(n, v) : FALSE;
  free_wide(n);
  free_wide(v);
  return ok;
}
/* What fits is copied as the real one does it: a variable's value goes in whole or not at all (after one that does not fit only the end mark is written), plain text goes
   in up to capacity - 1 characters; the end mark follows only when the whole result fits. The result is the size needed, with the end mark. */
EXPORT DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR source, LPWSTR dest, DWORD capacity) {
  DWORD needed = 0;
  BOOL stopped = FALSE;      /* a variable did not fit: nothing more is written */
  for (const WCHAR *s = source; *s;) {
    const WCHAR *close = NULL;
    if (*s == '%') for (const WCHAR *e = s + 1; *e; ++e) if (*e == '%') { close = e; break; }
    size_t_ length = 0;
    const WCHAR *value = NULL;
    if (close && close > s + 1) {
      WCHAR name[128];
      size_t_ n = (size_t_)(close - s - 1);
      if (n < 128) {
        memcpy(name, s + 1, n * 2);
        name[n] = 0;
        value = find_variable(parameters()->Environment, name, &length);
      }
    }
    if (value) {
      if (dest && !stopped) {
        if (needed + length < capacity) {
          memcpy(dest + needed, value, length * 2);
          dest[needed + length] = 0;      /* (the end mark waits where the next character goes) */
        } else {
          if (capacity > 0) dest[needed] = 0;
          stopped = TRUE;
        }
      }
      needed += (DWORD)length;
      s = close + 1;
    } else {
      if (dest && !stopped && needed + 1 < capacity) dest[needed] = *s;
      ++needed;
      ++s;
    }
  }
  if (dest && !stopped && needed < capacity) dest[needed] = 0;
  return needed + 1;
}
/* The ANSI function converts what the wide one produced, and needs a byte more than that: when the buffer is not bigger than the size needed nothing of the result is copied,
   the buffer is made an empty string and the size needed plus one is returned. */
EXPORT DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR source, LPSTR dest, DWORD capacity) {
  WCHAR *w = to_wide(source);
  if (!w) return 0;
  const DWORD needed = ExpandEnvironmentStringsW(w, NULL, 0);
  if (capacity <= needed) {
    free_wide(w);
    if (dest) dest[0] = 0;
    return needed + 1;
  }
  WCHAR *wide = RtlAllocateHeap(heap(), 0, (size_t_)needed * 2 + 2);
  if (!wide) { free_wide(w); return 0; }
  ExpandEnvironmentStringsW(w, wide, needed);
  free_wide(w);
  for (DWORD i = 0; i < needed; i++) dest[i] = (char)wide[i];      /* (ASCII: a character is a byte) */
  RtlFreeHeap(heap(), 0, wide);
  return needed;
}

/* The startup information the process was created with: the fields of the process parameters (the strings are the ones in the parameters). The standard handles are in it
   only if the process was started with STARTF_USESTDHANDLES: the wide version leaves them as they are, the ANSI one sets them to INVALID_HANDLE_VALUE. */
EXPORT void WINAPI GetStartupInfoW(LPSTARTUPINFOW info) {
  const RTL_USER_PROCESS_PARAMETERS *pp = parameters();
  info->cb = sizeof(*info);
  info->lpReserved = NULL;
  info->lpDesktop = pp->DesktopInfo.Buffer;
  info->lpTitle = pp->WindowTitle.Buffer;
  info->dwX = pp->StartingX; info->dwY = pp->StartingY; info->dwXSize = pp->CountX; info->dwYSize = pp->CountY;
  info->dwXCountChars = pp->CountCharsX; info->dwYCountChars = pp->CountCharsY; info->dwFillAttribute = pp->FillAttribute;
  info->dwFlags = pp->WindowFlags;
  info->wShowWindow = (WORD)pp->ShowWindowFlags;
  info->cbReserved2 = 0;
  info->lpReserved2 = NULL;
  if (pp->WindowFlags & 0x100) {
    info->hStdInput = pp->StandardInput;
    info->hStdOutput = pp->StandardOutput;
    info->hStdError = pp->StandardError;
  }
}
/* The same with ANSI strings, converted once and kept: the first call makes it and later calls give the same answer whatever happens to the parameters. */
EXPORT void WINAPI GetStartupInfoA(LPSTARTUPINFOA info) {
  static char title[260], desktop[260];
  static STARTUPINFOA cached;
  static volatile LONG state;      /* 0: not made, 1: being made, 2: made */
  if (state != 2) {
    if (__sync_bool_compare_and_swap(&state, 0, 1)) {
      const RTL_USER_PROCESS_PARAMETERS *pp = parameters();
      STARTUPINFOW wide;
      GetStartupInfoW(&wide);
      memset(&cached, 0, sizeof(cached));
      cached.cb = sizeof(cached);
      if (wide.lpTitle) { WideCharToMultiByte(CP_UTF8, 0, wide.lpTitle, -1, title, sizeof(title), NULL, NULL); cached.lpTitle = title; }
      if (wide.lpDesktop) { WideCharToMultiByte(CP_UTF8, 0, wide.lpDesktop, -1, desktop, sizeof(desktop), NULL, NULL); cached.lpDesktop = desktop; }
      cached.dwX = wide.dwX; cached.dwY = wide.dwY; cached.dwXSize = wide.dwXSize; cached.dwYSize = wide.dwYSize;
      cached.dwXCountChars = wide.dwXCountChars; cached.dwYCountChars = wide.dwYCountChars; cached.dwFillAttribute = wide.dwFillAttribute;
      cached.dwFlags = wide.dwFlags; cached.wShowWindow = wide.wShowWindow;
      if (pp->WindowFlags & 0x100) { cached.hStdInput = pp->StandardInput; cached.hStdOutput = pp->StandardOutput; cached.hStdError = pp->StandardError; }
      else cached.hStdInput = cached.hStdOutput = cached.hStdError = INVALID_HANDLE_VALUE;
      state = 2;
    } else while (state != 2) NtYieldExecution();
  }
  memcpy(info, &cached, sizeof(cached));
}

NTSTATUS NTAPI RtlGetVersion(RTL_OSVERSIONINFOW *info);
IMPORT NTSTATUS NTAPI RtlQueryInformationActiveActivationContext(ULONG class, PVOID buffer, SIZE_T length, SIZE_T *retlen);
/* What GetVersion and GetVersionEx tell a program: not the version of the system (RtlGetVersion and VerifyVersionInfo know that) but the one the program was made for. A
 * manifest that lists the systems the program supports (supportedOS) gets the newest of them; without such a list the version in the program's own header decides: 10.0 or
 * newer is 10.0, 6.3 up to there is 6.3, anything older is 6.2 (a program of Windows 7 or Vista without a manifest is told Windows 8). */
typedef struct { DWORD major, minor, build; } ReportedVersion;
static const struct { DWORD a; WORD b, c; BYTE d[8]; DWORD major, minor, build; } kSupportedOs[] = {
  {0x8e0f7a12, 0xbfb3, 0x4fe8, {0xb9, 0xa5, 0x48, 0xfd, 0x50, 0xa1, 0x5a, 0x9a}, 10, 0, 19045},      /* Windows 10 */
  {0x1f676c76, 0x80e1, 0x4239, {0x95, 0xbb, 0x83, 0xd0, 0xf6, 0xd0, 0xda, 0x78}, 6, 3, 9600},      /* Windows 8.1 */
  {0x4a2f28e3, 0x53b9, 0x4441, {0xba, 0x9c, 0xd6, 0x9d, 0x4a, 0x4a, 0x6e, 0x38}, 6, 2, 9200},      /* Windows 8 */
  {0x35138b9a, 0x5d96, 0x4fbd, {0x8e, 0x2d, 0xa2, 0x44, 0x02, 0x25, 0xf9, 0x3a}, 6, 1, 7601},      /* Windows 7 */
  {0xe2011457, 0x1546, 0x43c5, {0xa5, 0xfe, 0x00, 0x8d, 0xee, 0xe3, 0xd3, 0xf0}, 6, 0, 6002},      /* Windows Vista */
};
static ReportedVersion reported_version(void) {
  static volatile LONG state;      /* 0: not looked at, 1: being worked out, 2: done */
  static ReportedVersion cached;
  if (state != 2) {
    if (__sync_bool_compare_and_swap(&state, 0, 1)) {
      ReportedVersion r = {6, 2, 9200};
      int found = 0;
      BYTE buffer[8 + 32 * 16];
      SIZE_T needed = 0;
      if (NT_SUCCESS(RtlQueryInformationActiveActivationContext(6, buffer, sizeof(buffer), &needed))) {
        const DWORD count = *(const DWORD *)buffer;
        for (DWORD k = 0; k < count && k < 16; ++k) {
          const BYTE *e = buffer + 8 + (size_t_)k * 32;
          if (*(const DWORD *)(e + 16) != 1) continue;      /* supportedOS */
          for (size_t_ g = 0; g < sizeof(kSupportedOs) / sizeof(kSupportedOs[0]); ++g) {
            if (memcmp(e, &kSupportedOs[g].a, 4) || memcmp(e + 4, &kSupportedOs[g].b, 2) || memcmp(e + 6, &kSupportedOs[g].c, 2) || memcmp(e + 8, kSupportedOs[g].d, 8)) continue;
            if (!found || kSupportedOs[g].major > r.major || (kSupportedOs[g].major == r.major && kSupportedOs[g].minor > r.minor)) { r.major = kSupportedOs[g].major; r.minor = kSupportedOs[g].minor; r.build = kSupportedOs[g].build; }
            found = 1;
          }
        }
      }
      if (!found) {
        const BYTE *base = (const BYTE *)NtCurrentPeb()->ImageBaseAddress;
        const DWORD pe = *(const DWORD *)(base + 0x3C);
        const DWORD image = ((DWORD)*(const WORD *)(base + pe + 24 + 0x28) << 16) | *(const WORD *)(base + pe + 24 + 0x2A);      /* the operating system version of the header */
        if ((image >> 16) >= 10) { r.major = 10; r.minor = 0; r.build = 19045; }
        else if ((image >> 16) > 6 || ((image >> 16) == 6 && (image & 0xFFFF) >= 3)) { r.major = 6; r.minor = 3; r.build = 9600; }
      }
      cached = r;
      state = 2;
    } else while (state != 2) NtYieldExecution();
  }
  return cached;
}
EXPORT DWORD WINAPI GetVersion(void) {
  const ReportedVersion r = reported_version();
  return (r.build << 16) | (r.minor << 8) | r.major;
}
/* GetVersionEx: the size says which structure; any other size is ERROR_INSUFFICIENT_BUFFER. The extended fields come from ntdll. */
EXPORT BOOL WINAPI GetVersionExW(LPOSVERSIONINFOW v) {
  if (!v || (v->dwOSVersionInfoSize != sizeof(OSVERSIONINFOW) && v->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW))) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  RTL_OSVERSIONINFOEXW info;
  memset(&info, 0, sizeof(info));
  info.dwOSVersionInfoSize = v->dwOSVersionInfoSize;
  if (!NT_SUCCESS(RtlGetVersion((RTL_OSVERSIONINFOW *)&info))) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  const ReportedVersion r = reported_version();
  info.dwMajorVersion = r.major; info.dwMinorVersion = r.minor; info.dwBuildNumber = r.build;
  memcpy(v, &info, v->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW) ? sizeof(OSVERSIONINFOEXW) : sizeof(OSVERSIONINFOW));
  return TRUE;
}
EXPORT BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA v) {
  if (!v || (v->dwOSVersionInfoSize != sizeof(OSVERSIONINFOA) && v->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXA))) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  RTL_OSVERSIONINFOEXW info;
  memset(&info, 0, sizeof(info));
  info.dwOSVersionInfoSize = sizeof(info);
  RtlGetVersion((RTL_OSVERSIONINFOW *)&info);
  const ReportedVersion r = reported_version();
  v->dwMajorVersion = r.major; v->dwMinorVersion = r.minor; v->dwBuildNumber = r.build; v->dwPlatformId = info.dwPlatformId; v->szCSDVersion[0] = 0;
  if (v->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA)) {
    OSVERSIONINFOEXA *ex = (OSVERSIONINFOEXA *)v;
    ex->wServicePackMajor = info.wServicePackMajor; ex->wServicePackMinor = info.wServicePackMinor; ex->wSuiteMask = info.wSuiteMask; ex->wProductType = info.wProductType; ex->wReserved = 0;
  }
  return TRUE;
}
static ULONGLONG get64(const BYTE *p) { ULONGLONG v; memcpy(&v, p, 8); return v; }
static DWORD get32(const BYTE *p) { DWORD v; memcpy(&v, p, 4); return v; }
static WORD get16(const BYTE *p) { WORD v; memcpy(&v, p, 2); return v; }
EXPORT void WINAPI GetSystemInfo(LPSYSTEM_INFO info) {
  memset(info, 0, sizeof(*info));
  BYTE basic[64] = {}, processor[16] = {};
  info->wProcessorArchitecture = 9;      /* PROCESSOR_ARCHITECTURE_AMD64 */
  info->dwPageSize = 4096;
  info->lpMinimumApplicationAddress = (LPVOID)0x10000;
  info->lpMaximumApplicationAddress = (LPVOID)0x00007FFFFFFEFFFFull;
  info->dwActiveProcessorMask = 1;
  info->dwNumberOfProcessors = 1;
  info->dwProcessorType = 8664;
  info->dwAllocationGranularity = 0x10000;
  info->wProcessorLevel = 6;
  if (NT_SUCCESS(NtQuerySystemInformation(0, basic, sizeof(basic), NULL))) {      /* SystemBasicInformation */
    info->dwPageSize = get32(basic + 8);
    info->dwAllocationGranularity = get32(basic + 24);
    info->lpMinimumApplicationAddress = (LPVOID)get64(basic + 32);
    info->lpMaximumApplicationAddress = (LPVOID)get64(basic + 40);
    info->dwActiveProcessorMask = get64(basic + 48);
    info->dwNumberOfProcessors = basic[56];
  }
  if (NT_SUCCESS(NtQuerySystemInformation(1, processor, sizeof(processor), NULL))) {      /* SystemProcessorInformation */
    info->wProcessorArchitecture = get16(processor);
    info->wProcessorLevel = get16(processor + 2);
    info->wProcessorRevision = get16(processor + 4);
  }
}
EXPORT void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO info) { GetSystemInfo(info); }
EXPORT BOOL WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX m) {
  if (m->dwLength < sizeof(MEMORYSTATUSEX)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BYTE basic[64] = {}, performance[312] = {};
  if (!NT_SUCCESS(NtQuerySystemInformation(0, basic, sizeof(basic), NULL)) || !NT_SUCCESS(NtQuerySystemInformation(2, performance, sizeof(performance), NULL))) { set_error(ERROR_INVALID_FUNCTION); return FALSE; }
  const ULONGLONG page = get32(basic + 8), total = (ULONGLONG)get32(basic + 12) * page, available = (ULONGLONG)get32(performance + 44) * page;
  m->dwMemoryLoad = total ? (DWORD)((total - available) * 100 / total) : 0;
  m->ullTotalPhys = total;
  m->ullAvailPhys = available;
  m->ullTotalPageFile = total;
  m->ullAvailPageFile = available;
  m->ullTotalVirtual = get64(basic + 40) - get64(basic + 32) + 1;
  m->ullAvailVirtual = m->ullTotalVirtual;      /* (the address space is not tracked in bytes left: what the process uses is taken off) */
  {
    ULONGLONG counters[16] = {0};
    if (NT_SUCCESS(NtQueryInformationProcess(NtCurrentProcess(), 3 /* ProcessVmCounters */, counters, sizeof(counters), NULL)) && counters[4] < m->ullAvailVirtual) m->ullAvailVirtual -= counters[4];
  }
  m->ullAvailExtendedVirtual = 0;
  return TRUE;
}
EXPORT void WINAPI GlobalMemoryStatus(LPMEMORYSTATUS m) {
  MEMORYSTATUSEX x;
  x.dwLength = sizeof(x);
  memset(m, 0, sizeof(*m));
  m->dwLength = sizeof(*m);
  if (!GlobalMemoryStatusEx(&x)) return;
  m->dwMemoryLoad = x.dwMemoryLoad;
  m->dwTotalPhys = x.ullTotalPhys;
  m->dwAvailPhys = x.ullAvailPhys;
  m->dwTotalPageFile = x.ullTotalPageFile;
  m->dwAvailPageFile = x.ullAvailPageFile;
  m->dwTotalVirtual = x.ullTotalVirtual;
  m->dwAvailVirtual = x.ullAvailVirtual;
}
EXPORT BOOL WINAPI GetPhysicallyInstalledSystemMemory(ULONGLONG *kilobytes) {
  if (!kilobytes) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  MEMORYSTATUSEX x;
  x.dwLength = sizeof(x);
  if (!GlobalMemoryStatusEx(&x)) return FALSE;
  *kilobytes = x.ullTotalPhys / 1024;
  return TRUE;
}
EXPORT BOOL WINAPI IsProcessorFeaturePresent(DWORD feature) { return feature == 6 || feature == 10; }
EXPORT BOOL WINAPI GetComputerNameW(LPWSTR name, LPDWORD size) {
  static const WCHAR kName[] = {'T', 'C', 'O', 'S', 0};
  if (*size < 5) { *size = 5; set_error(111 /* ERROR_BUFFER_OVERFLOW */); return FALSE; }
  memcpy(name, kName, sizeof(kName));
  *size = 4;
  return TRUE;
}
EXPORT BOOL WINAPI GetComputerNameA(LPSTR name, LPDWORD size) {
  if (*size < 5) { *size = 5; set_error(111 /* ERROR_BUFFER_OVERFLOW */); return FALSE; }
  memcpy(name, "TCOS", 5);
  *size = 4;
  return TRUE;
}

/* Modules: the loader lists the kernel built in PEB.Ldr. */
static LDR_DATA_TABLE_ENTRY *module_by_base(PVOID base) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr) return NULL;
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)e;      /* InLoadOrderLinks is the first member */
    if (m->DllBase == base) return m;
  }
  return NULL;
}
/* A library by name, with or without ".dll", in any case ("KERNEL32", "kernel32.dll"). */
static BOOL actctx_redirect(LPCWSTR name, WCHAR *out, DWORD capacity);
static BOOL actctx_redirect_key(LPCWSTR name, WCHAR *out, DWORD capacity, BOOL append_dll);
static LDR_DATA_TABLE_ENTRY *module_by_name(LPCWSTR name) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr) return NULL;
  /* a bare name that the active activation context redirects is the library of that file; otherwise the libraries of contexts are not seen under their names */
  WCHAR redirected[PATH_CHARS];
  BOOL bare = TRUE;
  for (const WCHAR *c = name; *c; ++c) if (*c == '\\' || *c == '/') bare = FALSE;
  const BOOL has_redirect = bare && actctx_redirect(name, redirected, PATH_CHARS);
  size_t_ length = wcslen(name);
  /* A path: only the file name counts. */
  for (size_t_ i = 0; i < length; ++i) if (name[i] == '\\' || name[i] == '/') { name += i + 1; length -= i + 1; i = (size_t_)-1; }
  const int has_extension = length >= 4 && name[length - 4] == '.';
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)e;
    const size_t_ base_length = m->BaseDllName.Length / 2;
    const WCHAR *base = m->BaseDllName.Buffer;
    size_t_ compare = base_length;
    if (!has_extension && base_length >= 4 && base[base_length - 4] == '.') compare = base_length - 4;
    if (compare != length) continue;
    size_t_ i = 0;
    while (i < length && upper(base[i]) == upper(name[i])) ++i;
    if (i != length) continue;
    if (has_redirect) {      /* only the library of that file */
      const WCHAR *full = m->FullDllName.Buffer;
      size_t_ k = 0;
      const size_t_ full_length = m->FullDllName.Length / 2;
      while (k < full_length && redirected[k] && upper(full[k]) == upper(redirected[k])) ++k;
      if (!(m->Flags & 0x10000000) || k != full_length || redirected[k]) continue;
    } else if (m->Flags & 0x10000000) continue;
    return m;
  }
  return NULL;
}
PVOID NTAPI TcosFindModuleByFile(PCWSTR path);
static BOOL has_separator(LPCWSTR name);
static DWORD full_path_w(LPCWSTR in, WCHAR *out, DWORD capacity);
EXPORT DWORD WINAPI GetLongPathNameW(LPCWSTR path, LPWSTR buffer, DWORD capacity);
/* A library is known by its long name; one loaded by its 8.3 alias ("C:\\dir\\LONGNA~1.DLL") keeps being told by the spelling it was loaded with. */
#define MODULE_SPELLINGS 8
static struct { PVOID base; WCHAR path[260]; } g_module_spellings[MODULE_SPELLINGS];
static void module_spelling_set(PVOID base, const WCHAR *spelled) {      /* NULL: the library is known by its own name */
  int free_slot = -1;
  for (int i = 0; i < MODULE_SPELLINGS; ++i) {
    if (g_module_spellings[i].base == base) { g_module_spellings[i].base = NULL; }
    if (!g_module_spellings[i].base && free_slot < 0) free_slot = i;
  }
  if (!spelled || free_slot < 0) return;
  size_t_ n = 0;
  for (; spelled[n] && n < 259; ++n) g_module_spellings[free_slot].path[n] = spelled[n];
  g_module_spellings[free_slot].path[n] = 0;
  g_module_spellings[free_slot].base = base;
}
static const WCHAR *module_spelling(PVOID base) {
  for (int i = 0; i < MODULE_SPELLINGS; ++i) if (g_module_spellings[i].base == base) return g_module_spellings[i].path;
  return NULL;
}
static BOOL has_tilde_name(const WCHAR *name) { for (; *name; ++name) if (*name == '~') return TRUE; return FALSE; }
EXPORT HMODULE WINAPI GetModuleHandleW(LPCWSTR name) {
  if (!name) return NtCurrentPeb()->ImageBaseAddress;
  LDR_DATA_TABLE_ENTRY *m = module_by_name(name);
  if (!m && (equals_ignore_case(name, (const WCHAR *)L"kernelbase.dll") || equals_ignore_case(name, (const WCHAR *)L"kernelbase"))) m = module_by_name((const WCHAR *)L"kernel32.dll");      /* (kernelbase's functions are kernel32's) */
  if (!m && has_separator(name)) {      /* a file: the library loaded from it, under whatever name */
    WCHAR full[PATH_CHARS];
    PVOID base;
    if (full_path_w(name, full, PATH_CHARS) && (base = TcosFindModuleByFile(full)) != NULL) return base;
    if (has_tilde_name(name) && full_path_w(name, full, PATH_CHARS)) {
      WCHAR expanded[PATH_CHARS];
      const DWORD n = GetLongPathNameW(full, expanded, PATH_CHARS);
      if (n != 0 && n < PATH_CHARS && (base = TcosFindModuleByFile(expanded)) != NULL) return base;
    }
  }
  if (!m) { set_error(ERROR_MOD_NOT_FOUND); return NULL; }
  return m->DllBase;
}
EXPORT HMODULE WINAPI GetModuleHandleA(LPCSTR name) {
  if (!name) return NtCurrentPeb()->ImageBaseAddress;
  WCHAR *w = to_wide(name);
  HMODULE m = w ? GetModuleHandleW(w) : NULL;
  free_wide(w);
  return m;
}
/* GET_MODULE_HANDLE_EX_FLAG_*: PIN 1 (never unload), UNCHANGED_REFCOUNT 2, FROM_ADDRESS 4. Without UNCHANGED_REFCOUNT the library has one more use, which FreeLibrary ends. */
static LDR_DATA_TABLE_ENTRY *module_containing(PVOID address) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr) return NULL;
  for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)e;
    if ((BYTE *)address >= (BYTE *)m->DllBase && (BYTE *)address < (BYTE *)m->DllBase + m->SizeOfImage) return m;
  }
  return NULL;
}
static DWORD dos_error_of_load(NTSTATUS status) {
  switch ((ULONG)status) {
    case 0xC0000135u: return ERROR_MOD_NOT_FOUND;
    case 0xC000007Bu: case 0xC000012Fu: case 0xC000011Bu: case 0xC0000131u: case 0xC0000130u: case 0xC000035Au: case 0xC00000BBu: return ERROR_BAD_EXE_FORMAT;      /* (a file that is no image this system can map) */
    case 0xC0000142u: return 1114;      /* ERROR_DLL_INIT_FAILED */
    case 0xC0000017u: return ERROR_NOT_ENOUGH_MEMORY;
    default: return RtlNtStatusToDosError(status);
  }
}
EXPORT BOOL WINAPI GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *module) {
  if (module) *module = NULL;
  if (!module || (flags & ~7u) || (flags & 3) == 3 || ((flags & 4) && !name)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  LDR_DATA_TABLE_ENTRY *m = (flags & 4) ? module_containing((PVOID)name) : (name ? module_by_name(name) : module_by_base(NtCurrentPeb()->ImageBaseAddress));
  if (!m) { set_error(ERROR_MOD_NOT_FOUND); return FALSE; }
  if (!(flags & 2) || (flags & 1)) {      /* another use (a pin is a use that is never given back) */
    UNICODE_STRING base_name = m->BaseDllName;
    PVOID handle = NULL;
    if (m->LoadCount != 0xFFFF) {
      const NTSTATUS status = LdrLoadDll(NULL, NULL, &base_name, &handle);
      if (!NT_SUCCESS(status)) { set_error(dos_error_of_load(status)); return FALSE; }
    }
  }
  if (module) *module = m->DllBase;
  return TRUE;
}
EXPORT BOOL WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *module) {
  if (flags & 4) return GetModuleHandleExW(flags, (LPCWSTR)name, module);
  WCHAR *w = name ? to_wide(name) : NULL;
  if (name && !w) return FALSE;
  const BOOL ok = GetModuleHandleExW(flags, w, module);
  free_wide(w);
  return ok;
}
EXPORT DWORD WINAPI GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD capacity) {
  LDR_DATA_TABLE_ENTRY *m = module_by_base(module ? module : NtCurrentPeb()->ImageBaseAddress);
  if (!m) { set_error(ERROR_MOD_NOT_FOUND); return 0; }
  const WCHAR *spelled = module_spelling(m->DllBase);
  const WCHAR *text = spelled ? spelled : m->FullDllName.Buffer;
  const DWORD length = spelled ? (DWORD)wcslen(spelled) : m->FullDllName.Length / 2;
  if (length + 1 > capacity) {
    if (capacity) { memcpy(buffer, text, (capacity - 1) * 2); buffer[capacity - 1] = 0; }
    set_error(ERROR_INSUFFICIENT_BUFFER);
    return capacity;
  }
  memcpy(buffer, text, length * 2);
  buffer[length] = 0;
  set_error(ERROR_SUCCESS);
  return length;
}
EXPORT DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD capacity) {
  WCHAR wide[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(module, wide, MAX_PATH * 2);
  if (n == 0) return 0;
  if (n + 1 > capacity) {      /* too small: what fits, ended by a NUL, and the size of the buffer */
    if (capacity) {
      wide_result_to_ansi(wide, capacity - 1, buffer, capacity);
      buffer[capacity - 1] = 0;
    }
    set_error(ERROR_INSUFFICIENT_BUFFER);
    return capacity;
  }
  return wide_result_to_ansi(wide, n, buffer, capacity);
}

typedef struct { ULONG Characteristics, TimeDateStamp; USHORT MajorVersion, MinorVersion; ULONG Name, Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals; } ExportDirectory;

/* The address of an exported function of the image at `base`, by name or (name == NULL) ordinal; follows forwarders ("NTDLL.RtlFoo"). */
static FARPROC export_of(HMODULE module, LPCSTR name, DWORD ordinal, int depth) {
  const BYTE *base = module;
  if (!base || depth > 4) return NULL;
  const ULONG pe = *(const ULONG *)(base + 0x3C);
  const ULONG dir_rva = *(const ULONG *)(base + pe + 0x18 + 0x70), dir_size = *(const ULONG *)(base + pe + 0x18 + 0x74);
  if (!dir_rva) return NULL;
  const ExportDirectory *d = (const ExportDirectory *)(base + dir_rva);
  const ULONG *functions = (const ULONG *)(base + d->AddressOfFunctions), *names = (const ULONG *)(base + d->AddressOfNames);
  const USHORT *ordinals = (const USHORT *)(base + d->AddressOfNameOrdinals);
  ULONG index = (ULONG)-1;
  if (name) {
    for (ULONG i = 0; i < d->NumberOfNames; ++i) {
      const char *candidate = (const char *)(base + names[i]);
      size_t_ k = 0;
      while (name[k] && name[k] == candidate[k]) ++k;
      if (!name[k] && !candidate[k]) { index = ordinals[i]; break; }
    }
  } else if (ordinal >= d->Base && ordinal - d->Base < d->NumberOfFunctions) {
    index = ordinal - d->Base;
  }
  if (index >= d->NumberOfFunctions) return NULL;
  const ULONG rva = functions[index];
  if (rva >= dir_rva && rva < dir_rva + dir_size) {      /* a forwarder: "DLL.Function" or "DLL.#ordinal" */
    const char *forward = (const char *)(base + rva);
    WCHAR library[64];
    size_t_ n = 0;
    while (forward[n] && forward[n] != '.' && n < 60) { library[n] = (WCHAR)(BYTE)forward[n]; ++n; }
    if (forward[n] != '.') return NULL;
    library[n] = 0;
    LDR_DATA_TABLE_ENTRY *target = module_by_name(library);
    if (!target) return NULL;
    const char *function = forward + n + 1;
    FARPROC resolved;
    if (function[0] == '#') {
      DWORD number = 0;
      for (const char *c = function + 1; *c >= '0' && *c <= '9'; ++c) number = number * 10 + (DWORD)(*c - '0');
      resolved = export_of(target->DllBase, NULL, number, depth + 1);
    } else {
      resolved = export_of(target->DllBase, function, 0, depth + 1);
    }
    if (resolved) {      /* the library that forwards depends on the one it forwards to from now on (unless it stays as long as the process does) */
      LDR_DATA_TABLE_ENTRY *self = module_by_base((HMODULE)module);
      if (self && self->LoadCount != 0xFFFF && target->LoadCount != 0xFFFF) NtTcosLinkModules(module, target->DllBase);
    }
    return resolved;
  }
  return (FARPROC)(base + rva);
}
EXPORT FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name) {
  if (!module) module = GetModuleHandleW(NULL);
  if (!name) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  if (!range_allowed(module, 0x40, FALSE) || *(const WORD *)module != 0x5A4D) { set_error(ERROR_MOD_NOT_FOUND); return NULL; }
  FARPROC p = ((ULONG_PTR)name >> 16) == 0 ? export_of(module, NULL, (DWORD)(ULONG_PTR)name, 0) : export_of(module, name, 0, 0);
  if (!p) set_error(ERROR_PROC_NOT_FOUND);
  return p;
}
static BOOL find_file(LPCWSTR name, LPCWSTR extension, LPCWSTR extra, WCHAR *out);
static void unicode_string(UNICODE_STRING *s, LPCWSTR text);
static WCHAR g_dll_directory[PATH_CHARS];      /* SetDllDirectory */

NTSTATUS NTAPI LdrSetDllDirectory(PUNICODE_STRING directory);
NTSTATUS NTAPI LdrGetDllDirectory(PUNICODE_STRING directory);
EXPORT BOOL WINAPI SetDllDirectoryW(LPCWSTR directory) {
  if (directory && wcslen(directory) >= PATH_CHARS) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
  if (directory) lstrcpyW(g_dll_directory, directory); else g_dll_directory[0] = 0;
  UNICODE_STRING text;
  text.Buffer = g_dll_directory;
  text.Length = text.MaximumLength = (USHORT)(wcslen(g_dll_directory) * 2);
  LdrSetDllDirectory(&text);
  return TRUE;
}
EXPORT BOOL WINAPI SetDllDirectoryA(LPCSTR directory) {
  WCHAR *w = directory ? to_wide(directory) : NULL;
  if (directory && !w) return FALSE;
  const BOOL ok = SetDllDirectoryW(w);
  free_wide(w);
  return ok;
}
EXPORT DWORD WINAPI GetDllDirectoryW(DWORD capacity, LPWSTR buffer) {
  const DWORD n = (DWORD)wcslen(g_dll_directory);
  if (n + 1 > capacity) {
    if (capacity != 0 && buffer) buffer[0] = 0;
    return n + 1;
  }
  memcpy(buffer, g_dll_directory, (n + 1) * 2);
  return n;
}
EXPORT DWORD WINAPI GetDllDirectoryA(DWORD capacity, LPSTR buffer) {
  const DWORD n = (DWORD)wcslen(g_dll_directory);
  if (n + 1 > capacity) {
    if (capacity != 0 && buffer) buffer[0] = 0;
    return n + 1;
  }
  return wide_result_to_ansi(g_dll_directory, n, buffer, capacity);
}

EXPORT HANDLE WINAPI CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD size_high, DWORD size_low, LPCWSTR name);
EXPORT LPVOID WINAPI MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T bytes);
EXPORT BOOL WINAPI UnmapViewOfFile(LPCVOID base);
#ifndef SEC_IMAGE
#define SEC_IMAGE 0x1000000
#endif

/* LoadLibrary: a library the process has already (by its name) gets one more use; otherwise the file is looked for and ntdll maps it, with the libraries it imports, and
   calls their entry points. Libraries with thread-local data of their own (__declspec(thread) in a DLL) and the data-file modes are refused. */
static HMODULE load_library_ex(LPCWSTR name, HANDLE file, DWORD flags);
/* The files that exclusive data-file loads keep open (view -> handle), until FreeLibrary. */
static struct { LPVOID view; HANDLE file; } g_exclusive_files[16];
static BOOL exclusive_hold(LPVOID view, HANDLE file) {
  for (int i = 0; i < 16; i++) if (!g_exclusive_files[i].view) { g_exclusive_files[i].view = view; g_exclusive_files[i].file = file; return TRUE; }
  return FALSE;
}
static void exclusive_release(LPCVOID view) {
  for (int i = 0; i < 16; i++) if (g_exclusive_files[i].view == view) { CloseHandle(g_exclusive_files[i].file); g_exclusive_files[i].view = NULL; }
}
static int find_library_restricted(LPCWSTR name, DWORD search, WCHAR *out);
static DWORD g_default_dll_dirs;      /* SetDefaultDllDirectories */
static BOOL has_separator(LPCWSTR name) { for (; *name; name++) if (*name == '\\' || *name == '/') return TRUE; return FALSE; }
EXPORT HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
  const DWORD saved = GetLastError();      /* (a call that works leaves the last error as it was) */
  WCHAR trimmed[PATH_CHARS];
  if (name && name[0] && wcslen(name) < PATH_CHARS) {      /* blanks at the end of a name do not count */
    SIZE_T n = wcslen(name);
    while (n > 1 && name[n - 1] == ' ') n--;
    memcpy(trimmed, name, n * 2);
    trimmed[n] = 0;
    name = trimmed;
  }
  HMODULE m = load_library_ex(name, file, flags);
  if (m) set_error(saved);
  return m;
}
static HMODULE load_library_ex(LPCWSTR name, HANDLE file, DWORD flags) {
  if (file || !name || !name[0]) { set_error(ERROR_INVALID_PARAMETER); return NULL; }      /* (the file handle is reserved) */
  WCHAR path[PATH_CHARS];
  UNICODE_STRING us;
  if (!(flags & 0x62) && !has_separator(name) && actctx_redirect(name, path, PATH_CHARS)) {      /* a library of the active activation context: that file, as a library of its own */
    PVOID handle = NULL;
    ULONG ldr_flags = ((flags & 1) ? 2 : 0) | 0x40000000;
    unicode_string(&us, path);
    const NTSTATUS status = LdrLoadDll(NULL, &ldr_flags, &us, &handle);
    if (!NT_SUCCESS(status)) { set_error(dos_error_of_load(status)); return NULL; }
    return (HMODULE)handle;
  }
  /* The file, if there is one; otherwise the bare name (a library the process has already may be known by an alias, "api-ms-win-crt-runtime-l1-1-0.dll"): the kernel knows
     which libraries are loaded, and decides under the loader lock. */
  static const WCHAR dll[] = {'.', 'd', 'l', 'l', 0};
  path[0] = 0;
  DWORD search = flags & 0x1F00;      /* LOAD_LIBRARY_SEARCH_*: only the folders named are looked in */
  if (search && (flags & 8)) { set_error(ERROR_INVALID_PARAMETER); return NULL; }      /* (LOAD_WITH_ALTERED_SEARCH_PATH excludes them) */
  if (!search && g_default_dll_dirs && !has_separator(name)) search = g_default_dll_dirs;
  if (search) {
    const int found = find_library_restricted(name, search, path);
    if (found < 0) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (found > 0) unicode_string(&us, path);
    else if (GetModuleHandleW(name)) unicode_string(&us, name);      /* (a library the process has already is not searched for) */
    else { set_error(ERROR_MOD_NOT_FOUND); return NULL; }
  } else if (find_file(name, dll, g_dll_directory, path)) {
    unicode_string(&us, path);
  } else {
    path[0] = 0;      /* (find_file may have left a name behind that is not a file) */
    unicode_string(&us, name);
  }
  WCHAR spelled[PATH_CHARS];
  spelled[0] = 0;
  if (path[0] && has_tilde_name(path)) {      /* an 8.3 alias: the library is loaded as the long name, and told by this spelling */
    WCHAR expanded[PATH_CHARS];
    const DWORD n = GetLongPathNameW(path, expanded, PATH_CHARS);
    if (n != 0 && n < PATH_CHARS) {
      memcpy(spelled, path, ((size_t_)wcslen(path) + 1) * 2);
      memcpy(path, expanded, ((size_t_)n + 1) * 2);
      unicode_string(&us, path);
    }
  }
  PVOID already_loaded = NULL;
  if (path[0] && !(flags & 0x62)) {      /* the file of a library that is loaded already (under another name, or moved): one more use of it */
    PVOID loaded = TcosFindModuleByFile(path);
    already_loaded = loaded;
    LDR_DATA_TABLE_ENTRY *entry = loaded ? module_by_base(loaded) : NULL;
    if (entry) us = entry->FullDllName;
  }
  if ((flags & 0x62) && has_separator(name) && !path[0] && !search) { set_error(ERROR_FILE_NOT_FOUND); return NULL; }      /* (a file named with a folder has to be there) */
  if ((flags & 0x62) && GetModuleHandleW(path[0] ? path : name) == (HMODULE)NtCurrentPeb()->ImageBaseAddress) return (HMODULE)NtCurrentPeb()->ImageBaseAddress;      /* (the program is not loaded a second time) */
  if ((flags & 0x62) && !GetModuleHandleW(path[0] ? path : name)) {      /* AS_DATAFILE 2, AS_DATAFILE_EXCLUSIVE 0x40, AS_IMAGE_RESOURCE 0x20: the file is only mapped (a loaded library is used as it is) */
    if (!path[0]) { set_error(has_separator(name) ? ERROR_FILE_NOT_FOUND : ERROR_MOD_NOT_FOUND); return NULL; }
    const BOOL exclusive = (flags & 0x40) != 0;      /* AS_DATAFILE_EXCLUSIVE: nobody else may write the file while it is loaded */
    HANDLE f = CreateFileW(path, GENERIC_READ, exclusive ? FILE_SHARE_READ | FILE_SHARE_DELETE : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY | ((flags & 0x20) ? SEC_IMAGE : 0), 0, 0, NULL);
    if (!m) { CloseHandle(f); return NULL; }
    LPVOID view = MapViewOfFile(m, 4 /* FILE_MAP_READ */, 0, 0, 0);
    CloseHandle(m);
    if (!view) { CloseHandle(f); return NULL; }
    if (exclusive && !exclusive_hold(view, f)) CloseHandle(f);
    else if (!exclusive) CloseHandle(f);
    return (HMODULE)((ULONG_PTR)view | ((flags & 0x20) ? 2 : 1));      /* (the low bits tell FreeLibrary and the resource functions what it is) */
  }
  if (path[0] && !GetModuleHandleW(path)) {      /* a file somebody holds open without sharing cannot be read by the loader */
    HANDLE probe = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (probe == INVALID_HANDLE_VALUE) { if (GetLastError() == ERROR_SHARING_VIOLATION) return NULL; }
    else CloseHandle(probe);
  }
  PVOID handle = NULL;
  ULONG ldr_flags = (flags & 1) ? 2 : 0;      /* DONT_RESOLVE_DLL_REFERENCES = LDR_DONT_RESOLVE_REFS */
  const NTSTATUS status = LdrLoadDll(NULL, &ldr_flags, &us, &handle);
  if (!NT_SUCCESS(status)) { set_error(dos_error_of_load(status)); return NULL; }
  if (!already_loaded && path[0]) module_spelling_set(handle, spelled[0] ? spelled : NULL);
  return (HMODULE)handle;
}
EXPORT HMODULE WINAPI LoadLibraryW(LPCWSTR name) { return LoadLibraryExW(name, NULL, 0); }
EXPORT HMODULE WINAPI LoadLibraryA(LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (name && !w) return NULL;
  HMODULE m = LoadLibraryExW(w, NULL, 0);
  free_wide(w);
  return m;
}
EXPORT HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (name && !w) return NULL;
  HMODULE m = LoadLibraryExW(w, file, flags);
  free_wide(w);
  return m;
}
EXPORT BOOL WINAPI FreeLibrary(HMODULE module) {
  if (((ULONG_PTR)module & 3) == 1 || ((ULONG_PTR)module & 3) == 2) {
    MEMORY_BASIC_INFORMATION info;
    const LPCVOID view = (LPCVOID)((ULONG_PTR)module & ~(ULONG_PTR)3);
    if (!VirtualQuery(view, &info, sizeof(info)) || info.AllocationBase != view) { set_error(ERROR_BAD_EXE_FORMAT); return FALSE; }      /* (not the start of a view) */
    exclusive_release(view);
    return UnmapViewOfFile(view);
  }      /* a data file or an image resource view */
  if (!module_by_base(module)) { set_error(ERROR_MOD_NOT_FOUND); return FALSE; }
  const NTSTATUS status = LdrUnloadDll(module);
  return NT_SUCCESS(status) ? TRUE : (set_error(dos_error_of_load(status)), FALSE);
}
/* There are no packages here. */
EXPORT HMODULE WINAPI LoadPackagedLibrary(LPCWSTR name, DWORD reserved) {
  (void)name; (void)reserved;
  set_error(15700);      /* APPMODEL_ERROR_NO_PACKAGE */
  return NULL;
}
EXPORT void WINAPI FreeLibraryAndExitThread(HMODULE module, DWORD code) { FreeLibrary(module); ExitThread(code); }

/* ---- thread-local storage, fiber-local storage --------------------------------------------------------------------------------------------------------- */

static BYTE g_tls_used[(64 + 1024) / 8];
static int tls_test(DWORD i) { return (g_tls_used[i / 8] >> (i % 8)) & 1; }
static void tls_mark(DWORD i, int on) { if (on) g_tls_used[i / 8] |= (BYTE)(1u << (i % 8)); else g_tls_used[i / 8] &= (BYTE)~(1u << (i % 8)); }

EXPORT DWORD WINAPI TlsAlloc(void) {
  for (DWORD i = 0; i < 64 + 1024; ++i) {
    if (tls_test(i)) continue;
    tls_mark(i, 1);
    if (i < 64) {
      TEB_TLS_SLOTS(NtCurrentTeb())[i] = NULL;
    } else {
      PVOID **expansion = &TEB_TLS_EXPANSION(NtCurrentTeb());
      if (!*expansion) {
        *expansion = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, 1024 * sizeof(PVOID));
        if (!*expansion) { tls_mark(i, 0); set_error(ERROR_NOT_ENOUGH_MEMORY); return TLS_OUT_OF_INDEXES; }
      }
      (*expansion)[i - 64] = NULL;
    }
    set_error(ERROR_SUCCESS);
    return i;
  }
  set_error(ERROR_NO_MORE_ITEMS);
  return TLS_OUT_OF_INDEXES;
}
EXPORT BOOL WINAPI TlsFree(DWORD index) {
  if (index >= 64 + 1024 || !tls_test(index)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  tls_mark(index, 0);
  return TRUE;
}
EXPORT LPVOID WINAPI TlsGetValue(DWORD index) {
  if (index >= 64 + 1024 || !tls_test(index)) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  set_error(ERROR_SUCCESS);
  if (index < 64) return TEB_TLS_SLOTS(NtCurrentTeb())[index];
  PVOID *expansion = TEB_TLS_EXPANSION(NtCurrentTeb());
  return expansion ? expansion[index - 64] : NULL;
}
EXPORT BOOL WINAPI TlsSetValue(DWORD index, LPVOID value) {
  if (index >= 64 + 1024 || !tls_test(index)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (index < 64) { TEB_TLS_SLOTS(NtCurrentTeb())[index] = value; return TRUE; }
  PVOID *expansion = TEB_TLS_EXPANSION(NtCurrentTeb());
  if (!expansion) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  expansion[index - 64] = value;
  return TRUE;
}

/* Fiber local storage lives in ntdll (per thread, with callbacks); a failure there is an invalid parameter or no room. */
IMPORT NTSTATUS NTAPI RtlFlsAlloc(PFLS_CALLBACK_FUNCTION callback, PULONG index);
IMPORT NTSTATUS NTAPI RtlFlsFree(ULONG index);
IMPORT NTSTATUS NTAPI RtlFlsGetValue(ULONG index, PVOID *value);
IMPORT NTSTATUS NTAPI RtlFlsSetValue(ULONG index, PVOID value);
EXPORT DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION callback) {
  ULONG index = 0;
  const NTSTATUS status = RtlFlsAlloc(callback, &index);
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0xFFFFFFFFu; }
  return index;
}
EXPORT BOOL WINAPI FlsFree(DWORD index) {
  const NTSTATUS status = RtlFlsFree(index);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT PVOID WINAPI FlsGetValue(DWORD index) {
  PVOID value = NULL;
  const NTSTATUS status = RtlFlsGetValue(index, &value);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  set_error(ERROR_SUCCESS);
  return value;
}
EXPORT BOOL WINAPI FlsSetValue(DWORD index, PVOID value) {
  const NTSTATUS status = RtlFlsSetValue(index, value);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* ---- synchronization: one thread, so these count and never wait ------------------------------------------------------------------------------------------ */

EXPORT void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs) { RtlInitializeCriticalSection(cs); }
IMPORT NTSTATUS NTAPI RtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION, ULONG, ULONG);
EXPORT BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags) { const NTSTATUS s = RtlInitializeCriticalSectionEx(cs, spin, flags); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }
EXPORT BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin) { return InitializeCriticalSectionEx(cs, spin, 0); }
EXPORT void WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs) { RtlEnterCriticalSection(cs); }
EXPORT BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION cs) { return RtlTryEnterCriticalSection(cs); }
EXPORT void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs) { RtlLeaveCriticalSection(cs); }
EXPORT void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs) { RtlDeleteCriticalSection(cs); }
EXPORT DWORD WINAPI SetCriticalSectionSpinCount(LPCRITICAL_SECTION cs, DWORD spin) { (void)cs; (void)spin; return 0; }

EXPORT LONG WINAPI InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
EXPORT LONG WINAPI InterlockedDecrement(volatile LONG *p) { return __sync_sub_and_fetch(p, 1); }
EXPORT LONG WINAPI InterlockedExchange(volatile LONG *p, LONG v) { return __sync_lock_test_and_set(p, v); }
EXPORT LONG WINAPI InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __sync_fetch_and_add(p, v); }
EXPORT LONG WINAPI InterlockedCompareExchange(volatile LONG *p, LONG exchange, LONG compare) { return __sync_val_compare_and_swap(p, compare, exchange); }
EXPORT LONG64 WINAPI InterlockedCompareExchange64(volatile LONG64 *p, LONG64 exchange, LONG64 compare) { return __sync_val_compare_and_swap(p, compare, exchange); }

/* One-time initialization. INIT_ONCE.Ptr is 0 (not started), 1 (a thread is initializing; others wait), 3 (asynchronous initialization under way: several threads may
 * try; the first to finish wins) or <context> | 2 (done; the context's two low bits are free for this). */
#define INIT_ONCE_CHECK_ONLY_ 1
#define INIT_ONCE_ASYNC_ 2
#define INIT_ONCE_INIT_FAILED_ 4
EXPORT void WINAPI InitOnceInitialize(PINIT_ONCE once) { once->Ptr = NULL; }
EXPORT BOOL WINAPI InitOnceBeginInitialize(PINIT_ONCE once, DWORD flags, PBOOL pending, PVOID *context) {
  volatile ULONG_PTR *state = (volatile ULONG_PTR *)&once->Ptr;
  if ((flags & ~(DWORD)(INIT_ONCE_CHECK_ONLY_ | INIT_ONCE_ASYNC_)) || ((flags & INIT_ONCE_CHECK_ONLY_) && (flags & INIT_ONCE_ASYNC_))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  for (;;) {
    const ULONG_PTR v = *state;
    if ((v & 3) == 2) { *pending = FALSE; if (context) *context = (PVOID)(v & ~(ULONG_PTR)3); return TRUE; }
    if (flags & INIT_ONCE_CHECK_ONLY_) { set_error(31 /* 31 */); return FALSE; }
    if (flags & INIT_ONCE_ASYNC_) {
      if (v == 1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }      /* (another thread is initializing synchronously) */
      if (v == 0 && !__sync_bool_compare_and_swap(state, 0, 3)) continue;
      *pending = TRUE;
      return TRUE;
    }
    if (v == 3) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (v == 0) { if (!__sync_bool_compare_and_swap(state, 0, 1)) continue; *pending = TRUE; return TRUE; }
    NtYieldExecution();      /* v == 1: somebody else is at it: wait for them */
  }
}
EXPORT BOOL WINAPI InitOnceComplete(PINIT_ONCE once, DWORD flags, PVOID context) {
  volatile ULONG_PTR *state = (volatile ULONG_PTR *)&once->Ptr;
  const BOOL failed = (flags & INIT_ONCE_INIT_FAILED_) != 0, async = (flags & INIT_ONCE_ASYNC_) != 0;
  if ((flags & ~(DWORD)(INIT_ONCE_ASYNC_ | INIT_ONCE_INIT_FAILED_)) || (failed && async) || (failed ? context != NULL : ((ULONG_PTR)context & 3) != 0)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const ULONG_PTR v = *state;
  if ((v == 3) != async && v != 0 && (v & 3) != 2) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }      /* (the kind of the completion is the kind of the begin) */
  if (v == 0 || (v & 3) == 2) { if (async && v == 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } set_error(31); return FALSE; }
  const ULONG_PTR next = failed ? 0 : ((ULONG_PTR)context | 2);
  if (!__sync_bool_compare_and_swap(state, v, next)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN function, PVOID parameter, PVOID *context) {
  BOOL pending = FALSE;
  PVOID result = NULL;
  if (!InitOnceBeginInitialize(once, 0, &pending, &result)) return FALSE;
  if (!pending) { if (context) *context = result; return TRUE; }
  result = context ? *context : NULL;      /* (the callback is given what the caller's variable holds) */
  if (!function(once, parameter, &result)) { InitOnceComplete(once, INIT_ONCE_INIT_FAILED_, NULL); return FALSE; }      /* (the error is the callback's) */
  if (!InitOnceComplete(once, 0, result)) { InitOnceComplete(once, INIT_ONCE_INIT_FAILED_, NULL); return FALSE; }
  if (context) *context = result;
  return TRUE;
}

EXPORT void WINAPI Sleep(DWORD ms);
EXPORT ULONGLONG WINAPI GetTickCount64(void);

/* Slim reader/writer locks and condition variables for the threads of the process: waiting threads give their time slice away (condition variables wake all waiters,
 * which the contract allows: a waiter rechecks its condition). */
/* The lock word: bit 0 is set while a thread holds the lock exclusively, bits 4 and up count the shared holders (16 each), bits 32 and up count the threads waiting for the
 * exclusive lock; shared requests give way to those. */
#define SRW_EXCLUSIVE 1ull
#define SRW_SHARED_UNIT 16ull
#define SRW_SHARED_MASK 0xFFFFFFF0ull
#define SRW_WAITER_UNIT (1ull << 32)
#define SRW_WORD(lock) ((volatile ULONGLONG *)&(lock)->Ptr)
EXPORT void WINAPI InitializeSRWLock(PSRWLOCK lock) { lock->Ptr = NULL; }
EXPORT BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK lock) {
  for (;;) {
    const ULONGLONG v = *SRW_WORD(lock);
    if (v & (SRW_EXCLUSIVE | SRW_SHARED_MASK)) return FALSE;
    if (__sync_bool_compare_and_swap(SRW_WORD(lock), v, v | SRW_EXCLUSIVE)) return TRUE;
  }
}
EXPORT void WINAPI AcquireSRWLockExclusive(PSRWLOCK lock) {
  __sync_fetch_and_add(SRW_WORD(lock), SRW_WAITER_UNIT);
  for (;;) {
    const ULONGLONG v = *SRW_WORD(lock);
    if (!(v & (SRW_EXCLUSIVE | SRW_SHARED_MASK)) && __sync_bool_compare_and_swap(SRW_WORD(lock), v, (v | SRW_EXCLUSIVE) - SRW_WAITER_UNIT)) return;
    NtYieldExecution();
  }
}
EXPORT void WINAPI ReleaseSRWLockExclusive(PSRWLOCK lock) { __sync_fetch_and_and(SRW_WORD(lock), ~SRW_EXCLUSIVE); }
EXPORT BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK lock) {
  for (;;) {
    const ULONGLONG v = *SRW_WORD(lock);
    if ((v & SRW_EXCLUSIVE) || (v >> 32) != 0) return FALSE;      /* (held, or a thread is waiting for it exclusively) */
    if (__sync_bool_compare_and_swap(SRW_WORD(lock), v, v + SRW_SHARED_UNIT)) return TRUE;
  }
}
EXPORT void WINAPI AcquireSRWLockShared(PSRWLOCK lock) { while (!TryAcquireSRWLockShared(lock)) NtYieldExecution(); }
EXPORT void WINAPI ReleaseSRWLockShared(PSRWLOCK lock) { __sync_fetch_and_sub(SRW_WORD(lock), SRW_SHARED_UNIT); }
/* Synchronization barriers. The 32 bytes of the SYNCHRONIZATION_BARRIER hold: the number of threads that wait without NO_DELETE (DeleteSynchronizationBarrier waits for them to
 * be gone), the number of threads the barrier is for, and one 64-bit word with the generation (high half) and how many have arrived in it (low half). The thread that
 * arrives last starts the next generation and gets TRUE. */
EXPORT void WINAPI RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments);
typedef struct SyncBarrier { volatile LONG delete_waiters; LONG total; volatile ULONGLONG state; LONG spin; LONG unused[3]; } SyncBarrier;
EXPORT BOOL WINAPI InitializeSynchronizationBarrier(SyncBarrier *barrier, LONG total, LONG spin) {
  if (!barrier || total < 1 || spin < -1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  memset(barrier, 0, sizeof(*barrier));
  barrier->total = total;
  barrier->spin = spin;
  return TRUE;
}
EXPORT BOOL WINAPI EnterSynchronizationBarrier(SyncBarrier *barrier, DWORD flags) {
  if (flags & ~7u) RaiseException(0xC000000Du /* STATUS_INVALID_PARAMETER */, 1 /* EXCEPTION_NONCONTINUABLE */, 0, NULL);
  const BOOL counted = !(flags & 4 /* SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE */);
  if (counted) __sync_fetch_and_add(&barrier->delete_waiters, 1);
  ULONGLONG generation;
  for (;;) {
    const ULONGLONG v = barrier->state;
    generation = v >> 32;
    if ((LONG)(v & 0xFFFFFFFFu) + 1 >= barrier->total) {      /* the last to arrive */
      if (__sync_bool_compare_and_swap(&barrier->state, v, (generation + 1) << 32)) {
        if (counted) __sync_fetch_and_sub(&barrier->delete_waiters, 1);
        return TRUE;
      }
    } else if (__sync_bool_compare_and_swap(&barrier->state, v, v + 1)) break;
  }
  while ((barrier->state >> 32) == generation) NtYieldExecution();
  if (counted) __sync_fetch_and_sub(&barrier->delete_waiters, 1);
  return FALSE;
}
EXPORT BOOL WINAPI DeleteSynchronizationBarrier(SyncBarrier *barrier) {
  while (barrier->delete_waiters > 0) NtYieldExecution();
  return TRUE;
}
EXPORT void WINAPI InitializeConditionVariable(PCONDITION_VARIABLE cv) { cv->Ptr = NULL; }
EXPORT void WINAPI WakeConditionVariable(PCONDITION_VARIABLE cv) { __sync_fetch_and_add((ULONG_PTR *)&cv->Ptr, 1); }
EXPORT void WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE cv) { __sync_fetch_and_add((ULONG_PTR *)&cv->Ptr, 1); }
EXPORT void WINAPI Sleep(DWORD ms);
EXPORT ULONGLONG WINAPI GetTickCount64(void);
static BOOL wait_condition(PCONDITION_VARIABLE cv, DWORD timeout_ms, void (*release)(PVOID), void (*acquire)(PVOID), PVOID lock) {
  const ULONG_PTR seen = (ULONG_PTR)cv->Ptr;
  const ULONGLONG start = GetTickCount64();
  release(lock);
  BOOL woken = TRUE;
  while ((ULONG_PTR)cv->Ptr == seen) {
    if (timeout_ms != INFINITE && GetTickCount64() - start >= timeout_ms) { woken = FALSE; break; }
    Sleep(0);
  }
  acquire(lock);
  if (!woken) set_error(ERROR_TIMEOUT);
  return woken;
}
static void cs_release(PVOID cs) { RtlLeaveCriticalSection(cs); }
static void cs_acquire(PVOID cs) { RtlEnterCriticalSection(cs); }
EXPORT BOOL WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE cv, LPCRITICAL_SECTION cs, DWORD timeout_ms) { return wait_condition(cv, timeout_ms, cs_release, cs_acquire, cs); }
/* SleepConditionVariableSRW: exclusive (flags 0) or shared (flags 1) */
static BOOL g_srw_shared;
static void srw_release(PVOID lock) { if (g_srw_shared) ReleaseSRWLockShared(lock); else ReleaseSRWLockExclusive(lock); }
static void srw_acquire(PVOID lock) { if (g_srw_shared) AcquireSRWLockShared(lock); else AcquireSRWLockExclusive(lock); }
EXPORT BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD timeout_ms, ULONG flags) {
  g_srw_shared = (flags & 1) != 0;
  return wait_condition(cv, timeout_ms, srw_release, srw_acquire, lock);
}

/* Events, mutexes and semaphores are objects of the system (src/Process/Objects.hpp): a name makes two processes share one. The wait functions below wait for them and
 * for threads. Names live in "\\BaseNamedObjects\\" (Local\\ and Global\\ are the same here). */
EXPORT void WINAPI Sleep(DWORD ms);
EXPORT ULONGLONG WINAPI GetTickCount64(void);

/* Builds the OBJECT_ATTRIBUTES of a name (or none). `buffer` holds the wide name: 3 + 17 + name characters. */
typedef struct { OBJECT_ATTRIBUTES attributes; UNICODE_STRING string; WCHAR text[300]; } NamedAttributes;
static POBJECT_ATTRIBUTES named_attributes(NamedAttributes *n, LPCWSTR name, BOOL inherit) {
  if ((!name || !*name) && !inherit) return NULL;
  static const WCHAR prefix[] = L"\\BaseNamedObjects\\";
  unsigned length = 0;
  for (; prefix[length]; ++length) n->text[length] = prefix[length];
  const WCHAR *skip = name;
  if (!name || !*name) skip = NULL;      /* (only the inherit flag was asked for) */
  else if ((name[0] == 'L' || name[0] == 'l') && (name[1] == 'o' || name[1] == 'O') && (name[2] == 'c' || name[2] == 'C') && (name[3] == 'a' || name[3] == 'A') && (name[4] == 'l' || name[4] == 'L') && name[5] == '\\') skip = name + 6;
  else if ((name[0] == 'G' || name[0] == 'g') && (name[1] == 'l' || name[1] == 'L') && (name[2] == 'o' || name[2] == 'O') && (name[3] == 'b' || name[3] == 'B') && (name[4] == 'a' || name[4] == 'A') && (name[5] == 'l' || name[5] == 'L') && name[6] == '\\') skip = name + 7;
  if (!skip) length = 0;
  else for (; *skip && length < 299; ++skip) n->text[length++] = *skip;
  n->text[length] = 0;
  n->string.Buffer = n->text;
  n->string.Length = (USHORT)(length * 2);
  n->string.MaximumLength = (USHORT)(length * 2 + 2);
  n->attributes.Length = sizeof(OBJECT_ATTRIBUTES);
  n->attributes.RootDirectory = NULL;
  n->attributes.ObjectName = length ? &n->string : NULL;
  n->attributes.Attributes = 0x40 | (inherit ? 2 : 0);      /* OBJ_CASE_INSENSITIVE, OBJ_INHERIT */
  n->attributes.SecurityDescriptor = NULL;
  n->attributes.SecurityQualityOfService = NULL;
  return &n->attributes;
}
/* The result of a create call: the handle, with ERROR_ALREADY_EXISTS set if the name was taken. */
static HANDLE created(NTSTATUS status, HANDLE h) {
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  set_error(status == STATUS_OBJECT_NAME_EXISTS ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  return h;
}
static HANDLE opened(NTSTATUS status, HANDLE h) {
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return h;
}

EXPORT HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name) {
  NamedAttributes n;
  const BOOL inherit_handle = sa && sa->bInheritHandle;
  HANDLE h = NULL;
  { const NTSTATUS status = NtCreateEvent(&h, 0x1F0003, named_attributes(&n, name, inherit_handle), manual ? 0 : 1, initial != 0); return created(status, h); }
}
EXPORT HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  HANDLE h = CreateEventW(sa, manual, initial, w);
  free_wide(w);
  return h;
}
EXPORT HANDLE WINAPI CreateEventExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access) { (void)access; return CreateEventW(sa, (flags & 1) ? TRUE : FALSE, (flags & 2) ? TRUE : FALSE, name); }
EXPORT HANDLE WINAPI CreateEventExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access) { (void)access; return CreateEventA(sa, (flags & 1) ? TRUE : FALSE, (flags & 2) ? TRUE : FALSE, name); }
EXPORT HANDLE WINAPI OpenEventW(DWORD access, BOOL inherit, LPCWSTR name) {
  NamedAttributes n;
  const BOOL inherit_handle = inherit;
  HANDLE h = NULL;
  { const NTSTATUS status = NtOpenEvent(&h, access, named_attributes(&n, name, inherit_handle)); return opened(status, h); }
}
EXPORT HANDLE WINAPI OpenEventA(DWORD access, BOOL inherit, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  HANDLE h = OpenEventW(access, inherit, w);
  free_wide(w);
  return h;
}
EXPORT BOOL WINAPI SetEvent(HANDLE h) { const NTSTATUS s = NtSetEvent(h, NULL); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }
EXPORT BOOL WINAPI ResetEvent(HANDLE h) { const NTSTATUS s = NtResetEvent(h, NULL); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }
EXPORT BOOL WINAPI PulseEvent(HANDLE h) { const NTSTATUS s = NtPulseEvent(h, NULL); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }

EXPORT HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCWSTR name) {
  NamedAttributes n;
  const BOOL inherit_handle = sa && sa->bInheritHandle;
  HANDLE h = NULL;
  { const NTSTATUS status = NtCreateMutant(&h, 0x1F0001, named_attributes(&n, name, inherit_handle), owned != 0); return created(status, h); }
}
EXPORT HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  HANDLE h = CreateMutexW(sa, owned, w);
  free_wide(w);
  return h;
}
EXPORT HANDLE WINAPI CreateMutexExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access) { (void)access; return CreateMutexW(sa, (flags & 1) ? TRUE : FALSE, name); }
EXPORT HANDLE WINAPI CreateMutexExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access) { (void)access; return CreateMutexA(sa, (flags & 1) ? TRUE : FALSE, name); }
EXPORT HANDLE WINAPI OpenMutexW(DWORD access, BOOL inherit, LPCWSTR name) {
  NamedAttributes n;
  const BOOL inherit_handle = inherit;
  HANDLE h = NULL;
  { const NTSTATUS status = NtOpenMutant(&h, access, named_attributes(&n, name, inherit_handle)); return opened(status, h); }
}
EXPORT HANDLE WINAPI OpenMutexA(DWORD access, BOOL inherit, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  HANDLE h = OpenMutexW(access, inherit, w);
  free_wide(w);
  return h;
}
EXPORT BOOL WINAPI ReleaseMutex(HANDLE h) { const NTSTATUS s = NtReleaseMutant(h, NULL); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }

EXPORT HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name) {
  NamedAttributes n;
  const BOOL inherit_handle = sa && sa->bInheritHandle;
  HANDLE h = NULL;
  { const NTSTATUS status = NtCreateSemaphore(&h, 0x1F0003, named_attributes(&n, name, inherit_handle), initial, maximum); return created(status, h); }
}
EXPORT HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  HANDLE h = CreateSemaphoreW(sa, initial, maximum, w);
  free_wide(w);
  return h;
}
EXPORT HANDLE WINAPI CreateSemaphoreExW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name, DWORD flags, DWORD access) { (void)flags; (void)access; return CreateSemaphoreW(sa, initial, maximum, name); }
EXPORT HANDLE WINAPI OpenSemaphoreW(DWORD access, BOOL inherit, LPCWSTR name) {
  NamedAttributes n;
  const BOOL inherit_handle = inherit;
  HANDLE h = NULL;
  { const NTSTATUS status = NtOpenSemaphore(&h, access, named_attributes(&n, name, inherit_handle)); return opened(status, h); }
}
EXPORT HANDLE WINAPI OpenSemaphoreA(DWORD access, BOOL inherit, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  HANDLE h = OpenSemaphoreW(access, inherit, w);
  free_wide(w);
  return h;
}
EXPORT BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG count, PLONG previous) { const NTSTATUS s = NtReleaseSemaphore(h, count, previous); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }

EXPORT HANDLE WINAPI GetStdHandle(DWORD which);
EXPORT DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD timeout_ms, BOOL alertable);
EXPORT DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD timeout_ms) { return WaitForSingleObjectEx(h, timeout_ms, FALSE); }

EXPORT void WINAPI Sleep(DWORD ms) {
  if (ms == INFINITE) { for (;;) { LARGE_INTEGER t; t.QuadPart = -(LONGLONG)10000000 * 3600; NtDelayExecution(FALSE, &t); } }
  LARGE_INTEGER t;
  t.QuadPart = -(LONGLONG)ms * 10000;
  if (ms == 0) NtYieldExecution(); else NtDelayExecution(FALSE, &t);
}
EXPORT DWORD WINAPI SleepEx(DWORD ms, BOOL alertable) {
  if (!alertable) { Sleep(ms); return 0; }
  LARGE_INTEGER t;
  t.QuadPart = ms == INFINITE ? (LONGLONG)0x8000000000000000ull : -(LONGLONG)ms * 10000;
  const NTSTATUS status = NtDelayExecution(TRUE, &t);
  return status == STATUS_USER_APC || status == STATUS_ALERTED ? WAIT_IO_COMPLETION : 0;      /* an APC ended the sleep */
}
EXPORT DWORD WINAPI SignalObjectAndWait(HANDLE signal, HANDLE wait, DWORD timeout_ms, BOOL alertable) {
  LARGE_INTEGER t;
  t.QuadPart = -(LONGLONG)timeout_ms * 10000;
  return wait_result(NtSignalAndWaitForSingleObject(signal, wait, alertable != 0, timeout_ms == INFINITE ? NULL : &t));
}

/* ---- asynchronous procedure calls --------------------------------------------------------------------------------------------------------------------- */

/* The routine that runs in the target thread: a PAPCFUNC takes one argument. */
static void NTAPI apc_function(ULONG_PTR function, ULONG_PTR parameter, ULONG_PTR unused) { (void)unused; ((void (WINAPI *)(ULONG_PTR))function)(parameter); }
EXPORT DWORD WINAPI QueueUserAPC(void (WINAPI *function)(ULONG_PTR), HANDLE thread, ULONG_PTR parameter) {
  if (!function) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  const NTSTATUS status = NtQueueApcThread(thread, (PVOID)apc_function, (ULONG_PTR)function, parameter, 0);
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0; }
  return 1;
}
/* QueueUserAPC2: flags 0 is QueueUserAPC; 1 (special user APC) runs in the thread at once - here, in the calling thread, when that is the target, and otherwise as an ordinary one. */
EXPORT DWORD WINAPI GetThreadId(HANDLE thread);
EXPORT DWORD WINAPI GetCurrentThreadId(void);
EXPORT BOOL WINAPI QueueUserAPC2(void (WINAPI *function)(ULONG_PTR), HANDLE thread, ULONG_PTR parameter, DWORD flags) {
  if (flags > 1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!QueueUserAPC(function, thread, parameter)) return FALSE;
  if ((flags & 1) && (thread == (HANDLE)(LONG_PTR)-2 || GetThreadId(thread) == GetCurrentThreadId())) NtTestAlert();
  return TRUE;
}
static BOOL pipe_start_overlapped(int op, HANDLE h, LPVOID buffer, DWORD length, LPOVERLAPPED overlapped, void (WINAPI *routine)(DWORD, DWORD, LPOVERLAPPED));
static int pipe_begin(int op, HANDLE h, LPVOID buffer, DWORD length, LPOVERLAPPED overlapped, void (WINAPI *routine)(DWORD, DWORD, LPOVERLAPPED), ULONG_PTR *information, NTSTATUS *result, LPVOID in, DWORD in_size);
/* ReadFileEx / WriteFileEx: the operation is done at once; the completion routine is an APC for this thread, called when it next waits alertably. */
typedef void (WINAPI *LPOVERLAPPED_COMPLETION_ROUTINE_)(DWORD, DWORD, LPOVERLAPPED);
static void NTAPI completion_apc(ULONG_PTR routine, ULONG_PTR overlapped, ULONG_PTR unused) {
  (void)unused;
  LPOVERLAPPED o = (LPOVERLAPPED)overlapped;
  ((LPOVERLAPPED_COMPLETION_ROUTINE_)routine)(NT_SUCCESS((NTSTATUS)(ULONG)o->Internal) ? 0 : RtlNtStatusToDosError((NTSTATUS)(ULONG)o->Internal), (DWORD)o->InternalHigh, o);
}
EXPORT BOOL WINAPI ReadFileEx(HANDLE h, LPVOID buffer, DWORD length, LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE_ routine) {
  if (!overlapped || !routine) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (pipe_start_overlapped(0, h, buffer, length, overlapped, routine)) return TRUE;
  DWORD got = 0;
  const BOOL ok = ReadFile(h, buffer, length, &got, overlapped);
  if (!ok && GetLastError() != ERROR_HANDLE_EOF) return FALSE;
  NtQueueApcThread(NtCurrentThread(), (PVOID)completion_apc, (ULONG_PTR)routine, (ULONG_PTR)overlapped, 0);
  return TRUE;
}
EXPORT BOOL WINAPI WriteFileEx(HANDLE h, LPCVOID buffer, DWORD length, LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE_ routine) {
  if (!overlapped || !routine) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  {
    ULONG_PTR information = 0;
    NTSTATUS result = 0;
    const int started = pipe_begin(1, h, (LPVOID)buffer, length, overlapped, routine, &information, &result, NULL, 0);      /* 1: waiting, 2: it went at once, 3: it failed */
    if (started == 1) return TRUE;
    if (started == 3) { overlapped->Internal = (ULONG_PTR)STATUS_PENDING; return fail_nt(result); }
    if (started == 2) {
      complete_overlapped(overlapped, result, information);
      port_note(h, overlapped, information);
      NtQueueApcThread(NtCurrentThread(), (PVOID)completion_apc, (ULONG_PTR)routine, (ULONG_PTR)overlapped, 0);
      return TRUE;
    }
  }
  DWORD done = 0;
  if (!WriteFile(h, buffer, length, &done, overlapped)) return FALSE;
  NtQueueApcThread(NtCurrentThread(), (PVOID)completion_apc, (ULONG_PTR)routine, (ULONG_PTR)overlapped, 0);
  return TRUE;
}

/* ReadDirectoryChangesW: done at once (there is no real asynchronous I/O here either) over NtNotifyChangeDirectoryFile; an overlapped call's event is set and its
 * completion routine, if any, queued as an ordinary APC exactly like ReadFileEx/WriteFileEx above. */
EXPORT BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE source, HANDLE tp, HANDLE *target, DWORD access, BOOL inherit, DWORD options);
EXPORT HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD id);
typedef struct { HANDLE directory, caller; LPVOID buffer; DWORD length; BOOLEAN subtree; DWORD filter; LPOVERLAPPED overlapped; LPOVERLAPPED_COMPLETION_ROUTINE_ routine; } DirectoryRequest;
/* An overlapped request waits for a change in a thread of its own (the kernel's calls are synchronous); when one comes the buffer, the overlapped structure and the event are
   completed, and a completion routine runs as an APC of the thread that asked. */
static DWORD WINAPI directory_request_thread(LPVOID parameter) {
  DirectoryRequest request = *(DirectoryRequest *)parameter;
  RtlFreeHeap(heap(), 0, parameter);
  IO_STATUS_BLOCK iosb;
  iosb.Information = 0;
  NTSTATUS status;
  do {      /* (waits a tenth of a second at a time: when the handle is closed the call fails and the thread ends) */
    iosb.Information = 0;
    status = NtNotifyChangeDirectoryFile(request.directory, (HANDLE)1, NULL, NULL, &iosb, request.buffer, request.length, request.filter, request.subtree);
  } while (status == (NTSTATUS)0x102);
  complete_overlapped(request.overlapped, status, iosb.Information);
  if (request.routine) NtQueueApcThread(request.caller, (PVOID)completion_apc, (ULONG_PTR)request.routine, (ULONG_PTR)request.overlapped, 0);
  else NtClose(request.caller);
  NtNotifyChangeDirectoryFile(request.directory, (HANDLE)3, NULL, NULL, &iosb, NULL, 0, request.filter, request.subtree);      /* (whoever changed the directory waits for this) */
  return 0;
}
EXPORT BOOL WINAPI ReadDirectoryChangesW(HANDLE h, LPVOID buffer, DWORD length, BOOL watch_subtree, DWORD filter, LPDWORD bytes_returned, LPOVERLAPPED overlapped,
                                         LPOVERLAPPED_COMPLETION_ROUTINE_ routine) {
  IO_STATUS_BLOCK iosb;
  if (filter == 0 || (filter & ~0x1FFu) || (!overlapped && (!buffer || length == 0))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (overlapped) {
    { const NTSTATUS registered = NtNotifyChangeDirectoryFile(h, (HANDLE)2, NULL, NULL, &iosb, buffer, length, filter, (BOOLEAN)watch_subtree); if (!NT_SUCCESS(registered)) return fail_nt(registered); }      /* (this only makes the watch, so that changes from now on are kept) */
    overlapped->Internal = (ULONG_PTR)STATUS_PENDING;
    if (overlapped->hEvent) NtResetEvent((HANDLE)((ULONG_PTR)overlapped->hEvent & ~(ULONG_PTR)1), NULL);
    iosb.Information = 0;
    {      /* what changed since the last request completes this one at once */
      const NTSTATUS polled = NtNotifyChangeDirectoryFile(h, (HANDLE)5, NULL, NULL, &iosb, buffer, length, filter, (BOOLEAN)watch_subtree);
      if (polled != (NTSTATUS)0x103) {
        if (!NT_SUCCESS(polled)) return fail_nt(polled);
        complete_overlapped(overlapped, polled, iosb.Information);
        if (routine) NtQueueApcThread(NtCurrentThread(), (PVOID)completion_apc, (ULONG_PTR)routine, (ULONG_PTR)overlapped, 0);
        if (bytes_returned) *bytes_returned = 0;
        return TRUE;
      }
    }
    DirectoryRequest *request = RtlAllocateHeap(heap(), 0, sizeof(*request));
    if (!request) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    HANDLE caller = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &caller, 0, FALSE, 2 /* DUPLICATE_SAME_ACCESS */)) { RtlFreeHeap(heap(), 0, request); return FALSE; }
    request->directory = h; request->caller = caller; request->buffer = buffer; request->length = length; request->subtree = (BOOLEAN)watch_subtree;
    request->filter = filter; request->overlapped = overlapped; request->routine = routine;
    NtNotifyChangeDirectoryFile(h, (HANDLE)6, NULL, NULL, &iosb, NULL, 0, filter, (BOOLEAN)watch_subtree);      /* (the thread is waited for) */
    HANDLE thread = CreateThread(NULL, 0, directory_request_thread, request, 0, NULL);
    if (!thread) { NtNotifyChangeDirectoryFile(h, (HANDLE)3, NULL, NULL, &iosb, NULL, 0, filter, (BOOLEAN)watch_subtree); NtClose(caller); RtlFreeHeap(heap(), 0, request); return FALSE; }
    (void)thread;      /* the handle is not closed here: see the thread */
    if (bytes_returned) *bytes_returned = 0;
    return TRUE;
  }
  const NTSTATUS status = NtNotifyChangeDirectoryFile(h, NULL, NULL, NULL, &iosb, buffer, length, filter, (BOOLEAN)watch_subtree);
  if (!NT_SUCCESS(status)) { if (bytes_returned) *bytes_returned = 0; return fail_nt(status); }
  if (bytes_returned) *bytes_returned = (DWORD)iosb.Information;
  return TRUE;
}

/* ---- waitable timers ---------------------------------------------------------------------------------------------------------------------------------- */

EXPORT HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access) {
  (void)access;
  NamedAttributes n;
  HANDLE h = NULL;
  const NTSTATUS status = NtCreateTimer(&h, 0x1F0003, named_attributes(&n, name, sa && sa->bInheritHandle), (flags & 1) ? 0 : 1);      /* CREATE_WAITABLE_TIMER_MANUAL_RESET */
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  set_error(status == 0x40000000 ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  return h;
}
EXPORT HANDLE WINAPI CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual_reset, LPCWSTR name) { return CreateWaitableTimerExW(sa, name, manual_reset ? 1 : 0, 0x1F0003); }
EXPORT HANDLE WINAPI CreateWaitableTimerA(LPSECURITY_ATTRIBUTES sa, BOOL manual_reset, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (name && !w) return NULL;
  HANDLE h = CreateWaitableTimerW(sa, manual_reset, w);
  free_wide(w);
  return h;
}
EXPORT HANDLE WINAPI CreateWaitableTimerExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (name && !w) return NULL;
  HANDLE h = CreateWaitableTimerExW(sa, w, flags, access);
  free_wide(w);
  return h;
}
EXPORT HANDLE WINAPI OpenWaitableTimerW(DWORD access, BOOL inherit, LPCWSTR name) {
  NamedAttributes n;
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenTimer(&h, access, named_attributes(&n, name, inherit));
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return h;
}
EXPORT HANDLE WINAPI OpenWaitableTimerA(DWORD access, BOOL inherit, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (!w) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE h = OpenWaitableTimerW(access, inherit, w);
  free_wide(w);
  return h;
}
typedef void (WINAPI *PTIMERAPCROUTINE_)(LPVOID, DWORD, DWORD);
EXPORT BOOL WINAPI SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period_ms, PTIMERAPCROUTINE_ routine, LPVOID context, BOOL resume) {
  if (!due || period_ms < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  LARGE_INTEGER t = *due;      /* negative: relative; positive: a FILETIME */
  const NTSTATUS status = NtSetTimer(h, &t, (PVOID)routine, context, resume != 0, period_ms, NULL);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period_ms, PTIMERAPCROUTINE_ routine, LPVOID context, PVOID reason, ULONG tolerance) {
  (void)reason; (void)tolerance;
  return SetWaitableTimer(h, due, period_ms, routine, context, FALSE);
}
EXPORT BOOL WINAPI CancelWaitableTimer(HANDLE h) {
  const NTSTATUS status = NtCancelTimer(h, NULL);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* ---- other processes and threads by id ---------------------------------------------------------------------------------------------------------------- */

/* ---- file mappings ------------------------------------------------------------------------------------------------------------------------------------ */

#define FILE_MAP_COPY_ 1
#define FILE_MAP_WRITE_ 2
#define FILE_MAP_READ_ 4
#define FILE_MAP_EXECUTE_ 0x20
#define SECTION_ALL_ 0xF001F

EXPORT HANDLE WINAPI CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD size_high, DWORD size_low, LPCWSTR name) {
  const DWORD page = protect & 0xFF;      /* (SEC_COMMIT and the like are in the upper bits) */
  LARGE_INTEGER size;
  size.LowPart = size_low;
  size.HighPart = (LONG)size_high;
  PLARGE_INTEGER size_pointer = (size_low || size_high) ? &size : NULL;
  if (file == INVALID_HANDLE_VALUE) {
    file = NULL;
    if (!size_pointer) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  }
  NamedAttributes n;
  HANDLE h = NULL;
  const NTSTATUS status = NtCreateSection(&h, SECTION_ALL_, named_attributes(&n, name, sa && sa->bInheritHandle), size_pointer, page, protect & 0xFFFFFF00u /* the SEC_* flags */, file);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  set_error(status == 0x40000000 ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  return h;
}
EXPORT HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD size_high, DWORD size_low, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (name && !w) return NULL;
  HANDLE h = CreateFileMappingW(file, sa, protect, size_high, size_low, w);
  const DWORD error = GetLastError();
  free_wide(w);
  set_error(error);
  return h;
}
EXPORT HANDLE WINAPI OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name) {
  NamedAttributes n;
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenSection(&h, access, named_attributes(&n, name, inherit));
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return h;
}
EXPORT HANDLE WINAPI OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (!w) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  HANDLE h = OpenFileMappingW(access, inherit, w);
  free_wide(w);
  return h;
}
EXPORT LPVOID WINAPI MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T bytes, LPVOID base_address) {
  const BOOL exec = (access & FILE_MAP_EXECUTE_) != 0;      /* a copy-on-write view only when COPY is the whole request; no read/write/copy bit: a view nobody may touch */
  access &= ~(DWORD)FILE_MAP_EXECUTE_;
  ULONG page = access == FILE_MAP_COPY_ ? (exec ? 0x80 : 0x08) : (access & FILE_MAP_WRITE_) ? (exec ? 0x40 : PAGE_READWRITE) : (access & FILE_MAP_READ_) ? (exec ? 0x20 : 0x02) : 0x01;
  LARGE_INTEGER offset;
  offset.LowPart = offset_low;
  offset.HighPart = (LONG)offset_high;
  PVOID base = base_address;
  SIZE_T size = bytes;
  const NTSTATUS status = NtMapViewOfSection(mapping, NtCurrentProcess(), &base, 0, 0, &offset, &size, 1 /* ViewShare */, 0, page);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return base;
}
EXPORT LPVOID WINAPI MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T bytes) { return MapViewOfFileEx(mapping, access, offset_high, offset_low, bytes, NULL); }
EXPORT BOOL WINAPI UnmapViewOfFile(LPCVOID base) {
  const NTSTATUS status = NtUnmapViewOfSection(NtCurrentProcess(), (PVOID)base);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI FlushViewOfFile(LPCVOID base, SIZE_T bytes) {
  IO_STATUS_BLOCK iosb;
  PVOID address = (PVOID)base;
  SIZE_T size = bytes;
  const NTSTATUS status = NtFlushVirtualMemory(NtCurrentProcess(), &address, &size, &iosb);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* The memory functions with a process handle: only this process is in reach. */
EXPORT LPVOID WINAPI VirtualAllocEx(HANDLE process, LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
  PVOID base = address;
  SIZE_T bytes = size;
  const NTSTATUS status = NtAllocateVirtualMemory(process, &base, 0, &bytes, type, protect);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return base;
}
EXPORT BOOL WINAPI VirtualFreeEx(HANDLE process, LPVOID address, SIZE_T size, DWORD type) {
  PVOID base = address;
  SIZE_T bytes = size;
  const NTSTATUS status = NtFreeVirtualMemory(process, &base, &bytes, type);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI VirtualProtectEx(HANDLE process, LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
  if (!old) { set_error(ERROR_NOACCESS); return FALSE; }
  PVOID base = address;
  SIZE_T bytes = size;
  ULONG previous = 0;
  const NTSTATUS status = NtProtectVirtualMemory(process, &base, &bytes, protect, &previous);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (old) *old = previous;
  return TRUE;
}
EXPORT SIZE_T WINAPI VirtualQueryEx(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length) {
  if (length < sizeof(*info)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  ULONG64 got = 0;
  const NTSTATUS status = NtQueryVirtualMemory(process, (PVOID)address, 0, info, sizeof(*info), &got);
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0; }
  return sizeof(*info);
}

EXPORT HANDLE WINAPI OpenThread(DWORD access, BOOL inherit, DWORD id) {
  OBJECT_ATTRIBUTES attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.Length = sizeof(attributes);
  attributes.Attributes = inherit ? 2 : 0;
  ULONG64 client[2] = {0, id};
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenThread(&h, access, &attributes, client);
  if (!NT_SUCCESS(status)) { fail_nt(status); return NULL; }
  return h;
}
EXPORT BOOL WINAPI SwitchToThread(void) { NtYieldExecution(); return TRUE; }

/* The Win32 result of a wait: the index (WAIT_OBJECT_0 + i), WAIT_ABANDONED_0 + i, WAIT_TIMEOUT, WAIT_IO_COMPLETION. */
static DWORD wait_result(NTSTATUS status) {
  if (status >= 0 && status < 64) return WAIT_OBJECT_0 + (DWORD)status;
  if (status >= 0x80 && status < 0x80 + 64) return WAIT_ABANDONED + ((DWORD)status - 0x80);
  if (status == 0x102) return WAIT_TIMEOUT;
  if (status == 0x101 || status == 0xC0) return WAIT_IO_COMPLETION;      /* STATUS_ALERTED, STATUS_USER_APC: the APCs have run */
  fail_nt(status);
  return WAIT_FAILED;
}
EXPORT BOOL WINAPI SwitchToThread(void);
EXPORT DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD timeout_ms, BOOL alertable) {
  if ((ULONG_PTR)h >= 0xFFFFFFF4u && (ULONG_PTR)h <= 0xFFFFFFF6u) h = GetStdHandle((DWORD)(ULONG_PTR)h);      /* STD_INPUT_HANDLE and the others */
  con_sync_signals();
  if (con_wait_polled(h)) {      /* the console input is ready when a key event is waiting: asked again until it is */
    const DWORD start = GetTickCount();
    while (!con_input_ready()) {
      if (timeout_ms != INFINITE && GetTickCount() - start >= timeout_ms) return WAIT_TIMEOUT;
      SleepEx(5, alertable);
    }
    return WAIT_OBJECT_0;
  }
  LARGE_INTEGER t;
  t.QuadPart = -(LONGLONG)timeout_ms * 10000;
  DWORD result = wait_result(NtWaitForSingleObject(h, alertable != 0, timeout_ms == INFINITE ? NULL : &t));
  return result;
}
EXPORT DWORD WINAPI WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout_ms, BOOL alertable) {
  if (count == 0 || count > 64 || !handles) { set_error(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
  HANDLE mapped[64];      /* the STD_*_HANDLE numbers stand for the handles the process has for them */
  for (DWORD i = 0; i < count; ++i) mapped[i] = ((ULONG_PTR)handles[i] >= 0xFFFFFFF4u && (ULONG_PTR)handles[i] <= 0xFFFFFFF6u) ? GetStdHandle((DWORD)(ULONG_PTR)handles[i]) : handles[i];
  handles = mapped;
  con_sync_signals();
  for (DWORD i = 0; i < count && !all; ++i) {
    if (!con_wait_polled(handles[i])) continue;
    HANDLE others[64];      /* a console input among the handles: the others are asked without waiting, in turns with it */
    DWORD index[64], n = 0;
    for (DWORD k = 0; k < count; ++k) if (!con_wait_polled(handles[k])) { others[n] = handles[k]; index[n++] = k; }
    const DWORD start = GetTickCount();
    for (;;) {
      if (con_input_ready()) return WAIT_OBJECT_0 + i;
      if (n > 0) { const DWORD r = WaitForMultipleObjectsEx(n, others, FALSE, 0, FALSE); if (r < n) return WAIT_OBJECT_0 + index[r]; if (r != WAIT_TIMEOUT) return r; }
      if (timeout_ms != INFINITE && GetTickCount() - start >= timeout_ms) return WAIT_TIMEOUT;
      SleepEx(5, alertable);
    }
  }
  LARGE_INTEGER t;
  t.QuadPart = -(LONGLONG)timeout_ms * 10000;
  return wait_result(NtWaitForMultipleObjects(count, handles, all ? 0 : 1, alertable != 0, timeout_ms == INFINITE ? NULL : &t));
}
EXPORT DWORD WINAPI WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout_ms) { return WaitForMultipleObjectsEx(count, handles, all, timeout_ms, FALSE); }

/* ---- threads ------------------------------------------------------------------------------------------------------------------------------------------------- */

#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000
IMPORT NTSTATUS NTAPI RtlGetActiveActivationContext(PVOID *);
IMPORT NTSTATUS NTAPI RtlActivateActivationContext(ULONG, PVOID, ULONG_PTR *);
IMPORT VOID NTAPI RtlReleaseActivationContext(PVOID);
/* A thread starts with the activation context that was active in the thread that made it. */
typedef struct ActThreadStart { LPTHREAD_START_ROUTINE start; LPVOID parameter; PVOID context; } ActThreadStart;
static DWORD WINAPI actctx_thread_start(LPVOID p) {
  ActThreadStart a = *(ActThreadStart *)p;
  RtlFreeHeap(heap(), 0, p);
  ULONG_PTR cookie;
  RtlActivateActivationContext(0, a.context, &cookie);
  RtlReleaseActivationContext(a.context);      /* (the frame keeps its own use of it) */
  return a.start(a.parameter);
}
EXPORT HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD id) {
  (void)sa;
  HANDLE thread = NULL;
  PVOID active = NULL;
  RtlGetActiveActivationContext(&active);
  if (active) {
    ActThreadStart *a = RtlAllocateHeap(heap(), 0, sizeof(*a));
    if (a) { a->start = start; a->parameter = parameter; a->context = active; start = actctx_thread_start; parameter = a; }
    else { RtlReleaseActivationContext(active); active = NULL; }
  }
  const NTSTATUS status = NtCreateThreadEx(&thread, 0x1FFFFF, NULL, NtCurrentProcess(), (PVOID)start, parameter, (flags & CREATE_SUSPENDED) ? 1 : 0, 0, stack, stack, NULL);
  if (!NT_SUCCESS(status)) {
    if (active) { RtlReleaseActivationContext(active); RtlFreeHeap(heap(), 0, parameter); }
    set_error(status == STATUS_NOT_IMPLEMENTED ? ERROR_NOT_SUPPORTED : RtlNtStatusToDosError(status));
    return NULL;
  }
  if (id) {
    ULONG64 info[6];
    *id = NT_SUCCESS(NtQueryInformationThread(thread, 0, info, sizeof(info), NULL)) ? (DWORD)info[3] : 0;
  }
  return thread;
}
EXPORT BOOL WINAPI GetExitCodeThread(HANDLE thread, LPDWORD code) {
  ULONG64 info[6];
  const NTSTATUS status = NtQueryInformationThread(thread, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *code = (DWORD)info[0];
  return TRUE;
}
EXPORT DWORD WINAPI GetThreadId(HANDLE thread) {
  ULONG64 info[6];
  return NT_SUCCESS(NtQueryInformationThread(thread, 0, info, sizeof(info), NULL)) ? (DWORD)info[3] : 0;
}
EXPORT DWORD WINAPI ResumeThread(HANDLE thread) {
  ULONG previous = 0;
  const NTSTATUS status = NtResumeThread(thread, &previous);
  if (!NT_SUCCESS(status)) { fail_nt(status); return (DWORD)-1; }
  return previous;
}
EXPORT DWORD WINAPI SuspendThread(HANDLE h) {
  ULONG previous = 0;
  const NTSTATUS status = NtSuspendThread(h, &previous);
  if (!NT_SUCCESS(status)) { fail_nt(status); return (DWORD)-1; }
  return previous;
}
EXPORT BOOL WINAPI TerminateThread(HANDLE h, DWORD code) { const NTSTATUS status = NtTerminateThread(h, (NTSTATUS)code); return NT_SUCCESS(status) ? TRUE : fail_nt(status); }
EXPORT BOOL WINAPI SetThreadPriority(HANDLE h, int priority) {
  if (!(priority == -15 || priority == 15 || (priority >= -2 && priority <= 2))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }      /* IDLE, LOWEST..HIGHEST, TIME_CRITICAL */
  LONG value = priority;
  const NTSTATUS status = NtSetInformationThread(h, 3, &value, sizeof(value));      /* ThreadBasePriority */
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

EXPORT BOOL WINAPI SetProcessAffinityMask(HANDLE h, DWORD_PTR mask);
typedef struct GroupAffinity { ULONG_PTR Mask; WORD Group; WORD Reserved[3]; } GROUP_AFFINITY;
typedef ULONG_PTR KAFFINITY, *PULONG_PTR;
EXPORT BOOL WINAPI GetProcessAffinityMask(HANDLE h, DWORD_PTR *process, DWORD_PTR *system);
/* Priority boost: whether the system may raise the thread's priority is only remembered (nothing here boosts). */
static struct { DWORD id; BOOL disabled; } g_boost[64];
static BOOL g_process_boost_disabled;
EXPORT BOOL WINAPI SetThreadPriorityBoost(HANDLE h, BOOL disabled) {
  BYTE info[48];
  const NTSTATUS status = NtQueryInformationThread(h, 0, info, sizeof(info), NULL);      /* (the handle has to name a thread, and allow querying) */
  if (!NT_SUCCESS(status)) return fail_nt(status);
  LONG value = 0;
  const NTSTATUS set = NtSetInformationThread(h, 3, &value, 0);      /* (only the right to set is checked: a length of 0 changes nothing) */
  if (set == 0xC0000022u) return fail_nt(set);
  const DWORD id = GetThreadId(h);
  int slot = -1;
  for (int i = 0; i < 64; ++i) if (g_boost[i].id == id) { slot = i; break; }
  if (slot < 0) for (int i = 0; i < 64; ++i) if (!g_boost[i].id) { slot = i; break; }
  if (slot < 0) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  g_boost[slot].id = id;
  g_boost[slot].disabled = disabled != 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetThreadPriorityBoost(HANDLE h, PBOOL disabled) {
  BYTE info[48];
  const NTSTATUS status = NtQueryInformationThread(h, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  const DWORD id = GetThreadId(h);
  *disabled = g_process_boost_disabled;
  for (int i = 0; i < 64; ++i) if (g_boost[i].id == id) *disabled = g_boost[i].disabled;
  return TRUE;
}
EXPORT BOOL WINAPI SetProcessPriorityBoost(HANDLE process, BOOL disabled) {
  (void)process;
  g_process_boost_disabled = disabled != 0;
  for (int i = 0; i < 64; ++i) g_boost[i].id = 0;      /* (every thread follows the process) */
  return TRUE;
}
EXPORT BOOL WINAPI GetProcessPriorityBoost(HANDLE process, PBOOL disabled) { (void)process; *disabled = g_process_boost_disabled; return TRUE; }

/* The limits of the thread's stack, and the part of it kept for handling a stack overflow. */
EXPORT void WINAPI GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high) {
  *low = *(ULONG_PTR *)TEB_BYTE(NtCurrentTeb(), 0x1478);      /* DeallocationStack */
  *high = *(ULONG_PTR *)TEB_BYTE(NtCurrentTeb(), 0x08);      /* StackBase */
}
EXPORT BOOL WINAPI SetThreadStackGuarantee(PULONG size) {
  ULONG *guarantee = (ULONG *)TEB_BYTE(NtCurrentTeb(), 0x1748);
  const ULONG old = *guarantee, wanted = *size;
  const ULONG_PTR stack = *(ULONG_PTR *)TEB_BYTE(NtCurrentTeb(), 0x08) - *(ULONG_PTR *)TEB_BYTE(NtCurrentTeb(), 0x1478);
  *size = old;
  if (wanted == 0) return TRUE;
  if (wanted > stack / 2) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  ULONG rounded = (wanted + 0xFFF) & ~0xFFFu;
  if (rounded < 8192) rounded = 8192;      /* (at least two pages) */
  if (rounded > old) *guarantee = rounded;
  return TRUE;
}

/* Processor groups: there is one, group 0, and the thread's affinity mask is its mask. */
EXPORT BOOL WINAPI GetThreadGroupAffinity(HANDLE h, GROUP_AFFINITY *affinity) {
  if (!affinity) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BYTE info[48];
  const NTSTATUS status = NtQueryInformationThread(h, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  memset(affinity, 0, sizeof(*affinity));
  affinity->Mask = (KAFFINITY)get64(info + 32);
  affinity->Group = 0;
  return TRUE;
}
EXPORT BOOL WINAPI SetThreadGroupAffinity(HANDLE h, const GROUP_AFFINITY *affinity, GROUP_AFFINITY *previous) {
  if (!affinity) { set_error(ERROR_NOACCESS); return FALSE; }
  DWORD_PTR process_mask = 0, system_mask = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
  if (affinity->Group != 0 || (affinity->Mask & ~system_mask)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BYTE info[48];
  NTSTATUS status = NtQueryInformationThread(h, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  const KAFFINITY old = (KAFFINITY)get64(info + 32);
  (void)process_mask;
  GROUP_AFFINITY sent = *affinity;      /* (no mask: the thread follows the process's again) */
  status = NtSetInformationThread(h, 30, &sent, 16);      /* ThreadGroupInformation */
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (previous) { memset(previous, 0, sizeof(*previous)); previous->Mask = old; }
  return TRUE;
}
EXPORT int WINAPI GetThreadPriority(HANDLE h) {
  BYTE info[48];
  const NTSTATUS status = NtQueryInformationThread(h, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) { fail_nt(status); return THREAD_PRIORITY_ERROR_RETURN; }
  return (LONG)get32(info + 44);      /* BasePriority */
}
EXPORT DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
  if (mask == 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  BYTE info[48];
  NTSTATUS status = NtQueryInformationThread(h, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0; }
  const DWORD_PTR old = get64(info + 32);
  ULONGLONG value = mask;
  status = NtSetInformationThread(h, 4, &value, sizeof(value));      /* ThreadAffinityMask */
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0; }
  return old;
}
EXPORT DWORD WINAPI SetThreadIdealProcessor(HANDLE h, DWORD processor) {
  ULONG value = processor;
  const NTSTATUS status = NtSetInformationThread(h, 13, &value, sizeof(value));
  if (!NT_SUCCESS(status)) { fail_nt(status); return (DWORD)-1; }
  return 0;
}
EXPORT BOOL WINAPI SetProcessAffinityMask(HANDLE h, DWORD_PTR mask) {
  ULONGLONG value = mask;
  const NTSTATUS status = NtSetInformationProcess(h, 21, &value, sizeof(value));      /* ProcessAffinityMask */
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI GetProcessAffinityMask(HANDLE h, DWORD_PTR *process, DWORD_PTR *system) {
  ULONGLONG mask = 0;
  BYTE basic[64] = {};
  NTSTATUS status = NtQueryInformationProcess(h, 21, &mask, sizeof(mask), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  status = NtQuerySystemInformation(0, basic, sizeof(basic), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (process) *process = mask;
  if (system) *system = get64(basic + 48);
  return TRUE;
}
EXPORT DWORD WINAPI GetCurrentProcessorNumber(void) { return NtGetCurrentProcessorNumber(); }
EXPORT DWORD WINAPI GetPriorityClass(HANDLE h) {
  BYTE info[2];
  const NTSTATUS status = NtQueryInformationProcess(h, 18, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0; }
  switch (info[1]) {
    case 1: return IDLE_PRIORITY_CLASS;
    case 3: return HIGH_PRIORITY_CLASS;
    case 4: return REALTIME_PRIORITY_CLASS;
    case 5: return BELOW_NORMAL_PRIORITY_CLASS;
    case 6: return ABOVE_NORMAL_PRIORITY_CLASS;
    default: return NORMAL_PRIORITY_CLASS;
  }
}
EXPORT BOOL WINAPI SetPriorityClass(HANDLE h, DWORD class_) {
  BYTE info[2] = {0, 2};
  switch (class_) {
    case IDLE_PRIORITY_CLASS: info[1] = 1; break;
    case HIGH_PRIORITY_CLASS: info[1] = 3; break;
    case REALTIME_PRIORITY_CLASS: info[1] = 4; break;
    case BELOW_NORMAL_PRIORITY_CLASS: info[1] = 5; break;
    case ABOVE_NORMAL_PRIORITY_CLASS: info[1] = 6; break;
    case NORMAL_PRIORITY_CLASS: break;
    default: set_error(ERROR_INVALID_PARAMETER); return FALSE;
  }
  const NTSTATUS status = NtSetInformationProcess(h, 18, info, sizeof(info));
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI GetProcessTimes(HANDLE h, LPFILETIME creation, LPFILETIME exit_time, LPFILETIME kernel, LPFILETIME user) {
  ULONGLONG t[4];
  const NTSTATUS status = NtQueryInformationProcess(h, 4, t, sizeof(t), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  FILETIME *out[4] = {creation, exit_time, kernel, user};
  for (int i = 0; i < 4; ++i) if (out[i]) { out[i]->dwLowDateTime = (DWORD)t[i]; out[i]->dwHighDateTime = (DWORD)(t[i] >> 32); }
  return TRUE;
}
EXPORT BOOL WINAPI GetThreadTimes(HANDLE h, LPFILETIME creation, LPFILETIME exit_time, LPFILETIME kernel, LPFILETIME user) {
  ULONGLONG t[4];
  const NTSTATUS status = NtQueryInformationThread(h, 1, t, sizeof(t), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  FILETIME *out[4] = {creation, exit_time, kernel, user};
  for (int i = 0; i < 4; ++i) if (out[i]) { out[i]->dwLowDateTime = (DWORD)t[i]; out[i]->dwHighDateTime = (DWORD)(t[i] >> 32); }
  return TRUE;
}
EXPORT BOOL WINAPI GetProcessHandleCount(HANDLE h, PDWORD count) {
  ULONG n = 0;
  const NTSTATUS status = NtQueryInformationProcess(h, 20, &n, sizeof(n), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *count = n;
  return TRUE;
}
EXPORT DWORD WINAPI QueryDosDeviceW(LPCWSTR name, LPWSTR buffer, DWORD capacity);
/* A path "C:\dir\file" as the NT name of the device that holds the drive followed by "\dir\file" ("\Device\HarddiskVolume1\dir\file"). False if there is no such drive. */
static BOOL dos_path_to_native(const WCHAR *dos, WCHAR *out, DWORD capacity) {
  WCHAR drive[3] = {dos[0], ':', 0}, device[PATH_CHARS];
  if (!dos[0] || dos[1] != ':' || QueryDosDeviceW(drive, device, PATH_CHARS) == 0) return FALSE;
  const size_t_ device_length = wcslen(device), rest = wcslen(dos + 2);
  if (device_length + rest + 1 > capacity) return FALSE;
  memcpy(out, device, device_length * 2);
  memcpy(out + device_length, dos + 2, (rest + 1) * 2);
  return TRUE;
}
EXPORT BOOL WINAPI QueryFullProcessImageNameW(HANDLE h, DWORD flags, LPWSTR name, PDWORD size) {
  if (flags > 1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }      /* (PROCESS_NAME_NATIVE is the only flag) */
  BYTE info[16 + 2 * (MAX_PATH * 2 + 1)];
  const NTSTATUS status = NtQueryInformationProcess(h, (flags & 1) ? 27 : 43 /* ProcessImageFileName, ProcessImageFileNameWin32 */, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  const WCHAR *text = (const WCHAR *)(ULONG_PTR)get64(info + 8);
  const DWORD n = get16(info) / 2;
  if (!name) { set_error(*size == 0 ? ERROR_INSUFFICIENT_BUFFER : ERROR_INVALID_PARAMETER); return FALSE; }
  if (n + 1 > *size) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memcpy(name, text, n * 2);
  name[n] = 0;
  *size = n;
  return TRUE;
}
EXPORT BOOL WINAPI QueryFullProcessImageNameA(HANDLE h, DWORD flags, LPSTR name, PDWORD size) {
  WCHAR wide[PATH_CHARS];
  DWORD n = PATH_CHARS;
  if (!QueryFullProcessImageNameW(h, flags, wide, &n)) return FALSE;
  if (!name) { set_error(*size == 0 ? ERROR_INSUFFICIENT_BUFFER : ERROR_INVALID_PARAMETER); return FALSE; }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, (int)n + 1, NULL, 0, NULL, NULL);
  if (needed <= 0 || (DWORD)needed > *size) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  WideCharToMultiByte(CP_UTF8, 0, wide, (int)n + 1, name, needed, NULL, NULL);
  *size = (DWORD)needed - 1;
  return TRUE;
}
EXPORT HANDLE WINAPI OpenProcess(DWORD access, BOOL inherit, DWORD id) {
  OBJECT_ATTRIBUTES attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.Length = sizeof(attributes);
  attributes.Attributes = inherit ? 2 : 0;
  ULONG64 client[2] = {id, 0};
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenProcess(&h, access, &attributes, client);
  if (!NT_SUCCESS(status)) { if (status == (NTSTATUS)0xC000000B) set_error(ERROR_INVALID_PARAMETER); else fail_nt(status); return NULL; }
  return h;
}
/* ---- CreateProcess ----------------------------------------------------------------------------------------------------------------------------------------- */

static DWORD full_path_w(LPCWSTR in, WCHAR *out, DWORD capacity);

EXPORT DWORD WINAPI GetFileAttributesW(LPCWSTR path);
EXPORT DWORD WINAPI GetCurrentDirectoryW(DWORD capacity, LPWSTR buffer);

static BOOL is_program_file(LPCWSTR path) {
  const DWORD a = GetFileAttributesW(path);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Looks for a file the way CreateProcess and LoadLibrary do: a name with a path as it is; a bare name in the folder of this program, `extra` (SetDllDirectory), the folders
   C:\Windows\System32 and C:\Windows, the current folder and then those of PATH. `extension` (".exe", ".dll") is added to a name that has none. */
static BOOL try_candidate(LPCWSTR directory, size_t_ directory_length, LPCWSTR file, WCHAR *out) {
  WCHAR candidate[PATH_CHARS];
  const size_t_ f = wcslen(file);
  if (directory_length + 1 + f + 1 > PATH_CHARS) return FALSE;
  memcpy(candidate, directory, directory_length * 2);
  candidate[directory_length] = '\\';
  memcpy(candidate + directory_length + 1, file, (f + 1) * 2);
  if (!is_program_file(candidate)) return FALSE;
  lstrcpyW(out, candidate);
  return TRUE;
}
static BOOL find_file(LPCWSTR name, LPCWSTR extension, LPCWSTR extra, WCHAR *out) {
  BOOL path_given = FALSE, has_extension = FALSE;
  for (LPCWSTR c = name; *c; ++c) {
    if (*c == '\\' || *c == '/' || *c == ':') { path_given = TRUE; has_extension = FALSE; }
    else if (*c == '.') has_extension = TRUE;
  }
  WCHAR file[MAX_PATH];
  const size_t_ length = wcslen(name), extension_length = wcslen(extension);
  if (length == 0 || length + extension_length + 1 > MAX_PATH) return FALSE;
  memcpy(file, name, (length + 1) * 2);
  if (!has_extension) memcpy(file + length, extension, (extension_length + 1) * 2);
  if (path_given) {
    if (full_path_w(file, out, PATH_CHARS) == 0) return FALSE;
    return is_program_file(out);
  }
  static const WCHAR system32[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', '3', '2', 0};
  static const WCHAR windows[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', 0};
  WCHAR directory[PATH_CHARS];
  DWORD n = GetModuleFileNameW(NULL, directory, PATH_CHARS);      /* the folder of the program */
  while (n > 0 && directory[n - 1] != '\\') --n;
  if (n > 0 && try_candidate(directory, n - 1, file, out)) return TRUE;
  if (extra && extra[0] && try_candidate(extra, wcslen(extra), file, out)) return TRUE;
  if (try_candidate(system32, wcslen(system32), file, out) || try_candidate(windows, wcslen(windows), file, out)) return TRUE;
  n = GetCurrentDirectoryW(PATH_CHARS, directory);
  if (n > 0 && n < PATH_CHARS && try_candidate(directory, n, file, out)) return TRUE;
  WCHAR path[8192];
  static const WCHAR variable[] = {'P', 'A', 'T', 'H', 0};
  n = GetEnvironmentVariableW(variable, path, 8192);
  if (n == 0 || n >= 8192) return FALSE;
  for (WCHAR *start = path; *start;) {
    WCHAR *end = start;
    while (*end && *end != ';') ++end;
    if (end > start && (size_t_)(end - start) < PATH_CHARS && try_candidate(start, (size_t_)(end - start), file, out)) return TRUE;
    start = *end ? end + 1 : end;
  }
  return FALSE;
}
NTSTATUS NTAPI LdrAddDllDirectory(PUNICODE_STRING directory, PVOID *cookie);
NTSTATUS NTAPI LdrRemoveDllDirectory(PVOID cookie);
NTSTATUS NTAPI LdrGetDllPath(PCWSTR name, ULONG flags, PWSTR *path, PWSTR *unused);
NTSTATUS NTAPI LdrSetDefaultDllDirectories(ULONG flags);
NTSTATUS NTAPI RtlSetSearchPathMode(ULONG flags);
NTSTATUS NTAPI RtlGetSearchPath(PWSTR *path);
VOID NTAPI RtlReleasePath(PWSTR path);

EXPORT PVOID WINAPI AddDllDirectory(LPCWSTR directory) {
  const BOOL device = directory && directory[0] == '\\' && directory[1] == '\\' && (directory[2] == '.' || directory[2] == '?') && directory[3] == '\\';
  if (!directory || !((directory[0] && directory[1] == ':' && directory[2] == '\\') || directory[0] == '\\')) { set_error(ERROR_INVALID_PARAMETER); return NULL; }      /* (a full path) */
  if (!device && GetFileAttributesW(directory) == INVALID_FILE_ATTRIBUTES) { set_error(ERROR_FILE_NOT_FOUND); return NULL; }
  UNICODE_STRING text;
  unicode_string(&text, directory);
  PVOID cookie = NULL;
  const NTSTATUS status = LdrAddDllDirectory(&text, &cookie);
  if (!NT_SUCCESS(status)) { set_error(status == STATUS_NO_MEMORY ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return NULL; }
  return cookie;
}
EXPORT BOOL WINAPI RemoveDllDirectory(PVOID cookie) {
  if (!NT_SUCCESS(LdrRemoveDllDirectory(cookie))) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI SetDefaultDllDirectories(DWORD flags) {
  if (flags == 0 || (flags & ~0x1F00u) || (flags & 0x100)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  g_default_dll_dirs = flags;
  LdrSetDefaultDllDirectories(flags);
  return TRUE;
}

/* The LOAD_LIBRARY_SEARCH_* lookup: a full path is taken as it is, anything else is looked for in the folders the flags name (application 0x200, user 0x400, System32 0x800).
   Returns 1 found, 0 not found, -1 for flags that cannot work with this name. */
static int find_library_restricted(LPCWSTR name, DWORD search, WCHAR *out) {
  static const WCHAR dll[] = {'.', 'd', 'l', 'l', 0};
  static const WCHAR system32[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', '3', '2', 0};
  const BOOL absolute = (name[0] && name[1] == ':' && (name[2] == '\\' || name[2] == '/')) || (name[0] == '\\' && name[1] == '\\');
  BOOL has_extension = FALSE;
  for (LPCWSTR c = name; *c; ++c) { if (*c == '\\' || *c == '/' || *c == ':') has_extension = FALSE; else if (*c == '.') has_extension = TRUE; }
  WCHAR file[MAX_PATH];
  const size_t_ length = wcslen(name);
  if (length + 5 > MAX_PATH) return 0;
  memcpy(file, name, (length + 1) * 2);
  if (!has_extension) memcpy(file + length, dll, sizeof(dll));
  const BOOL rooted = name[0] == '\\' || (name[0] && name[1] == ':');      /* (rooted in the current drive, or a drive with its current folder) */
  if (absolute || rooted) return full_path_w(file, out, PATH_CHARS) != 0 && is_program_file(out) ? 1 : 0;
  if (search & 0x100) return -1;      /* (the folder of the library that is loaded needs a path to take it from) */
  PWSTR list = NULL, unused = NULL;
  if (!NT_SUCCESS(LdrGetDllPath(name, search, &list, &unused))) return -1;
  int found = 0;
  for (WCHAR *start = list; *start && !found;) {
    WCHAR *end = start;
    while (*end && *end != ';') ++end;
    if (end > start && (size_t_)(end - start) < PATH_CHARS && try_candidate(start, (size_t_)(end - start), file, out)) found = 1;
    start = *end ? end + 1 : end;
  }
  RtlReleasePath(list);
  return found;
}

static BOOL find_program(LPCWSTR name, WCHAR *out) {
  static const WCHAR exe[] = {'.', 'e', 'x', 'e', 0};
  return find_file(name, exe, NULL, out);
}

static void unicode_string(UNICODE_STRING *s, LPCWSTR text) {
  const size_t_ n = text ? wcslen(text) : 0;
  s->Length = (USHORT)(n * 2);
  s->MaximumLength = (USHORT)(n * 2 + 2);
  s->Buffer = (PWSTR)text;
}

typedef struct ProcThreadAttributeEntry { DWORD_PTR attribute; SIZE_T size; PVOID value; } ProcThreadAttributeEntry;
typedef struct ProcThreadAttributeList { DWORD flags; DWORD capacity; DWORD count; DWORD reserved; PVOID unknown; ProcThreadAttributeEntry entries[1]; } ProcThreadAttributeList;
/* The value of an attribute of a list, or NULL. */
static const ProcThreadAttributeEntry *attribute_of(const ProcThreadAttributeList *list, DWORD_PTR attribute) {
  for (DWORD i = 0; list && i < list->count && i < list->capacity; ++i) if (list->entries[i].attribute == attribute) return &list->entries[i];
  return NULL;
}

#define CON_ID_NO_WINDOW 0x1000000000000ull      /* in the number of a console: it has no window (CREATE_NO_WINDOW) */
#define CON_ID_PSEUDO 0x2000000000000ull         /* it is a pseudo console: its size is in the bits above (12 bits each: width from bit 36, height from bit 50 up) */
#define CON_ID_PSEUDO_X(id) ((int)(((id) >> 36) & 0xFFF))
#define CON_ID_PSEUDO_Y(id) ((int)(((id) >> 50) & 0xFFF))

/* The pseudo consoles that were made (CreatePseudoConsole): the object that stands for one and the size it has. */
static struct { HANDLE object; SHORT x, y; } g_pseudo[8];

/* Is the program a window program (IMAGE_SUBSYSTEM_WINDOWS_GUI)? Its header says; a file that cannot be read is taken for a console program. */
static BOOL image_is_gui(LPCWSTR path) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return FALSE;
  BYTE head[0x400];
  DWORD got = 0;
  BOOL gui = FALSE;
  if (ReadFile(f, head, sizeof(head), &got, NULL) && got >= 0x40) {
    const DWORD at = head[0x3C] | (head[0x3D] << 8) | (head[0x3E] << 16) | ((DWORD)head[0x3F] << 24);
    if (at <= got && got - at >= 24 + 70) gui = (head[at + 24 + 68] | (head[at + 24 + 69] << 8)) == 2;
  }
  CloseHandle(f);
  return gui;
}

/* The console a new process belongs to. A window program has none. A console program has none with DETACHED_PROCESS, a console of its own (with a window) with CREATE_NEW_CONSOLE
 * or when this process has no console, a console without a window with CREATE_NO_WINDOW, otherwise the one of this process. `fresh` tells that the console is a new one. */
static void con_note_child(DWORD pid, HANDLE console, DWORD group, DWORD ignored);
static HANDLE console_for_new_process(DWORD flags, BOOL gui, BOOL *fresh) {
  static volatile LONG counter;
  if (fresh) *fresh = FALSE;
  if (gui || (flags & 0x8 /* DETACHED_PROCESS */)) return NULL;
  const BOOL new_console = (flags & 0x10 /* CREATE_NEW_CONSOLE */) != 0;
  if (!new_console && parameters()->ConsoleHandle && !(flags & 0x08000000 /* CREATE_NO_WINDOW */)) return parameters()->ConsoleHandle;
  if (fresh) *fresh = TRUE;
  return (HANDLE)(ULONG_PTR)(0x100000000ull | (!new_console && (flags & 0x08000000) ? CON_ID_NO_WINDOW : 0) | ((ULONG_PTR)GetCurrentProcessId() << 8) |
                             (ULONG_PTR)(InterlockedIncrement(&counter) & 0xFF));
}
/* The console of a program started in a pseudo console: a new one of the size the pseudo console has. */
static HANDLE console_for_pseudo(HANDLE pseudo) {
  for (int i = 0; i < 8; ++i) {
    if (g_pseudo[i].object != pseudo || !pseudo) continue;
    ULONG_PTR id = (ULONG_PTR)console_for_new_process(0x10, FALSE, NULL);
    return (HANDLE)(id | CON_ID_PSEUDO | ((ULONG_PTR)g_pseudo[i].x << 36) | ((ULONG_PTR)g_pseudo[i].y << 50));
  }
  return NULL;
}
EXPORT BOOL WINAPI CreateProcessW(LPCWSTR application, LPWSTR command_line, LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes,
                                  BOOL inherit_handles, DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) {
  (void)process_attributes; (void)thread_attributes;
  if (information) memset(information, 0, sizeof(*information));
  if (!application && !command_line) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR name[MAX_PATH], image[PATH_CHARS];
  if (application) {
    if (!application[0]) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (wcslen(application) >= MAX_PATH) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    lstrcpyW(name, application);
  } else {      /* the first word of the command line (in quotes, or up to a space or tab); nothing before it may be blank */
    LPCWSTR c = command_line;
    size_t_ n = 0;
    const BOOL quoted = *c == '"';
    if (quoted) ++c;
    while (*c && (quoted ? *c != '"' : (*c != ' ' && *c != '\t'))) { if (n + 1 >= MAX_PATH) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; } name[n++] = *c++; }
    name[n] = 0;
    while (n > 0 && name[n - 1] == ' ') name[--n] = 0;      /* (the blanks that end a name are not part of it) */
    if (n == 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  }
  if (directory) {      /* a folder that is not there is an error of its own, whatever the program */
    WCHAR full[PATH_CHARS];
    const DWORD a = full_path_w(directory, full, PATH_CHARS) ? GetFileAttributesW(full) : INVALID_FILE_ATTRIBUTES;
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { set_error(ERROR_DIRECTORY); return FALSE; }
  }
  if (!find_program(name, image)) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
  WCHAR current[PATH_CHARS];
  UNICODE_STRING image_name, command, cwd;
  unicode_string(&image_name, image);
  unicode_string(&command, command_line ? command_line : name);
  PUNICODE_STRING cwd_pointer = NULL;
  if (directory) {
    if (full_path_w(directory, current, PATH_CHARS) == 0) return FALSE;
    unicode_string(&cwd, current);
    cwd_pointer = &cwd;
  }
  HANDLE process = NULL, thread = NULL;
  ULONG process_id = 0, thread_id = 0;
  /* STARTF_USESTDHANDLES (0x100): the child's standard handles are the ones in the startup information (inheritable handles of this process, or the console's). */
  const BOOL debug_start = (flags & (0x1 | 0x2) /* DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS */) != 0;
  if (debug_start && !NT_SUCCESS(DbgUiConnectToDbg())) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  ULONG nt_flags = ((flags & CREATE_SUSPENDED) || debug_start ? 1 : 0) | (inherit_handles ? 2 : 0) | ((flags & 0x01000000 /* CREATE_BREAKAWAY_FROM_JOB */) ? 8 : 0);
  /* the standard handles of the child: the ones of the startup information, none for a program without a console, or those of this program (the kernel decides which of them the
     child can have, by what it inherits) */
  HANDLE std_in, std_out, std_err;
  nt_flags |= 0x10;
  if (startup && (startup->dwFlags & 0x100)) {
    nt_flags |= 4;
    std_in = startup->hStdInput;
    std_out = startup->hStdOutput;
    std_err = startup->hStdError;
  } else {
    std_in = parameters()->StandardInput;
    std_out = parameters()->StandardOutput;
    std_err = parameters()->StandardError;
  }
  /* the startup block of the kernel: size, STARTF_ flags, position, size, characters, fill, show, console flags, reserved; the console; the title and the desktop; the process
     that stands in for the parent (an attribute) and the handles that are inherited (an attribute) */
  struct { ULONG words[12]; HANDLE console; PUNICODE_STRING title, desktop; HANDLE parent; HANDLE *inherit_list; ULONG inherit_count, reserved; } block;
  UNICODE_STRING title_string, desktop_string;
  memset(&block, 0, sizeof(block));
  block.words[0] = sizeof(block);
  if (startup) {
    block.words[1] = startup->dwFlags; block.words[2] = startup->dwX; block.words[3] = startup->dwY; block.words[4] = startup->dwXSize; block.words[5] = startup->dwYSize;
    block.words[6] = startup->dwXCountChars; block.words[7] = startup->dwYCountChars; block.words[8] = startup->dwFillAttribute; block.words[9] = startup->wShowWindow;
    if (startup->lpTitle) { unicode_string(&title_string, startup->lpTitle); block.title = &title_string; }
    if (startup->lpDesktop) { unicode_string(&desktop_string, startup->lpDesktop); block.desktop = &desktop_string; }
  }
  if (!block.title) { unicode_string(&title_string, image); block.title = &title_string; }      /* (a program without a title has its own path as the title) */
  BOOL fresh_console = FALSE;
  block.console = console_for_new_process(flags, image_is_gui(image), &fresh_console);
  if (fresh_console) nt_flags |= 0x100;      /* (the standard handles of a console program with a console of its own are that console's) */
  /* the console flags (bit 0: Ctrl+C is ignored) are the ones of this process, and a new process group ignores it; the group is this process's own unless a new one starts */
  block.words[10] = (parameters()->ConsoleFlags & 1) | ((flags & 0x210 /* CREATE_NEW_PROCESS_GROUP, not with CREATE_NEW_CONSOLE */) == 0x200 ? 1 : 0);
  block.words[11] = (flags & 0x200) ? 0xFFFFFFFFu : *(ULONG *)((BYTE *)parameters() + 0x408);
  if ((flags & 0x80000 /* EXTENDED_STARTUPINFO_PRESENT */) && startup && startup->cb >= sizeof(STARTUPINFOW) + sizeof(PVOID)) {
    const ProcThreadAttributeList *attributes = *(ProcThreadAttributeList *const *)((const BYTE *)startup + sizeof(STARTUPINFOW));
    if (attributes) {
      const ProcThreadAttributeEntry *entry = attribute_of(attributes, 0x00020000);      /* PARENT_PROCESS */
      if (entry) { block.parent = *(HANDLE *)entry->value; nt_flags |= 0x40; }
      entry = attribute_of(attributes, 0x00020002);      /* HANDLE_LIST */
      if (entry) { block.inherit_list = entry->value; block.inherit_count = (ULONG)(entry->size / sizeof(HANDLE)); nt_flags |= 0x80; }
      entry = attribute_of(attributes, 0x00020016);      /* PSEUDOCONSOLE */
      if (entry) {
        nt_flags |= 0x20;
        if (!(flags & 0x8) && !image_is_gui(image)) { HANDLE own = console_for_pseudo((HANDLE)entry->value); if (own) block.console = own; }
      }
    }
  }
  const NTSTATUS status = NtCreateUserProcess(&process, &thread, &image_name, &command, cwd_pointer, environment ? environment : GetEnvironmentStringsW(), nt_flags, &process_id,
                                              &thread_id, std_in, std_out, std_err, &block);
  if (!NT_SUCCESS(status)) {
    if (status == (NTSTATUS)0xC000007B) set_error(ERROR_BAD_EXE_FORMAT);
    else fail_nt(status);
    return FALSE;
  }
  con_note_child(process_id, block.console, block.words[11] == 0xFFFFFFFFu ? process_id : block.words[11], block.words[10] & 1);
  if (debug_start) {      /* attached before it runs its first instruction; its children too, unless DEBUG_ONLY_THIS_PROCESS */
    const NTSTATUS attached = NtDebugActiveProcess(process, DbgUiGetThreadDebugObject());
    if (NT_SUCCESS(attached) && (flags & 0x1) && !(flags & 0x2)) { ULONG inherit = 1; NtSetInformationProcess(process, 31, &inherit, sizeof(inherit)); }
    if (!NT_SUCCESS(attached)) { NtTerminateProcess(process, 0); NtClose(process); NtClose(thread); fail_nt(attached); return FALSE; }
    if (!(flags & CREATE_SUSPENDED)) { ULONG previous = 0; NtResumeThread(thread, &previous); }
  }
  if (information) {
    information->hProcess = process;
    information->hThread = thread;
    information->dwProcessId = process_id;
    information->dwThreadId = thread_id;
  } else {
    NtClose(process);
    NtClose(thread);
  }
  return TRUE;
}

EXPORT BOOL WINAPI CreateProcessA(LPCSTR application, LPSTR command_line, LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes,
                                  BOOL inherit_handles, DWORD flags, LPVOID environment, LPCSTR directory, LPSTARTUPINFOA startup, LPPROCESS_INFORMATION information) {
  WCHAR *wide_application = application ? to_wide(application) : NULL, *wide_command = command_line ? to_wide(command_line) : NULL, *wide_directory = directory ? to_wide(directory) : NULL;
  WCHAR *wide_environment = NULL, *wide_title = NULL, *wide_desktop = NULL;
  struct { STARTUPINFOW info; LPVOID attributes; } wide_startup;      /* (STARTUPINFOEXW) */
  BOOL ok = FALSE;
  if (startup) {      /* the startup information with wide strings */
    memset(&wide_startup, 0, sizeof(wide_startup));
    memcpy(&wide_startup.info, startup, sizeof(wide_startup.info));
    if (startup->cb >= sizeof(wide_startup)) wide_startup.attributes = ((struct { STARTUPINFOA info; LPVOID attributes; } *)startup)->attributes;
    wide_title = startup->lpTitle ? to_wide(startup->lpTitle) : NULL;
    wide_desktop = startup->lpDesktop ? to_wide(startup->lpDesktop) : NULL;
    wide_startup.info.lpTitle = wide_title;
    wide_startup.info.lpDesktop = wide_desktop;
  }
  if ((application && !wide_application) || (command_line && !wide_command) || (directory && !wide_directory)) goto done;
  if (environment) {      /* "NAME=value\0...\0\0" in the ANSI code page: converted as a whole */
    const char *e = (const char *)environment;
    size_t_ n = 0;
    while (e[n] || e[n + 1]) ++n;
    n += 2;
    const int units = MultiByteToWideChar(CP_UTF8, 0, e, (int)n, NULL, 0);
    wide_environment = RtlAllocateHeap(heap(), 0, (SIZE_T)units * 2 + 2);
    if (!wide_environment) { set_error(ERROR_NOT_ENOUGH_MEMORY); goto done; }
    MultiByteToWideChar(CP_UTF8, 0, e, (int)n, wide_environment, units);
  }
  ok = CreateProcessW(wide_application, wide_command, process_attributes, thread_attributes, inherit_handles, flags | CREATE_UNICODE_ENVIRONMENT, wide_environment, wide_directory, startup ? &wide_startup.info : NULL, information);
done:
  free_wide(wide_application);
  free_wide(wide_command);
  free_wide(wide_directory);
  free_wide(wide_environment);
  free_wide(wide_title);
  free_wide(wide_desktop);
  return ok;
}
EXPORT BOOL WINAPI GetThreadContext(HANDLE h, CONTEXT *context) { const NTSTATUS status = NtGetContextThread(h, context); return NT_SUCCESS(status) ? TRUE : fail_nt(status); }
EXPORT BOOL WINAPI SetThreadContext(HANDLE h, const CONTEXT *context) { const NTSTATUS status = NtSetContextThread(h, context); return NT_SUCCESS(status) ? TRUE : fail_nt(status); }
/* WinExec: the 16-bit-era way to start a program, kept for old callers; uSuccess (>= 32) is the real contract, not a count. */
EXPORT UINT WINAPI WinExec(LPCSTR command_line, UINT show) {
  (void)show;
  char buffer[1024];
  size_t_ i = 0;
  for (; command_line[i] && i + 1 < sizeof(buffer); ++i) buffer[i] = command_line[i];
  buffer[i] = 0;
  STARTUPINFOA startup;
  PROCESS_INFORMATION information;
  memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  if (!CreateProcessA(NULL, buffer, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &information)) return (UINT)GetLastError();
  CloseHandle(information.hThread);
  CloseHandle(information.hProcess);
  return 33;      /* > 31: success, like the real one */
}

/* ---- handles, time --------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL pipe_cancel_pending(HANDLE h, LPOVERLAPPED overlapped);
EXPORT BOOL WINAPI CloseHandle(HANDLE h) {
  if ((ULONG_PTR)h == 0x4B4F54) return TRUE;      /* (advapi32's token "handle": there is nothing behind it) */
  pipe_cancel_pending(h, NULL);      /* (the calls that wait on a pipe handle end with it, and their OVERLAPPEDs are completed before it is gone) */
  port_unlink(h);
  overlapped_forget(h);
  con_alias_forget(h);
  const NTSTATUS status = NtClose(h);
  if (NT_SUCCESS(status)) con_handle_closed(h);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI GetHandleInformation(HANDLE h, LPDWORD flags) {
  BYTE info[2];
  const NTSTATUS status = NtQueryObject(h, 4, info, sizeof(info), NULL);      /* ObjectHandleFlagInformation */
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *flags = (info[0] ? 1 : 0) | (info[1] ? 2 : 0);      /* HANDLE_FLAG_INHERIT, HANDLE_FLAG_PROTECT_FROM_CLOSE */
  return TRUE;
}
EXPORT BOOL WINAPI SetHandleInformation(HANDLE h, DWORD mask, DWORD flags) {
  BYTE info[2];
  NTSTATUS status = NtQueryObject(h, 4, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (mask & 1) info[0] = (flags & 1) ? 1 : 0;
  if (mask & 2) info[1] = (flags & 2) ? 1 : 0;
  status = NtSetInformationObject(h, 4, info, sizeof(info));
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

EXPORT BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *counter) { LARGE_INTEGER f; return NT_SUCCESS(NtQueryPerformanceCounter(counter, &f)); }
EXPORT BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *frequency) { LARGE_INTEGER c; return NT_SUCCESS(NtQueryPerformanceCounter(&c, frequency)); }
EXPORT ULONGLONG WINAPI GetTickCount64(void) { LARGE_INTEGER c, f; NtQueryPerformanceCounter(&c, &f); return (ULONGLONG)c.QuadPart / 10000; }
EXPORT DWORD WINAPI GetTickCount(void) { return (DWORD)GetTickCount64(); }
EXPORT void WINAPI GetSystemTimeAsFileTime(LPFILETIME ft) {
  LARGE_INTEGER t;
  NtQuerySystemTime(&t);
  ft->dwLowDateTime = t.LowPart;
  ft->dwHighDateTime = (DWORD)t.HighPart;
}
EXPORT void WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME ft) { GetSystemTimeAsFileTime(ft); }

/* FILETIME (100 ns since 1601) <-> calendar, in the proleptic Gregorian calendar (days from civil, after Howard Hinnant). */
static LONGLONG days_from_civil(LONGLONG y, LONGLONG m, LONGLONG d) {
  y -= m <= 2;
  const LONGLONG era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
EXPORT BOOL WINAPI FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st) {
  const ULONGLONG t = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
  if (t >= 0x8000000000000000ull) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const ULONGLONG ms_total = t / 10000, seconds = ms_total / 1000;
  const LONGLONG days = (LONGLONG)(seconds / 86400) - 134774;      /* since 1970 (the epoch of 1601 is 134774 days earlier) */
  const LONGLONG z = days + 719468, era = z / 146097, doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const LONGLONG doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  st->wDay = (WORD)(doy - (153 * mp + 2) / 5 + 1);
  st->wMonth = (WORD)(mp < 10 ? mp + 3 : mp - 9);
  st->wYear = (WORD)(yoe + era * 400 + (st->wMonth <= 2));
  st->wDayOfWeek = (WORD)((ULONGLONG)((seconds / 86400) + 1) % 7);      /* 1601-01-01 was a Monday */
  const ULONGLONG rest = seconds % 86400;
  st->wHour = (WORD)(rest / 3600);
  st->wMinute = (WORD)(rest % 3600 / 60);
  st->wSecond = (WORD)(rest % 60);
  st->wMilliseconds = (WORD)(ms_total % 1000);
  return TRUE;
}
EXPORT BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft) {
  if (st->wYear < 1601 || st->wYear > 30827 || st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > month_days(st->wYear, st->wMonth) || st->wHour > 23 || st->wMinute > 59 || st->wSecond > 59 || st->wMilliseconds > 999) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const LONGLONG days = days_from_civil(st->wYear, st->wMonth, st->wDay) + 134774;
  const ULONGLONG ms = (ULONGLONG)days * 86400000ull + (ULONGLONG)st->wHour * 3600000ull + (ULONGLONG)st->wMinute * 60000ull + (ULONGLONG)st->wSecond * 1000ull + st->wMilliseconds;
  const ULONGLONG t = ms * 10000;
  ft->dwLowDateTime = (DWORD)t;
  ft->dwHighDateTime = (DWORD)(t >> 32);
  return TRUE;
}
EXPORT void WINAPI GetSystemTime(LPSYSTEMTIME st) { FILETIME ft; GetSystemTimeAsFileTime(&ft); FileTimeToSystemTime(&ft, st); }
/* Exception handling lives in ntdll.dll (src/Windows/ntdll/except.inc); kernel32 gives programs the names they know. RtlUnwindEx, RtlVirtualUnwind and the others
 * are forwarded to ntdll by kernel32.def. */
EXPORT LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) { return RtlSetUnhandledExceptionFilter(filter); }
EXPORT LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS info) {
  LPTOP_LEVEL_EXCEPTION_FILTER filter = RtlSetUnhandledExceptionFilter(NULL);      /* (read it, put it back) */
  RtlSetUnhandledExceptionFilter(filter);
  if (info && info->ExceptionRecord && (info->ExceptionRecord->ExceptionFlags & EXCEPTION_NESTED_CALL)) return EXCEPTION_CONTINUE_SEARCH;
  ULONG_PTR port = 0;
  NtQueryInformationProcess(NtCurrentProcess(), 7 /* ProcessDebugPort */, &port, sizeof(port), NULL);
  if (port) return EXCEPTION_CONTINUE_SEARCH;      /* (the debugger gets the second chance) */
  if (filter) { const LONG answer = filter(info); if (answer != EXCEPTION_CONTINUE_SEARCH) return answer; }
  return EXCEPTION_EXECUTE_HANDLER;
}
EXPORT void WINAPI RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments) {
  EXCEPTION_RECORD record;
  memset(&record, 0, sizeof(record));
  record.ExceptionCode = code;
  record.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
  if (arguments && count > 15) count = 15;
  record.NumberParameters = arguments ? count : 0;
  for (DWORD i = 0; i < record.NumberParameters; ++i) record.ExceptionInformation[i] = arguments[i];
  RtlRaiseException(&record);
}
EXPORT PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVOID handler) { return RtlAddVectoredExceptionHandler(first, handler); }
EXPORT ULONG WINAPI RemoveVectoredExceptionHandler(PVOID handler) { return RtlRemoveVectoredExceptionHandler(handler); }

/* ---- the console and standard handles ------------------------------------------------------------------------------------------------------------------ */

static HANDLE g_std_override[3];
static BOOL g_std_overridden[3];
EXPORT HANDLE WINAPI GetStdHandle(DWORD which) {
  const int i = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
  if (i < 0) { set_error(ERROR_INVALID_HANDLE); return INVALID_HANDLE_VALUE; }
  if (g_std_overridden[i]) return g_std_override[i];
  RTL_USER_PROCESS_PARAMETERS *p = parameters();
  return i == 0 ? p->StandardInput : i == 1 ? p->StandardOutput : p->StandardError;
}
EXPORT BOOL WINAPI SetStdHandle(DWORD which, HANDLE h) {
  const int i = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
  if (i < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  RTL_USER_PROCESS_PARAMETERS *p = parameters();      /* (the standard handles live in the process parameters, where the console functions look too) */
  if (i == 0) p->StandardInput = h; else if (i == 1) p->StandardOutput = h; else p->StandardError = h;
  g_std_override[i] = h;
  g_std_overridden[i] = FALSE;
  return TRUE;
}

/* FILE_DEVICE_CONSOLE (0x50) or a disk? (How GetFileType tells a console from a file.) */
static int device_type(HANDLE h) {
  IO_STATUS_BLOCK iosb;
  BYTE info[8];
  if (!NT_SUCCESS(NtQueryVolumeInformationFile(h, &iosb, info, sizeof(info), 4))) return -1;
  return info[0] | (info[1] << 8);
}
EXPORT DWORD WINAPI GetFileType(HANDLE h) {
  if ((ULONG_PTR)h >= 0xFFFFFFF4u && (ULONG_PTR)h <= 0xFFFFFFF6u) h = GetStdHandle((DWORD)(ULONG_PTR)h);      /* STD_INPUT_HANDLE and the others */
  const int type = device_type(h);
  if (con_unbound_kind(h)) { set_error(ERROR_SUCCESS); return FILE_TYPE_CHAR; }      /* a handle of the console that is an event */
  if (type < 0) { set_error(ERROR_INVALID_HANDLE); return FILE_TYPE_UNKNOWN; }
  set_error(ERROR_SUCCESS);
  if (type == 0x0c) return FILE_TYPE_UNKNOWN;      /* a mailslot */
  return (type == 0x50 || type == 0x15) ? FILE_TYPE_CHAR : type == 0x11 ? FILE_TYPE_PIPE : FILE_TYPE_DISK;
}

/* A buffer for a file on a disk has to be usable as a whole before anything is moved (a call never does half of its work for that). The handle is checked first. */
static BOOL disk_buffer_usable(HANDLE h, const void *buffer, DWORD length, BOOL for_reading, DWORD error) {
  if (length == 0 || range_allowed(buffer, length, for_reading)) return TRUE;
  const int type = device_type(h);
  if (type < 0) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (type == 0x50 || type == 0x15 || type == 0x11 || type == 0x0c) return TRUE;      /* (a console, a pipe or a mailslot answers for itself) */
  set_error(error);
  return FALSE;
}
EXPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPOVERLAPPED overlapped) {
  if (written) *written = 0;
  if (con_dead_handle(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (!disk_buffer_usable(h, buffer, length, FALSE, ERROR_INVALID_USER_BUFFER)) return FALSE;
  if (!overlapped) {      /* (probing for the console must not leave its own error behind: a write that works leaves the last error alone) */
    const DWORD saved = TEB_LAST_ERROR(NtCurrentTeb());
    BOOL handled;
    const BOOL result = console_write_hook(h, buffer, length, written, &handled);
    if (!handled || result) set_error(saved);
    if (handled) return result;
  }
  if (overlapped) {
    ULONG_PTR information = 0;
    NTSTATUS result = 0;
    const int started = pipe_begin(1, h, (LPVOID)buffer, length, overlapped, NULL, &information, &result, NULL, 0);      /* 1: waiting, 2: it went at once, 3: it failed */
    if (started == 1) { if (written) *written = 0; return FALSE; }
    if (started == 2) {
      complete_overlapped(overlapped, result, information);
      port_note(h, overlapped, information);
      if (written) *written = (DWORD)information;
      return TRUE;
    }
    if (started == 3) { overlapped->Internal = (ULONG_PTR)STATUS_PENDING; return fail_nt(result); }
  }
  IO_STATUS_BLOCK iosb;
  LARGE_INTEGER offset;
  PLARGE_INTEGER pointer = NULL;
  if (overlapped) {
    offset.LowPart = overlapped->Offset;
    offset.HighPart = (LONG)overlapped->OffsetHigh;
    pointer = &offset;
    overlapped->Internal = (ULONG_PTR)STATUS_PENDING;
  }
  if (written) *written = 0;
  iosb.Information = 0;
  const NTSTATUS status = NtWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)buffer, length, pointer, NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);      /* (a call that fails at once leaves the OVERLAPPED as it set it: still pending) */
  complete_overlapped(overlapped, status, iosb.Information);
  port_note(h, overlapped, iosb.Information);
  if (overlapped && unbuffered_known(h)) { set_error(ERROR_IO_PENDING); return FALSE; }      /* (a call that goes to the disk is asynchronous: the result is in the OVERLAPPED) */
  if (written) *written = (DWORD)iosb.Information;
  return TRUE;
}
/* An asynchronous operation's result is in the OVERLAPPED: Internal is its status (STATUS_PENDING until it is done), InternalHigh the bytes moved. While it is pending the
 * call may wait for the event (or the file) to be signalled; GetOverlappedResultEx also waits on a completed one unless the low bit of the file handle is set. */
static BOOL overlapped_result(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred, DWORD timeout, BOOL alertable, BOOL ex) {
  NTSTATUS status = (NTSTATUS)overlapped->Internal;
  if (status == STATUS_PENDING && !timeout) { set_error(ERROR_IO_INCOMPLETE); return FALSE; }
  const BOOL no_block = ex && ((ULONG_PTR)file & 1);      /* (the Ex call only looks, without waiting, when the low bit of the file handle is set) */
  if (timeout && (status == STATUS_PENDING || (ex && !no_block))) {
    const DWORD waited = WaitForSingleObjectEx(overlapped->hEvent ? overlapped->hEvent : file, no_block ? 0 : timeout, alertable);
    if (waited == WAIT_FAILED) return FALSE;
    if (waited == WAIT_IO_COMPLETION || (waited == WAIT_TIMEOUT && !no_block)) { set_error(waited); return FALSE; }
    status = (NTSTATUS)overlapped->Internal;
  }
  if (transferred) *transferred = (DWORD)overlapped->InternalHigh;
  set_error(RtlNtStatusToDosError(status));
  return status == STATUS_SUCCESS || status == STATUS_PENDING;
}
EXPORT BOOL WINAPI GetOverlappedResultEx(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred, DWORD timeout, BOOL alertable) { return overlapped_result(file, overlapped, transferred, timeout, alertable, TRUE); }
EXPORT BOOL WINAPI GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait) {
  if (!overlapped) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if ((NTSTATUS)overlapped->Internal != STATUS_PENDING) return overlapped_result(file, overlapped, transferred, 0, FALSE, FALSE);      /* (a finished operation is not waited for) */
  return overlapped_result(file, overlapped, transferred, wait ? INFINITE : 0, FALSE, FALSE);
}

/* ---- pipes ------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI CreatePipe(PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES sa, DWORD size) {
  OBJECT_ATTRIBUTES attributes;
  POBJECT_ATTRIBUTES pointer = NULL;
  if (sa && sa->bInheritHandle) {
    memset(&attributes, 0, sizeof(attributes));
    attributes.Length = sizeof(attributes);
    attributes.Attributes = 2;      /* OBJ_INHERIT */
    pointer = &attributes;
  }
  const NTSTATUS status = NtCreatePipe(read, write, pointer, size);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* "\\.\pipe\name" as the NT name "\??\pipe\name". */
static BOOL pipe_nt_name(LPCWSTR name, WCHAR *buffer, UNICODE_STRING *us, OBJECT_ATTRIBUTES *attributes, BOOL inherit) {
  static const WCHAR prefix[] = {'\\', '\\', '.', '\\', 'p', 'i', 'p', 'e', '\\', 0};
  static const WCHAR nt_prefix[] = {'\\', '?', '?', '\\', 'p', 'i', 'p', 'e', '\\', 0};
  size_t_ i = 0;
  while (prefix[i] && (name[i] == prefix[i] || (name[i] | 0x20) == (prefix[i] | 0x20))) ++i;
  if (prefix[i]) { set_error(ERROR_INVALID_NAME); return FALSE; }
  const size_t_ rest = wcslen(name + i);
  if (rest > 250) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
  memcpy(buffer, nt_prefix, 9 * 2);
  memcpy(buffer + 9, name + i, (rest + 1) * 2);
  us->Buffer = buffer;
  us->Length = (USHORT)((9 + rest) * 2);
  us->MaximumLength = (USHORT)(us->Length + 2);
  attributes->Length = sizeof(OBJECT_ATTRIBUTES);
  attributes->RootDirectory = NULL;
  attributes->ObjectName = us;
  attributes->Attributes = 0x40 | (inherit ? 2 : 0);
  attributes->SecurityDescriptor = attributes->SecurityQualityOfService = NULL;
  return TRUE;
}

EXPORT HANDLE WINAPI CreateNamedPipeW(LPCWSTR name, DWORD open_mode, DWORD pipe_mode, DWORD max_instances, DWORD out_size, DWORD in_size, DWORD timeout_ms, LPSECURITY_ATTRIBUTES sa) {
  WCHAR buffer[300];
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES attributes;
  if (!name) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  if (!pipe_nt_name(name, buffer, &us, &attributes, sa && sa->bInheritHandle)) return INVALID_HANDLE_VALUE;
  if ((pipe_mode & 2) && !(pipe_mode & 4)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }      /* (a byte pipe cannot be read as messages) */
  /* PIPE_ACCESS_INBOUND 1 (the server reads: clients may write), OUTBOUND 2, DUPLEX 3; the NT share access says what clients may do */
  const ULONG share = (open_mode & 3) == 1 ? 2 : (open_mode & 3) == 2 ? 1 : (open_mode & 3) == 3 ? 3 : 0;
  if (share == 0) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  LARGE_INTEGER timeout;
  timeout.QuadPart = -10000ll * (timeout_ms ? timeout_ms : 50);
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtCreateNamedPipeFile(&h, 0xC0100000u /* GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE */, &attributes, &iosb, share, 3 /* FILE_OPEN_IF */, 0x20,
                                                (pipe_mode & 4) ? 1 : 0, (pipe_mode & 2) ? 1 : 0, (pipe_mode & 1) ? 1 : 0, max_instances == 255 ? 0xFFFFFFFFu : max_instances,
                                                in_size, out_size, &timeout);
  if (!NT_SUCCESS(status)) { fail_nt(status); return INVALID_HANDLE_VALUE; }
  return h;
}
EXPORT HANDLE WINAPI CreateNamedPipeA(LPCSTR name, DWORD open_mode, DWORD pipe_mode, DWORD max_instances, DWORD out_size, DWORD in_size, DWORD timeout_ms, LPSECURITY_ATTRIBUTES sa) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (!w) { if (name) return INVALID_HANDLE_VALUE; set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  HANDLE h = CreateNamedPipeW(w, open_mode, pipe_mode, max_instances, out_size, in_size, timeout_ms, sa);
  free_wide(w);
  return h;
}

#define FSCTL_PIPE_DISCONNECT_ 0x110004
#define FSCTL_PIPE_LISTEN_ 0x110008
#define FSCTL_PIPE_PEEK_ 0x11400C
#define FSCTL_PIPE_WAIT_ 0x110018
#define FSCTL_PIPE_TRANSCEIVE_ 0x11C017

/* An overlapped call on a named pipe that has to wait (nothing to read yet, nobody connected yet, a write that does not fit) is an operation of the pipe in the kernel,
 * started by the calling thread (a write is queued there, whole, at once: the other end sees its bytes) and waited for by a thread of its own, as the kernel's calls are
 * synchronous; the thread completes the OVERLAPPED (and so its event) when the operation is over, then says so (ACK) - whoever finished the operation does not return before
 * that, so the result is there when its call returns. A call that can be answered at once is done in line, as without a thread. */
typedef struct { int op; HANDLE pipe; LPVOID buffer; DWORD length; LPOVERLAPPED overlapped; LPOVERLAPPED_COMPLETION_ROUTINE_ routine; HANDLE caller; int slot; DWORD tag; } PipeRequest;      /* op: 0 read, 1 write, 2 connect */
/* The operations waiting in threads, so that CancelIo can end them by their tags. */
#define PIPE_PENDING_MAX 32
#define FSCTL_PIPE_BEGIN_READ_ 0x11FFF0
#define FSCTL_PIPE_BEGIN_WRITE_ 0x11FFF1
#define FSCTL_PIPE_BEGIN_LISTEN_ 0x11FFF2
#define FSCTL_PIPE_AWAIT_ 0x11FFF3
#define FSCTL_PIPE_ACK_ 0x11FFF4
#define FSCTL_PIPE_CANCEL_ 0x11FFF5
#define FSCTL_PIPE_BEGIN_TRANSACT_ 0x11FFF6
static struct { HANDLE pipe; LPOVERLAPPED overlapped; DWORD tag; volatile LONG used; } g_pipe_pending[PIPE_PENDING_MAX];
static void atom_lock(void);
static CRITICAL_SECTION g_atom_lock;      /* (defined in extra3.inc; a tentative definition here) */
static DWORD WINAPI pipe_request_thread(LPVOID parameter) {
  PipeRequest request = *(PipeRequest *)parameter;
  RtlFreeHeap(heap(), 0, parameter);
  IO_STATUS_BLOCK iosb;
  iosb.Information = 0;
  const BYTE no_event = request.overlapped->hEvent ? 0 : 1;      /* (the OVERLAPPED may be gone once it is completed) */
  NTSTATUS status = NtFsControlFile(request.pipe, NULL, NULL, NULL, &iosb, FSCTL_PIPE_AWAIT_, &request.tag, 4, request.op == 0 ? request.buffer : NULL, request.op == 0 ? request.length : 0);
  if (status == STATUS_PENDING) status = STATUS_SUCCESS;
  if (request.routine) {      /* ReadFileEx/WriteFileEx: the routine is queued before the event is set, so that a wait that ends on the event has already run it */
    request.overlapped->Internal = (ULONG_PTR)(ULONG)status;
    request.overlapped->InternalHigh = iosb.Information;
    NtQueueApcThread(request.caller, (PVOID)completion_apc, (ULONG_PTR)request.routine, (ULONG_PTR)request.overlapped, 0);
    NtClose(request.caller);
    if (request.overlapped->hEvent) NtSetEvent((HANDLE)((ULONG_PTR)request.overlapped->hEvent & ~(ULONG_PTR)1), NULL);
  } else {
    complete_overlapped(request.overlapped, status, iosb.Information);
    port_note(request.pipe, request.overlapped, iosb.Information);
  }
  g_pipe_pending[request.slot].used = 0;
  BYTE ack[5] = {(BYTE)request.tag, (BYTE)(request.tag >> 8), (BYTE)(request.tag >> 16), (BYTE)(request.tag >> 24), no_event};
  NtFsControlFile(request.pipe, NULL, NULL, NULL, &iosb, FSCTL_PIPE_ACK_, ack, 5, NULL, 0);
  return 0;
}
/* 0 if the call is to be made the usual way (nothing was done); 1 if it waits in a thread (ERROR_IO_PENDING is set); for a write also 2 if it went at once (*information
 * bytes) and 3 if it failed (*result). */
static int pipe_begin(int op, HANDLE h, LPVOID buffer, DWORD length, LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE_ routine, ULONG_PTR *information, NTSTATUS *result, LPVOID in, DWORD in_size) {
  if (!overlapped || device_type(h) != 0x11) return 0;
  PipeRequest *request = RtlAllocateHeap(heap(), 0, sizeof(*request));
  if (!request) return 0;
  int slot = -1;
  atom_lock();
  for (int n = 0; n < PIPE_PENDING_MAX; ++n) if (!g_pipe_pending[n].used) { g_pipe_pending[n].used = 1; g_pipe_pending[n].pipe = h; g_pipe_pending[n].overlapped = overlapped; g_pipe_pending[n].tag = 0; slot = n; break; }
  LeaveCriticalSection(&g_atom_lock);
  if (slot < 0) { RtlFreeHeap(heap(), 0, request); return 0; }
  IO_STATUS_BLOCK iosb;
  DWORD tag = 0;
  iosb.Information = 0;
  NTSTATUS status;
  if (op == 0) status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_BEGIN_READ_, &length, 4, &tag, 4);
  else if (op == 1) status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_BEGIN_WRITE_, buffer, length, &tag, 4);
  else if (op == 3) { tag = length; status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_BEGIN_TRANSACT_, in, in_size, &tag, 4); }      /* (the read length goes in, the tag comes out) */
  else status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_BEGIN_LISTEN_, NULL, 0, &tag, 4);
  if (status != STATUS_PENDING) {      /* answered at once (or not an operation at all) */
    g_pipe_pending[slot].used = 0;
    RtlFreeHeap(heap(), 0, request);
    if ((op != 1 && op != 3) || status == (NTSTATUS)0xC00000BBu) return 0;
    *information = iosb.Information;
    *result = status;
    return NT_SUCCESS(status) ? 2 : 3;
  }
  g_pipe_pending[slot].tag = tag;
  request->slot = slot;
  request->op = op == 3 ? 0 : op; request->pipe = h; request->buffer = buffer; request->length = length; request->overlapped = overlapped; request->routine = routine; request->caller = NULL; request->tag = tag;
  overlapped->Internal = (ULONG_PTR)STATUS_PENDING;
  if (overlapped->hEvent) NtResetEvent((HANDLE)((ULONG_PTR)overlapped->hEvent & ~(ULONG_PTR)1), NULL);
  HANDLE thread = NULL;
  if (!routine || DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &request->caller, 0, FALSE, 2 /* DUPLICATE_SAME_ACCESS */)) thread = CreateThread(NULL, 0, pipe_request_thread, request, 0, NULL);
  if (!thread) {      /* no thread to wait for it: the operation is given up */
    if (request->caller) NtClose(request->caller);
    NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_CANCEL_, &tag, 4, NULL, 0);
    NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_ACK_, &tag, 4, NULL, 0);
    g_pipe_pending[slot].used = 0;
    RtlFreeHeap(heap(), 0, request);
    *result = (NTSTATUS)0xC000009Au;
    return op == 1 || op == 3 ? 3 : 0;
  }
  NtClose(thread);
  set_error(997);      /* ERROR_IO_PENDING */
  return 1;
}
static BOOL pipe_start_overlapped(int op, HANDLE h, LPVOID buffer, DWORD length, LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE_ routine) {
  ULONG_PTR information = 0;
  NTSTATUS result = 0;
  return pipe_begin(op, h, buffer, length, overlapped, routine, &information, &result, NULL, 0) == 1;
}
EXPORT BOOL WINAPI ConnectNamedPipe(HANDLE h, LPOVERLAPPED overlapped) {
  if (pipe_start_overlapped(2, h, NULL, 0, overlapped, NULL)) return FALSE;
  IO_STATUS_BLOCK iosb;
  if (overlapped) overlapped->Internal = (ULONG_PTR)STATUS_PENDING;
  const NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_LISTEN_, NULL, 0, NULL, 0);
  if (NT_SUCCESS(status)) complete_overlapped(overlapped, status, 0);      /* (a call that fails, such as one on a pipe already connected, leaves the OVERLAPPED pending) */
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI DisconnectNamedPipe(HANDLE h) {
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_DISCONNECT_, NULL, 0, NULL, 0);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI PeekNamedPipe(HANDLE h, LPVOID buffer, DWORD size, LPDWORD read, LPDWORD available, LPDWORD left) {
  IO_STATUS_BLOCK iosb;
  BYTE stack[16 + 512];
  BYTE *out = size <= 512 ? stack : NULL;
  if (!out) out = RtlAllocateHeap(heap(), 0, 16 + size);
  if (!out) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  const NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_PEEK_, NULL, 0, out, 16 + size);
  BOOL ok = NT_SUCCESS(status);
  if (ok) {
    const DWORD got = (DWORD)iosb.Information > 16 ? (DWORD)iosb.Information - 16 : 0;
    if (buffer && got) memcpy(buffer, out + 16, got);
    if (read) *read = got;
    if (available) *available = get32(out + 4);
    if (left) *left = get32(out + 12) - got;      /* (MessageLength: 0 for a byte pipe, so this is minus what was copied) */
  } else {
    fail_nt(status);
  }
  if (out != stack) RtlFreeHeap(heap(), 0, out);
  return ok;
}
EXPORT BOOL WINAPI TransactNamedPipe(HANDLE h, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD read, LPOVERLAPPED overlapped) {
  if (overlapped) {      /* the message goes at once; the answer is awaited in a thread */
    ULONG_PTR information = 0;
    NTSTATUS result = 0;
    const int started = pipe_begin(3, h, out, out_size, overlapped, NULL, &information, &result, in, in_size);
    if (started == 1) return FALSE;
    if (started == 3) return fail_nt(result);
  }
  IO_STATUS_BLOCK iosb;
  iosb.Information = 0;
  const NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_TRANSCEIVE_, in, in_size, out, out_size);
  complete_overlapped(overlapped, status, iosb.Information);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (read) *read = (DWORD)iosb.Information;
  return TRUE;
}
EXPORT BOOL WINAPI WaitNamedPipeW(LPCWSTR name, DWORD timeout_ms) {
  WCHAR buffer[300];
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES attributes;
  if (!pipe_nt_name(name, buffer, &us, &attributes, FALSE)) return FALSE;
  const WCHAR *leaf = buffer + 9;
  const size_t_ n = wcslen(leaf);
  /* the pipe folder: \??\pipe\ */
  UNICODE_STRING folder;
  OBJECT_ATTRIBUTES folder_attributes = attributes;
  folder.Buffer = buffer;
  folder.Length = 9 * 2;
  folder.MaximumLength = 9 * 2 + 2;
  folder_attributes.ObjectName = &folder;
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  NTSTATUS status = NtOpenFile(&h, 0x80100000u, &folder_attributes, &iosb, 3, 0x20);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  BYTE request[20 + 2 * 256];
  memset(request, 0, 20);
  LARGE_INTEGER timeout;
  /* NMPWAIT_USE_DEFAULT_WAIT 0 (the pipe's own), NMPWAIT_WAIT_FOREVER 0xFFFFFFFF */
  timeout.QuadPart = timeout_ms == 0xFFFFFFFFu ? (LONGLONG)0x8000000000000000ull : -10000ll * timeout_ms;
  memcpy(request, &timeout, 8);
  const DWORD name_bytes = (DWORD)(n * 2);
  memcpy(request + 8, &name_bytes, 4);
  request[12] = timeout_ms != 0;      /* TimeoutSpecified */
  memcpy(request + 20, leaf, n * 2);
  status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_WAIT_, request, 20 + name_bytes, NULL, 0);
  NtClose(h);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI WaitNamedPipeA(LPCSTR name, DWORD timeout_ms) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (!w) { if (!name) set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const BOOL ok = WaitNamedPipeW(w, timeout_ms);
  free_wide(w);
  return ok;
}
EXPORT BOOL WINAPI SetNamedPipeHandleState(HANDLE h, LPDWORD mode, LPDWORD collect, LPDWORD timeout);
EXPORT BOOL WINAPI CallNamedPipeW(LPCWSTR name, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD read, DWORD timeout_ms) {
  HANDLE h;
  for (;;) {
    h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) break;
    if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name, timeout_ms)) return FALSE;
  }
  DWORD mode = 2 /* PIPE_READMODE_MESSAGE */;      /* (the answer is a message) */
  const BOOL ok = SetNamedPipeHandleState(h, &mode, NULL, NULL) && TransactNamedPipe(h, in, in_size, out, out_size, read, NULL);
  const DWORD error = GetLastError();
  CloseHandle(h);
  if (!ok) set_error(error);
  return ok;
}
EXPORT BOOL WINAPI CallNamedPipeA(LPCSTR name, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD read, DWORD timeout_ms) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (!w) { if (!name) set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const BOOL ok = CallNamedPipeW(w, in, in_size, out, out_size, read, timeout_ms);
  free_wide(w);
  return ok;
}
EXPORT BOOL WINAPI GetNamedPipeInfo(HANDLE h, LPDWORD flags, LPDWORD out_size, LPDWORD in_size, LPDWORD max_instances) {
  IO_STATUS_BLOCK iosb;
  BYTE info[40];
  const NTSTATUS status = NtQueryInformationFile(h, &iosb, info, sizeof(info), 24);      /* FilePipeLocalInformation */
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (flags) *flags = (get32(info + 36) ? 1 : 0) | (get32(info) ? 4 : 0);      /* PIPE_SERVER_END 1 (client 0); PIPE_TYPE_MESSAGE 4 */
  if (out_size) *out_size = get32(info + 24);
  if (in_size) *in_size = get32(info + 16);
  if (max_instances) *max_instances = get32(info + 8) >= 255 ? 255 : get32(info + 8);
  return TRUE;
}
EXPORT BOOL WINAPI GetNamedPipeHandleStateW(HANDLE h, LPDWORD state, LPDWORD instances, LPDWORD collect, LPDWORD timeout, LPWSTR user, DWORD user_size) {
  IO_STATUS_BLOCK iosb;
  BYTE mode[8], info[40];
  if (collect || timeout || (user && user_size)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }      /* (these mean nothing for a pipe on this machine) */
  NTSTATUS status = NtQueryInformationFile(h, &iosb, mode, sizeof(mode), 23);      /* FilePipeInformation */
  if (!NT_SUCCESS(status)) return fail_nt(status);
  status = NtQueryInformationFile(h, &iosb, info, sizeof(info), 24);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (state) *state = get32(mode + 4) ? 1 : 0;      /* PIPE_NOWAIT */
  if (instances) *instances = get32(info + 12);
  if (collect) *collect = 0;
  if (timeout) *timeout = 0;
  if (user && user_size) user[0] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetNamedPipeHandleStateA(HANDLE h, LPDWORD state, LPDWORD instances, LPDWORD collect, LPDWORD timeout, LPSTR user, DWORD user_size) {
  return GetNamedPipeHandleStateW(h, state, instances, collect, timeout, (LPWSTR)user, user_size);
}
/* The processes at the two ends of a pipe: the kernel keeps them with the instance (`which` 0: the client's, 1: the server's). */
static BOOL pipe_process_of(HANDLE h, DWORD which, ULONG *pid) {
  if (!pid) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  IO_STATUS_BLOCK iosb;
  DWORD out = 0;
  const NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, 0x11FFF7, &which, 4, &out, 4);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *pid = out;
  return TRUE;
}
EXPORT BOOL WINAPI ProcessIdToSessionId(DWORD pid, DWORD *session);
EXPORT BOOL WINAPI GetNamedPipeClientProcessId(HANDLE h, PULONG pid) { return pipe_process_of(h, 0, pid); }
EXPORT BOOL WINAPI GetNamedPipeServerProcessId(HANDLE h, PULONG pid) { return pipe_process_of(h, 1, pid); }
EXPORT BOOL WINAPI GetNamedPipeClientSessionId(HANDLE h, PULONG session) {
  ULONG pid = 0;
  if (!session) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  return pipe_process_of(h, 0, &pid) && ProcessIdToSessionId(pid, (DWORD *)session);
}
/* (There is one session, so the server's is the client's.) */
EXPORT BOOL WINAPI GetNamedPipeServerSessionId(HANDLE h, PULONG session) {
  ULONG pid = 0;
  if (!session) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  return pipe_process_of(h, 1, &pid) && ProcessIdToSessionId(pid, (DWORD *)session);
}
/* Every client is on this machine: the call fails with ERROR_PIPE_LOCAL (and leaves the buffer alone), or with ERROR_INSUFFICIENT_BUFFER if there is no room for a name. */
EXPORT BOOL WINAPI GetNamedPipeClientComputerNameW(HANDLE h, LPWSTR name, ULONG length) {
  (void)h; (void)name;
  set_error(length == 0 ? ERROR_INSUFFICIENT_BUFFER : 229 /* ERROR_PIPE_LOCAL */);
  return FALSE;
}
EXPORT BOOL WINAPI GetNamedPipeClientComputerNameA(HANDLE h, LPSTR name, ULONG length) { return GetNamedPipeClientComputerNameW(h, (LPWSTR)name, length); }
EXPORT BOOL WINAPI SetNamedPipeHandleState(HANDLE h, LPDWORD mode, LPDWORD collect, LPDWORD timeout) {
  (void)collect; (void)timeout;
  if (!mode) return TRUE;
  IO_STATUS_BLOCK iosb;
  BYTE info[8] = {0};
  const DWORD completion = (*mode & 1) ? 1 : 0;      /* PIPE_NOWAIT: reads and writes fail instead of waiting */
  const DWORD read_mode = (*mode & 2) ? 1 : 0;      /* PIPE_READMODE_MESSAGE */
  memcpy(info, &read_mode, 4);
  memcpy(info + 4, &completion, 4);
  const NTSTATUS status = NtSetInformationFile(h, &iosb, info, sizeof(info), 23);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* CreateMailslot/GetMailslotInfo/SetMailslotInfo: the one reader (NtCreateMailslotFile). A writer is just CreateFile(\\.\mailslot\name) - an ordinary file service,
 * like a named pipe's client end, needing no wrapper of its own here. MAILSLOT_WAIT_FOREVER (0xFFFFFFFF ms) is a null NT ReadTimeout (wait forever); any other
 * millisecond value becomes a relative, negative 100ns one, the convention every other timeout in this OS's NT layer already uses. */
#define MAILSLOT_WAIT_FOREVER_ 0xFFFFFFFFu
static BOOL mailslot_nt_name(LPCWSTR name, WCHAR *buffer, UNICODE_STRING *us, OBJECT_ATTRIBUTES *attributes, BOOL inherit) {
  static const WCHAR prefix[] = {'\\', '\\', '.', '\\', 'm', 'a', 'i', 'l', 's', 'l', 'o', 't', '\\', 0};
  static const WCHAR nt_prefix[] = {'\\', '?', '?', '\\', 'm', 'a', 'i', 'l', 's', 'l', 'o', 't', '\\', 0};
  size_t_ i = 0;
  while (prefix[i] && (name[i] == prefix[i] || (name[i] | 0x20) == (prefix[i] | 0x20))) ++i;
  if (prefix[i]) { set_error(ERROR_INVALID_NAME); return FALSE; }
  const size_t_ rest = wcslen(name + i);
  if (rest > 250) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
  memcpy(buffer, nt_prefix, 13 * 2);
  memcpy(buffer + 13, name + i, (rest + 1) * 2);
  us->Buffer = buffer;
  us->Length = (USHORT)((13 + rest) * 2);
  us->MaximumLength = (USHORT)(us->Length + 2);
  attributes->Length = sizeof(OBJECT_ATTRIBUTES);
  attributes->RootDirectory = NULL;
  attributes->ObjectName = us;
  attributes->Attributes = 0x40 | (inherit ? 2 : 0);
  attributes->SecurityDescriptor = attributes->SecurityQualityOfService = NULL;
  return TRUE;
}
EXPORT HANDLE WINAPI CreateMailslotW(LPCWSTR name, DWORD max_message_size, DWORD read_timeout_ms, LPSECURITY_ATTRIBUTES sa) {
  WCHAR buffer[300];
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES attributes;
  if (!name) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  if (!mailslot_nt_name(name, buffer, &us, &attributes, sa && sa->bInheritHandle)) return INVALID_HANDLE_VALUE;
  LARGE_INTEGER timeout;
  const BOOL forever = read_timeout_ms == MAILSLOT_WAIT_FOREVER_;
  timeout.QuadPart = forever ? -1 : (read_timeout_ms ? -10000ll * (LONGLONG)read_timeout_ms : 0);
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtCreateMailslotFile(&h, 0xC0100000u /* GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE */, &attributes, &iosb, 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */, 0, max_message_size,
                                               forever ? NULL : &timeout);
  if (!NT_SUCCESS(status)) { fail_nt(status); return INVALID_HANDLE_VALUE; }
  return h;
}
EXPORT HANDLE WINAPI CreateMailslotA(LPCSTR name, DWORD max_message_size, DWORD read_timeout_ms, LPSECURITY_ATTRIBUTES sa) {
  WCHAR *w = name ? to_wide(name) : NULL;
  if (!w) { if (name) return INVALID_HANDLE_VALUE; set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  HANDLE h = CreateMailslotW(w, max_message_size, read_timeout_ms, sa);
  free_wide(w);
  return h;
}
EXPORT BOOL WINAPI GetMailslotInfo(HANDLE h, LPDWORD max_message_size, LPDWORD next_size, LPDWORD message_count, LPDWORD read_timeout_ms) {
  IO_STATUS_BLOCK iosb;
  BYTE info[24];
  const NTSTATUS status = NtQueryInformationFile(h, &iosb, info, sizeof(info), 26 /* FileMailslotQueryInformation */);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (max_message_size) *max_message_size = get32(info);
  if (next_size) *next_size = get32(info + 8);
  if (message_count) *message_count = get32(info + 12);
  if (read_timeout_ms) {
    const ULONGLONG hundred_ns = *(const ULONGLONG *)(info + 16);      /* (a count of 100 ns; all ones is forever) */
    *read_timeout_ms = hundred_ns == ~0ull ? MAILSLOT_WAIT_FOREVER_ : (DWORD)(hundred_ns / 10000);
  }
  return TRUE;
}
EXPORT BOOL WINAPI SetMailslotInfo(HANDLE h, DWORD read_timeout_ms) {
  IO_STATUS_BLOCK iosb;
  BYTE info[8];
  const LONGLONG hundred_ns = read_timeout_ms == MAILSLOT_WAIT_FOREVER_ ? -1 : -10000ll * (LONGLONG)read_timeout_ms;
  memcpy(info, &hundred_ns, 8);
  const NTSTATUS status = NtSetInformationFile(h, &iosb, info, sizeof(info), 27 /* FileMailslotSetInformation */);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* Overlapped I/O is done at once here: the OVERLAPPED is filled in as the system would, the event is set, and the call returns the result (a program that handles
   ERROR_IO_PENDING never sees it; one that waits on the event finds it set). */
static void complete_overlapped(LPOVERLAPPED overlapped, NTSTATUS status, ULONG_PTR bytes) {
  if (!overlapped) return;
  overlapped->Internal = (ULONG_PTR)(ULONG)status;
  overlapped->InternalHigh = bytes;
  if (overlapped->hEvent) NtSetEvent((HANDLE)((ULONG_PTR)overlapped->hEvent & ~(ULONG_PTR)1), NULL);
}
static BOOL con_std_is_console(HANDLE h);
static BOOL con_is_output_only(HANDLE h);
EXPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPOVERLAPPED overlapped) {
  if (read) *read = 0;
  if (con_dead_handle(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (con_is_output_only(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (length > 0x7FFFFFFFu && con_std_is_console(h)) { set_error(ERROR_NOACCESS); return FALSE; }
  if (!disk_buffer_usable(h, buffer, length, TRUE, ERROR_NOACCESS)) return FALSE;
  if (pipe_start_overlapped(0, h, buffer, length, overlapped, NULL)) return FALSE;
  if (!overlapped) { BOOL handled; const BOOL result = console_read_hook(h, buffer, length, read, &handled); if (handled) return result; }
  IO_STATUS_BLOCK iosb;
  LARGE_INTEGER offset;
  PLARGE_INTEGER pointer = NULL;
  if (overlapped) {
    offset.LowPart = overlapped->Offset;
    offset.HighPart = (LONG)overlapped->OffsetHigh;
    pointer = &offset;
    overlapped->Internal = (ULONG_PTR)STATUS_PENDING;
  }
  if (read) *read = 0;
  iosb.Information = 0;
  const NTSTATUS status = NtReadFile(h, NULL, NULL, NULL, &iosb, buffer, length, pointer, NULL);
  if (status == STATUS_BUFFER_OVERFLOW) {      /* a message read in part: the bytes are there, the call says there is more */
    if (read && !overlapped) *read = (DWORD)iosb.Information;
    complete_overlapped(overlapped, status, iosb.Information);
    set_error(ERROR_MORE_DATA);
    return FALSE;
  }
  if (status == STATUS_END_OF_FILE) {
    complete_overlapped(overlapped, status, 0);
    if (overlapped && unbuffered_known(h)) { port_note(h, overlapped, 0); set_error(ERROR_IO_PENDING); return FALSE; }
    return overlapped ? (set_error(ERROR_HANDLE_EOF), FALSE) : TRUE;      /* (an overlapped read says so) */
  }
  if (!NT_SUCCESS(status)) return fail_nt(status);
  complete_overlapped(overlapped, status, iosb.Information);
  port_note(h, overlapped, iosb.Information);
  if (overlapped && unbuffered_known(h)) { set_error(ERROR_IO_PENDING); return FALSE; }      /* (a call that goes to the disk is asynchronous: the result is in the OVERLAPPED) */
  if (read) *read = (DWORD)iosb.Information;
  return TRUE;
}
#include "console.inc"

/* ---- paths ---------------------------------------------------------------------------------------------------------------------------------------------- */


static int is_separator(WCHAR c) { return c == '\\' || c == '/'; }

/* The current directory as the process parameters hold it: "C:\dir\" (always with the trailing backslash). */
static const WCHAR *current_directory(size_t_ *length) {
  UNICODE_STRING *cwd = &parameters()->CurrentDirectoryPath;
  *length = cwd->Length / 2;
  return cwd->Buffer;
}

/* Makes `in` a full path ("C:\dir\file"): the current directory fills in what is missing, "." and ".." are resolved, either slash may be used. Returns the length without the
 * NUL, or 0 (with the error set) if it does not fit in `capacity` characters. "\\?\C:\x" and "\??\C:\x" are taken as "C:\x". */
static DWORD full_path_w(LPCWSTR in, WCHAR *out, DWORD capacity) {
  if (!in || !*in) { set_error(ERROR_INVALID_NAME); return 0; }
  WCHAR drive = 'C';
  int absolute = 0;
  size_t_ cwd_length = 0;
  const WCHAR *cwd = current_directory(&cwd_length);
  if (is_separator(in[0]) && (in[1] == '?' || in[1] == '\\') && (in[2] == '?' || in[2] == '.') && is_separator(in[3]) && in[3]) {      /* \\?\ \??\ \\.\ */
    in += 4;
  } else if (is_separator(in[0]) && in[1] == '?' && in[2] == '?' && is_separator(in[3])) {
    in += 4;
  }
  if (((in[0] >= 'A' && in[0] <= 'Z') || (in[0] >= 'a' && in[0] <= 'z')) && in[1] == ':') {
    drive = upper(in[0]);
    in += 2;
    absolute = is_separator(in[0]);
  } else if (is_separator(in[0])) {
    absolute = 1;
    if (cwd_length >= 1) drive = upper(cwd[0]);
  } else if (cwd_length >= 1) {
    drive = upper(cwd[0]);
  }
  /* The result starts as "C:" and gains "\component" for each component. */
  DWORD n = 0;
  if (capacity < 4) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  out[n++] = drive;
  out[n++] = ':';
  int overflow = 0;
  /* Adds the components of a path to `out`. */
  for (int pass = absolute ? 1 : 0; pass < 2; ++pass) {
    const WCHAR *text = pass == 0 ? (cwd_length > 2 ? cwd + 2 : (const WCHAR *)L"") : in;      /* the current directory without its "C:" */
    while (*text) {
      while (is_separator(*text)) ++text;
      const WCHAR *end = text;
      while (*end && !is_separator(*end)) ++end;
      const size_t_ length = (size_t_)(end - text);
      if (length == 0) break;
      if (length == 1 && text[0] == '.') {
      } else if (length == 2 && text[0] == '.' && text[1] == '.') {
        while (n > 2 && out[n - 1] != '\\') --n;
        if (n > 2) --n;
      } else {
        if (n + 1 + length + 1 > capacity) { overflow = 1; break; }
        out[n++] = '\\';
        size_t_ keep = length;
        if (!*end) while (keep > 1 && (text[keep - 1] == '.' || text[keep - 1] == ' ')) --keep;      /* Windows drops trailing dots and spaces of the last name */
        memcpy(out + n, text, keep * 2);
        n += (DWORD)keep;
      }
      text = end;
    }
    if (overflow) break;
  }
  if (overflow) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  if (n == 2) out[n++] = '\\';
  out[n] = 0;
  return n;
}

static DWORD dosdev_redirect(WCHAR *buffer, DWORD length, DWORD capacity);
static BOOL volume_nt_name(LPCWSTR dos, WCHAR *buffer, DWORD capacity, DWORD *length);

/* The NT name ("\??\C:\dir\file") for a Win32 path, and an OBJECT_ATTRIBUTES for it. `buffer` needs PATH_CHARS + 8 characters. */
static BOOL expand_short_names(WCHAR *path, DWORD length, DWORD capacity, DWORD *new_length);
static int has_tilde(const WCHAR *s) { for (; *s; ++s) if (*s == '~') return 1; return 0; }
static BOOL nt_name_ex(LPCWSTR dos, WCHAR *buffer, UNICODE_STRING *name, OBJECT_ATTRIBUTES *attributes, int expand) {
  DWORD total = 0;
  if (dos && dos[0] == '\\' && ((dos[1] == '\\' && dos[2] == '?') || (dos[1] == '?' && dos[2] == '?')) && dos[3] == '\\') {      /* \\?\ and \??\: the name is taken as it is, nothing is made of "." and ".." */
    const WCHAR *rest = dos + 4;
    if (!*rest) { set_error(ERROR_INVALID_NAME); return FALSE; }
    for (const WCHAR *c = rest; *c;) {
      const WCHAR *end = c;
      while (*end && *end != '\\') ++end;
      if ((end - c == 1 && c[0] == '.') || (end - c == 2 && c[0] == '.' && c[1] == '.')) { set_error(ERROR_INVALID_NAME); return FALSE; }
      c = *end ? end + 1 : end;
    }
  }
  if (!volume_nt_name(dos, buffer, PATH_CHARS + 8, &total)) {
    buffer[0] = '\\'; buffer[1] = '?'; buffer[2] = '?'; buffer[3] = '\\';
    const DWORD n = full_path_w(dos, buffer + 4, PATH_CHARS);
    if (n == 0) return FALSE;
    total = n + 4;
    if (expand && has_tilde(buffer + 4)) {      /* a component may be the 8.3 alias of a longer name */
      DWORD expanded = n;
      if (!expand_short_names(buffer + 4, n, PATH_CHARS, &expanded)) return FALSE;
      total = expanded + 4;
    }
  }
  total = dosdev_redirect(buffer, total, PATH_CHARS + 8);
  if (total == 0) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
  name->Buffer = buffer;
  name->Length = (USHORT)(total * 2);
  name->MaximumLength = (USHORT)(name->Length + 2);
  attributes->Length = sizeof(OBJECT_ATTRIBUTES);
  attributes->RootDirectory = NULL;
  attributes->ObjectName = name;
  attributes->Attributes = 0x40;      /* OBJ_CASE_INSENSITIVE */
  attributes->SecurityDescriptor = attributes->SecurityQualityOfService = NULL;
  return TRUE;
}

static BOOL nt_name(LPCWSTR dos, WCHAR *buffer, UNICODE_STRING *name, OBJECT_ATTRIBUTES *attributes) { return nt_name_ex(dos, buffer, name, attributes, 1); }

/* full_path_w for GetFullPathName: a path written with a trailing separator ("dir\\") keeps it (the NT names do without). */
static DWORD full_path_keep_slash(LPCWSTR path, WCHAR *full, DWORD capacity) {
  DWORD n = full_path_w(path, full, capacity - 1);
  if (n == 0) return 0;
  const size_t_ length = wcslen(path);
  if (length > 0 && is_separator(path[length - 1]) && full[n - 1] != '\\') { full[n++] = '\\'; full[n] = 0; }
  return n;
}

EXPORT DWORD WINAPI GetFullPathNameW(LPCWSTR path, DWORD capacity, LPWSTR buffer, LPWSTR *file_part) {
  WCHAR full[PATH_CHARS];
  const DWORD n = full_path_keep_slash(path, full, PATH_CHARS);
  if (n == 0) return 0;
  if (n + 1 > capacity) return n + 1;
  memcpy(buffer, full, (n + 1) * 2);
  if (file_part) {
    LPWSTR slash = buffer + n;
    while (slash > buffer && *(slash - 1) != '\\') --slash;
    *file_part = (n > 3 && *slash) ? slash : NULL;
  }
  return n;
}
EXPORT DWORD WINAPI GetFullPathNameA(LPCSTR path, DWORD capacity, LPSTR buffer, LPSTR *file_part) {
  WCHAR *w = to_wide(path);
  if (!w) return 0;
  WCHAR full[PATH_CHARS];
  const DWORD n = full_path_keep_slash(w, full, PATH_CHARS);
  free_wide(w);
  if (n == 0) return 0;
  const DWORD r = wide_result_to_ansi(full, n, buffer, capacity);
  if (r < capacity && file_part) {
    LPSTR slash = buffer + r;
    while (slash > buffer && *(slash - 1) != '\\') --slash;
    *file_part = (r > 3 && *slash) ? slash : NULL;
  }
  return r;
}

EXPORT DWORD WINAPI GetCurrentDirectoryW(DWORD capacity, LPWSTR buffer) {
  size_t_ length = 0;
  const WCHAR *cwd = current_directory(&length);
  if (length > 3 && cwd[length - 1] == '\\') --length;      /* "C:\dir\" is reported as "C:\dir"; the root keeps its slash */
  if (length + 1 > capacity) return (DWORD)length + 1;
  memcpy(buffer, cwd, length * 2);
  buffer[length] = 0;
  return (DWORD)length;
}
EXPORT DWORD WINAPI GetCurrentDirectoryA(DWORD capacity, LPSTR buffer) {
  WCHAR w[PATH_CHARS];
  const DWORD n = GetCurrentDirectoryW(PATH_CHARS, w);
  if (n == 0 || n >= PATH_CHARS) return 0;
  return wide_result_to_ansi(w, n, buffer, capacity);
}

EXPORT DWORD WINAPI GetFileAttributesW(LPCWSTR path) {
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(path, buffer, &name, &attributes)) return INVALID_FILE_ATTRIBUTES;
  BYTE basic[40];
  const NTSTATUS status = NtQueryAttributesFile(&attributes, basic);
  if (!NT_SUCCESS(status)) { fail_nt(status); return INVALID_FILE_ATTRIBUTES; }
  return *(DWORD *)(basic + 32);
}
EXPORT DWORD WINAPI GetFileAttributesA(LPCSTR path) {
  WCHAR *w = to_wide(path);
  const DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
  free_wide(w);
  return a;
}
/* The read-only attribute is the one a file system here keeps; the others (hidden, system, archive) are accepted and not stored. */
EXPORT BOOL WINAPI SetFileAttributesW(LPCWSTR path, DWORD attributes) {
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES object;
  if (!nt_name(path, buffer, &name, &object)) return FALSE;
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  NTSTATUS status = NtOpenFile(&h, 0x100 | 0x00100000 /* FILE_WRITE_ATTRIBUTES | SYNCHRONIZE */, &object, &iosb, 7, 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  BYTE basic[40] = {0};
  DWORD value = (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) ? attributes & FILE_ATTRIBUTE_READONLY : 0x80;      /* (0 would mean no change: NORMAL clears) */
  memcpy(basic + 32, &value, 4);
  status = NtSetInformationFile(h, &iosb, basic, sizeof(basic), 4);
  NtClose(h);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI SetFileAttributesA(LPCSTR path, DWORD attributes) {
  WCHAR *w = to_wide(path);
  const BOOL ok = w ? SetFileAttributesW(w, attributes) : FALSE;
  free_wide(w);
  return ok;
}

EXPORT BOOL WINAPI SetCurrentDirectoryW(LPCWSTR path) {
  WCHAR full[PATH_CHARS];
  const DWORD n = full_path_w(path, full, PATH_CHARS - 2);
  if (n == 0) return FALSE;
  const DWORD attributes = GetFileAttributesW(full);
  if (attributes == INVALID_FILE_ATTRIBUTES) return FALSE;
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { set_error(ERROR_DIRECTORY); return FALSE; }
  DWORD length = n;
  if (full[length - 1] != '\\') full[length++] = '\\';
  full[length] = 0;
  const SIZE_T room = length + 1 > 260 ? length + 1 : 260;      /* (MAX_PATH characters, as Windows keeps it) */
  WCHAR *fresh = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, room * 2);
  if (!fresh) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  memcpy(fresh, full, (SIZE_T)(length + 1) * 2);
  /* The directory is held open (FILE_TRAVERSE, sharing read and write but not delete), as the real one is: it cannot be removed while it is the current one. */
  HANDLE held = NULL;
  {
    WCHAR nt_buffer[PATH_CHARS + 8];
    UNICODE_STRING nt_us;
    OBJECT_ATTRIBUTES nt_attributes;
    IO_STATUS_BLOCK iosb;
    if (nt_name(full, nt_buffer, &nt_us, &nt_attributes) &&
        !NT_SUCCESS(NtOpenFile(&held, 0x20 | 0x00100000 /* FILE_TRAVERSE | SYNCHRONIZE */, &nt_attributes, &iosb, 3, 0x1 | 0x20 /* FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT */)))
      held = NULL;
  }
  RTL_USER_PROCESS_PARAMETERS *current = parameters();
  UNICODE_STRING *cwd = &current->CurrentDirectoryPath;
  cwd->Buffer = fresh;
  cwd->Length = (USHORT)(length * 2);
  cwd->MaximumLength = (USHORT)(room * 2);
  if (current->CurrentDirectoryHandle) NtClose(current->CurrentDirectoryHandle);
  current->CurrentDirectoryHandle = held;
  return TRUE;
}
EXPORT BOOL WINAPI SetCurrentDirectoryA(LPCSTR path) {
  WCHAR *w = to_wide(path);
  const BOOL ok = w ? SetCurrentDirectoryW(w) : FALSE;
  free_wide(w);
  return ok;
}

EXPORT UINT WINAPI GetSystemDirectoryW(LPWSTR buffer, UINT capacity) {
  static const WCHAR kPath[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', '3', '2', 0};
  if (capacity < 20) return 20;
  memcpy(buffer, kPath, sizeof(kPath));
  return 19;
}
EXPORT UINT WINAPI GetSystemDirectoryA(LPSTR buffer, UINT capacity) {
  if (capacity < 20) return 20;
  memcpy(buffer, "C:\\Windows\\System32", 20);
  return 19;
}
EXPORT UINT WINAPI GetSystemWow64DirectoryW(LPWSTR buffer, UINT capacity) {
  static const WCHAR kPath[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 'W', 'O', 'W', '6', '4', 0};
  if (capacity < 20) return 20;
  memcpy(buffer, kPath, sizeof(kPath));
  return 19;
}
EXPORT UINT WINAPI GetSystemWow64DirectoryA(LPSTR buffer, UINT capacity) {
  if (capacity < 20) return 20;
  memcpy(buffer, "C:\\Windows\\SysWOW64", 20);
  return 19;
}
EXPORT UINT WINAPI GetWindowsDirectoryW(LPWSTR buffer, UINT capacity) {
  static const WCHAR kPath[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', 0};
  if (capacity < 11) return 11;
  memcpy(buffer, kPath, sizeof(kPath));
  return 10;
}
EXPORT UINT WINAPI GetWindowsDirectoryA(LPSTR buffer, UINT capacity) {
  if (capacity < 11) return 11;
  memcpy(buffer, "C:\\Windows", 11);
  return 10;
}
/* The temporary directory as a full path with a trailing backslash: TMP, TEMP, USERPROFILE, in that order (C:\\Temp\\ if none is set). 0 if it is too long. */
static DWORD temp_path_w(WCHAR *result) {
  static const WCHAR kTmp[] = {'T', 'M', 'P', 0}, kTemp[] = {'T', 'E', 'M', 'P', 0}, kProfile[] = {'U', 'S', 'E', 'R', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0};
  static const WCHAR kDefault[] = {'C', ':', '\\', 'T', 'e', 'm', 'p', 0};
  const WCHAR *const names[3] = {kTmp, kTemp, kProfile};
  WCHAR value[PATH_CHARS];
  DWORD n = 0;
  for (int i = 0; i < 3 && n == 0; ++i) {
    n = GetEnvironmentVariableW(names[i], value, PATH_CHARS);
    if (n >= PATH_CHARS) n = 0;
  }
  if (n == 0) memcpy(value, kDefault, sizeof(kDefault));
  n = full_path_w(value, result, MAX_PATH);
  if (n == 0 || n > MAX_PATH - 2) { set_error(ERROR_FILENAME_EXCED_RANGE); return 0; }
  if (result[n - 1] != '\\') { result[n++] = '\\'; result[n] = 0; }
  return n;
}
/* A buffer that is too small is emptied (as far as it goes) and the call returns the size needed; otherwise the buffer is zeroed after the path up to 32767 characters. */
EXPORT DWORD WINAPI GetTempPathW(DWORD capacity, LPWSTR buffer) {
  WCHAR path[MAX_PATH + 2];
  const DWORD n = temp_path_w(path);
  if (n == 0) return 0;
  if (capacity < n + 1) {
    if (buffer && capacity) memset(buffer, 0, (size_t_)capacity * 2);
    return n + 1;
  }
  memcpy(buffer, path, (n + 1) * 2);
  const DWORD end = capacity < 32767 ? capacity : 32767;
  if (end > n + 1) memset(buffer + n + 1, 0, (size_t_)(end - (n + 1)) * 2);
  return n;
}
EXPORT DWORD WINAPI GetTempPathA(DWORD capacity, LPSTR buffer) {
  WCHAR path[MAX_PATH + 2];
  const DWORD n = temp_path_w(path);
  if (n == 0) return 0;
  return wide_result_to_ansi(path, n, buffer, capacity);      /* (a buffer that is too small is left alone) */
}

/* ---- files ----------------------------------------------------------------------------------------------------------------------------------------------- */

#define FILE_OPEN_ 1
#define FILE_CREATE_ 2
#define FILE_OPEN_IF_ 3
#define FILE_OVERWRITE_ 4
#define FILE_OVERWRITE_IF_ 5
#define FILE_DIRECTORY_FILE_ 0x1
#define FILE_NON_DIRECTORY_FILE_ 0x40
#define FILE_SYNCHRONOUS_IO_NONALERT_ 0x20
#define FILE_DELETE_ON_CLOSE_ 0x1000
#define FILE_LIST_DIRECTORY_ 1

EXPORT HANDLE WINAPI CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file) {
  (void)template_file;
  if (!path || !path[0]) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  /* The console by its device names. */
  if (equals_ignore_case(path, (const WCHAR *)L"CON")) {      /* the console device: its input when it is opened to read, its output when it is opened to write, not both */
    const BOOL reads = (access & 0x80000000u) != 0, writes = (access & 0x40000000u) != 0;
    if (reads && writes) { set_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return con_open_alias(reads, sa && sa->bInheritHandle);
  }
  if (equals_ignore_case(path, (const WCHAR *)L"CONOUT$") || equals_ignore_case(path, (const WCHAR *)L"CONIN$")) {      /* another handle to the console (descriptors 0 and 1 are it) */
    return con_open_alias(equals_ignore_case(path, (const WCHAR *)L"CONIN$"), sa && sa->bInheritHandle);
  }
  if (path[0] == '\\' && path[1] == '?' && path[2] == '?' && path[3] == '\\' &&
      (equals_ignore_case(path + 4, (const WCHAR *)L"CONOUT$") || equals_ignore_case(path + 4, (const WCHAR *)L"CONIN$"))) {      /* the same by its NT name */
    if (disposition == 0 || disposition > 5) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    return con_open_alias(equals_ignore_case(path + 4, (const WCHAR *)L"CONIN$"), sa && sa->bInheritHandle);
  }
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  const BOOL is_pipe = (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/') && path[2] == '.' && (path[3] == '\\' || path[3] == '/') && (path[4] | 0x20) == 'p' &&
                       (path[5] | 0x20) == 'i' && (path[6] | 0x20) == 'p' && (path[7] | 0x20) == 'e' && (path[8] == '\\' || path[8] == '/' || path[8] == 0);      /* \\.\pipe\ */
  {
    const size_t_ plen = wcslen(path);
    /* a name that ends in a separator is a directory: opening it as a file finds no such path */
    if (!(flags & FILE_FLAG_BACKUP_SEMANTICS) && is_separator(path[plen - 1]) && !(plen == 3 && path[1] == ':') && !is_pipe) {      /* (a pipe's name may end in one) */ set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    /* \\.\Volume{GUID}: the raw volume, which no program here may open */
    if (is_separator(path[0]) && is_separator(path[1]) && path[2] == '.' && is_separator(path[3]) && vol_prefix_ci(path + 4, (const WCHAR *)L"Volume{544f5343-0000-4000-8000-000000000001}") && !path[4 + 44]) { set_error(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
  }
  if (is_pipe) {
    WCHAR normal[300];
    size_t_ i = 0;
    for (; path[i] && i < 299; ++i) normal[i] = (path[i] == '/' && i < 9) ? '\\' : path[i];      /* (only the \\.\pipe\ part may use the other slash) */
    normal[i] = 0;
    if (!pipe_nt_name(normal, buffer, &name, &attributes, FALSE)) return INVALID_HANDLE_VALUE;
  } else if ((path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/') && path[2] == '.' && (path[3] == '\\' || path[3] == '/') && (path[4] | 0x20) == 'm' && (path[5] | 0x20) == 'a' &&
             (path[6] | 0x20) == 'i' && (path[7] | 0x20) == 'l' && (path[8] | 0x20) == 's' && (path[9] | 0x20) == 'l' && (path[10] | 0x20) == 'o' && (path[11] | 0x20) == 't' &&
             (path[12] == '\\' || path[12] == '/')) {      /* \\.\mailslot\: the writer's end of a mailslot */
    WCHAR normal[300];
    size_t_ i = 0;
    for (; path[i] && i < 299; ++i) normal[i] = path[i] == '/' ? '\\' : path[i];
    normal[i] = 0;
    if (!mailslot_nt_name(normal, buffer, &name, &attributes, FALSE)) return INVALID_HANDLE_VALUE;
  } else if (is_separator(path[0]) && is_separator(path[1]) && path[2] == '.' && is_separator(path[3]) && equals_ignore_case(path + 4, (const WCHAR *)L"MountPointManager")) {
    /* \\.\MountPointManager: the device that knows the volumes and where they are mounted */
    static const WCHAR mountmgr_name[] = {'\\', '?', '?', '\\', 'M', 'o', 'u', 'n', 't', 'P', 'o', 'i', 'n', 't', 'M', 'a', 'n', 'a', 'g', 'e', 'r', 0};
    memcpy(buffer, mountmgr_name, sizeof(mountmgr_name));
    name.Buffer = buffer;
    name.Length = (sizeof(mountmgr_name) - 2);
    name.MaximumLength = sizeof(mountmgr_name);
    memset(&attributes, 0, sizeof(attributes));
    attributes.Length = sizeof(attributes);
    attributes.ObjectName = &name;
    attributes.Attributes = 0x40;      /* OBJ_CASE_INSENSITIVE */
  } else {
    /* A DOS device name (in any directory) is the device: NUL is the one that exists here, the others are not there. */
    size_t_ last = wcslen(path);
    while (last > 0 && path[last - 1] != '\\' && path[last - 1] != '/' && !(last == 2 && path[1] == ':')) --last;
    if (path[last] && is_dos_device_name(path + last)) {
      WCHAR first = upper(path[last]);
      if (first != 'N') { set_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
      static const WCHAR nul_name[] = {'\\', '?', '?', '\\', 'N', 'U', 'L', 0};
      memcpy(buffer, nul_name, sizeof(nul_name));
      name.Buffer = buffer;
      name.Length = 7 * 2;
      name.MaximumLength = sizeof(nul_name);
      memset(&attributes, 0, sizeof(attributes));
      attributes.Length = sizeof(attributes);
      attributes.ObjectName = &name;
      attributes.Attributes = 0x40;      /* OBJ_CASE_INSENSITIVE */
    } else if (!nt_name(path, buffer, &name, &attributes)) {
      return INVALID_HANDLE_VALUE;
    }
  }
  if (sa && sa->bInheritHandle) attributes.Attributes |= 2;      /* OBJ_INHERIT */
  if (disposition == TRUNCATE_EXISTING && !(access & GENERIC_WRITE)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }      /* (it has to be able to write) */
  ULONG nt_disposition;
  switch (disposition) {
    case CREATE_NEW: nt_disposition = FILE_CREATE_; break;
    case CREATE_ALWAYS: nt_disposition = FILE_OVERWRITE_IF_; break;
    case OPEN_EXISTING: nt_disposition = FILE_OPEN_; break;
    case OPEN_ALWAYS: nt_disposition = FILE_OPEN_IF_; break;
    case TRUNCATE_EXISTING: nt_disposition = FILE_OVERWRITE_; break;
    default: set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
  }
  ULONG nt_access = (access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL)) | SYNCHRONIZE | FILE_READ_ATTRIBUTES;
  if (access & GENERIC_READ) nt_access |= 0x120089;      /* FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE and FILE_ALL_ACCESS written out */
  if (access & GENERIC_WRITE) nt_access |= 0x120116;
  if (access & GENERIC_EXECUTE) nt_access |= 0x1200A0;
  if (access & GENERIC_ALL) nt_access |= 0x1F01FF;
  ULONG options = FILE_SYNCHRONOUS_IO_NONALERT_;
  if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) options |= FILE_NON_DIRECTORY_FILE_;
  if (flags & FILE_FLAG_DELETE_ON_CLOSE) { options |= FILE_DELETE_ON_CLOSE_; nt_access |= DELETE; }
  if ((flags & FILE_FLAG_BACKUP_SEMANTICS) && (flags & 0x01000000u /* FILE_FLAG_POSIX_SEMANTICS */) && (flags & FILE_ATTRIBUTE_DIRECTORY) && disposition == CREATE_NEW) {      /* a new directory, asked for this way */
    options &= ~(ULONG)FILE_NON_DIRECTORY_FILE_;
    options |= FILE_DIRECTORY_FILE_;
  }
  if (flags & 0x00200000u /* FILE_FLAG_OPEN_REPARSE_POINT */) options |= 0x00200000u;      /* FILE_OPEN_REPARSE_POINT: a link is opened itself */
  HANDLE handle = NULL;
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtCreateFile(&handle, nt_access, &attributes, &iosb, NULL, (flags & 0x7FB7) ? (flags & 0x7FB7) : FILE_ATTRIBUTE_NORMAL, share & 7, nt_disposition, options, NULL, 0);
  if (!NT_SUCCESS(status)) {
    set_error(status == STATUS_OBJECT_NAME_COLLISION ? ERROR_FILE_EXISTS : status == STATUS_FILE_IS_A_DIRECTORY ? ERROR_ACCESS_DENIED : RtlNtStatusToDosError(status));
    return INVALID_HANDLE_VALUE;
  }
  /* CREATE_ALWAYS and OPEN_ALWAYS say, by a success with ERROR_ALREADY_EXISTS, that the file was there. */
  set_error(((disposition == CREATE_ALWAYS && iosb.Information == 3) || (disposition == OPEN_ALWAYS && iosb.Information == 1)) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  overlapped_forget(handle);
  if (flags & 0x40000000u /* FILE_FLAG_OVERLAPPED */) { overlapped_note(handle); if (flags & 0x20000000u /* FILE_FLAG_NO_BUFFERING */) unbuffered_note(handle); }
  return handle;
}
EXPORT HANDLE WINAPI CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file) {
  if (!path) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  WCHAR *w = to_wide(path);
  if (!w) return INVALID_HANDLE_VALUE;
  HANDLE h = CreateFileW(w, access, share, sa, disposition, flags, template_file);
  free_wide(w);
  return h;
}

typedef struct { DWORD dwSize, dwFileAttributes, dwFileFlags, dwSecurityQosFlags; LPSECURITY_ATTRIBUTES lpSecurityAttributes; HANDLE hTemplateFile; } CREATEFILE2_PARAMS_;
/* CreateFile2: CreateFileW with its optional arguments in a structure. */
EXPORT HANDLE WINAPI CreateFile2(LPCWSTR path, DWORD access, DWORD share, DWORD disposition, const CREATEFILE2_PARAMS_ *parameters) {
  DWORD flags = 0;
  LPSECURITY_ATTRIBUTES sa = NULL;
  if (parameters) { flags = (parameters->dwFileAttributes & 0x7FB7) | (parameters->dwFileFlags & 0xFFF80000u) | parameters->dwSecurityQosFlags; sa = parameters->lpSecurityAttributes; }
  return CreateFileW(path, access, share, sa, disposition, flags, parameters ? parameters->hTemplateFile : NULL);
}

/* OpenFile: pre-Win32 (16-bit era) file opening, kept for old callers; a small enough subset of uStyle to be worth having (OF_READ/OF_WRITE/OF_READWRITE,
 * OF_CREATE, OF_EXIST), over CreateFileA like a real one. HFILE is just an int (the real type too, historically narrower than a HANDLE). */
typedef int HFILE;
typedef struct _OFSTRUCT { BYTE cBytes, fFixedDisk; WORD nErrCode, Reserved1, Reserved2; CHAR szPathName[128]; } OFSTRUCT, *LPOFSTRUCT;
#define OF_READ_ 0
#define OF_WRITE_ 1
#define OF_READWRITE_ 2
#define OF_CREATE_ 0x1000
#define OF_EXIST_ 0x4000
/* Beep: this OS has no PC speaker driver, so there is nothing to make a sound with; succeeding (not failing outright) matches how real Windows behaves when the
 * speaker driver isn't present either. CancelIo/CancelIoEx/CancelSynchronousIo: every I/O call here is already done by the time it returns (there is no real
 * asynchronous I/O to cancel), so there is never anything pending to find. */
EXPORT BOOL WINAPI Beep(DWORD frequency, DWORD duration) { (void)frequency; (void)duration; return TRUE; }
/* The operations waiting in threads (see pipe_request_thread) end with STATUS_CANCELLED; the kernel does not answer before their threads have completed the OVERLAPPEDs.
   Returns whether one was ended (one that has just finished on its own cannot be cancelled any more). */
static BOOL pipe_cancel_pending(HANDLE h, LPOVERLAPPED overlapped) {
  BOOL cancelled = FALSE;
  for (int n = 0; n < PIPE_PENDING_MAX; ++n) {
    if (!g_pipe_pending[n].used || g_pipe_pending[n].pipe != h || (overlapped && g_pipe_pending[n].overlapped != overlapped)) continue;
    DWORD tag = g_pipe_pending[n].tag;
    IO_STATUS_BLOCK iosb;
    if (tag && NT_SUCCESS(NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_CANCEL_, &tag, 4, NULL, 0))) cancelled = TRUE;
  }
  return cancelled;
}
EXPORT BOOL WINAPI CancelIo(HANDLE h) {
  pipe_cancel_pending(h, NULL);
  return TRUE;
}
EXPORT BOOL WINAPI CancelIoEx(HANDLE h, LPOVERLAPPED overlapped) {
  if (!pipe_cancel_pending(h, overlapped)) { set_error(ERROR_NOT_FOUND); return FALSE; }
  return TRUE;
}
/* Ends the call a thread waits in on a pipe (the kernel knows which thread waits where); a thread that waits for something else has nothing to cancel. */
EXPORT BOOL WINAPI CancelSynchronousIo(HANDLE thread) {
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtCancelSynchronousIoFile(thread, NULL, &iosb);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
/* FatalAppExit: ends the process at once (-1 as the exit code, like a real one's message-box dialog being dismissed); there is no message-box UI here to show
 * the text first. */
EXPORT void WINAPI FatalAppExitW(UINT action, LPCWSTR text) { (void)action; (void)text; ExitProcess(0xFFFFFFFFu); }
EXPORT void WINAPI FatalAppExitA(UINT action, LPCSTR text) { (void)action; (void)text; ExitProcess(0xFFFFFFFFu); }

/* FindFirstChangeNotification/FindNextChangeNotification/FindCloseChangeNotification: the handle is an ordinary ChangeNotification object (NtCreateChangeNotification),
 * signalled like a manual-reset event when something changes under the watched directory - WaitForSingleObject/WaitForMultipleObjects already work on it unmodified.
 * FindNextChangeNotification re-arms it the same way ResetEvent does (it is reset(), not a separate NT service); FindCloseChangeNotification is just CloseHandle. */
EXPORT HANDLE WINAPI FindFirstChangeNotificationW(LPCWSTR path, BOOL watch_subtree, DWORD notify_filter) {
  if (!path) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(path, buffer, &name, &attributes)) return INVALID_HANDLE_VALUE;
  HANDLE handle = NULL;
  const NTSTATUS status = NtCreateChangeNotification(&attributes, watch_subtree ? 1 : 0, notify_filter, &handle);
  if (!NT_SUCCESS(status)) {
    set_error(status == STATUS_NOT_A_DIRECTORY ? ERROR_DIRECTORY : RtlNtStatusToDosError(status));
    return INVALID_HANDLE_VALUE;
  }
  return handle;
}
EXPORT HANDLE WINAPI FindFirstChangeNotificationA(LPCSTR path, BOOL watch_subtree, DWORD notify_filter) {
  if (!path) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  WCHAR *w = to_wide(path);
  if (!w) return INVALID_HANDLE_VALUE;
  HANDLE h = FindFirstChangeNotificationW(w, watch_subtree, notify_filter);
  free_wide(w);
  return h;
}
EXPORT BOOL WINAPI FindNextChangeNotification(HANDLE h) { const NTSTATUS s = NtResetEvent(h, NULL); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }
EXPORT BOOL WINAPI FindCloseChangeNotification(HANDLE h) { return CloseHandle(h); }

/* The two 64-bit queries on an open file. */
static BOOL query_position(HANDLE h, LONGLONG *position) {
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtQueryInformationFile(h, &iosb, position, 8, 14);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
static BOOL query_size(HANDLE h, LONGLONG *size) {
  IO_STATUS_BLOCK iosb;
  BYTE info[24];
  const NTSTATUS status = NtQueryInformationFile(h, &iosb, info, sizeof(info), 5);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *size = *(LONGLONG *)(info + 8);
  return TRUE;
}
EXPORT BOOL WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) { return query_size(h, &size->QuadPart); }
EXPORT DWORD WINAPI GetFileSize(HANDLE h, LPDWORD high) {
  LONGLONG size;
  if (!query_size(h, &size)) return INVALID_FILE_SIZE;
  if (high) *high = (DWORD)((ULONGLONG)size >> 32);
  set_error(ERROR_SUCCESS);
  return (DWORD)size;
}
/* No file system here compresses anything, so the compressed size is always the real one. */
EXPORT DWORD WINAPI GetCompressedFileSizeW(LPCWSTR path, LPDWORD high) {
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (!GetFileAttributesExW(path, 0 /* GetFileExInfoStandard */, &info)) return INVALID_FILE_SIZE;
  if (high) *high = info.nFileSizeHigh;
  return info.nFileSizeLow;
}
EXPORT DWORD WINAPI GetCompressedFileSizeA(LPCSTR path, LPDWORD high) {
  WCHAR *w = to_wide(path);
  if (!w) return INVALID_FILE_SIZE;
  const DWORD r = GetCompressedFileSizeW(w, high);
  free_wide(w);
  return r;
}
EXPORT BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER result, DWORD method) {
  LONGLONG base = 0;
  if (method == FILE_CURRENT) { if (!query_position(h, &base)) return FALSE; }
  else if (method == FILE_END) { if (!query_size(h, &base)) return FALSE; }
  else if (method != FILE_BEGIN) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const LONGLONG target = base + distance.QuadPart;
  if (target < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtSetInformationFile(h, &iosb, (PVOID)&target, 8, 14);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (result) result->QuadPart = target;
  return TRUE;
}
EXPORT DWORD WINAPI SetFilePointer(HANDLE h, LONG low, PLONG high, DWORD method) {
  LARGE_INTEGER distance, result;
  distance.QuadPart = high ? ((LONGLONG)*high << 32) | (DWORD)low : (LONGLONG)low;
  if (!SetFilePointerEx(h, distance, &result, method)) return INVALID_SET_FILE_POINTER;
  if (high) *high = result.HighPart;
  set_error(ERROR_SUCCESS);
  return result.LowPart;
}
EXPORT BOOL WINAPI SetEndOfFile(HANDLE h) {
  LONGLONG position;
  if (!query_position(h, &position)) return FALSE;
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtSetInformationFile(h, &iosb, &position, 8, 20);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI FlushConsoleInputBuffer(HANDLE h);
EXPORT BOOL WINAPI FlushFileBuffers(HANDLE h) {
  IO_STATUS_BLOCK iosb;
  if (con_dead_handle(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (con_is_input(h)) return FlushConsoleInputBuffer(h);      /* the input of a console: what was typed and not read is dropped */
  if (con_is_output_only(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  const NTSTATUS status = NtFlushBuffersFile(h, &iosb);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info) {
  IO_STATUS_BLOCK iosb;
  BYTE basic[40], standard[24];
  NTSTATUS status = NtQueryInformationFile(h, &iosb, basic, sizeof(basic), 4);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  status = NtQueryInformationFile(h, &iosb, standard, sizeof(standard), 5);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  memset(info, 0, sizeof(*info));
  info->dwFileAttributes = *(DWORD *)(basic + 32);
  memcpy(&info->ftCreationTime, basic, 8);
  memcpy(&info->ftLastAccessTime, basic + 8, 8);
  memcpy(&info->ftLastWriteTime, basic + 16, 8);
  info->dwVolumeSerialNumber = 1;
  info->nFileSizeLow = *(DWORD *)(standard + 8);
  info->nFileSizeHigh = *(DWORD *)(standard + 12);
  info->nNumberOfLinks = 1;
  return TRUE;
}
EXPORT BOOL WINAPI GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written) {
  IO_STATUS_BLOCK iosb;
  BYTE basic[40];
  const NTSTATUS status = NtQueryInformationFile(h, &iosb, basic, sizeof(basic), 4);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (created) memcpy(created, basic, 8);
  if (accessed) memcpy(accessed, basic + 8, 8);
  if (written) memcpy(written, basic + 16, 8);
  return TRUE;
}
EXPORT BOOL WINAPI SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w) {
  IO_STATUS_BLOCK iosb;
  BYTE basic[40] = {0};
  if (c) memcpy(basic, c, 8);
  if (a) memcpy(basic + 8, a, 8);      /* (no file system here keeps an access time: ignored) */
  if (w) memcpy(basic + 16, w, 8);
  const NTSTATUS status = NtSetInformationFile(h, &iosb, basic, sizeof(basic), 4);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI GetFileAttributesExW(LPCWSTR path, int level, LPVOID data) {
  if (level != 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(path, buffer, &name, &attributes)) return FALSE;
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  NTSTATUS status = NtOpenFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &iosb, 7, FILE_SYNCHRONOUS_IO_NONALERT_);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  BYTE info[56];
  status = NtQueryInformationFile(h, &iosb, info, sizeof(info), 34);      /* FileNetworkOpenInformation */
  NtClose(h);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  LPWIN32_FILE_ATTRIBUTE_DATA out = data;
  out->dwFileAttributes = *(DWORD *)(info + 48);
  memcpy(&out->ftCreationTime, info, 8);
  memcpy(&out->ftLastAccessTime, info + 8, 8);
  memcpy(&out->ftLastWriteTime, info + 16, 8);
  out->nFileSizeLow = *(DWORD *)(info + 40);
  out->nFileSizeHigh = *(DWORD *)(info + 44);
  return TRUE;
}
EXPORT BOOL WINAPI GetFileAttributesExA(LPCSTR path, int level, LPVOID data) {
  WCHAR *w = to_wide(path);
  const BOOL ok = w ? GetFileAttributesExW(w, level, data) : FALSE;
  free_wide(w);
  return ok;
}
EXPORT BOOL WINAPI DeleteFileW(LPCWSTR path) {
  if (!path || !path[0]) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(path, buffer, &name, &attributes)) return FALSE;
  BYTE basic[40];
  NTSTATUS status = NtQueryAttributesFile(&attributes, basic);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (*(DWORD *)(basic + 32) & (FILE_ATTRIBUTE_DIRECTORY | 1 /* FILE_ATTRIBUTE_READONLY */)) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  status = NtDeleteFile(&attributes);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI DeleteFileA(LPCSTR path) { if (!path) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; } WCHAR *w = to_wide(path); const BOOL ok = w ? DeleteFileW(w) : FALSE; free_wide(w); return ok; }

EXPORT BOOL WINAPI CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa) {
  (void)sa;
  if (!path || !path[0]) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(path, buffer, &name, &attributes)) return FALSE;
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtCreateFile(&h, FILE_LIST_DIRECTORY_ | SYNCHRONIZE, &attributes, &iosb, NULL, FILE_ATTRIBUTE_DIRECTORY, 7, FILE_CREATE_, FILE_DIRECTORY_FILE_ | FILE_SYNCHRONOUS_IO_NONALERT_, NULL, 0);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  NtClose(h);
  return TRUE;
}
EXPORT BOOL WINAPI CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa) { if (!path) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; } WCHAR *w = to_wide(path); const BOOL ok = w ? CreateDirectoryW(w, sa) : FALSE; free_wide(w); return ok; }
EXPORT BOOL WINAPI RemoveDirectoryW(LPCWSTR path) {
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(path, buffer, &name, &attributes)) return FALSE;
  BYTE basic[40];
  NTSTATUS status = NtQueryAttributesFile(&attributes, basic);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (!(*(DWORD *)(basic + 32) & FILE_ATTRIBUTE_DIRECTORY)) { set_error(ERROR_DIRECTORY); return FALSE; }
  status = NtDeleteFile(&attributes);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI RemoveDirectoryA(LPCSTR path) { WCHAR *w = to_wide(path); const BOOL ok = w ? RemoveDirectoryW(w) : FALSE; free_wide(w); return ok; }

VOID NTAPI TcosNoteFileRename(PCWSTR from, PCWSTR to);
EXPORT BOOL WINAPI MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  WCHAR source_buffer[PATH_CHARS + 8], target_buffer[PATH_CHARS + 8];
  UNICODE_STRING source_name, target_name;
  OBJECT_ATTRIBUTES source, target;
  if (!nt_name(from, source_buffer, &source_name, &source) || !nt_name(to, target_buffer, &target_name, &target)) return FALSE;
  if (source_name.Length == target_name.Length) {      /* a file moved onto itself stays where it is (a different spelling of the name is a rename) */
    ULONG same = 1;
    for (ULONG i = 0; i < source_name.Length / 2 && same; ++i) if (source_name.Buffer[i] != target_name.Buffer[i]) same = 0;
    if (same && GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES) return TRUE;
  }
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  NTSTATUS status = NtOpenFile(&h, DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &source, &iosb, 7, FILE_SYNCHRONOUS_IO_NONALERT_);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  const ULONG name_bytes = target_name.Length;
  BYTE *info = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, 20 + name_bytes + 2);
  if (!info) { NtClose(h); set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  info[0] = (flags & MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
  *(ULONG *)(info + 16) = name_bytes;
  memcpy(info + 20, target_name.Buffer, name_bytes);
  status = NtSetInformationFile(h, &iosb, info, 20 + name_bytes, 10);      /* FileRenameInformation */
  RtlFreeHeap(heap(), 0, info);
  NtClose(h);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  WCHAR from_full[PATH_CHARS], to_full[PATH_CHARS];      /* (the loader follows a library whose file moved, to know it by its new name) */
  if (full_path_w(from, from_full, PATH_CHARS) && full_path_w(to, to_full, PATH_CHARS)) TcosNoteFileRename(from_full, to_full);
  return TRUE;
}
EXPORT BOOL WINAPI MoveFileW(LPCWSTR from, LPCWSTR to) { return MoveFileExW(from, to, 0); }
EXPORT BOOL WINAPI MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
  WCHAR *a = to_wide(from), *b = to_wide(to);
  const BOOL ok = a && b ? MoveFileExW(a, b, flags) : FALSE;
  free_wide(a);
  free_wide(b);
  return ok;
}
EXPORT BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to) { return MoveFileExA(from, to, 0); }
/* Copying: the source is opened first (reading, sharing everything); the destination is created or overwritten (sharing reading and writing, and asking for DELETE access
 * too if the others allow it, which a cancelled copy needs to remove it again). The data goes over in pieces; the last write time and the read-only attribute follow.
 * `progress` is called once when the stream starts and after each piece; it may cancel (PROGRESS_CANCEL: the destination is removed if it can be) or stop. */
typedef DWORD (WINAPI *COPY_PROGRESS_)(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID);
static BOOL copy_file_core(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists, COPY_PROGRESS_ progress, LPVOID data, LPBOOL cancel) {
  if (!from || !to) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
  if (cancel && *cancel) { set_error(ERROR_REQUEST_ABORTED); return FALSE; }
  HANDLE in = CreateFileW(from, GENERIC_READ, 7, NULL, OPEN_EXISTING, 0, NULL);
  if (in == INVALID_HANDLE_VALUE) return FALSE;
  WCHAR full_from[PATH_CHARS], full_to[PATH_CHARS];
  if (full_path_w(from, full_from, PATH_CHARS) && full_path_w(to, full_to, PATH_CHARS) && equals_ignore_case(full_from, full_to)) { CloseHandle(in); set_error(ERROR_SHARING_VIOLATION); return FALSE; }
  BOOL can_delete = TRUE;
  HANDLE out = CreateFileW(to, GENERIC_WRITE | DELETE, 3, NULL, fail_if_exists ? CREATE_NEW : CREATE_ALWAYS, 0, NULL);
  if (out == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
    can_delete = FALSE;
    out = CreateFileW(to, GENERIC_WRITE, 3, NULL, fail_if_exists ? CREATE_NEW : CREATE_ALWAYS, 0, NULL);
  }
  if (out == INVALID_HANDLE_VALUE) { const DWORD e = GetLastError(); CloseHandle(in); set_error(e); return FALSE; }
  LARGE_INTEGER total;
  total.QuadPart = 0;
  GetFileSizeEx(in, &total);
  LARGE_INTEGER done;
  done.QuadPart = 0;
  DWORD verdict = 0;      /* PROGRESS_CONTINUE */
  if (progress) verdict = progress(total, done, total, done, 1, 1 /* CALLBACK_STREAM_SWITCH */, in, out, data);
  BYTE *chunk = verdict == 0 ? RtlAllocateHeap(heap(), 0, 16384) : NULL;
  BOOL ok = verdict == 0 && chunk != NULL;
  DWORD error = verdict == 0 ? (chunk ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY) : ERROR_REQUEST_ABORTED;
  while (ok) {
    DWORD got = 0, put = 0;
    ok = ReadFile(in, chunk, 16384, &got, NULL);
    if (!ok || got == 0) { error = GetLastError(); break; }
    ok = WriteFile(out, chunk, got, &put, NULL) && put == got;
    if (!ok) { error = GetLastError(); break; }
    done.QuadPart += got;
    if (progress) {
      verdict = progress(total, done, total, done, 1, 0 /* CALLBACK_CHUNK_FINISHED */, in, out, data);
      if (verdict) { ok = FALSE; error = ERROR_REQUEST_ABORTED; }
    }
    if (cancel && *cancel) { ok = FALSE; error = ERROR_REQUEST_ABORTED; }
  }
  if (chunk) RtlFreeHeap(heap(), 0, chunk);
  if (ok || (error == ERROR_SUCCESS && !chunk)) { ok = TRUE; error = ERROR_SUCCESS; }
  FILETIME written;
  const BOOL have_time = ok && GetFileTime(in, NULL, NULL, &written);
  if (have_time) SetFileTime(out, NULL, NULL, &written);
  const DWORD source_attributes = GetFileAttributesW(from);
  CloseHandle(in);
  CloseHandle(out);
  if (ok && source_attributes != INVALID_FILE_ATTRIBUTES && (source_attributes & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(to, FILE_ATTRIBUTE_READONLY);
  if (!ok && verdict == 1 /* PROGRESS_CANCEL */ && can_delete) DeleteFileW(to);
  set_error(ok ? ERROR_SUCCESS : error);
  return ok;
}
EXPORT BOOL WINAPI CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) { return copy_file_core(from, to, fail_if_exists, NULL, NULL, NULL); }
EXPORT BOOL WINAPI CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
  if (!from || !to) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
  WCHAR *a = to_wide(from), *b = to_wide(to);
  const BOOL ok = a && b ? CopyFileW(a, b, fail_if_exists) : FALSE;
  free_wide(a);
  free_wide(b);
  return ok;
}
EXPORT UINT WINAPI GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR result) {
  static UINT counter;
  WCHAR full[PATH_CHARS];
  const DWORD n = full_path_w(dir, full, PATH_CHARS - 24);
  if (n == 0) return 0;
  {      /* the folder has to be there */
    const DWORD attributes = GetFileAttributesW(full);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) { set_error(ERROR_DIRECTORY); return 0; }
  }
  for (UINT attempt = 0; attempt < 0x10000; ++attempt) {
    const UINT id = unique ? unique : (GetTickCount() + ++counter) & 0xFFFF;
    DWORD length = n;
    if (full[length - 1] != '\\') full[length++] = '\\';
    for (int i = 0; i < 3 && prefix && prefix[i]; ++i) full[length++] = prefix[i];
    for (int shift = 12; shift >= 0; shift -= 4) { const UINT digit = (id >> shift) & 15; if (unique && digit == 0 && shift > 0 && (id >> shift) == 0) continue; full[length++] = (WCHAR)"0123456789ABCDEF"[digit]; }
    full[length++] = '.'; full[length++] = 't'; full[length++] = 'm'; full[length++] = 'p'; full[length] = 0;
    if (unique) { memcpy(result, full, (length + 1) * 2); return unique; }
    HANDLE h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); memcpy(result, full, (length + 1) * 2); return id; }
    if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return 0;      /* (only a name that is taken is tried again) */
  }
  return 0;
}
EXPORT UINT WINAPI GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR result) {
  WCHAR *d = to_wide(dir), *p = prefix ? to_wide(prefix) : NULL;
  WCHAR wide[PATH_CHARS];
  const UINT id = d && (p || !prefix) ? GetTempFileNameW(d, p, unique, wide) : 0;
  free_wide(d);
  free_wide(p);
  if (!id) return 0;
  wide_result_to_ansi(wide, (DWORD)wcslen(wide), result, MAX_PATH);
  return id;
}
EXPORT BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE source, HANDLE tp, HANDLE *target, DWORD access, BOOL inherit, DWORD options) {
  const NTSTATUS status = NtDuplicateObject(sp, source, tp, target, access, inherit ? 2 /* OBJ_INHERIT */ : 0, options & 7);      /* DUPLICATE_CLOSE_SOURCE, DUPLICATE_SAME_ACCESS, DUPLICATE_SAME_ATTRIBUTES */
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}

/* ---- directory listings ---------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
  HANDLE directory;
  BYTE buffer[4096];
  ULONG offset, used;
  BOOLEAN started, finished;
  WCHAR pattern[MAX_PATH];
} FindState;

/* Is this the name of a DOS device (NUL, CON, PRN, AUX, COM1..9, LPT1..9, with or without an extension)? */
static BOOL is_dos_device_name(const WCHAR *name) {
  WCHAR base[8];
  size_t_ n = 0;
  while (name[n] && name[n] != '.' && name[n] != ':' && n < 7) { base[n] = upper(name[n]); ++n; }
  if (name[n] && name[n] != '.' && name[n] != ':') return FALSE;      /* (too long to be one) */
  while (n > 0 && base[n - 1] == ' ') --n;
  base[n] = 0;
  if (n == 3 && ((base[0] == 'N' && base[1] == 'U' && base[2] == 'L') || (base[0] == 'C' && base[1] == 'O' && base[2] == 'N') || (base[0] == 'P' && base[1] == 'R' && base[2] == 'N') ||
                 (base[0] == 'A' && base[1] == 'U' && base[2] == 'X'))) return TRUE;
  if (n == 4 && ((base[0] == 'C' && base[1] == 'O' && base[2] == 'M') || (base[0] == 'L' && base[1] == 'P' && base[2] == 'T')) && base[3] >= '1' && base[3] <= '9') return TRUE;
  return FALSE;
}

/* The next record (FILE_BOTH_DIRECTORY_INFORMATION) of the listing; NULL at the end. */
static const BYTE *find_record(FindState *f) {
  while (f->offset >= f->used) {
    if (f->finished) { set_error(ERROR_NO_MORE_FILES); return NULL; }
    IO_STATUS_BLOCK iosb;
    UNICODE_STRING pattern;
    pattern.Buffer = f->pattern;
    pattern.Length = (USHORT)(wcslen(f->pattern) * 2);
    pattern.MaximumLength = pattern.Length + 2;
    const NTSTATUS status = NtQueryDirectoryFile(f->directory, NULL, NULL, NULL, &iosb, f->buffer, sizeof(f->buffer), 3, FALSE, f->started ? NULL : &pattern, !f->started);
    f->started = TRUE;
    if (!NT_SUCCESS(status)) {
      f->finished = TRUE;
      set_error(status == STATUS_NO_MORE_FILES ? ERROR_NO_MORE_FILES : RtlNtStatusToDosError(status));
      return NULL;
    }
    f->offset = 0;
    f->used = (ULONG)iosb.Information;
  }
  const BYTE *record = f->buffer + f->offset;
  const ULONG next = *(const ULONG *)record;
  f->offset = next ? f->offset + next : f->used;
  return record;
}
static void fill_find_data(const BYTE *record, WIN32_FIND_DATAW *data) {
  memset(data, 0, sizeof(*data));
  data->dwFileAttributes = *(const DWORD *)(record + 56);
  memcpy(&data->ftCreationTime, record + 8, 8);
  memcpy(&data->ftLastAccessTime, record + 16, 8);
  memcpy(&data->ftLastWriteTime, record + 24, 8);
  data->nFileSizeLow = *(const DWORD *)(record + 40);
  data->nFileSizeHigh = *(const DWORD *)(record + 44);
  DWORD units = *(const DWORD *)(record + 60) / 2;
  if (units > MAX_PATH - 1) units = MAX_PATH - 1;
  memcpy(data->cFileName, record + 94, units * 2);
  data->cFileName[units] = 0;
}

/* `expand` says whether 8.3 aliases in the directory part are turned into the long names (the 8.3 code itself lists directories without that). */
static HANDLE find_first_impl(LPCWSTR path, LPWIN32_FIND_DATAW data, int expand) {
  if (!path) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  /* The pattern is what follows the last separator of the path as it was written: normalising the whole path would take trailing dots and spaces off it, and those
   * mean something in a pattern ("*." is the names without an extension). */
  const size_t_ path_length = wcslen(path);
  size_t_ cut = path_length;
  while (cut > 0 && path[cut - 1] != '\\' && path[cut - 1] != '/' && !(cut == 2 && path[1] == ':')) --cut;
  WCHAR directory_text[PATH_CHARS];
  if (path_length >= PATH_CHARS) { set_error(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
  if (cut == 0) { directory_text[0] = '.'; directory_text[1] = '\\'; directory_text[2] = 0; }
  else { memcpy(directory_text, path, cut * 2); directory_text[cut] = 0; }
  WCHAR full[PATH_CHARS];
  const DWORD n = full_path_w(directory_text, full, PATH_CHARS - 2);
  if (n == 0) return INVALID_HANDLE_VALUE;
  DWORD split = n;
  if (full[split - 1] != '\\') full[split++] = '\\';
  FindState *f = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(FindState));
  if (!f) { set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  const DWORD pattern_length = (DWORD)(path_length - cut);
  if (pattern_length >= MAX_PATH) { RtlFreeHeap(heap(), 0, f); set_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  memcpy(f->pattern, path + cut, pattern_length * 2);
  f->pattern[pattern_length] = 0;
  if (split > 3) --split;      /* the directory without its trailing backslash, except the root */
  full[split] = 0;
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name_ex(full, buffer, &name, &attributes, expand)) { RtlFreeHeap(heap(), 0, f); return INVALID_HANDLE_VALUE; }
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtOpenFile(&f->directory, FILE_LIST_DIRECTORY_ | SYNCHRONIZE, &attributes, &iosb, 7, FILE_DIRECTORY_FILE_ | FILE_SYNCHRONOUS_IO_NONALERT_);
  if (!NT_SUCCESS(status)) {
    RtlFreeHeap(heap(), 0, f);
    set_error(status == STATUS_OBJECT_NAME_NOT_FOUND ? ERROR_PATH_NOT_FOUND : status == STATUS_NOT_A_DIRECTORY ? ERROR_DIRECTORY : RtlNtStatusToDosError(status));
    return INVALID_HANDLE_VALUE;
  }
  if (pattern_length == 0) {      /* "dir\\": the directory is there, but nothing is named by it */
    NtClose(f->directory);
    RtlFreeHeap(heap(), 0, f);
    set_error(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  if (is_dos_device_name(f->pattern)) {      /* a device is found by its name, as an empty file */
    memset(data, 0, sizeof(*data));
    data->dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    memcpy(data->cFileName, f->pattern, (wcslen(f->pattern) + 1) * 2);
    f->finished = TRUE;
    f->offset = f->used = 0;
    return f;
  }
  const BYTE *record = find_record(f);
  if (!record) {
    NtClose(f->directory);
    RtlFreeHeap(heap(), 0, f);
    set_error(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  fill_find_data(record, data);
  return f;
}
EXPORT HANDLE WINAPI FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) { return find_first_impl(path, data, 1); }
EXPORT HANDLE WINAPI FindFirstFileExW(LPCWSTR path, int level, LPVOID data, int search_op, LPVOID filter, DWORD flags) {
  (void)level; (void)search_op; (void)filter; (void)flags;
  return FindFirstFileW(path, data);
}
EXPORT BOOL WINAPI FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data) {
  if (!h || h == INVALID_HANDLE_VALUE) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  const BYTE *record = find_record(h);
  if (!record) return FALSE;
  fill_find_data(record, data);
  return TRUE;
}
EXPORT BOOL WINAPI FindClose(HANDLE h) {
  if (!h || h == INVALID_HANDLE_VALUE) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  FindState *f = h;
  NtClose(f->directory);
  RtlFreeHeap(heap(), 0, f);
  return TRUE;
}
/* The streams of a file: only its data (named "::$DATA"); a directory has none, which the search says by running out at once. */
typedef struct { LARGE_INTEGER StreamSize; WCHAR cStreamName[MAX_PATH + 36]; } WIN32_FIND_STREAM_DATA_;
EXPORT HANDLE WINAPI FindFirstStreamW(LPCWSTR path, int level, LPVOID data, DWORD flags) {
  if (level != 0 || flags != 0 || !data || !path) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (!GetFileAttributesExW(path, 0, &info)) return INVALID_HANDLE_VALUE;
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { set_error(ERROR_HANDLE_EOF); return INVALID_HANDLE_VALUE; }
  FindState *f = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(FindState));
  if (!f) { set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  f->finished = TRUE;
  WIN32_FIND_STREAM_DATA_ *out = data;
  out->StreamSize.LowPart = info.nFileSizeLow;
  out->StreamSize.HighPart = (LONG)info.nFileSizeHigh;
  static const WCHAR name[] = {':', ':', '$', 'D', 'A', 'T', 'A', 0};
  memcpy(out->cStreamName, name, sizeof(name));
  return f;
}
EXPORT BOOL WINAPI FindNextStreamW(HANDLE h, LPVOID data) {
  (void)data;
  if (!h || h == INVALID_HANDLE_VALUE) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  set_error(ERROR_HANDLE_EOF);
  return FALSE;
}
static void find_data_to_ansi(const WIN32_FIND_DATAW *w, LPWIN32_FIND_DATAA a) {
  memset(a, 0, sizeof(*a));
  a->dwFileAttributes = w->dwFileAttributes;
  a->ftCreationTime = w->ftCreationTime;
  a->ftLastAccessTime = w->ftLastAccessTime;
  a->ftLastWriteTime = w->ftLastWriteTime;
  a->nFileSizeHigh = w->nFileSizeHigh;
  a->nFileSizeLow = w->nFileSizeLow;
  wide_result_to_ansi(w->cFileName, (DWORD)wcslen(w->cFileName), a->cFileName, MAX_PATH);
}
EXPORT HANDLE WINAPI FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data) {
  WCHAR *w = to_wide(path);
  if (!w) return INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW wide;
  HANDLE h = FindFirstFileW(w, &wide);
  free_wide(w);
  if (h != INVALID_HANDLE_VALUE) find_data_to_ansi(&wide, data);
  return h;
}
EXPORT HANDLE WINAPI FindFirstFileExA(LPCSTR path, int level, LPVOID data, int search_op, LPVOID filter, DWORD flags) { (void)level; (void)search_op; (void)filter; (void)flags; return FindFirstFileA(path, data); }
EXPORT BOOL WINAPI FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data) {
  WIN32_FIND_DATAW wide;
  if (!FindNextFileW(h, &wide)) return FALSE;
  find_data_to_ansi(&wide, data);
  return TRUE;
}

/* ---- 8.3 names -------------------------------------------------------------------------------------------------------------------------------------------- */

/* The file systems keep no 8.3 names, so they are made up: a name that is not a legal 8.3 name gets "BASE~N.EXT" where N counts the names with the same BASE (6 letters)
 * and EXT (3 letters) that the directory lists before it. The same listing gives the same names, and a path with an alias in it can be turned back into the long one. */
typedef struct { WCHAR base[7]; WCHAR ext[4]; } ShortKey;

static BOOL name_is_8dot3(const WCHAR *name, size_t_ length) {
  if (length == 0) return FALSE;
  if (name[0] == '.' && (length == 1 || (length == 2 && name[1] == '.'))) return TRUE;
  size_t_ base = 0, ext = 0;
  BOOL dot = FALSE;
  for (size_t_ i = 0; i < length; ++i) {
    const WCHAR c = name[i];
    if (c == '.') { if (dot || i == 0) return FALSE; dot = TRUE; continue; }
    if (c <= ' ' || c >= 0x7F) return FALSE;
    for (const char *bad = "\"*+,/:;<=>?[\\]|"; *bad; ++bad) if (c == (WCHAR)*bad) return FALSE;
    if (dot) ++ext; else ++base;
  }
  return base <= 8 && ext <= 3 && (!dot || ext > 0);
}

/* The part of a name that its alias is made of: the first letters of the base and of the extension, upper case, spaces and dots left out, odd characters as '_'. */
static void short_key(const WCHAR *name, size_t_ length, ShortKey *key) {
  size_t_ last_dot = length;
  for (size_t_ i = 1; i < length; ++i) if (name[i] == '.') last_dot = i;
  size_t_ b = 0, e = 0;
  for (size_t_ i = 0; i < length; ++i) {
    WCHAR c = name[i];
    if (c == ' ' || c == '.') continue;
    if (c <= ' ' || c >= 0x7F) c = '_';
    else { for (const char *bad = "\"*+,/:;<=>?[\\]|"; *bad; ++bad) if (c == (WCHAR)*bad) c = '_'; }
    c = upper(c);
    if (i > last_dot) { if (e < 3) key->ext[e++] = c; }
    else if (b < 6) key->base[b++] = c;
  }
  key->base[b] = 0;
  key->ext[e] = 0;
}

static BOOL wide_equal_ci(const WCHAR *a, size_t_ a_length, const WCHAR *b, size_t_ b_length) {
  if (a_length != b_length) return FALSE;
  for (size_t_ i = 0; i < a_length; ++i) if (upper(a[i]) != upper(b[i])) return FALSE;
  return TRUE;
}

/* A listing of the directory written as `dir` (it ends in a separator or ':', or is empty for the current directory). */
static HANDLE scan_open(const WCHAR *dir, size_t_ dir_length, WIN32_FIND_DATAW *data) {
  WCHAR pattern[PATH_CHARS];
  if (dir_length + 2 > PATH_CHARS) { set_error(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
  memcpy(pattern, dir, dir_length * 2);
  pattern[dir_length] = '*';
  pattern[dir_length + 1] = 0;
  return find_first_impl(pattern, data, 0);
}

/* The alias of the file `name` in the directory `dir`. */
static BOOL short_alias(const WCHAR *dir, size_t_ dir_length, const WCHAR *name, WCHAR *alias) {
  const size_t_ length = wcslen(name);
  if (name_is_8dot3(name, length)) { memcpy(alias, name, (length + 1) * 2); return TRUE; }
  ShortKey want;
  short_key(name, length, &want);
  DWORD rank = 1;
  WIN32_FIND_DATAW data;
  HANDLE h = scan_open(dir, dir_length, &data);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (lstrcmpW(data.cFileName, name) == 0) break;
      const size_t_ other_length = wcslen(data.cFileName);
      if (name_is_8dot3(data.cFileName, other_length)) continue;
      ShortKey other;
      short_key(data.cFileName, other_length, &other);
      if (lstrcmpW(other.base, want.base) == 0 && lstrcmpW(other.ext, want.ext) == 0) ++rank;
    } while (FindNextFileW(h, &data));
    FindClose(h);
  }
  WCHAR digits[12];
  size_t_ digit_count = 0;
  for (DWORD r = rank; r; r /= 10) digits[digit_count++] = (WCHAR)('0' + r % 10);
  size_t_ base_length = wcslen(want.base);
  if (base_length + 1 + digit_count > 8) base_length = 8 - 1 - digit_count;
  size_t_ n = 0;
  memcpy(alias, want.base, base_length * 2);
  n = base_length;
  alias[n++] = '~';
  while (digit_count) alias[n++] = digits[--digit_count];
  if (want.ext[0]) {
    alias[n++] = '.';
    for (size_t_ i = 0; want.ext[i]; ++i) alias[n++] = want.ext[i];
  }
  alias[n] = 0;
  return TRUE;
}

/* Finds the file `comp` in the directory `dir`: by its name, or as an alias ("BASE~N.EXT") of one. `by_alias` says which. */
static BOOL find_entry(const WCHAR *dir, size_t_ dir_length, const WCHAR *comp, size_t_ comp_length, WIN32_FIND_DATAW *found, BOOL *by_alias) {
  /* the alias pattern: BASE ~ digits [. EXT] */
  size_t_ tilde = comp_length;
  for (size_t_ i = 0; i < comp_length; ++i) if (comp[i] == '~') tilde = i;
  size_t_ digits_end = tilde + 1;
  DWORD rank = 0;
  while (tilde < comp_length && digits_end < comp_length && comp[digits_end] >= '0' && comp[digits_end] <= '9' && rank < 100000) { rank = rank * 10 + (DWORD)(comp[digits_end] - '0'); ++digits_end; }
  BOOL pattern = tilde > 0 && tilde < comp_length && digits_end > tilde + 1 && rank > 0 && tilde <= 6 && (digits_end == comp_length || comp[digits_end] == '.');
  const WCHAR *ext = comp + digits_end + 1;
  size_t_ ext_length = pattern && digits_end < comp_length ? comp_length - digits_end - 1 : 0;
  if (pattern && ext_length > 3) pattern = FALSE;
  WIN32_FIND_DATAW data, candidate;
  BOOL have_candidate = FALSE;
  DWORD count = 0;
  HANDLE h = scan_open(dir, dir_length, &data);
  if (h == INVALID_HANDLE_VALUE) return FALSE;
  do {
    const size_t_ length = wcslen(data.cFileName);
    if (wide_equal_ci(data.cFileName, length, comp, comp_length)) { *found = data; *by_alias = FALSE; FindClose(h); return TRUE; }
    if (!pattern || have_candidate || name_is_8dot3(data.cFileName, length)) continue;
    ShortKey key;
    short_key(data.cFileName, length, &key);
    size_t_ base_length = wcslen(key.base);
    const size_t_ digit_count = digits_end - tilde - 1;
    if (base_length + 1 + digit_count > 8) base_length = 8 - 1 - digit_count;
    if (base_length != tilde || !wide_equal_ci(key.base, base_length, comp, tilde) || !wide_equal_ci(key.ext, wcslen(key.ext), ext, ext_length)) continue;
    if (++count == rank) { candidate = data; have_candidate = TRUE; }
  } while (FindNextFileW(h, &data));
  FindClose(h);
  if (!have_candidate) return FALSE;
  *found = candidate;
  *by_alias = TRUE;
  return TRUE;
}

/* The full path `path` ("C:\a\b", `length` characters, room for `capacity`) with the 8.3 aliases in it replaced by the long names. */
static BOOL expand_short_names(WCHAR *path, DWORD length, DWORD capacity, DWORD *new_length) {
  DWORD at = 0;
  while (at < length) {
    DWORD start = at, end = at;
    while (start < length && path[start] == '\\') ++start;
    end = start;
    while (end < length && path[end] != '\\') ++end;
    if (end == start) break;
    at = end;
    BOOL tilde = FALSE;
    for (DWORD i = start; i < end; ++i) if (path[i] == '~') tilde = TRUE;
    if (!tilde || start < 3) continue;      /* (the drive is no component) */
    WIN32_FIND_DATAW found;
    BOOL by_alias = FALSE;
    if (!find_entry(path, start, path + start, end - start, &found, &by_alias) || !by_alias) continue;
    const DWORD long_length = (DWORD)wcslen(found.cFileName);
    if (long_length > capacity || length - (end - start) > capacity - long_length - 1) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
    memmove(path + start + long_length, path + end, (length - end + 1) * 2);
    memcpy(path + start, found.cFileName, long_length * 2);
    length = length - (end - start) + long_length;
    at = start + long_length;
  }
  *new_length = length;
  return TRUE;
}

/* GetLongPathName and GetShortPathName work on the path as it is written: the separators, "." and ".." stay, a relative path stays relative, and each of the other components
 * is looked up in the directory before it. A long name that is given in its long form keeps the case it was written in. */
static DWORD convert_path(LPCWSTR in, LPWSTR out, DWORD capacity, int to_short) {
  if (!in) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (!*in) { set_error(ERROR_PATH_NOT_FOUND); return 0; }
  WCHAR result[PATH_CHARS], lookup[PATH_CHARS];      /* the path as it is returned, and the same with real names for the directory listings */
  size_t_ r = 0, l = 0, sp = 0;
  const size_t_ in_length = wcslen(in);
  const BOOL prefixed = in_length >= 4 && in[0] == '\\' && in[1] == '\\' && in[2] == '?' && in[3] == '\\';
  if (!prefixed && in_length >= MAX_PATH) { set_error(ERROR_PATH_NOT_FOUND); return 0; }
  if (in_length + 2 > PATH_CHARS) { set_error(ERROR_FILENAME_EXCED_RANGE); return 0; }
  if (prefixed) {
    memcpy(result, in, 4 * 2); memcpy(lookup, in, 4 * 2);
    r = l = sp = 4;
  } else if (in[0] == '\\' && in[1] == '\\') {      /* \\computer\X$\...: the drive X of this computer; any other UNC path is returned as it is */
    WCHAR name[16];
    DWORD name_length = 16;
    size_t_ host = 2;
    while (in[host] && in[host] != '\\') ++host;
    if (!GetComputerNameW(name, &name_length) || !wide_equal_ci(in + 2, host - 2, name, name_length) || in[host] != '\\' ||
        !(((in[host + 1] >= 'A' && in[host + 1] <= 'Z') || (in[host + 1] >= 'a' && in[host + 1] <= 'z')) && in[host + 2] == '$' && (in[host + 3] == 0 || in[host + 3] == '\\'))) {
      if (in_length + 1 > capacity) return (DWORD)in_length + 1;
      memcpy(out, in, (in_length + 1) * 2);
      return (DWORD)in_length;
    }
    memcpy(result, in, (host + 3) * 2);
    r = host + 3;
    lookup[0] = in[host + 1]; lookup[1] = ':';
    l = 2;
    sp = host + 3;
  }
  if (((in[sp] >= 'A' && in[sp] <= 'Z') || (in[sp] >= 'a' && in[sp] <= 'z')) && in[sp + 1] == ':' && (sp == 0 || sp == 4)) {
    result[r++] = lookup[l++] = in[sp++];
    result[r++] = lookup[l++] = in[sp++];
  }
  for (size_t_ i = sp; in[i]; ++i) if (in[i] == '*' || in[i] == '?') { set_error(ERROR_INVALID_NAME); return 0; }
  while (in[sp]) {
    if (is_separator(in[sp])) { result[r++] = lookup[l++] = in[sp++]; continue; }
    size_t_ end = sp;
    while (in[end] && !is_separator(in[end])) ++end;
    const size_t_ length = end - sp;
    if (length >= MAX_PATH) { set_error(ERROR_FILENAME_EXCED_RANGE); return 0; }
    if ((length == 1 && in[sp] == '.') || (length == 2 && in[sp] == '.' && in[sp + 1] == '.')) {
      memcpy(result + r, in + sp, length * 2); r += length;
      memcpy(lookup + l, in + sp, length * 2); l += length;
      sp = end;
      continue;
    }
    WIN32_FIND_DATAW found;
    BOOL by_alias = FALSE;
    if (!find_entry(lookup, l, in + sp, length, &found, &by_alias)) { set_error(ERROR_FILE_NOT_FOUND); return 0; }
    const WCHAR *text = found.cFileName;
    size_t_ text_length = wcslen(text);
    WCHAR alias[16];
    if (to_short) {
      short_alias(lookup, l, found.cFileName, alias);
      text = alias;
      text_length = wcslen(alias);
    } else if (!by_alias && !name_is_8dot3(found.cFileName, text_length)) {
      text = in + sp;      /* (a long name as it was written) */
      text_length = length;
    }
    const size_t_ real_length = wcslen(found.cFileName);
    if (r + text_length + 2 > PATH_CHARS || l + real_length + 2 > PATH_CHARS) { set_error(ERROR_FILENAME_EXCED_RANGE); return 0; }
    memcpy(result + r, text, text_length * 2); r += text_length;
    memcpy(lookup + l, found.cFileName, real_length * 2); l += real_length;
    sp = end;
  }
  result[r] = 0;
  if (!out || r + 1 > capacity) return (DWORD)r + 1;      /* (without a buffer it is the size needed, whatever the size says) */
  memcpy(out, result, (r + 1) * 2);
  return (DWORD)r;
}
static DWORD convert_path_a(LPCSTR path, LPSTR buffer, DWORD capacity, int to_short) {
  if (!path) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *w = to_wide(path);
  if (!w) return 0;
  WCHAR result[PATH_CHARS];
  const DWORD n = convert_path(w, result, PATH_CHARS, to_short);
  free_wide(w);
  if (n == 0 || n >= PATH_CHARS) return n;
  return wide_result_to_ansi(result, n, buffer, buffer ? capacity : 0);
}
EXPORT DWORD WINAPI GetLongPathNameW(LPCWSTR path, LPWSTR buffer, DWORD capacity) { return convert_path(path, buffer, capacity, 0); }
EXPORT DWORD WINAPI GetLongPathNameA(LPCSTR path, LPSTR buffer, DWORD capacity) { return convert_path_a(path, buffer, capacity, 0); }
EXPORT DWORD WINAPI GetShortPathNameW(LPCWSTR path, LPWSTR buffer, DWORD capacity) { return convert_path(path, buffer, capacity, 1); }
EXPORT DWORD WINAPI GetShortPathNameA(LPCSTR path, LPSTR buffer, DWORD capacity) { return convert_path_a(path, buffer, capacity, 1); }

/* Is `name` a legal 8.3 name for CheckNameLegalDOS8Dot3: a base of up to 8 characters, one dot and up to 3 characters of extension, none of the characters a file name cannot
 * have (spaces are allowed, and said so). The empty name counts as legal. */
static BOOL check_8dot3(const WCHAR *name, BOOL *spaces) {
  *spaces = FALSE;
  size_t_ base = 0, ext = 0;
  BOOL dot = FALSE;
  const size_t_ length = wcslen(name);
  if (length == 0) return TRUE;
  if (name[0] == '.' && (length == 1 || (length == 2 && name[1] == '.'))) return TRUE;
  for (size_t_ i = 0; i < length; ++i) {
    const WCHAR c = name[i];
    if (c == '.') { if (dot || i == 0) return FALSE; dot = TRUE; continue; }
    if (c < ' ' || c >= 0x7F) return FALSE;
    for (const char *bad = "\"*+,/:;<=>?[\\]|"; *bad; ++bad) if (c == (WCHAR)*bad) return FALSE;
    if (c == ' ') *spaces = TRUE;
    if (dot) ++ext; else ++base;
  }
  return base <= 8 && ext <= 3;
}
EXPORT BOOL WINAPI CheckNameLegalDOS8Dot3W(LPCWSTR name, LPSTR oem_name, DWORD oem_length, BOOL *contains_spaces, BOOL *is_legal) {
  if (!name || !is_legal) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  BOOL spaces = FALSE;
  *is_legal = check_8dot3(name, &spaces);
  if (*is_legal && oem_name && oem_length) {      /* (the name in the OEM code page, upper case: as far as it fits) */
    const size_t_ length = wcslen(name);
    if (length + 1 > oem_length) *is_legal = FALSE;
    else { for (size_t_ i = 0; i < length; ++i) oem_name[i] = (char)upper(name[i]); oem_name[length] = 0; }
  }
  if (contains_spaces) *contains_spaces = spaces;
  return TRUE;
}
EXPORT BOOL WINAPI CheckNameLegalDOS8Dot3A(LPCSTR name, LPSTR oem_name, DWORD oem_length, BOOL *contains_spaces, BOOL *is_legal) {
  if (!name || !is_legal) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR *w = to_wide(name);
  if (!w) return FALSE;
  const BOOL ok = CheckNameLegalDOS8Dot3W(w, oem_name, oem_length, contains_spaces, is_legal);
  free_wide(w);
  return ok;
}

/* Does a program that is started by this name also look in the current folder? Not when NoDefaultCurrentDirectoryInExePath is set, unless the name has a path. */
EXPORT BOOL WINAPI NeedCurrentDirectoryForExePathW(LPCWSTR name) {
  static const WCHAR kVariable[] = {'N', 'o', 'D', 'e', 'f', 'a', 'u', 'l', 't', 'C', 'u', 'r', 'r', 'e', 'n', 't', 'D', 'i', 'r', 'e', 'c', 't', 'o', 'r', 'y', 'I', 'n', 'E', 'x', 'e', 'P', 'a', 't', 'h', 0};
  for (LPCWSTR c = name; *c; ++c) if (*c == '\\') return TRUE;
  WCHAR value[8];
  const DWORD saved = GetLastError();
  const BOOL set = GetEnvironmentVariableW(kVariable, value, 8) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
  set_error(saved);
  return !set;
}
EXPORT BOOL WINAPI NeedCurrentDirectoryForExePathA(LPCSTR name) {
  WCHAR *w = to_wide(name);
  if (!w) return TRUE;
  const BOOL need = NeedCurrentDirectoryForExePathW(w);
  free_wide(w);
  return need;
}
EXPORT BOOL WINAPI SetSearchPathMode(DWORD flags) {
  const NTSTATUS status = RtlSetSearchPathMode(flags);
  if (NT_SUCCESS(status)) return TRUE;
  set_error(status == STATUS_ACCESS_DENIED ? ERROR_ACCESS_DENIED : ERROR_INVALID_PARAMETER);
  return FALSE;
}

/* ---- error texts ---------------------------------------------------------------------------------------------------------------------------------------- */

static const char *error_text(DWORD error) {
  switch (error) {
    case ERROR_SUCCESS: return "The operation completed successfully.";
    case ERROR_FILE_NOT_FOUND: return "The system cannot find the file specified.";
    case ERROR_PATH_NOT_FOUND: return "The system cannot find the path specified.";
    case ERROR_ACCESS_DENIED: return "Access is denied.";
    case ERROR_INVALID_HANDLE: return "The handle is invalid.";
    case ERROR_NOT_ENOUGH_MEMORY: return "Not enough memory resources are available to process this command.";
    case ERROR_NO_MORE_FILES: return "There are no more files.";
    case ERROR_SHARING_VIOLATION: return "The process cannot access the file because it is being used by another process.";
    case ERROR_FILE_EXISTS: return "The file exists.";
    case ERROR_INVALID_PARAMETER: return "The parameter is incorrect.";
    case ERROR_INSUFFICIENT_BUFFER: return "The data area passed to a system call is too small.";
    case ERROR_INVALID_NAME: return "The filename, directory name, or volume label syntax is incorrect.";
    case ERROR_MOD_NOT_FOUND: return "The specified module could not be found.";
    case ERROR_PROC_NOT_FOUND: return "The specified procedure could not be found.";
    case ERROR_DIR_NOT_EMPTY: return "The directory is not empty.";
    case ERROR_ALREADY_EXISTS: return "Cannot create a file when that file already exists.";
    case ERROR_DIRECTORY: return "The directory name is invalid.";
    case ERROR_DISK_FULL: return "There is not enough space on the disk.";
    case ERROR_HANDLE_EOF: return "Reached the end of the file.";
    default: return NULL;
  }
}
/* ---- start ------------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved; (void)reason;
  return TRUE;
}

#include "locale.inc"
#include "extra.inc"
#include "extra2.inc"
#include "volume.inc"
#include "extra3.inc"
#include "legacy.inc"
#include "fmtmsg.inc"
#include "time.inc"
#include "comm.inc"
#include "profile.inc"
#include "appmodel.inc"
#include "nls.inc"
#include "nls2.inc"
#include "idn.inc"
#include "actctx.inc"
#include "rsrcupdate.inc"
