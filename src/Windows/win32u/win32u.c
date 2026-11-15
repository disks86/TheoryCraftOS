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

/* win32u.dll: the system-call stubs of win32k (the NtUser* and NtGdi* services that user32.dll and gdi32.dll call), numbered from NT_SERVICE_WIN32K_BASE (see
 * src/Windows/tcos_syscalls.h). The stubs are generated: win32u/services.inc.
 *
 * The kernel cannot run user code in the middle of a service, so the services that may have to call a window procedure or a hook (creating a window, sending a message,
 * getting a message that a sent message is waiting before, SetWindowPos, ...) are wrapped here. See docs/win32k/User.md for the protocol:
 *   - the stub of such a service keeps its code but is called NtUserXxx_raw (not exported);
 *   - the exported NtUserXxx calls it. If it answers TCOS_UPCALL_DUE, the thread has window procedure / hook calls to make: drain_upcalls() fetches each with
 *     NtUserTcosGetUpcall, makes it and reports the result with NtUserTcosUpcallDone; then NtUserTcosResume continues the operation of the service (which may answer
 *     TCOS_UPCALL_DUE again). If it answers TCOS_DRAIN_RETRY (the message retrieval services), the calls are made and the service is called again with the same arguments. */
#include "../tcos_syscalls.h"

#define _STR(x) _STR2(x)
#define _STR2(x) #x

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");      /* (the stack probe of the compiler: the stack is committed) */

#ifdef __i386__
/* The library of 32-bit programs (docs/Wow64.md). A parameter is 32 bits here and 64 bits in the kernel, which has one set of services, written for
 * 64-bit programs: a service is called through INT 2E with every argument widened (see the stubs below), and the structures the kernel reads and
 * writes keep their 64-bit layouts - the libraries that call these services (user32.dll, gdi32.dll) hand them over that way. What a window
 * procedure is given is the exception: it is a 32-bit program's code, so the structures of its messages are converted here, around the call. */
typedef unsigned long U;           /* every parameter and result of a service is one register */
#define PROC_CALL __stdcall        /* (window procedures, hooks and timer procedures take their arguments off the stack) */
#else
typedef unsigned long long U;      /* every parameter and result of a service is one register */
#define PROC_CALL
#endif
typedef unsigned long long U64;
typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;

#define TCOS_UPCALL_DUE 0x7FFFFFF1ull
#define TCOS_DRAIN_RETRY 0x7FFFFFF3ull

/* ---- the stubs --------------------------------------------------------------------------------------------------------------------------------------------------- */
#ifdef __i386__
#include "../tcos_wow64_win32k.h"
WOW_WIN32K_CODE(_wow_win32k_service, "");
#define NT_SERVICE_ASM(label, number) WOW_WIN32K_STUB(label, number, _wow_win32k_service)
#else
#define NT_SERVICE_ASM(label, number)                                                                                                                          \
  __asm__(".text\n.globl " #label "\n" #label ":\n mov %rcx, %r10\n mov $" _STR(number) ", %eax\n syscall\n ret\n")
#endif
#define NT_SERVICE_EXPORT(name) __asm__(".section .drectve\n.ascii \" -export:" #name "\"\n.text\n")

/* The services that have a wrapper below: WRAPPED_<name> expands to "~," which the macros recognise. */
#define WRAPPED_NtUserCreateWindowEx ~,
#define WRAPPED_NtUserDestroyWindow ~,
#define WRAPPED_NtUserSetParent ~,
#define WRAPPED_NtUserEnableWindow ~,
#define WRAPPED_NtUserSetWindowLong ~,
#define WRAPPED_NtUserSetWindowLongPtr ~,
#define WRAPPED_NtUserSetWindowWord ~,
#define WRAPPED_NtUserSetWindowPos ~,
#define WRAPPED_NtUserMoveWindow ~,
#define WRAPPED_NtUserShowWindow ~,
#define WRAPPED_NtUserShowWindowAsync ~,
#define WRAPPED_NtUserEndDeferWindowPosEx ~,
#define WRAPPED_NtUserSetWindowPlacement ~,
#define WRAPPED_NtUserBeginPaint ~,
#define WRAPPED_NtUserGetUpdateRect ~,
#define WRAPPED_NtUserGetUpdateRgn ~,
#define WRAPPED_NtUserRedrawWindow ~,
#define WRAPPED_NtUserUpdateWindow ~,
#define WRAPPED_NtUserSetFocus ~,
#define WRAPPED_NtUserSetActiveWindow ~,
#define WRAPPED_NtUserSetForegroundWindow ~,
#define WRAPPED_NtUserSetCapture ~,
#define WRAPPED_NtUserReleaseCapture ~,
#define WRAPPED_NtUserMessageCall ~,
#define WRAPPED_NtUserDispatchMessage ~,
#define WRAPPED_NtUserGetMessage ~,
#define WRAPPED_NtUserPeekMessage ~,
#define WRAPPED_NtUserWaitMessage ~,
#define WRAPPED_NtUserMsgWaitForMultipleObjectsEx ~,
#define WRAPPED_NtUserCallHwnd ~,
#define WRAPPED_NtUserCallNextHookEx ~,
#define WRAPPED_NtUserCallMsgFilter ~,

#ifdef __i386__      /* (the services whose structures are converted for a 32-bit program: wow64.inc) */
#define WRAPPED_NtUserRegisterClassExWOW ~,
#define WRAPPED_NtUserGetClassInfoEx ~,
#define WRAPPED_NtUserUnregisterClass ~,
#define WRAPPED_NtUserGetClassName ~,
#define WRAPPED_NtUserRegisterWindowMessage ~,
#define WRAPPED_NtUserGetAtomName ~,
#define WRAPPED_NtUserFindWindowEx ~,
#define WRAPPED_NtUserTranslateMessage ~,
#define WRAPPED_NtUserCallHwndParam ~,
#define WRAPPED_NtUserBuildHwndList ~,
#define WRAPPED_NtUserEndPaint ~,
#define WRAPPED_NtUserGetGUIThreadInfo ~,
#endif

#define SECOND_OF(a, b, ...) b
#define IS_PROBE(...) SECOND_OF(__VA_ARGS__, 0, )
#define PASTE(a, b) PASTE2(a, b)
#define PASTE2(a, b) a##b
#define PROBE_TEST(name) IS_PROBE(WRAPPED_##name 1)
#define NT_SERVICE_0(name) NT_SERVICE_ASM(name, NT_SERVICE_##name); NT_SERVICE_EXPORT(name)
#define NT_SERVICE_1(name) NT_SERVICE_ASM(name##_raw, NT_SERVICE_##name)
#define NT_SERVICE(name) PASTE(NT_SERVICE_, PROBE_TEST(name))(name)

#include "services.inc"

/* ---- the upcalls ------------------------------------------------------------------------------------------------------------------------------------------------- */
extern U NtUserTcosGetUpcall(U record);
extern U NtUserTcosUpcallDone(U id, U result, U payload, U payload_length);
extern U NtUserTcosResume(void);

typedef struct {
  U64 id;
  U32 kind, flags;
  U64 proc, hwnd;
  U32 msg, payload_len;
  U64 wparam, lparam, extra[3];
  U16 fix_at, fix_to;
  U32 reserved;
  U8 payload[4096];
} __attribute__((aligned(16))) TcosUpcall;

#define UPF_CREATESTRUCT 1
#define UPF_PAYLOAD_LPARAM 2
#define KIND_WNDPROC 1
#define KIND_HOOK 2
#define KIND_WINEVENT 3
#define KIND_SENDASYNC 4
#define KIND_TIMER 5

/* The pointers a CREATESTRUCT needs that the kernel does not have: the window name and the class name of the CreateWindowEx call of this thread. */
static struct { volatile U64 tid; U64 name, cls; } g_create[32];

static U64 current_thread_id(void) {
#ifdef __i386__
  U32 tid;
  __asm__ volatile("movl %%fs:0x24, %0" : "=r"(tid));     /* TEB.ClientId.UniqueThread of the 32-bit TEB */
#else
  U64 tid;
  __asm__ volatile("mov %%gs:0x48, %0" : "=r"(tid));      /* TEB.ClientId.UniqueThread */
#endif
  return tid;
}
static int create_context_enter(U64 name, U64 cls) {
  const U64 tid = current_thread_id();
  for (int i = 0; i < 32; ++i) {
    U64 expected = 0;
    if (__atomic_compare_exchange_n(&g_create[i].tid, &expected, tid, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      g_create[i].name = name;
      g_create[i].cls = cls;
      return i;
    }
  }
  return -1;
}
static void create_context_leave(int slot) {
  if (slot >= 0) __atomic_store_n(&g_create[slot].tid, 0, __ATOMIC_RELEASE);
}
static int create_context_find(U64* name, U64* cls) {
  const U64 tid = current_thread_id();
  for (int i = 0; i < 32; ++i) {
    if (__atomic_load_n(&g_create[i].tid, __ATOMIC_ACQUIRE) == tid) { *name = g_create[i].name; *cls = g_create[i].cls; return 1; }
  }
  return 0;
}

#ifdef __i386__
/* ---- the structures of a window procedure's messages, for a 32-bit program -------------------------------------------------------------------------------------
 * The kernel hands over the structure a message's lparam points to in its 64-bit layout (the payload of the upcall). A 32-bit window procedure gets
 * the 32-bit layout, made here, and what it changed goes back into the payload before the kernel takes it. Only the structures with handles or
 * pointers in them differ: CREATESTRUCT, WINDOWPOS, NCCALCSIZE_PARAMS, COPYDATASTRUCT. */
#define WM_CREATE_ 0x0001u
#define WM_WINDOWPOSCHANGING_ 0x0046u
#define WM_WINDOWPOSCHANGED_ 0x0047u
#define WM_COPYDATA_ 0x004Au
#define WM_NCCREATE_ 0x0081u
#define WM_NCCALCSIZE_ 0x0083u
typedef struct { U32 params, instance, menu, parent; int cy, cx, y, x; U32 style, name, cls, ex_style; } CreateStruct32;
typedef struct { U32 hwnd, after; int x, y, cx, cy; U32 flags; } WindowPos32;
typedef struct { int rects[12]; U32 pos; WindowPos32 window_pos; } NcCalcSize32;
typedef struct { U32 data, bytes, pointer; } CopyData32;
typedef union { CreateStruct32 create; WindowPos32 pos; NcCalcSize32 calc; CopyData32 copy; } Message32;

static U64 get64(const U8* at) { U64 v; __builtin_memcpy(&v, at, 8); return v; }
static U32 get32(const U8* at) { U32 v; __builtin_memcpy(&v, at, 4); return v; }
static void put32(U8* at, U32 v) { __builtin_memcpy(at, &v, 4); }
static void put64s(U8* at, U32 v) { const long long wide = (int)v; __builtin_memcpy(at, &wide, 8); }      /* (a handle: with its sign) */

static void window_pos_to_32(WindowPos32* out, const U8* in) {
  out->hwnd = (U32)get64(in); out->after = (U32)get64(in + 8);
  out->x = (int)get32(in + 16); out->y = (int)get32(in + 20); out->cx = (int)get32(in + 24); out->cy = (int)get32(in + 28); out->flags = get32(in + 32);
}
static void window_pos_from_32(U8* out, const WindowPos32* in) {
  put64s(out, in->hwnd); put64s(out + 8, in->after);
  put32(out + 16, (U32)in->x); put32(out + 20, (U32)in->y); put32(out + 24, (U32)in->cx); put32(out + 28, (U32)in->cy); put32(out + 32, in->flags);
}
/* The 32-bit form of the upcall's payload in `m`, if the message has one; 0 if the payload is passed as it is. */
static int message_to_32(const TcosUpcall* u, Message32* m) {
  const U8* p = u->payload;
  if ((u->flags & UPF_CREATESTRUCT) && u->payload_len >= 80) {
    m->create.params = (U32)get64(p); m->create.instance = (U32)get64(p + 8); m->create.menu = (U32)get64(p + 16); m->create.parent = (U32)get64(p + 24);
    m->create.cy = (int)get32(p + 32); m->create.cx = (int)get32(p + 36); m->create.y = (int)get32(p + 40); m->create.x = (int)get32(p + 44);
    m->create.style = get32(p + 48); m->create.name = (U32)get64(p + 56); m->create.cls = (U32)get64(p + 64); m->create.ex_style = get32(p + 72);
    return 1;
  }
  if (u->kind != KIND_WNDPROC) return 0;
  if ((u->msg == WM_WINDOWPOSCHANGING_ || u->msg == WM_WINDOWPOSCHANGED_) && u->payload_len >= 40) { window_pos_to_32(&m->pos, p); return 1; }
  if (u->msg == WM_NCCALCSIZE_ && u->wparam && u->payload_len >= 104) {
    __builtin_memcpy(m->calc.rects, p, 48);
    m->calc.pos = (U32)&m->calc.window_pos;
    window_pos_to_32(&m->calc.window_pos, p + 56);
    return 1;
  }
  if (u->msg == WM_COPYDATA_ && u->payload_len >= 24) {
    m->copy.data = (U32)get64(p); m->copy.bytes = get32(p + 8); m->copy.pointer = (U32)(p + 24);
    return 1;
  }
  return 0;
}
/* What the procedure changed, back in the payload. */
static void message_from_32(TcosUpcall* u, const Message32* m) {
  U8* p = u->payload;
  if (u->flags & UPF_CREATESTRUCT) {
    put32(p + 32, (U32)m->create.cy); put32(p + 36, (U32)m->create.cx); put32(p + 40, (U32)m->create.y); put32(p + 44, (U32)m->create.x);
  } else if (u->msg == WM_WINDOWPOSCHANGING_ || u->msg == WM_WINDOWPOSCHANGED_) {
    window_pos_from_32(p, &m->pos);
  } else if (u->msg == WM_NCCALCSIZE_) {
    __builtin_memcpy(p, m->calc.rects, 48);
    window_pos_from_32(p + 56, &m->calc.window_pos);
  }
}
#endif

static void drain_upcalls(void) {
  TcosUpcall u;
  while ((U32)NtUserTcosGetUpcall((U)&u)) {
    U64 lparam = u.lparam, result = 0;
    if (u.flags & UPF_PAYLOAD_LPARAM) {
      if (u.fix_at != 0xFFFF && u.fix_at + 8u <= sizeof(u.payload) && u.fix_to < sizeof(u.payload)) *(U64*)(u.payload + u.fix_at) = (U64)(U)(u.payload + u.fix_to);
      lparam = (U64)(U)u.payload;
    }
    if ((u.flags & UPF_CREATESTRUCT) && u.payload_len >= 80) {      /* CREATESTRUCTW: lpszName at 56, lpszClass at 64 */
      U64 name = 0, cls = 0;
      if (create_context_find(&name, &cls)) { *(U64*)(u.payload + 56) = name; *(U64*)(u.payload + 64) = cls; }
    }
#ifdef __i386__
    Message32 message32;
    const int converted = (u.flags & UPF_PAYLOAD_LPARAM) && message_to_32(&u, &message32);
    if (converted) lparam = (U64)(U)&message32;
#endif
    switch (u.kind) {
      case KIND_WNDPROC:
        result = ((U (PROC_CALL *)(U, U32, U, U))(U)u.proc)((U)u.hwnd, u.msg, (U)u.wparam, (U)lparam);
        break;
      case KIND_HOOK:      /* HOOKPROC(code, wparam, lparam); the "hwnd" of the record is the hook handle */
        result = ((U (PROC_CALL *)(int, U, U))(U)u.proc)((int)u.msg, (U)u.wparam, (U)lparam);
        break;
      case KIND_WINEVENT:      /* WINEVENTPROC(hook, event, hwnd, object id, child id, thread, time) */
        ((void (PROC_CALL *)(U, U32, U, int, int, U32, U32))(U)u.proc)((U)u.wparam, u.msg, (U)u.hwnd, (int)u.lparam, (int)u.extra[0], (U32)u.extra[1], (U32)u.extra[2]);
        break;
      case KIND_TIMER:      /* TIMERPROC(hwnd, WM_TIMER, id, tick) */
        ((void (PROC_CALL *)(U, U32, U, U32))(U)u.proc)((U)u.hwnd, u.msg, (U)u.wparam, (U32)u.lparam);
        break;
      default:
        break;
    }
#ifdef __i386__
    if (converted) message_from_32(&u, &message32);
    result = (U64)(long long)(int)(U32)result;      /* (an LRESULT: with its sign) */
#endif
    NtUserTcosUpcallDone((U)u.id, (U)result, (U)u.payload, u.payload_len);
  }
}

/* The wrapper of a service with parameters `params` (arguments `args`): the exported function. */
#define WRAP(name, params, args)                                                                                                                              \
  extern U name##_raw params;                                                                                                                                  \
  __attribute__((dllexport)) U name params {                                                                                                                   \
    U r = name##_raw args;                                                                                                                                     \
    for (;;) {                                                                                                                                                 \
      if (r == TCOS_UPCALL_DUE) { drain_upcalls(); r = NtUserTcosResume(); }                                                                                   \
      else if (r == TCOS_DRAIN_RETRY) { drain_upcalls(); r = name##_raw args; }                                                                                \
      else return r;                                                                                                                                           \
    }                                                                                                                                                          \
  }

#define P1 (U a)
#define A1 (a)
#define P2 (U a, U b)
#define A2 (a, b)
#define P3 (U a, U b, U c)
#define A3 (a, b, c)
#define P4 (U a, U b, U c, U d)
#define A4 (a, b, c, d)
#define P5 (U a, U b, U c, U d, U e)
#define A5 (a, b, c, d, e)
#define P6 (U a, U b, U c, U d, U e, U f)
#define A6 (a, b, c, d, e, f)
#define P7 (U a, U b, U c, U d, U e, U f, U g)
#define A7 (a, b, c, d, e, f, g)
#define P10 (U a, U b, U c, U d, U e, U f, U g, U h, U i, U j)

WRAP(NtUserDestroyWindow, P1, A1)
WRAP(NtUserSetParent, P2, A2)
WRAP(NtUserEnableWindow, P2, A2)
WRAP(NtUserSetWindowLong, P4, A4)
WRAP(NtUserSetWindowLongPtr, P4, A4)
WRAP(NtUserSetWindowWord, P3, A3)
WRAP(NtUserSetWindowPos, P7, A7)
WRAP(NtUserMoveWindow, P6, A6)
WRAP(NtUserShowWindow, P2, A2)
WRAP(NtUserShowWindowAsync, P2, A2)
WRAP(NtUserEndDeferWindowPosEx, P2, A2)
WRAP(NtUserSetWindowPlacement, P2, A2)
#ifndef __i386__
WRAP(NtUserBeginPaint, P2, A2)
#endif
WRAP(NtUserGetUpdateRect, P3, A3)
WRAP(NtUserGetUpdateRgn, P3, A3)
WRAP(NtUserRedrawWindow, P4, A4)
WRAP(NtUserUpdateWindow, P1, A1)
WRAP(NtUserSetFocus, P1, A1)
WRAP(NtUserSetActiveWindow, P1, A1)
WRAP(NtUserSetForegroundWindow, P1, A1)
WRAP(NtUserSetCapture, P1, A1)
#ifndef __i386__
WRAP(NtUserMessageCall, P7, A7)
#endif
#ifndef __i386__
WRAP(NtUserDispatchMessage, P1, A1)
#endif
#ifndef __i386__
WRAP(NtUserGetMessage, P4, A4)
#endif
#ifndef __i386__
WRAP(NtUserPeekMessage, P5, A5)
#endif
WRAP(NtUserCallHwnd, P2, A2)
WRAP(NtUserCallNextHookEx, P4, A4)
#ifndef __i386__
WRAP(NtUserCallMsgFilter, P2, A2)
#endif

/* NtUserReleaseCapture() and NtUserWaitMessage() take no arguments */
extern U NtUserReleaseCapture_raw(void);
__attribute__((dllexport)) U NtUserReleaseCapture(void) {
  U r = NtUserReleaseCapture_raw();
  while (r == TCOS_UPCALL_DUE) { drain_upcalls(); r = NtUserTcosResume(); }
  return r;
}
extern U NtUserWaitMessage_raw(void);
__attribute__((dllexport)) U NtUserWaitMessage(void) {
  U r = NtUserWaitMessage_raw();
  while (r == TCOS_DRAIN_RETRY) { drain_upcalls(); r = NtUserWaitMessage_raw(); }
  return r;
}

#ifdef __i386__
#include "wow64.inc"
#else
/* NtUserCreateWindowEx has 17 parameters; the class name and the title are remembered for the creation messages. */
extern U NtUserCreateWindowEx_raw(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserCreateWindowEx(U ex_style, U class_name, U version, U window_name, U style, U x, U y, U cx, U cy, U parent, U menu, U instance, U params, U flags,
                                                  U client_instance, U cls, U ansi) {
  U64 title = 0;
  if (window_name) title = *(U64*)(U)(window_name + 8);      /* UNICODE_STRING.Buffer */
  const int slot = create_context_enter(title, cls);
  U r = NtUserCreateWindowEx_raw(ex_style, class_name, version, window_name, style, x, y, cx, cy, parent, menu, instance, params, flags, client_instance, cls, ansi);
  while (r == TCOS_UPCALL_DUE) { drain_upcalls(); r = NtUserTcosResume(); }
  create_context_leave(slot);
  return r;
}
#endif

/* ---- waiting for objects and input --------------------------------------------------------------------------------------------------------------------------------- */
extern U NtUserMsgWaitForMultipleObjectsEx_raw(U, U, U, U, U);

#ifdef __i386__
/* (the kernel reads 64-bit handles and 64-bit arguments: see the stubs above) */
static long nt_wait_for_multiple_objects(unsigned count, const void* handles, unsigned wait_type, unsigned alertable, const long long* timeout) {
  long long wide[64];
  if (count > 64) return (long)0xC00000EFu;      /* STATUS_INVALID_PARAMETER_1 */
  for (unsigned i = 0; i < count; ++i) wide[i] = ((const int*)handles)[i];
  const U64 arguments[5] = {count, (U)wide, wait_type, alertable, (U)timeout};
  long status;
  __asm__ volatile("int $0x2e" : "=a"(status) : "a"(NT_SERVICE_NtWaitForMultipleObjects), "d"(arguments) : "memory", "cc");
  return status;
}
#else
static long nt_wait_for_multiple_objects(unsigned count, const void* handles, unsigned wait_type, unsigned alertable, const long long* timeout) {
  long status;
  __asm__ volatile(
      "sub $0x38, %%rsp\n"
      "mov %[timeout], 0x28(%%rsp)\n"
      "mov %[count], %%r10d\n"      /* Count */
      "mov %[handles], %%rdx\n"
      "mov %[type], %%r8d\n"
      "mov %[alert], %%r9d\n"
      "mov $" _STR(NT_SERVICE_NtWaitForMultipleObjects) ", %%eax\n"
      "syscall\n"
      "add $0x38, %%rsp\n"
      : "=a"(status)
      : [count] "r"(count), [handles] "r"(handles), [type] "r"(wait_type), [alert] "r"(alertable), [timeout] "r"(timeout)
      : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
  return status;
}
#endif

/* MsgWaitForMultipleObjectsEx: the kernel half waits for input and time; the objects are polled between slices of the wait (10 ms at most), so they are noticed within
 * that time. Answers WAIT_OBJECT_0 + n for object n, WAIT_OBJECT_0 + count for input, WAIT_TIMEOUT. */
__attribute__((dllexport)) U NtUserMsgWaitForMultipleObjectsEx(U count, U handles, U timeout, U mask, U flags) {
  if (count == 0) {
    U r = NtUserMsgWaitForMultipleObjectsEx_raw(0, 0, timeout, mask, flags);
    for (;;) {
      if (r == TCOS_UPCALL_DUE) { drain_upcalls(); r = NtUserTcosResume(); }
      else if (r == TCOS_DRAIN_RETRY) { drain_upcalls(); r = NtUserMsgWaitForMultipleObjectsEx_raw(0, 0, timeout, mask, flags); }
      else return r;
    }
  }
  U32 remaining = (U32)timeout;
  for (;;) {
    const long long zero = 0;
    const long status = nt_wait_for_multiple_objects((unsigned)count, (const void*)handles, (flags & 1) ? 0 : 1, (flags & 2) ? 1 : 0, &zero);
    if ((unsigned long)status < count || ((unsigned long)status >= 0x80 && (unsigned long)status < 0x80 + count)) return (U)(unsigned long)status;      /* signalled / abandoned */
    U32 slice = remaining < 10 ? remaining : 10;
    if (remaining == 0xFFFFFFFFu) slice = 10;
    U r = NtUserMsgWaitForMultipleObjectsEx_raw(0, 0, slice, mask, flags);
    for (;;) {
      if (r == TCOS_UPCALL_DUE) { drain_upcalls(); r = NtUserTcosResume(); }
      else if (r == TCOS_DRAIN_RETRY) { drain_upcalls(); r = NtUserMsgWaitForMultipleObjectsEx_raw(0, 0, slice, mask, flags); }
      else break;
    }
    if (r != 0x102) return r == 0 ? count : (r == 0xFFFFFFFFu ? r : count);      /* input is there (the kernel answers the count it was given: 0) */
    if (remaining != 0xFFFFFFFFu) {
      if (remaining <= slice) return 0x102;
      remaining -= slice;
    }
  }
}

int PROC_CALL DllMain(void* module, unsigned reason, void* reserved) {
  (void)module;
  (void)reason;
  (void)reserved;
  return 1;
}
