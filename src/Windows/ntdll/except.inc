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

/* Structured exception handling of ntdll.dll: the x64 table-driven unwinder (.pdata and UNWIND_INFO), the dispatcher that walks the stack looking for handlers, and
 * RtlUnwindEx, which runs the termination handlers on the way to the frame that handles the exception. Included by ntdll.c. C++ exceptions of mingw (libgcc's
 * _GCC_specific_handler) and __try/__except (__C_specific_handler) are two language handlers over this. */

#define UWOP_PUSH_NONVOL 0
#define UWOP_ALLOC_LARGE 1
#define UWOP_ALLOC_SMALL 2
#define UWOP_SET_FPREG 3
#define UWOP_SAVE_NONVOL 4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_EPILOG 6
#define UWOP_SAVE_XMM128 8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME 10
#define STATUS_NONCONTINUABLE_EXCEPTION_ ((NTSTATUS)0xC0000025)
#define STATUS_UNWIND_ ((NTSTATUS)0xC0000027)
#define MAX_FRAMES 20000

typedef struct { BYTE VersionFlags, SizeOfProlog, CountOfCodes, FrameRegOffset; } UNWIND_INFO_HEADER;
typedef EXCEPTION_DISPOSITION (*LANGUAGE_HANDLER)(EXCEPTION_RECORD *, PVOID, CONTEXT *, DISPATCHER_CONTEXT *);
typedef LONG (NTAPI *VECTORED_HANDLER)(PEXCEPTION_POINTERS);

NT_SERVICE(NtContinue);
__asm__(".text\n.globl KiRaiseExceptionSecondChance\nKiRaiseExceptionSecondChance:\n mov %rcx, %r10\n mov $" _STR(NT_SERVICE_NtRaiseException) ", %eax\n syscall\n ret\n");
IMPORT NTSTATUS NTAPI NtContinue(CONTEXT *, BOOLEAN);
IMPORT NTSTATUS NTAPI KiRaiseExceptionSecondChance(EXCEPTION_RECORD *, CONTEXT *, BOOLEAN);

EXPORT void NTAPI RtlRaiseException(EXCEPTION_RECORD *record);

static ULONG64 *int_register(CONTEXT *c, unsigned index) { return &c->Rax + index; }

/* ---- finding the function of an address --------------------------------------------------------------------------------------------------------------------- */

typedef struct { PRUNTIME_FUNCTION table; ULONG count; ULONG64 base; } FunctionTable;
static FunctionTable g_function_tables[32];

static PRUNTIME_FUNCTION find_in_table(PRUNTIME_FUNCTION table, ULONG count, ULONG64 rva) {
  ULONG low = 0, high = count;
  while (low < high) {
    const ULONG mid = low + (high - low) / 2;
    if (rva < table[mid].BeginAddress) high = mid;
    else if (rva >= table[mid].EndAddress) low = mid + 1;
    else return &table[mid];
  }
  return NULL;
}

/* The module of the loader lists that holds `pc`: its base, or 0. */
static ULONG64 module_of(ULONG64 pc, ULONG64 *size) {
  PEB *peb = NtCurrentPeb();
  if (!peb || !peb->Ldr) return 0;
  LIST_ENTRY *head = &peb->Ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *e = head->Flink; e && e != head; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)e;
    const ULONG64 base = (ULONG64)m->DllBase;
    if (base && pc >= base && pc < base + m->SizeOfImage) { if (size) *size = m->SizeOfImage; return base; }
  }
  return 0;
}

EXPORT PVOID NTAPI RtlPcToFileHeader(PVOID pc, PVOID *base) {
  const ULONG64 b = module_of((ULONG64)pc, NULL);
  if (base) *base = (PVOID)b;
  return (PVOID)b;
}

EXPORT PRUNTIME_FUNCTION NTAPI RtlLookupFunctionEntry(ULONG64 pc, PULONG64 base, PUNWIND_HISTORY_TABLE history) {
  (void)history;
  for (unsigned i = 0; i < 32; ++i) {
    const FunctionTable *t = &g_function_tables[i];
    if (!t->table || pc < t->base) continue;
    PRUNTIME_FUNCTION f = find_in_table(t->table, t->count, pc - t->base);
    if (f) { if (base) *base = t->base; return f; }
  }
  const ULONG64 image = module_of(pc, NULL);
  if (base) *base = image;
  if (!image) return NULL;
  const BYTE *pe = (const BYTE *)image;
  const ULONG nt = *(const ULONG *)(pe + 0x3C);
  const ULONG rva = *(const ULONG *)(pe + nt + 24 + 0x70 + 3 * 8);        /* DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] */
  const ULONG size = *(const ULONG *)(pe + nt + 24 + 0x70 + 3 * 8 + 4);
  if (!rva || size < sizeof(RUNTIME_FUNCTION)) return NULL;
  return find_in_table((PRUNTIME_FUNCTION)(pe + rva), size / sizeof(RUNTIME_FUNCTION), pc - image);
}

EXPORT BOOLEAN NTAPI RtlAddFunctionTable(PRUNTIME_FUNCTION table, ULONG count, ULONG64 base) {
  for (unsigned i = 0; i < 32; ++i) {
    if (g_function_tables[i].table) continue;
    g_function_tables[i].table = table;
    g_function_tables[i].count = count;
    g_function_tables[i].base = base;
    return TRUE;
  }
  return FALSE;
}
EXPORT BOOLEAN NTAPI RtlDeleteFunctionTable(PRUNTIME_FUNCTION table) {
  for (unsigned i = 0; i < 32; ++i) if (g_function_tables[i].table == table) { g_function_tables[i].table = NULL; return TRUE; }
  return FALSE;
}
EXPORT BOOLEAN NTAPI RtlInstallFunctionTableCallback(ULONG64 id, ULONG64 base, ULONG length, PVOID callback, PVOID context, PCWSTR out_of_process) {
  (void)id; (void)base; (void)length; (void)callback; (void)context; (void)out_of_process;
  return FALSE;      /* tables made on demand are not supported */
}

/* ---- unwinding one frame -------------------------------------------------------------------------------------------------------------------------------------- */

static void remember(KNONVOLATILE_CONTEXT_POINTERS *ptrs, unsigned reg, ULONG64 *where) { if (ptrs) ptrs->Integer[reg] = where; }

/* Undoes what the function's prologue did to `c` (the registers on the way into the function from the point `c` is at), following the unwind codes of `f`. `pc_rva` is where the
 * context is in the function: inside the prologue only the codes that ran are undone. Returns the language handler of the kinds in `type` (and its data), if the function has one. */
static PEXCEPTION_ROUTINE unwind_function(ULONG type, ULONG64 base, ULONG64 pc_rva, const RUNTIME_FUNCTION *f, CONTEXT *c, PVOID *data, PULONG64 frame_ret, KNONVOLATILE_CONTEXT_POINTERS *ptrs, int top) {
  const UNWIND_INFO_HEADER *info = (const UNWIND_INFO_HEADER *)(base + f->UnwindData);
  const BYTE *codes = (const BYTE *)(info + 1);
  const unsigned count = info->CountOfCodes;
  const unsigned frame_register = info->FrameRegOffset & 15, frame_offset = info->FrameRegOffset >> 4;
  const ULONG prolog_offset = (pc_rva != ~(ULONG64)0 && pc_rva < (ULONG64)f->BeginAddress + info->SizeOfProlog) ? (ULONG)(pc_rva - f->BeginAddress) : 0xFFFFFFFFu;
  /* The frame register only counts once the prologue has set it. */
  unsigned effective_frame_register = frame_register;
  for (unsigned i = 0; i < count && frame_register; ) {
    const BYTE op = codes[2 * i + 1] & 15, inf = codes[2 * i + 1] >> 4;
    if (op == UWOP_SET_FPREG && prolog_offset < codes[2 * i]) effective_frame_register = 0;
    i += (op == UWOP_ALLOC_LARGE) ? (inf ? 3 : 2) : (op == UWOP_SAVE_NONVOL || op == UWOP_SAVE_XMM128 || op == UWOP_EPILOG) ? 2 : (op == UWOP_SAVE_NONVOL_FAR || op == UWOP_SAVE_XMM128_FAR) ? 3 : 1;
  }
  const ULONG64 frame = effective_frame_register ? *int_register(c, effective_frame_register) - (ULONG64)frame_offset * 16 : c->Rsp;
  if (top && frame_ret) *frame_ret = frame;
  int machine_frame = 0;
  for (unsigned i = 0; i < count; ) {
    const BYTE offset = codes[2 * i], op = codes[2 * i + 1] & 15, inf = codes[2 * i + 1] >> 4;
    unsigned slots = 1;
    switch (op) {
      case UWOP_ALLOC_LARGE: slots = inf ? 3 : 2; break;
      case UWOP_SAVE_NONVOL: case UWOP_SAVE_XMM128: case UWOP_EPILOG: slots = 2; break;
      case UWOP_SAVE_NONVOL_FAR: case UWOP_SAVE_XMM128_FAR: slots = 3; break;
      default: break;
    }
    if (prolog_offset < offset) { i += slots; continue; }      /* this part of the prologue had not run yet */
    switch (op) {
      case UWOP_PUSH_NONVOL: { ULONG64 *where = (ULONG64 *)c->Rsp; *int_register(c, inf) = *where; remember(ptrs, inf, where); c->Rsp += 8; break; }
      case UWOP_ALLOC_LARGE: {
        const ULONG64 size = inf ? (ULONG64)(*(const USHORT *)(codes + 2 * (i + 1)) | (ULONG64)*(const USHORT *)(codes + 2 * (i + 2)) << 16) : (ULONG64)*(const USHORT *)(codes + 2 * (i + 1)) * 8;
        c->Rsp += size;
        break;
      }
      case UWOP_ALLOC_SMALL: c->Rsp += (ULONG64)inf * 8 + 8; break;
      case UWOP_SET_FPREG: c->Rsp = *int_register(c, frame_register) - (ULONG64)frame_offset * 16; break;
      case UWOP_SAVE_NONVOL: { ULONG64 *where = (ULONG64 *)(frame + (ULONG64)*(const USHORT *)(codes + 2 * (i + 1)) * 8); *int_register(c, inf) = *where; remember(ptrs, inf, where); break; }
      case UWOP_SAVE_NONVOL_FAR: {
        ULONG64 *where = (ULONG64 *)(frame + (*(const USHORT *)(codes + 2 * (i + 1)) | (ULONG64)*(const USHORT *)(codes + 2 * (i + 2)) << 16));
        *int_register(c, inf) = *where;
        remember(ptrs, inf, where);
        break;
      }
      case UWOP_SAVE_XMM128: { const M128A *where = (const M128A *)(frame + (ULONG64)*(const USHORT *)(codes + 2 * (i + 1)) * 16); CONTEXT_XMM(c, inf) = *where; if (ptrs) ptrs->Xmm[inf] = (ULONG64 *)where; break; }
      case UWOP_SAVE_XMM128_FAR: {
        const M128A *where = (const M128A *)(frame + (*(const USHORT *)(codes + 2 * (i + 1)) | (ULONG64)*(const USHORT *)(codes + 2 * (i + 2)) << 16));
        CONTEXT_XMM(c, inf) = *where;
        if (ptrs) ptrs->Xmm[inf] = (ULONG64 *)where;
        break;
      }
      case UWOP_PUSH_MACHFRAME: {
        const ULONG64 *mf = (const ULONG64 *)(c->Rsp + (inf ? 8 : 0));      /* with an error code first when inf is 1 */
        c->Rip = mf[0];
        c->Rsp = mf[3];
        machine_frame = 1;
        break;
      }
      default: break;
    }
    i += slots;
  }
  const ULONG64 *tail = (const ULONG64 *)(codes + 2 * ((count + 1) & ~1u));
  if (info->VersionFlags >> 3 & UNW_FLAG_CHAININFO) {      /* the rest is described by the function this one continues */
    return unwind_function(type, base, ~(ULONG64)0, (const RUNTIME_FUNCTION *)tail, c, data, frame_ret, ptrs, 0);
  }
  if (!machine_frame) {
    c->Rip = *(const ULONG64 *)c->Rsp;
    c->Rsp += 8;
    c->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
  } else {
    c->ContextFlags &= ~(DWORD)CONTEXT_UNWOUND_TO_CALL;
  }
  if (!((info->VersionFlags >> 3) & type)) return NULL;
  if (data) *data = (PVOID)((const ULONG *)tail + 1);
  return (PEXCEPTION_ROUTINE)(base + *(const ULONG *)tail);
}

EXPORT PEXCEPTION_ROUTINE NTAPI RtlVirtualUnwind(ULONG type, ULONG64 base, ULONG64 pc, PRUNTIME_FUNCTION function, CONTEXT *context, PVOID *data, PULONG64 frame, KNONVOLATILE_CONTEXT_POINTERS *pointers) {
  if (data) *data = NULL;
  if (frame) *frame = context->Rsp;
  return unwind_function(type, base, pc - base, function, context, data, frame, pointers, 1);
}

/* One frame up for the dispatcher and the unwinder: fills `dc` for the frame that `c` is in and moves `c` to the frame's caller. A function without unwind information is a
 * leaf: its return address is on the top of the stack. */
static PEXCEPTION_ROUTINE unwind_step(ULONG type, DISPATCHER_CONTEXT *dc, CONTEXT *c) {
  dc->ScopeIndex = 0;
  dc->Fill0 = 0;
  dc->HandlerData = NULL;
  dc->HistoryTable = NULL;
  dc->LanguageHandler = NULL;
  dc->TargetIp = 0;
  dc->ControlPc = (c->ContextFlags & CONTEXT_UNWOUND_TO_CALL) ? c->Rip - 1 : c->Rip;      /* a return address is one past the call */
  dc->FunctionEntry = RtlLookupFunctionEntry(dc->ControlPc, &dc->ImageBase, NULL);
  if (dc->FunctionEntry) {
    PEXCEPTION_ROUTINE handler = RtlVirtualUnwind(type, dc->ImageBase, dc->ControlPc, dc->FunctionEntry, c, &dc->HandlerData, &dc->EstablisherFrame, NULL);
    dc->LanguageHandler = handler;
    return handler;
  }
  dc->EstablisherFrame = c->Rsp;
  c->Rip = *(const ULONG64 *)c->Rsp;
  c->Rsp += 8;
  c->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
  return NULL;
}

static int stack_contains(ULONG64 address) {
  const ULONG64 base = *(const ULONG64 *)TEB_BYTE(NtCurrentTeb(), 0x08), limit = *(const ULONG64 *)TEB_BYTE(NtCurrentTeb(), 0x10);
  return base == 0 || (address >= limit && address < base);
}

/* ---- capture and restore ------------------------------------------------------------------------------------------------------------------------------------- */

__asm__(
    ".text\n"
    ".globl RtlCaptureContext\nRtlCaptureContext:\n"
    "  movq %rax, 0x78(%rcx)\n  movq %rcx, 0x80(%rcx)\n  movq %rdx, 0x88(%rcx)\n  movq %rbx, 0x90(%rcx)\n"
    "  leaq 8(%rsp), %rax\n  movq %rax, 0x98(%rcx)\n"
    "  movq %rbp, 0xA0(%rcx)\n  movq %rsi, 0xA8(%rcx)\n  movq %rdi, 0xB0(%rcx)\n"
    "  movq %r8, 0xB8(%rcx)\n  movq %r9, 0xC0(%rcx)\n  movq %r10, 0xC8(%rcx)\n  movq %r11, 0xD0(%rcx)\n"
    "  movq %r12, 0xD8(%rcx)\n  movq %r13, 0xE0(%rcx)\n  movq %r14, 0xE8(%rcx)\n  movq %r15, 0xF0(%rcx)\n"
    "  movq (%rsp), %rax\n  movq %rax, 0xF8(%rcx)\n"
    "  pushfq\n  popq %rax\n  movl %eax, 0x44(%rcx)\n"
    "  movw %cs, 0x38(%rcx)\n  movw %ds, 0x3A(%rcx)\n  movw %es, 0x3C(%rcx)\n  movw %fs, 0x3E(%rcx)\n  movw %gs, 0x40(%rcx)\n  movw %ss, 0x42(%rcx)\n"
    "  stmxcsr 0x34(%rcx)\n"
    "  fxsave 0x100(%rcx)\n"
    "  movl $0x10000F, 0x30(%rcx)\n"
    "  movq 0x78(%rcx), %rax\n"
    "  ret\n"
    ".globl RtlRestoreContext\nRtlRestoreContext:\n"
    "  ldmxcsr 0x34(%rcx)\n"
    "  fxrstor 0x100(%rcx)\n"
    "  movq 0x98(%rcx), %r8\n  movq 0xF8(%rcx), %r9\n"
    "  movq %r9, -8(%r8)\n  leaq -8(%r8), %r8\n"
    "  movq 0x78(%rcx), %rax\n  movq 0x88(%rcx), %rdx\n  movq 0x90(%rcx), %rbx\n  movq 0xA0(%rcx), %rbp\n  movq 0xA8(%rcx), %rsi\n  movq 0xB0(%rcx), %rdi\n"
    "  movq 0xC0(%rcx), %r9\n  movq 0xC8(%rcx), %r10\n  movq 0xD0(%rcx), %r11\n  movq 0xD8(%rcx), %r12\n  movq 0xE0(%rcx), %r13\n  movq 0xE8(%rcx), %r14\n  movq 0xF0(%rcx), %r15\n"
    "  movq %r8, %rsp\n"
    "  movq 0xB8(%rcx), %r8\n  movq 0x80(%rcx), %rcx\n"
    "  ret\n"
    ".section .drectve\n.ascii \" -export:RtlCaptureContext -export:RtlRestoreContext\"\n.text\n");
IMPORT void NTAPI RtlCaptureContext(CONTEXT *);
IMPORT void NTAPI RtlRestoreContext(CONTEXT *, EXCEPTION_RECORD *);

/* ---- vectored handlers and the top-level filter ----------------------------------------------------------------------------------------------------------- */

static VECTORED_HANDLER g_vectored[16];
static LPTOP_LEVEL_EXCEPTION_FILTER g_top_level_filter;

EXPORT PVOID NTAPI RtlAddVectoredExceptionHandler(ULONG first, PVOID handler_arg) {
  VECTORED_HANDLER handler = (VECTORED_HANDLER)handler_arg;
  unsigned n = 0;
  while (n < 16 && g_vectored[n]) ++n;
  if (n == 16 || !handler) return NULL;
  if (first) for (unsigned i = n; i > 0; --i) g_vectored[i] = g_vectored[i - 1];
  g_vectored[first ? 0 : n] = handler;
  return (PVOID)handler;
}
EXPORT ULONG NTAPI RtlRemoveVectoredExceptionHandler(PVOID cookie) {
  for (unsigned i = 0; i < 16; ++i) {
    if (g_vectored[i] != (VECTORED_HANDLER)cookie) continue;
    for (unsigned j = i; j < 15; ++j) g_vectored[j] = g_vectored[j + 1];
    g_vectored[15] = NULL;
    return 1;
  }
  return 0;
}
EXPORT PVOID NTAPI RtlAddVectoredContinueHandler(ULONG first, PVOID handler) { (void)first; (void)handler; return NULL; }
EXPORT ULONG NTAPI RtlRemoveVectoredContinueHandler(PVOID cookie) { (void)cookie; return 0; }
EXPORT LPTOP_LEVEL_EXCEPTION_FILTER NTAPI RtlSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  LPTOP_LEVEL_EXCEPTION_FILTER old = g_top_level_filter;
  g_top_level_filter = filter;
  return old;
}

/* ---- dispatching an exception ------------------------------------------------------------------------------------------------------------------------------- */

/* The handlers of the frames from the one `context` is in, outwards. TRUE: a handler wants the thread to go on with `context` (which it may have changed). FALSE: nobody handled it. */
EXPORT BOOLEAN NTAPI RtlDispatchException(EXCEPTION_RECORD *record, CONTEXT *context) {
  EXCEPTION_POINTERS pointers = {record, context};
  for (unsigned i = 0; i < 16 && g_vectored[i]; ++i) {
    if (g_vectored[i](&pointers) == EXCEPTION_CONTINUE_EXECUTION) return TRUE;
  }
  CONTEXT current = *context, own;
  DISPATCHER_CONTEXT dc;
  EXCEPTION_RECORD again;
  for (unsigned depth = 0; depth < MAX_FRAMES && current.Rip != 0; ++depth) {
    own = current;
    PEXCEPTION_ROUTINE handler = unwind_step(UNW_FLAG_EHANDLER, &dc, &current);
    if (!stack_contains(dc.EstablisherFrame)) return FALSE;
    if (!handler) continue;
    dc.ContextRecord = &own;
    for (;;) {
      const EXCEPTION_DISPOSITION d = ((LANGUAGE_HANDLER)handler)(record, (PVOID)dc.EstablisherFrame, context, &dc);
      if (d == ExceptionContinueExecution) {
        if (!(record->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) return TRUE;
        /* A handler may not resume a noncontinuable exception: that is an exception of its own, which the same frame's handler (from where it left off) and the frames
         * outside get to see. */
        again.ExceptionCode = (DWORD)STATUS_NONCONTINUABLE_EXCEPTION_;
        again.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        again.ExceptionRecord = record;
        again.ExceptionAddress = record->ExceptionAddress;
        again.NumberParameters = 0;
        record = &again;
        continue;
      }
      if (d == ExceptionContinueSearch || d == ExceptionNestedException) break;
      return FALSE;
    }
  }
  return FALSE;
}

static void report_unhandled(const EXCEPTION_RECORD *record) {
  HANDLE err = NtCurrentPeb()->ProcessParameters ? NtCurrentPeb()->ProcessParameters->StandardError : NULL;
  if (!err) return;
  char text[96] = "Unhandled exception 0x";
  unsigned n = (unsigned)strlen(text);
  for (int shift = 28; shift >= 0; shift -= 4) text[n++] = "0123456789ABCDEF"[(record->ExceptionCode >> shift) & 15];
  const char *at = " at 0x";
  for (unsigned i = 0; at[i]; ++i) text[n++] = at[i];
  for (int shift = 60; shift >= 0; shift -= 4) text[n++] = "0123456789ABCDEF"[((ULONG64)record->ExceptionAddress >> shift) & 15];
  text[n++] = '\n';
  IO_STATUS_BLOCK iosb;
  NtWriteFile(err, NULL, NULL, NULL, &iosb, text, n, NULL, NULL);
}

/* Nobody handled it: the top-level filter gets its say, then the process ends with the exception's code. */
static __attribute__((noreturn, noinline)) void unhandled_exception(EXCEPTION_RECORD *record, CONTEXT *context) {
  if (g_init_guard && g_init_guard_teb == NtCurrentTeb()) __builtin_longjmp(g_init_guard, 1);      /* (a library's entry point failed: see call_library_entry) */
  if (g_top_level_filter) {
    EXCEPTION_POINTERS pointers = {record, context};
    if (g_top_level_filter(&pointers) == EXCEPTION_CONTINUE_EXECUTION) NtContinue(context, FALSE);
  }
  report_unhandled(record);
  KiRaiseExceptionSecondChance(record, context, FALSE);
  RtlExitUserProcess((NTSTATUS)record->ExceptionCode);
  for (;;) {}
}

/* The kernel starts the thread here when the program faults (see src/Process/WindowsException.hpp): rsp is at a machine frame, rcx the record, rdx the CONTEXT. The machine
 * frame is what lets the unwinder get from the handlers' frames to the faulting instruction (UWOP_PUSH_MACHFRAME, .seh_pushframe). */
__attribute__((noreturn, noinline)) void tcos_dispatch_exception(EXCEPTION_RECORD *record, CONTEXT *context) {
  if (RtlDispatchException(record, context)) NtContinue(context, FALSE);
  unhandled_exception(record, context);
}
__asm__(
    ".text\n"
    ".globl KiUserExceptionDispatcher\n"
    ".seh_proc KiUserExceptionDispatcher\n"
    "KiUserExceptionDispatcher:\n"
    "  .seh_pushframe\n"
    "  subq $0x20, %rsp\n"
    "  .seh_stackalloc 0x20\n"
    "  .seh_endprologue\n"
    "  call tcos_dispatch_exception\n"
    "  int3\n"
    "  .seh_endproc\n"
    ".section .drectve\n.ascii \" -export:KiUserExceptionDispatcher\"\n.text\n");

EXPORT void NTAPI RtlRaiseException(EXCEPTION_RECORD *record) {
  CONTEXT context;
  RtlCaptureContext(&context);
  /* The exception is raised where RtlRaiseException was called from: take this function's frame off the context, as if it had returned. */
  DISPATCHER_CONTEXT dc;
  unwind_step(UNW_FLAG_NHANDLER, &dc, &context);
  context.ContextFlags &= ~(DWORD)CONTEXT_UNWOUND_TO_CALL;
  record->ExceptionAddress = (PVOID)context.Rip;
  if (RtlDispatchException(record, &context)) NtContinue(&context, FALSE);
  unhandled_exception(record, &context);
}
EXPORT void NTAPI RtlRaiseStatus(NTSTATUS status) {
  EXCEPTION_RECORD record = {(DWORD)status, EXCEPTION_NONCONTINUABLE, NULL, NULL, 0, {0}};
  RtlRaiseException(&record);
}
EXPORT NTSTATUS NTAPI NtRaiseException(EXCEPTION_RECORD *record, CONTEXT *context, BOOLEAN first_chance) {
  if (first_chance && RtlDispatchException(record, context)) return NtContinue(context, FALSE);
  return KiRaiseExceptionSecondChance(record, context, FALSE);
}

/* ---- unwinding to a frame ----------------------------------------------------------------------------------------------------------------------------------- */

/* Walks the frames from the caller of RtlUnwindEx out to `target_frame` (NULL: all of them), giving each frame's handler its chance to clean up (EXCEPTION_UNWINDING), then
 * continues at `target_ip` in the target frame with `return_value` in RAX. */
EXPORT void NTAPI RtlUnwindEx(PVOID target_frame, PVOID target_ip, EXCEPTION_RECORD *record, PVOID return_value, CONTEXT *original, PUNWIND_HISTORY_TABLE history) {
  (void)original;
  EXCEPTION_RECORD local;
  CONTEXT context, next;
  DISPATCHER_CONTEXT dc;
  RtlCaptureContext(&context);
  /* start at the caller of this function */
  unwind_step(UNW_FLAG_NHANDLER, &dc, &context);
  if (record) {
    local = *record;
  } else {
    memset(&local, 0, sizeof(local));
    local.ExceptionCode = (DWORD)STATUS_UNWIND_;
    local.ExceptionAddress = (PVOID)context.Rip;
  }
  local.ExceptionFlags |= EXCEPTION_UNWINDING | (target_frame ? 0 : EXCEPTION_EXIT_UNWIND);
  next = context;
  for (unsigned depth = 0; depth < MAX_FRAMES && next.Rip != 0; ++depth) {
    PEXCEPTION_ROUTINE handler = unwind_step(UNW_FLAG_UHANDLER, &dc, &next);
    if (!dc.EstablisherFrame || !stack_contains(dc.EstablisherFrame)) break;
    const int at_target = target_frame && dc.EstablisherFrame == (ULONG64)target_frame;
    if (handler) {
      dc.ContextRecord = &context;
      dc.TargetIp = (ULONG64)target_ip;
      dc.HistoryTable = history;
      if (at_target) local.ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
      ((LANGUAGE_HANDLER)handler)(&local, (PVOID)dc.EstablisherFrame, &context, &dc);
    }
    if (at_target) break;
    context = next;
  }
  context.Rax = (ULONG64)return_value;
  context.Rip = (ULONG64)target_ip;
  context.ContextFlags &= ~(DWORD)CONTEXT_UNWOUND_TO_CALL;
  RtlRestoreContext(&context, &local);
}
EXPORT void NTAPI RtlUnwind(PVOID target_frame, PVOID target_ip, EXCEPTION_RECORD *record, PVOID return_value) {
  CONTEXT context;
  RtlUnwindEx(target_frame, target_ip, record, return_value, &context, NULL);
}

EXPORT USHORT NTAPI RtlCaptureStackBackTrace(ULONG skip, ULONG count, PVOID *frames, PULONG hash) {
  CONTEXT context;
  DISPATCHER_CONTEXT dc;
  RtlCaptureContext(&context);
  ULONG got = 0, seen = 0;
  if (hash) *hash = 0;
  for (unsigned depth = 0; depth < MAX_FRAMES && context.Rip != 0 && got < count; ++depth) {
    unwind_step(UNW_FLAG_NHANDLER, &dc, &context);
    if (!stack_contains(dc.EstablisherFrame)) break;
    if (seen++ < skip + 1) continue;
    frames[got++] = (PVOID)context.Rip;
    if (hash) *hash += (ULONG)context.Rip;
  }
  return (USHORT)got;
}

/* ---- the C language handler (__try/__except/__finally of Microsoft and clang-style code) ------------------------------------------------------------- */

typedef struct { ULONG BeginAddress, EndAddress, HandlerAddress, JumpTarget; } C_SCOPE_RECORD;
typedef struct { ULONG Count; C_SCOPE_RECORD ScopeRecord[1]; } C_SCOPE_TABLE;
typedef LONG (*C_FILTER)(EXCEPTION_POINTERS *, PVOID);
typedef void (*C_TERMINATION)(BOOLEAN, PVOID);

EXPORT EXCEPTION_DISPOSITION __C_specific_handler(EXCEPTION_RECORD *record, PVOID frame, CONTEXT *context, DISPATCHER_CONTEXT *dc) {
  const C_SCOPE_TABLE *table = (const C_SCOPE_TABLE *)dc->HandlerData;
  const ULONG64 base = dc->ImageBase;
  if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
    for (ULONG i = dc->ScopeIndex; i < table->Count; ++i) {
      const C_SCOPE_RECORD *s = &table->ScopeRecord[i];
      if (dc->ControlPc < base + s->BeginAddress || dc->ControlPc >= base + s->EndAddress) continue;
      if (s->JumpTarget) continue;      /* an __except scope has nothing to run while unwinding */
      if ((record->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && dc->TargetIp >= base + s->BeginAddress && dc->TargetIp < base + s->EndAddress) return ExceptionContinueSearch;
      dc->ScopeIndex = i + 1;
      ((C_TERMINATION)(base + s->HandlerAddress))(TRUE, frame);
    }
    return ExceptionContinueSearch;
  }
  for (ULONG i = dc->ScopeIndex; i < table->Count; ++i) {
    const C_SCOPE_RECORD *s = &table->ScopeRecord[i];
    if (dc->ControlPc < base + s->BeginAddress || dc->ControlPc >= base + s->EndAddress) continue;
    if (!s->JumpTarget) continue;      /* a __finally scope */
    if (s->HandlerAddress != EXCEPTION_EXECUTE_HANDLER) {
      EXCEPTION_POINTERS pointers = {record, context};
      dc->ScopeIndex = i + 1;      /* an exception raised in the filter goes on with the next scope */
      const LONG verdict = ((C_FILTER)(base + s->HandlerAddress))(&pointers, frame);
      if (verdict == EXCEPTION_CONTINUE_SEARCH) continue;
      if (verdict == EXCEPTION_CONTINUE_EXECUTION) return ExceptionContinueExecution;
    }
    RtlUnwindEx(frame, (PVOID)(base + s->JumpTarget), record, (PVOID)0, dc->ContextRecord, dc->HistoryTable);
  }
  return ExceptionContinueSearch;
}
