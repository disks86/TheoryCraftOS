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

/* Hooks and WinEvents of user32.dll (included by window.inc before the controls). The hooks themselves live in win32k (src/Process/Win32k/Win32kUserHook.cpp); a hook procedure runs in the
 * thread that raises the event, and only the hooks of this process can be called. win32k calls a hook only for CallNextHookEx and CallMsgFilter, so this layer raises the events that
 * happen in user mode: the message a thread is about to get (WH_GETMESSAGE, WH_KEYBOARD, WH_MOUSE), a message sent or dispatched to a window (WH_CALLWNDPROC, WH_CALLWNDPROCRET), and
 * the window events of the CBT hook (activate, set focus, destroy, minimize/maximize, move/size). A hook procedure that sends a message does not raise WH_CALLWNDPROC again for it. */

#define WH_MSGFILTER (-1)
#define WH_JOURNALRECORD 0
#define WH_JOURNALPLAYBACK 1
#define WH_KEYBOARD 2
#define WH_GETMESSAGE 3
#define WH_CALLWNDPROC 4
#define WH_CBT 5
#define WH_SYSMSGFILTER 6
#define WH_MOUSE 7
#define WH_CALLWNDPROCRET 12
#define HC_ACTION 0
#define HC_NOREMOVE 3
#define HCBT_MOVESIZE 0
#define HCBT_MINMAX 1
#define HCBT_DESTROYWND 4
#define HCBT_ACTIVATE 5
#define HCBT_SETFOCUS 9

SVC NtUserSetWindowsHookEx(ULONG_PTR instance, ULONG_PTR module, ULONG_PTR thread, ULONG_PTR id, ULONG_PTR proc, ULONG_PTR ansi);
SVC NtUserUnhookWindowsHookEx(ULONG_PTR hook);
SVC NtUserUnhookWindowsHook(ULONG_PTR id, ULONG_PTR proc);
SVC NtUserCallNextHookEx(ULONG_PTR hook, ULONG_PTR code, ULONG_PTR wparam, ULONG_PTR lparam);
SVC NtUserCallMsgFilter(ULONG_PTR msg, ULONG_PTR code);
SVC NtUserSetWinEventHook(ULONG_PTR min, ULONG_PTR max, ULONG_PTR module, ULONG_PTR module_name, ULONG_PTR proc, ULONG_PTR pid, ULONG_PTR thread, ULONG_PTR flags);
SVC NtUserUnhookWinEvent(ULONG_PTR hook);
SVC NtUserNotifyWinEvent(ULONG_PTR event, ULONG_PTR hwnd, ULONG_PTR object, ULONG_PTR child);

/* the hooks of this process by id (index id + 1); a counter keeps the common case, no hook, free of system calls */
#define HOOK_SLOTS 128
static volatile LONG g_hook_count[16];
static struct { HANDLE handle; int id; ULONG_PTR proc; } g_hooks[HOOK_SLOTS];
#define HOOK_ACTIVE(id) (g_hook_count[(id) + 1] != 0)

/* the hooks of an event, newest first: `(1 << 32) | (id + 1)` is win32k's name for the start of the chain */
static LONG_PTR call_hooks(int id, int code, ULONG_PTR wparam, LONG_PTR lparam) {
  if (!HOOK_ACTIVE(id)) return 0;
  return (LONG_PTR)NtUserCallNextHookEx(((ULONG_PTR)1 << 32) | (ULONG_PTR)(id + 1), (ULONG_PTR)(LONG_PTR)code, wparam, (ULONG_PTR)lparam);
}

static void hook_remember(HANDLE handle, int id, ULONG_PTR proc) {
  for (int i = 0; i < HOOK_SLOTS; ++i) {
    if (!__sync_bool_compare_and_swap((LONG_PTR *)&g_hooks[i].handle, 0, (LONG_PTR)handle)) continue;
    g_hooks[i].id = id;
    g_hooks[i].proc = proc;
    __sync_fetch_and_add(&g_hook_count[id + 1], 1);
    return;
  }
}
static void hook_forget(int slot) {
  const int id = g_hooks[slot].id;
  g_hooks[slot].proc = 0;
  g_hooks[slot].handle = NULL;
  __sync_fetch_and_sub(&g_hook_count[id + 1], 1);
}

static HANDLE set_hook(int id, ULONG_PTR proc, HANDLE module, DWORD thread, BOOL ansi) {
  if (id < WH_MSGFILTER || id > 14) { SetLastError(1426 /* ERROR_INVALID_HOOK_FILTER */); return NULL; }
  const HANDLE h = (HANDLE)NtUserSetWindowsHookEx((ULONG_PTR)module, 0, thread, (ULONG_PTR)(LONG_PTR)id, proc, ansi);
  if (h) hook_remember(h, id, proc);
  return h;
}
EXPORT HANDLE WINAPI SetWindowsHookExW(int id, ULONG_PTR proc, HANDLE module, DWORD thread) { return set_hook(id, proc, module, thread, FALSE); }
EXPORT HANDLE WINAPI SetWindowsHookExA(int id, ULONG_PTR proc, HANDLE module, DWORD thread) { return set_hook(id, proc, module, thread, TRUE); }
IMPORT DWORD WINAPI GetCurrentThreadId(void);
EXPORT HANDLE WINAPI SetWindowsHookW(int id, ULONG_PTR proc) { return set_hook(id, proc, NULL, GetCurrentThreadId(), FALSE); }
EXPORT HANDLE WINAPI SetWindowsHookA(int id, ULONG_PTR proc) { return set_hook(id, proc, NULL, GetCurrentThreadId(), TRUE); }
EXPORT BOOL WINAPI UnhookWindowsHookEx(HANDLE hook) {
  if (!(NtUserUnhookWindowsHookEx((ULONG_PTR)hook) & 0xFFFFFFFFu)) return FALSE;
  for (int i = 0; i < HOOK_SLOTS; ++i) if (g_hooks[i].handle == hook) hook_forget(i);
  return TRUE;
}
EXPORT BOOL WINAPI UnhookWindowsHook(int id, ULONG_PTR proc) {
  if (!(NtUserUnhookWindowsHook((ULONG_PTR)(LONG_PTR)id, proc) & 0xFFFFFFFFu)) return FALSE;
  for (int i = 0; i < HOOK_SLOTS; ++i) if (g_hooks[i].handle && g_hooks[i].id == id && g_hooks[i].proc == proc) { hook_forget(i); break; }
  return TRUE;
}
EXPORT LONG_PTR WINAPI CallNextHookEx(HANDLE hook, int code, ULONG_PTR wparam, LONG_PTR lparam) { return (LONG_PTR)NtUserCallNextHookEx((ULONG_PTR)hook, (ULONG_PTR)(LONG_PTR)code, wparam, (ULONG_PTR)lparam); }
EXPORT BOOL WINAPI CallMsgFilterW(MSG_ *msg, int code) { return msg && (NtUserCallMsgFilter((ULONG_PTR)msg, (ULONG_PTR)(LONG_PTR)code) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI CallMsgFilterA(MSG_ *msg, int code) { return CallMsgFilterW(msg, code); }
EXPORT BOOL WINAPI CallMsgFilter(MSG_ *msg, int code) { return CallMsgFilterW(msg, code); }

EXPORT HANDLE WINAPI SetWinEventHook(DWORD min, DWORD max, HANDLE module, ULONG_PTR proc, DWORD pid, DWORD thread, DWORD flags) {
  return (HANDLE)NtUserSetWinEventHook(min, max, (ULONG_PTR)module, 0, proc, pid, thread, flags);
}
EXPORT BOOL WINAPI UnhookWinEvent(HANDLE hook) { return (NtUserUnhookWinEvent((ULONG_PTR)hook) & 0xFFFFFFFFu) != 0; }
EXPORT void WINAPI NotifyWinEvent(DWORD event, HANDLE hwnd, LONG object, LONG child) { NtUserNotifyWinEvent(event, (ULONG_PTR)hwnd, (ULONG_PTR)(LONG_PTR)object, (ULONG_PTR)(LONG_PTR)child); }
EXPORT BOOL WINAPI IsWinEventHookInstalled(DWORD event) { (void)event; return TRUE; }

/* ---- the events raised in user mode ------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct { LONG_PTR lparam; ULONG_PTR wparam; UINT message; HANDLE hwnd; } CWPSTRUCT_;
typedef struct { LONG_PTR result; LONG_PTR lparam; ULONG_PTR wparam; UINT message; HANDLE hwnd; } CWPRETSTRUCT_;
typedef struct { POINT_ pt; HANDLE hwnd; UINT hit; ULONG_PTR extra; } MOUSEHOOKSTRUCT_;

static volatile LONG g_in_call_hook;

/* the hooks that see a message before it reaches the window procedure; `sent` is nonzero when the sending thread is this one */
static void hook_call_wnd_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  if (!HOOK_ACTIVE(WH_CALLWNDPROC) || __sync_lock_test_and_set(&g_in_call_hook, 1)) return;
  CWPSTRUCT_ cwp = {lparam, wparam, message, hwnd};
  call_hooks(WH_CALLWNDPROC, HC_ACTION, 1, (LONG_PTR)&cwp);
  g_in_call_hook = 0;
}
static void hook_call_wnd_proc_ret(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam, LONG_PTR result) {
  if (!HOOK_ACTIVE(WH_CALLWNDPROCRET) || __sync_lock_test_and_set(&g_in_call_hook, 1)) return;
  CWPRETSTRUCT_ cwp = {result, lparam, wparam, message, hwnd};
  call_hooks(WH_CALLWNDPROCRET, HC_ACTION, 1, (LONG_PTR)&cwp);
  g_in_call_hook = 0;
}

/* The hooks of a message that GetMessage or PeekMessage is about to return. FALSE: a keyboard or mouse hook asked for the message to be discarded (only when it is being removed). */
static BOOL hook_filter_message(MSG_ *msg, BOOL remove) {
  const int code = remove ? HC_ACTION : HC_NOREMOVE;
  BOOL keep = TRUE;
  if (HOOK_ACTIVE(WH_KEYBOARD) && (msg->message == 0x100 || msg->message == 0x101 || msg->message == 0x104 || msg->message == 0x105)) {
    if (call_hooks(WH_KEYBOARD, code, msg->wparam, msg->lparam) != 0 && remove) keep = FALSE;
  }
  if (keep && HOOK_ACTIVE(WH_MOUSE) && ((msg->message >= 0x200 && msg->message <= 0x20E) || (msg->message >= 0xA0 && msg->message <= 0xAD))) {
    MOUSEHOOKSTRUCT_ mh = {msg->pt, msg->hwnd, msg->message >= 0xA0 && msg->message <= 0xAD ? (UINT)msg->wparam : 1u /* HTCLIENT */, 0};
    if (call_hooks(WH_MOUSE, code, msg->message, (LONG_PTR)&mh) != 0 && remove) keep = FALSE;
  }
  if (keep) call_hooks(WH_GETMESSAGE, code, remove ? 1 /* PM_REMOVE */ : 0, (LONG_PTR)msg);
  return keep;
}

/* the CBT hook: nonzero from the hook means the thing must not happen */
static BOOL hook_cbt(int code, ULONG_PTR wparam, LONG_PTR lparam) { return HOOK_ACTIVE(WH_CBT) && call_hooks(WH_CBT, code, wparam, lparam) != 0; }
