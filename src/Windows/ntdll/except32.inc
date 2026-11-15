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
 * dispatched to the handlers a function chains through FS:[0], and unwinding walks the same chain. A fault of the processor arrives from the kernel at
 * KiUserExceptionDispatcher with an i386 CONTEXT; an exception that software raises starts at RtlRaiseException. Either way: the vectored handlers,
 * the frames' handlers, the unhandled-exception filter, and then the process ends with the exception's code. */

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

/* ---- the services that take a CONTEXT: the kernel reads and writes the i386 layout for a 32-bit program (src/Process/WindowsException.cpp) ---------- */
EXPORT NTSTATUS NTAPI NtContinue(CONTEXT *context, BOOLEAN test_alert) {
  const ULONG64 v[4] = {WOW_POINTER(context), test_alert, 0, 0};
  return wow_service(NT_SERVICE_NtContinue, v);
}
EXPORT NTSTATUS NTAPI NtGetContextThread(HANDLE thread, CONTEXT *context) {
  const ULONG64 v[4] = {WOW_HANDLE(thread), WOW_POINTER(context), 0, 0};
  return wow_service(NT_SERVICE_NtGetContextThread, v);
}
EXPORT NTSTATUS NTAPI NtSetContextThread(HANDLE thread, const CONTEXT *context) {
  const ULONG64 v[4] = {WOW_HANDLE(thread), WOW_POINTER(context), 0, 0};
  return wow_service(NT_SERVICE_NtSetContextThread, v);
}
/* (the last chance: the kernel ends the process with the exception's code) */
static NTSTATUS raise_exception_final(EXCEPTION_RECORD *record, CONTEXT *context) {
  const ULONG64 v[4] = {WOW_POINTER(record), WOW_POINTER(context), 0, 0};
  return wow_service(NT_SERVICE_NtRaiseException, v);
}

/* The registers of the caller's caller, found through the frame pointer as on Windows: Ebp, Eip and Esp are what they will be when the function that
 * called this one has returned (it must have a frame pointer). No floating-point state. */
EXPORT __attribute__((naked)) void NTAPI RtlCaptureContext(CONTEXT *context) {
  __asm__ volatile(
      "pushfl\n"
      "pushl %eax\n"
      "movl 12(%esp), %eax\n"
      "movl $0x10007, (%eax)\n"      /* CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS */
      "movl %edi, 0x9c(%eax)\n"
      "movl %esi, 0xa0(%eax)\n"
      "movl %ebx, 0xa4(%eax)\n"
      "movl %edx, 0xa8(%eax)\n"
      "movl %ecx, 0xac(%eax)\n"
      "popl 0xb0(%eax)\n"            /* Eax */
      "popl 0xc0(%eax)\n"            /* EFlags */
      "xorl %edx, %edx\n"
      "movw %gs, %dx\n  movl %edx, 0x8c(%eax)\n"
      "movw %fs, %dx\n  movl %edx, 0x90(%eax)\n"
      "movw %es, %dx\n  movl %edx, 0x94(%eax)\n"
      "movw %ds, %dx\n  movl %edx, 0x98(%eax)\n"
      "movw %cs, %dx\n  movl %edx, 0xbc(%eax)\n"
      "movw %ss, %dx\n  movl %edx, 0xc8(%eax)\n"
      "movl 0(%ebp), %edx\n  movl %edx, 0xb4(%eax)\n"      /* Ebp */
      "movl 4(%ebp), %edx\n  movl %edx, 0xb8(%eax)\n"      /* Eip */
      "leal 8(%ebp), %edx\n  movl %edx, 0xc4(%eax)\n"      /* Esp */
      "movl 0xa8(%eax), %edx\n"
      "movl 0xb0(%eax), %eax\n"
      "ret $4");
}

/* ---- the handlers of the frames -------------------------------------------------------------------------------------------------------------------
 * A function that handles exceptions links an EXCEPTION_REGISTRATION_RECORD (on its stack) at FS:[0]: the previous one, and its handler. Dispatching
 * walks that list from the newest; unwinding walks it again, telling each handler that its frame is going away, and takes the records off. */
typedef int (__cdecl *FrameHandler)(EXCEPTION_RECORD *, EXCEPTION_REGISTRATION_RECORD *, CONTEXT *, EXCEPTION_REGISTRATION_RECORD **);

static inline EXCEPTION_REGISTRATION_RECORD *chain_head(void) {
  EXCEPTION_REGISTRATION_RECORD *head;
  __asm__("movl %%fs:0, %0" : "=r"(head));
  return head;
}
static inline void chain_set(EXCEPTION_REGISTRATION_RECORD *head) { __asm__ volatile("movl %0, %%fs:0" : : "r"(head) : "memory"); }

/* Calls a frame's handler with a frame of its own around it, so that an exception inside the handler is noticed (`guard` is that frame's handler; it
 * finds the frame whose handler was running in the third slot of its record). A handler may leave the stack as it likes: the stack pointer comes
 * back from the guard's record, and the registers a C function keeps are saved here. */
int __cdecl tcos_call_frame_handler(EXCEPTION_RECORD *record, EXCEPTION_REGISTRATION_RECORD *frame, CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher,
                                    FrameHandler handler, FrameHandler guard);
__asm__(
    ".text\n"
    ".globl _tcos_call_frame_handler\n"
    "_tcos_call_frame_handler:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  pushl %ebx\n"
    "  pushl %esi\n"
    "  pushl %edi\n"
    "  pushl 12(%ebp)\n"         /* the frame whose handler runs */
    "  pushl 28(%ebp)\n"         /* the guard */
    "  pushl %fs:0\n"
    "  movl %esp, %fs:0\n"
    "  pushl 20(%ebp)\n"
    "  pushl 16(%ebp)\n"
    "  pushl 12(%ebp)\n"
    "  pushl 8(%ebp)\n"
    "  movl 24(%ebp), %ecx\n"
    "  call *%ecx\n"
    "  movl %fs:0, %esp\n"
    "  popl %fs:0\n"
    "  leal -12(%ebp), %esp\n"
    "  popl %edi\n"
    "  popl %esi\n"
    "  popl %ebx\n"
    "  popl %ebp\n"
    "  ret\n");

static EXCEPTION_REGISTRATION_RECORD *guarded_frame(EXCEPTION_REGISTRATION_RECORD *guard) { return ((EXCEPTION_REGISTRATION_RECORD **)guard)[2]; }
/* An exception while a handler was looking at another: the search goes on, marked as nested until it is past that handler's frame. */
static int __cdecl nested_exception_guard(EXCEPTION_RECORD *record, EXCEPTION_REGISTRATION_RECORD *frame, CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher) {
  (void)context;
  if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
  *dispatcher = guarded_frame(frame);
  return ExceptionNestedException;
}
/* An unwind that passes a handler which was itself unwinding: it takes over from that handler's frame. */
static int __cdecl collided_unwind_guard(EXCEPTION_RECORD *record, EXCEPTION_REGISTRATION_RECORD *frame, CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher) {
  (void)context;
  if (!(record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))) return ExceptionContinueSearch;
  *dispatcher = guarded_frame(frame);
  return ExceptionCollidedUnwind;
}

/* A record of the list must be on the thread's stack, aligned, and its handler must not be (a smashed stack is not followed into code on it). */
static BOOL frame_is_sound(const EXCEPTION_REGISTRATION_RECORD *frame) {
  const ULONG_PTR base = *(const ULONG_PTR *)TEB_BYTE(NtCurrentTeb(), 0x04), limit = *(const ULONG_PTR *)TEB_BYTE(NtCurrentTeb(), 0x08);
  const ULONG_PTR at = (ULONG_PTR)frame;
  if ((at & 3) || at < limit || base < sizeof(*frame) || at > base - sizeof(*frame)) return FALSE;
  const ULONG_PTR handler = (ULONG_PTR)frame->Handler;
  return !(handler >= limit && handler < base);
}

static void raise_with_cause(NTSTATUS status, EXCEPTION_RECORD *cause) {
  EXCEPTION_RECORD record = {.ExceptionCode = (DWORD)status, .ExceptionFlags = EXCEPTION_NONCONTINUABLE, .ExceptionRecord = cause};
  RtlRaiseException(&record);
}

/* The vectored handlers, then the handlers of the frames. TRUE if one of them wants the program to go on where it was (with the context as it now is). */
EXPORT BOOLEAN NTAPI RtlDispatchException(EXCEPTION_RECORD *record, CONTEXT *context) {
  EXCEPTION_POINTERS pointers = {record, context};
  for (int i = 0; i < MAX_VECTORED && g_vectored[i].handler; ++i)
    if (g_vectored[i].handler(&pointers) == EXCEPTION_CONTINUE_EXECUTION) return TRUE;
  EXCEPTION_REGISTRATION_RECORD *nested = NULL;
  for (EXCEPTION_REGISTRATION_RECORD *frame = chain_head(); frame != EXCEPTION_CHAIN_END; frame = frame->Prev) {
    if (!frame_is_sound(frame)) { record->ExceptionFlags |= EXCEPTION_STACK_INVALID; break; }
    EXCEPTION_REGISTRATION_RECORD *dispatcher = NULL;
    const int answer = tcos_call_frame_handler(record, frame, context, &dispatcher, (FrameHandler)frame->Handler, nested_exception_guard);
    if (frame == nested) { record->ExceptionFlags &= ~(DWORD)EXCEPTION_NESTED_CALL; nested = NULL; }
    switch (answer) {
      case ExceptionContinueExecution:
        if (!(record->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) return TRUE;
        raise_with_cause((NTSTATUS)0xC0000025u, record);      /* STATUS_NONCONTINUABLE_EXCEPTION */
        break;
      case ExceptionContinueSearch: break;
      case ExceptionNestedException:
        record->ExceptionFlags |= EXCEPTION_NESTED_CALL;
        if ((ULONG_PTR)dispatcher > (ULONG_PTR)nested) nested = dispatcher;
        break;
      default: raise_with_cause((NTSTATUS)0xC0000026u, record); break;      /* STATUS_INVALID_DISPOSITION */
    }
  }
  return FALSE;
}

/* Nobody handled it: the top-level filter gets its say, then the process ends with the exception's code. */
static __attribute__((noreturn, noinline)) void unhandled_exception(EXCEPTION_RECORD *record, CONTEXT *context) {
  EXCEPTION_POINTERS pointers = {record, context};
  if (g_unhandled_hook) {
    if (g_unhandled_hook(&pointers) == EXCEPTION_CONTINUE_EXECUTION) NtContinue(context, FALSE);
  } else if (g_unhandled_filter) {
    if (g_unhandled_filter(&pointers) == EXCEPTION_CONTINUE_EXECUTION) NtContinue(context, FALSE);
  }
  raise_exception_final(record, context);
  for (;;) NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)record->ExceptionCode);
}

/* The kernel starts the thread here when the program faults: on the stack are the record's and the CONTEXT's addresses, as a call's arguments without
 * the return address (src/Process/WindowsException.cpp). */
__attribute__((noreturn, noinline, used)) void __cdecl tcos_dispatch_exception(EXCEPTION_RECORD *record, CONTEXT *context) {
  if (debugger_handled(record, context, TRUE)) NtContinue(context, FALSE);
  if (RtlDispatchException(record, context)) NtContinue(context, FALSE);
  unhandled_exception(record, context);
}
EXPORT __attribute__((naked)) void NTAPI KiUserExceptionDispatcher(void) { __asm__ volatile("call _tcos_dispatch_exception\n\tint3"); }

/* An exception raised by software, at the place RtlRaiseException was called from: the context is the caller's as it will be after the call. */
__attribute__((noinline, used)) void __cdecl tcos_raise_exception(EXCEPTION_RECORD *record, CONTEXT *context) {
  if (debugger_handled(record, context, TRUE) || RtlDispatchException(record, context)) {
    NtContinue(context, FALSE);
    return;      /* (the service failed: going back to the caller is the same thing, less what a handler changed) */
  }
  unhandled_exception(record, context);
}
EXPORT __attribute__((naked)) void NTAPI RtlRaiseException(EXCEPTION_RECORD *record) {
  __asm__ volatile(
      "pushl %ebp\n"
      "movl %esp, %ebp\n"
      "subl $0x2cc, %esp\n"          /* a CONTEXT */
      "pushl %esp\n"
      "call _RtlCaptureContext@4\n"  /* (what it calls the caller's caller is who called us) */
      "movl 8(%ebp), %ecx\n"         /* the record */
      "movl 4(%ebp), %eax\n"
      "movl %eax, 12(%ecx)\n"        /* ExceptionAddress */
      "leal 12(%ebp), %eax\n"
      "movl %eax, 0xc4(%esp)\n"      /* Esp once the argument is off the stack */
      "movl %esp, %eax\n"
      "pushl %eax\n"
      "pushl %ecx\n"
      "call _tcos_raise_exception\n"
      "leave\n"
      "ret $4");
}
EXPORT void NTAPI RtlRaiseStatus(NTSTATUS status) {
  EXCEPTION_RECORD record = {.ExceptionCode = (DWORD)status, .ExceptionFlags = EXCEPTION_NONCONTINUABLE};
  RtlRaiseException(&record);
}
EXPORT NTSTATUS NTAPI NtRaiseException(EXCEPTION_RECORD *record, CONTEXT *context, BOOLEAN first_chance) {
  if (first_chance && (debugger_handled(record, context, TRUE) || RtlDispatchException(record, context))) return NtContinue(context, FALSE);
  unhandled_exception(record, context);
}

/* Unwinds the frames above `target_frame` (all of them if it is NULL): each one's handler is called with EXCEPTION_UNWINDING to clean up, and its
 * record is taken off the list. Then it returns: the caller, which is the handler that decided to handle the exception, moves to its own frame. */
EXPORT __attribute__((noinline)) void NTAPI RtlUnwind(PVOID target_frame, PVOID target_ip, EXCEPTION_RECORD *record, PVOID return_value) {
  (void)target_ip;
  const ULONG_PTR *self = __builtin_frame_address(0);
  CONTEXT context;
  EXCEPTION_RECORD unwind;
  memset(&context, 0, sizeof(context));
  context.ContextFlags = CONTEXT_FULL;
  context.SegCs = 0x3B; context.SegDs = context.SegEs = context.SegFs = context.SegSs = 0x2B;
  context.EFlags = 0x202;
  context.Ebp = self[0];
  context.Eip = self[1];
  context.Esp = (ULONG_PTR)self + 8 + 16;      /* (the four arguments are off the stack when this returns) */
  context.Eax = (ULONG_PTR)return_value;
  if (!record) {
    memset(&unwind, 0, sizeof(unwind));
    unwind.ExceptionCode = 0xC0000027u;      /* STATUS_UNWIND */
    unwind.ExceptionAddress = (PVOID)context.Eip;
    record = &unwind;
  }
  record->ExceptionFlags |= EXCEPTION_UNWINDING | (target_frame ? 0 : EXCEPTION_EXIT_UNWIND);
  EXCEPTION_REGISTRATION_RECORD *frame = chain_head();
  while (frame != EXCEPTION_CHAIN_END && frame != target_frame) {
    if (target_frame && (ULONG_PTR)target_frame < (ULONG_PTR)frame) raise_with_cause((NTSTATUS)0xC0000029u, record);      /* STATUS_INVALID_UNWIND_TARGET */
    if (!frame_is_sound(frame)) raise_with_cause((NTSTATUS)0xC0000028u, record);      /* STATUS_BAD_STACK */
    EXCEPTION_REGISTRATION_RECORD *dispatcher = NULL;
    switch (tcos_call_frame_handler(record, frame, &context, &dispatcher, (FrameHandler)frame->Handler, collided_unwind_guard)) {
      case ExceptionContinueSearch: break;
      case ExceptionCollidedUnwind: frame = dispatcher; break;
      default: raise_with_cause((NTSTATUS)0xC0000026u, record); break;      /* STATUS_INVALID_DISPOSITION */
    }
    frame = frame->Prev;
    chain_set(frame);
  }
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
