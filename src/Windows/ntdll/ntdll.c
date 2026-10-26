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

EXPORT ULONG NTAPI RtlNtStatusToDosError(NTSTATUS status) {
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

static int is_program(const LDR_DATA_TABLE_ENTRY *m) { return NtCurrentPeb()->ImageBaseAddress == m->DllBase; }

static BOOL notify_module(LDR_DATA_TABLE_ENTRY *m, DWORD reason, PVOID reserved) {
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
  if (!is_program(m) && m->EntryPoint) return ((BOOL (WINAPI *)(PVOID, DWORD, PVOID))m->EntryPoint)(m->DllBase, reason, reserved);
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
EXPORT void NTAPI RtlExitUserProcess(NTSTATUS status) {
  if (__sync_bool_compare_and_swap(&g_process_ending, 0, 1)) notify_all_modules(DLL_PROCESS_DETACH, (PVOID)1, 0);      /* (1: the process is ending) */
  for (;;) NtTerminateProcess(NtCurrentProcess(), status);
}

/* The thread ends (the libraries and the program are told); the process ends with its last thread. */
EXPORT void NTAPI RtlExitUserThread(NTSTATUS status) {
  if (!g_process_ending) notify_all_modules(DLL_THREAD_DETACH, NULL, 0);
  for (;;) NtTerminateThread((HANDLE)(LONG_PTR)-2, status);
}

/* New threads begin here (the kernel passes the thread's function and parameter in the first two argument registers); the function's return value is the thread's exit code. */
EXPORT void NTAPI TcosThreadStart(LPTHREAD_START_ROUTINE start, PVOID parameter) {
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
  BYTE *start = (BYTE *)(((ULONG_PTR)base + sizeof(Segment) + 15 + 8) & ~15ull) - 8;      /* so that start + 16 is 16-byte aligned */
  BYTE *end = (BYTE *)(((ULONG_PTR)base + size) & ~15ull) - 24;      /* (so that end - start is a multiple of 16: the chunks are) */
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
  BYTE *start = (BYTE *)(((ULONG_PTR)segment + sizeof(Segment) + 15 + 8) & ~15ull) - 8;
  BYTE *end = (BYTE *)(((ULONG_PTR)base + size) & ~15ull) - 24;      /* (so that end - start is a multiple of 16: the chunks are) */
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
static volatile LONG g_heap_lock;
static void heap_lock(void) { while (__sync_lock_test_and_set(&g_heap_lock, 1)) NtYieldExecution(); }
static void heap_unlock(void) { __sync_lock_release(&g_heap_lock); }

static PVOID heap_alloc(PVOID heap, ULONG flags, SIZE_T bytes) {
  Heap *h = heap;
  if (!heap_ok(h)) return NULL;
  if (bytes > (1ull << 46)) return NULL;
  ULONGLONG need = (bytes + 8 + 15) & ~15ull;
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
      if (flags & HEAP_ZERO_MEMORY) memset(user, 0, (size_t_)(CHUNK_SIZE(c) - 8));
      return user;
    }
    if (attempt == 0 && !heap_add_segment(h, need)) return NULL;
  }
  return NULL;
}

static BOOLEAN heap_free(PVOID heap, ULONG flags, PVOID memory) {
  (void)flags;
  Heap *h = heap;
  if (!heap_ok(h)) return FALSE;
  if (!memory) return TRUE;
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
  if (!heap_ok(heap) || !memory) return (SIZE_T)-1;
  Chunk *c = (Chunk *)((BYTE *)memory - 16);
  return (c->size_flags & CHUNK_USED) ? (SIZE_T)(CHUNK_SIZE(c) - 8) : (SIZE_T)-1;
}

static PVOID heap_realloc(PVOID heap, ULONG flags, PVOID memory, SIZE_T bytes) {
  if (!memory) return heap_alloc(heap, flags, bytes);
  const SIZE_T have = heap_size(heap, flags, memory);
  if (have == (SIZE_T)-1) return NULL;
  if (bytes <= have) return memory;
  PVOID fresh = heap_alloc(heap, flags & ~HEAP_ZERO_MEMORY, bytes);
  if (!fresh) return NULL;
  memcpy(fresh, memory, have);
  if (flags & HEAP_ZERO_MEMORY) memset((BYTE *)fresh + have, 0, bytes - have);
  heap_free(heap, flags, memory);
  return fresh;
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

EXPORT BOOLEAN NTAPI RtlLockHeap(PVOID heap) { (void)heap; return TRUE; }
EXPORT BOOLEAN NTAPI RtlUnlockHeap(PVOID heap) { (void)heap; return TRUE; }
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
static Chunk *segment_first_chunk(Segment *s) { return (Chunk *)((BYTE *)(((ULONG_PTR)s + sizeof(Segment) + 15 + 8) & ~15ull) - 8); }
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
    e->cbData = (DWORD)(CHUNK_SIZE(c) - 8);
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
  while (!__sync_bool_compare_and_swap(&cs->LockCount, -1, 0)) NtYieldExecution();
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
static LDR_DATA_TABLE_ENTRY *ldr_add(const LoadRecord *record, const UNICODE_STRING *path) {
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
  e->Flags = 0x4;      /* LDRP_IMAGE_DLL */
  e->LoadCount = 1;
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  ldr_append(&ldr->InLoadOrderModuleList, &e->InLoadOrderLinks);
  ldr_append(&ldr->InMemoryOrderModuleList, &e->InMemoryOrderLinks);
  ldr_append(&ldr->InInitializationOrderModuleList, &e->InInitializationOrderLinks);
  return e;
}
static void ldr_forget(LDR_DATA_TABLE_ENTRY *e) {
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
EXPORT NTSTATUS NTAPI LdrLoadDll(PWSTR search_path, PULONG flags, PUNICODE_STRING name, PVOID *handle) {
  (void)search_path; (void)flags;
  LoadRecord records[MAX_LOAD_RECORDS];
  ULONG count = 0;
  PVOID base = NULL;
  loader_lock();
  NTSTATUS status = NtLoadModule(name, &base, records, MAX_LOAD_RECORDS, &count);
  if (!NT_SUCCESS(status)) { loader_unlock(); return status; }
  if (count == 0) {      /* it was there: one more use (or a library the process started with) */
    *handle = base;
    loader_unlock();
    return STATUS_SUCCESS;
  }
  LDR_DATA_TABLE_ENTRY *added[MAX_LOAD_RECORDS];
  ULONG listed = 0;
  for (; listed < count; ++listed) {
    added[listed] = ldr_add(&records[listed], records[listed].base == (ULONG64)(ULONG_PTR)base ? name : NULL);
    if (!added[listed]) break;
  }
  ULONG attached = 0;
  if (listed == count) {
    for (; attached < count; ++attached) if (!notify_module(added[attached], DLL_PROCESS_ATTACH, NULL)) break;
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

EXPORT NTSTATUS NTAPI LdrUnloadDll(PVOID base) {
  loader_lock();
  LDR_DATA_TABLE_ENTRY *e = ldr_find_base(base);
  NTSTATUS status = STATUS_SUCCESS;
  if (!e) status = STATUS_DLL_NOT_FOUND;
  else if (e->LoadCount != LDR_STATIC) ldr_release(base, 1);
  loader_unlock();
  return status;
}

/* ntdll's own entry point: nothing to set up (the heap is made when first used). */
EXPORT BOOL WINAPI DllMain(HANDLE module, DWORD reason, PVOID reserved) {
  (void)module; (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) process_heap();
  return TRUE;
}
