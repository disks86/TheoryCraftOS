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

typedef unsigned long long U;      /* every parameter and result of a service is one register */
typedef unsigned long long U64;
typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;

#define TCOS_UPCALL_DUE 0x7FFFFFF1ull
#define TCOS_DRAIN_RETRY 0x7FFFFFF3ull

/* ---- the stubs --------------------------------------------------------------------------------------------------------------------------------------------------- */
#define NT_SERVICE_ASM(label, number)                                                                                                                          \
  __asm__(".text\n.globl " #label "\n" #label ":\n mov %rcx, %r10\n mov $" _STR(number) ", %eax\n syscall\n ret\n")
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
  U64 tid;
  __asm__ volatile("mov %%gs:0x48, %0" : "=r"(tid));      /* TEB.ClientId.UniqueThread */
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

static void drain_upcalls(void) {
  TcosUpcall u;
  while ((U32)NtUserTcosGetUpcall((U)&u)) {
    U64 lparam = u.lparam, result = 0;
    if (u.flags & UPF_PAYLOAD_LPARAM) {
      if (u.fix_at != 0xFFFF && u.fix_at + 8u <= sizeof(u.payload) && u.fix_to < sizeof(u.payload)) *(U64*)(u.payload + u.fix_at) = (U64)(u.payload + u.fix_to);
      lparam = (U64)u.payload;
    }
    if ((u.flags & UPF_CREATESTRUCT) && u.payload_len >= 80) {      /* CREATESTRUCTW: lpszName at 56, lpszClass at 64 */
      U64 name = 0, cls = 0;
      if (create_context_find(&name, &cls)) { *(U64*)(u.payload + 56) = name; *(U64*)(u.payload + 64) = cls; }
    }
    switch (u.kind) {
      case KIND_WNDPROC:
        result = ((U64(*)(U64, U32, U64, U64))u.proc)(u.hwnd, u.msg, u.wparam, lparam);
        break;
      case KIND_HOOK:      /* HOOKPROC(code, wparam, lparam); the "hwnd" of the record is the hook handle */
        result = ((U64(*)(int, U64, U64))u.proc)((int)u.msg, u.wparam, lparam);
        break;
      case KIND_WINEVENT:      /* WINEVENTPROC(hook, event, hwnd, object id, child id, thread, time) */
        ((void(*)(U64, U32, U64, int, int, U32, U32))u.proc)(u.wparam, u.msg, u.hwnd, (int)u.lparam, (int)u.extra[0], (U32)u.extra[1], (U32)u.extra[2]);
        break;
      case KIND_TIMER:      /* TIMERPROC(hwnd, WM_TIMER, id, tick) */
        ((void(*)(U64, U32, U64, U32))u.proc)(u.hwnd, u.msg, u.wparam, (U32)u.lparam);
        break;
      default:
        break;
    }
    NtUserTcosUpcallDone(u.id, result, (U)u.payload, u.payload_len);
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
WRAP(NtUserBeginPaint, P2, A2)
WRAP(NtUserGetUpdateRect, P3, A3)
WRAP(NtUserGetUpdateRgn, P3, A3)
WRAP(NtUserRedrawWindow, P4, A4)
WRAP(NtUserUpdateWindow, P1, A1)
WRAP(NtUserSetFocus, P1, A1)
WRAP(NtUserSetActiveWindow, P1, A1)
WRAP(NtUserSetForegroundWindow, P1, A1)
WRAP(NtUserSetCapture, P1, A1)
WRAP(NtUserMessageCall, P7, A7)
WRAP(NtUserDispatchMessage, P1, A1)
WRAP(NtUserGetMessage, P4, A4)
WRAP(NtUserPeekMessage, P5, A5)
WRAP(NtUserCallHwnd, P2, A2)
WRAP(NtUserCallNextHookEx, P4, A4)
WRAP(NtUserCallMsgFilter, P2, A2)

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

/* NtUserCreateWindowEx has 17 parameters; the class name and the title are remembered for the creation messages. */
extern U NtUserCreateWindowEx_raw(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserCreateWindowEx(U ex_style, U class_name, U version, U window_name, U style, U x, U y, U cx, U cy, U parent, U menu, U instance, U params, U flags,
                                                  U client_instance, U cls, U ansi) {
  U64 title = 0;
  if (window_name) title = *(U64*)(window_name + 8);      /* UNICODE_STRING.Buffer */
  const int slot = create_context_enter(title, cls);
  U r = NtUserCreateWindowEx_raw(ex_style, class_name, version, window_name, style, x, y, cx, cy, parent, menu, instance, params, flags, client_instance, cls, ansi);
  while (r == TCOS_UPCALL_DUE) { drain_upcalls(); r = NtUserTcosResume(); }
  create_context_leave(slot);
  return r;
}

/* ---- waiting for objects and input --------------------------------------------------------------------------------------------------------------------------------- */
extern U NtUserMsgWaitForMultipleObjectsEx_raw(U, U, U, U, U);

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

int DllMain(void* module, unsigned reason, void* reserved) {
  (void)module;
  (void)reason;
  (void)reserved;
  return 1;
}
