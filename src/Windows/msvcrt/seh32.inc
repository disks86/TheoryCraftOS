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

/* ---- what the old 32-bit Microsoft C++ runtime start-up and code import from msvcrt.dll -----------------------------------------------------------------------
 * A library built with Visual C++ 6 against msvcrt.dll (a setup program's plug-in, say) imports the global operator new and delete, the destructor of
 * type_info, and the flag for the Pentium's division bug. They are exported under the compiler's names by msvcrt32.def. */
void *__cdecl msvcrt_operator_new(size_t_ bytes) { return malloc(bytes ? bytes : 1); }
void __cdecl msvcrt_operator_delete(void *memory) { free(memory); }
int msvcrt_adjust_fdiv = 0;      /* (no processor this runs on has that bug) */
/* type_info::~type_info(): `this` in ecx, nothing on the stack; a type_info here owns nothing. */
__attribute__((naked)) void msvcrt_type_info_destroy(void) { __asm__ volatile("ret"); }

/* ---- C++ exceptions of the 32-bit Microsoft compiler ----------------------------------------------------------------------------------------------------------------
 * `throw` is _CxxThrowException(object, throw information): the exception 0xE06D7363 with three parameters - a magic number, the thrown object and the
 * description of its type (the types it can be caught as, how to copy and destroy it). A function with try blocks or objects to destroy links a frame
 *     [ebp-12] previous record   [ebp-8] handler   [ebp-4] state ("try level")   [ebp] the caller's ebp        ([ebp-16]: the stack pointer to go back to)
 * whose handler loads eax with the function's description and jumps to __CxxFrameHandler. That looks for a catch block of the state the function was
 * in that takes the object's type; if there is one, the frames above are unwound, the objects of the try block destroyed, the catch block called
 * (it is a piece of the function, run with the function's ebp, and returns where the function goes on), and the thrown object destroyed. On an
 * unwind the handler destroys the function's objects.
 * Not done: exception specifications, objects thrown with virtual base classes caught by value through a copy constructor that takes the "is most
 * derived" flag, and std::exception_ptr. */
#define CXX_EXCEPTION 0xE06D7363u
#define CXX_MAGIC_VC6 0x19930520u
#define CXX_MAGIC_VC8 0x19930522u
typedef struct { void *vtable; char *name; char mangled[1]; } CxxTypeInfo;
typedef struct { int this_offset, vbase_descr, vbase_offset; } CxxThisOffsets;
typedef struct { UINT flags; const CxxTypeInfo *type_info; CxxThisOffsets offsets; UINT size; void *copy_constructor; } CxxType;      /* flags: 1 a simple type, 4 has virtual bases */
typedef struct { UINT count; const CxxType *types[1]; } CxxTypeTable;
typedef struct { UINT flags; void *destructor; void *custom_handler; const CxxTypeTable *types; } CxxThrowInfo;      /* flags: 1 const, 2 volatile */
typedef struct { int previous; void *handler; } CxxUnwind;
typedef struct { UINT flags; const CxxTypeInfo *type_info; int offset; void *handler; } CxxCatch;      /* flags: 1 const, 2 volatile, 8 by reference */
typedef struct { int start_level, end_level, catch_level, catch_count; const CxxCatch *catches; } CxxTry;
typedef struct { UINT magic, unwind_count; const CxxUnwind *unwinds; UINT try_count; const CxxTry *tries; UINT ip_count; const void *ip_map; const void *expected; UINT flags; } CxxFunction;
typedef struct { EXCEPTION_REGISTRATION_RECORD record; int level; int ebp; } CxxFrame;

IMPORT void WINAPI RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments);
API void abort(void);

/* The exception a catch block of this thread is handling, for `throw;` (the innermost one): kept in a thread-local slot made at the first use. */
IMPORT DWORD WINAPI TlsAlloc(void);
IMPORT BOOL WINAPI TlsFree(DWORD slot);
IMPORT PVOID WINAPI TlsGetValue(DWORD slot);
IMPORT BOOL WINAPI TlsSetValue(DWORD slot, PVOID value);
static volatile LONG g_cxx_slot = -1;
static DWORD cxx_slot(void) {
  LONG slot = __atomic_load_n(&g_cxx_slot, __ATOMIC_ACQUIRE);
  if (slot >= 0) return (DWORD)slot;
  const DWORD fresh = TlsAlloc();
  if (fresh == 0xFFFFFFFFu) return fresh;
  LONG expected = -1;
  if (__atomic_compare_exchange_n(&g_cxx_slot, &expected, (LONG)fresh, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
  TlsFree(fresh);      /* (another thread made it first) */
  return (DWORD)expected;
}
static EXCEPTION_RECORD *cxx_current(void) { const DWORD slot = cxx_slot(); return slot == 0xFFFFFFFFu ? NULL : (EXCEPTION_RECORD *)TlsGetValue(slot); }
static void cxx_set_current(EXCEPTION_RECORD *record) { const DWORD slot = cxx_slot(); if (slot != 0xFFFFFFFFu) TlsSetValue(slot, record); }

API void WINAPI _CxxThrowException(void *object, const CxxThrowInfo *info) {
  ULONG_PTR arguments[3] = {CXX_MAGIC_VC6, (ULONG_PTR)object, (ULONG_PTR)info};
  const EXCEPTION_RECORD *handling = !object && !info ? cxx_current() : NULL;
  if (handling) {      /* `throw;` in a catch block: the exception it is handling, again */
    for (int i = 0; i < 3; ++i) arguments[i] = handling->ExceptionInformation[i];
  }
  RaiseException(CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 3, arguments);
  for (;;) abort();
}

/* A member function of the thrown object's class (a destructor, a copy constructor): `this` in ecx, the arguments on the stack, which it takes off. */
void __cdecl seh_call_member0(void *function, void *object);
void __cdecl seh_call_member1(void *function, void *object, void *argument);
/* The end of a catch block: the stack pointer the function kept at [ebp-16], its ebp, and on at `address`. */
__attribute__((noreturn)) void __cdecl seh_continue_after_catch(CxxFrame *frame, void *address);
__asm__(
    ".text\n"
    "_seh_call_member0:\n"
    "  movl 8(%esp), %ecx\n"
    "  jmp *4(%esp)\n"
    "_seh_call_member1:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  movl 12(%ebp), %ecx\n"
    "  pushl 16(%ebp)\n"
    "  call *8(%ebp)\n"
    "  movl %ebp, %esp\n"
    "  popl %ebp\n"
    "  ret\n"
    "_seh_continue_after_catch:\n"
    "  movl 4(%esp), %edx\n"
    "  movl 8(%esp), %eax\n"
    "  movl -4(%edx), %esp\n"
    "  leal 12(%edx), %ebp\n"
    "  jmp *%eax\n");

static int cxx_same_text(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }

/* The object as the base class `offsets` describes (a base class is somewhere inside the object, and may be found through a table of virtual bases). */
static void *cxx_this_pointer(const CxxThisOffsets *offsets, void *object) {
  if (!object) return NULL;
  if (offsets->vbase_descr >= 0) {
    object = (char *)object + offsets->vbase_descr;
    const int *table = *(const int **)object;
    object = (char *)object + *(const int *)((const char *)table + offsets->vbase_offset);
  }
  return (char *)object + offsets->this_offset;
}

/* The form of the thrown object a catch block for `wanted` takes, or NULL. */
static const CxxType *cxx_caught_type(const CxxThrowInfo *info, const CxxTypeInfo *wanted, UINT catch_flags) {
  for (UINT i = 0; info->types && i < info->types->count; ++i) {
    const CxxType *type = info->types->types[i];
    if (!wanted) return type;      /* catch (...) */
    if (wanted != type->type_info && !cxx_same_text(wanted->mangled, type->type_info->mangled)) continue;
    if ((info->flags & 1) && !(catch_flags & 1)) continue;      /* a const object needs a const catch */
    if ((info->flags & 2) && !(catch_flags & 2)) continue;
    return type;
  }
  return NULL;
}

/* The catch block's variable gets the object: a pointer to it for a reference, else a copy. */
static void cxx_copy_object(void *object, CxxFrame *frame, const CxxCatch *block, const CxxType *type) {
  if (!block->type_info || !block->type_info->mangled[0] || !block->offset) return;
  void **destination = (void **)((char *)&frame->ebp + block->offset);
  if (block->flags & 8) {
    *destination = cxx_this_pointer(&type->offsets, object);
  } else if (type->flags & 1) {
    memmove(destination, object, type->size);
    if (type->size == sizeof(void *)) *destination = cxx_this_pointer(&type->offsets, *destination);      /* (a pointer to a class: to the base it is caught as) */
  } else if (!type->copy_constructor) {
    memmove(destination, cxx_this_pointer(&type->offsets, object), type->size);
  } else {
    seh_call_member1(type->copy_constructor, destination, cxx_this_pointer(&type->offsets, object));
  }
}

/* Destroys the function's objects from its current state down to `last`. */
static void cxx_local_unwind(CxxFrame *frame, const CxxFunction *function, int last) {
  int level = frame->level;
  while (level != last) {
    if (level < 0 || (UINT)level >= function->unwind_count) abort();      /* (a state the function does not have: its frame is not what it should be) */
    const CxxUnwind *unwind = &function->unwinds[level];
    level = unwind->previous;
    frame->level = level;
    if (unwind->handler) seh_call_block(unwind->handler, &frame->ebp);
  }
  frame->level = last;
}

/* An exception while a catch block runs goes on to the frames below; this record only marks the place. */
static int __cdecl cxx_catch_guard(EXCEPTION_RECORD *record, EXCEPTION_REGISTRATION_RECORD *guard, CONTEXT *context, EXCEPTION_REGISTRATION_RECORD **dispatcher) {
  (void)record; (void)guard; (void)context; (void)dispatcher;
  return ExceptionContinueSearch;
}

int __cdecl seh_cxx_frame_handler(EXCEPTION_RECORD *record, CxxFrame *frame, CONTEXT *context, void *dispatcher, const CxxFunction *function) {
  (void)context; (void)dispatcher;
  if (function->magic < CXX_MAGIC_VC6 || function->magic > CXX_MAGIC_VC8) return ExceptionContinueSearch;
  if (function->magic >= CXX_MAGIC_VC8 && (function->flags & 1) && record->ExceptionCode != CXX_EXCEPTION) return ExceptionContinueSearch;      /* (built to take C++ exceptions only) */
  if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
    if (function->unwind_count) cxx_local_unwind(frame, function, -1);
    return ExceptionContinueSearch;
  }
  if (!function->try_count) return ExceptionContinueSearch;
  const CxxThrowInfo *info = NULL;
  void *object = NULL;
  if (record->ExceptionCode == CXX_EXCEPTION && record->NumberParameters == 3 && record->ExceptionInformation[0] >= CXX_MAGIC_VC6 &&
      record->ExceptionInformation[0] <= CXX_MAGIC_VC8) {
    object = (void *)record->ExceptionInformation[1];
    info = (const CxxThrowInfo *)record->ExceptionInformation[2];
  }
  const int level = frame->level;
  for (UINT i = 0; i < function->try_count; ++i) {
    const CxxTry *block = &function->tries[i];
    if (level < block->start_level || level > block->end_level) continue;
    for (int k = 0; k < block->catch_count; ++k) {
      const CxxCatch *handler = &block->catches[k];
      if (info) {
        const CxxType *type = cxx_caught_type(info, handler->type_info, handler->flags);
        if (!type) continue;
        cxx_copy_object(object, frame, handler, type);
      } else if (handler->type_info) {
        continue;      /* (not a C++ exception: only catch (...) takes it) */
      }
      /* This catch block takes it: the frames above go, then the objects of the try block, and the block runs. */
      RtlUnwind(frame, NULL, record, NULL);
      cxx_local_unwind(frame, function, block->start_level);
      frame->level = block->end_level + 1;
      volatile EXCEPTION_REGISTRATION_RECORD guard;
      EXCEPTION_RECORD *const outer = cxx_current();
      const int saved_esp = ((int *)frame)[-1];
      guard.Handler = (void *)cxx_catch_guard;
      guard.Prev = seh_chain_head();
      seh_chain_set((EXCEPTION_REGISTRATION_RECORD *)&guard);
      cxx_set_current(record);
      void *const go_on = (void *)(ULONG_PTR)seh_call_block(handler->handler, &frame->ebp);
      cxx_set_current(outer);
      seh_chain_set(guard.Prev);
      ((int *)frame)[-1] = saved_esp;
      if (info && info->destructor && object) seh_call_member0(info->destructor, object);
      seh_continue_after_catch(frame, go_on);
    }
  }
  return ExceptionContinueSearch;
}

/* The handler a function's frame names loads eax with the function's description and jumps here: the usual four arguments of a frame's handler are
 * on the stack. (__CxxFrameHandler2 and 3 are later compilers' names for the same thing.) */
__asm__(
    ".text\n"
    ".globl ___CxxFrameHandler\n___CxxFrameHandler:\n"
    ".globl ___CxxFrameHandler2\n___CxxFrameHandler2:\n"
    ".globl ___CxxFrameHandler3\n___CxxFrameHandler3:\n"
    "  pushl %eax\n"
    "  pushl 20(%esp)\n"
    "  pushl 20(%esp)\n"
    "  pushl 20(%esp)\n"
    "  pushl 20(%esp)\n"
    "  call _seh_cxx_frame_handler\n"
    "  addl $20, %esp\n"
    "  ret\n"
    ".section .drectve\n"
    ".ascii \" -export:__CxxFrameHandler -export:__CxxFrameHandler2 -export:__CxxFrameHandler3\"\n"
    ".text\n");
