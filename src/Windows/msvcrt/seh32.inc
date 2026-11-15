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

/* msvcrt.dll for i386: the handlers of the Microsoft compilers' __try blocks. Included by msvcrt.c when it is built for i386.
 *
 * A function with __try links this frame at FS:[0] (its registration record is the first two fields), with _except_handler3 or, with stack cookies,
 * _except_handler4_common as its handler:
 *     [ebp-0x18] the stack pointer to go back to    [ebp-0x14] EXCEPTION_POINTERS* for GetExceptionInformation()
 *     [ebp-0x10] previous record   [ebp-0x0C] handler   [ebp-0x08] scope table   [ebp-0x04] try level   [ebp] the caller's ebp
 * The scope table has one entry per __try: the enclosing level, the filter (NULL for __try/__finally) and the __except or __finally block. The blocks
 * are pieces of the function: they run with the function's ebp. */

IMPORT void NTAPI RtlUnwind(PVOID target_frame, PVOID target_ip, EXCEPTION_RECORD *record, PVOID return_value);

typedef struct { int previous_level; void *filter; void *handler; } SehScope;
typedef struct {
  EXCEPTION_REGISTRATION_RECORD *prev;
  void *handler;
  SehScope *scopes;
  int level;
  int ebp;      /* (the address of this field is the function's ebp) */
} SehFrame;
typedef struct { int gs_cookie_offset; ULONG gs_cookie_xor; int eh_cookie_offset; ULONG eh_cookie_xor; SehScope entries[1]; } SehScopeTable4;
#define SEH_LEVEL_END (-1)
#define SEH4_LEVEL_END (-2)

/* A filter: int filter(void), run with ebp = the function's. A __finally block: void block(void), the same way. The last jumps into the function at
 * its __except block and does not come back. All keep nothing of the registers, so the ones a C function keeps are saved around the calls. */
int __cdecl seh_call_block(void *block, void *ebp);
__attribute__((noreturn)) void __cdecl seh_jump_block(void *block, void *ebp);
__asm__(
    ".text\n"
    "_seh_call_block:\n"
    "  pushl %ebp\n"
    "  pushl %ebx\n"
    "  pushl %esi\n"
    "  pushl %edi\n"
    "  movl 20(%esp), %eax\n"
    "  movl 24(%esp), %ebp\n"
    "  call *%eax\n"
    "  popl %edi\n"
    "  popl %esi\n"
    "  popl %ebx\n"
    "  popl %ebp\n"
    "  ret\n"
    "_seh_jump_block:\n"
    "  movl 4(%esp), %eax\n"
    "  movl 8(%esp), %ebp\n"
    "  jmp *%eax\n");

static inline EXCEPTION_REGISTRATION_RECORD *seh_chain_head(void) {
  EXCEPTION_REGISTRATION_RECORD *head;
  __asm__("movl %%fs:0, %0" : "=r"(head));
  return head;
}
static inline void seh_chain_set(EXCEPTION_REGISTRATION_RECORD *head) { __asm__ volatile("movl %0, %%fs:0" : : "r"(head) : "memory"); }

/* While the __finally blocks of a frame run, this record is on the list: an unwind that comes by (a __finally block that leaves through an exception)
 * is told to carry on from the frame whose blocks were running. */
typedef struct { EXCEPTION_REGISTRATION_RECORD record; SehFrame *frame; } SehUnwindGuard;
static int __cdecl seh_unwind_guard(EXCEPTION_RECORD *record, EXCEPTION_REGISTRATION_RECORD *guard, CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher) {
  (void)context;
  if (!(record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))) return ExceptionContinueSearch;
  *dispatcher = (EXCEPTION_REGISTRATION_RECORD *)((SehUnwindGuard *)guard)->frame;
  return ExceptionCollidedUnwind;
}

/* Runs the __finally blocks of the levels from the frame's current one down to (not including) `stop`. */
static void seh_local_unwind(SehFrame *frame, const SehScope *scopes, int stop, int end) {
  volatile SehUnwindGuard guard;
  guard.frame = frame;
  guard.record.Handler = (void *)seh_unwind_guard;
  guard.record.Prev = seh_chain_head();
  seh_chain_set((EXCEPTION_REGISTRATION_RECORD *)&guard.record);
  while (frame->level != end && frame->level != stop) {
    const SehScope *scope = &scopes[frame->level];
    frame->level = scope->previous_level;
    if (!scope->filter) seh_call_block(scope->handler, &frame->ebp);
  }
  seh_chain_set(guard.record.Prev);
}

API void __cdecl _local_unwind2(SehFrame *frame, int stop) { seh_local_unwind(frame, frame->scopes, stop, SEH_LEVEL_END); }
API void __cdecl _local_unwind4(ULONG *cookie, SehFrame *frame, int stop) {
  seh_local_unwind(frame, ((const SehScopeTable4 *)((ULONG_PTR)frame->scopes ^ *cookie))->entries, stop, SEH4_LEVEL_END);
}
API void __cdecl _global_unwind2(EXCEPTION_REGISTRATION_RECORD *frame) { RtlUnwind(frame, NULL, NULL, NULL); }
/* For a __finally block: is it running because of an unwind (and not because the __try block ended)? */
API int __cdecl _abnormal_termination(void) {
  const EXCEPTION_REGISTRATION_RECORD *head = seh_chain_head();
  return head != EXCEPTION_CHAIN_END && head->Handler == (void *)seh_unwind_guard;
}

static int seh_dispatch(EXCEPTION_RECORD *record, SehFrame *frame, CONTEXT *context, const SehScope *scopes, int end, ULONG *cookie) {
  if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {      /* the frame goes away: all its __finally blocks */
    seh_local_unwind(frame, scopes, end, end);
    return ExceptionContinueSearch;
  }
  EXCEPTION_POINTERS pointers = {record, context};
  ((EXCEPTION_POINTERS **)frame)[-1] = &pointers;
  for (int level = frame->level; level != end; level = scopes[level].previous_level) {
    if (!scopes[level].filter) continue;
    const int answer = seh_call_block(scopes[level].filter, &frame->ebp);
    if (answer == EXCEPTION_CONTINUE_EXECUTION) return ExceptionContinueExecution;
    if (answer != EXCEPTION_EXECUTE_HANDLER) continue;
    /* This __except takes it: the frames above go, then the levels inside this __try, and the function goes on in its __except block. */
    _global_unwind2((EXCEPTION_REGISTRATION_RECORD *)frame);
    seh_local_unwind(frame, scopes, level, end);
    frame->level = scopes[level].previous_level;
    (void)cookie;
    seh_jump_block(scopes[level].handler, &frame->ebp);
  }
  return ExceptionContinueSearch;
}

API int __cdecl _except_handler3(EXCEPTION_RECORD *record, SehFrame *frame, CONTEXT *context, void *dispatcher) {
  (void)dispatcher;
  return seh_dispatch(record, frame, context, frame->scopes, SEH_LEVEL_END, NULL);
}
/* _except_handler2 is the older name of the same frame. */
API int __cdecl _except_handler2(EXCEPTION_RECORD *record, SehFrame *frame, CONTEXT *context, void *dispatcher) { return _except_handler3(record, frame, context, dispatcher); }
/* With stack cookies: the scope table's address is stored xor the module's cookie, and the table starts with where the frame's cookies are (which are
 * not checked here: a smashed frame is not detected, the handlers still run). */
API int __cdecl _except_handler4_common(ULONG *cookie, void (__fastcall *check_cookie)(ULONG), EXCEPTION_RECORD *record, SehFrame *frame, CONTEXT *context, void *dispatcher) {
  (void)check_cookie; (void)dispatcher;
  const SehScopeTable4 *table = (const SehScopeTable4 *)((ULONG_PTR)frame->scopes ^ *cookie);
  return seh_dispatch(record, frame, context, table->entries, SEH4_LEVEL_END, cookie);
}
