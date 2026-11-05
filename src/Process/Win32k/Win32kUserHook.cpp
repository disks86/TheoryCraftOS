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
// Hooks (SetWindowsHookEx and its relatives) and WinEvent hooks. Only hooks in the calling process can be called (a hook procedure of another process would need its DLL in
// this one); the hook procedures run in the thread that calls the hook (an upcall).
#include "Process/Win32k/Win32kUserInternal.hpp"

namespace process::win32k::user {

namespace {

State& S() { return state(); }
constexpr int32_t WH_MSGFILTER = -1, WH_JOURNALRECORD = 0, WH_JOURNALPLAYBACK = 1, WH_SYSMSGFILTER = 6, WH_MIN = -1, WH_MAX = 14, WH_KEYBOARD_LL = 13, WH_MOUSE_LL = 14;
constexpr uint32_t kErrorInvalidFilterProc = 1427, kErrorThreadHasNoQueue = 1444;
constexpr uint32_t WINEVENT_OUTOFCONTEXT = 0, WINEVENT_SKIPOWNTHREAD = 1, WINEVENT_SKIPOWNPROCESS = 2, WINEVENT_INCONTEXT = 4;

// The hooks that apply to a thread for an id: its own, newest first, then the global ones, newest first. Only those of the calling process are callable.
void chain(int32_t id, uint64_t thread, uint64_t pid, List<uint32_t>& out) {
  State& s = S();
  for (int pass = 0; pass < 2; ++pass) {
    for (;;) {
      Hook* best = nullptr;
      for (Hook& h : s.hooks.items()) {
        if (h.id != id || h.removed || h.pid != pid) continue;
        if (pass == 0 ? h.thread != thread : h.thread != 0) continue;
        bool taken = false;
        for (uint32_t t : out) if (t == h.handle) taken = true;
        if (taken) continue;
        if (!best || h.seq > best->seq) best = &h;
      }
      if (!best) break;
      out.push_back(best->handle);
    }
  }
}

}  // namespace

bool has_hooks(int32_t id, uint64_t thread) {
  for (Hook& h : S().hooks.items()) if (h.id == id && !h.removed && (h.thread == 0 || h.thread == thread)) return true;
  return false;
}

namespace {

// HHOOK NtUserSetWindowsHookEx(HINSTANCE inst, UNICODE_STRING* module, DWORD tid, INT id, HOOKPROC proc, BOOL ansi)
uint64_t nt_set_windows_hook_ex(Call& call) {
  State& s = S();
  const int32_t id = call.argi(3);
  const uint64_t proc = call.arg(4);
  const uint32_t tid = call.arg32(2);
  if (id < WH_MIN || id > WH_MAX) return call.fail(kErrorInvalidHookFilter);
  if (proc == 0) return call.fail(kErrorInvalidFilterProc);
  uint64_t thread = 0;
  if (tid != 0) {
    Queue* q = nullptr;
    for (Queue& x : s.queues) if (uint32_t(x.thread) == tid) q = &x;
    if (q) thread = q->thread;
    else {
      Process::ThreadRecord* t = (uint64_t{tid} & 0xFFFFF) == call.pid() ? call.process().find_thread(tid) : nullptr;
      if (!t || t->finished) return call.fail(kErrorInvalidThreadId);
      thread = tid;
    }
    if (id == WH_JOURNALRECORD || id == WH_JOURNALPLAYBACK || id == WH_KEYBOARD_LL || id == WH_MOUSE_LL) return call.fail(kErrorGlobalOnlyHook);
    if ((thread & 0xFFFFF) != call.pid() && call.arg(0) == 0) return call.fail(kErrorHookNeedsHmod);
  } else if (call.arg(0) == 0 && id != WH_KEYBOARD_LL && id != WH_MOUSE_LL && id != WH_JOURNALRECORD && id != WH_JOURNALPLAYBACK) {
    return call.fail(kErrorHookNeedsHmod);
  }
  Hook* h = s.hooks.create();
  if (!h) return call.fail(kErrorNotEnoughMemory);
  h->thread = thread;
  h->pid = call.pid();
  h->id = id;
  h->proc = proc;
  h->module = call.arg(0);
  h->unicode = !call.arg_bool(5);
  h->seq = s.next_hook_seq++;
  return h->handle;
}

uint64_t nt_unhook_windows_hook_ex(Call& call) {
  State& s = S();
  Hook* h = s.hooks.find(call.arg32(0));
  if (!h) return call.fail(kErrorInvalidHookHandle);
  if (h->pid != call.pid()) return call.fail(kErrorAccessDenied);
  s.hooks.destroy(h);
  return 1;
}

// BOOL NtUserUnhookWindowsHook(INT id, HOOKPROC): the old API; removes the hook with this procedure
uint64_t nt_unhook_windows_hook(Call& call) {
  State& s = S();
  for (Hook& h : s.hooks.items()) {
    if (h.id == call.argi(0) && h.proc == call.arg(1) && h.pid == call.pid()) { s.hooks.destroy(&h); return 1; }
  }
  return call.fail(kErrorHookNotInstalled);
}

// LRESULT NtUserCallNextHookEx(HHOOK, INT code, WPARAM, LPARAM): a[0] hook handle after which to look; the result is the next procedure's
uint64_t hook_step(Call& call, Op& op) {
  (void)call;
  return op.a[5] ? (op.first_result != 0) : op.first_result;      // CallMsgFilter answers a BOOL
}

// The handle (1 << 32) | (id + 1) stands for "the start of the chain of hooks with this id": user32 calls the hooks of an event (a message about to be returned, a window about to
// be activated ...) in the thread with it.
uint64_t nt_call_next_hook_ex(Call& call) {
  State& s = S();
  const bool head = (call.arg(0) >> 32) == 1;
  Hook* current = head ? nullptr : s.hooks.find(call.arg32(0));
  if (!head && !current) {      // (the handle is ignored by Windows: the hook procedure being run is the current one)
    const uint64_t thread = thread_of(call);
    const Upcall* running = nullptr;
    for (const Upcall& u : s.upcalls) {
      if (u.kind == kUpcallHook && u.state == Upcall::Delivered && u.thread == thread && (!running || u.id > running->id)) running = &u;
    }
    if (running) current = s.hooks.find(running->hwnd);
  }
  if (!head && !current) return call.fail(kErrorInvalidHookHandle);
  List<uint32_t> order(s.alloc);
  chain(head ? int32_t(call.arg32(0)) - 1 : current->id, thread_of(call), call.pid(), order);
  // the hook after `current` in the order
  uint32_t next = 0;
  if (head) {
    if (order.count() != 0) next = order[0];
  } else {
    bool seen = false;
    for (uint32_t h : order) {
      if (seen) { next = h; break; }
      if (h == current->handle) seen = true;
    }
    if (!seen) {      // the hook was removed while it ran, or belongs to another thread's chain: start from the ones older than it
      for (uint32_t h : order) { Hook* x = s.hooks.find(h); if (x && x->seq < current->seq) { next = h; break; } }
    }
  }
  if (next == 0) return 0;
  Hook* n = s.hooks.find(next);
  Queue* q = queue_of(call, true);
  if (!n || !q) return 0;
  Op* op = op_begin(call, hook_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  Upcall* u = s.upcalls.emplace(s.alloc);
  if (!u) return 0;
  u->id = s.next_upcall++;
  u->thread = q->thread;
  u->kind = kUpcallHook;
  u->proc = n->proc;
  u->flags = n->unicode ? 0 : UPF_WNDPROC_ANSI;
  u->hwnd = n->handle;
  u->msg = call.arg32(1);
  u->wparam = call.arg(2);
  u->lparam = call.arg(3);
  u->op = op->id;
  ++op->outstanding;
  q->upcalls.push_back(u->id);
  q->changed |= wn::QS_SENDMESSAGE;
  return op_continue(call, *op);
}

// BOOL NtUserCallMsgFilter(MSG*, INT code): WH_MSGFILTER hooks, then WH_SYSMSGFILTER
uint64_t nt_call_msg_filter(Call& call) {
  State& s = S();
  uint8_t raw[48];
  if (!call.read(call.arg(0), ByteView(raw, 48))) return call.fail(kErrorNoaccess);
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  const int32_t ids[2] = {WH_MSGFILTER, WH_SYSMSGFILTER};
  for (int32_t id : ids) {
    List<uint32_t> order(s.alloc);
    chain(id, q->thread, call.pid(), order);
    if (order.count() == 0) continue;
    Hook* h = s.hooks.find(order[0]);
    if (!h) continue;
    Op* op = op_begin(call, hook_step);
    if (!op) return 0;
    op->a[5] = 1;
    Upcall* u = s.upcalls.emplace(s.alloc);
    if (!u) return 0;
    u->id = s.next_upcall++;
    u->thread = q->thread;
    u->kind = kUpcallHook;
    u->proc = h->proc;
    u->hwnd = h->handle;
    u->msg = call.arg32(1);
    u->wparam = 0;
    for (uint8_t b : raw) u->payload.push_back(b);
    u->flags |= UPF_PAYLOAD_LPARAM;
    u->op = op->id;
    ++op->outstanding;
    q->upcalls.push_back(u->id);
    q->changed |= wn::QS_SENDMESSAGE;
    return op_continue(call, *op);
  }
  return 0;
}

// ---- WinEvents

uint64_t nt_set_win_event_hook(Call& call) {
  State& s = S();
  const uint32_t min = call.arg32(0), max = call.arg32(1), flags = call.arg32(7);
  if (call.arg(4) == 0 || min > max) return call.fail(kErrorInvalidParameter);
  if (flags & WINEVENT_INCONTEXT) return call.fail(kErrorNotSupported);      // the module of another process cannot be loaded here; out-of-context only
  WinEventHook* h = s.win_events.create();
  if (!h) return call.fail(kErrorNotEnoughMemory);
  Queue* q = queue_of(call, true);
  h->pid = call.pid();
  h->thread = q ? q->thread : 0;
  h->event_min = min;
  h->event_max = max;
  h->proc = call.arg(4);
  h->target_pid = call.arg32(5);
  h->target_thread = call.arg32(6);
  h->flags = flags;
  return h->handle;
}
uint64_t nt_unhook_win_event(Call& call) {
  State& s = S();
  WinEventHook* h = s.win_events.find(call.arg32(0));
  if (!h) return call.fail(kErrorInvalidHandle);
  if (h->pid != call.pid()) return call.fail(kErrorAccessDenied);
  s.win_events.destroy(h);
  return 1;
}

// void NtUserNotifyWinEvent(DWORD event, HWND, LONG object_id, LONG child_id): the hooks of matching events run in the thread that installed them
uint64_t nt_notify_win_event(Call& call) {
  State& s = S();
  Window* w = find_window(call.arg32(1));
  const uint32_t event = call.arg32(0);
  const uint64_t source_thread = thread_of(call);
  for (WinEventHook& h : s.win_events.items()) {
    if (event < h.event_min || event > h.event_max) continue;
    if (h.target_pid != 0 && (!w || w->pid != h.target_pid)) continue;
    if (h.target_thread != 0 && (!w || uint32_t(w->thread) != h.target_thread)) continue;
    if ((h.flags & WINEVENT_SKIPOWNTHREAD) && h.thread == source_thread) continue;
    if ((h.flags & WINEVENT_SKIPOWNPROCESS) && h.pid == call.pid()) continue;
    Queue* q = find_queue(h.thread);
    if (!q) continue;
    Upcall* u = s.upcalls.emplace(s.alloc);
    if (!u) return 0;
    u->id = s.next_upcall++;
    u->thread = q->thread;
    u->kind = kUpcallWinEvent;
    u->proc = h.proc;
    u->hwnd = w ? w->handle : 0;
    u->msg = event;
    u->wparam = h.handle;
    u->lparam = uint64_t(uint32_t(call.arg32(2)));
    u->extra[0] = call.arg32(3);
    u->extra[1] = uint32_t(source_thread);
    u->extra[2] = now_ms(call);
    u->abandoned = true;
    q->upcalls.push_back(u->id);
    q->changed |= wn::QS_SENDMESSAGE;
  }
  wake_all();
  return 0;
}

}  // namespace

void register_hook_services() {
  register_handler(WindowsSyscallNumber::NtUserSetWindowsHookEx, nt_set_windows_hook_ex);
  register_handler(WindowsSyscallNumber::NtUserUnhookWindowsHookEx, nt_unhook_windows_hook_ex);
  register_handler(WindowsSyscallNumber::NtUserUnhookWindowsHook, nt_unhook_windows_hook);
  register_handler(WindowsSyscallNumber::NtUserCallNextHookEx, nt_call_next_hook_ex);
  register_handler(WindowsSyscallNumber::NtUserCallMsgFilter, nt_call_msg_filter);
  register_handler(WindowsSyscallNumber::NtUserSetWinEventHook, nt_set_win_event_hook);
  register_handler(WindowsSyscallNumber::NtUserUnhookWinEvent, nt_unhook_win_event);
  register_handler(WindowsSyscallNumber::NtUserNotifyWinEvent, nt_notify_win_event);
}

}  // namespace process::win32k::user
