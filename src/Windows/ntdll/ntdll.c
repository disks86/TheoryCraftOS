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
NT_SERVICE(NtTcosDebugException);
NT_SERVICE(NtTcosLinkModules);
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
NT_SERVICE_RAW(NtCreateFile);
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

#define OWN_TOWCASE 1
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

/* ---- formatting (the printf family) --------------------------------------------------------------------------------------------------------------------- */

/* Whether a %s or %c argument is a wide string or character: the native width of the printf (%s, %c) or the other one (%S, %C), moved by h (narrow),
   w (wide), both (the other one) and an odd number of l (wide). */
static int fmt_wide_arg(int native_wide, int native_conversion, int has_h, int has_w, int ells) {
  if (has_h && has_w) return !native_wide;
  if (has_w) return 1;
  if (has_h) return 0;
  if (ells & 1) return 1;
  return native_conversion ? native_wide : !native_wide;
}
#define FMT_CH char
#define FMT_WIDE 0
#define FMT_SINK Sink
#define FMT_PUT sink_put
#define FMT_PAD sink_pad
#define FMT_FN vformat
#include "format.inc"
#undef FMT_CH
#undef FMT_WIDE
#undef FMT_SINK
#undef FMT_PUT
#undef FMT_PAD
#undef FMT_FN
#define FMT_CH WCHAR
#define FMT_WIDE 1
#define FMT_SINK WSink
#define FMT_PUT wsink_put
#define FMT_PAD wsink_pad
#define FMT_FN vformatw
#include "format.inc"
#undef FMT_CH
#undef FMT_WIDE
#undef FMT_SINK
#undef FMT_PUT
#undef FMT_PAD
#undef FMT_FN

EXPORT int vsnprintf(char *buffer, size_t_ capacity, const char *format, va_list_ args) { return vformat(buffer, capacity, 0, format, args); }
EXPORT int _vsnprintf(char *b, size_t_ n, const char *f, va_list_ a) { return vformat(b, n, 1, f, a); }
EXPORT int _vsnwprintf(WCHAR *b, size_t_ n, const WCHAR *f, va_list_ a) { return vformatw(b, n, 1, f, a); }
EXPORT int _snwprintf(WCHAR *b, size_t_ n, const WCHAR *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vformatw(b, n, 1, f, a); __builtin_va_end(a); return r; }
EXPORT int swprintf(WCHAR *b, size_t_ n, const WCHAR *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vformatw(b, n, 0, f, a); __builtin_va_end(a); return r < 0 || (size_t_)r >= n ? -1 : r; }
EXPORT int vswprintf(WCHAR *b, size_t_ n, const WCHAR *f, va_list_ a) { int r = vformatw(b, n, 0, f, a); return r < 0 || (size_t_)r >= n ? -1 : r; }
EXPORT int vsprintf(char *b, const char *f, va_list_ a) { return vsnprintf(b, (size_t_)-1 >> 1, f, a); }
EXPORT int snprintf(char *b, size_t_ n, const char *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vsnprintf(b, n, f, a); __builtin_va_end(a); return r; }
EXPORT int _snprintf(char *b, size_t_ n, const char *f, ...) { va_list_ a; __builtin_va_start(a, f); int r = vformat(b, n, 1, f, a); __builtin_va_end(a); return r; }
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

#define STATUS_REVISION_MISMATCH ((NTSTATUS)0xC0000059)
/* The operating system calls itself Windows 10 (10.0, build 19045), a workstation without a service pack. */
EXPORT NTSTATUS NTAPI RtlGetVersion(RTL_OSVERSIONINFOW *info) {
  if (!info || (info->dwOSVersionInfoSize != sizeof(RTL_OSVERSIONINFOW) && info->dwOSVersionInfoSize != sizeof(RTL_OSVERSIONINFOEXW))) return STATUS_INVALID_PARAMETER;
  info->dwMajorVersion = 10;
  info->dwMinorVersion = 0;
  info->dwBuildNumber = 19045;
  info->dwPlatformId = 2;
  info->szCSDVersion[0] = 0;
  if (info->dwOSVersionInfoSize == sizeof(RTL_OSVERSIONINFOEXW)) {
    RTL_OSVERSIONINFOEXW *ex = (RTL_OSVERSIONINFOEXW *)info;
    ex->wServicePackMajor = ex->wServicePackMinor = 0;
    ex->wSuiteMask = 0x0100;      /* VER_SUITE_SINGLEUSERTS */
    ex->wProductType = 1;         /* VER_NT_WORKSTATION */
    ex->wReserved = 0;
  }
  return STATUS_SUCCESS;
}
EXPORT VOID NTAPI RtlGetNtVersionNumbers(ULONG *major, ULONG *minor, ULONG *build) {
  if (major) *major = 10;
  if (minor) *minor = 0;
  if (build) *build = 19045 | 0xF0000000u;      /* (the top bits say it is a free build) */
}
EXPORT ULONGLONG NTAPI VerSetConditionMask(ULONGLONG mask, ULONG type, UCHAR condition) {
  condition &= 7;
  if (type & 0x80) mask |= (ULONGLONG)condition << 21;      /* VER_PRODUCT_TYPE */
  else if (type & 0x40) mask |= (ULONGLONG)condition << 18;      /* VER_SUITENAME */
  else if (type & 0x20) mask |= (ULONGLONG)condition << 15;      /* VER_SERVICEPACKMAJOR */
  else if (type & 0x10) mask |= (ULONGLONG)condition << 12;      /* VER_SERVICEPACKMINOR */
  else if (type & 8) mask |= (ULONGLONG)condition << 9;      /* VER_PLATFORMID */
  else if (type & 4) mask |= (ULONGLONG)condition << 6;      /* VER_BUILDNUMBER */
  else if (type & 2) mask |= (ULONGLONG)condition << 3;      /* VER_MAJORVERSION */
  else if (type & 1) mask |= (ULONGLONG)condition;      /* VER_MINORVERSION */
  return mask;
}
/* The condition of a field of the chain (major, minor, service pack major, service pack minor), given the one before it: an EQUAL before it gives way to the new
 * condition if that is one of the comparisons, a GREATER or LESS keeps what it said unless the new one is compatible (the way Windows does it). */
static UCHAR version_update_condition(UCHAR *last, UCHAR condition) {
  switch (*last) {
    case 0: *last = condition; break;
    case 1: if (condition >= 1 && condition <= 5) { *last = condition; return condition; } break;      /* VER_EQUAL */
    case 2: case 3: if (condition >= 1 && condition <= 3) return condition; break;      /* VER_GREATER, VER_GREATER_EQUAL */
    case 4: case 5: if (condition == 1 || (condition >= 4 && condition <= 5)) return condition; break;      /* VER_LESS, VER_LESS_EQUAL */
  }
  if (!condition) *last |= 0x10;
  return *last & 0xf;
}
static NTSTATUS version_compare_values(ULONG left, ULONG right, UCHAR condition) {
  switch (condition) {
    case 1: if (left != right) return STATUS_REVISION_MISMATCH; break;      /* VER_EQUAL */
    case 2: if (left <= right) return STATUS_REVISION_MISMATCH; break;      /* VER_GREATER */
    case 3: if (left < right) return STATUS_REVISION_MISMATCH; break;      /* VER_GREATER_EQUAL */
    case 4: if (left >= right) return STATUS_REVISION_MISMATCH; break;      /* VER_LESS */
    case 5: if (left > right) return STATUS_REVISION_MISMATCH; break;      /* VER_LESS_EQUAL */
    default: return STATUS_REVISION_MISMATCH;
  }
  return STATUS_SUCCESS;
}
/* Does this system meet the version asked for? The conditions are three bits per field in `mask`. (Ported from Wine's version.c.) STATUS_REVISION_MISMATCH if not. */
EXPORT NTSTATUS NTAPI RtlVerifyVersionInfo(const RTL_OSVERSIONINFOEXW *info, ULONG types, ULONGLONG mask) {
  RTL_OSVERSIONINFOEXW ver;
  NTSTATUS status;
  ver.dwOSVersionInfoSize = sizeof(ver);
  if ((status = RtlGetVersion((RTL_OSVERSIONINFOW *)&ver)) != STATUS_SUCCESS) return status;
  if (!(types && mask) || !info) return STATUS_INVALID_PARAMETER;
  if (types & 0x80) { status = version_compare_values(ver.wProductType, info->wProductType, (UCHAR)(mask >> 21 & 7)); if (status != STATUS_SUCCESS) return status; }
  if (types & 0x40) {      /* VER_SUITENAME: VER_AND needs every bit, VER_OR any */
    switch (mask >> 18 & 7) {
      case 6: if ((info->wSuiteMask & ver.wSuiteMask) != info->wSuiteMask) return STATUS_REVISION_MISMATCH; break;
      case 7: if (!(info->wSuiteMask & ver.wSuiteMask) && info->wSuiteMask) return STATUS_REVISION_MISMATCH; break;
      default: return STATUS_INVALID_PARAMETER;
    }
  }
  if (types & 8) { status = version_compare_values(ver.dwPlatformId, info->dwPlatformId, (UCHAR)(mask >> 9 & 7)); if (status != STATUS_SUCCESS) return status; }
  if (types & 4) { status = version_compare_values(ver.dwBuildNumber, info->dwBuildNumber, (UCHAR)(mask >> 6 & 7)); if (status != STATUS_SUCCESS) return status; }
  if (types & (2 | 1 | 0x20 | 0x10)) {
    UCHAR condition, last_condition = 0;
    BOOLEAN do_next_check = TRUE;
    if (types & 2) {
      condition = version_update_condition(&last_condition, (UCHAR)(mask >> 3 & 7));
      status = version_compare_values(ver.dwMajorVersion, info->dwMajorVersion, condition);
      do_next_check = ver.dwMajorVersion == info->dwMajorVersion && condition >= 1 && condition <= 5;
    }
    if ((types & 1) && do_next_check) {
      condition = version_update_condition(&last_condition, (UCHAR)(mask & 7));
      status = version_compare_values(ver.dwMinorVersion, info->dwMinorVersion, condition);
      do_next_check = ver.dwMinorVersion == info->dwMinorVersion && condition >= 1 && condition <= 5;
    }
    if ((types & 0x20) && do_next_check) {
      condition = version_update_condition(&last_condition, (UCHAR)(mask >> 15 & 7));
      status = version_compare_values(ver.wServicePackMajor, info->wServicePackMajor, condition);
      do_next_check = ver.wServicePackMajor == info->wServicePackMajor && condition >= 1 && condition <= 5;
    }
    if ((types & 0x10) && do_next_check) {
      condition = version_update_condition(&last_condition, (UCHAR)(mask >> 12 & 7));
      status = version_compare_values(ver.wServicePackMinor, info->wServicePackMinor, condition);
    }
    if (status != STATUS_SUCCESS) return status;
  }
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
/* The entry point of a library runs under a guard (whatever it is told, as it is by the loader of Windows): a fault nobody handles ends the call (the library failed to start) instead of the process. */
static void **volatile g_init_guard;
static volatile PVOID g_init_guard_teb;
static BOOL call_library_entry(LDR_DATA_TABLE_ENTRY *m, DWORD reason, PVOID reserved) {
  BOOL (WINAPI *entry)(PVOID, DWORD, PVOID) = (BOOL (WINAPI *)(PVOID, DWORD, PVOID))m->EntryPoint;
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
static volatile LONG g_cs_taken_over;      /* a section its owner died holding was taken over while the process ended */
static void fls_destroy_current(int free_structure);
static volatile LONG g_heap_lock;
static volatile LONG g_fls_lock;
static void fls_thread_begins(void);
EXPORT void NTAPI RtlFreeThreadActivationContextStack(void);
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

/* Is the process shutting down (ExitProcess has begun to tell the libraries)? Windows answers FALSE once a library's detach has had to take over a section that a
 * terminated thread still held (the loader test's phase 7), so that is what a takeover does here too. */
EXPORT BOOLEAN NTAPI RtlDllShutdownInProgress(void) { return g_process_ending != 0 && !g_cs_taken_over; }

/* The thread ends (the libraries and the program are told); the process ends with its last thread. */
EXPORT void NTAPI RtlExitUserThread(NTSTATUS status) {
  fls_destroy_current(0);
  if (!g_process_ending) notify_all_modules(DLL_THREAD_DETACH, NULL, 0);
  fls_destroy_current(1);
  RtlFreeThreadActivationContextStack();
  { HANDLE *debug_object = (HANDLE *)((BYTE *)NtCurrentTeb() + 0x1660); if (*debug_object) { NtClose(*debug_object); *debug_object = NULL; } }      /* (a thread that debugged lets go of it) */
  for (;;) NtTerminateThread((HANDLE)(LONG_PTR)-2, status);
}

/* New threads begin here (the kernel passes the thread's function and parameter in the first two argument registers); the function's return value is the thread's exit code. */
/* Threads start with the floating point state Windows gives them: 53 bit precision, every exception masked, round to nearest. */
static void reset_floating_point(void) {
  static const unsigned short cw = 0x27f;
  static const unsigned mxcsr = 0x1f80;
  __asm__ volatile("fldcw %0; ldmxcsr %1" : : "m"(cw), "m"(mxcsr));
}
EXPORT void NTAPI TcosThreadStart(LPTHREAD_START_ROUTINE start, PVOID parameter) {
  reset_floating_point();
  if (!TEB_TLS_POINTER(NtCurrentTeb())) TEB_TLS_POINTER(NtCurrentTeb()) = *(PVOID **)((BYTE *)NtCurrentTeb() + 0x1788);      /* (the kernel left the thread-local array where it could not be seen) */
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
    if (!apc[0]) continue;      /* (an APC with no routine only ends the wait) */
    ((void (NTAPI *)(ULONG_PTR, ULONG_PTR, ULONG_PTR))apc[0])((ULONG_PTR)apc[1], (ULONG_PTR)apc[2], (ULONG_PTR)apc[3]);
    ++count;
  }
  return count;
}
EXPORT NTSTATUS NTAPI NtTestAlert(void) { run_apcs(); return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI NtWaitForSingleObject(HANDLE h, BOOLEAN alertable, PLARGE_INTEGER timeout) {
  const NTSTATUS status = RawNtWaitForSingleObject(h, alertable, timeout);
  if (status == STATUS_USER_APC && !run_apcs()) return STATUS_SUCCESS;      /* (only routine-less ones) */
  return status;
}
/* ---- console handles without a screen buffer ---------------------------------------------------------------------------------------------------------------- *
 * "\Device\ConDrv\Input" and "\Output" (and CurrentIn, CurrentOut, "\??\CONIN$", "\??\CONOUT$") open a handle to the console of the process that is not bound to a screen buffer or to the
 * standard handles. Here it is a named event (TcosConI_ or TcosConO_, the console, the process, a number): whoever owns the console sets it while the console has input waiting, and
 * the name says what the handle is to any process it was duplicated into. kernel32.dll keeps the state; the handles made are listed here so that it finds them. */
typedef struct { HANDLE handle; ULONG_PTR console; LONG kind; } ConEvent;      /* kind: 1 input, 2 output */
#define CON_EVENTS 64
static ConEvent g_con_events[CON_EVENTS];
static volatile LONG g_con_event_lock, g_con_event_serial;
static void con_event_lock(void) { while (__sync_lock_test_and_set(&g_con_event_lock, 1)) NtYieldExecution(); }
static void con_event_unlock(void) { __sync_lock_release(&g_con_event_lock); }
EXPORT void NTAPI RtlTcosConsoleEventNote(HANDLE h, ULONG_PTR console, LONG kind) {
  con_event_lock();
  int free_at = -1;
  for (int i = 0; i < CON_EVENTS; ++i) {
    if (g_con_events[i].handle == h) { free_at = i; break; }
    if (!g_con_events[i].handle && free_at < 0) free_at = i;
  }
  if (free_at >= 0) { g_con_events[free_at].handle = h; g_con_events[free_at].console = console; g_con_events[free_at].kind = kind; }
  con_event_unlock();
}
EXPORT void NTAPI RtlTcosConsoleEventForget(HANDLE h) {
  con_event_lock();
  for (int i = 0; i < CON_EVENTS; ++i) if (g_con_events[i].handle == h) g_con_events[i].handle = NULL;
  con_event_unlock();
}
EXPORT int NTAPI RtlTcosConsoleEvents(ConEvent *out, int max) {
  int n = 0;
  con_event_lock();
  for (int i = 0; i < CON_EVENTS && (!out || n < max); ++i) if (g_con_events[i].handle) { if (out) out[n] = g_con_events[i]; ++n; }      /* (no buffer: only how many) */
  con_event_unlock();
  return n;
}
static BOOL console_device_name(const UNICODE_STRING *name, LONG *kind) {
  static const WCHAR *const names[] = {L"\\Device\\ConDrv\\Input", L"\\Device\\ConDrv\\CurrentIn", L"\\??\\CONIN$", L"\\Device\\ConDrv\\Output", L"\\Device\\ConDrv\\CurrentOut", L"\\??\\CONOUT$"};
  if (!name || !name->Buffer) return FALSE;
  for (int i = 0; i < 6; ++i) {
    const WCHAR *t = names[i];
    ULONG k = 0;
    for (; t[k] && k < name->Length / 2; ++k) {
      WCHAR a = name->Buffer[k], b = t[k];
      if (a >= 'a' && a <= 'z') a = (WCHAR)(a - 32);
      if (b >= 'a' && b <= 'z') b = (WCHAR)(b - 32);
      if (a != b) break;
    }
    if (!t[k] && k == name->Length / 2) { *kind = i < 3 ? 1 : 2; return TRUE; }
  }
  return FALSE;
}
extern NTSTATUS NTAPI RawNtCreateFile(PHANDLE, ULONG, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
EXPORT NTSTATUS NTAPI NtCreateFile(PHANDLE handle, ULONG access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK iosb, PLARGE_INTEGER size, ULONG file_attributes, ULONG share, ULONG disposition,
                                   ULONG options, PVOID ea, ULONG ea_length) {
  LONG kind;
  if (attributes && attributes->Length >= sizeof(OBJECT_ATTRIBUTES) && !attributes->RootDirectory && handle && iosb && console_device_name(attributes->ObjectName, &kind)) {
    const ULONG_PTR console = (ULONG_PTR)NtCurrentPeb()->ProcessParameters->ConsoleHandle;
    if (!console) return STATUS_INVALID_HANDLE;      /* (there is nothing to open without a console) */
    static const WCHAR hex[] = L"0123456789abcdef";
    WCHAR text[96] = L"\\BaseNamedObjects\\TcosConX_";
    int n = 0;
    while (text[n]) ++n;
    text[n - 2] = kind == 1 ? 'I' : 'O';
    for (int shift = 60; shift >= 0; shift -= 4) text[n++] = hex[(console >> shift) & 15];
    text[n++] = '_';
    const ULONG_PTR pid = TEB_CLIENT_PROCESS(NtCurrentTeb()), serial = (ULONG_PTR)__sync_add_and_fetch(&g_con_event_serial, 1);
    for (int shift = 28; shift >= 0; shift -= 4) text[n++] = hex[(pid >> shift) & 15];
    text[n++] = '_';
    for (int shift = 12; shift >= 0; shift -= 4) text[n++] = hex[(serial >> shift) & 15];
    text[n] = 0;
    UNICODE_STRING name = { (USHORT)(n * 2), (USHORT)(n * 2 + 2), text };
    OBJECT_ATTRIBUTES oa = { sizeof(oa), NULL, &name, (attributes->Attributes & 2) | 0x40, NULL, NULL };
    HANDLE event = NULL;
    const NTSTATUS made = NtCreateEvent(&event, 0x1F0003, &oa, 0 /* NotificationEvent */, FALSE);
    if (!NT_SUCCESS(made)) return made;
    RtlTcosConsoleEventNote(event, console, kind);
    *handle = event;
    iosb->Status = STATUS_SUCCESS;
    iosb->Information = 1;      /* FILE_OPENED */
    return STATUS_SUCCESS;
  }
  return RawNtCreateFile(handle, access, attributes, iosb, size, file_attributes, share, disposition, options, ea, ea_length);
}
EXPORT NTSTATUS NTAPI NtWaitForMultipleObjects(ULONG count, const HANDLE *handles, ULONG type, BOOLEAN alertable, PLARGE_INTEGER timeout) {
  const NTSTATUS status = RawNtWaitForMultipleObjects(count, handles, type, alertable, timeout);
  if (status == STATUS_USER_APC && !run_apcs()) return STATUS_SUCCESS;      /* (only routine-less ones) */
  return status;
}
EXPORT NTSTATUS NTAPI NtDelayExecution(BOOLEAN alertable, PLARGE_INTEGER interval) {
  const NTSTATUS status = RawNtDelayExecution(alertable, interval);
  if (status == STATUS_USER_APC && !run_apcs()) return STATUS_SUCCESS;      /* (only routine-less ones) */
  return status;
}
EXPORT NTSTATUS NTAPI NtSignalAndWaitForSingleObject(HANDLE signal, HANDLE wait, BOOLEAN alertable, PLARGE_INTEGER timeout) {
  const NTSTATUS status = RawNtSignalAndWaitForSingleObject(signal, wait, alertable, timeout);
  if (status == STATUS_USER_APC && !run_apcs()) return STATUS_SUCCESS;      /* (only routine-less ones) */
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

static NTSTATUS utf8_to_unicode_old(PWSTR dest, ULONG dest_bytes, PULONG written, const char *src, ULONG src_bytes) {
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
EXPORT NTSTATUS NTAPI RtlMultiByteToUnicodeN(PWSTR d, ULONG db, PULONG w, const char *s, ULONG sb) { return utf8_to_unicode_old(d, db, w, s, sb); }
EXPORT NTSTATUS NTAPI RtlMultiByteToUnicodeSize(PULONG size, const char *s, ULONG sb) { return utf8_to_unicode_old(NULL, 0, size, s, sb); }

static NTSTATUS unicode_to_utf8_old(char *dest, ULONG dest_bytes, PULONG written, PCWSTR src, ULONG src_bytes) {
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

/* The next code point of UTF-8 text for RtlUTF8ToUnicodeN: *used bytes make up what is returned, and an invalid sequence is U+FFFD with *bad set. A lead byte
   that can start no sequence is one U+FFFD; a lead followed by a continuation byte its range does not allow is one U+FFFD for the two; a valid beginning that
   stops early is one U+FFFD for all of it. */
static ULONG utf8_decode(const BYTE *s, ULONG n, ULONG *used, int *bad) {
  const BYTE b = s[0];
  if (b < 0x80) { *used = 1; return b; }
  ULONG extra = 0, low = 0x80, high = 0xBF, cp = 0;
  if (b >= 0xC2 && b <= 0xDF) { extra = 1; cp = b & 0x1F; }
  else if (b >= 0xE0 && b <= 0xEF) { extra = 2; cp = b & 0x0F; if (b == 0xE0) low = 0xA0; if (b == 0xED) high = 0x9F; }
  else if (b >= 0xF0 && b <= 0xF4) { extra = 3; cp = b & 0x07; if (b == 0xF0) low = 0x90; if (b == 0xF4) high = 0x8F; }
  else { *used = 1; *bad = 1; return 0xFFFD; }
  ULONG k = 1;
  for (; k <= extra; ++k) {
    if (k >= n) break;      /* (the text ends inside the sequence) */
    const BYTE c = s[k];
    if (c < (k == 1 ? low : 0x80) || c > (k == 1 ? high : 0xBF)) {
      if ((c & 0xC0) == 0x80 && k == 1) ++k;      /* (a continuation byte that the lead does not take goes with it) */
      break;
    }
    cp = (cp << 6) | (c & 0x3F);
  }
  if (k <= extra) { *used = k; *bad = 1; return 0xFFFD; }
  *used = extra + 1;
  return cp;
}
EXPORT NTSTATUS NTAPI RtlUTF8ToUnicodeN(PWSTR dest, ULONG dest_bytes, PULONG written, const char *src, ULONG src_bytes) {
  if (!src) return 0xC00000F2;      /* STATUS_INVALID_PARAMETER_4 */
  if (!written) return 0xC000000D;
  ULONG units = 0;
  int bad = 0, cut = 0;
  for (ULONG i = 0; i < src_bytes;) {
    ULONG used;
    const ULONG cp = utf8_decode((const BYTE *)src + i, src_bytes - i, &used, &bad);
    i += used;
    ULONG pair[2];
    ULONG count = 1;
    if (cp >= 0x10000) { pair[0] = 0xD800 + ((cp - 0x10000) >> 10); pair[1] = 0xDC00 + ((cp - 0x10000) & 0x3FF); count = 2; }
    else pair[0] = cp;
    for (ULONG k = 0; k < count; ++k) {
      if (dest) {
        if ((units + 1) * 2 > dest_bytes) { cut = 1; break; }
        dest[units] = (WCHAR)pair[k];
      }
      ++units;
    }
    if (cut) break;
  }
  *written = units * 2;
  if (cut) return 0xC0000023u;
  return bad ? 0x00000107u : STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlUnicodeToUTF8N(char *dest, ULONG dest_bytes, PULONG written, PCWSTR src, ULONG src_bytes) {
  if (!src) return 0xC00000F2;
  if (!written) return 0xC000000D;
  if (dest && (src_bytes & 1)) return 0xC00000F3;      /* STATUS_INVALID_PARAMETER_5 (only when there is something to write to) */
  ULONG out = 0;
  int bad = 0, cut = 0;
  const ULONG count = src_bytes / 2;
  for (ULONG i = 0; i < count && !cut; ++i) {
    ULONG cp = src[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count && src[i + 1] >= 0xDC00 && src[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (src[i + 1] - 0xDC00); ++i; }
    else if (cp >= 0xD800 && cp < 0xE000) { cp = 0xFFFD; bad = 1; }
    BYTE encoded[4];
    ULONG n;
    if (cp < 0x80) { encoded[0] = (BYTE)cp; n = 1; }
    else if (cp < 0x800) { encoded[0] = (BYTE)(0xC0 | (cp >> 6)); encoded[1] = (BYTE)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { encoded[0] = (BYTE)(0xE0 | (cp >> 12)); encoded[1] = (BYTE)(0x80 | ((cp >> 6) & 0x3F)); encoded[2] = (BYTE)(0x80 | (cp & 0x3F)); n = 3; }
    else { encoded[0] = (BYTE)(0xF0 | (cp >> 18)); encoded[1] = (BYTE)(0x80 | ((cp >> 12) & 0x3F)); encoded[2] = (BYTE)(0x80 | ((cp >> 6) & 0x3F)); encoded[3] = (BYTE)(0x80 | (cp & 0x3F)); n = 4; }
    if (dest) {
      if (n > dest_bytes - out) { cut = 1; break; }
      for (ULONG k = 0; k < n; ++k) dest[out + k] = (char)encoded[k];
    }
    out += n;
  }
  *written = out;
  if (cut) return 0xC0000023u;
  return bad ? 0x00000107u : STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlUnicodeToMultiByteN(char *d, ULONG db, PULONG w, PCWSTR s, ULONG sb) { return unicode_to_utf8_old(d, db, w, s, sb); }
EXPORT NTSTATUS NTAPI RtlUnicodeToMultiByteSize(PULONG size, PCWSTR s, ULONG sb) { return unicode_to_utf8_old(NULL, 0, size, s, sb); }

/* ---- the heap ------------------------------------------------------------------------------------------------------------------------------------------ */

#include "heap.inc"

IMPORT NTSTATUS NTAPI NtOpenKey(PHANDLE, ULONG, POBJECT_ATTRIBUTES);
IMPORT NTSTATUS NTAPI NtQueryValueKey(HANDLE, PUNICODE_STRING, ULONG, PVOID, ULONG, PULONG);
/* The GlobalFlag value (a DWORD, or a hexadecimal text) of a key; FALSE if there is none. */
static int key_global_flag(const WCHAR *path, ULONG path_chars, ULONG *out) {
  UNICODE_STRING name = {(USHORT)(path_chars * 2), (USHORT)(path_chars * 2 + 2), (PWSTR)path};
  OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, &name, 0x40, NULL, NULL};
  HANDLE key = NULL;
  if (!NT_SUCCESS(NtOpenKey(&key, 0x20019, &attributes))) return 0;
  static const WCHAR value_name[] = {'G', 'l', 'o', 'b', 'a', 'l', 'F', 'l', 'a', 'g', 0};
  UNICODE_STRING value = {20, 22, (PWSTR)value_name};
  BYTE buffer[12 + 64];
  ULONG needed = 0;
  const NTSTATUS status = NtQueryValueKey(key, &value, 2, buffer, sizeof(buffer), &needed);
  NtClose(key);
  if (!NT_SUCCESS(status)) return 0;
  const ULONG type = *(ULONG *)(buffer + 4), length = *(ULONG *)(buffer + 8);
  const BYTE *data = buffer + 12;
  if (type == 4 && length >= 4) { *out = *(const ULONG *)data; return 1; }
  if (type == 1) {      /* REG_SZ: hexadecimal, with or without 0x */
    ULONG v = 0;
    const WCHAR *w = (const WCHAR *)data;
    ULONG n = length / 2, i = 0;
    if (n > 2 && w[0] == '0' && (w[1] == 'x' || w[1] == 'X')) i = 2;
    for (; i < n && w[i]; ++i) { const WCHAR c = w[i]; v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : 0); }
    *out = v;
    return 1;
  }
  return 0;
}
/* The global flags of the process: the image's own entry of the image file execution options, or the system's (Session Manager). */
static void read_global_flags(void) {
  PEB *peb = NtCurrentPeb();
  static const WCHAR session[] = L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager";
  static const WCHAR options[] = L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";
  ULONG flags = 0;
  WCHAR path[260];
  ULONG n = 0;
  if (peb->ProcessParameters && peb->ProcessParameters->ImagePathName.Buffer) {
    const WCHAR *image = peb->ProcessParameters->ImagePathName.Buffer;
    ULONG length = peb->ProcessParameters->ImagePathName.Length / 2, base = 0;
    for (ULONG i = 0; i < length; ++i) if (image[i] == '\\' || image[i] == '/') base = i + 1;
    for (; n < sizeof(options) / 2 - 1; ++n) path[n] = ((const WCHAR *)options)[n];
    for (ULONG i = base; i < length && n < 255; ++i) path[n++] = image[i];
    if (!key_global_flag((const WCHAR *)path, n, &flags)) key_global_flag((const WCHAR *)session, sizeof(session) / 2 - 1, &flags);
  }
  *(ULONG *)((BYTE *)peb + 0xBC) = flags;
}
static PVOID process_heap(void) {
  PEB *peb = NtCurrentPeb();
  if (!peb->ProcessHeap) {
    static int flags_read;
    if (!flags_read) { flags_read = 1; read_global_flags(); } while (__sync_lock_test_and_set(&g_heap_lock, 1)) NtYieldExecution(); if (!peb->ProcessHeap) peb->ProcessHeap = heap_new(0, 0, 0, 1); __sync_lock_release(&g_heap_lock); }
  return peb->ProcessHeap;
}
EXPORT PVOID NTAPI RtlProcessHeap(void) { return process_heap(); }

#include "fls.inc"

#include "rtl.inc"
#include "libc.inc"
#include "rtl2.inc"
#include "rtl3.inc"
#include "sec.inc"
#include "uilang.inc"
#include "normalize.inc"
#include "reg.inc"

/* ---- critical sections (one thread: they only count) ---------------------------------------------------------------------------------------------------- */

/* DebugInfo: -1 for a section without any (what Windows 8 and later give by default); a section made with RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO gets a record. */
#define CS_FLAG_NO_DEBUG_INFO 0x01000000u
#define CS_FLAG_DYNAMIC_SPIN 0x02000000u
#define CS_FLAG_STATIC_INIT 0x04000000u
#define CS_FLAG_RESOURCE_TYPE 0x08000000u
#define CS_FLAG_FORCE_DEBUG_INFO 0x10000000u
EXPORT NTSTATUS NTAPI RtlInitializeCriticalSectionEx(PRTL_CRITICAL_SECTION cs, ULONG spin, ULONG flags) {
  if (flags & ~(CS_FLAG_NO_DEBUG_INFO | CS_FLAG_DYNAMIC_SPIN | CS_FLAG_STATIC_INIT | CS_FLAG_RESOURCE_TYPE | CS_FLAG_FORCE_DEBUG_INFO | 0x00FFFFFFu)) return STATUS_INVALID_PARAMETER;
  cs->DebugInfo = (PVOID)(ULONG_PTR)-1;
  if ((flags & CS_FLAG_FORCE_DEBUG_INFO) && !(flags & CS_FLAG_NO_DEBUG_INFO)) {
    BYTE *info = RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, 0x30);
    if (info) { memcpy(info + 8, &cs, 8); cs->DebugInfo = (PVOID)info; }
  }
  cs->LockCount = -1;
  cs->RecursionCount = 0;
  cs->OwningThread = NULL;
  cs->LockSemaphore = NULL;
  cs->SpinCount = spin & 0x00FFFFFFu;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs) { return RtlInitializeCriticalSectionEx(cs, 0, 0); }
EXPORT NTSTATUS NTAPI RtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION cs, ULONG spin) { return RtlInitializeCriticalSectionEx(cs, spin, 0); }
/* LockCount is -1 while the section is free and -2 while a thread holds it (the recursion depth is the RecursionCount; a thread waiting adds 4 to its distance from -1). A thread that finds it taken gives its time slice away until the owner
 * has let go. */
EXPORT NTSTATUS NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
  const HANDLE me = (HANDLE)TEB_CLIENT_THREAD(NtCurrentTeb());
  if (cs->OwningThread == me && cs->RecursionCount > 0) {      /* (only the owner can see itself here) */
    ++cs->RecursionCount;
    return STATUS_SUCCESS;
  }
  while (!__sync_bool_compare_and_swap(&cs->LockCount, -1, -2)) {
    if (g_process_ending) { g_cs_taken_over = 1; cs->LockCount = -2; break; }      /* (the other threads are gone: a section one of them held is taken over rather than waited for) */
    if (cs->LockSemaphore) {      /* a section somebody gave a semaphore: the wait is on it, and a handle that is not one is an error that cannot be carried on from */
      LARGE_INTEGER none = { 0 };
      __sync_sub_and_fetch(&cs->LockCount, 4);
      if (NtWaitForSingleObject(cs->LockSemaphore, FALSE, &none) == STATUS_INVALID_HANDLE) {
        EXCEPTION_RECORD record;
        memset(&record, 0, sizeof(record));
        record.ExceptionCode = STATUS_INVALID_HANDLE;
        record.ExceptionFlags = 1 /* EXCEPTION_NONCONTINUABLE */;
        RtlRaiseException(&record);
      }
      __sync_add_and_fetch(&cs->LockCount, 4);
    }
    NtYieldExecution();
  }
  cs->OwningThread = me;
  cs->RecursionCount = 1;
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
  const HANDLE me = (HANDLE)TEB_CLIENT_THREAD(NtCurrentTeb());
  if (cs->OwningThread == me && cs->RecursionCount > 0) { ++cs->RecursionCount; return TRUE; }
  if (!__sync_bool_compare_and_swap(&cs->LockCount, -1, -2)) return FALSE;
  cs->OwningThread = me;
  cs->RecursionCount = 1;
  return TRUE;
}
EXPORT NTSTATUS NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs) {
  if (cs->RecursionCount <= 0) { --cs->RecursionCount; return STATUS_SUCCESS; }      /* (a section that is not held: the count goes below zero, the lock stays as it is) */
  if (--cs->RecursionCount > 0) return STATUS_SUCCESS;
  cs->OwningThread = NULL;
  __sync_lock_test_and_set(&cs->LockCount, -1);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs) {
  if (cs->DebugInfo && cs->DebugInfo != (PVOID)(ULONG_PTR)-1) RtlFreeHeap(process_heap(), 0, cs->DebugInfo);
  cs->DebugInfo = NULL;
  cs->LockCount = -1;
  cs->RecursionCount = 0;
  cs->OwningThread = NULL;
  return STATUS_SUCCESS;
}

/* ---- exceptions: unwinding, dispatching, __C_specific_handler ----------------------------------------------------------------------------------------------- */

#include "except.inc"
#include "actctx.inc"
/* SSE and SSE2 are there (PF_XMMI_INSTRUCTIONS_AVAILABLE = 6, PF_XMMI64_INSTRUCTIONS_AVAILABLE = 10); nothing newer is reported. */
EXPORT BOOLEAN NTAPI RtlIsProcessorFeaturePresent(ULONG feature) { return feature == 6 || feature == 10; }

/* ---- process start-up ------------------------------------------------------------------------------------------------------------------------------- */

typedef BOOL (WINAPI *DllEntry)(HANDLE, DWORD, PVOID);
typedef int (WINAPI *ProgramEntry)(PVOID);

/* The program's first breakpoint, where a debugger that started it takes over; it is not DbgBreakPoint, as on Windows. */
static __attribute__((noinline)) void tcos_initial_break(void) { *((BYTE *)NtCurrentTeb() + 0x1668) = 2; __asm__ volatile("int3" ::: "memory"); *((BYTE *)NtCurrentTeb() + 0x1668) = 0; }

/* The thread of a program starts here: the kernel puts the program's entry point in the first argument and the PEB in the second. The libraries' entry points run first
 * (in the order the kernel listed them: a library after the libraries it imports), then the program's. Its return value ends the process. */
EXPORT void NTAPI RtlUserThreadStart(ProgramEntry entry, PVOID parameter) {
  PEB *peb = NtCurrentPeb();
  PEB_LDR_DATA *ldr = peb->Ldr;
  process_heap();
  relocate_process_parameters();
  reset_floating_point();
  fls_thread_begins();
  act_init_process();
  if (peb->BeingDebugged) tcos_initial_break();      /* (a debugger that started the program gets the first word) */
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
static LDR_DATA_TABLE_ENTRY *ldr_add(const LoadRecord *record, const UNICODE_STRING *path, int mapped_only, ULONG extra_flags) {
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
  e->Flags = 0x4 | (mapped_only ? LDRP_MAPPED_ONLY : 0) | extra_flags;      /* LDRP_IMAGE_DLL */
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
  const int redirected = flags && (*flags & 0x40000000);      /* (ours) a library of an activation context: its own copy, whatever else of that name is loaded */
  LoadRecord records[MAX_LOAD_RECORDS];
  ULONG count = 0;
  PVOID base = NULL;
  loader_lock();
  NTSTATUS status = NtLoadModule(name, &base, records, MAX_LOAD_RECORDS, &count, (mapped_only ? 1 : 0) | (redirected ? 4 : 0));
  if (!NT_SUCCESS(status)) { loader_unlock(); return status; }
  if (count == 0) {      /* it was there: one more use (or a library the process started with) */
    *handle = base;
    loader_unlock();
    return STATUS_SUCCESS;
  }
  LDR_DATA_TABLE_ENTRY *added[MAX_LOAD_RECORDS];
  ULONG listed = 0;
  for (; listed < count; ++listed) {
    added[listed] = ldr_add(&records[listed], records[listed].base == (ULONG64)(ULONG_PTR)base ? name : NULL, mapped_only, records[listed].base == (ULONG64)(ULONG_PTR)base && redirected ? 0x10000000u : 0);
    if (!added[listed]) break;
  }
  ULONG attached = 0;
  if (listed == count) {
    for (; attached < count; ++attached) {
      notify_dll_event(1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */, added[attached]);      /* (before the entry point, which the callback may replace) */
      if (!notify_module(added[attached], DLL_PROCESS_ATTACH, NULL)) break;
    }
  }
  if (listed == count && attached == count) {
    *handle = base;
    loader_unlock();
    return STATUS_SUCCESS;
  }
  /* it did not start: the libraries that did are told, then everything goes again */
  for (ULONG i = attached < listed ? attached + 1 : attached; i > 0; --i) notify_module(added[i - 1], DLL_PROCESS_DETACH, NULL);      /* (the one that refused is told as well) */
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
/* ---- the DLL search path ---------------------------------------------------------------------------------------------------------------------------------- */

static const WCHAR kSystemDir[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', '3', '2', 0};
static const WCHAR kSystemPath[] = {'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', '3', '2', ';', 'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', '\\', 'S', 'y', 's', 't', 'e', 'm', ';',
                                    'C', ':', '\\', 'W', 'i', 'n', 'd', 'o', 'w', 's', 0};
static LONG g_path_safe_mode;      /* RtlSetSearchPathMode: 0 the current folder comes first, 1 it comes after the system folders, 2 that for good */
static ULONG g_default_search_flags;      /* LdrSetDefaultDllDirectories */

static SIZE_T text_length(const WCHAR *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }
/* The end of the folder part of a module path: "C:\dir\file" gives "C:\dir", "C:\file" gives "C:\", "C:file" gives "C:". */
static SIZE_T module_path_end(const WCHAR *module) {
  SIZE_T end = 0;
  for (SIZE_T i = 0; module[i]; ++i) if (module[i] == '\\' || module[i] == '/') end = i;
  if (end == 2 && module[1] == ':') ++end;
  if (end == 0 && module[0] && module[1] == ':') end = 2;
  return end;
}
static WCHAR *append_path(WCHAR *p, const WCHAR *text, SIZE_T length) {
  if (!length) return p;
  for (SIZE_T i = 0; i < length; ++i) p[i] = text[i];
  p[length] = ';';
  return p + length + 1;
}
static const WCHAR *image_path_name(void) {
  const BYTE *params = *(const BYTE *const *)(*(const BYTE *const *)((const BYTE *)NtCurrentTeb() + 0x60) + 0x20);
  return ((const UNICODE_STRING *)(params + 0x60))->Buffer;
}
/* module folder; dll_dir or the current folder; the system folders; PATH (with the current folder before it in safe mode). The result is from the process heap. */
static NTSTATUS get_dll_load_path(const WCHAR *module, const WCHAR *dll_dir, LONG safe_mode, PWSTR *path) {
  static const WCHAR kPath[] = {'P', 'A', 'T', 'H', 0};
  UNICODE_STRING name = { 8, 10, (PWSTR)kPath }, value = { 0, 0, NULL };
  SIZE_T module_chars = module ? module_path_end(module) : 0;
  SIZE_T path_bytes = 0;
  if (RtlQueryEnvironmentVariable_U(NULL, &name, &value) == STATUS_BUFFER_TOO_SMALL_) path_bytes = value.Length;
  const SIZE_T total = (text_length(kSystemPath) + 1) + module_chars + 1 + (dll_dir ? text_length(dll_dir) + 1 : 2) + 2 + path_bytes / 2 + 2;
  WCHAR *result = RtlAllocateHeap(process_heap(), 0, total * 2), *p = result;
  if (!result) return STATUS_NO_MEMORY;
  if (module) p = append_path(p, module, module_chars);
  if (dll_dir) p = append_path(p, dll_dir, text_length(dll_dir));
  else if (!safe_mode) p = append_path(p, (const WCHAR *)L".", 1);
  p = append_path(p, kSystemPath, text_length(kSystemPath));
  if (!dll_dir && safe_mode) p = append_path(p, (const WCHAR *)L".", 1);
  value.Buffer = p;
  value.MaximumLength = (USHORT)(path_bytes > 0xFFFC ? 0xFFFC : path_bytes);
  value.Length = 0;
  if (path_bytes && RtlQueryEnvironmentVariable_U(NULL, &name, &value) == STATUS_SUCCESS) p[value.Length / 2] = 0;
  else *p = 0;
  *path = result;
  return STATUS_SUCCESS;
}
/* The folders a set of LOAD_LIBRARY_SEARCH_* flags names, separated by ';': the folder of `module` (0x100), of the program (0x200), the user folders (0x400, AddDllDirectory in
 * the order of the newest first, then SetDllDirectory's), System32 (0x800). */
static NTSTATUS search_flags_path(const WCHAR *module, ULONG flags, WCHAR *out, SIZE_T capacity) {
  SIZE_T n = 0;
  out[0] = 0;
  if (flags & 0x1000) flags |= 0xE00;      /* LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: application, user and System32 folders */
  if (flags & 0x100) {
    if (!module) return STATUS_INVALID_PARAMETER;
    const ULONG type = RtlDetermineDosPathNameType_U(module);
    if (type != 2 && type != 4 && type != 6 && type != 1) return STATUS_INVALID_PARAMETER;      /* drive absolute, rooted, local device, UNC */
    const SIZE_T end = module_path_end(module);
    if (end + 2 > capacity) return STATUS_NAME_TOO_LONG_;
    for (SIZE_T i = 0; i < end; ++i) out[n++] = module[i];
    if (end) out[n++] = ';';
  }
  if (flags & 0x200) {
    const WCHAR *image = image_path_name();
    const SIZE_T end = module_path_end(image);
    if (n + end + 2 > capacity) return STATUS_NAME_TOO_LONG_;
    for (SIZE_T i = 0; i < end; ++i) out[n++] = image[i];
    if (end) out[n++] = ';';
  }
  if (flags & 0x400) {
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
  if (flags & 0x800) {
    for (SIZE_T i = 0; kSystemDir[i] && n + 2 < capacity; ++i) out[n++] = kSystemDir[i];
    out[n++] = ';';
  }
  if (n > 0) --n;
  out[n] = 0;
  return STATUS_SUCCESS;
}
EXPORT ULONG NTAPI RtlGetFullPathName_U(PCWSTR name, ULONG size, PWSTR buffer, PWSTR *file_part);
EXPORT NTSTATUS NTAPI LdrSetDefaultDllDirectories(ULONG flags) {
  if (!flags || (flags & ~0x1F00u) || (flags & 0x100)) return STATUS_INVALID_PARAMETER;
  g_default_search_flags = flags;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI LdrGetDllPath(PCWSTR module, ULONG flags, PWSTR *path, PWSTR *unused) {
  const ULONG search_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x1000;
  if (unused) *unused = NULL;
  if (flags & 8) {      /* LOAD_WITH_ALTERED_SEARCH_PATH */
    if (flags & search_flags) return STATUS_INVALID_PARAMETER;
    if (g_default_search_flags) flags |= g_default_search_flags | 0x100;
  } else if (!(flags & search_flags)) flags |= g_default_search_flags;
  if (flags & search_flags) {
    WCHAR text[1040];
    const NTSTATUS status = search_flags_path(module, flags, text, 1040);
    if (!NT_SUCCESS(status)) return status;
    const SIZE_T n = text_length(text);
    PWSTR copy = RtlAllocateHeap(process_heap(), 0, (n + 1) * 2);
    if (!copy) return STATUS_NO_MEMORY;
    for (SIZE_T i = 0; i <= n; ++i) copy[i] = text[i];
    *path = copy;
    return STATUS_SUCCESS;
  }
  WCHAR dll_directory[520];
  const WCHAR *dll_dir = NULL;
  if (g_dll_directory_chars) {
    for (USHORT i = 0; i <= g_dll_directory_chars; ++i) dll_directory[i] = g_dll_directory_text[i];
    dll_dir = dll_directory;
  }
  const WCHAR *from = module;
  BOOLEAN has_backslash = FALSE;
  if (module) for (const WCHAR *c = module; *c; ++c) if (*c == '\\') has_backslash = TRUE;
  if (!(flags & 8) || !has_backslash) from = image_path_name();
  return get_dll_load_path(from, dll_dir, 1, path);
}
EXPORT NTSTATUS NTAPI RtlSetSearchPathMode(ULONG flags) {
  LONG value;
  switch (flags) {
    case 1: value = 1; break;      /* BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE */
    case 0x10000: value = 0; break;      /* BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE */
    case 1 | 0x8000: __sync_lock_test_and_set(&g_path_safe_mode, 2); return STATUS_SUCCESS;      /* ... and for good */
    default: return STATUS_INVALID_PARAMETER;
  }
  for (;;) {
    const LONG previous = g_path_safe_mode;
    if (previous == 2) break;
    if (__sync_val_compare_and_swap(&g_path_safe_mode, previous, value) == previous) return STATUS_SUCCESS;
  }
  return STATUS_ACCESS_DENIED;
}
/* The folders an executable is looked for in: the program's, the current folder unless NoDefaultCurrentDirectoryInExePath is set (and the name has no path), the system ones, PATH. */
EXPORT NTSTATUS NTAPI RtlGetExePath(PCWSTR name, PWSTR *path) {
  static const WCHAR kNoDefault[] = {'N', 'o', 'D', 'e', 'f', 'a', 'u', 'l', 't', 'C', 'u', 'r', 'r', 'e', 'n', 't', 'D', 'i', 'r', 'e', 'c', 't', 'o', 'r', 'y', 'I', 'n', 'E', 'x', 'e', 'P', 'a', 't', 'h', 0};
  const WCHAR *dll_dir = (const WCHAR *)L".";
  BOOLEAN has_backslash = FALSE;
  for (const WCHAR *c = name; *c; ++c) if (*c == '\\') has_backslash = TRUE;
  if (!has_backslash) {
    UNICODE_STRING variable = { (USHORT)(text_length(kNoDefault) * 2), (USHORT)(text_length(kNoDefault) * 2 + 2), (PWSTR)kNoDefault }, value = { 0, 0, NULL };
    if (RtlQueryEnvironmentVariable_U(NULL, &variable, &value) != STATUS_VARIABLE_NOT_FOUND_) dll_dir = (const WCHAR *)L"";
  }
  return get_dll_load_path(image_path_name(), dll_dir, 0, path);
}
EXPORT NTSTATUS NTAPI RtlGetSearchPath(PWSTR *path) { return get_dll_load_path(image_path_name(), NULL, g_path_safe_mode, path); }
EXPORT VOID NTAPI RtlReleasePath(PWSTR path) { if (path) RtlFreeHeap(process_heap(), 0, path); }
EXPORT NTSTATUS NTAPI LdrLoadDll(PWSTR search_path, PULONG flags, PUNICODE_STRING name, PVOID *handle) {
  WCHAR coded[1040];
  BOOLEAN absolute = name->Length >= 6 && name->Buffer[1] == ':' && (name->Buffer[2] == '\\' || name->Buffer[2] == '/');
  if (name->Length >= 2 && name->Buffer[0] == '\\') absolute = TRUE;      /* (\\server\share, or rooted in the current drive) */
  if ((ULONG_PTR)search_path <= 0xFFFF && ((ULONG_PTR)search_path & 1)) {      /* LOAD_LIBRARY_SEARCH_* flags in place of a path */
    const ULONG search = (ULONG)(ULONG_PTR)search_path & ~1u;
    if (((search & 8) && (search & 0x1F00)) || ((search & 0x100) && !absolute)) return STATUS_INVALID_PARAMETER;
    if (!NT_SUCCESS(search_flags_path(NULL, search, coded, 1040))) return STATUS_INVALID_PARAMETER;
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
      if (NT_SUCCESS(status) || status == STATUS_DLL_INIT_FAILED || status == (NTSTATUS)0xC000007B || status == (NTSTATUS)0xC000012F || status == (NTSTATUS)0xC000011B || status == (NTSTATUS)0xC0000131 || status == (NTSTATUS)0xC0000130 || status == (NTSTATUS)0xC000035A) return status;      /* (a file that is no image of any kind, or a library that began to run, is the answer) */
      if (status != STATUS_DLL_NOT_FOUND && status != (NTSTATUS)0xC0000034 && status != (NTSTATUS)0xC000003A && !found) { found = TRUE; first_error = status; }
    }
    dir = *end ? end + 1 : end;
  }
  return first_error;
}

/* Files that were moved while the process runs (MoveFile tells us): a library is known by the path it was loaded from, and the same file under its new name is the same library. */
static WCHAR ldr_upper(WCHAR c);
#define MAX_RENAMES 16
static struct { WCHAR from[260], to[260]; } g_renames[MAX_RENAMES];
static ULONG g_rename_count;
EXPORT VOID NTAPI TcosNoteFileRename(PCWSTR from, PCWSTR to) {
  const ULONG slot = g_rename_count++ % MAX_RENAMES;
  SIZE_T i = 0;
  for (; from[i] && i < 259; i++) g_renames[slot].from[i] = from[i];
  g_renames[slot].from[i] = 0;
  for (i = 0; to[i] && i < 259; i++) g_renames[slot].to[i] = to[i];
  g_renames[slot].to[i] = 0;
}
static BOOLEAN wide_equal_nocase(const WCHAR *a, const WCHAR *b) {
  for (; *a && *b; a++, b++) if (ldr_upper(*a) != ldr_upper(*b)) return FALSE;
  return *a == *b;
}
/* Is `path` where the file of `module_path` is now, following the moves? */
static BOOLEAN moved_to(const WCHAR *module_path, const WCHAR *path) {
  WCHAR current[260];
  SIZE_T n = 0;
  for (; module_path[n] && n < 259; n++) current[n] = module_path[n];
  current[n] = 0;
  for (ULONG step = 0; step < MAX_RENAMES; step++) {
    BOOLEAN moved = FALSE;
    const ULONG first = g_rename_count > MAX_RENAMES ? g_rename_count - MAX_RENAMES : 0;
    for (ULONG r = first; r < g_rename_count; r++) {
      const ULONG slot = r % MAX_RENAMES;
      if (wide_equal_nocase(g_renames[slot].from, current)) {
        SIZE_T k = 0;
        for (; g_renames[slot].to[k]; k++) current[k] = g_renames[slot].to[k];
        current[k] = 0;
        moved = TRUE;
      }
    }
    if (!moved) break;
  }
  return wide_equal_nocase(current, path);
}
/* Hard links made while the process runs: two names of one file. */
static struct { WCHAR a[260], b[260]; } g_links[MAX_RENAMES];
static ULONG g_link_count;
EXPORT VOID NTAPI TcosNoteFileLink(PCWSTR existing, PCWSTR link) {
  const ULONG slot = g_link_count++ % MAX_RENAMES;
  SIZE_T i = 0;
  for (; existing[i] && i < 259; i++) g_links[slot].a[i] = existing[i];
  g_links[slot].a[i] = 0;
  for (i = 0; link[i] && i < 259; i++) g_links[slot].b[i] = link[i];
  g_links[slot].b[i] = 0;
}
/* Where the file that was at `path` is now (following MoveFile), in `out`. */
static void file_now(const WCHAR *path, WCHAR *out) {
  SIZE_T n = 0;
  for (; path[n] && n < 259; n++) out[n] = path[n];
  out[n] = 0;
  for (ULONG step = 0; step < MAX_RENAMES; step++) {
    BOOLEAN moved = FALSE;
    const ULONG first = g_rename_count > MAX_RENAMES ? g_rename_count - MAX_RENAMES : 0;
    for (ULONG r = first; r < g_rename_count; r++) {
      const ULONG slot = r % MAX_RENAMES;
      if (wide_equal_nocase(g_renames[slot].from, out)) {
        SIZE_T k = 0;
        for (; g_renames[slot].to[k]; k++) out[k] = g_renames[slot].to[k];
        out[k] = 0;
        moved = TRUE;
      }
    }
    if (!moved) break;
  }
}
/* Do the two paths name one file, counting the moves and hard links the process made? */
static BOOLEAN same_file(const WCHAR *x, const WCHAR *y) {
  WCHAR a[260], b[260];
  file_now(x, a);
  file_now(y, b);
  if (wide_equal_nocase(a, b)) return TRUE;
  const ULONG first = g_link_count > MAX_RENAMES ? g_link_count - MAX_RENAMES : 0;
  for (ULONG r = first; r < g_link_count; r++) {
    WCHAR la[260], lb[260];
    file_now(g_links[r % MAX_RENAMES].a, la);
    file_now(g_links[r % MAX_RENAMES].b, lb);
    if ((wide_equal_nocase(la, a) && wide_equal_nocase(lb, b)) || (wide_equal_nocase(la, b) && wide_equal_nocase(lb, a))) return TRUE;
  }
  return FALSE;
}
/* The library loaded from the file at `path` (a full path), whatever it was loaded as; NULL if there is none. */
EXPORT PVOID NTAPI TcosFindModuleByFile(PCWSTR path) {
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  PVOID found = NULL;
  loader_lock();
  for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList && !found; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *c = entry_of(e);
    WCHAR text[260];
    const SIZE_T length = c->FullDllName.Length / 2;
    if (length >= 260) continue;
    for (SIZE_T i = 0; i < length; i++) text[i] = c->FullDllName.Buffer[i];
    text[length] = 0;
    if (same_file(text, path)) found = c->DllBase;
  }
  loader_unlock();
  return found;
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
  LDR_DATA_TABLE_ENTRY *m = NULL;
  BOOLEAN has_folder = FALSE;
  for (USHORT i = 0; i < name->Length / 2; i++) if (name->Buffer[i] == '\\' || name->Buffer[i] == '/') has_folder = TRUE;
  if (has_folder) {      /* a name with a folder is a file: the library that was loaded from it, or from where it was before it moved */
    WCHAR text[260], full[260];
    const SIZE_T n = name->Length / 2;
    if (n < 260) {
      for (SIZE_T i = 0; i < n; i++) text[i] = name->Buffer[i];
      text[n] = 0;
      const ULONG got = RtlGetFullPathName_U(text, sizeof(full), full, NULL);
      if (got != 0 && got < sizeof(full)) {
        PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
        for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList && !m; e = e->Flink) {
          LDR_DATA_TABLE_ENTRY *c = entry_of(e);
          WCHAR path[260];
          const SIZE_T length = c->FullDllName.Length / 2;
          if (length >= 260) continue;
          for (SIZE_T i = 0; i < length; i++) path[i] = c->FullDllName.Buffer[i];
          path[length] = 0;
          if (same_file(path, full)) m = c;
        }
      }
    }
  }
  if (!m) m = ldr_find_name(name);
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

EXPORT NTSTATUS NTAPI LdrAddRefDll(ULONG flags, PVOID base) {
  if (flags & ~1u) return STATUS_INVALID_PARAMETER;      /* (only LDR_ADDREF_DLL_PIN) */
  loader_lock();
  LDR_DATA_TABLE_ENTRY *e = ldr_find_base(base);
  NTSTATUS status = STATUS_SUCCESS;
  if (!e) status = STATUS_DLL_NOT_FOUND;
  else if (flags & 1) e->LoadCount = LDR_STATIC;      /* pinned: never unloaded */
  else if (e->LoadCount != LDR_STATIC) ++e->LoadCount;
  loader_unlock();
  return status;
}
typedef void (NTAPI *LdrEnumProc)(LDR_DATA_TABLE_ENTRY *module, PVOID context, BOOLEAN *stop);
EXPORT NTSTATUS NTAPI LdrEnumerateLoadedModules(PVOID unknown, LdrEnumProc callback, PVOID context) {
  if (unknown || !callback) return STATUS_INVALID_PARAMETER;
  loader_lock();
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  BOOLEAN stop = FALSE;
  for (LIST_ENTRY *e = ldr->InMemoryOrderModuleList.Flink; e != &ldr->InMemoryOrderModuleList && !stop; e = e->Flink)
    callback((LDR_DATA_TABLE_ENTRY *)((BYTE *)e - sizeof(LIST_ENTRY)), context, &stop);      /* (InMemoryOrderLinks follow InLoadOrderLinks) */
  loader_unlock();
  return STATUS_SUCCESS;
}

/* ntdll's own entry point: nothing to set up (the heap is made when first used). */
EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) process_heap();
  return TRUE;
}
