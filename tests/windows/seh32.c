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

/* Exceptions in a 32-bit Windows program, for tests/wow64_qemu.sh (docs/Wow64.md): the faults of the processor that the kernel delivers with an i386
 * CONTEXT, the handlers a function chains at FS:[0], going on from a changed context (NtContinue), unwinding (RtlUnwind), nested exceptions, the
 * vectored handlers, the unhandled-exception filter, the Microsoft compilers' __try frames (_except_handler3 of msvcrt.dll, with a frame made by hand:
 * this compiler has no __try), and a thread's context. Ends with exit code 42 if every check passed.
 *   i686-w64-mingw32-gcc -O1 -o seh32.exe tests/windows/seh32.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "seh32: ok: %s\n" : "seh32: FAILED: %s\n", what);
  if (ok) ++g_passed; else ++g_failed;
}

/* A frame of the exception list, as a function with a handler has on its stack. */
struct Frame;
typedef int (__cdecl *Handler)(EXCEPTION_RECORD *, struct Frame *, CONTEXT *, void *);
typedef struct Frame { struct Frame *prev; Handler handler; } Frame;
static Frame *chain_head(void) { Frame *head; __asm__("movl %%fs:0, %0" : "=r"(head)); return head; }
static void chain_set(Frame *head) { __asm__ volatile("movl %0, %%fs:0" : : "r"(head) : "memory"); }
static void link_frame(Frame *frame, Handler handler) { frame->handler = handler; frame->prev = chain_head(); chain_set(frame); }
static void unlink_frame(Frame *frame) { chain_set(frame->prev); }
enum { kContinueExecution = 0, kContinueSearch = 1 };

static volatile int g_calls, g_good, g_target = 77;
static volatile DWORD g_last_code;

/* ---- going on after an exception ---- */
static int __cdecl on_software(EXCEPTION_RECORD *record, Frame *frame, CONTEXT *context, void *dispatcher) {
  (void)frame; (void)dispatcher;
  ++g_calls;
  g_last_code = record->ExceptionCode;
  g_good = record->NumberParameters == 2 && record->ExceptionInformation[0] == 7 && record->ExceptionInformation[1] == 9 && record->ExceptionFlags == 0 &&
           context->Eip == (DWORD)(ULONG_PTR)record->ExceptionAddress && (context->ContextFlags & CONTEXT_CONTROL) == CONTEXT_CONTROL;
  return kContinueExecution;
}
static int __cdecl on_fault(EXCEPTION_RECORD *record, Frame *frame, CONTEXT *context, void *dispatcher) {
  (void)frame; (void)dispatcher;
  if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return kContinueSearch;
  ++g_calls;
  g_last_code = record->ExceptionCode;
  const int at_instruction = context->Eip == (DWORD)(ULONG_PTR)record->ExceptionAddress;
  switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
      if (record->ExceptionInformation[0] == 0) {      /* a read of address 0x10: point the register at a variable and try again */
        g_good = at_instruction && record->NumberParameters == 2 && record->ExceptionInformation[1] == 0x10 && context->Ecx == 0x10 && context->SegCs != 0 &&
                 context->Esp > (DWORD)(ULONG_PTR)&record && (context->ContextFlags & CONTEXT_EXTENDED_REGISTERS) == CONTEXT_EXTENDED_REGISTERS;
        context->Ecx = (DWORD)(ULONG_PTR)&g_target;
      } else {                                         /* a write: step over the instruction (movl $imm32, (%ecx) is six bytes) */
        g_good = at_instruction && record->ExceptionInformation[0] == 1 && record->ExceptionInformation[1] == 0x20;
        context->Eip += 6;
      }
      return kContinueExecution;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      g_good = at_instruction && context->Ecx == 0;
      context->Ecx = 3;
      return kContinueExecution;
    case EXCEPTION_BREAKPOINT:
      g_good = at_instruction && *(const unsigned char *)(ULONG_PTR)context->Eip == 0xCC;
      context->Eip += 1;
      return kContinueExecution;
    default: return kContinueSearch;
  }
}

/* ---- the floating-point state in the CONTEXT ---- */
static volatile int g_float_good;
static int __cdecl on_float_fault(EXCEPTION_RECORD *record, Frame *frame, CONTEXT *context, void *dispatcher) {
  (void)frame; (void)dispatcher;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return kContinueSearch;
  /* One register of the x87 stack is in use (2.5: sign 0, exponent 0x4000, mantissa 0xA000...), and XMM3 holds 0x11223344 in its low word. */
  const unsigned top = (context->FloatSave.StatusWord >> 11) & 7;
  const BYTE *st0 = context->FloatSave.RegisterArea;
  const DWORD xmm3 = *(const DWORD *)(context->ExtendedRegisters + 160 + 3 * 16);
  g_float_good = ((context->FloatSave.TagWord >> (2 * top)) & 3) == 0 && st0[9] == 0x40 && st0[8] == 0x00 && st0[7] == 0xA0 && xmm3 == 0x11223344u &&
                 (context->FloatSave.ControlWord & 0xFFFF) == (*(const WORD *)context->ExtendedRegisters);
  context->Ecx = (DWORD)(ULONG_PTR)&g_target;
  return kContinueExecution;
}

/* ---- unwinding ---- */
static void *g_jump[5];
static volatile int g_inner_search, g_inner_unwind, g_chain_good, g_nested_seen;
static int __cdecl on_inner(EXCEPTION_RECORD *record, Frame *frame, CONTEXT *context, void *dispatcher) {
  (void)frame; (void)context; (void)dispatcher;
  if (record->ExceptionFlags & EXCEPTION_UNWINDING) ++g_inner_unwind; else ++g_inner_search;
  return kContinueSearch;
}
static int __cdecl on_outer(EXCEPTION_RECORD *record, Frame *frame, CONTEXT *context, void *dispatcher) {
  (void)context; (void)dispatcher;
  if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return kContinueSearch;
  g_last_code = record->ExceptionCode;
  RtlUnwind(frame, NULL, record, NULL);
  g_chain_good = chain_head() == frame && (record->ExceptionFlags & EXCEPTION_UNWINDING) != 0;
  __builtin_longjmp(g_jump, 1);
}
static int __cdecl on_stubborn(EXCEPTION_RECORD *record, Frame *frame, CONTEXT *context, void *dispatcher) {
  (void)frame; (void)context; (void)dispatcher;
  if (record->ExceptionCode != 0xE0000003u || (record->ExceptionFlags & EXCEPTION_UNWINDING)) return kContinueSearch;
  ++g_calls;
  return kContinueExecution;
}
static __attribute__((noinline)) void fault_inside_a_frame(void) {
  Frame inner;
  link_frame(&inner, on_inner);
  *(volatile int *)0x30 = 1;
  unlink_frame(&inner);
}
static __attribute__((noinline)) int unwind_test(int noncontinuable) {
  Frame outer;
  volatile int caught = 0;
  Frame *const before = chain_head();
  if (!__builtin_setjmp(g_jump)) {
    link_frame(&outer, on_outer);
    if (noncontinuable) {      /* a handler says "go on" for an exception that cannot: that raises STATUS_NONCONTINUABLE_EXCEPTION, which the outer frame takes */
      Frame below;
      link_frame(&below, on_stubborn);
      ULONG_PTR arguments[2] = {7, 9};
      RaiseException(0xE0000003u, EXCEPTION_NONCONTINUABLE, 2, arguments);
    } else {
      fault_inside_a_frame();
    }
  } else {
    caught = 1;
  }
  chain_set(before);
  return caught;
}

/* ---- an exception inside a handler ---- */
static int __cdecl on_nested(EXCEPTION_RECORD *record, Frame *frame, CONTEXT *context, void *dispatcher) {
  (void)frame; (void)context; (void)dispatcher;
  ++g_calls;
  if (record->ExceptionCode == 0xE0000010u) {
    RaiseException(0xE0000011u, 0, 0, NULL);      /* comes back here, marked as nested */
    return kContinueExecution;
  }
  if (record->ExceptionCode == 0xE0000011u && (record->ExceptionFlags & EXCEPTION_NESTED_CALL)) ++g_nested_seen;
  return kContinueExecution;
}

/* ---- the vectored handlers and the last filter ---- */
static volatile int g_vectored_calls, g_filter_calls;
static LONG CALLBACK vectored(EXCEPTION_POINTERS *pointers) {
  ++g_vectored_calls;
  return pointers->ExceptionRecord->ExceptionCode == 0xE0000021u ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI last_filter(EXCEPTION_POINTERS *pointers) {
  ++g_filter_calls;
  if (pointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    pointers->ContextRecord->Ecx = (DWORD)(ULONG_PTR)&g_target;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static __attribute__((noinline)) int read_through(ULONG_PTR address) {
  int value;
  __asm__ volatile("movl (%%ecx), %%eax" : "=a"(value), "+c"(address) : : "memory");
  return value;
}

/* ---- a __try frame of the Microsoft compilers, made by hand ----
 * int msvc_try(void): __try { __try { *(int *)0 = 5; } __finally { ++g_finally_runs; } } __except (g_filter_code = GetExceptionCode(), 1) { return 200; } */
void *g_except_handler3;
volatile int g_finally_runs;
volatile DWORD g_filter_code;
int msvc_try(void);
__asm__(
    ".data\n"
    "_msvc_scopes:\n"
    "  .long -1, _msvc_filter, _msvc_except\n"       /* level 0: __try/__except */
    "  .long 0, 0, _msvc_finally\n"                  /* level 1: __try/__finally, inside level 0 */
    ".text\n"
    "_msvc_try:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  pushl $-1\n"                                  /* [ebp-4] the try level */
    "  pushl $_msvc_scopes\n"                        /* [ebp-8] */
    "  pushl _g_except_handler3\n"                   /* [ebp-12] */
    "  pushl %fs:0\n"                                /* [ebp-16] */
    "  movl %esp, %fs:0\n"
    "  subl $8, %esp\n"                              /* [ebp-20] the exception pointers, [ebp-24] the stack pointer to come back to */
    "  pushl %ebx\n"
    "  pushl %esi\n"
    "  pushl %edi\n"
    "  movl %esp, -24(%ebp)\n"
    "  movl $0, -4(%ebp)\n"
    "  movl $1, -4(%ebp)\n"
    "  xorl %ecx, %ecx\n"
    "  movl $5, (%ecx)\n"
    "  movl $-1, -4(%ebp)\n"
    "  movl $100, %eax\n"
    "  jmp 1f\n"
    "_msvc_filter:\n"
    "  movl -20(%ebp), %eax\n"
    "  movl (%eax), %eax\n"
    "  movl (%eax), %eax\n"
    "  movl %eax, _g_filter_code\n"
    "  movl $1, %eax\n"
    "  ret\n"
    "_msvc_finally:\n"
    "  incl _g_finally_runs\n"
    "  ret\n"
    "_msvc_except:\n"
    "  movl -24(%ebp), %esp\n"
    "  movl $-1, -4(%ebp)\n"
    "  movl $200, %eax\n"
    "1:\n"
    "  movl -16(%ebp), %ecx\n"
    "  movl %ecx, %fs:0\n"
    "  leal -36(%ebp), %esp\n"
    "  popl %edi\n"
    "  popl %esi\n"
    "  popl %ebx\n"
    "  movl %ebp, %esp\n"
    "  popl %ebp\n"
    "  ret\n");

/* ---- threads ---- */
static DWORD WINAPI faulting_thread(LPVOID parameter) {
  (void)parameter;
  Frame frame;
  link_frame(&frame, on_fault);
  const int value = read_through(0x10);
  unlink_frame(&frame);
  return (DWORD)value;
}
static DWORD WINAPI idle_thread(LPVOID parameter) { return (DWORD)(ULONG_PTR)parameter + 1; }

int main(void) {
  printf("seh32: exceptions in a 32-bit program\n");
  Frame frame;
  Frame *const top = chain_head();
  check(top != NULL, "the thread has an exception list");

  /* Raised by software, and gone on from. */
  link_frame(&frame, on_software);
  ULONG_PTR arguments[2] = {7, 9};
  volatile int after = 0;
  RaiseException(0xE0000001u, 0, 2, arguments);
  after = 1;
  unlink_frame(&frame);
  check(g_calls == 1 && g_last_code == 0xE0000001u && g_good && after == 1, "RaiseException reaches the frame's handler, which lets the program go on");

  /* Faults of the processor. */
  link_frame(&frame, on_fault);
  g_calls = 0; g_good = 0;
  int value = read_through(0x10);
  check(g_calls == 1 && g_good && value == 77, "a read of a bad address: the handler changes a register in the CONTEXT and the read is tried again");
  g_calls = 0; g_good = 0;
  __asm__ volatile("movl $0x20, %%ecx\n\tmovl $0x12345678, (%%ecx)" : : : "ecx", "memory");
  check(g_calls == 1 && g_good, "a write to a bad address: the handler steps over the instruction");
  g_calls = 0; g_good = 0;
  int quotient;
  __asm__ volatile("xorl %%ecx, %%ecx\n\txorl %%edx, %%edx\n\tmovl $12, %%eax\n\tidivl %%ecx" : "=a"(quotient) : : "ecx", "edx", "cc");
  check(g_calls == 1 && g_good && g_last_code == EXCEPTION_INT_DIVIDE_BY_ZERO && quotient == 4, "a division by zero is an exception, and is tried again with a divisor");
  g_calls = 0; g_good = 0;
  __asm__ volatile("int3");
  check(g_calls == 1 && g_good && g_last_code == EXCEPTION_BREAKPOINT, "a breakpoint is reported at the INT3");
  unlink_frame(&frame);

  /* The floating-point registers are in the CONTEXT and come back. */
  link_frame(&frame, on_float_fault);
  static const double two_and_a_half = 2.5;
  static const DWORD pattern[4] __attribute__((aligned(16))) = {0x11223344u, 0, 0, 0};
  double back = 0;
  DWORD xmm_back[4] __attribute__((aligned(16))) = {0, 0, 0, 0};
  __asm__ volatile("movdqa %1, %%xmm3\n\tfldl %0\n\tmovl $0x10, %%ecx\n\tmovl (%%ecx), %%eax\n\tfstpl %2\n\tmovdqa %%xmm3, %3"
                   : : "m"(two_and_a_half), "m"(pattern), "m"(back), "m"(xmm_back) : "eax", "ecx", "memory");
  unlink_frame(&frame);
  check(g_float_good, "the x87 and SSE registers are in the CONTEXT of a fault");
  check(back == 2.5 && xmm_back[0] == 0x11223344u, "and are what they were when the program goes on");

  /* Unwinding to the frame that handles it. */
  check(unwind_test(0) == 1 && g_last_code == EXCEPTION_ACCESS_VIOLATION && g_inner_search == 1 && g_inner_unwind == 1 && g_chain_good,
        "RtlUnwind: the frame above is asked first, then told to unwind, and is off the list");
  g_calls = 0;
  check(unwind_test(1) == 1 && g_last_code == 0xC0000025u && g_calls == 1, "going on from an exception that cannot be continued raises STATUS_NONCONTINUABLE_EXCEPTION");
  check(chain_head() == top, "the exception list is what it was");

  /* An exception inside a handler. */
  link_frame(&frame, on_nested);
  g_calls = 0;
  RaiseException(0xE0000010u, 0, 0, NULL);
  unlink_frame(&frame);
  check(g_calls == 2 && g_nested_seen == 1, "an exception raised inside a handler is marked EXCEPTION_NESTED_CALL");

  /* Vectored handlers come before the frames. */
  PVOID cookie = AddVectoredExceptionHandler(1, vectored);
  link_frame(&frame, on_software);
  g_calls = 0;
  RaiseException(0xE0000020u, 0, 2, arguments);
  RaiseException(0xE0000021u, 0, 2, arguments);      /* (the vectored handler ends this one) */
  unlink_frame(&frame);
  check(cookie != NULL && g_vectored_calls == 2 && g_calls == 1, "a vectored handler sees every exception first and can end the search");
  check(RemoveVectoredExceptionHandler(cookie) != 0, "RemoveVectoredExceptionHandler");

  /* What no frame handles goes to the unhandled-exception filter. */
  LPTOP_LEVEL_EXCEPTION_FILTER old_filter = SetUnhandledExceptionFilter(last_filter);
  value = read_through(0x10);
  SetUnhandledExceptionFilter(old_filter);
  check(g_filter_calls == 1 && value == 77, "the unhandled-exception filter gets an exception no frame took, and can let the program go on");

  /* The Microsoft compilers' __try frame, handled by msvcrt.dll. */
  g_except_handler3 = (void *)GetProcAddress(GetModuleHandleA("msvcrt.dll"), "_except_handler3");
  check(g_except_handler3 != NULL, "msvcrt.dll has _except_handler3");
  if (g_except_handler3) {
    const int result = msvc_try();
    check(result == 200 && g_filter_code == EXCEPTION_ACCESS_VIOLATION && g_finally_runs == 1 && chain_head() == top,
          "_except_handler3: the filter runs, the __finally block runs, and the function goes on in its __except block");
  }

  /* A thread's exceptions, and a thread's context. */
  DWORD code = 0;
  HANDLE thread = CreateThread(NULL, 0, faulting_thread, NULL, 0, NULL);
  check(thread != NULL && WaitForSingleObject(thread, 20000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &code) && code == 77, "a fault in another thread is handled there");
  if (thread) CloseHandle(thread);
  thread = CreateThread(NULL, 0, idle_thread, (LPVOID)(ULONG_PTR)40, CREATE_SUSPENDED, NULL);
  CONTEXT context;
  memset(&context, 0, sizeof(context));
  context.ContextFlags = CONTEXT_FULL;
  const BOOL got = thread != NULL && GetThreadContext(thread, &context);
  if (!got) printf("seh32: GetThreadContext: error %lu\n", (unsigned long)GetLastError());
  check(sizeof(CONTEXT) == 716 && got && context.Eip != 0 && context.Eip < 0x7FFF0000u && context.Esp != 0 && context.Esp < 0x7FFF0000u,
        "GetThreadContext gives the registers of a suspended thread");
  check(got && SetThreadContext(thread, &context) && ResumeThread(thread) == 1 && WaitForSingleObject(thread, 20000) == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &code) && code == 41, "SetThreadContext, and the thread runs");
  if (thread) CloseHandle(thread);

  printf(g_failed == 0 ? "seh32: all %d checks passed\n" : "seh32: %d checks passed, some FAILED\n", g_passed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
