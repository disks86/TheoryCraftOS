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
// Message queues, the kernel-to-user upcall machinery (operations), posting, retrieving, sending, timers.
#include "Process/Win32k/Win32kUserInternal.hpp"

namespace process::win32k::user {

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Queues

static State& S() { return state(); }

Queue* find_queue(uint64_t thread) {
  for (Queue& q : S().queues) if (q.thread == thread) return &q;
  return nullptr;
}

static Queue* find_queue32(uint32_t thread) {
  for (Queue& q : S().queues) if (uint32_t(q.thread) == thread) return &q;
  return nullptr;
}

Queue* queue_of(Call& call, bool create) {
  const uint64_t tid = thread_of(call);
  if (Queue* q = find_queue(tid)) return q;
  if (!create) return nullptr;
  State& s = S();
  Queue* q = s.queues.emplace(s.alloc);
  if (!q) return nullptr;
  q->thread = tid;
  q->pid = call.pid();
  InputState* in = s.input_states.emplace();
  if (!in) { s.queues.erase(q); return nullptr; }
  in->refs = 1;
  q->input_state = in;
  return q;
}

InputState& input_of(Queue& q) { return *q.input_state; }

void queue_wake(Queue& q) { (void)q; wake_all(); }

static uint32_t cur_time_ms() { return now_ms_from_ns(S().last_now_ns); }
static void touch_time(Call& call) { S().last_now_ns = now_ns(call.process()); }

static Upcall* find_upcall(uint64_t id) {
  for (Upcall& u : S().upcalls) if (u.id == id) return &u;
  return nullptr;
}
static Op* find_op(uint64_t id) {
  for (Op& o : S().ops) if (o.id == id) return &o;
  return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Operations and upcalls

Op* op_begin(Call& call, OpStep step) {
  State& s = S();
  Queue* q = queue_of(call, true);
  if (!q) return nullptr;
  Op* op = s.ops.emplace(s.alloc);
  if (!op) return nullptr;
  op->id = s.next_op++;
  op->thread = q->thread;
  op->step = step;
  if (!q->ops.push_back(op->id)) { s.ops.erase(op); return nullptr; }
  return op;
}

static void enqueue_upcall(Queue& q, Upcall* u) {
  q.upcalls.push_back(u->id);
  q.changed |= wn::QS_SENDMESSAGE;
  wake_all();
}

static Upcall* make_upcall(uint64_t thread) {
  State& s = S();
  Upcall* u = s.upcalls.emplace(s.alloc);
  if (!u) return nullptr;
  u->id = s.next_upcall++;
  u->thread = thread;
  return u;
}

// A call of the window's procedure on the calling thread, part of `op`.
Upcall* upcall_to_self(Call& call, Op* op, uint32_t hwnd, uint32_t msg, uint64_t wparam, uint64_t lparam) {
  Queue* q = queue_of(call, true);
  Window* w = find_window(hwnd);
  if (!q || !w) return nullptr;
  if (w->wndproc == 0) { op->first_result = 0; return nullptr; }      // a system window without a procedure: nothing to call
  Upcall* u = make_upcall(q->thread);
  if (!u) return nullptr;
  u->kind = kUpcallWindowProc;
  u->proc = w->wndproc;
  u->flags = w->unicode ? 0 : UPF_WNDPROC_ANSI;
  u->hwnd = hwnd;
  u->msg = msg;
  u->wparam = wparam;
  u->lparam = lparam;
  u->op = op->id;
  ++op->outstanding;
  enqueue_upcall(*q, u);
  return u;
}

static bool kind_needs_payload(uint32_t msg) {
  switch (msg) {
    case wn::WM_COPYDATA: case wn::WM_SETTEXT: case wn::WM_GETTEXT: case wn::WM_WINDOWPOSCHANGING: case wn::WM_WINDOWPOSCHANGED: case wn::WM_GETMINMAXINFO:
    case wn::WM_NCCALCSIZE: case wn::WM_STYLECHANGING: case wn::WM_STYLECHANGED: return true;
    default: return false;
  }
}

// Messages whose lparam points to memory of the sender: for a window in another process the structure is copied (and copied back).
static bool marshal_to_payload(Call& sender, uint32_t msg, uint64_t wparam, uint64_t lparam, Upcall* u, bool* fail) {
  if (!kind_needs_payload(msg) || lparam == 0) return true;
  uint32_t size = 0;
  bool copy_in = true;
  uint16_t fix_at = 0xFFFF, fix_to = 0;
  switch (msg) {
    case wn::WM_COPYDATA: {
      uint32_t cb = 0;
      uint64_t data = 0;
      if (!sender.read_u32(lparam + 8, &cb) || !sender.read_u64(lparam + 16, &data)) { *fail = true; return false; }
      if (cb > kMaxPayload - 24) { *fail = true; return false; }
      uint8_t cds[24];
      if (!sender.read(lparam, ByteView(cds, 24))) { *fail = true; return false; }
      for (uint32_t i = 0; i < 24; ++i) u->payload.push_back(cds[i]);
      uint8_t chunk[256];
      for (uint32_t done = 0; done < cb;) {
        const uint32_t n = cb - done < 256 ? cb - done : 256;
        if (!sender.read(data + done, ByteView(chunk, n))) { *fail = true; return false; }
        for (uint32_t i = 0; i < n; ++i) u->payload.push_back(chunk[i]);
        done += n;
      }
      u->fix_at = 16;
      u->fix_to = 24;
      u->flags |= UPF_PAYLOAD_LPARAM;
      return true;
    }
    case wn::WM_SETTEXT: {
      // a NUL-terminated string (wide or ANSI as the window is; the sender's width is the target's here): up to 2048 characters
      for (uint32_t i = 0; i < 2048; ++i) {
        uint16_t c = 0;
        if (!sender.read_u16(lparam + 2ull * i, &c)) { *fail = true; return false; }
        u->payload.push_back(uint8_t(c));
        u->payload.push_back(uint8_t(c >> 8));
        if (c == 0) { u->flags |= UPF_PAYLOAD_LPARAM; return true; }
      }
      *fail = true;
      return false;
    }
    case wn::WM_GETTEXT: {
      if (wparam > 2048) wparam = 2048;
      for (uint64_t i = 0; i < wparam * 2; ++i) u->payload.push_back(0);
      u->out_addr = lparam;
      u->flags |= UPF_PAYLOAD_LPARAM;
      return true;
    }
    case wn::WM_WINDOWPOSCHANGING: case wn::WM_WINDOWPOSCHANGED: size = 48; break;
    case wn::WM_GETMINMAXINFO: size = 40; break;
    case wn::WM_STYLECHANGING: case wn::WM_STYLECHANGED: size = 8; break;
    case wn::WM_NCCALCSIZE:
      if (wparam) { size = 48 + 8 + 48; fix_at = 48; fix_to = 56; }
      else size = 16;
      break;
    default: return true;
  }
  (void)copy_in;
  uint8_t raw[112];
  if (msg == wn::WM_NCCALCSIZE && wparam) {
    uint64_t pos_ptr = 0;
    if (!sender.read(lparam, ByteView(raw, 48)) || !sender.read_u64(lparam + 48, &pos_ptr) || !sender.read(pos_ptr, ByteView(raw + 56, 48))) { *fail = true; return false; }
    for (uint32_t i = 48; i < 56; ++i) raw[i] = 0;
  } else if (!sender.read(lparam, ByteView(raw, size))) { *fail = true; return false; }
  for (uint32_t i = 0; i < size; ++i) u->payload.push_back(raw[i]);
  u->fix_at = fix_at;
  u->fix_to = fix_to;
  u->out_addr = lparam;
  u->flags |= UPF_PAYLOAD_LPARAM;
  return true;
}

// Copies the changed payload of a finished cross-process send back to the sender.
static void unmarshal_from_payload(Call& sender, Upcall* u, uint32_t msg, uint64_t wparam) {
  if (u->out_addr == 0 || u->payload.count() == 0) return;
  if (msg == wn::WM_COPYDATA || msg == wn::WM_SETTEXT) return;      // read-only
  if (msg == wn::WM_NCCALCSIZE && wparam) {
    uint64_t pos_ptr = 0;
    if (u->payload.count() >= 104 && sender.read_u64(u->out_addr + 48, &pos_ptr)) {
      sender.write(u->out_addr, ConstByteView(u->payload.view().data(), 48));
      sender.write(pos_ptr, ConstByteView(u->payload.view().data() + 56, 48));
    }
    return;
  }
  sender.write(u->out_addr, ConstByteView(u->payload.view().data(), u->payload.count()));
}

// A call of the window's procedure for `op`: on this thread if the window is ours, else on the window's thread (the op then waits for it). Null if it cannot be made.
Upcall* send_proc_upcall(Call& call, Op* op, Window* w, uint32_t msg, uint64_t wparam, uint64_t lparam) {
  Queue* q = queue_of(call, true);
  if (!q) return nullptr;
  if (w->thread == q->thread) return upcall_to_self(call, op, w->handle, msg, wparam, lparam);
  Queue* target = find_queue(w->thread);
  if (!target) { call.set_last_error(kErrorInvalidThreadId); op->a[7] = 2; op->first_result = 0; return nullptr; }
  if (w->wndproc == 0) { op->first_result = 0; op->a[7] = 0; return nullptr; }
  Upcall* u = make_upcall(target->thread);
  if (!u) return nullptr;
  u->kind = kUpcallWindowProc;
  u->proc = w->wndproc;
  u->flags = w->unicode ? 0 : UPF_WNDPROC_ANSI;
  u->hwnd = w->handle;
  u->msg = msg;
  u->wparam = wparam;
  u->lparam = lparam;
  u->sender_thread = q->thread;
  u->sender_pid = q->pid;
  if (w->pid != call.pid()) {
    bool fail = false;
    if (!marshal_to_payload(call, msg, wparam, lparam, u, &fail) && fail) { S().upcalls.erase(u); call.set_last_error(kErrorInvalidParameter); op->first_result = 0; op->a[7] = 2; return nullptr; }
  }
  op->waiting_on = u->id;
  op->a[7] = 0;
  enqueue_upcall(*target, u);
  return u;
}

// A call of the window's procedure with a structure for lparam: `payload` is copied to the thread that makes the call, which hands the procedure a pointer to its copy (and
// sends the changed copy back). `fix_at`/`fix_to`: the structure holds a pointer to a later part of itself.
Upcall* queue_payload_upcall(Call& call, Op* op, Window* w, uint32_t msg, uint64_t wparam, const List<uint8_t>& payload, uint16_t fix_at, uint16_t fix_to, uint32_t flags) {
  Upcall* u = send_proc_upcall(call, op, w, msg, wparam, 0);
  if (!u) return nullptr;
  for (uint8_t b : payload) u->payload.push_back(b);
  u->flags |= flags | UPF_PAYLOAD_LPARAM;
  u->fix_at = fix_at;
  u->fix_to = fix_to;
  return u;
}

static bool thread_alive(Call& call, uint64_t tid) {
  if (!find_queue(tid)) return false;
  if ((tid & 0xFFFFF) == call.pid()) {
    if (Process::ThreadRecord* t = call.process().find_thread(tid)) return !t->finished;
  }
  return true;
}

uint64_t op_continue(Call& call, Op& start) {
  State& s = S();
  const uint64_t op_id = start.id;
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  for (;;) {
    q = find_queue(thread_of(call));
    if (!q) return 0;
    if (q->upcalls.count() > 0) { q->last_due_has_op = true; return kUpcallDue; }
    Op* op = find_op(op_id);
    if (!op) return 0;
    if (op->waiting_on != 0) {
      struct Ctx { Call* call; uint64_t upcall; Queue* queue; bool interrupted; } ctx{&call, op->waiting_on, q, false};
      uint64_t timeout = ~uint64_t{0};
      if (op->deadline_ns != ~uint64_t{0} && op->deadline_ns != 0) {
        const uint64_t now = now_ns(call.process());
        timeout = op->deadline_ns > now ? op->deadline_ns - now : 0;
      }
      const bool ok = wait_loop(call, timeout, [](void* c) -> bool {
        Ctx& x = *static_cast<Ctx*>(c);
        Upcall* u = find_upcall(x.upcall);
        if (!u || u->state == Upcall::Done || u->failed) return true;
        if (!thread_alive(*x.call, u->thread)) { u->failed = true; return true; }
        if (x.queue->upcalls.count() > 0) { x.interrupted = true; return true; }
        return false;
      }, &ctx);
      op = find_op(op_id);
      Upcall* u = find_upcall(op->waiting_on);
      if (ctx.interrupted && u && u->state != Upcall::Done && !u->failed) { q->last_due_has_op = true; return kUpcallDue; }
      if (!ok && u && u->state != Upcall::Done) {      // timed out (or the process is ending): the sender gives up
        u->abandoned = true;
        if (u->state == Upcall::Queued) {
          if (Queue* t = find_queue(u->thread)) for (uint64_t i = 0; i < t->upcalls.count(); ++i) if (t->upcalls[i] == u->id) { t->upcalls.remove_at(i); break; }
          s.upcalls.erase(u);
        }
        op->waiting_on = 0;
        op->first_result = 0;
        op->a[7] = 1;
        call.set_last_error(kErrorTimeout);
      } else if (u) {
        op->first_result = u->failed ? 0 : u->result;
        op->a[7] = u->failed ? 2 : 0;
        if (!u->failed) {
          op->payload.clear();
          for (uint8_t b : u->payload) op->payload.push_back(b);
          if (u->out_addr != 0 && u->out_addr != ~uint64_t{0}) unmarshal_from_payload(call, u, uint32_t(op->a[6] >> 32), op->a[2]);
        }
        s.upcalls.erase(u);
        op->waiting_on = 0;
      } else {
        op->waiting_on = 0;
        op->a[7] = 2;
      }
    }
    op = find_op(op_id);
    if (op->outstanding > 0) { q->last_due_has_op = true; return kUpcallDue; }
    if (op->notes.count() >= 3) {
      // messages the operation still has to send: the ones for windows of this thread are queued together, one for another thread is sent (and waited for) alone
      bool queued = false;
      while (op->notes.count() >= 3) {
        const uint64_t head = op->notes[0];
        Window* target = find_window(uint32_t(head));
        if (target && target->thread != q->thread && queued) break;
        const uint64_t wp = op->notes[1], lp = op->notes[2];
        op->notes.remove_at(0);
        op->notes.remove_at(0);
        op->notes.remove_at(0);
        if (!target) continue;
        const bool cross = target->thread != q->thread;
        if (send_proc_upcall(call, op, target, uint32_t(head >> 32), wp, lp)) queued = true;
        if (cross) { queued = true; break; }
      }
      if (queued) continue;
    }
    const uint64_t r = op->step(call, *op);
    if (r == kUpcallDue) continue;
    op = find_op(op_id);
    q = find_queue(thread_of(call));
    if (q) {
      for (uint64_t i = q->ops.count(); i > 0; --i) if (q->ops[i - 1] == op_id) { q->ops.remove_at(i - 1); break; }
      q->last_due_has_op = false;
    }
    if (op) s.ops.erase(op);
    return r;
  }
}

uint64_t op_return(Call& call, Op* op, uint64_t immediate) {
  op->result = immediate;
  return op_continue(call, *op);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Posting

static uint32_t qs_for(uint32_t message) {
  if (message >= wn::WM_KEYFIRST && message <= wn::WM_KEYLAST) return wn::QS_KEY;
  if (message == wn::WM_MOUSEMOVE) return wn::QS_MOUSEMOVE;
  if (message > wn::WM_MOUSEMOVE && message <= wn::WM_MOUSELAST) return wn::QS_MOUSEBUTTON;
  if (message == wn::WM_HOTKEY) return wn::QS_HOTKEY;
  return wn::QS_POSTMESSAGE | wn::QS_ALLPOSTMESSAGE;
}

bool post_to_queue(Queue& q, const Msg& m) {
  if (q.posted.count() >= kMaxPostedMessages) return false;
  Msg copy = m;
  copy.qs = qs_for(m.message);
  if (!q.posted.push_back(copy)) return false;
  q.changed |= copy.qs;
  wake_all();
  return true;
}

void post_hardware(Queue& q, Msg m) {
  m.hardware = true;
  m.qs = qs_for(m.message);
  if (m.message == wn::WM_MOUSEMOVE && q.input.count() > 0) {      // pending moves of the same window collapse into the last one
    Msg& last = q.input[q.input.count() - 1];
    if (last.message == wn::WM_MOUSEMOVE && last.hwnd == m.hwnd) { last = m; q.changed |= m.qs; wake_all(); return; }
  }
  if (q.input.count() >= kMaxPostedMessages) return;
  q.input.push_back(m);
  q.changed |= m.qs;
  wake_all();
}

static void fill_msg_time(Msg* m) {
  m->time = cur_time_ms();
  m->x = S().cursor_x;
  m->y = S().cursor_y;
}

static bool post_message_internal(Call* call, uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam, uint32_t* error) {
  Msg m;
  m.hwnd = hwnd;
  m.message = message;
  m.wparam = wparam;
  m.lparam = lparam;
  fill_msg_time(&m);
  *error = 0;
  if (hwnd == kHwndBroadcast) {
    List<uint32_t> targets(S().alloc);
    Desktop* d = default_desktop();
    if (d && d->root) for (Window* c = d->root->first_child; c; c = c->next) targets.push_back(c->handle);
    for (uint32_t t : targets) {
      Window* w = find_window(t);
      Queue* q = w ? find_queue(w->thread) : nullptr;
      if (!q) continue;
      m.hwnd = t;
      post_to_queue(*q, m);
    }
    return true;
  }
  if (hwnd == 0) {
    Queue* q = call ? queue_of(*call, true) : nullptr;
    if (!q) { *error = kErrorInvalidThreadId; return false; }
    return post_to_queue(*q, m) ? true : (*error = 1816, false);
  }
  Window* w = find_window(hwnd);
  if (!w) { *error = kErrorInvalidWindowHandle; return false; }
  Queue* q = find_queue(w->thread);
  if (!q) { *error = kErrorInvalidThreadId; return false; }
  if (!post_to_queue(*q, m)) { *error = 1816; return false; }      // ERROR_NOT_ENOUGH_QUOTA
  return true;
}

bool post_message(uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam) {
  uint32_t error;
  return hwnd != 0 && post_message_internal(nullptr, hwnd, message, wparam, lparam, &error);
}

bool post_thread_message(uint64_t thread, uint32_t message, uint64_t wparam, uint64_t lparam) {
  Queue* q = find_queue(thread);
  if (!q) return false;
  Msg m;
  m.message = message;
  m.wparam = wparam;
  m.lparam = lparam;
  fill_msg_time(&m);
  return post_to_queue(*q, m);
}

static bool sync_only_message(uint32_t m) {
  switch (m) {
    case 1: case wn::WM_SETTEXT: case wn::WM_GETTEXT: case wn::WM_GETMINMAXINFO: case wn::WM_WINDOWPOSCHANGING: case wn::WM_WINDOWPOSCHANGED: case wn::WM_COPYDATA:
    case wn::WM_STYLECHANGING: case wn::WM_STYLECHANGED: case wn::WM_NCCREATE: case wn::WM_NCCALCSIZE: return true;
    default: return false;
  }
}

// BOOL NtUserPostMessage(HWND, UINT, WPARAM, LPARAM)
static uint64_t nt_post_message(Call& call) {
  touch_time(call);
  const uint32_t msg = call.arg32(1);
  if (sync_only_message(msg)) return call.fail(kErrorMessageSyncOnly);
  uint32_t error = 0;
  if (!post_message_internal(&call, call.arg32(0), msg, call.arg(2), call.arg(3), &error)) return call.fail(error);
  return 1;
}

// BOOL NtUserPostThreadMessage(DWORD thread, UINT, WPARAM, LPARAM)
static uint64_t nt_post_thread_message(Call& call) {
  touch_time(call);
  Queue* q = find_queue32(call.arg32(0));
  if (!q) {
    // a thread of the calling process that has not made a queue yet gets one (the message waits for it)
    const uint64_t tid = call.arg32(0);
    Process::ThreadRecord* t = (tid & 0xFFFFF) == call.pid() ? call.process().find_thread(tid) : nullptr;
    if (!t || t->finished) return call.fail(kErrorInvalidThreadId);
    State& s = S();
    q = s.queues.emplace(s.alloc);
    if (!q) return call.fail(kErrorNotEnoughMemory);
    q->thread = tid;
    q->pid = call.pid();
    InputState* in = s.input_states.emplace();
    in->refs = 1;
    q->input_state = in;
  }
  if (sync_only_message(call.arg32(1))) return call.fail(kErrorMessageSyncOnly);
  Msg m;
  m.message = call.arg32(1);
  m.wparam = call.arg(2);
  m.lparam = call.arg(3);
  fill_msg_time(&m);
  if (!post_to_queue(*q, m)) return call.fail(1816);
  return 1;
}

// BOOL NtUserPostQuitMessage(INT exit_code)
static uint64_t nt_post_quit_message(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return call.fail(kErrorNotEnoughMemory);
  q->quit = true;
  q->quit_code = call.argi(0);
  q->changed |= wn::QS_POSTMESSAGE;
  wake_all();
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Timers

static void refresh_timers(Queue& q, uint64_t now) {
  for (Timer& t : S().timers) {
    if (t.thread == q.thread && !t.ready && t.due_ns <= now) {
      t.ready = true;
      q.changed |= wn::QS_TIMER;
    }
  }
}

void pump_timers(uint64_t now) {
  State* s = state_or_null();
  if (!s) return;
  bool woke = false;
  for (Timer& t : s->timers) {
    if (!t.ready && t.due_ns <= now) {
      t.ready = true;
      if (Queue* q = find_queue(t.thread)) q->changed |= wn::QS_TIMER;
      woke = true;
    }
  }
  if (woke) wake_all();
}

void pump(uint64_t now) {
  ObjectManager* objects = system_objects();
  if (!objects || !state_or_null()) return;
  objects->sync().lock();
  state().last_now_ns = now;
  pump_timers(now);
  objects->sync().unlock();
}

void kill_timers_of_window(uint32_t hwnd) {
  for (;;) {
    Timer* t = nullptr;
    for (Timer& x : S().timers) if (x.hwnd == hwnd) { t = &x; break; }
    if (!t) return;
    S().timers.erase(t);
  }
}
void kill_timers_of_thread(uint64_t thread) {
  for (;;) {
    Timer* t = nullptr;
    for (Timer& x : S().timers) if (x.thread == thread) { t = &x; break; }
    if (!t) return;
    S().timers.erase(t);
  }
}

static uint64_t set_timer_impl(Call& call, bool system) {
  touch_time(call);
  Queue* q = queue_of(call, true);
  if (!q) return call.fail(kErrorNotEnoughMemory);
  const uint32_t hwnd = call.arg32(0);
  uint64_t id = call.arg(1);
  uint32_t elapse = call.arg32(2);
  Window* w = nullptr;
  if (hwnd != 0) {
    w = find_window(hwnd);
    if (!w) return call.fail(kErrorInvalidWindowHandle);
    if (w->thread != q->thread) return call.fail(kErrorAccessDenied);
  }
  if (elapse < 10) elapse = 10;
  if (elapse > 0x7FFFFFFFu) elapse = 0x7FFFFFFFu;
  State& s = S();
  Timer* t = nullptr;
  if (hwnd != 0) {
    for (Timer& x : s.timers) if (x.hwnd == hwnd && x.id == id && x.system == system) { t = &x; break; }
  } else {
    id = 0;
  }
  if (!t) {
    t = s.timers.emplace();
    if (!t) return call.fail(kErrorNotEnoughMemory);
    if (hwnd == 0) {
      uint64_t candidate = 0x100;
      for (bool clash = true; clash;) {
        clash = false;
        candidate = candidate + 1 + (s.next_timer++ % 7);
        for (Timer& x : s.timers) if (&x != t && x.hwnd == 0 && x.id == candidate) clash = true;
      }
      id = candidate;
    }
  }
  t->thread = q->thread;
  t->pid = q->pid;
  t->hwnd = hwnd;
  t->id = id;
  t->elapse_ms = elapse;
  t->due_ns = now_ns(call.process()) + uint64_t{elapse} * 1000000u;
  t->proc = call.arg(3);
  t->system = system;
  t->ready = false;
  return id;
}

static uint64_t kill_timer_impl(Call& call, bool system) {
  const uint32_t hwnd = call.arg32(0);
  const uint64_t id = call.arg(1);
  for (Timer& x : S().timers) {
    if (x.hwnd == hwnd && x.id == id && x.system == system) {
      if (x.pid != call.pid()) return call.fail(kErrorAccessDenied);
      S().timers.erase(&x);
      return 1;
    }
  }
  return call.fail(kErrorInvalidParameter);
}

static uint64_t nt_set_timer(Call& call) { return set_timer_impl(call, false); }
static uint64_t nt_set_system_timer(Call& call) { return set_timer_impl(call, true); }
static uint64_t nt_kill_timer(Call& call) { return kill_timer_impl(call, false); }
static uint64_t nt_kill_system_timer(Call& call) { return kill_timer_impl(call, true); }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Retrieving

namespace {

struct Filter {
  uint32_t hwnd = 0;      // 0 any, kHwndAllThread (0xFFFFFFFF) thread messages only
  uint32_t first = 0, last = 0;
  uint32_t flags = 0;
};

bool in_range(const Filter& f, uint32_t message) {
  if (f.first == 0 && f.last == 0) return true;
  return message >= f.first && message <= f.last;
}

bool match_window(const Filter& f, uint32_t hwnd) {
  if (f.hwnd == 0) return true;
  if (f.hwnd == 0xFFFFFFFFu) return hwnd == 0;
  if (hwnd == f.hwnd) return true;
  Window* parent = find_window(f.hwnd);
  Window* w = hwnd ? find_window(hwnd) : nullptr;
  return parent && w && is_child_of(parent, w);
}

bool match(const Filter& f, const Msg& m) { return in_range(f, m.message) && match_window(f, m.hwnd); }

// The PM_QS_* part of PeekMessage flags restricts what is looked at.
bool wants(const Filter& f, uint32_t qs_bits) {
  const uint32_t mask = f.flags >> 16;
  return mask == 0 || (mask & qs_bits) != 0;
}

void update_key_state(Queue& q, const Msg& m) {
  InputState& in = *q.input_state;
  auto set = [&](uint32_t vk, bool down) {
    if (vk >= 256) return;
    const bool was = (in.key_state[vk] & 0x80) != 0;
    in.key_state[vk] = uint8_t((in.key_state[vk] & 1) | (down ? 0x80 : 0));
    if (down && !was && (vk == wn::VK_CAPITAL || vk == wn::VK_NUMLOCK || vk == wn::VK_SCROLL)) in.key_state[vk] ^= 1;
  };
  switch (m.message) {
    case wn::WM_KEYDOWN: case wn::WM_SYSKEYDOWN: set(uint32_t(m.wparam), true); break;
    case wn::WM_KEYUP: case wn::WM_SYSKEYUP: set(uint32_t(m.wparam), false); break;
    case wn::WM_LBUTTONDOWN: set(wn::VK_LBUTTON, true); break;
    case wn::WM_LBUTTONUP: set(wn::VK_LBUTTON, false); break;
    case wn::WM_RBUTTONDOWN: set(wn::VK_RBUTTON, true); break;
    case wn::WM_RBUTTONUP: set(wn::VK_RBUTTON, false); break;
    case wn::WM_MBUTTONDOWN: set(wn::VK_MBUTTON, true); break;
    case wn::WM_MBUTTONUP: set(wn::VK_MBUTTON, false); break;
    default: break;
  }
  if (m.message == wn::WM_KEYDOWN || m.message == wn::WM_KEYUP || m.message == wn::WM_SYSKEYDOWN || m.message == wn::WM_SYSKEYUP) {
    // the generic modifier keys follow the left/right ones
    const bool shift = (in.key_state[wn::VK_LSHIFT] | in.key_state[wn::VK_RSHIFT]) & 0x80, ctrl = (in.key_state[wn::VK_LCONTROL] | in.key_state[wn::VK_RCONTROL]) & 0x80,
               alt = (in.key_state[wn::VK_LMENU] | in.key_state[wn::VK_RMENU]) & 0x80;
    in.key_state[wn::VK_SHIFT] = uint8_t((in.key_state[wn::VK_SHIFT] & 1) | (shift ? 0x80 : 0));
    in.key_state[wn::VK_CONTROL] = uint8_t((in.key_state[wn::VK_CONTROL] & 1) | (ctrl ? 0x80 : 0));
    in.key_state[wn::VK_MENU] = uint8_t((in.key_state[wn::VK_MENU] & 1) | (alt ? 0x80 : 0));
  }
}

void note_retrieved(Queue& q, const Msg& m) {
  q.last_time = m.time;
  q.last_x = m.x;
  q.last_y = m.y;
  q.extra_info = m.extra;
  update_key_state(q, m);
}

// Looks for the next message for the queue's thread. Returns 1 found, 0 none, -1 sent messages/upcalls are due.
int try_get(Call& call, Queue& q, const Filter& f, bool remove, Msg* out) {
  const uint64_t now = now_ns(call.process());
  S().last_now_ns = now;
  const bool sent_ok = wants(f, wn::QS_SENDMESSAGE) || (f.flags >> 16) == 0;
  if (sent_ok && q.upcalls.count() > 0) return -1;
  // posted messages
  if (wants(f, wn::QS_POSTMESSAGE | wn::QS_ALLPOSTMESSAGE | wn::QS_HOTKEY)) {
    for (uint64_t i = 0; i < q.posted.count(); ++i) {
      if (!match(f, q.posted[i])) continue;
      *out = q.posted[i];
      if (remove) q.posted.remove_at(i);
      return 1;
    }
    if (q.quit && f.hwnd == 0 && in_range(f, wn::WM_QUIT)) {
      *out = Msg();
      out->message = wn::WM_QUIT;
      out->wparam = uint64_t(uint32_t(q.quit_code));
      out->time = cur_time_ms();
      if (remove) q.quit = false;
      return 1;
    }
  }
  if (wants(f, wn::QS_INPUT)) {
    for (uint64_t i = 0; i < q.input.count(); ++i) {
      if (!match(f, q.input[i])) continue;
      if ((f.flags >> 16) != 0 && !wants(f, q.input[i].qs)) continue;
      *out = q.input[i];
      if (remove) q.input.remove_at(i);
      return 1;
    }
  }
  if (wants(f, wn::QS_PAINT) && in_range(f, wn::WM_PAINT)) {
    for (Window& w : S().windows.items()) {
      if (w.thread != q.thread || !(w.update.count() != 0 || w.paint_pending) || !is_visible(&w)) continue;
      if (!match_window(f, w.handle)) continue;
      *out = Msg();
      out->hwnd = w.handle;
      out->message = wn::WM_PAINT;
      out->time = cur_time_ms();
      out->x = S().cursor_x;
      out->y = S().cursor_y;
      return 1;
    }
  }
  if (wants(f, wn::QS_TIMER)) {
    refresh_timers(q, now);
    for (Timer& t : S().timers) {
      if (t.thread != q.thread || !t.ready) continue;
      const uint32_t message = t.system ? wn::WM_SYSTIMER : wn::WM_TIMER;
      if (!in_range(f, message) || !match_window(f, t.hwnd)) continue;
      *out = Msg();
      out->hwnd = t.hwnd;
      out->message = message;
      out->wparam = t.id;
      out->lparam = t.proc;
      out->time = cur_time_ms();
      out->x = S().cursor_x;
      out->y = S().cursor_y;
      if (remove) {
        t.ready = false;
        t.due_ns = now + uint64_t{t.elapse_ms} * 1000000u;
      }
      return 1;
    }
  }
  return 0;
}

uint64_t earliest_timer(const Queue* q) {
  uint64_t best = ~uint64_t{0};
  for (Timer& t : S().timers) if (t.thread == q->thread && !t.ready && t.due_ns < best) best = t.due_ns;
  return best;
}

bool write_msg(Call& call, uint64_t address, const Msg& m) {
  const uint32_t k = caller_scale(call);      // (MSG.pt in the caller's coordinates: an unaware thread's 96-dpi units)
  uint8_t raw[48] = {};
  auto put = [&](uint32_t o, uint64_t v, uint32_t n) { for (uint32_t i = 0; i < n; ++i) raw[o + i] = uint8_t(v >> (8 * i)); };
  put(0, m.hwnd == 0 ? 0 : uint64_t(m.hwnd), 8);
  put(8, m.message, 4);
  put(16, m.wparam, 8);
  put(24, m.lparam, 8);
  put(32, m.time, 4);
  put(36, uint32_t(dpi_down(m.x, k)), 4);
  put(40, uint32_t(dpi_down(m.y, k)), 4);
  return call.write(address, ConstByteView(raw, 48));
}

bool read_msg(Call& call, uint64_t address, Msg* m) {
  uint8_t raw[48];
  if (!call.read(address, ByteView(raw, 48))) return false;
  auto get = [&](uint32_t o, uint32_t n) { uint64_t v = 0; for (uint32_t i = 0; i < n; ++i) v |= uint64_t{raw[o + i]} << (8 * i); return v; };
  m->hwnd = uint32_t(get(0, 8));
  m->message = uint32_t(get(8, 4));
  m->wparam = get(16, 8);
  m->lparam = get(24, 8);
  m->time = uint32_t(get(32, 4));
  m->x = int32_t(get(36, 4));
  m->y = int32_t(get(40, 4));
  return true;
}

// The hwnd argument of GetMessage/PeekMessage: 0, -1 (thread messages) or a window.
bool decode_filter(Call& call, uint32_t hwnd_index, uint32_t first_index, uint32_t last_index, uint32_t flags, Filter* f) {
  const uint64_t h = call.arg(hwnd_index);
  f->first = call.arg32(first_index);
  f->last = call.arg32(last_index);
  f->flags = flags;
  if (h == 0) f->hwnd = 0;
  else if (h == ~uint64_t{0}) f->hwnd = 0xFFFFFFFFu;
  else {
    f->hwnd = uint32_t(h);
    if (!find_window(f->hwnd)) { call.set_last_error(kErrorInvalidWindowHandle); return false; }
  }
  return true;
}

struct WaitCtx {
  Call* call;
  Queue* queue;
  Filter filter;
  Msg msg;
  int result;
  bool remove;
};

uint64_t next_timer_event(void* context) { return earliest_timer(static_cast<WaitCtx*>(context)->queue); }

// BOOL NtUserGetMessage(MSG*, HWND, UINT first, UINT last): 1 message, 0 WM_QUIT, -1 error
uint64_t nt_get_message(Call& call) {
  touch_time(call);
  Queue* q = queue_of(call, true);
  if (!q) return call.fail(kErrorNotEnoughMemory, uint32_t(-1));
  Filter f;
  if (!decode_filter(call, 1, 2, 3, 0, &f)) return uint32_t(-1);
  const uint8_t zeros[48] = {};
  if (!call.write(call.arg(0), ConstByteView(zeros, 48))) return call.fail(998, uint32_t(-1));
  WaitCtx ctx{&call, q, f, Msg(), 0, true};
  const bool ok = wait_loop(call, ~uint64_t{0}, [](void* c) -> bool {
    WaitCtx& x = *static_cast<WaitCtx*>(c);
    x.queue->changed &= ~(wn::QS_ALLINPUT | wn::QS_SENDMESSAGE);
    x.result = try_get(*x.call, *x.queue, x.filter, true, &x.msg);
    return x.result != 0;
  }, &ctx, next_timer_event);
  q = find_queue(thread_of(call));
  if (!q) return uint32_t(-1);
  if (!ok) return call.process().exited() ? 0 : call.fail(kErrorTimeout, uint32_t(-1));
  if (ctx.result < 0) return kDrainRetry;
  note_retrieved(*q, ctx.msg);
  if (!write_msg(call, call.arg(0), ctx.msg)) return call.fail(998, uint32_t(-1));
  return ctx.msg.message == wn::WM_QUIT ? 0 : 1;
}

// BOOL NtUserPeekMessage(MSG*, HWND, UINT first, UINT last, UINT flags)
uint64_t nt_peek_message(Call& call) {
  touch_time(call);
  Queue* q = queue_of(call, true);
  if (!q) return call.fail(kErrorNotEnoughMemory);
  const uint32_t flags = call.arg32(4);
  Filter f;
  if (!decode_filter(call, 1, 2, 3, flags, &f)) return 0;
  Msg m;
  q->changed &= ~(wn::QS_ALLINPUT | wn::QS_SENDMESSAGE);
  const int r = try_get(call, *q, f, (flags & wn::PM_REMOVE) != 0, &m);
  if (r < 0) return kDrainRetry;
  if (r == 0) return 0;
  if (flags & wn::PM_REMOVE) note_retrieved(*q, m);
  if (!write_msg(call, call.arg(0), m)) return call.fail(998);
  return 1;
}

// BOOL NtUserWaitMessage(void)
uint64_t nt_wait_message(Call& call) {
  touch_time(call);
  Queue* q = queue_of(call, true);
  if (!q) return call.fail(kErrorNotEnoughMemory);
  WaitCtx ctx{&call, q, Filter(), Msg(), 0, false};
  const bool ok = wait_loop(call, ~uint64_t{0}, [](void* c) -> bool {
    WaitCtx& x = *static_cast<WaitCtx*>(c);
    refresh_timers(*x.queue, now_ns(x.call->process()));
    if (x.queue->upcalls.count() > 0) { x.result = -1; return true; }
    if (x.queue->changed & wn::QS_ALLINPUT) { x.queue->changed &= ~wn::QS_ALLINPUT; x.result = 1; return true; }
    return false;
  }, &ctx, next_timer_event);
  q = find_queue(thread_of(call));
  if (!q) return 0;
  if (!ok) return 0;
  if (ctx.result < 0) return kDrainRetry;
  return 1;
}

// DWORD NtUserMsgWaitForMultipleObjectsEx(DWORD count, const HANDLE*, DWORD timeout, DWORD mask, DWORD flags): the kernel half waits for input only (count is 0 here; the
// user-mode half polls the handles and calls this for slices of the time).
uint64_t nt_msg_wait(Call& call) {
  touch_time(call);
  Queue* q = queue_of(call, true);
  if (!q) return call.fail(kErrorNotEnoughMemory, 0xFFFFFFFFu);
  const uint32_t count = call.arg32(0);
  const uint32_t timeout = call.arg32(2);
  uint32_t mask = call.arg32(3);
  const uint32_t flags = call.arg32(4);      // MWMO_WAITALL 1, MWMO_ALERTABLE 2, MWMO_INPUTAVAILABLE 4
  if (count > 63) return call.fail(kErrorInvalidParameter, 0xFFFFFFFFu);
  if (mask == 0) mask = 0;      // no input wanted: only the timeout ends the wait
  WaitCtx ctx{&call, q, Filter(), Msg(), 0, false};
  struct Local { WaitCtx* c; uint32_t mask; bool available; } local{&ctx, mask, (flags & 4) != 0};
  const uint64_t limit = timeout == 0xFFFFFFFFu ? ~uint64_t{0} : uint64_t{timeout} * 1000000u;
  const bool ok = wait_loop(call, limit, [](void* c) -> bool {
    Local& l = *static_cast<Local*>(c);
    Queue& qq = *l.c->queue;
    refresh_timers(qq, now_ns(l.c->call->process()));
    if (qq.upcalls.count() > 0) { l.c->result = -1; return true; }
    uint32_t wake = qq.changed;
    if (l.available) wake = qs_state(qq, nullptr);
    if (wake & l.mask) { qq.changed &= ~l.mask; l.c->result = 1; return true; }
    return false;
  }, &local, [](void* c) -> uint64_t { return earliest_timer(static_cast<Local*>(c)->c->queue); });
  q = find_queue(thread_of(call));
  if (!q) return 0xFFFFFFFFu;
  if (!ok) return call.process().exited() ? 0xFFFFFFFFu : kStatusTimeout;
  if (ctx.result < 0) return kDrainRetry;
  return count;      // WAIT_OBJECT_0 + count: input is available
}

// DWORD NtUserGetQueueStatus(UINT flags)
uint64_t nt_get_queue_status(Call& call) {
  touch_time(call);
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  const uint32_t mask = call.arg32(0);
  if (mask & ~wn::QS_ALLINPUT & ~0u) {
    if (mask & 0xFFFFF000u) return call.fail(kErrorInvalidFlags);
  }
  const uint32_t current = qs_state(*q, &call);
  const uint32_t changed = q->changed;
  q->changed &= ~mask;
  return (uint64_t{(changed & mask) & 0xFFFF} << 16) | (current & mask & 0xFFFF);
}

}  // namespace

// Which QS_* kinds of messages the queue holds right now.
uint32_t qs_state(Queue& q, Call* call) {
  uint32_t bits = 0;
  for (const Msg& m : q.posted) bits |= m.qs;
  if (q.quit) bits |= wn::QS_POSTMESSAGE | wn::QS_ALLPOSTMESSAGE;
  for (const Msg& m : q.input) bits |= m.qs;
  if (q.upcalls.count() > 0) bits |= wn::QS_SENDMESSAGE;
  const uint64_t now = call ? now_ns(call->process()) : S().last_now_ns;
  for (Timer& t : S().timers) if (t.thread == q.thread && (t.ready || t.due_ns <= now)) bits |= wn::QS_TIMER;
  for (Window& w : S().windows.items()) if (w.thread == q.thread && (w.update.count() != 0 || w.paint_pending) && is_visible(&w)) { bits |= wn::QS_PAINT; break; }
  if (bits & wn::QS_KEY) bits |= 0;
  return bits;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Sending

namespace {

// Op for SendMessage / SendMessageTimeout: phase 0 makes the call, phase 1 answers.
// a[0] = hwnd, a[1] = message, a[2] = wparam, a[3] = lparam, a[4] = result_info (timeout params), a[5] = type, a[6] = message << 32 (for unmarshalling)
uint64_t send_step(Call& call, Op& op) {
  if (op.phase == 0) {
    op.phase = 1;
    Window* w = find_window(uint32_t(op.a[0]));
    if (!w) { call.set_last_error(kErrorInvalidWindowHandle); op.result = 0; return 0; }
    Upcall* u = send_proc_upcall(call, &op, w, uint32_t(op.a[1]), op.a[2], op.a[3]);
    if (!u && op.a[7] == 2) { op.result = 0; return 0; }
    return kUpcallDue;
  }
  const uint64_t result = op.first_result;
  if (op.a[5] == 0x2b7) return 1;
  if (op.a[5] == 0x2b3) {      // SendMessageTimeout: the result goes to the params, the service answers a BOOL
    const bool ok = op.a[7] == 0;
    if (ok && op.a[4] != 0) call.write_u64(op.a[4] + 8, result);
    return ok ? 1 : 0;
  }
  return result;
}

}  // namespace

// LRESULT NtUserMessageCall(HWND, UINT msg, WPARAM, LPARAM, void* result_info, DWORD type, BOOL ansi)
uint64_t def_window_proc_entry(Call& call, uint32_t hwnd, uint32_t msg, uint64_t wparam, uint64_t lparam, bool ansi);

static uint64_t nt_message_call(Call& call) {
  touch_time(call);
  const uint32_t hwnd = call.arg32(0), msg = call.arg32(1);
  const uint64_t wparam = call.arg(2), lparam = call.arg(3), info = call.arg(4);
  const uint32_t type = call.arg32(5);
  const bool ansi = call.arg_bool(6);
  switch (type) {
    case 0x29e: case 0x29d:
      return def_window_proc_entry(call, hwnd, msg, wparam, lparam, ansi);
    case 0x2b1: case 0x2b3: {
      Queue* q = queue_of(call, true);
      if (!q) return call.fail(kErrorNotEnoughMemory);
      if (hwnd == kHwndBroadcast) {      // delivered like SendNotifyMessage to every top-level window (the senders do not wait here)
        Msg m;
        m.message = msg; m.wparam = wparam; m.lparam = lparam;
        List<uint32_t> targets(S().alloc);
        Desktop* d = default_desktop();
        if (d && d->root) for (Window* c = d->root->first_child; c; c = c->next) targets.push_back(c->handle);
        for (uint32_t t : targets) post_message_internal(&call, t, msg, wparam, lparam, &m.time);
        return type == 0x2b3 ? 1 : 0;
      }
      if (!find_window(hwnd)) return call.fail(kErrorInvalidWindowHandle);
      Op* op = op_begin(call, send_step);
      if (!op) return call.fail(kErrorNotEnoughMemory);
      op->a[0] = hwnd; op->a[1] = msg; op->a[2] = wparam; op->a[3] = lparam; op->a[5] = type; op->a[6] = uint64_t{msg} << 32;
      if (type == 0x2b3) {
        uint32_t flags = 0, timeout = 0;
        call.read_u32(info, &flags);
        call.read_u32(info + 4, &timeout);
        op->a[4] = info;
        op->deadline_ns = (flags & wn::SMTO_NORMAL) == 0 || true ? now_ns(call.process()) + uint64_t{timeout} * 1000000u : 0;
        if (timeout == 0) op->deadline_ns = 0;      // 0: no timeout was asked for
      }
      return op_continue(call, *op);
    }
    case 0x2b7: {      // SendNotifyMessage: same thread = SendMessage; other thread = posted to be handled as a sent message (the sender does not wait)
      Window* w = find_window(hwnd);
      if (!w) return call.fail(kErrorInvalidWindowHandle);
      Queue* q = queue_of(call, true);
      if (w->thread == q->thread) {
        Op* op = op_begin(call, send_step);
        if (!op) return call.fail(kErrorNotEnoughMemory);
        op->a[0] = hwnd; op->a[1] = msg; op->a[2] = wparam; op->a[3] = lparam; op->a[5] = 0x2b7;
        return op_continue(call, *op);
      }
      Queue* target = find_queue(w->thread);
      if (!target) return call.fail(kErrorInvalidThreadId);
      Upcall* u = make_upcall(target->thread);
      if (!u) return call.fail(kErrorNotEnoughMemory);
      u->kind = kUpcallWindowProc;
      u->proc = w->wndproc;
      u->flags = w->unicode ? 0 : UPF_WNDPROC_ANSI;
      u->hwnd = hwnd; u->msg = msg; u->wparam = wparam; u->lparam = lparam;
      u->abandoned = true;      // nobody waits for the result
      u->sender_thread = q->thread;
      u->sender_pid = q->pid;
      if (w->pid != call.pid()) { bool fail = false; if (!marshal_to_payload(call, msg, wparam, lparam, u, &fail) && fail) { S().upcalls.erase(u); return call.fail(kErrorInvalidParameter); } }
      enqueue_upcall(*target, u);
      return 1;
    }
    default:
      return call.fail(kErrorInvalidParameter);
  }
}

namespace {

// BOOL NtUserReplyMessage(LRESULT result): answers the message being handled before the handler returns
uint64_t nt_reply_message(Call& call) {
  Queue* q = queue_of(call, false);
  if (!q) return call.fail(kErrorInvalidParameter);
  for (Upcall& u : S().upcalls) {
    if (u.thread == q->thread && u.state == Upcall::Delivered && u.sender_thread != 0 && !u.abandoned && u.op == 0 && u.out_addr != ~uint64_t{0}) {
      u.result = call.arg(0);
      u.state = Upcall::Done;
      u.out_addr = ~uint64_t{0};      // replied: UpcallDone only has to finish the call on this side
      wake_all();
      return 1;
    }
  }
  return call.fail(kErrorInvalidParameter);      // ERROR_INVALID_PARAMETER: not inside a message sent from another thread
}

// Fills the record for the user-mode half. BOOL NtUserTcosGetUpcall(UpcallRecord*)
uint64_t nt_get_upcall(Call& call) {
  Queue* q = queue_of(call, false);
  if (!q || q->upcalls.count() == 0) return 0;
  const uint64_t id = q->upcalls[0];
  Upcall* u = find_upcall(id);
  if (!u) { q->upcalls.remove_at(0); return 0; }
  uint8_t head[128] = {};
  auto put = [&](uint32_t o, uint64_t v, uint32_t n) { for (uint32_t i = 0; i < n; ++i) head[o + i] = uint8_t(v >> (8 * i)); };
  put(0, u->id, 8);
  put(8, u->kind, 4);
  put(12, u->flags, 4);
  put(16, u->proc, 8);
  put(24, u->hwnd == 0 ? 0 : uint64_t(u->hwnd), 8);
  put(32, u->msg, 4);
  put(36, uint32_t(u->payload.count()), 4);
  put(40, u->wparam, 8);
  put(48, u->lparam, 8);
  put(56, u->extra[0], 8);
  put(64, u->extra[1], 8);
  put(72, u->extra[2], 8);
  put(80, u->fix_at, 2);
  put(82, u->fix_to, 2);
  if (!call.write(call.arg(0), ConstByteView(head, 88))) return call.fail(kErrorInvalidParameter);
  if (u->payload.count() != 0 && !call.write(call.arg(0) + 88, ConstByteView(u->payload.view().data(), u->payload.count()))) return call.fail(kErrorInvalidParameter);
  q->upcalls.remove_at(0);
  u->state = Upcall::Delivered;
  if (u->sender_thread != 0) ++q->in_send_depth;
  return 1;
}

// BOOL NtUserTcosUpcallDone(id, result, payload, payload_len)
uint64_t nt_upcall_done(Call& call) {
  Upcall* u = find_upcall(call.arg(0));
  Queue* q = queue_of(call, false);
  if (!u || !q || u->thread != q->thread || (u->state != Upcall::Delivered && !(u->state == Upcall::Done && u->out_addr == ~uint64_t{0}))) return call.fail(kErrorInvalidParameter);
  const bool replied = u->state == Upcall::Done;
  const uint32_t len = call.arg32(3);
  if (len != 0 && !replied && call.arg(2) != 0 && len <= kMaxPayload) {
    List<uint8_t> data(S().alloc);
    uint8_t chunk[256];
    for (uint32_t done = 0; done < len;) {
      const uint32_t n = len - done < 256 ? len - done : 256;
      if (!call.read(call.arg(2) + done, ByteView(chunk, n))) break;
      for (uint32_t i = 0; i < n; ++i) data.push_back(chunk[i]);
      done += n;
    }
    u->payload.clear();
    for (uint8_t b : data) u->payload.push_back(b);
  }
  if (u->sender_thread != 0 && q->in_send_depth > 0) --q->in_send_depth;
  if (replied) {
    if (u->abandoned) S().upcalls.erase(u);
    return 1;
  }
  u->result = call.arg(1);
  if (u->op != 0) {
    if (Op* op = find_op(u->op)) {
      if (op->outstanding) --op->outstanding;
      op->first_result = u->result;
      op->payload.clear();
      for (uint8_t b : u->payload) op->payload.push_back(b);
    }
    S().upcalls.erase(u);
    return 1;
  }
  u->state = Upcall::Done;
  if (u->abandoned) S().upcalls.erase(u);
  else wake_all();
  return 1;
}

// Continues the operation that made the last service answer kUpcallDue.
uint64_t nt_resume(Call& call) {
  Queue* q = queue_of(call, false);
  if (!q || q->ops.count() == 0) return call.fail(kErrorInvalidParameter);
  Op* op = find_op(q->ops[q->ops.count() - 1]);
  if (!op) return call.fail(kErrorInvalidParameter);
  return op_continue(call, *op);
}

// BOOL NtUserDispatchMessage(const MSG*) - answers an LRESULT
struct DispatchCtx { int dummy; };
uint64_t dispatch_step(Call& call, Op& op) {
  if (op.phase == 0) {
    op.phase = 1;
    return kUpcallDue;
  }
  return op.first_result;
}

uint64_t nt_dispatch_message(Call& call) {
  touch_time(call);
  Msg m;
  if (!read_msg(call, call.arg(0), &m)) return call.fail(998);
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  if ((m.message == wn::WM_TIMER || m.message == wn::WM_SYSTIMER) && m.lparam != 0) {
    Op* op = op_begin(call, dispatch_step);
    if (!op) return 0;
    Upcall* u = make_upcall(q->thread);
    if (!u) return 0;
    u->kind = kUpcallTimer;
    u->proc = m.lparam;
    u->hwnd = m.hwnd;
    u->msg = m.message;
    u->wparam = m.wparam;
    u->lparam = cur_time_ms();
    u->op = op->id;
    ++op->outstanding;
    enqueue_upcall(*q, u);
    return op_continue(call, *op);
  }
  Window* w = find_window(m.hwnd);
  if (!w) return m.hwnd == 0 ? 0 : call.fail(kErrorInvalidWindowHandle);
  if (w->thread != q->thread) return call.fail(kErrorInvalidParameter);
  Op* op = op_begin(call, dispatch_step);
  if (!op) return 0;
  upcall_to_self(call, op, m.hwnd, m.message, m.wparam, m.lparam);
  return op_continue(call, *op);
}

// BOOL NtUserTranslateMessage(const MSG*, UINT flags): characters are queued by the input path (inject_char), so there is nothing to translate
uint64_t nt_translate_message(Call& call) { (void)call; return 0; }

// DWORD NtUserGetMessagePos(void)
uint64_t nt_get_message_pos(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  const uint32_t k = caller_scale(call);
  return (uint32_t(dpi_down(q->last_y, k)) << 16) | (uint32_t(dpi_down(q->last_x, k)) & 0xFFFF);
}

// LPARAM NtUserSetMessageExtraInfo(LPARAM)
uint64_t nt_set_message_extra_info(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  const uint64_t old = q->extra_info;
  q->extra_info = call.arg(0);
  return old;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A thread is gone

void thread_gone(uint64_t thread) {
  State& s = S();
  // windows of the thread
  for (;;) {
    Window* victim = nullptr;
    for (Window& w : s.windows.items()) if (w.thread == thread && w.parent) { victim = &w; break; }
    if (!victim) break;
    Window* top = victim;
    while (top->parent && top->parent->parent && top->parent->thread == thread) top = top->parent;
    destroy_window_tree(top);
  }
  kill_timers_of_thread(thread);
  for (;;) {
    HotKey* h = nullptr;
    for (HotKey& x : s.hotkeys) if (x.thread == thread) { h = &x; break; }
    if (!h) break;
    s.hotkeys.erase(h);
  }
  for (;;) {
    Hook* h = nullptr;
    for (Hook& x : s.hooks.items()) if (x.thread == thread) { h = &x; break; }
    if (!h) break;
    s.hooks.destroy(h);
  }
  // calls waiting for this thread fail; calls this thread was waiting for are abandoned
  List<uint64_t> doomed(s.alloc);
  for (Upcall& u : s.upcalls) {
    if (u.thread == thread) {
      if (u.op != 0 || u.abandoned) doomed.push_back(u.id);
      else { u.failed = true; u.state = Upcall::Done; }
    } else if (u.sender_thread == thread && u.state != Upcall::Done) {
      u.abandoned = true;
      if (u.state == Upcall::Queued) doomed.push_back(u.id);
    }
  }
  for (uint64_t id : doomed) {
    for (Queue& q : s.queues) for (uint64_t i = 0; i < q.upcalls.count(); ++i) if (q.upcalls[i] == id) { q.upcalls.remove_at(i); break; }
    if (Upcall* u = find_upcall(id)) s.upcalls.erase(u);
  }
  for (;;) {
    Op* o = nullptr;
    for (Op& x : s.ops) if (x.thread == thread) { o = &x; break; }
    if (!o) break;
    s.ops.erase(o);
  }
  if (Queue* q = find_queue(thread)) {
    if (q->input_state) {
      if (q->input_state->refs > 0) --q->input_state->refs;
      if (q->input_state->refs == 0) s.input_states.erase(q->input_state);
    }
    s.queues.erase(q);
  }
  if (s.foreground_thread == thread) s.foreground_thread = 0;
  wake_all();
}

void remove_window_messages(Window* w) {
  for (Queue& q : S().queues) {
    for (uint64_t i = 0; i < q.posted.count();) { if (q.posted[i].hwnd == w->handle) q.posted.remove_at(i); else ++i; }
    for (uint64_t i = 0; i < q.input.count();) { if (q.input[i].hwnd == w->handle) q.input.remove_at(i); else ++i; }
  }
}

void register_message_services() {
  register_handler(WindowsSyscallNumber::NtUserPostMessage, nt_post_message);
  register_handler(WindowsSyscallNumber::NtUserPostThreadMessage, nt_post_thread_message);
  register_handler(WindowsSyscallNumber::NtUserPostQuitMessage, nt_post_quit_message);
  register_handler(WindowsSyscallNumber::NtUserGetMessage, nt_get_message);
  register_handler(WindowsSyscallNumber::NtUserPeekMessage, nt_peek_message);
  register_handler(WindowsSyscallNumber::NtUserWaitMessage, nt_wait_message);
  register_handler(WindowsSyscallNumber::NtUserMsgWaitForMultipleObjectsEx, nt_msg_wait);
  register_handler(WindowsSyscallNumber::NtUserGetQueueStatus, nt_get_queue_status);
  register_handler(WindowsSyscallNumber::NtUserMessageCall, nt_message_call);
  register_handler(WindowsSyscallNumber::NtUserReplyMessage, nt_reply_message);
  register_handler(WindowsSyscallNumber::NtUserDispatchMessage, nt_dispatch_message);
  register_handler(WindowsSyscallNumber::NtUserTranslateMessage, nt_translate_message);
  register_handler(WindowsSyscallNumber::NtUserGetMessagePos, nt_get_message_pos);
  register_handler(WindowsSyscallNumber::NtUserSetMessageExtraInfo, nt_set_message_extra_info);
  register_handler(WindowsSyscallNumber::NtUserSetTimer, nt_set_timer);
  register_handler(WindowsSyscallNumber::NtUserSetSystemTimer, nt_set_system_timer);
  register_handler(WindowsSyscallNumber::NtUserKillTimer, nt_kill_timer);
  register_handler(WindowsSyscallNumber::NtUserKillSystemTimer, nt_kill_system_timer);
  register_handler(WindowsSyscallNumber::NtUserTcosGetUpcall, nt_get_upcall);
  register_handler(WindowsSyscallNumber::NtUserTcosUpcallDone, nt_upcall_done);
  register_handler(WindowsSyscallNumber::NtUserTcosResume, nt_resume);
}

}  // namespace process::win32k::user
