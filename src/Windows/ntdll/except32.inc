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

/* ntdll.dll for i386: exceptions. Included by ntdll.c in place of except.inc, which is the x64 mechanism (unwind tables). On i386 an exception is
 * dispatched to the handlers a function chains through FS:[0], and unwinding walks the same chain - which is not written yet (docs/Wow64.md): for now an
 * exception that software raises is offered to the vectored handlers and the unhandled-exception filter, and then ends the process with its code; a
 * fault of the processor ends the process in the kernel. What is here keeps the names the other libraries and programs import. */

EXPORT void NTAPI RtlRaiseException(EXCEPTION_RECORD *record);
EXPORT void NTAPI RtlExitUserThread(NTSTATUS);

#define MAX_VECTORED 32
static struct { LONG (WINAPI *handler)(PEXCEPTION_POINTERS); } g_vectored[MAX_VECTORED], g_continue_handlers[MAX_VECTORED];
static LPTOP_LEVEL_EXCEPTION_FILTER g_unhandled_filter;
static LONG (WINAPI *g_unhandled_hook)(PEXCEPTION_POINTERS);

static PVOID add_handler(void *table_arg, ULONG first, PVOID handler) {
  __typeof__(g_vectored[0]) *table = table_arg;
  if (!handler) return NULL;
  if (first) {
    if (table[MAX_VECTORED - 1].handler) return NULL;
    for (int i = MAX_VECTORED - 1; i > 0; --i) table[i] = table[i - 1];
    table[0].handler = handler;
    return handler;
  }
  for (int i = 0; i < MAX_VECTORED; ++i) if (!table[i].handler) { table[i].handler = handler; return handler; }
  return NULL;
}
static ULONG remove_handler(void *table_arg, PVOID cookie) {
  __typeof__(g_vectored[0]) *table = table_arg;
  for (int i = 0; i < MAX_VECTORED; ++i) {
    if ((PVOID)table[i].handler != cookie) continue;
    for (int k = i; k < MAX_VECTORED - 1; ++k) table[k] = table[k + 1];
    table[MAX_VECTORED - 1].handler = NULL;
    return 1;
  }
  return 0;
}
EXPORT PVOID NTAPI RtlAddVectoredExceptionHandler(ULONG first, PVOID handler) { return add_handler(g_vectored, first, handler); }
EXPORT ULONG NTAPI RtlRemoveVectoredExceptionHandler(PVOID cookie) { return remove_handler(g_vectored, cookie); }
EXPORT PVOID NTAPI RtlAddVectoredContinueHandler(ULONG first, PVOID handler) { return add_handler(g_continue_handlers, first, handler); }
EXPORT ULONG NTAPI RtlRemoveVectoredContinueHandler(PVOID cookie) { return remove_handler(g_continue_handlers, cookie); }
EXPORT void NTAPI RtlTcosSetUnhandledExceptionHook(LONG (WINAPI *hook)(PEXCEPTION_POINTERS)) { g_unhandled_hook = hook; }
EXPORT LPTOP_LEVEL_EXCEPTION_FILTER NTAPI RtlSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  const LPTOP_LEVEL_EXCEPTION_FILTER old = g_unhandled_filter;
  g_unhandled_filter = filter;
  return old;
}

/* The module an address is in. */
EXPORT PVOID NTAPI RtlPcToFileHeader(PVOID pc, PVOID *base) {
  PVOID found = NULL;
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  if (ldr) {
    for (LIST_ENTRY *e = ldr->InLoadOrderModuleList.Flink; e != &ldr->InLoadOrderModuleList; e = e->Flink) {
      const LDR_DATA_TABLE_ENTRY *m = (const LDR_DATA_TABLE_ENTRY *)e;
      if ((ULONG_PTR)pc >= (ULONG_PTR)m->DllBase && (ULONG_PTR)pc - (ULONG_PTR)m->DllBase < m->SizeOfImage) { found = m->DllBase; break; }
    }
  }
  if (base) *base = found;
  return found;
}

static BOOL debugger_handled(EXCEPTION_RECORD *record, CONTEXT *context, BOOLEAN first_chance) { (void)record; (void)context; (void)first_chance; return FALSE; }
/* (there is no 32-bit CONTEXT yet: the record that is given is empty) */
EXPORT void NTAPI RtlCaptureContext(CONTEXT *context) { memset(context, 0, sizeof(*context)); }

/* The vectored handlers, then the filter for what nobody handled. TRUE if one of them wants the program to go on. */
EXPORT BOOLEAN NTAPI RtlDispatchException(EXCEPTION_RECORD *record, CONTEXT *context) {
  EXCEPTION_POINTERS pointers = {record, context};
  for (int i = 0; i < MAX_VECTORED && g_vectored[i].handler; ++i)
    if (g_vectored[i].handler(&pointers) == EXCEPTION_CONTINUE_EXECUTION) return TRUE;
  if (g_unhandled_hook && g_unhandled_hook(&pointers) == EXCEPTION_CONTINUE_EXECUTION) return TRUE;
  if (g_unhandled_filter && g_unhandled_filter(&pointers) == EXCEPTION_CONTINUE_EXECUTION) return TRUE;
  return FALSE;
}

EXPORT void NTAPI RtlRaiseException(EXCEPTION_RECORD *record) {
  static CONTEXT context;      /* (large, and empty: see RtlCaptureContext) */
  record->ExceptionAddress = __builtin_return_address(0);
  if (RtlDispatchException(record, &context) && !(record->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) return;
  for (;;) NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)record->ExceptionCode);
}
EXPORT void NTAPI RtlRaiseStatus(NTSTATUS status) {
  EXCEPTION_RECORD record = {.ExceptionCode = (DWORD)status, .ExceptionFlags = EXCEPTION_NONCONTINUABLE};
  RtlRaiseException(&record);
}
EXPORT NTSTATUS NTAPI NtRaiseException(EXCEPTION_RECORD *record, CONTEXT *context, BOOLEAN first_chance) {
  (void)context; (void)first_chance;
  for (;;) NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)record->ExceptionCode);
}
EXPORT NTSTATUS NTAPI NtContinue(CONTEXT *context, BOOLEAN test_alert) { (void)context; (void)test_alert; return (NTSTATUS)0xC0000002u; }      /* STATUS_NOT_IMPLEMENTED */
EXPORT NTSTATUS NTAPI NtGetContextThread(HANDLE thread, CONTEXT *context) { (void)thread; (void)context; return (NTSTATUS)0xC0000002u; }
EXPORT NTSTATUS NTAPI NtSetContextThread(HANDLE thread, const CONTEXT *context) { (void)thread; (void)context; return (NTSTATUS)0xC0000002u; }

/* Unwinding to a frame: not written. A program that gets here was handling an exception, which cannot happen yet (see above). */
EXPORT void NTAPI RtlUnwind(PVOID target_frame, PVOID target_ip, EXCEPTION_RECORD *record, PVOID return_value) {
  (void)target_frame; (void)target_ip; (void)record; (void)return_value;
  for (;;) NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)0xC0000002u);
}

/* The return addresses of the callers, by the frame pointers (code built without them ends the walk early). */
EXPORT USHORT NTAPI RtlCaptureStackBackTrace(ULONG skip, ULONG count, PVOID *frames, PULONG hash) {
  const ULONG_PTR base = *(const ULONG_PTR *)TEB_BYTE(NtCurrentTeb(), 0x04), limit = *(const ULONG_PTR *)TEB_BYTE(NtCurrentTeb(), 0x08);
  const ULONG_PTR *frame = __builtin_frame_address(0);
  USHORT captured = 0;
  ULONG sum = 0;
  for (ULONG depth = 0; captured < count && depth < 512; ++depth) {
    if ((ULONG_PTR)frame < limit || (ULONG_PTR)frame > base - 8 || ((ULONG_PTR)frame & 3)) break;
    const ULONG_PTR address = frame[1];
    if (!address) break;
    if (depth >= skip) { frames[captured++] = (PVOID)address; sum += (ULONG)address; }
    const ULONG_PTR *next = (const ULONG_PTR *)frame[0];
    if (next <= frame) break;
    frame = next;
  }
  if (hash) *hash = sum;
  return captured;
}

#define TEB_DEBUG_OBJECT(teb) (*(HANDLE *)TEB_BYTE(TEB_RAW(teb), 0x1660))
EXPORT __attribute__((naked)) void NTAPI DbgBreakPoint(void) { __asm__ volatile("int3\n\tret"); }
EXPORT HANDLE NTAPI DbgUiGetThreadDebugObject(void) { return TEB_DEBUG_OBJECT(NtCurrentTeb()); }
EXPORT void NTAPI DbgUiSetThreadDebugObject(HANDLE object) { TEB_DEBUG_OBJECT(NtCurrentTeb()) = object; }
EXPORT NTSTATUS NTAPI DbgUiConnectToDbg(void) {
  if (TEB_DEBUG_OBJECT(NtCurrentTeb())) return STATUS_SUCCESS;
  OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, NULL, 0, NULL, NULL};
  HANDLE object = NULL;
  const NTSTATUS status = NtCreateDebugObject(&object, 0x1F000F, &attributes, 1 /* DEBUG_KILL_ON_CLOSE */);
  if (NT_SUCCESS(status)) TEB_DEBUG_OBJECT(NtCurrentTeb()) = object;
  return status;
}
EXPORT NTSTATUS NTAPI DbgUiWaitStateChange(PVOID state_change, PLARGE_INTEGER timeout) { return NtWaitForDebugEvent(TEB_DEBUG_OBJECT(NtCurrentTeb()), TRUE, timeout, state_change); }
EXPORT NTSTATUS NTAPI DbgUiContinue(PVOID client_id, NTSTATUS status) { return NtDebugContinue(TEB_DEBUG_OBJECT(NtCurrentTeb()), client_id, status); }
EXPORT NTSTATUS NTAPI DbgUiStopDebugging(HANDLE process) { return NtRemoveProcessDebug(process, TEB_DEBUG_OBJECT(NtCurrentTeb())); }
/* The thread that DbgUiIssueRemoteBreakin makes in the program runs this. (A 32-bit program is not debugged yet: there is nothing to break into.) */
EXPORT void NTAPI DbgUiRemoteBreakin(PVOID parameter) { (void)parameter; RtlExitUserThread(0); }
EXPORT NTSTATUS NTAPI DbgUiIssueRemoteBreakin(HANDLE process) {
  HANDLE thread = NULL;
  const NTSTATUS status = NtCreateThreadEx(&thread, 0x1FFFFF, NULL, process, (PVOID)DbgUiRemoteBreakin, NULL, 0, 0, 0, 0, NULL);
  if (NT_SUCCESS(status)) NtClose(thread);
  return status;
}
EXPORT NTSTATUS NTAPI DbgUiDebugActiveProcess(HANDLE process) {
  NTSTATUS status = NtDebugActiveProcess(process, TEB_DEBUG_OBJECT(NtCurrentTeb()));
  if (NT_SUCCESS(status)) {
    status = DbgUiIssueRemoteBreakin(process);
    if (!NT_SUCCESS(status)) DbgUiStopDebugging(process);
  }
  return status;
}
