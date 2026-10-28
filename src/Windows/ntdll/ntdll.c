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

/* ntdll.dll for TheoryCraftOS: the system-call stubs (the numbers are this OS's own), the heap, the Rtl* string and error helpers, critical sections,
 * the C functions a program may take from ntdll, and the process start-up (RtlUserThreadStart). Single-threaded: the process has one thread, so the locks
 * only count. Built freestanding with the OS's mingw compiler; kernel32.dll imports from it.
 *
 * Compile with -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns: the compiler must not turn the loops of memcpy and memset into calls to themselves. */
#define IMPORT
#include "../tcos_win.h"
#include "../tcos_syscalls.h"

/* ---- code the compiler expects from the platform ------------------------------------------------------------------------------------------------------- */

/* A function with a big frame calls this to touch its pages; the stack here is mapped in full, so there is nothing to do. */
__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* The service stubs: "mov r10, rcx; mov eax, number; syscall; ret". Arguments past the fourth stay on the stack where the caller put them. */
#define NT_SERVICE(name)                                                                                                                                      \
  __asm__(".text\n.globl " #name "\n" #name ":\n mov %rcx, %r10\n mov $" _STR(NT_SERVICE_##name) ", %eax\n syscall\n ret\n.section .drectve\n.ascii \" -export:" #name "\"\n.text\n")
/* The services that can end with STATUS_USER_APC are called through wrappers below that run the queued APCs first; these are the bare stubs. */
#define NT_SERVICE_RAW(name)                                                                                                                                  \
  __asm__(".text\n.globl Raw" #name "\nRaw" #name ":\n mov %rcx, %r10\n mov $" _STR(NT_SERVICE_##name) ", %eax\n syscall\n ret\n.text\n")
/* A Zw name of a service: in user mode it is the same call as the Nt one. */
#define ZW_SERVICE(nt, zw)                                                                                                                                    \
  __asm__(".text\n.globl " #zw "\n" #zw ":\n mov %rcx, %r10\n mov $" _STR(NT_SERVICE_##nt) ", %eax\n syscall\n ret\n.section .drectve\n.ascii \" -export:" #zw "\"\n.text\n")
#define _STR(x) _STR2(x)
#define _STR2(x) #x
NT_SERVICE(NtReadFile);
NT_SERVICE(NtWriteFile);
NT_SERVICE(NtCreateThreadEx);
NT_SERVICE(NtTerminateThread);
NT_SERVICE_RAW(NtWaitForSingleObject);
NT_SERVICE(NtResumeThread);
NT_SERVICE(NtQueryInformationThread);
NT_SERVICE(NtCreateEvent);
NT_SERVICE(NtOpenEvent);
NT_SERVICE(NtSetEvent);
NT_SERVICE(NtResetEvent);
NT_SERVICE(NtPulseEvent);
NT_SERVICE(NtCreateMutant);
NT_SERVICE(NtOpenMutant);
NT_SERVICE(NtReleaseMutant);
NT_SERVICE(NtCreateSemaphore);
NT_SERVICE(NtOpenSemaphore);
NT_SERVICE(NtReleaseSemaphore);
NT_SERVICE_RAW(NtWaitForMultipleObjects);
NT_SERVICE(NtDuplicateObject);
NT_SERVICE(NtSuspendThread);
NT_SERVICE(NtGetContextThread);
NT_SERVICE(NtSetContextThread);
NT_SERVICE(NtCreateUserProcess);
NT_SERVICE(NtQueueApcThread);
NT_SERVICE(NtDequeueApc);
NT_SERVICE_RAW(NtSignalAndWaitForSingleObject);
NT_SERVICE(NtCreateTimer);
NT_SERVICE(NtOpenTimer);
NT_SERVICE(NtSetTimer);
NT_SERVICE(NtCancelTimer);
NT_SERVICE(NtQueryTimer);
NT_SERVICE(NtQueryTimerResolution);
NT_SERVICE(NtSetTimerResolution);
NT_SERVICE(NtOpenThread);
NT_SERVICE(NtOpenProcess);
NT_SERVICE(NtCreateSection);
NT_SERVICE(NtOpenSection);
NT_SERVICE(NtMapViewOfSection);
NT_SERVICE(NtUnmapViewOfSection);
NT_SERVICE(NtFlushVirtualMemory);
NT_SERVICE(NtLockFile);
NT_SERVICE(NtUnlockFile);
NT_SERVICE(NtCreateJobObject);
NT_SERVICE(NtOpenJobObject);
NT_SERVICE(NtAssignProcessToJobObject);
NT_SERVICE(NtTerminateJobObject);
NT_SERVICE(NtQueryInformationJobObject);
NT_SERVICE(NtSetInformationJobObject);
NT_SERVICE(NtIsProcessInJob);
NT_SERVICE(NtReadVirtualMemory);
NT_SERVICE(NtWriteVirtualMemory);
NT_SERVICE(NtCreateSymbolicLink);
NT_SERVICE(NtCreateSocket);
NT_SERVICE(NtBindSocket);
NT_SERVICE(NtConnectSocket);
NT_SERVICE(NtListenSocket);
NT_SERVICE(NtAcceptSocket);
NT_SERVICE(NtSendSocket);
NT_SERVICE(NtReceiveSocket);
NT_SERVICE(NtSocketControl);
NT_SERVICE(NtSelectSockets);
NT_SERVICE(NtCreateChangeNotification);
NT_SERVICE(NtCreateMailslotFile);
NT_SERVICE(NtNotifyChangeDirectoryFile);
NT_SERVICE(NtQuerySystemInformation);
NT_SERVICE(NtSetInformationThread);
NT_SERVICE(NtSetInformationProcess);
NT_SERVICE(NtGetCurrentProcessorNumber);
NT_SERVICE(NtLoadModule);
NT_SERVICE(NtUnloadModule);
NT_SERVICE(NtReleaseModule);
NT_SERVICE(NtQueryObject);
NT_SERVICE(NtSetInformationObject);
NT_SERVICE(NtCreatePipe);
NT_SERVICE(NtCreateNamedPipeFile);
NT_SERVICE(NtFsControlFile);
NT_SERVICE(NtClose);
NT_SERVICE(NtTerminateProcess);
NT_SERVICE(NtYieldExecution);
NT_SERVICE_RAW(NtDelayExecution);
NT_SERVICE(NtQuerySystemTime);
#include "services.inc"      /* the rest of the services: generated, the kernel answers STATUS_NOT_IMPLEMENTED for the ones it has no code for */
#include "zw.inc"
NT_SERVICE(NtAllocateVirtualMemory);
NT_SERVICE(NtFreeVirtualMemory);
NT_SERVICE(NtProtectVirtualMemory);
NT_SERVICE(NtCreateFile);
NT_SERVICE(NtOpenFile);
NT_SERVICE(NtQueryInformationFile);
NT_SERVICE(NtSetInformationFile);
NT_SERVICE(NtQueryDirectoryFile);
NT_SERVICE(NtQueryAttributesFile);
NT_SERVICE(NtDeleteFile);
NT_SERVICE(NtFlushBuffersFile);
NT_SERVICE(NtQueryVolumeInformationFile);
NT_SERVICE(NtQueryPerformanceCounter);
NT_SERVICE(NtQueryVirtualMemory);
NT_SERVICE(NtQueryInformationProcess);

/* ---- the C library functions ntdll exports (shared with msvcrt.dll) -------------------------------------------------------------------------------------- */

#include "../libc_string.inc"

/* The Rtl* names kernel32.dll forwards straight to ntdll for (RtlFillMemory's argument order is (dest, length, fill), not memset's (dest, fill, length) - a real,
 * easy-to-miss gotcha copying one into the other). RtlCompareMemory returns how many leading bytes match, not a bool. */
EXPORT void *NTAPI RtlCopyMemory(void *d, const void *s, SIZE_T n) { return memcpy(d, s, n); }
EXPORT void *NTAPI RtlMoveMemory(void *d, const void *s, SIZE_T n) { return memmove(d, s, n); }
EXPORT void *NTAPI RtlFillMemory(void *d, SIZE_T n, BYTE fill) { return memset(d, fill, n); }
EXPORT void *NTAPI RtlZeroMemory(void *d, SIZE_T n) { return memset(d, 0, n); }
EXPORT SIZE_T NTAPI RtlCompareMemory(const void *a, const void *b, SIZE_T n) {
  const BYTE *p = a, *q = b;
  SIZE_T i = 0;
  while (i < n && p[i] == q[i]) ++i;
  return i;
}

/* ---- formatting (the narrow printf family) ------------------------------------------------------------------------------------------------------------- */

typedef struct { char *out; size_t_ capacity, length; } Sink;
static void sink_put(Sink *s, char c) {
  if (s->length + 1 < s->capacity) s->out[s->length] = c;
  ++s->length;
}
static void sink_pad(Sink *s, char c, int n) { while (n-- > 0) sink_put(s, c); }

EXPORT int vsnprintf(char *buffer, size_t_ capacity, const char *format, va_list_ args) {
  Sink sink = {buffer, capacity, 0};
  for (const char *f = format; *f; ++f) {
    if (*f != '%') { sink_put(&sink, *f); continue; }
    ++f;
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, precision = -1, size = 0;
    for (;; ++f) {
      if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else break;
    }
    if (*f == '*') { width = __builtin_va_arg(args, int); if (width < 0) { left = 1; width = -width; } ++f; }
    else while (isdigit((BYTE)*f)) width = width * 10 + (*f++ - '0');
    if (*f == '.') {
      ++f; precision = 0;
      if (*f == '*') { precision = __builtin_va_arg(args, int); ++f; }
      else while (isdigit((BYTE)*f)) precision = precision * 10 + (*f++ - '0');
    }
    for (;; ++f) {      /* size: 1 = h, 2 = hh, 4 = l (32 bit on Windows), 8 = ll, z, j, t, I64 */
      if (*f == 'h') size = size == 1 ? 2 : 1;
      else if (*f == 'l') size = size == 4 ? 8 : 4;
      else if (*f == 'z' || *f == 'j' || *f == 't') size = 8;
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') { size = 8; f += 2; }
      else if (*f == 'I' && f[1] == '3' && f[2] == '2') { size = 0; f += 2; }
      else break;
    }
    const char conversion = *f;
    if (!conversion) break;
    char text[80];
    const char *body = text;
    int length = 0;
    char sign = 0;
    const char *digits = "0123456789abcdef";
    switch (conversion) {
      case '%': sink_put(&sink, '%'); continue;
      case 'c': text[0] = (char)__builtin_va_arg(args, int); length = 1; break;
      case 's': {
        body = __builtin_va_arg(args, const char *);
        if (!body) body = "(null)";
        while (body[length] && (precision < 0 || length < precision)) ++length;
        break;
      }
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
        unsigned long long value;
        int is_signed = conversion == 'd' || conversion == 'i';
        unsigned base = conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 : 10;
        if (conversion == 'X') digits = "0123456789ABCDEF";
        if (conversion == 'p') { value = (unsigned long long)__builtin_va_arg(args, void *); is_signed = 0; if (precision < 0) precision = 16; digits = "0123456789ABCDEF"; }
        else if (is_signed) {
          long long v = size == 8 ? __builtin_va_arg(args, long long) : __builtin_va_arg(args, int);
          if (size == 1) v = (short)v; else if (size == 2) v = (signed char)v;
          if (v < 0) { sign = '-'; value = (unsigned long long)-v; } else { value = (unsigned long long)v; if (plus) sign = '+'; else if (space) sign = ' '; }
        } else {
          value = size == 8 ? __builtin_va_arg(args, unsigned long long) : __builtin_va_arg(args, unsigned int);
          if (size == 1) value &= 0xFFFF; else if (size == 2) value &= 0xFF;
        }
        char reversed[72];
        int n = 0;
        do { reversed[n++] = digits[value % base]; value /= base; } while (value);
        while (precision >= 0 && n < precision && n < 70) reversed[n++] = '0';
        if (alt && base == 16 && conversion != 'p') { reversed[n++] = conversion; reversed[n++] = '0'; }
        for (int i = 0; i < n; ++i) text[i] = reversed[n - 1 - i];
        length = n;
        if (precision >= 0 && conversion != 'p') zero = 0;
        break;
      }
      default: sink_put(&sink, '%'); sink_put(&sink, conversion); continue;
    }
    int pad = width - length - (sign ? 1 : 0);
    if (!left && !zero) sink_pad(&sink, ' ', pad);
    if (sign) sink_put(&sink, sign);
    if (!left && zero && conversion != 's' && conversion != 'c') sink_pad(&sink, '0', pad);
    for (int i = 0; i < length; ++i) sink_put(&sink, body[i]);
    if (left) sink_pad(&sink, ' ', pad);
  }
  if (capacity) sink.out[sink.length < capacity ? sink.length : capacity - 1] = 0;
  return (int)sink.length;
}
EXPORT int _vsnprintf(char *b, size_t_ n, const char *f, va_list_ a) { return vsnprintf(b, n, f, a); }
EXPORT int vsprintf(char *b, const char *f, va_list_ a) { return vsnprintf(b, (size_t_)-1 >> 1, f, a); }
EXPORT int snprintf(char *b, size_t_ n, const char *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vsnprintf(b, n, f, a); __builtin_va_end(a); return r; }
EXPORT int _snprintf(char *b, size_t_ n, const char *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vsnprintf(b, n, f, a); __builtin_va_end(a); return r; }
EXPORT int sprintf(char *b, const char *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vsnprintf(b, (size_t_)-1 >> 1, f, a); __builtin_va_end(a); return r; }

/* ---- last error, status codes, the process --------------------------------------------------------------------------------------------------------------- */

EXPORT PEB *NTAPI RtlGetCurrentPeb(void) { return NtCurrentPeb(); }
EXPORT ULONG NTAPI RtlGetLastWin32Error(void) { return TEB_LAST_ERROR(NtCurrentTeb()); }
EXPORT void NTAPI RtlSetLastWin32Error(ULONG error) { TEB_LAST_ERROR(NtCurrentTeb()) = error; }

#include "status.inc"
EXPORT ULONG NTAPI RtlNtStatusToDosError(NTSTATUS status) {
  {      /* the documented table (binary search), then the codes this OS gives its own meaning */
    ULONG lo = 0, hi = sizeof(status_table) / sizeof(status_table[0]);
    while (lo < hi) {
      const ULONG mid = (lo + hi) / 2;
      if (status_table[mid].status == (ULONG)status) return status_table[mid].error;
      if (status_table[mid].status < (ULONG)status) lo = mid + 1; else hi = mid;
    }
  }
  if (((ULONG)status >> 16) == 0x8007u || ((ULONG)status >> 16) == 0xC007u) return (ULONG)status & 0xFFFFu;      /* an HRESULT_FROM_WIN32 / NT_FACILITY_WIN32 code */
  if ((ULONG)status & 0x20000000u) return (ULONG)status;      /* a customer code */
  switch ((ULONG)status) {
    case 0: return ERROR_SUCCESS;
    case 0x80000005u: return ERROR_MORE_DATA;
    case 0x80000006u: return ERROR_NO_MORE_FILES;
    case 0xC0000002u: return ERROR_INVALID_FUNCTION;
    case 0xC0000003u: case 0xC0000004u: return ERROR_INVALID_PARAMETER;
    case 0xC0000005u: return ERROR_NOACCESS;
    case 0xC0000008u: return ERROR_INVALID_HANDLE;
    case 0xC000000Du: return ERROR_INVALID_PARAMETER;
    case 0xC0000010u: return ERROR_INVALID_FUNCTION;
    case 0xC0000011u: return ERROR_HANDLE_EOF;
    case 0xC0000017u: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC0000018u: return ERROR_INVALID_ADDRESS;
    case 0xC0000022u: return ERROR_ACCESS_DENIED;
    case 0xC0000033u: return ERROR_INVALID_NAME;
    case 0xC0000034u: return ERROR_FILE_NOT_FOUND;
    case 0xC0000035u: return ERROR_ALREADY_EXISTS;
    case 0xC0000024u: return ERROR_INVALID_HANDLE;      /* the wrong type of object */
    case 0xC0000046u: return 288;                       /* ERROR_NOT_OWNER */
    case 0xC0000047u: return 298;                       /* ERROR_TOO_MANY_POSTS */
    case 0xC000009Au: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC0000106u: return 206;                       /* ERROR_FILENAME_EXCED_RANGE */
    case 0xC000003Au: return ERROR_PATH_NOT_FOUND;
    case 0xC0000043u: return ERROR_SHARING_VIOLATION;
    case 0xC000007Fu: return ERROR_DISK_FULL;
    case 0xC00000A2u: return ERROR_ACCESS_DENIED;      /* write-protected */
    case 0xC00000BAu: return ERROR_ACCESS_DENIED;      /* a directory opened as a file */
    case 0xC0000101u: return ERROR_DIR_NOT_EMPTY;
    case 0xC0000103u: return ERROR_DIRECTORY;
    case 0xC000011Fu: return ERROR_TOO_MANY_OPEN_FILES;
    case 0xC0000185u: return ERROR_IO_DEVICE;
    case 0xC0000001u: return 31;                        /* ERROR_GEN_FAILURE */
    case 0xC0000055u: return 33;                        /* ERROR_LOCK_VIOLATION */
    case 0xC000007Eu: return 158;                       /* ERROR_NOT_LOCKED */
    case 0xC0000019u: return 487;                       /* ERROR_INVALID_ADDRESS (not the start of a mapped view) */
    case 0xC000004Eu: return ERROR_ACCESS_DENIED;       /* a view with more rights than the mapping has */
    case 0xC000000Bu: return ERROR_INVALID_PARAMETER;   /* an id that is no process or thread */
    case 0xC0000023u: return 122;                       /* ERROR_INSUFFICIENT_BUFFER */
    case 0xC000004Bu: return ERROR_ACCESS_DENIED;       /* the thread is terminating */
    case 0xC000004Cu: return 156;                       /* ERROR_SIGNAL_REFUSED (suspend count exceeded) */
    case 0xC0000056u: return 303;                       /* ERROR_DELETE_PENDING */
    case 0xC000007Bu: return 193;                       /* ERROR_BAD_EXE_FORMAT */
    case 0xC00000AEu: return 231;                       /* ERROR_PIPE_BUSY */
    case 0xC00000AFu: return 535;                       /* ERROR_PIPE_CONNECTED */
    case 0xC00000B0u: return 233;                       /* ERROR_PIPE_NOT_CONNECTED */
    case 0xC00000B1u: return 232;                       /* ERROR_NO_DATA (the pipe is being closed) */
    case 0xC00000B5u: return 121;                       /* ERROR_SEM_TIMEOUT */
    case 0x8000000Du: return 299;                       /* ERROR_PARTIAL_COPY */
    case 0xC0000044u: return 1816;                      /* ERROR_NOT_ENOUGH_QUOTA */
    case 0xC00000BBu: return 50;                        /* ERROR_NOT_SUPPORTED */
    case 0xC00000D9u: return 232;                       /* ERROR_NO_DATA (a non-blocking read finds nothing) */
    case 0xC0000120u: return 1223;                      /* ERROR_CANCELLED */
    case 0xC0000135u: return ERROR_MOD_NOT_FOUND;       /* a DLL was not found */
    case 0xC0000139u: return ERROR_PROC_NOT_FOUND;
    case 0xC000014Bu: return 109;                       /* ERROR_BROKEN_PIPE */
    case 0xC0000142u: return 1114;                      /* ERROR_DLL_INIT_FAILED */
    case 0xC0000235u: return ERROR_INVALID_HANDLE;      /* a protected handle */
    default: return (ULONG)status >= 0xC0000000u ? ERROR_MR_MID_NOT_FOUND : ERROR_INVALID_PARAMETER;
  }
}

EXPORT NTSTATUS NTAPI RtlGetVersion(RTL_OSVERSIONINFOW *info) {
  if (!info) return STATUS_INVALID_PARAMETER;
  info->dwMajorVersion = 10;
  info->dwMinorVersion = 0;
  info->dwBuildNumber = 19045;
  info->dwPlatformId = 2;
  info->szCSDVersion[0] = 0;
  return STATUS_SUCCESS;
}

/* ---- what the loader tells the modules: TLS callbacks and DllMain --------------------------------------------------------------------------------------------- */

/* A module hears about the process starting and ending and about threads starting and ending: first its TLS callbacks (the program's too), then, for a library, its
 * entry point (DllMain), unless it asked not to be told about threads (DisableThreadLibraryCalls). One notification at a time (the loader lock, which a thread that is told
 * something may take again). The libraries are told in load order when something starts and in the opposite order when it ends; the program's callbacks come first and last. */
typedef void (NTAPI *TlsCallback)(PVOID, DWORD, PVOID);
#define LDRP_DONT_CALL_FOR_THREADS 0x00040000u
static volatile ULONG_PTR g_loader_owner;
static volatile LONG g_loader_depth;
static void loader_lock(void) {
  const ULONG_PTR me = TEB_CLIENT_THREAD(NtCurrentTeb());
  if (g_loader_owner == me) { ++g_loader_depth; return; }
  while (!__sync_bool_compare_and_swap(&g_loader_owner, 0, me)) NtYieldExecution();
  g_loader_depth = 1;
}
static void loader_unlock(void) { if (--g_loader_depth == 0) __sync_lock_release(&g_loader_owner); }

/* LdrLockLoaderLock(flags, &disposition, &cookie): flag 1 raises on error, flag 2 only tries. Disposition 1: taken, 2: someone else holds it (try only). The cookie is
 * what LdrUnlockLoaderLock wants back (it is the owner: a different thread may not release the lock). */
EXPORT NTSTATUS NTAPI LdrLockLoaderLock(ULONG flags, ULONG *disposition, ULONG_PTR *cookie) {
  if (disposition) *disposition = 0;
  if (cookie) *cookie = 0;
  if (flags & ~3u) return (NTSTATUS)0xC00000EF;
  if (!cookie) return (NTSTATUS)0xC00000F1;
  if ((flags & 2) && !disposition) return (NTSTATUS)0xC00000F0;
  const ULONG_PTR me = TEB_CLIENT_THREAD(NtCurrentTeb());
  if ((flags & 2) && g_loader_owner != 0 && g_loader_owner != me) { *disposition = 2; return STATUS_SUCCESS; }
  loader_lock();
  if (disposition) *disposition = 1;
  *cookie = me | 1;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI LdrUnlockLoaderLock(ULONG flags, ULONG_PTR cookie) {
  if (flags & ~1u) return (NTSTATUS)0xC00000EF;
  if (cookie == 0) return STATUS_SUCCESS;
  if (g_loader_owner != (cookie & ~(ULONG_PTR)1) || g_loader_depth == 0) return (NTSTATUS)0xC00000F0;
  loader_unlock();
  return STATUS_SUCCESS;
}

static int is_program(const LDR_DATA_TABLE_ENTRY *m) { return NtCurrentPeb()->ImageBaseAddress == m->DllBase; }

#define LDRP_MAPPED_ONLY 0x80000000u      /* loaded with DONT_RESOLVE_DLL_REFERENCES: no imports, no thread-local data, no entry point */
#define LDRP_PROCESS_DETACH_CALLED 0x00080000u      /* the library has been told the process (or its use of it) ends: once only */
/* The entry point of a library that is being attached runs under a guard: a fault nobody handles ends the call (the library failed to start) instead of the process. */
static void **volatile g_init_guard;
static volatile PVOID g_init_guard_teb;
static BOOL call_library_entry(LDR_DATA_TABLE_ENTRY *m, DWORD reason, PVOID reserved) {
  BOOL (WINAPI *entry)(PVOID, DWORD, PVOID) = (BOOL (WINAPI *)(PVOID, DWORD, PVOID))m->EntryPoint;
  if (reason != DLL_PROCESS_ATTACH) return entry(m->DllBase, reason, reserved);
  void *buffer[5];
  void **const outer = g_init_guard;
  PVOID const outer_teb = g_init_guard_teb;
  if (__builtin_setjmp(buffer)) {      /* the entry point faulted */
    g_init_guard = outer;
    g_init_guard_teb = outer_teb;
    return FALSE;
  }
  g_init_guard = buffer;
  g_init_guard_teb = NtCurrentTeb();
  const BOOL result = entry(m->DllBase, reason, reserved);
  g_init_guard = outer;
  g_init_guard_teb = outer_teb;
  return result;
}

static BOOL notify_module(LDR_DATA_TABLE_ENTRY *m, DWORD reason, PVOID reserved) {
  if (m->Flags & LDRP_MAPPED_ONLY) return TRUE;
  if (reason == DLL_PROCESS_DETACH) {
    if (m->Flags & LDRP_PROCESS_DETACH_CALLED) return TRUE;
    m->Flags |= LDRP_PROCESS_DETACH_CALLED;
  } else if (reason == DLL_PROCESS_ATTACH) {
    m->Flags &= ~LDRP_PROCESS_DETACH_CALLED;
  }
  const int about_threads = reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH;
  if (about_threads && !is_program(m) && (m->Flags & LDRP_DONT_CALL_FOR_THREADS)) return TRUE;
  const BYTE *image = (const BYTE *)m->DllBase;
  if (!image) return TRUE;
  const ULONG nt = *(const ULONG *)(image + 0x3C);
  const ULONG rva = *(const ULONG *)(image + nt + 24 + 0x70 + 9 * 8);      /* IMAGE_DIRECTORY_ENTRY_TLS */
  const ULONG size = *(const ULONG *)(image + nt + 24 + 0x70 + 9 * 8 + 4);
  if (rva && size >= 0x28) {
    const TlsCallback *callbacks = (const TlsCallback *)*(const ULONG64 *)(image + rva + 0x18);
    if (callbacks) for (; *callbacks; ++callbacks) (*callbacks)(m->DllBase, reason, reserved);
  }
  if (!is_program(m) && m->EntryPoint) return call_library_entry(m, reason, reserved);
  return TRUE;
}

static LDR_DATA_TABLE_ENTRY *entry_of(LIST_ENTRY *link) { return (LDR_DATA_TABLE_ENTRY *)link; }      /* (InLoadOrderLinks is the first member) */

/* `forward`: libraries in load order, the program first (a start); otherwise the libraries in the opposite order, the program last (an end). */
static void notify_all_modules(DWORD reason, PVOID reserved, int forward) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr) return;
  loader_lock();
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  if (forward) {
    for (LIST_ENTRY *e = head->Flink; e && e != head; e = e->Flink) notify_module(entry_of(e), reason, reserved);
  } else {
    for (LIST_ENTRY *e = head->Blink; e && e != head; e = e->Blink) if (!is_program(entry_of(e))) notify_module(entry_of(e), reason, reserved);
    for (LIST_ENTRY *e = head->Flink; e && e != head; e = e->Flink) if (is_program(entry_of(e))) notify_module(entry_of(e), reason, reserved);
  }
  loader_unlock();
}

static volatile LONG g_process_ending;
static void fls_destroy_current(int free_structure);
static volatile LONG g_heap_lock;
static volatile LONG g_fls_lock;
static void fls_thread_begins(void);
/* The libraries are told the process is ending (once): this thread's fiber local storage goes first, then every library's entry point and TLS callbacks get
 * DLL_PROCESS_DETACH with a non-NULL reserved. The libraries stay loaded; the process is not over. */
EXPORT void NTAPI LdrShutdownProcess(void) {
  if (__sync_bool_compare_and_swap(&g_process_ending, 0, 1)) {
    fls_destroy_current(0);
    notify_all_modules(DLL_PROCESS_DETACH, (PVOID)1, 0);
  }
}

/* ExitProcess: the other threads end first (with the exit code), then the libraries are told, then the process goes. */
/* The PEB lock (RtlAcquirePebLock): one recursive lock for the process-wide data. */
static RTL_CRITICAL_SECTION g_peb_lock = { .LockCount = -1 };
NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs);
NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs);
BOOLEAN NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs);
EXPORT void NTAPI RtlAcquirePebLock(void) { RtlEnterCriticalSection(&g_peb_lock); }
EXPORT void NTAPI RtlReleasePebLock(void) { RtlLeaveCriticalSection(&g_peb_lock); }
EXPORT BOOLEAN NTAPI RtlTryAcquirePebLock(void) { return RtlTryEnterCriticalSection(&g_peb_lock); }

/* ExitProcess waits for whoever holds the PEB lock or the heap lock (a thread blocked inside either keeps the process alive), and keeps the PEB lock itself: the
 * other threads are ended after that, so none of them can hold a lock the shutdown needs. */
EXPORT void NTAPI RtlExitUserProcess(NTSTATUS status) {
  if (!g_process_ending) {
    RtlAcquirePebLock();
    while (g_heap_lock) NtYieldExecution();
    NtTerminateProcess(NULL, status);
    g_heap_lock = 0;      /* (a thread that was ended inside the heap or the fiber storage cannot give its lock back; the process is going away) */
    g_fls_lock = 0;
  }
  LdrShutdownProcess();
  for (;;) NtTerminateProcess(NtCurrentProcess(), status);
}

/* Is the process shutting down (ExitProcess has begun to tell the libraries)? */
EXPORT BOOLEAN NTAPI RtlDllShutdownInProgress(void) { return g_process_ending != 0; }

/* The thread ends (the libraries and the program are told); the process ends with its last thread. */
EXPORT void NTAPI RtlExitUserThread(NTSTATUS status) {
  fls_destroy_current(0);
  if (!g_process_ending) notify_all_modules(DLL_THREAD_DETACH, NULL, 0);
  fls_destroy_current(1);
  for (;;) NtTerminateThread((HANDLE)(LONG_PTR)-2, status);
}

/* New threads begin here (the kernel passes the thread's function and parameter in the first two argument registers); the function's return value is the thread's exit code. */
EXPORT void NTAPI TcosThreadStart(LPTHREAD_START_ROUTINE start, PVOID parameter) {
  fls_thread_begins();
  notify_all_modules(DLL_THREAD_ATTACH, NULL, 1);
  RtlExitUserThread((NTSTATUS)start(parameter));
}

/* ---- APCs ------------------------------------------------------------------------------------------------------------------------------------------------ */

extern NTSTATUS NTAPI RawNtWaitForSingleObject(HANDLE, BOOLEAN, PLARGE_INTEGER);
extern NTSTATUS NTAPI RawNtWaitForMultipleObjects(ULONG, const HANDLE *, ULONG, BOOLEAN, PLARGE_INTEGER);
extern NTSTATUS NTAPI RawNtDelayExecution(BOOLEAN, PLARGE_INTEGER);
extern NTSTATUS NTAPI RawNtSignalAndWaitForSingleObject(HANDLE, HANDLE, BOOLEAN, PLARGE_INTEGER);

/* Calls the APCs queued for this thread, oldest first; how many there were. An APC may queue more or wait alertably itself. */
static ULONG run_apcs(void) {
  ULONG count = 0;
  ULONG64 apc[4];
  while (NtDequeueApc(apc) == STATUS_SUCCESS) {
    ((void (NTAPI *)(ULONG_PTR, ULONG_PTR, ULONG_PTR))apc[0])((ULONG_PTR)apc[1], (ULONG_PTR)apc[2], (ULONG_PTR)apc[3]);
    ++count;
  }
  return count;
}
EXPORT NTSTATUS NTAPI NtTestAlert(void) { return run_apcs() ? STATUS_ALERTED : STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI NtWaitForSingleObject(HANDLE h, BOOLEAN alertable, PLARGE_INTEGER timeout) {
  const NTSTATUS status = RawNtWaitForSingleObject(h, alertable, timeout);
  if (status == STATUS_USER_APC) run_apcs();
  return status;
}
EXPORT NTSTATUS NTAPI NtWaitForMultipleObjects(ULONG count, const HANDLE *handles, ULONG type, BOOLEAN alertable, PLARGE_INTEGER timeout) {
  const NTSTATUS status = RawNtWaitForMultipleObjects(count, handles, type, alertable, timeout);
  if (status == STATUS_USER_APC) run_apcs();
  return status;
}
EXPORT NTSTATUS NTAPI NtDelayExecution(BOOLEAN alertable, PLARGE_INTEGER interval) {
  const NTSTATUS status = RawNtDelayExecution(alertable, interval);
  if (status == STATUS_USER_APC) run_apcs();
  return status;
}
EXPORT NTSTATUS NTAPI NtSignalAndWaitForSingleObject(HANDLE signal, HANDLE wait, BOOLEAN alertable, PLARGE_INTEGER timeout) {
  const NTSTATUS status = RawNtSignalAndWaitForSingleObject(signal, wait, alertable, timeout);
  if (status == STATUS_USER_APC) run_apcs();
  return status;
}

/* ---- strings ------------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT void NTAPI RtlInitUnicodeString(PUNICODE_STRING s, PCWSTR text) {
  s->Buffer = (PWSTR)text;
  s->Length = s->MaximumLength = 0;
  if (text) {
    const size_t_ n = wcslen(text) * 2;
    s->Length = (USHORT)(n > 0xFFFC ? 0xFFFC : n);
    s->MaximumLength = (USHORT)(s->Length + 2);
  }
}
EXPORT void NTAPI RtlInitAnsiString(PANSI_STRING s, PCSTR text) {
  s->Buffer = (PSTR)text;
  s->Length = s->MaximumLength = 0;
  if (text) {
    const size_t_ n = strlen(text);
    s->Length = (USHORT)(n > 0xFFFE ? 0xFFFE : n);
    s->MaximumLength = (USHORT)(s->Length + 1);
  }
}

/* The ANSI code page of this OS is UTF-8. Decodes one code point; a bad sequence is U+FFFD and consumes one byte. */
static ULONG utf8_next(const BYTE *s, ULONG n, ULONG *used) {
  const BYTE b = s[0];
  ULONG cp, extra;
  if (b < 0x80) { *used = 1; return b; }
  if ((b & 0xE0) == 0xC0) cp = b & 0x1F, extra = 1;
  else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, extra = 2;
  else if ((b & 0xF8) == 0xF0) cp = b & 0x07, extra = 3;
  else { *used = 1; return 0xFFFD; }
  if (extra >= n) { *used = 1; return 0xFFFD; }
  for (ULONG k = 1; k <= extra; ++k) {
    if ((s[k] & 0xC0) != 0x80) { *used = 1; return 0xFFFD; }
    cp = (cp << 6) | (s[k] & 0x3F);
  }
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000) || (extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) { *used = 1; return 0xFFFD; }
  *used = extra + 1;
  return cp;
}

EXPORT NTSTATUS NTAPI RtlUTF8ToUnicodeN(PWSTR dest, ULONG dest_bytes, PULONG written, const char *src, ULONG src_bytes) {
  ULONG units = 0;      /* what the whole source needs */
  ULONG out = 0;
  for (ULONG i = 0; i < src_bytes;) {
    ULONG used;
    const ULONG cp = utf8_next((const BYTE *)src + i, src_bytes - i, &used);
    i += used;
    const ULONG count = cp >= 0x10000 ? 2 : 1;
    if (dest && (units + count) * 2 <= dest_bytes) {
      if (cp >= 0x10000) { dest[units] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)); dest[units + 1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
      else dest[units] = (WCHAR)cp;
      out = (units + count) * 2;
    }
    units += count;
  }
  if (written) *written = dest ? out : units * 2;
  return dest && units * 2 > dest_bytes ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlMultiByteToUnicodeN(PWSTR d, ULONG db, PULONG w, const char *s, ULONG sb) { return RtlUTF8ToUnicodeN(d, db, w, s, sb); }
EXPORT NTSTATUS NTAPI RtlMultiByteToUnicodeSize(PULONG size, const char *s, ULONG sb) { return RtlUTF8ToUnicodeN(NULL, 0, size, s, sb); }

EXPORT NTSTATUS NTAPI RtlUnicodeToUTF8N(char *dest, ULONG dest_bytes, PULONG written, PCWSTR src, ULONG src_bytes) {
  ULONG needed = 0, out = 0;
  const ULONG count = src_bytes / 2;
  for (ULONG i = 0; i < count; ++i) {
    ULONG cp = src[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count && src[i + 1] >= 0xDC00 && src[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (src[i + 1] - 0xDC00); ++i; }
    else if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
    BYTE encoded[4];
    ULONG n;
    if (cp < 0x80) { encoded[0] = (BYTE)cp; n = 1; }
    else if (cp < 0x800) { encoded[0] = (BYTE)(0xC0 | (cp >> 6)); encoded[1] = (BYTE)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { encoded[0] = (BYTE)(0xE0 | (cp >> 12)); encoded[1] = (BYTE)(0x80 | ((cp >> 6) & 0x3F)); encoded[2] = (BYTE)(0x80 | (cp & 0x3F)); n = 3; }
    else { encoded[0] = (BYTE)(0xF0 | (cp >> 18)); encoded[1] = (BYTE)(0x80 | ((cp >> 12) & 0x3F)); encoded[2] = (BYTE)(0x80 | ((cp >> 6) & 0x3F)); encoded[3] = (BYTE)(0x80 | (cp & 0x3F)); n = 4; }
    if (dest && needed + n <= dest_bytes) { for (ULONG k = 0; k < n; ++k) dest[needed + k] = (char)encoded[k]; out = needed + n; }
    needed += n;
  }
  if (written) *written = dest ? out : needed;
  return dest && needed > dest_bytes ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlUnicodeToMultiByteN(char *d, ULONG db, PULONG w, PCWSTR s, ULONG sb) { return RtlUnicodeToUTF8N(d, db, w, s, sb); }
EXPORT NTSTATUS NTAPI RtlUnicodeToMultiByteSize(PULONG size, PCWSTR s, ULONG sb) { return RtlUnicodeToUTF8N(NULL, 0, size, s, sb); }

/* ---- the heap ------------------------------------------------------------------------------------------------------------------------------------------ */

/* A chunk is [prev_size (8)][size and flags (8)][payload...]; the user pointer is the payload, 16-byte aligned. prev_size is only meaningful while the chunk before is
 * free (it then lies in that chunk's last 8 bytes). A free chunk keeps two list links at the start of its payload. A segment ends with a zero-size "in use" chunk. */
#define CHUNK_USED 1ull
#define CHUNK_PREV_USED 2ull
#define CHUNK_SIZE(c) ((c)->size_flags & ~15ull)
typedef struct Chunk { ULONGLONG prev_size, size_flags; struct Chunk *next, *prev; } Chunk;
typedef struct Segment { struct Segment *next; PVOID base; SIZE_T size; } Segment;
typedef struct Heap { ULONGLONG magic; ULONG flags; Segment *segments; Chunk free_list; } Heap;
#define HEAP_MAGIC 0x50414548454B4F54ull
#define MIN_CHUNK 32ull
#define SEGMENT_MIN (1ull << 20)

static Chunk *chunk_next(Chunk *c) { return (Chunk *)((BYTE *)c + CHUNK_SIZE(c)); }
static void free_unlink(Chunk *c) { c->prev->next = c->next; c->next->prev = c->prev; }
static void free_link(Heap *h, Chunk *c) { c->next = h->free_list.next; c->prev = &h->free_list; h->free_list.next->prev = c; h->free_list.next = c; }

/* Makes `c` (size bytes, not in any list) a free chunk and puts the bookkeeping of its neighbour right. */
static void make_free(Heap *h, Chunk *c, ULONGLONG size, int prev_used) {
  c->size_flags = size | (prev_used ? CHUNK_PREV_USED : 0);
  Chunk *after = (Chunk *)((BYTE *)c + size);
  after->prev_size = size;
  after->size_flags &= ~CHUNK_PREV_USED;
  free_link(h, c);
}

static int heap_add_segment(Heap *h, SIZE_T minimum) {
  SIZE_T size = minimum + sizeof(Segment) + 64 + 4096;
  if (size < SEGMENT_MIN) size = SEGMENT_MIN;
  PVOID base = NULL;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) return 0;
  Segment *segment = (Segment *)base;
  segment->base = base;
  segment->size = size;
  segment->next = h->segments;
  h->segments = segment;
  BYTE *start = (BYTE *)(((ULONG_PTR)base + sizeof(Segment) + 15) & ~15ull);      /* (16-aligned, so that start + 16, the payload, is too) */
  BYTE *end = (BYTE *)(((ULONG_PTR)base + size) & ~15ull) - 16;      /* (so that end - start is a multiple of 16: the chunks are) */
  Chunk *sentinel = (Chunk *)end;
  sentinel->size_flags = CHUNK_USED;      /* size 0 */
  make_free(h, (Chunk *)start, (ULONGLONG)(end - start), 1);
  return 1;
}

static void heaps_add(Heap *h);
static Heap *heap_new(ULONG flags, SIZE_T commit) {
  SIZE_T size = commit < SEGMENT_MIN ? SEGMENT_MIN : commit;
  PVOID base = NULL;
  if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) return NULL;
  Heap *h = (Heap *)base;
  h->magic = HEAP_MAGIC;
  h->flags = flags;
  h->free_list.next = h->free_list.prev = &h->free_list;
  Segment *segment = (Segment *)((BYTE *)base + ((sizeof(Heap) + 15) & ~15ull));
  segment->base = base;
  segment->size = size;
  segment->next = NULL;
  h->segments = segment;
  BYTE *start = (BYTE *)(((ULONG_PTR)segment + sizeof(Segment) + 15) & ~15ull);
  BYTE *end = (BYTE *)(((ULONG_PTR)base + size) & ~15ull) - 16;      /* (so that end - start is a multiple of 16: the chunks are) */
  Chunk *sentinel = (Chunk *)end;
  sentinel->size_flags = CHUNK_USED;
  make_free(h, (Chunk *)start, (ULONGLONG)(end - start), 1);
  heaps_add(h);
  return h;
}

static int heap_ok(Heap *h) { return h && h->magic == HEAP_MAGIC; }

/* Every heap of the process, in the order they were made (the process heap first): RtlGetProcessHeaps and the Toolhelp heap lists. */
#define MAX_HEAPS 128
static Heap *g_heaps[MAX_HEAPS];
static volatile LONG g_heaps_lock;
static void heaps_lock(void) { while (__sync_lock_test_and_set(&g_heaps_lock, 1)) NtYieldExecution(); }
static void heaps_unlock(void) { __sync_lock_release(&g_heaps_lock); }
static void heaps_add(Heap *h) { heaps_lock(); for (int i = 0; i < MAX_HEAPS; ++i) if (!g_heaps[i]) { g_heaps[i] = h; break; } heaps_unlock(); }
static void heaps_remove(Heap *h) { heaps_lock(); for (int i = 0; i < MAX_HEAPS; ++i) if (g_heaps[i] == h) g_heaps[i] = NULL; heaps_unlock(); }

EXPORT PVOID NTAPI RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID parameters) {
  (void)base; (void)reserve; (void)lock; (void)parameters;
  return heap_new(flags, commit);
}

/* The heaps are shared by the threads of the process: one spin lock (a waiting thread gives its time slice to the holder) covers all of them. */
/* The heap lock is recursive (RtlLockHeap holds it across calls of the same thread). */
static volatile ULONG_PTR g_heap_owner;
static LONG g_heap_depth;
static void heap_lock(void) {
  const ULONG_PTR me = TEB_CLIENT_THREAD(NtCurrentTeb());
  if (g_heap_lock && g_heap_owner == me) { ++g_heap_depth; return; }
  while (__sync_lock_test_and_set(&g_heap_lock, 1)) NtYieldExecution();
  g_heap_owner = me;
  g_heap_depth = 1;
}
static void heap_unlock(void) {
  if (--g_heap_depth > 0) return;
  g_heap_owner = 0;
  __sync_lock_release(&g_heap_lock);
}

static PVOID heap_alloc(PVOID heap, ULONG flags, SIZE_T bytes) {
  Heap *h = heap;
  if (!heap_ok(h)) return NULL;
  if (bytes > (1ull << 46)) return NULL;
  /* (16 for the header; at the end of the payload 8 for the user value and 8 for the user flags - RtlSetUserValueHeap and friends - and the size asked for, kept in the next chunk's prev_size field, which is payload while this chunk is used) */
  ULONGLONG need = (bytes + 32 + 15) & ~15ull;
  if (need < MIN_CHUNK) need = MIN_CHUNK;
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (Chunk *c = h->free_list.next; c != &h->free_list; c = c->next) {
      const ULONGLONG size = CHUNK_SIZE(c);
      if (size < need) continue;
      free_unlink(c);
      if (size - need >= MIN_CHUNK) {      /* split: the rest stays free */
        c->size_flags = need | CHUNK_USED | (c->size_flags & CHUNK_PREV_USED);
        make_free(h, (Chunk *)((BYTE *)c + need), size - need, 1);
      } else {
        c->size_flags |= CHUNK_USED;
        chunk_next(c)->size_flags |= CHUNK_PREV_USED;
      }
      BYTE *user = (BYTE *)c + 16;
      if (flags & HEAP_ZERO_MEMORY) memset(user, 0, (size_t_)bytes);
      *(ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c)) = bytes;      /* the last 8 bytes of the payload say how many were asked for (HeapSize) */
      *(ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c) - 16) = 0;      /* user value */
      *(ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c) - 8) = flags & 0xF00;      /* user flags (HEAP_SETTABLE_USER_FLAG1..3) and whether there is a user value (HEAP_SETTABLE_USER_VALUE) */
      return user;
    }
    if (attempt == 0 && !heap_add_segment(h, need)) return NULL;
  }
  return NULL;
}

/* Does `memory` lie inside a segment of the heap, where a block can be? (Callers hand over all kinds of pointers; none must be followed before this says yes.) */
static int heap_owns(Heap *h, PVOID memory) {
  const ULONG_PTR p = (ULONG_PTR)memory;
  if (p & 15) return 0;
  for (Segment *s = h->segments; s; s = s->next) {
    const ULONG_PTR base = (ULONG_PTR)s->base;
    if (p >= base + sizeof(Segment) + 16 && p < base + s->size) return 1;
  }
  return 0;
}

static BOOLEAN heap_free(PVOID heap, ULONG flags, PVOID memory) {
  (void)flags;
  Heap *h = heap;
  if (!heap_ok(h)) return FALSE;
  if (!memory) return TRUE;
  if (!heap_owns(h, memory)) return FALSE;
  Chunk *c = (Chunk *)((BYTE *)memory - 16);
  if (!(c->size_flags & CHUNK_USED)) return FALSE;      /* freed twice */
  ULONGLONG size = CHUNK_SIZE(c);
  int prev_used = (c->size_flags & CHUNK_PREV_USED) != 0;
  Chunk *after = chunk_next(c);
  if (!(after->size_flags & CHUNK_USED)) {      /* merge with the free chunk after */
    size += CHUNK_SIZE(after);
    free_unlink(after);
  }
  if (!prev_used) {      /* merge with the free chunk before */
    Chunk *before = (Chunk *)((BYTE *)c - c->prev_size);
    size += CHUNK_SIZE(before);
    prev_used = (before->size_flags & CHUNK_PREV_USED) != 0;
    free_unlink(before);
    c = before;
  }
  make_free(h, c, size, prev_used);
  return TRUE;
}

static SIZE_T heap_size(PVOID heap, ULONG flags, PVOID memory) {
  (void)flags;
  if (!heap_ok(heap) || !memory || !heap_owns(heap, memory)) return (SIZE_T)-1;
  Chunk *c = (Chunk *)((BYTE *)memory - 16);
  return (c->size_flags & CHUNK_USED) ? (SIZE_T) * (ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c)) : (SIZE_T)-1;
}
static SIZE_T heap_capacity(PVOID memory) { Chunk *c = (Chunk *)((BYTE *)memory - 16); return (SIZE_T)(CHUNK_SIZE(c) - 32); }
static ULONGLONG *chunk_user_value(PVOID memory) { Chunk *c = (Chunk *)((BYTE *)memory - 16); return (ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c) - 16); }
static ULONGLONG *chunk_user_flags(PVOID memory) { Chunk *c = (Chunk *)((BYTE *)memory - 16); return (ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c) - 8); }

static PVOID heap_realloc(PVOID heap, ULONG flags, PVOID memory, SIZE_T bytes) {
  if (!memory) return heap_alloc(heap, flags, bytes);
  const SIZE_T have = heap_size(heap, flags, memory);
  if (have == (SIZE_T)-1) return NULL;
  if (bytes <= heap_capacity(memory)) {      /* fits in the chunk: stays where it is */
    Chunk *c = (Chunk *)((BYTE *)memory - 16);
    if ((flags & HEAP_ZERO_MEMORY) && bytes > have) memset((BYTE *)memory + have, 0, bytes - have);
    *(ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c)) = bytes;
    return memory;
  }
  if (flags & 0x10 /* HEAP_REALLOC_IN_PLACE_ONLY */) {
    /* A big block may grow into the free chunk after it (a small one is never grown in place: it would be a low-fragmentation block on Windows). */
    Chunk *c = (Chunk *)((BYTE *)memory - 16);
    Chunk *after = chunk_next(c);
    const ULONGLONG have_size = CHUNK_SIZE(c), need = (bytes + 32 + 15) & ~15ull;
    if (have_size < 0x4000 || (after->size_flags & CHUNK_USED) || have_size + CHUNK_SIZE(after) < need) return NULL;
    const ULONGLONG total = have_size + CHUNK_SIZE(after);
    free_unlink(after);
    if (total - need >= MIN_CHUNK) {
      c->size_flags = need | CHUNK_USED | (c->size_flags & CHUNK_PREV_USED);
      make_free(heap, (Chunk *)((BYTE *)c + need), total - need, 1);
    } else {
      c->size_flags = total | CHUNK_USED | (c->size_flags & CHUNK_PREV_USED);
      chunk_next(c)->size_flags |= CHUNK_PREV_USED;
    }
    const ULONGLONG user_value = *(ULONGLONG *)((BYTE *)c + have_size - 16), user_flags = *(ULONGLONG *)((BYTE *)c + have_size - 8);
    *(ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c) - 16) = user_value;
    *(ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c) - 8) = user_flags;
    if ((flags & HEAP_ZERO_MEMORY) && bytes > have) memset((BYTE *)memory + have, 0, bytes - have);
    *(ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c)) = bytes;
    return memory;
  }
  PVOID fresh = heap_alloc(heap, flags & ~HEAP_ZERO_MEMORY, bytes);
  if (!fresh) return NULL;
  memcpy(fresh, memory, have < bytes ? have : bytes);
  if ((flags & HEAP_ZERO_MEMORY) && bytes > have) memset((BYTE *)fresh + have, 0, bytes - have);
  *chunk_user_value(fresh) = *chunk_user_value(memory);
  *chunk_user_flags(fresh) = *chunk_user_flags(memory);
  heap_free(heap, flags, memory);
  return fresh;
}

/* The value and the flags (HEAP_SETTABLE_USER_FLAG1..3) a program can keep with a block. */
EXPORT BOOLEAN NTAPI RtlGetUserInfoHeap(PVOID heap, ULONG flags, PVOID memory, PVOID *value, PULONG user_flags) {
  (void)flags;
  heap_lock();
  const int ok = heap_ok(heap) && memory && heap_size(heap, 0, memory) != (SIZE_T)-1;
  if (ok) { if (value && (*chunk_user_flags(memory) & 0x100)) *value = (PVOID)(ULONG_PTR)*chunk_user_value(memory); if (user_flags) *user_flags = (ULONG)(*chunk_user_flags(memory) & 0xE00); }
  heap_unlock();
  return ok ? TRUE : FALSE;
}
EXPORT BOOLEAN NTAPI RtlSetUserValueHeap(PVOID heap, ULONG flags, PVOID memory, PVOID value) {
  (void)flags;
  heap_lock();
  const int ok = heap_ok(heap) && memory && heap_size(heap, 0, memory) != (SIZE_T)-1;
  if (ok) { *chunk_user_value(memory) = (ULONGLONG)(ULONG_PTR)value; *chunk_user_flags(memory) |= 0x100; }
  heap_unlock();
  return ok ? TRUE : FALSE;
}
EXPORT BOOLEAN NTAPI RtlSetUserFlagsHeap(PVOID heap, ULONG flags, PVOID memory, ULONG clear, ULONG set) {
  (void)flags;
  heap_lock();
  const int ok = heap_ok(heap) && memory && heap_size(heap, 0, memory) != (SIZE_T)-1;
  if (ok) *chunk_user_flags(memory) = (*chunk_user_flags(memory) & ~(ULONGLONG)(clear & 0xE00)) | (set & 0xE00);
  heap_unlock();
  return ok ? TRUE : FALSE;
}
EXPORT PVOID NTAPI RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T bytes) { heap_lock(); PVOID r = heap_alloc(heap, flags, bytes); heap_unlock(); return r; }
EXPORT BOOLEAN NTAPI RtlFreeHeap(PVOID heap, ULONG flags, PVOID memory) { heap_lock(); BOOLEAN r = heap_free(heap, flags, memory); heap_unlock(); return r; }
EXPORT SIZE_T NTAPI RtlSizeHeap(PVOID heap, ULONG flags, PVOID memory) { heap_lock(); SIZE_T r = heap_size(heap, flags, memory); heap_unlock(); return r; }
EXPORT PVOID NTAPI RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID memory, SIZE_T bytes) { heap_lock(); PVOID r = heap_realloc(heap, flags, memory, bytes); heap_unlock(); return r; }

EXPORT BOOLEAN NTAPI RtlValidateHeap(PVOID heap, ULONG flags, PVOID memory) {
  (void)flags;
  if (!heap_ok(heap)) return FALSE;
  return memory ? heap_size(heap, 0, memory) != (SIZE_T)-1 : TRUE;
}

EXPORT PVOID NTAPI RtlDestroyHeap(PVOID heap) {
  Heap *h = heap;
  if (!heap_ok(h) || h == NtCurrentPeb()->ProcessHeap) return heap;
  Segment *segment = h->segments;
  PVOID first = h;
  h->magic = 0;
  heaps_remove(h);
  while (segment) {
    Segment *next = segment->next;
    PVOID base = segment->base;
    SIZE_T zero = 0;
    if (base != first) NtFreeVirtualMemory(NtCurrentProcess(), &base, &zero, MEM_RELEASE);
    segment = next;
  }
  SIZE_T zero = 0;
  NtFreeVirtualMemory(NtCurrentProcess(), &first, &zero, MEM_RELEASE);
  return NULL;
}

EXPORT BOOLEAN NTAPI RtlLockHeap(PVOID heap) { (void)heap; heap_lock(); return TRUE; }
EXPORT BOOLEAN NTAPI RtlUnlockHeap(PVOID heap) { (void)heap; if (g_heap_lock && g_heap_owner == TEB_CLIENT_THREAD(NtCurrentTeb())) heap_unlock(); return TRUE; }
EXPORT ULONG NTAPI RtlCompactHeap(PVOID heap, ULONG flags) { (void)heap; (void)flags; return 0; }
EXPORT NTSTATUS NTAPI RtlSetHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T length) { (void)heap; (void)info_class; (void)info; (void)length; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlQueryHeapInformation(PVOID heap, ULONG info_class, PVOID info, SIZE_T length, PSIZE_T returned) {
  (void)heap; (void)info_class; (void)info; (void)length; (void)returned;
  return STATUS_NOT_IMPLEMENTED;
}
EXPORT ULONG NTAPI RtlGetProcessHeaps(ULONG count, PVOID *heaps) {
  ULONG n = 0;
  heaps_lock();
  for (int i = 0; i < MAX_HEAPS; ++i) if (g_heaps[i]) { if (n < count && heaps) heaps[n] = g_heaps[i]; ++n; }
  heaps_unlock();
  return n;
}

/* RtlWalkHeap(heap, entry): the blocks of a heap one after the other, in the layout of PROCESS_HEAP_ENTRY (lpData NULL starts). */
typedef struct { PVOID lpData; DWORD cbData; BYTE cbOverhead, iRegionIndex; WORD wFlags; ULONGLONG region[3]; } HeapWalkEntry;
static Chunk *segment_first_chunk(Segment *s) { return (Chunk *)((BYTE *)(((ULONG_PTR)s + sizeof(Segment) + 15) & ~15ull)); }
EXPORT NTSTATUS NTAPI RtlWalkHeap(PVOID heap, PVOID entry) {
  Heap *h = heap;
  HeapWalkEntry *e = entry;
  if (!heap_ok(h) || !e) return STATUS_INVALID_HANDLE;
  Segment *segments[64];
  int count = 0;
  for (Segment *s = h->segments; s && count < 64; s = s->next) segments[count++] = s;
  NTSTATUS status = 0x8000001A;      /* STATUS_NO_MORE_ENTRIES */
  heap_lock();
  int region = 0;
  Chunk *c = NULL;
  if (!e->lpData) { if (count > 0) c = segment_first_chunk(segments[0]); }
  else { region = e->iRegionIndex; c = chunk_next((Chunk *)((BYTE *)e->lpData - 16)); }
  while (c) {
    if (CHUNK_SIZE(c) != 0) break;      /* (a segment ends with a chunk of size 0) */
    if (++region >= count) { c = NULL; break; }
    c = segment_first_chunk(segments[region]);
  }
  if (c) {
    e->lpData = (BYTE *)c + 16;
    e->cbData = (c->size_flags & CHUNK_USED) ? (DWORD)*(ULONGLONG *)((BYTE *)c + CHUNK_SIZE(c)) : (DWORD)(CHUNK_SIZE(c) - 16);
    e->cbOverhead = 8;
    e->iRegionIndex = (BYTE)region;
    e->wFlags = (c->size_flags & CHUNK_USED) ? 4 /* PROCESS_HEAP_ENTRY_BUSY */ : 0;
    status = STATUS_SUCCESS;
  }
  heap_unlock();
  return status;
}

static PVOID process_heap(void) {
  PEB *peb = NtCurrentPeb();
  if (!peb->ProcessHeap) { heap_lock(); if (!peb->ProcessHeap) peb->ProcessHeap = heap_new(0, 0); heap_unlock(); }
  return peb->ProcessHeap;
}
EXPORT PVOID NTAPI RtlProcessHeap(void) { return process_heap(); }

#include "fls.inc"

#include "rtl.inc"
#include "libc.inc"
#include "rtl2.inc"
#include "reg.inc"

/* ---- critical sections (one thread: they only count) ---------------------------------------------------------------------------------------------------- */

EXPORT NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs) {
  cs->DebugInfo = NULL;
  cs->LockCount = -1;
  cs->RecursionCount = 0;
  cs->OwningThread = NULL;
  cs->LockSemaphore = NULL;
  cs->SpinCount = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION cs, ULONG spin, ULONG flags) { (void)spin; (void)flags; return RtlInitializeCriticalSection(cs); }
EXPORT NTSTATUS NTAPI RtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION cs, ULONG spin) { (void)spin; return RtlInitializeCriticalSection(cs); }
/* LockCount is -1 while the section is free, and the recursion depth minus one while a thread holds it. A thread that finds it taken gives its time slice away until the owner
 * has let go. */
EXPORT NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
  const HANDLE me = (HANDLE)TEB_CLIENT_THREAD(NtCurrentTeb());
  if (cs->OwningThread == me && cs->RecursionCount > 0) {      /* (only the owner can see itself here) */
    ++cs->RecursionCount;
    ++cs->LockCount;
    return STATUS_SUCCESS;
  }
  while (!__sync_bool_compare_and_swap(&cs->LockCount, -1, 0)) {
    if (g_process_ending) { cs->LockCount = 0; break; }      /* (the other threads are gone: a section one of them held is taken over rather than waited for) */
    NtYieldExecution();
  }
  cs->OwningThread = me;
  cs->RecursionCount = 1;
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
  const HANDLE me = (HANDLE)TEB_CLIENT_THREAD(NtCurrentTeb());
  if (cs->OwningThread == me && cs->RecursionCount > 0) { ++cs->RecursionCount; ++cs->LockCount; return TRUE; }
  if (!__sync_bool_compare_and_swap(&cs->LockCount, -1, 0)) return FALSE;
  cs->OwningThread = me;
  cs->RecursionCount = 1;
  return TRUE;
}
EXPORT NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs) {
  if (cs->RecursionCount <= 0) return STATUS_SUCCESS;
  if (--cs->RecursionCount > 0) { --cs->LockCount; return STATUS_SUCCESS; }
  cs->OwningThread = NULL;
  __sync_lock_test_and_set(&cs->LockCount, -1);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs) { cs->LockCount = -1; cs->RecursionCount = 0; cs->OwningThread = NULL; return STATUS_SUCCESS; }

/* ---- exceptions: unwinding, dispatching, __C_specific_handler ----------------------------------------------------------------------------------------------- */

#include "except.inc"
EXPORT ULONG DbgPrint(const char *format, ...) { (void)format; return 0; }
/* SSE and SSE2 are there (PF_XMMI_INSTRUCTIONS_AVAILABLE = 6, PF_XMMI64_INSTRUCTIONS_AVAILABLE = 10); nothing newer is reported. */
EXPORT BOOLEAN NTAPI RtlIsProcessorFeaturePresent(ULONG feature) { return feature == 6 || feature == 10; }

/* ---- process start-up ------------------------------------------------------------------------------------------------------------------------------- */

typedef BOOL (WINAPI *DllEntry)(HANDLE, DWORD, PVOID);
typedef int (WINAPI *ProgramEntry)(PVOID);

/* The thread of a program starts here: the kernel puts the program's entry point in the first argument and the PEB in the second. The libraries' entry points run first
 * (in the order the kernel listed them: a library after the libraries it imports), then the program's. Its return value ends the process. */
EXPORT void NTAPI RtlUserThreadStart(ProgramEntry entry, PVOID parameter) {
  PEB *peb = NtCurrentPeb();
  PEB_LDR_DATA *ldr = peb->Ldr;
  process_heap();
  fls_thread_begins();
  if (ldr) {
    loader_lock();
    /* the libraries in the order they depend on each other, then the program's TLS callbacks, then the program */
    LIST_ENTRY *head = &ldr->InInitializationOrderModuleList;
    for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
      LDR_DATA_TABLE_ENTRY *module = (LDR_DATA_TABLE_ENTRY *)((BYTE *)e - __builtin_offsetof(LDR_DATA_TABLE_ENTRY, InInitializationOrderLinks));
      notify_module(module, DLL_PROCESS_ATTACH, (PVOID)1);
    }
    for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList; e = e->Flink) if (is_program(entry_of(e))) notify_module(entry_of(e), DLL_PROCESS_ATTACH, (PVOID)1);
    loader_unlock();
  }
  RtlExitUserProcess((NTSTATUS)entry(parameter));
}

/* ---- libraries loaded after start-up (LoadLibrary, FreeLibrary) ------------------------------------------------------------------------------------- */

typedef struct { ULONG64 base, size, entry; char name[40]; } LoadRecord;      /* what NtLoadModule and NtReleaseModule report */
#define LDR_STATIC 0xFFFF
#define MAX_LOAD_RECORDS 32
#define STATUS_DLL_NOT_FOUND ((NTSTATUS)0xC0000135)
#define STATUS_DLL_INIT_FAILED ((NTSTATUS)0xC0000142)

static LDR_DATA_TABLE_ENTRY *ldr_find_base(PVOID base) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList; e = e->Flink) if (entry_of(e)->DllBase == base) return entry_of(e);
  return NULL;
}
static void ldr_append(LIST_ENTRY *head, LIST_ENTRY *e) {
  e->Flink = head;
  e->Blink = head->Blink;
  head->Blink->Flink = e;
  head->Blink = e;
}
static void ldr_remove(LIST_ENTRY *e) {
  e->Blink->Flink = e->Flink;
  e->Flink->Blink = e->Blink;
}

/* A loader entry for a library the kernel mapped: listed in the three module lists. `path` (a UNICODE_STRING) is the file the program asked for; a library that came in
 * because another imports it is in C:\Windows\System32. */
static LDR_DATA_TABLE_ENTRY *ldr_add(const LoadRecord *record, const UNICODE_STRING *path, int mapped_only) {
  {      /* a program image loaded as a library is only mapped, like a DONT_RESOLVE load */
    const BYTE *image = (const BYTE *)(ULONG_PTR)record->base;
    const ULONG nt = *(const ULONG *)(image + 0x3C);
    if (!(*(const USHORT *)(image + nt + 22) & 0x2000)) mapped_only = 1;      /* IMAGE_FILE_DLL */
  }
  static const WCHAR system32[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', '3', '2', '\\', 0};
  size_t_ base_length = strlen(record->name);
  size_t_ full_length = path ? path->Length / 2 : 20 + base_length;
  if (path) {      /* the file name at the end of the path is the library's name */
    size_t_ i = full_length;
    while (i > 0 && path->Buffer[i - 1] != '\\' && path->Buffer[i - 1] != '/') --i;
    base_length = full_length - i;
  }
  const SIZE_T bytes = sizeof(LDR_DATA_TABLE_ENTRY) + (full_length + 1) * 2;
  LDR_DATA_TABLE_ENTRY *e = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, bytes);
  if (!e) return NULL;
  WCHAR *full = (WCHAR *)(e + 1);
  if (path) {
    memcpy(full, path->Buffer, full_length * 2);
  } else {
    memcpy(full, system32, 20 * 2);
    for (size_t_ i = 0; i < base_length; ++i) full[20 + i] = (WCHAR)(BYTE)record->name[i];
  }
  full[full_length] = 0;
  e->DllBase = (PVOID)(ULONG_PTR)record->base;
  e->EntryPoint = (PVOID)(ULONG_PTR)record->entry;
  e->SizeOfImage = (ULONG)record->size;
  e->FullDllName.Buffer = full;
  e->FullDllName.Length = (USHORT)(full_length * 2);
  e->FullDllName.MaximumLength = (USHORT)(full_length * 2 + 2);
  e->BaseDllName.Buffer = full + (full_length - base_length);
  e->BaseDllName.Length = (USHORT)(base_length * 2);
  e->BaseDllName.MaximumLength = (USHORT)(base_length * 2 + 2);
  e->Flags = 0x4 | (mapped_only ? LDRP_MAPPED_ONLY : 0);      /* LDRP_IMAGE_DLL */
  e->LoadCount = 1;
  {      /* TlsIndex: -1 when the image has thread-local data that was set up */
    const BYTE *image = (const BYTE *)e->DllBase;
    const ULONG nt = *(const ULONG *)(image + 0x3C);
    e->TlsIndex = (!mapped_only && *(const ULONG *)(image + nt + 24 + 0x70 + 9 * 8) != 0) ? 0xFFFF : 0;
  }
  if (mapped_only) e->EntryPoint = NULL;
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  ldr_append(&ldr->InLoadOrderModuleList, &e->InLoadOrderLinks);
  ldr_append(&ldr->InMemoryOrderModuleList, &e->InMemoryOrderLinks);
  ldr_append(&ldr->InInitializationOrderModuleList, &e->InInitializationOrderLinks);
  return e;
}
/* DLL notifications (LdrRegisterDllNotification): the callbacks are told when a library is loaded or unloaded, with the loader lock held. */
typedef struct { ULONG Flags; const UNICODE_STRING *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } LdrDllNotificationData;
typedef void (NTAPI *LdrDllNotificationFunction)(ULONG reason, const LdrDllNotificationData *data, PVOID context);
#define MAX_DLL_NOTIFICATIONS 16
static struct { LdrDllNotificationFunction function; PVOID context; } g_dll_notifications[MAX_DLL_NOTIFICATIONS];
static void notify_dll_event(ULONG reason, LDR_DATA_TABLE_ENTRY *e) {
  LdrDllNotificationData data;
  data.Flags = 0;
  data.FullDllName = &e->FullDllName;
  data.BaseDllName = &e->BaseDllName;
  data.DllBase = e->DllBase;
  data.SizeOfImage = e->SizeOfImage;
  for (ULONG i = 0; i < MAX_DLL_NOTIFICATIONS; ++i) if (g_dll_notifications[i].function) g_dll_notifications[i].function(reason, &data, g_dll_notifications[i].context);
}
EXPORT NTSTATUS NTAPI LdrRegisterDllNotification(ULONG flags, LdrDllNotificationFunction function, PVOID context, PVOID *cookie) {
  if (flags || !function || !cookie) return STATUS_INVALID_PARAMETER;
  loader_lock();
  for (ULONG i = 0; i < MAX_DLL_NOTIFICATIONS; ++i) {
    if (g_dll_notifications[i].function) continue;
    g_dll_notifications[i].function = function;
    g_dll_notifications[i].context = context;
    *cookie = &g_dll_notifications[i];
    loader_unlock();
    return STATUS_SUCCESS;
  }
  loader_unlock();
  return STATUS_NO_MEMORY;
}
EXPORT NTSTATUS NTAPI LdrUnregisterDllNotification(PVOID cookie) {
  loader_lock();
  for (ULONG i = 0; i < MAX_DLL_NOTIFICATIONS; ++i) {
    if (cookie != &g_dll_notifications[i] || !g_dll_notifications[i].function) continue;
    g_dll_notifications[i].function = NULL;
    loader_unlock();
    return STATUS_SUCCESS;
  }
  loader_unlock();
  return STATUS_INVALID_PARAMETER;
}
EXPORT NTSTATUS NTAPI LdrFindEntryForAddress(PVOID address, LDR_DATA_TABLE_ENTRY **entry) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (!ldr) return STATUS_NO_MORE_ENTRIES;
  for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)e;
    if ((BYTE *)address >= (BYTE *)m->DllBase && (BYTE *)address < (BYTE *)m->DllBase + m->SizeOfImage) { *entry = m; return STATUS_SUCCESS; }
  }
  return STATUS_NO_MORE_ENTRIES;
}

static void ldr_forget(LDR_DATA_TABLE_ENTRY *e) {
  notify_dll_event(2 /* LDR_DLL_NOTIFICATION_REASON_UNLOADED */, e);
  ldr_remove(&e->InLoadOrderLinks);
  ldr_remove(&e->InMemoryOrderLinks);
  ldr_remove(&e->InInitializationOrderLinks);
  RtlFreeHeap(process_heap(), 0, e);
}

/* One use less of the library at `base`; what nobody uses any more is told about it (`detach`), taken out of the lists and unmapped. Loader lock held. */
static void ldr_release(PVOID base, int detach) {
  LoadRecord doomed[MAX_LOAD_RECORDS];
  ULONG count = 0;
  if (!NT_SUCCESS(NtReleaseModule(base, doomed, MAX_LOAD_RECORDS, &count))) return;
  for (ULONG i = 0; detach && i < count; ++i) {
    LDR_DATA_TABLE_ENTRY *e = ldr_find_base((PVOID)(ULONG_PTR)doomed[i].base);
    if (e) notify_module(e, DLL_PROCESS_DETACH, NULL);
  }
  for (ULONG i = 0; i < count; ++i) {
    LDR_DATA_TABLE_ENTRY *e = ldr_find_base((PVOID)(ULONG_PTR)doomed[i].base);
    if (e) ldr_forget(e);
  }
  for (ULONG i = 0; i < count; ++i) NtUnloadModule((PVOID)(ULONG_PTR)doomed[i].base);
}

/* Loads the DLL file at `name` (a Win32 path; or just a library's name when it is loaded already) and what it imports; calls the entry points. */
static NTSTATUS ldr_load_one(PULONG flags, PUNICODE_STRING name, PVOID *handle) {
  const int mapped_only = flags && (*flags & 2);      /* LDR_DONT_RESOLVE_REFS */
  LoadRecord records[MAX_LOAD_RECORDS];
  ULONG count = 0;
  PVOID base = NULL;
  loader_lock();
  NTSTATUS status = NtLoadModule(name, &base, records, MAX_LOAD_RECORDS, &count, mapped_only ? 1 : 0);
  if (!NT_SUCCESS(status)) { loader_unlock(); return status; }
  if (count == 0) {      /* it was there: one more use (or a library the process started with) */
    *handle = base;
    loader_unlock();
    return STATUS_SUCCESS;
  }
  LDR_DATA_TABLE_ENTRY *added[MAX_LOAD_RECORDS];
  ULONG listed = 0;
  for (; listed < count; ++listed) {
    added[listed] = ldr_add(&records[listed], records[listed].base == (ULONG64)(ULONG_PTR)base ? name : NULL, mapped_only);
    if (!added[listed]) break;
  }
  ULONG attached = 0;
  if (listed == count) {
    for (; attached < count; ++attached) if (!notify_module(added[attached], DLL_PROCESS_ATTACH, NULL)) break;
    if (attached == count) for (ULONG i = 0; i < count; ++i) notify_dll_event(1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */, added[i]);
  }
  if (listed == count && attached == count) {
    *handle = base;
    loader_unlock();
    return STATUS_SUCCESS;
  }
  /* it did not start: the libraries that did are told, then everything goes again */
  for (ULONG i = attached; i > 0; --i) notify_module(added[i - 1], DLL_PROCESS_DETACH, NULL);
  ldr_release(base, 0);
  loader_unlock();
  return listed == count ? STATUS_DLL_INIT_FAILED : STATUS_NO_MEMORY;
}

/* A bare file name is looked for in the directories of the search path (separated by ';'): a file that is found but cannot be loaded does not stop the search; if nothing
 * loads, the answer is the error of the first file that was found (or "not found"). */
/* The folder SetDllDirectory names (searched by LoadLibrary after the program's own). */
static WCHAR g_dll_directory_text[520];
static USHORT g_dll_directory_chars;
EXPORT NTSTATUS NTAPI LdrSetDllDirectory(PUNICODE_STRING directory) {
  const USHORT n = directory ? (USHORT)(directory->Length / 2) : 0;
  if (n >= 520) return 0xC0000106;      /* STATUS_NAME_TOO_LONG */
  for (USHORT i = 0; i < n; i++) g_dll_directory_text[i] = directory->Buffer[i];
  g_dll_directory_text[n] = 0;
  g_dll_directory_chars = n;
  return 0;
}
EXPORT NTSTATUS NTAPI LdrGetDllDirectory(PUNICODE_STRING directory) {
  const ULONG need = ((ULONG)g_dll_directory_chars + 1) * 2;
  if (directory->MaximumLength < need) {
    if (directory->MaximumLength >= 2 && directory->Buffer) directory->Buffer[0] = 0;
    directory->Length = (USHORT)need;
    return 0xC0000023;      /* STATUS_BUFFER_TOO_SMALL */
  }
  for (USHORT i = 0; i <= g_dll_directory_chars; i++) directory->Buffer[i] = g_dll_directory_text[i];
  directory->Length = (USHORT)(g_dll_directory_chars * 2);
  return 0;
}
#define MAX_USER_DLL_DIRS 16
static struct { WCHAR path[520]; BOOLEAN used; ULONG sequence; } g_user_dll_dirs[MAX_USER_DLL_DIRS];
static ULONG g_user_dll_sequence;
EXPORT NTSTATUS NTAPI LdrAddDllDirectory(PUNICODE_STRING directory, PVOID *cookie) {
  const USHORT n = directory->Length / 2;
  if (n >= 520 || n < 1) return STATUS_INVALID_PARAMETER;
  for (int i = 0; i < MAX_USER_DLL_DIRS; i++) {
    if (g_user_dll_dirs[i].used) continue;
    for (USHORT k = 0; k < n; k++) g_user_dll_dirs[i].path[k] = directory->Buffer[k];
    g_user_dll_dirs[i].path[n] = 0;
    g_user_dll_dirs[i].used = TRUE;
    g_user_dll_dirs[i].sequence = ++g_user_dll_sequence;
    *cookie = (PVOID)(ULONG_PTR)(i + 1);
    return STATUS_SUCCESS;
  }
  return STATUS_NO_MEMORY;
}
EXPORT NTSTATUS NTAPI LdrRemoveDllDirectory(PVOID cookie) {
  const ULONG_PTR i = (ULONG_PTR)cookie;
  if (i < 1 || i > MAX_USER_DLL_DIRS || !g_user_dll_dirs[i - 1].used) return STATUS_INVALID_PARAMETER;
  g_user_dll_dirs[i - 1].used = FALSE;
  return STATUS_SUCCESS;
}
/* The folders a set of LOAD_LIBRARY_SEARCH_* flags names, separated by ';' (application 0x200, System32 0x800; the user folders belong to kernel32). */
static BOOLEAN search_flags_path(ULONG flags, WCHAR *out, SIZE_T capacity) {
  static const WCHAR system32[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', '3', '2', 0};
  SIZE_T n = 0;
  out[0] = 0;
  if (flags & 0x1000) flags |= 0xE00;      /* LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: application, user and System32 folders */
  if (flags & 0x200) {      /* the folder of the program: ImagePathName of the process parameters, without the file */
    const UNICODE_STRING *image = (const UNICODE_STRING *)(*(const BYTE *const *)(*(const BYTE *const *)((const BYTE *)NtCurrentTeb() + 0x60) + 0x20) + 0x60);
    SIZE_T length = image->Length / 2;
    while (length > 0 && image->Buffer[length - 1] != '\\') --length;
    if (length > 1 && length < capacity) {
      for (SIZE_T i = 0; i + 1 < length; ++i) out[n++] = image->Buffer[i];
      out[n++] = ';';
    }
  }
  if (flags & 0x800) {
    for (SIZE_T i = 0; system32[i] && n + 1 < capacity; ++i) out[n++] = system32[i];
    out[n++] = ';';
  }
  if (flags & 0x400) {      /* the folders AddDllDirectory added, the newest first, then the one SetDllDirectory named */
    BOOLEAN done[MAX_USER_DLL_DIRS] = {0};
    for (;;) {
      int best = -1;
      for (int i = 0; i < MAX_USER_DLL_DIRS; i++) if (g_user_dll_dirs[i].used && !done[i] && (best < 0 || g_user_dll_dirs[i].sequence > g_user_dll_dirs[best].sequence)) best = i;
      if (best < 0) break;
      done[best] = TRUE;
      for (SIZE_T i = 0; g_user_dll_dirs[best].path[i] && n + 2 < capacity; ++i) out[n++] = g_user_dll_dirs[best].path[i];
      out[n++] = ';';
    }
    for (SIZE_T i = 0; i < g_dll_directory_chars && n + 2 < capacity; ++i) out[n++] = g_dll_directory_text[i];
    if (g_dll_directory_chars) out[n++] = ';';
  }
  if (n > 0) --n;
  out[n] = 0;
  return TRUE;
}
EXPORT ULONG NTAPI RtlGetFullPathName_U(PCWSTR name, ULONG size, PWSTR buffer, PWSTR *file_part);
EXPORT NTSTATUS NTAPI LdrGetDllPath(PCWSTR name, ULONG flags, PWSTR *path, PWSTR *unused) {
  (void)name;
  WCHAR text[1040];
  if ((flags & 8) && (flags & 0x1F00)) return STATUS_INVALID_PARAMETER;
  search_flags_path(flags ? flags : 0xA00, text, 1040);
  SIZE_T n = 0;
  while (text[n]) ++n;
  PWSTR copy = RtlAllocateHeap(process_heap(), 0, (n + 1) * 2);
  if (!copy) return STATUS_NO_MEMORY;
  for (SIZE_T i = 0; i <= n; ++i) copy[i] = text[i];
  *path = copy;
  if (unused) *unused = NULL;
  return STATUS_SUCCESS;
}
EXPORT VOID NTAPI RtlReleasePath(PWSTR path) { if (path) RtlFreeHeap(process_heap(), 0, path); }
EXPORT NTSTATUS NTAPI LdrLoadDll(PWSTR search_path, PULONG flags, PUNICODE_STRING name, PVOID *handle) {
  WCHAR coded[1040];
  BOOLEAN absolute = name->Length >= 6 && name->Buffer[1] == ':' && (name->Buffer[2] == '\\' || name->Buffer[2] == '/');
  if (name->Length >= 2 && name->Buffer[0] == '\\') absolute = TRUE;      /* (\\server\share, or rooted in the current drive) */
  if ((ULONG_PTR)search_path <= 0xFFFF && ((ULONG_PTR)search_path & 1)) {      /* LOAD_LIBRARY_SEARCH_* flags in place of a path */
    const ULONG search = (ULONG)(ULONG_PTR)search_path & ~1u;
    if (((search & 8) && (search & 0x1F00)) || ((search & 0x100) && !absolute)) return STATUS_INVALID_PARAMETER;
    search_flags_path(search, coded, 1040);
    search_path = coded;
  }
  BOOLEAN bare = TRUE;
  for (USHORT i = 0; i < name->Length / 2; ++i) if (name->Buffer[i] == '\\' || name->Buffer[i] == '/' || name->Buffer[i] == ':') bare = FALSE;
  const BOOLEAN drive_relative = name->Length >= 4 && name->Buffer[1] == ':' && !absolute;      /* ("C:name": the current folder of that drive) */
  if (!bare && !absolute && (!search_path || drive_relative)) {      /* a relative path is taken from the current folder */
    WCHAR full[520], text[520];
    SIZE_T n = name->Length / 2;
    if (n >= 520) return STATUS_NAME_TOO_LONG_;
    for (SIZE_T i = 0; i < n; ++i) text[i] = name->Buffer[i];
    text[n] = 0;
    const ULONG length = RtlGetFullPathName_U(text, sizeof(full), full, NULL);
    if (length == 0 || length >= sizeof(full)) return STATUS_DLL_NOT_FOUND;
    UNICODE_STRING us = { (USHORT)length, (USHORT)(length + 2), full };
    return ldr_load_one(flags, &us, handle);
  }
  if (!search_path || absolute || !search_path[0]) return ldr_load_one(flags, name, handle);
  NTSTATUS first_error = STATUS_DLL_NOT_FOUND;
  BOOLEAN found = FALSE;
  const WCHAR *dir = search_path;
  while (*dir) {
    const WCHAR *end = dir;
    while (*end && *end != ';') ++end;
    const SIZE_T length = (SIZE_T)(end - dir);
    WCHAR full[520];
    if (length > 0 && length + 1 + name->Length / 2 + 1 <= 520) {
      SIZE_T n = 0;
      for (SIZE_T i = 0; i < length; ++i) full[n++] = dir[i];
      if (full[n - 1] != '\\') full[n++] = '\\';
      for (USHORT i = 0; i < name->Length / 2; ++i) full[n++] = name->Buffer[i];
      full[n] = 0;
      WCHAR expanded[520];
      UNICODE_STRING us = { (USHORT)(n * 2), (USHORT)(n * 2 + 2), full };
      if (!(full[1] == ':' && full[2] == '\\') && full[0] != '\\') {      /* (a relative folder in the path: from the current folder) */
        const ULONG got = RtlGetFullPathName_U(full, sizeof(expanded), expanded, NULL);
        if (got != 0 && got < sizeof(expanded)) { us.Buffer = expanded; us.Length = (USHORT)got; us.MaximumLength = (USHORT)(got + 2); }
      }
      const NTSTATUS status = ldr_load_one(flags, &us, handle);
      if (NT_SUCCESS(status)) return status;
      if (status != STATUS_DLL_NOT_FOUND && status != (NTSTATUS)0xC0000034 && status != (NTSTATUS)0xC000003A && !found) { found = TRUE; first_error = status; }
    }
    dir = *end ? end + 1 : end;
  }
  return first_error;
}

/* A loaded library by name, with or without ".dll", in any case; only the file name counts. */
static WCHAR ldr_upper(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
static LDR_DATA_TABLE_ENTRY *ldr_find_name(const UNICODE_STRING *name) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  const WCHAR *text = name->Buffer;
  SIZE_T length = name->Length / 2;
  for (SIZE_T i = 0; i < length; ++i) if (text[i] == '\\' || text[i] == '/') { text += i + 1; length -= i + 1; i = (SIZE_T)-1; }
  const BOOLEAN has_extension = length >= 4 && text[length - 4] == '.';
  for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *m = entry_of(e);
    const SIZE_T base_length = m->BaseDllName.Length / 2;
    const WCHAR *base = m->BaseDllName.Buffer;
    SIZE_T compare = base_length;
    if (!has_extension && base_length >= 4 && base[base_length - 4] == '.') compare = base_length - 4;
    if (compare != length) continue;
    SIZE_T i = 0;
    while (i < length && ldr_upper(base[i]) == ldr_upper(text[i])) ++i;
    if (i == length) return m;
  }
  return NULL;
}
/* flags: UNCHANGED_REFCOUNT 1, PIN 2 (never unloaded); without the first the library has one more use. */
EXPORT NTSTATUS NTAPI LdrGetDllHandleEx(ULONG flags, PWSTR path, PULONG characteristics, PUNICODE_STRING name, PVOID *handle) {
  (void)path; (void)characteristics;
  if ((flags & ~7u) || (flags & 3) == 3) return STATUS_INVALID_PARAMETER;
  if (!name || !handle) return STATUS_INVALID_PARAMETER;
  loader_lock();
  LDR_DATA_TABLE_ENTRY *m = ldr_find_name(name);
  if (!m) { loader_unlock(); return STATUS_DLL_NOT_FOUND; }
  PVOID base = m->DllBase;
  if (!(flags & 1) && m->LoadCount != LDR_STATIC) {
    UNICODE_STRING base_name = m->BaseDllName;
    PVOID again = NULL;
    loader_unlock();
    const NTSTATUS status = LdrLoadDll(NULL, NULL, &base_name, &again);
    if (!NT_SUCCESS(status)) return status;
    loader_lock();
    m = ldr_find_base(base);
  }
  if (m && (flags & 2)) m->LoadCount = LDR_STATIC;
  loader_unlock();
  *handle = base;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI LdrGetDllHandle(PWSTR path, ULONG characteristics, PUNICODE_STRING name, PVOID *handle) {
  return LdrGetDllHandleEx(1, path, NULL, name, handle);
}

EXPORT NTSTATUS NTAPI LdrGetDllFullName(PVOID module, PUNICODE_STRING name) {
  loader_lock();
  LDR_DATA_TABLE_ENTRY *m = ldr_find_base(module ? module : NtCurrentPeb()->ImageBaseAddress);
  if (!m) { loader_unlock(); return STATUS_DLL_NOT_FOUND; }
  const ULONG need = (ULONG)m->FullDllName.Length + 2;
  const BYTE *from = (const BYTE *)m->FullDllName.Buffer;
  BYTE *to = (BYTE *)name->Buffer;
  NTSTATUS status = STATUS_SUCCESS;
  USHORT copied = (USHORT)need;
  if (name->MaximumLength < need) { copied = name->MaximumLength; status = 0xC0000023; }      /* STATUS_BUFFER_TOO_SMALL: what fits is copied */
  for (USHORT i = 0; i < copied; i++) to[i] = i < m->FullDllName.Length ? from[i] : 0;
  name->Length = status ? copied : m->FullDllName.Length;
  loader_unlock();
  return status;
}

/* API sets: the contracts this system answers for (name without the version, the highest version that exists). */
static const struct { const char *base; int maximum; int hidden; } g_api_sets[] = {
  {"api-ms-win-core-console-l1-1-", 0, 0}, {"api-ms-win-core-file-l1-1-", 0, 0}, {"api-ms-win-core-handle-l1-1-", 0, 0}, {"api-ms-win-core-heap-l1-1-", 0, 0},
  {"api-ms-win-core-libraryloader-l1-1-", 0, 0}, {"api-ms-win-core-memory-l1-1-", 0, 0}, {"api-ms-win-core-processthreads-l1-1-", 0, 0}, {"api-ms-win-core-synch-l1-1-", 0, 0},
  {"api-ms-win-core-sysinfo-l1-1-", 0, 0}, {"api-ms-win-core-string-l1-1-", 0, 0}, {"api-ms-win-core-errorhandling-l1-1-", 0, 0}, {"api-ms-win-core-localization-l1-1-", 0, 0},
  {"ext-ms-win-gdi-draw-l1-1-", 1, 0}, {"api-ms-win-deprecated-apis-advapi-l1-1-", 0, 1},
};
static WCHAR ascii_lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }
/* Splits "api-ms-win-core-console-l1-1-0" (not null-terminated, no ".dll") into the contract and its last number. STATUS_INVALID_PARAMETER for anything else. */
static NTSTATUS api_set_lookup(const UNICODE_STRING *name, BOOLEAN strip_extension, BOOLEAN *present_in_table, BOOLEAN *in_schema, BOOLEAN *implemented, BOOLEAN *visible) {
  SIZE_T n = name->Length / 2;
  char text[128];
  if (n == 0 || n >= sizeof(text)) return STATUS_INVALID_PARAMETER;
  for (SIZE_T i = 0; i < n; i++) {
    if (name->Buffer[i] == '.' && strip_extension) { n = i; break; }
    if (name->Buffer[i] == '.' || name->Buffer[i] > 127) return STATUS_INVALID_PARAMETER;
    text[i] = (char)ascii_lower(name->Buffer[i]);
  }
  text[n] = 0;
  const BOOLEAN api = text[0] == 'a' && text[1] == 'p' && text[2] == 'i' && text[3] == '-';
  const BOOLEAN ext = text[0] == 'e' && text[1] == 'x' && text[2] == 't' && text[3] == '-';
  if (!api && !ext) return STATUS_INVALID_PARAMETER;
  *present_in_table = *in_schema = *implemented = *visible = FALSE;
  for (SIZE_T k = 0; k < sizeof(g_api_sets) / sizeof(g_api_sets[0]); k++) {
    SIZE_T l = 0;
    while (g_api_sets[k].base[l] && g_api_sets[k].base[l] == text[l]) l++;
    if (g_api_sets[k].base[l] || l == 0) continue;
    int version = 0;
    SIZE_T i = l;
    if (!text[i]) return STATUS_SUCCESS;      /* (no version: not a contract name) */
    for (; text[i] >= '0' && text[i] <= '9'; i++) version = version * 10 + (text[i] - '0');
    if (text[i] || i == l) return STATUS_SUCCESS;
    *present_in_table = TRUE;
    *visible = !g_api_sets[k].hidden;
    *in_schema = TRUE;
    *implemented = version <= g_api_sets[k].maximum && !g_api_sets[k].hidden;
    if (version > g_api_sets[k].maximum && !g_api_sets[k].hidden) *in_schema = FALSE;
    return STATUS_SUCCESS;
  }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI ApiSetQueryApiSetPresence(const UNICODE_STRING *name, BOOLEAN *present) {
  BOOLEAN known, schema, implemented, visible;
  *present = FALSE;
  if (api_set_lookup(name, TRUE, &known, &schema, &implemented, &visible) == STATUS_SUCCESS) *present = known && visible;      /* (the contract exists in some version; a ".dll" is ignored) */
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI ApiSetQueryApiSetPresenceEx(const UNICODE_STRING *name, BOOLEAN *in_schema, BOOLEAN *present) {
  BOOLEAN known, schema, implemented, visible;
  const NTSTATUS status = api_set_lookup(name, FALSE, &known, &schema, &implemented, &visible);
  if (status) return status;
  *in_schema = schema;
  *present = implemented;
  return STATUS_SUCCESS;
}

EXPORT NTSTATUS NTAPI LdrUnloadDll(PVOID base) {
  loader_lock();
  LDR_DATA_TABLE_ENTRY *e = ldr_find_base(base);
  NTSTATUS status = STATUS_SUCCESS;
  if (!e) status = STATUS_DLL_NOT_FOUND;
  else if (e->LoadCount != LDR_STATIC && !g_process_ending) ldr_release(base, 1);      /* (nothing is unloaded while the process ends) */
  loader_unlock();
  return status;
}

/* ntdll's own entry point: nothing to set up (the heap is made when first used). */
EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) process_heap();
  return TRUE;
}
