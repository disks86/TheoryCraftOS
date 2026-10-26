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
static void complete_overlapped(LPOVERLAPPED overlapped, NTSTATUS status, ULONG_PTR bytes);
static DWORD wait_result(NTSTATUS status);
EXPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPOVERLAPPED overlapped);
EXPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPOVERLAPPED overlapped);
EXPORT DWORD WINAPI GetTickCount(void);
static BOOL console_write_hook(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, BOOL *handled);
static BOOL console_read_hook(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, BOOL *handled);
static BOOL con_input_ready(void);
static BOOL con_is_input(HANDLE h);
static BOOL pipe_nt_name(LPCWSTR name, WCHAR *buffer, UNICODE_STRING *us, OBJECT_ATTRIBUTES *attributes, BOOL inherit);
EXPORT HANDLE WINAPI CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file);
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

/* Converts `length` bytes (-1: up to and with the terminating NUL). Returns the number of characters written, or needed if `capacity` is 0; 0 on failure. */
EXPORT int WINAPI MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int length, LPWSTR dest, int capacity) {
  (void)code_page; (void)flags;
  if (!source || length == 0 || capacity < 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (length < 0) length = (int)strlen(source) + 1;
  ULONG bytes = 0;
  const NTSTATUS status = RtlMultiByteToUnicodeN(capacity ? dest : NULL, (ULONG)capacity * 2, &bytes, source, (ULONG)length);
  if (!NT_SUCCESS(status) || (status == STATUS_BUFFER_OVERFLOW)) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return (int)(bytes / 2);
}
EXPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int length, LPSTR dest, int capacity, LPCSTR default_char, LPBOOL used_default) {
  (void)code_page; (void)flags; (void)default_char;
  if (used_default) *used_default = FALSE;
  if (!source || length == 0 || capacity < 0) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (length < 0) length = (int)wcslen(source) + 1;
  ULONG bytes = 0;
  const NTSTATUS status = RtlUnicodeToMultiByteN(capacity ? dest : NULL, (ULONG)capacity, &bytes, source, (ULONG)length * 2);
  if (!NT_SUCCESS(status) || (status == STATUS_BUFFER_OVERFLOW)) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  return (int)bytes;
}
EXPORT UINT WINAPI GetACP(void) { return CP_UTF8; }
EXPORT UINT WINAPI GetOEMCP(void) { return CP_UTF8; }
EXPORT UINT WINAPI GetConsoleCP(void) { return CP_UTF8; }
EXPORT UINT WINAPI GetConsoleOutputCP(void) { return CP_UTF8; }
EXPORT BOOL WINAPI SetConsoleCP(UINT cp) { (void)cp; return TRUE; }
EXPORT BOOL WINAPI SetConsoleOutputCP(UINT cp) { (void)cp; return TRUE; }
EXPORT BOOL WINAPI IsValidCodePage(UINT cp) { return cp == CP_UTF8 || cp == 437 || cp == 1252 || cp == CP_ACP || cp == CP_OEMCP; }
typedef struct { UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]; } CPINFO;
EXPORT BOOL WINAPI GetCPInfo(UINT cp, CPINFO *info) {
  (void)cp;
  memset(info, 0, sizeof(*info));
  info->MaxCharSize = 4;
  info->DefaultChar[0] = '?';
  return TRUE;
}
EXPORT int WINAPI lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
EXPORT int WINAPI lstrlenW(LPCWSTR s) { return s ? (int)wcslen(s) : 0; }
EXPORT LPSTR WINAPI lstrcpyA(LPSTR d, LPCSTR s) { char *r = d; while ((*d++ = *s++)) {} return r; }
EXPORT LPWSTR WINAPI lstrcpyW(LPWSTR d, LPCWSTR s) { WCHAR *r = d; while ((*d++ = *s++)) {} return r; }
EXPORT LPSTR WINAPI lstrcpynA(LPSTR d, LPCSTR s, int n) {
  if (n <= 0) return d;
  int i = 0;
  for (; i < n - 1 && s[i]; ++i) d[i] = s[i];
  d[i] = 0;
  return d;
}
EXPORT LPWSTR WINAPI lstrcpynW(LPWSTR d, LPCWSTR s, int n) {
  if (n <= 0) return d;
  int i = 0;
  for (; i < n - 1 && s[i]; ++i) d[i] = s[i];
  d[i] = 0;
  return d;
}
EXPORT LPSTR WINAPI lstrcatA(LPSTR d, LPCSTR s) { lstrcpyA(d + strlen(d), s); return d; }
EXPORT LPWSTR WINAPI lstrcatW(LPWSTR d, LPCWSTR s) { lstrcpyW(d + wcslen(d), s); return d; }
EXPORT int WINAPI lstrcmpA(LPCSTR a, LPCSTR b) { while (*a && *a == *b) ++a, ++b; return (BYTE)*a < (BYTE)*b ? -1 : (BYTE)*a > (BYTE)*b; }
EXPORT int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b) { while (*a && *a == *b) ++a, ++b; return *a < *b ? -1 : *a > *b; }
EXPORT int WINAPI lstrcmpiA(LPCSTR a, LPCSTR b) {
  for (;; ++a, ++b) {
    const int x = (BYTE)*a >= 'a' && (BYTE)*a <= 'z' ? *a - 32 : (BYTE)*a, y = (BYTE)*b >= 'a' && (BYTE)*b <= 'z' ? *b - 32 : (BYTE)*b;
    if (x != y) return x < y ? -1 : 1;
    if (!x) return 0;
  }
}
EXPORT int WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b) {
  for (;; ++a, ++b) {
    const int x = upper(*a), y = upper(*b);
    if (x != y) return x < y ? -1 : 1;
    if (!x) return 0;
  }
}
/* CompareString: 1 = less, 2 = equal, 3 = greater. NORM_IGNORECASE (1) is honoured, the other flags are not. */
EXPORT int WINAPI CompareStringW(DWORD locale, DWORD flags, LPCWSTR a, int an, LPCWSTR b, int bn) {
  (void)locale;
  if (an < 0) an = (int)wcslen(a);
  if (bn < 0) bn = (int)wcslen(b);
  for (int i = 0; i < an || i < bn; ++i) {
    const int x = i < an ? ((flags & 1) ? upper(a[i]) : a[i]) : -1, y = i < bn ? ((flags & 1) ? upper(b[i]) : b[i]) : -1;
    if (x != y) return x < y ? 1 : 3;
  }
  return 2;
}
EXPORT int WINAPI CompareStringA(DWORD locale, DWORD flags, LPCSTR a, int an, LPCSTR b, int bn) {
  (void)locale;
  if (an < 0) an = (int)strlen(a);
  if (bn < 0) bn = (int)strlen(b);
  for (int i = 0; i < an || i < bn; ++i) {
    int x = i < an ? (BYTE)a[i] : -1, y = i < bn ? (BYTE)b[i] : -1;
    if (flags & 1) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; }
    if (x != y) return x < y ? 1 : 3;
  }
  return 2;
}

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
  (void)maximum;
  PVOID h = RtlCreateHeap(options, NULL, 0, initial, NULL, NULL);
  if (!h) set_error(ERROR_NOT_ENOUGH_MEMORY);
  return h;
}
EXPORT BOOL WINAPI HeapDestroy(HANDLE h) { return RtlDestroyHeap(h) == NULL; }
EXPORT LPVOID WINAPI HeapAlloc(HANDLE h, DWORD flags, SIZE_T bytes) {
  PVOID p = RtlAllocateHeap(h, flags, bytes);
  if (!p) set_error(ERROR_NOT_ENOUGH_MEMORY);
  return p;
}
EXPORT LPVOID WINAPI HeapReAlloc(HANDLE h, DWORD flags, LPVOID memory, SIZE_T bytes) {
  PVOID p = RtlReAllocateHeap(h, flags, memory, bytes);
  if (!p) set_error(ERROR_NOT_ENOUGH_MEMORY);
  return p;
}
EXPORT BOOL WINAPI HeapFree(HANDLE h, DWORD flags, LPVOID memory) { return RtlFreeHeap(h, flags, memory) ? TRUE : (set_error(ERROR_INVALID_PARAMETER), FALSE); }
EXPORT SIZE_T WINAPI HeapSize(HANDLE h, DWORD flags, LPCVOID memory) { return RtlSizeHeap(h, flags, (PVOID)memory); }
EXPORT BOOL WINAPI HeapValidate(HANDLE h, DWORD flags, LPCVOID memory) { return RtlValidateHeap(h, flags, (PVOID)memory); }
EXPORT BOOL WINAPI HeapSetInformation(HANDLE h, int info_class, PVOID info, SIZE_T length) { (void)h; (void)info_class; (void)info; (void)length; return TRUE; }
EXPORT SIZE_T WINAPI HeapCompact(HANDLE h, DWORD flags) { (void)h; (void)flags; return 0; }
EXPORT BOOL WINAPI HeapLock(HANDLE h) { (void)h; return TRUE; }
EXPORT BOOL WINAPI HeapUnlock(HANDLE h) { (void)h; return TRUE; }
EXPORT HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T bytes) { return HeapAlloc(heap(), (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, bytes); }
EXPORT HLOCAL WINAPI LocalReAlloc(HLOCAL m, SIZE_T bytes, UINT flags) { return HeapReAlloc(heap(), (flags & LMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, m, bytes); }
EXPORT HLOCAL WINAPI LocalFree(HLOCAL m) { return m && !RtlFreeHeap(heap(), 0, m) ? m : NULL; }
EXPORT SIZE_T WINAPI LocalSize(HLOCAL m) { return RtlSizeHeap(heap(), 0, m); }
EXPORT HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T bytes) { return HeapAlloc(heap(), (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, bytes); }
EXPORT HGLOBAL WINAPI GlobalReAlloc(HGLOBAL m, SIZE_T bytes, UINT flags) { return HeapReAlloc(heap(), (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, m, bytes); }
EXPORT HGLOBAL WINAPI GlobalFree(HGLOBAL m) { return m && !RtlFreeHeap(heap(), 0, m) ? m : NULL; }
EXPORT SIZE_T WINAPI GlobalSize(HGLOBAL m) { return RtlSizeHeap(heap(), 0, m); }
EXPORT LPVOID WINAPI GlobalLock(HGLOBAL m) { return m; }
EXPORT BOOL WINAPI GlobalUnlock(HGLOBAL m) { (void)m; return FALSE; }
EXPORT LPVOID WINAPI LocalLock(HLOCAL m) { return m; }
EXPORT BOOL WINAPI LocalUnlock(HLOCAL m) { (void)m; return FALSE; }

/* ---- the process: ids, command line, environment, modules, version ---------------------------------------------------------------------------------------- */

EXPORT void WINAPI ExitProcess(UINT code) { RtlExitUserProcess((NTSTATUS)code); }
EXPORT BOOL WINAPI TerminateProcess(HANDLE process, UINT code) {
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
EXPORT BOOL WINAPI IsWow64Process(HANDLE h, LPBOOL wow) { (void)h; *wow = FALSE; return TRUE; }
EXPORT BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }
EXPORT BOOL WINAPI CheckRemoteDebuggerPresent(HANDLE h, LPBOOL present) { (void)h; *present = FALSE; return TRUE; }
EXPORT void WINAPI DebugBreak(void) { __asm__ volatile("int3"); }
EXPORT void WINAPI OutputDebugStringA(LPCSTR s) { (void)s; }
EXPORT void WINAPI OutputDebugStringW(LPCWSTR s) { (void)s; }
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

EXPORT LPWSTR WINAPI GetCommandLineW(void) { return parameters()->CommandLine.Buffer; }
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
  const WCHAR *value = find_variable(parameters()->Environment, name, &length);
  if (!value) { set_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
  if (length + 1 > capacity) return (DWORD)length + 1;
  memcpy(buffer, value, (length + 1) * 2);
  return (DWORD)length;
}
EXPORT DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD capacity) {
  WCHAR *w = to_wide(name);
  if (!w) return 0;
  size_t_ length = 0;
  const WCHAR *value = find_variable(parameters()->Environment, w, &length);
  free_wide(w);
  if (!value) { set_error(ERROR_ENVVAR_NOT_FOUND); return 0; }
  return wide_result_to_ansi(value, (DWORD)length, buffer, capacity);
}
/* Builds a new block with NAME set (or removed when value is NULL) and points the process parameters at it. */
EXPORT BOOL WINAPI SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
  if (!name || !*name || wcslen(name) > 32767) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  for (const WCHAR *c = name; *c; ++c) if (*c == '=') { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const WCHAR *old = parameters()->Environment;
  size_t_ old_units = 0;
  while (old[old_units] || old[old_units + 1]) ++old_units;
  old_units += 2;
  const size_t_ name_length = wcslen(name), value_length = value ? wcslen(value) : 0;
  WCHAR *fresh = RtlAllocateHeap(heap(), 0, (old_units + name_length + value_length + 4) * 2);
  if (!fresh) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  size_t_ out = 0;
  for (const WCHAR *e = old; *e; e += wcslen(e) + 1) {
    size_t_ i = 0;
    while (i < name_length && e[i] && upper(e[i]) == upper(name[i])) ++i;
    if (i == name_length && e[i] == '=') continue;      /* replaced or removed */
    const size_t_ n = wcslen(e) + 1;
    memcpy(fresh + out, e, n * 2);
    out += n;
  }
  if (value) {
    memcpy(fresh + out, name, name_length * 2);
    out += name_length;
    fresh[out++] = '=';
    memcpy(fresh + out, value, value_length * 2);
    out += value_length;
    fresh[out++] = 0;
  }
  fresh[out++] = 0;
  if (out == 1) fresh[out++] = 0;
  parameters()->Environment = fresh;
  return TRUE;
}
EXPORT BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
  WCHAR *n = to_wide(name), *v = value ? to_wide(value) : NULL;
  const BOOL ok = n ? SetEnvironmentVariableW(n, v) : FALSE;
  free_wide(n);
  free_wide(v);
  return ok;
}
EXPORT DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR source, LPWSTR dest, DWORD capacity) {
  DWORD needed = 0;
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
      if (needed + length < capacity && dest) memcpy(dest + needed, value, length * 2);
      needed += (DWORD)length;
      s = close + 1;
    } else {
      if (needed + 1 < capacity && dest) dest[needed] = *s;
      ++needed;
      ++s;
    }
  }
  if (dest && needed < capacity) dest[needed] = 0;
  return needed + 1;
}
EXPORT DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR source, LPSTR dest, DWORD capacity) {
  WCHAR *w = to_wide(source);
  if (!w) return 0;
  WCHAR wide[1024];
  const DWORD n = ExpandEnvironmentStringsW(w, wide, 1024);
  free_wide(w);
  if (n > 1024) return 0;
  return wide_result_to_ansi(wide, n - 1, dest, capacity) + 1;
}

EXPORT void WINAPI GetStartupInfoW(LPSTARTUPINFOW info) {
  memset(info, 0, sizeof(*info));
  info->cb = sizeof(*info);
  info->dwFlags = 0x100;      /* STARTF_USESTDHANDLES */
  info->hStdInput = parameters()->StandardInput;
  info->hStdOutput = parameters()->StandardOutput;
  info->hStdError = parameters()->StandardError;
}
EXPORT void WINAPI GetStartupInfoA(LPSTARTUPINFOA info) {
  memset(info, 0, sizeof(*info));
  info->cb = sizeof(*info);
  info->dwFlags = 0x100;
  info->hStdInput = parameters()->StandardInput;
  info->hStdOutput = parameters()->StandardOutput;
  info->hStdError = parameters()->StandardError;
}

EXPORT DWORD WINAPI GetVersion(void) { return (19045u << 16) | (0u << 8) | 10u; }
EXPORT BOOL WINAPI GetVersionExW(LPOSVERSIONINFOW v) {
  v->dwMajorVersion = 10; v->dwMinorVersion = 0; v->dwBuildNumber = 19045; v->dwPlatformId = 2; v->szCSDVersion[0] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA v) {
  v->dwMajorVersion = 10; v->dwMinorVersion = 0; v->dwBuildNumber = 19045; v->dwPlatformId = 2; v->szCSDVersion[0] = 0;
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
  m->ullAvailVirtual = m->ullTotalVirtual;      /* (the address space is not tracked in bytes left) */
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
  MEMORYSTATUSEX x;
  x.dwLength = sizeof(x);
  if (!GlobalMemoryStatusEx(&x)) return FALSE;
  *kilobytes = x.ullTotalPhys / 1024;
  return TRUE;
}
EXPORT BOOL WINAPI IsProcessorFeaturePresent(DWORD feature) { return feature == 6 || feature == 10; }
EXPORT BOOL WINAPI GetComputerNameW(LPWSTR name, LPDWORD size) {
  static const WCHAR kName[] = {'T', 'C', 'O', 'S', 0};
  if (*size < 5) { *size = 5; set_error(ERROR_MORE_DATA); return FALSE; }
  memcpy(name, kName, sizeof(kName));
  *size = 4;
  return TRUE;
}
EXPORT BOOL WINAPI GetComputerNameA(LPSTR name, LPDWORD size) {
  if (*size < 5) { *size = 5; set_error(ERROR_MORE_DATA); return FALSE; }
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
static LDR_DATA_TABLE_ENTRY *module_by_name(LPCWSTR name) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr) return NULL;
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
    if (i == length) return m;
  }
  return NULL;
}
EXPORT HMODULE WINAPI GetModuleHandleW(LPCWSTR name) {
  if (!name) return NtCurrentPeb()->ImageBaseAddress;
  LDR_DATA_TABLE_ENTRY *m = module_by_name(name);
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
    case 0xC000007Bu: return ERROR_BAD_EXE_FORMAT;
    case 0xC0000142u: return 1114;      /* ERROR_DLL_INIT_FAILED */
    case 0xC00000BBu: return ERROR_NOT_SUPPORTED;
    case 0xC0000017u: return ERROR_NOT_ENOUGH_MEMORY;
    default: return RtlNtStatusToDosError(status);
  }
}
EXPORT BOOL WINAPI GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *module) {
  LDR_DATA_TABLE_ENTRY *m = (flags & 4) ? module_containing((PVOID)name) : (name ? module_by_name(name) : module_by_base(NtCurrentPeb()->ImageBaseAddress));
  if (module) *module = NULL;
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
  const DWORD length = m->FullDllName.Length / 2;
  if (length + 1 > capacity) {
    if (capacity) { memcpy(buffer, m->FullDllName.Buffer, (capacity - 1) * 2); buffer[capacity - 1] = 0; }
    set_error(ERROR_INSUFFICIENT_BUFFER);
    return capacity;
  }
  memcpy(buffer, m->FullDllName.Buffer, length * 2);
  buffer[length] = 0;
  return length;
}
EXPORT DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD capacity) {
  WCHAR wide[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(module, wide, MAX_PATH * 2);
  if (n == 0) return 0;
  const DWORD r = wide_result_to_ansi(wide, n, buffer, capacity);
  if (r > capacity) { if (capacity) buffer[capacity - 1] = 0; set_error(ERROR_INSUFFICIENT_BUFFER); return capacity; }
  return r;
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
    if (function[0] == '#') {
      DWORD number = 0;
      for (const char *c = function + 1; *c >= '0' && *c <= '9'; ++c) number = number * 10 + (DWORD)(*c - '0');
      return export_of(target->DllBase, NULL, number, depth + 1);
    }
    return export_of(target->DllBase, function, 0, depth + 1);
  }
  return (FARPROC)(base + rva);
}
EXPORT FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name) {
  FARPROC p = ((ULONG_PTR)name >> 16) == 0 ? export_of(module, NULL, (DWORD)(ULONG_PTR)name, 0) : export_of(module, name, 0, 0);
  if (!p) set_error(ERROR_PROC_NOT_FOUND);
  return p;
}
static BOOL find_file(LPCWSTR name, LPCWSTR extension, LPCWSTR extra, WCHAR *out);
static void unicode_string(UNICODE_STRING *s, LPCWSTR text);
static WCHAR g_dll_directory[PATH_CHARS];      /* SetDllDirectory */

EXPORT BOOL WINAPI SetDllDirectoryW(LPCWSTR directory) {
  if (directory && wcslen(directory) >= PATH_CHARS) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
  if (directory) lstrcpyW(g_dll_directory, directory); else g_dll_directory[0] = 0;
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
  if (n + 1 > capacity) return n + 1;
  memcpy(buffer, g_dll_directory, (n + 1) * 2);
  return n;
}
EXPORT DWORD WINAPI GetDllDirectoryA(DWORD capacity, LPSTR buffer) {
  const DWORD n = (DWORD)wcslen(g_dll_directory);
  if (n + 1 > capacity) return n + 1;
  return wide_result_to_ansi(g_dll_directory, n, buffer, capacity);
}

/* LoadLibrary: a library the process has already (by its name) gets one more use; otherwise the file is looked for and ntdll maps it, with the libraries it imports, and
   calls their entry points. Libraries with thread-local data of their own (__declspec(thread) in a DLL) and the data-file modes are refused. */
EXPORT HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
  (void)file;
  if (!name || !name[0]) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  if (flags & 0x32) { set_error(ERROR_NOT_SUPPORTED); return NULL; }      /* AS_DATAFILE 2, AS_DATAFILE_EXCLUSIVE 0x10, AS_IMAGE_RESOURCE 0x20 */
  WCHAR path[PATH_CHARS];
  UNICODE_STRING us;
  /* The file, if there is one; otherwise the bare name (a library the process has already may be known by an alias, "api-ms-win-crt-runtime-l1-1-0.dll"): the kernel knows
     which libraries are loaded, and decides under the loader lock. */
  static const WCHAR dll[] = {'.', 'd', 'l', 'l', 0};
  if (find_file(name, dll, g_dll_directory, path)) unicode_string(&us, path);
  else unicode_string(&us, name);
  PVOID handle = NULL;
  const NTSTATUS status = LdrLoadDll(NULL, NULL, &us, &handle);
  if (!NT_SUCCESS(status)) { set_error(dos_error_of_load(status)); return NULL; }
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
  if (!module_by_base(module)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  const NTSTATUS status = LdrUnloadDll(module);
  return NT_SUCCESS(status) ? TRUE : (set_error(dos_error_of_load(status)), FALSE);
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

#define FLS_SLOTS 128
static PVOID g_fls_values[FLS_SLOTS];
static BYTE g_fls_used[FLS_SLOTS];
EXPORT DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION callback) {
  (void)callback;      /* (callbacks run when a fiber ends: there are no fibers) */
  for (DWORD i = 0; i < FLS_SLOTS; ++i) {
    if (g_fls_used[i]) continue;
    g_fls_used[i] = 1;
    g_fls_values[i] = NULL;
    return i;
  }
  set_error(ERROR_NO_MORE_ITEMS);
  return TLS_OUT_OF_INDEXES;
}
EXPORT BOOL WINAPI FlsFree(DWORD index) {
  if (index >= FLS_SLOTS || !g_fls_used[index]) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  g_fls_used[index] = 0;
  return TRUE;
}
EXPORT PVOID WINAPI FlsGetValue(DWORD index) {
  if (index >= FLS_SLOTS || !g_fls_used[index]) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  set_error(ERROR_SUCCESS);
  return g_fls_values[index];
}
EXPORT BOOL WINAPI FlsSetValue(DWORD index, PVOID value) {
  if (index >= FLS_SLOTS || !g_fls_used[index]) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  g_fls_values[index] = value;
  return TRUE;
}

/* ---- synchronization: one thread, so these count and never wait ------------------------------------------------------------------------------------------ */

EXPORT void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs) { RtlInitializeCriticalSection(cs); }
EXPORT BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin) { (void)spin; RtlInitializeCriticalSection(cs); return TRUE; }
EXPORT BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags) { (void)spin; (void)flags; RtlInitializeCriticalSection(cs); return TRUE; }
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

EXPORT void WINAPI InitializeSListHead(PSLIST_HEADER head) { head->Alignment = head->Region = 0; }
EXPORT PSLIST_ENTRY WINAPI InterlockedPushEntrySList(PSLIST_HEADER head, PSLIST_ENTRY entry) {
  PSLIST_ENTRY old = head->Next;
  entry->Next = old;
  head->Next = entry;
  ++head->Depth;
  return old;
}
EXPORT PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER head) {
  PSLIST_ENTRY top = head->Next;
  if (top) { head->Next = top->Next; --head->Depth; }
  return top;
}
EXPORT PSLIST_ENTRY WINAPI InterlockedFlushSList(PSLIST_HEADER head) {
  PSLIST_ENTRY top = head->Next;
  head->Next = NULL;
  head->Depth = 0;
  return top;
}
EXPORT USHORT WINAPI QueryDepthSList(PSLIST_HEADER head) { return (USHORT)head->Depth; }

EXPORT BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN function, PVOID parameter, PVOID *context) {
  if ((ULONG_PTR)once->Ptr & 2) { if (context) *context = (PVOID)((ULONG_PTR)once->Ptr & ~3ull); return TRUE; }
  PVOID result = NULL;
  if (!function(once, parameter, &result)) return FALSE;
  once->Ptr = (PVOID)(((ULONG_PTR)result & ~3ull) | 2);
  if (context) *context = result;
  return TRUE;
}
EXPORT BOOL WINAPI InitOnceBeginInitialize(PINIT_ONCE once, DWORD flags, PBOOL pending, PVOID *context) {
  (void)flags;
  if ((ULONG_PTR)once->Ptr & 2) { *pending = FALSE; if (context) *context = (PVOID)((ULONG_PTR)once->Ptr & ~3ull); return TRUE; }
  *pending = TRUE;
  return TRUE;
}
EXPORT BOOL WINAPI InitOnceComplete(PINIT_ONCE once, DWORD flags, PVOID context) {
  (void)flags;
  once->Ptr = (PVOID)(((ULONG_PTR)context & ~3ull) | 2);
  return TRUE;
}

EXPORT void WINAPI Sleep(DWORD ms);
EXPORT ULONGLONG WINAPI GetTickCount64(void);

/* Slim reader/writer locks and condition variables for the threads of the process: waiting threads give their time slice away (condition variables wake all waiters,
 * which the contract allows: a waiter rechecks its condition). */
EXPORT void WINAPI InitializeSRWLock(PSRWLOCK lock) { lock->Ptr = NULL; }
EXPORT void WINAPI AcquireSRWLockExclusive(PSRWLOCK lock) { while (!__sync_bool_compare_and_swap((ULONG_PTR *)&lock->Ptr, 0, 1)) NtYieldExecution(); }
EXPORT BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK lock) { return __sync_bool_compare_and_swap((ULONG_PTR *)&lock->Ptr, 0, 1); }
EXPORT void WINAPI ReleaseSRWLockExclusive(PSRWLOCK lock) { __sync_lock_release((ULONG_PTR *)&lock->Ptr); }
EXPORT BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK lock) {
  const ULONG_PTR v = (ULONG_PTR)lock->Ptr;
  return !(v & 1) && __sync_bool_compare_and_swap((ULONG_PTR *)&lock->Ptr, v, v + 2);
}
EXPORT void WINAPI AcquireSRWLockShared(PSRWLOCK lock) { while (!TryAcquireSRWLockShared(lock)) NtYieldExecution(); }
EXPORT void WINAPI ReleaseSRWLockShared(PSRWLOCK lock) { __sync_fetch_and_sub((ULONG_PTR *)&lock->Ptr, 2); }
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
  if ((name[0] == 'L' || name[0] == 'l') && (name[1] == 'o' || name[1] == 'O') && (name[2] == 'c' || name[2] == 'C') && (name[3] == 'a' || name[3] == 'A') && (name[4] == 'l' || name[4] == 'L') && name[5] == '\\') skip = name + 6;
  else if ((name[0] == 'G' || name[0] == 'g') && (name[1] == 'l' || name[1] == 'L') && (name[2] == 'o' || name[2] == 'O') && (name[3] == 'b' || name[3] == 'B') && (name[4] == 'a' || name[4] == 'A') && (name[5] == 'l' || name[5] == 'L') && name[6] == '\\') skip = name + 7;
  if (!name || !*name) length = 0;      /* (only the inherit flag was asked for) */
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
  (void)access;
  NamedAttributes n;
  const BOOL inherit_handle = inherit;
  HANDLE h = NULL;
  { const NTSTATUS status = NtOpenEvent(&h, 0x1F0003, named_attributes(&n, name, inherit_handle)); return opened(status, h); }
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
  (void)access;
  NamedAttributes n;
  const BOOL inherit_handle = inherit;
  HANDLE h = NULL;
  { const NTSTATUS status = NtOpenMutant(&h, 0x1F0001, named_attributes(&n, name, inherit_handle)); return opened(status, h); }
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
  (void)access;
  NamedAttributes n;
  const BOOL inherit_handle = inherit;
  HANDLE h = NULL;
  { const NTSTATUS status = NtOpenSemaphore(&h, 0x1F0003, named_attributes(&n, name, inherit_handle)); return opened(status, h); }
}
EXPORT HANDLE WINAPI OpenSemaphoreA(DWORD access, BOOL inherit, LPCSTR name) {
  WCHAR *w = name ? to_wide(name) : NULL;
  HANDLE h = OpenSemaphoreW(access, inherit, w);
  free_wide(w);
  return h;
}
EXPORT BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG count, PLONG previous) { const NTSTATUS s = NtReleaseSemaphore(h, count, previous); return NT_SUCCESS(s) ? TRUE : fail_nt(s); }

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
/* ReadFileEx / WriteFileEx: the operation is done at once; the completion routine is an APC for this thread, called when it next waits alertably. */
typedef void (WINAPI *LPOVERLAPPED_COMPLETION_ROUTINE_)(DWORD, DWORD, LPOVERLAPPED);
static void NTAPI completion_apc(ULONG_PTR routine, ULONG_PTR overlapped, ULONG_PTR unused) {
  (void)unused;
  LPOVERLAPPED o = (LPOVERLAPPED)overlapped;
  ((LPOVERLAPPED_COMPLETION_ROUTINE_)routine)(NT_SUCCESS((NTSTATUS)(ULONG)o->Internal) ? 0 : RtlNtStatusToDosError((NTSTATUS)(ULONG)o->Internal), (DWORD)o->InternalHigh, o);
}
EXPORT BOOL WINAPI ReadFileEx(HANDLE h, LPVOID buffer, DWORD length, LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE_ routine) {
  if (!overlapped || !routine) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  DWORD got = 0;
  const BOOL ok = ReadFile(h, buffer, length, &got, overlapped);
  if (!ok && GetLastError() != ERROR_HANDLE_EOF) return FALSE;
  NtQueueApcThread(NtCurrentThread(), (PVOID)completion_apc, (ULONG_PTR)routine, (ULONG_PTR)overlapped, 0);
  return TRUE;
}
EXPORT BOOL WINAPI WriteFileEx(HANDLE h, LPCVOID buffer, DWORD length, LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE_ routine) {
  if (!overlapped || !routine) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  DWORD done = 0;
  if (!WriteFile(h, buffer, length, &done, overlapped)) return FALSE;
  NtQueueApcThread(NtCurrentThread(), (PVOID)completion_apc, (ULONG_PTR)routine, (ULONG_PTR)overlapped, 0);
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
  (void)access;
  NamedAttributes n;
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenTimer(&h, 0x1F0003, named_attributes(&n, name, inherit));
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
  const NTSTATUS status = NtCreateSection(&h, SECTION_ALL_, named_attributes(&n, name, sa && sa->bInheritHandle), size_pointer, page, 0x8000000 /* SEC_COMMIT */, file);
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
  (void)access;
  NamedAttributes n;
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenSection(&h, SECTION_ALL_, named_attributes(&n, name, inherit));
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
  ULONG page = (access & FILE_MAP_WRITE_) ? PAGE_READWRITE : (access & FILE_MAP_COPY_) ? 0x08 /* PAGE_WRITECOPY */ : 0x02 /* PAGE_READONLY */;
  if (access & FILE_MAP_EXECUTE_) page = page == PAGE_READWRITE ? 0x40 : page == 0x08 ? 0x80 : 0x20;
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
static BOOL is_this_process(HANDLE process) {
  if (process == NtCurrentProcess()) return TRUE;
  ULONGLONG info[6];
  if (!NT_SUCCESS(NtQueryInformationProcess(process, 0, info, sizeof(info), NULL))) return FALSE;
  return (DWORD)info[4] == GetCurrentProcessId();
}
EXPORT LPVOID WINAPI VirtualAllocEx(HANDLE process, LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
  if (!is_this_process(process)) { set_error(ERROR_NOT_SUPPORTED); return NULL; }
  return VirtualAlloc(address, size, type, protect);
}
EXPORT BOOL WINAPI VirtualFreeEx(HANDLE process, LPVOID address, SIZE_T size, DWORD type) {
  if (!is_this_process(process)) { set_error(ERROR_NOT_SUPPORTED); return FALSE; }
  return VirtualFree(address, size, type);
}
EXPORT BOOL WINAPI VirtualProtectEx(HANDLE process, LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
  if (!is_this_process(process)) { set_error(ERROR_NOT_SUPPORTED); return FALSE; }
  return VirtualProtect(address, size, protect, old);
}
EXPORT SIZE_T WINAPI VirtualQueryEx(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length) {
  if (!is_this_process(process)) { set_error(ERROR_NOT_SUPPORTED); return 0; }
  return VirtualQuery(address, info, length);
}

EXPORT HANDLE WINAPI OpenThread(DWORD access, BOOL inherit, DWORD id) {
  (void)access;
  OBJECT_ATTRIBUTES attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.Length = sizeof(attributes);
  attributes.Attributes = inherit ? 2 : 0;
  ULONG64 client[2] = {GetCurrentProcessId(), id};
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenThread(&h, 0x1F03FF, &attributes, client);
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
EXPORT DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD timeout_ms, BOOL alertable) {
  if (con_is_input(h)) {      /* the console input is ready when a key event is waiting: asked again until it is */
    const DWORD start = GetTickCount();
    while (!con_input_ready()) {
      if (timeout_ms != INFINITE && GetTickCount() - start >= timeout_ms) return WAIT_TIMEOUT;
      SleepEx(5, alertable);
    }
    return WAIT_OBJECT_0;
  }
  LARGE_INTEGER t;
  t.QuadPart = -(LONGLONG)timeout_ms * 10000;
  return wait_result(NtWaitForSingleObject(h, alertable != 0, timeout_ms == INFINITE ? NULL : &t));
}
EXPORT DWORD WINAPI WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout_ms, BOOL alertable) {
  if (count == 0 || count > 64 || !handles) { set_error(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
  for (DWORD i = 0; i < count && !all; ++i) {
    if (!con_is_input(handles[i])) continue;
    HANDLE others[64];      /* a console input among the handles: the others are asked without waiting, in turns with it */
    DWORD index[64], n = 0;
    for (DWORD k = 0; k < count; ++k) if (!con_is_input(handles[k])) { others[n] = handles[k]; index[n++] = k; }
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
EXPORT HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD id) {
  (void)sa;
  HANDLE thread = NULL;
  const NTSTATUS status = NtCreateThreadEx(&thread, 0x1FFFFF, NULL, NtCurrentProcess(), (PVOID)start, parameter, (flags & CREATE_SUSPENDED) ? 1 : 0, 0, stack, stack, NULL);
  if (!NT_SUCCESS(status)) { set_error(status == STATUS_NOT_IMPLEMENTED ? ERROR_NOT_SUPPORTED : RtlNtStatusToDosError(status)); return NULL; }
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
  LONG value = priority;
  const NTSTATUS status = NtSetInformationThread(h, 3, &value, sizeof(value));      /* ThreadBasePriority */
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT int WINAPI GetThreadPriority(HANDLE h) {
  BYTE info[48];
  const NTSTATUS status = NtQueryInformationThread(h, 0, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) { fail_nt(status); return THREAD_PRIORITY_ERROR_RETURN; }
  return (LONG)get32(info + 44);      /* BasePriority */
}
EXPORT DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
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
EXPORT BOOL WINAPI QueryFullProcessImageNameW(HANDLE h, DWORD flags, LPWSTR name, PDWORD size) {
  (void)flags;
  BYTE info[16 + 2 * (MAX_PATH * 2 + 1)];
  const NTSTATUS status = NtQueryInformationProcess(h, 27, info, sizeof(info), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  const DWORD n = get16(info) / 2;
  if (n + 1 > *size) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  memcpy(name, (WCHAR *)(ULONG_PTR)get64(info + 8), (n + 1) * 2);
  *size = n;
  return TRUE;
}
EXPORT BOOL WINAPI QueryFullProcessImageNameA(HANDLE h, DWORD flags, LPSTR name, PDWORD size) {
  WCHAR wide[MAX_PATH * 2];
  DWORD n = MAX_PATH * 2;
  if (!QueryFullProcessImageNameW(h, flags, wide, &n)) return FALSE;
  const DWORD got = (DWORD)WideCharToMultiByte(CP_UTF8, 0, wide, (int)n + 1, name, (int)*size, NULL, NULL);
  if (got == 0) { set_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  *size = got - 1;
  return TRUE;
}
EXPORT HANDLE WINAPI OpenProcess(DWORD access, BOOL inherit, DWORD id) {
  (void)access;
  OBJECT_ATTRIBUTES attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.Length = sizeof(attributes);
  attributes.Attributes = inherit ? 2 : 0;
  ULONG64 client[2] = {id, 0};
  HANDLE h = NULL;
  const NTSTATUS status = NtOpenProcess(&h, 0x1FFFFF, &attributes, client);
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

EXPORT BOOL WINAPI CreateProcessW(LPCWSTR application, LPWSTR command_line, LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes,
                                  BOOL inherit_handles, DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) {
  (void)process_attributes; (void)thread_attributes;
  if (!application && !command_line) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR name[MAX_PATH], image[PATH_CHARS];
  if (application) {
    if (wcslen(application) >= MAX_PATH) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    lstrcpyW(name, application);
  } else {      /* the first word of the command line (in quotes, or up to a space) */
    LPCWSTR c = command_line;
    while (*c == ' ') ++c;
    size_t_ n = 0;
    const BOOL quoted = *c == '"';
    if (quoted) ++c;
    while (*c && (quoted ? *c != '"' : *c != ' ')) { if (n + 1 >= MAX_PATH) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; } name[n++] = *c++; }
    name[n] = 0;
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
  ULONG nt_flags = ((flags & CREATE_SUSPENDED) ? 1 : 0) | (inherit_handles ? 2 : 0);
  HANDLE std_in = NULL, std_out = NULL, std_err = NULL;
  if (startup && (startup->dwFlags & 0x100)) {
    nt_flags |= 4;
    std_in = startup->hStdInput;
    std_out = startup->hStdOutput;
    std_err = startup->hStdError;
  }
  const NTSTATUS status = NtCreateUserProcess(&process, &thread, &image_name, &command, cwd_pointer, environment ? environment : GetEnvironmentStringsW(), nt_flags, &process_id,
                                              &thread_id, std_in, std_out, std_err);
  if (!NT_SUCCESS(status)) {
    if (status == (NTSTATUS)0xC000007B) set_error(ERROR_BAD_EXE_FORMAT);
    else fail_nt(status);
    return FALSE;
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
  WCHAR *wide_environment = NULL;
  BOOL ok = FALSE;
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
  ok = CreateProcessW(wide_application, wide_command, process_attributes, thread_attributes, inherit_handles, flags | CREATE_UNICODE_ENVIRONMENT, wide_environment, wide_directory, (LPSTARTUPINFOW)startup, information);
done:
  free_wide(wide_application);
  free_wide(wide_command);
  free_wide(wide_directory);
  free_wide(wide_environment);
  return ok;
}
EXPORT BOOL WINAPI GetThreadContext(HANDLE h, CONTEXT *context) { const NTSTATUS status = NtGetContextThread(h, context); return NT_SUCCESS(status) ? TRUE : fail_nt(status); }
EXPORT BOOL WINAPI SetThreadContext(HANDLE h, const CONTEXT *context) { const NTSTATUS status = NtSetContextThread(h, context); return NT_SUCCESS(status) ? TRUE : fail_nt(status); }

/* ---- handles, time --------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI CloseHandle(HANDLE h) {
  const NTSTATUS status = NtClose(h);
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
  if (st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31 || st->wHour > 23 || st->wMinute > 59 || st->wSecond > 59 || st->wMilliseconds > 999) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  const LONGLONG days = days_from_civil(st->wYear, st->wMonth, st->wDay) + 134774;
  const ULONGLONG ms = (ULONGLONG)days * 86400000ull + (ULONGLONG)st->wHour * 3600000ull + (ULONGLONG)st->wMinute * 60000ull + (ULONGLONG)st->wSecond * 1000ull + st->wMilliseconds;
  const ULONGLONG t = ms * 10000;
  ft->dwLowDateTime = (DWORD)t;
  ft->dwHighDateTime = (DWORD)(t >> 32);
  return TRUE;
}
EXPORT void WINAPI GetSystemTime(LPSYSTEMTIME st) { FILETIME ft; GetSystemTimeAsFileTime(&ft); FileTimeToSystemTime(&ft, st); }
EXPORT void WINAPI GetLocalTime(LPSYSTEMTIME st) { GetSystemTime(st); }      /* the time zone is UTC */
EXPORT BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *in, LPFILETIME out) { *out = *in; return TRUE; }
EXPORT BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *in, LPFILETIME out) { *out = *in; return TRUE; }
EXPORT DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz) { memset(tz, 0, sizeof(*tz)); return 0; /* TIME_ZONE_ID_UNKNOWN */ }
/* Exception handling lives in ntdll.dll (src/Windows/ntdll/except.inc); kernel32 gives programs the names they know. RtlUnwindEx, RtlVirtualUnwind and the others
 * are forwarded to ntdll by kernel32.def. */
EXPORT LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) { return RtlSetUnhandledExceptionFilter(filter); }
EXPORT LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS info) {
  LPTOP_LEVEL_EXCEPTION_FILTER filter = RtlSetUnhandledExceptionFilter(NULL);      /* (read it, put it back) */
  RtlSetUnhandledExceptionFilter(filter);
  return filter ? filter(info) : EXCEPTION_EXECUTE_HANDLER;
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
  if (i < 0) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (g_std_overridden[i]) return g_std_override[i];
  RTL_USER_PROCESS_PARAMETERS *p = parameters();
  return i == 0 ? p->StandardInput : i == 1 ? p->StandardOutput : p->StandardError;
}
EXPORT BOOL WINAPI SetStdHandle(DWORD which, HANDLE h) {
  const int i = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
  if (i < 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  g_std_override[i] = h;
  g_std_overridden[i] = TRUE;
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
  const int type = device_type(h);
  if (type < 0) { set_error(ERROR_INVALID_HANDLE); return FILE_TYPE_UNKNOWN; }
  set_error(ERROR_SUCCESS);
  return type == 0x50 ? FILE_TYPE_CHAR : type == 0x11 ? FILE_TYPE_PIPE : FILE_TYPE_DISK;
}

EXPORT BOOL WINAPI WriteFile(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPOVERLAPPED overlapped) {
  if (!overlapped) { BOOL handled; const BOOL result = console_write_hook(h, buffer, length, written, &handled); if (handled) return result; }
  IO_STATUS_BLOCK iosb;
  LARGE_INTEGER offset;
  PLARGE_INTEGER pointer = NULL;
  if (overlapped) {
    offset.LowPart = overlapped->Offset;
    offset.HighPart = (LONG)overlapped->OffsetHigh;
    pointer = &offset;
  }
  if (written) *written = 0;
  iosb.Information = 0;
  const NTSTATUS status = NtWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)buffer, length, pointer, NULL);
  if (!NT_SUCCESS(status)) { complete_overlapped(overlapped, status, 0); return fail_nt(status); }
  if (written) *written = (DWORD)iosb.Information;
  complete_overlapped(overlapped, status, iosb.Information);
  return TRUE;
}
EXPORT BOOL WINAPI GetOverlappedResult(HANDLE h, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL wait) {
  (void)h; (void)wait;      /* (the operation is finished by the time the call that started it returned) */
  if (!overlapped) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (transferred) *transferred = (DWORD)overlapped->InternalHigh;
  const NTSTATUS status = (NTSTATUS)(ULONG)overlapped->Internal;
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI GetOverlappedResultEx(HANDLE h, LPOVERLAPPED overlapped, LPDWORD transferred, DWORD ms, BOOL alertable) { (void)ms; (void)alertable; return GetOverlappedResult(h, overlapped, transferred, TRUE); }

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
  if (!name || !pipe_nt_name(name, buffer, &us, &attributes, sa && sa->bInheritHandle)) return INVALID_HANDLE_VALUE;
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
  if (!w) { if (name) return INVALID_HANDLE_VALUE; set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  HANDLE h = CreateNamedPipeW(w, open_mode, pipe_mode, max_instances, out_size, in_size, timeout_ms, sa);
  free_wide(w);
  return h;
}

#define FSCTL_PIPE_DISCONNECT_ 0x110004
#define FSCTL_PIPE_LISTEN_ 0x110008
#define FSCTL_PIPE_PEEK_ 0x11400C
#define FSCTL_PIPE_WAIT_ 0x110018
#define FSCTL_PIPE_TRANSCEIVE_ 0x11C017

EXPORT BOOL WINAPI ConnectNamedPipe(HANDLE h, LPOVERLAPPED overlapped) {
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtFsControlFile(h, NULL, NULL, NULL, &iosb, FSCTL_PIPE_LISTEN_, NULL, 0, NULL, 0);
  complete_overlapped(overlapped, status, 0);
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
    if (left) *left = 0;
  } else {
    fail_nt(status);
  }
  if (out != stack) RtlFreeHeap(heap(), 0, out);
  return ok;
}
EXPORT BOOL WINAPI TransactNamedPipe(HANDLE h, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD read, LPOVERLAPPED overlapped) {
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
EXPORT BOOL WINAPI CallNamedPipeW(LPCWSTR name, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD read, DWORD timeout_ms) {
  HANDLE h;
  for (;;) {
    h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) break;
    if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name, timeout_ms)) return FALSE;
  }
  const BOOL ok = TransactNamedPipe(h, in, in_size, out, out_size, read, NULL);
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
  if (flags) *flags = (get32(info + 36) ? 1 : 0);      /* PIPE_SERVER_END 1 (client 0); byte type */
  if (out_size) *out_size = get32(info + 24);
  if (in_size) *in_size = get32(info + 16);
  if (max_instances) *max_instances = get32(info + 8) >= 255 ? 255 : get32(info + 8);
  return TRUE;
}
EXPORT BOOL WINAPI GetNamedPipeHandleStateW(HANDLE h, LPDWORD state, LPDWORD instances, LPDWORD collect, LPDWORD timeout, LPWSTR user, DWORD user_size) {
  IO_STATUS_BLOCK iosb;
  BYTE mode[8], info[40];
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
  if (user && user_size) user[0] = 0;
  return GetNamedPipeHandleStateW(h, state, instances, collect, timeout, NULL, 0);
}
EXPORT BOOL WINAPI SetNamedPipeHandleState(HANDLE h, LPDWORD mode, LPDWORD collect, LPDWORD timeout) {
  (void)collect; (void)timeout;
  if (!mode) return TRUE;
  IO_STATUS_BLOCK iosb;
  BYTE info[8] = {0};
  const DWORD completion = (*mode & 1) ? 1 : 0;      /* PIPE_NOWAIT: reads and writes fail instead of waiting */
  memcpy(info + 4, &completion, 4);
  const NTSTATUS status = NtSetInformationFile(h, &iosb, info, sizeof(info), 23);
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
EXPORT BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPOVERLAPPED overlapped) {
  if (!overlapped) { BOOL handled; const BOOL result = console_read_hook(h, buffer, length, read, &handled); if (handled) return result; }
  IO_STATUS_BLOCK iosb;
  LARGE_INTEGER offset;
  PLARGE_INTEGER pointer = NULL;
  if (overlapped) {
    offset.LowPart = overlapped->Offset;
    offset.HighPart = (LONG)overlapped->OffsetHigh;
    pointer = &offset;
  }
  if (read) *read = 0;
  iosb.Information = 0;
  const NTSTATUS status = NtReadFile(h, NULL, NULL, NULL, &iosb, buffer, length, pointer, NULL);
  if (status == STATUS_END_OF_FILE) { complete_overlapped(overlapped, status, 0); return overlapped ? (set_error(ERROR_HANDLE_EOF), FALSE) : TRUE; }      /* (an overlapped read says so) */
  if (!NT_SUCCESS(status)) { complete_overlapped(overlapped, status, 0); return fail_nt(status); }
  if (read) *read = (DWORD)iosb.Information;
  complete_overlapped(overlapped, status, iosb.Information);
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

/* The NT name ("\??\C:\dir\file") for a Win32 path, and an OBJECT_ATTRIBUTES for it. `buffer` needs PATH_CHARS + 8 characters. */
static BOOL nt_name(LPCWSTR dos, WCHAR *buffer, UNICODE_STRING *name, OBJECT_ATTRIBUTES *attributes) {
  buffer[0] = '\\'; buffer[1] = '?'; buffer[2] = '?'; buffer[3] = '\\';
  const DWORD n = full_path_w(dos, buffer + 4, PATH_CHARS);
  if (n == 0) return FALSE;
  name->Buffer = buffer;
  name->Length = (USHORT)((n + 4) * 2);
  name->MaximumLength = (USHORT)(name->Length + 2);
  attributes->Length = sizeof(OBJECT_ATTRIBUTES);
  attributes->RootDirectory = NULL;
  attributes->ObjectName = name;
  attributes->Attributes = 0x40;      /* OBJ_CASE_INSENSITIVE */
  attributes->SecurityDescriptor = attributes->SecurityQualityOfService = NULL;
  return TRUE;
}

EXPORT DWORD WINAPI GetFullPathNameW(LPCWSTR path, DWORD capacity, LPWSTR buffer, LPWSTR *file_part) {
  WCHAR full[PATH_CHARS];
  const DWORD n = full_path_w(path, full, PATH_CHARS);
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
  const DWORD n = full_path_w(w, full, PATH_CHARS);
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
EXPORT DWORD WINAPI GetLongPathNameW(LPCWSTR path, LPWSTR buffer, DWORD capacity) { return GetFullPathNameW(path, capacity, buffer, NULL); }
EXPORT DWORD WINAPI GetLongPathNameA(LPCSTR path, LPSTR buffer, DWORD capacity) { return GetFullPathNameA(path, capacity, buffer, NULL); }
EXPORT DWORD WINAPI GetShortPathNameW(LPCWSTR path, LPWSTR buffer, DWORD capacity) { return GetFullPathNameW(path, capacity, buffer, NULL); }

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
  WCHAR *fresh = RtlAllocateHeap(heap(), 0, (SIZE_T)(length + 1) * 2);
  if (!fresh) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  memcpy(fresh, full, (SIZE_T)(length + 1) * 2);
  UNICODE_STRING *cwd = &parameters()->CurrentDirectoryPath;
  cwd->Buffer = fresh;
  cwd->Length = (USHORT)(length * 2);
  cwd->MaximumLength = (USHORT)(length * 2 + 2);
  return TRUE;
}
EXPORT BOOL WINAPI SetCurrentDirectoryA(LPCSTR path) {
  WCHAR *w = to_wide(path);
  const BOOL ok = w ? SetCurrentDirectoryW(w) : FALSE;
  free_wide(w);
  return ok;
}

EXPORT UINT WINAPI GetDriveTypeW(LPCWSTR root) { return (root && upper(root[0]) == 'C') ? 3 /* DRIVE_FIXED */ : 1 /* DRIVE_NO_ROOT_DIR */; }
EXPORT UINT WINAPI GetDriveTypeA(LPCSTR root) { return (root && (root[0] == 'C' || root[0] == 'c')) ? 3 : 1; }
EXPORT DWORD WINAPI GetLogicalDrives(void) { return 1u << 2; }
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
EXPORT DWORD WINAPI GetTempPathW(DWORD capacity, LPWSTR buffer) {
  static const WCHAR kPath[] = {'C', ':', '\\', 'T', 'e', 'm', 'p', '\\', 0};
  if (capacity < 9) return 9;
  memcpy(buffer, kPath, sizeof(kPath));
  return 8;
}
EXPORT DWORD WINAPI GetTempPathA(DWORD capacity, LPSTR buffer) {
  if (capacity < 9) return 9;
  memcpy(buffer, "C:\\Temp\\", 9);
  return 8;
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
  if (!path) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  /* The console by its device names. */
  if (equals_ignore_case(path, (const WCHAR *)L"CONOUT$")) return GetStdHandle(STD_OUTPUT_HANDLE);
  if (equals_ignore_case(path, (const WCHAR *)L"CONIN$")) return GetStdHandle(STD_INPUT_HANDLE);
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  const BOOL is_pipe = (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/') && path[2] == '.' && (path[3] == '\\' || path[3] == '/') && (path[4] | 0x20) == 'p' &&
                       (path[5] | 0x20) == 'i' && (path[6] | 0x20) == 'p' && (path[7] | 0x20) == 'e' && (path[8] == '\\' || path[8] == '/' || path[8] == 0);      /* \\.\pipe\ */
  if (is_pipe) {
    WCHAR normal[300];
    size_t_ i = 0;
    for (; path[i] && i < 299; ++i) normal[i] = path[i] == '/' ? '\\' : path[i];
    normal[i] = 0;
    if (!pipe_nt_name(normal, buffer, &name, &attributes, FALSE)) return INVALID_HANDLE_VALUE;
  } else if (!nt_name(path, buffer, &name, &attributes)) {
    return INVALID_HANDLE_VALUE;
  }
  if (sa && sa->bInheritHandle) attributes.Attributes |= 2;      /* OBJ_INHERIT */
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
  if (access & (GENERIC_READ | GENERIC_ALL)) nt_access |= FILE_READ_DATA;
  if (access & (GENERIC_WRITE | GENERIC_ALL)) nt_access |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES;
  if (disposition == TRUNCATE_EXISTING || disposition == CREATE_ALWAYS) nt_access |= FILE_WRITE_DATA;
  ULONG options = FILE_SYNCHRONOUS_IO_NONALERT_;
  if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) options |= FILE_NON_DIRECTORY_FILE_;
  if (flags & FILE_FLAG_DELETE_ON_CLOSE) { options |= FILE_DELETE_ON_CLOSE_; nt_access |= DELETE; }
  HANDLE handle = NULL;
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtCreateFile(&handle, nt_access, &attributes, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, share ? share : 7, nt_disposition, options, NULL, 0);
  if (!NT_SUCCESS(status)) {
    set_error(status == STATUS_OBJECT_NAME_COLLISION ? ERROR_FILE_EXISTS : status == STATUS_FILE_IS_A_DIRECTORY ? ERROR_ACCESS_DENIED : RtlNtStatusToDosError(status));
    return INVALID_HANDLE_VALUE;
  }
  /* CREATE_ALWAYS and OPEN_ALWAYS say, by a success with ERROR_ALREADY_EXISTS, that the file was there. */
  set_error(((disposition == CREATE_ALWAYS && iosb.Information == 3) || (disposition == OPEN_ALWAYS && iosb.Information == 1)) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  return handle;
}
EXPORT HANDLE WINAPI CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE template_file) {
  WCHAR *w = to_wide(path);
  if (!w) return INVALID_HANDLE_VALUE;
  HANDLE h = CreateFileW(w, access, share, sa, disposition, flags, template_file);
  free_wide(w);
  return h;
}

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
EXPORT BOOL WINAPI FlushFileBuffers(HANDLE h) {
  IO_STATUS_BLOCK iosb;
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
EXPORT DWORD WINAPI GetFinalPathNameByHandleW(HANDLE h, LPWSTR buffer, DWORD capacity, DWORD flags) {
  (void)flags;
  IO_STATUS_BLOCK iosb;
  BYTE info[4 + PATH_CHARS * 2];
  const NTSTATUS status = NtQueryInformationFile(h, &iosb, info, sizeof(info), 9);      /* FileNameInformation: the path on the volume */
  if (!NT_SUCCESS(status)) { fail_nt(status); return 0; }
  const DWORD units = *(DWORD *)info / 2;
  const DWORD needed = units + 2 + 4;      /* "\\?\C:" + path */
  if (needed + 1 > capacity) return needed + 1;
  DWORD n = 0;
  buffer[n++] = '\\'; buffer[n++] = '\\'; buffer[n++] = '?'; buffer[n++] = '\\'; buffer[n++] = 'C'; buffer[n++] = ':';
  memcpy(buffer + n, info + 4, units * 2);
  n += units;
  buffer[n] = 0;
  return n;
}

EXPORT BOOL WINAPI DeleteFileW(LPCWSTR path) {
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(path, buffer, &name, &attributes)) return FALSE;
  BYTE basic[40];
  NTSTATUS status = NtQueryAttributesFile(&attributes, basic);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  if (*(DWORD *)(basic + 32) & FILE_ATTRIBUTE_DIRECTORY) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  status = NtDeleteFile(&attributes);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI DeleteFileA(LPCSTR path) { WCHAR *w = to_wide(path); const BOOL ok = w ? DeleteFileW(w) : FALSE; free_wide(w); return ok; }

EXPORT BOOL WINAPI CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa) {
  (void)sa;
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
EXPORT BOOL WINAPI CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa) { WCHAR *w = to_wide(path); const BOOL ok = w ? CreateDirectoryW(w, sa) : FALSE; free_wide(w); return ok; }
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

EXPORT BOOL WINAPI MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  WCHAR source_buffer[PATH_CHARS + 8], target_buffer[PATH_CHARS + 8];
  UNICODE_STRING source_name, target_name;
  OBJECT_ATTRIBUTES source, target;
  if (!nt_name(from, source_buffer, &source_name, &source) || !nt_name(to, target_buffer, &target_name, &target)) return FALSE;
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
EXPORT BOOL WINAPI CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) {
  HANDLE in = CreateFileW(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (in == INVALID_HANDLE_VALUE) return FALSE;
  HANDLE out = CreateFileW(to, GENERIC_WRITE, 0, NULL, fail_if_exists ? CREATE_NEW : CREATE_ALWAYS, 0, NULL);
  if (out == INVALID_HANDLE_VALUE) { const DWORD e = GetLastError(); CloseHandle(in); set_error(e); return FALSE; }
  BYTE *chunk = RtlAllocateHeap(heap(), 0, 16384);
  BOOL ok = chunk != NULL;
  if (!chunk) set_error(ERROR_NOT_ENOUGH_MEMORY);
  while (ok) {
    DWORD got = 0, put = 0;
    ok = ReadFile(in, chunk, 16384, &got, NULL);
    if (!ok || got == 0) break;
    ok = WriteFile(out, chunk, got, &put, NULL) && put == got;
  }
  const DWORD e = GetLastError();
  if (chunk) RtlFreeHeap(heap(), 0, chunk);
  CloseHandle(in);
  CloseHandle(out);
  if (!ok) set_error(e);
  return ok;
}
EXPORT BOOL WINAPI CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
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
  for (UINT attempt = 0; attempt < 0x10000; ++attempt) {
    const UINT id = unique ? unique : (GetTickCount() + ++counter) & 0xFFFF;
    DWORD length = n;
    if (full[length - 1] != '\\') full[length++] = '\\';
    for (int i = 0; i < 3 && prefix && prefix[i]; ++i) full[length++] = prefix[i];
    for (int shift = 12; shift >= 0; shift -= 4) full[length++] = (WCHAR)"0123456789ABCDEF"[(id >> shift) & 15];
    full[length++] = '.'; full[length++] = 't'; full[length++] = 'm'; full[length++] = 'p'; full[length] = 0;
    if (unique) { memcpy(result, full, (length + 1) * 2); return unique; }
    HANDLE h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); memcpy(result, full, (length + 1) * 2); return id; }
  }
  return 0;
}
EXPORT UINT WINAPI GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR result) {
  WCHAR *d = to_wide(dir), *p = to_wide(prefix);
  WCHAR wide[PATH_CHARS];
  const UINT id = d && p ? GetTempFileNameW(d, p, unique, wide) : 0;
  free_wide(d);
  free_wide(p);
  if (!id) return 0;
  wide_result_to_ansi(wide, (DWORD)wcslen(wide), result, MAX_PATH);
  return id;
}
EXPORT BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE source, HANDLE tp, HANDLE *target, DWORD access, BOOL inherit, DWORD options) {
  (void)inherit;
  const NTSTATUS status = NtDuplicateObject(sp, source, tp, target, access, 0, options & 1);      /* DUPLICATE_CLOSE_SOURCE; within this process */
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

/* The next record (FILE_BOTH_DIRECTORY_INFORMATION) of the listing; NULL at the end. */
static const BYTE *find_record(FindState *f) {
  while (f->offset >= f->used) {
    if (f->finished) return NULL;
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

EXPORT HANDLE WINAPI FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
  WCHAR full[PATH_CHARS];
  const DWORD n = full_path_w(path, full, PATH_CHARS);
  if (n == 0) return INVALID_HANDLE_VALUE;
  /* "C:\dir\*.txt": the directory is everything before the last backslash, the pattern what follows. */
  DWORD split = n;
  while (split > 0 && full[split - 1] != '\\') --split;
  if (split == 0) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  FindState *f = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, sizeof(FindState));
  if (!f) { set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  const DWORD pattern_length = n - split;
  if (pattern_length == 0 || pattern_length >= MAX_PATH) { RtlFreeHeap(heap(), 0, f); set_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  memcpy(f->pattern, full + split, pattern_length * 2);
  f->pattern[pattern_length] = 0;
  if (split > 3) --split;      /* the directory without its trailing backslash, except the root */
  full[split] = 0;
  WCHAR buffer[PATH_CHARS + 8];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!nt_name(full, buffer, &name, &attributes)) { RtlFreeHeap(heap(), 0, f); return INVALID_HANDLE_VALUE; }
  IO_STATUS_BLOCK iosb;
  const NTSTATUS status = NtOpenFile(&f->directory, FILE_LIST_DIRECTORY_ | SYNCHRONIZE, &attributes, &iosb, 7, FILE_DIRECTORY_FILE_ | FILE_SYNCHRONOUS_IO_NONALERT_);
  if (!NT_SUCCESS(status)) {
    RtlFreeHeap(heap(), 0, f);
    set_error(status == STATUS_OBJECT_NAME_NOT_FOUND ? ERROR_PATH_NOT_FOUND : RtlNtStatusToDosError(status));
    return INVALID_HANDLE_VALUE;
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
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x200
#define FORMAT_MESSAGE_FROM_SYSTEM 0x1000
EXPORT DWORD WINAPI FormatMessageA(DWORD flags, LPCVOID source, DWORD id, DWORD language, LPSTR buffer, DWORD capacity, PVOID arguments) {
  (void)source; (void)language; (void)arguments;
  if (!(flags & FORMAT_MESSAGE_FROM_SYSTEM)) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  char fallback[40];
  const char *text = error_text(id);
  if (!text) {
    const char *prefix = "Error ";
    size_t_ n = 0;
    while (prefix[n]) { fallback[n] = prefix[n]; ++n; }
    char digits[12];
    int d = 0;
    DWORD v = id;
    do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (d) fallback[n++] = digits[--d];
    fallback[n++] = '.';
    fallback[n] = 0;
    text = fallback;
  }
  const DWORD length = (DWORD)strlen(text);
  if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
    char *allocated = RtlAllocateHeap(heap(), 0, length + 3);
    if (!allocated) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memcpy(allocated, text, length);
    allocated[length] = '\r'; allocated[length + 1] = '\n'; allocated[length + 2] = 0;
    *(char **)buffer = allocated;
    return length + 2;
  }
  if (length + 3 > capacity) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  memcpy(buffer, text, length);
  buffer[length] = '\r'; buffer[length + 1] = '\n'; buffer[length + 2] = 0;
  return length + 2;
}
EXPORT DWORD WINAPI FormatMessageW(DWORD flags, LPCVOID source, DWORD id, DWORD language, LPWSTR buffer, DWORD capacity, PVOID arguments) {
  char narrow[256];
  const DWORD n = FormatMessageA(flags & ~FORMAT_MESSAGE_ALLOCATE_BUFFER, source, id, language, narrow, sizeof(narrow), arguments);
  if (n == 0) return 0;
  const int units = MultiByteToWideChar(CP_UTF8, 0, narrow, (int)n + 1, NULL, 0);
  if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
    WCHAR *allocated = RtlAllocateHeap(heap(), 0, (SIZE_T)units * 2);
    if (!allocated) return 0;
    MultiByteToWideChar(CP_UTF8, 0, narrow, (int)n + 1, allocated, units);
    *(WCHAR **)buffer = allocated;
    return n;
  }
  if ((DWORD)units > capacity) { set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  MultiByteToWideChar(CP_UTF8, 0, narrow, (int)n + 1, buffer, (int)capacity);
  return n;
}

/* ---- start ------------------------------------------------------------------------------------------------------------------------------------------------ */

EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved; (void)reason;
  return TRUE;
}

#include "extra.inc"
#include "extra2.inc"
#include "extra3.inc"
