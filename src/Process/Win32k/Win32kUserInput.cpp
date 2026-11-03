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
// Input: focus, activation, foreground, capture, key state, the machine's keyboard and mouse (inject_*), SendInput, cursor position, hotkeys, the caret.
#include "Process/Win32k/Win32kUserInternal.hpp"

namespace process::win32k::user {

namespace {

State& S() { return state(); }
constexpr uint32_t WA_INACTIVE = 0, WA_ACTIVE = 1, WA_CLICKACTIVE = 2;

uint32_t make_lparam(int32_t x, int32_t y) { return (uint32_t(y) << 16) | (uint32_t(x) & 0xFFFF); }

void init_cursor() {
  State& s = S();
  if (s.cursor_init) return;
  s.cursor_init = true;
  s.cursor_x = int32_t(gdi::screen_width() / 2);
  s.cursor_y = int32_t(gdi::screen_height() / 2);
}

}  // namespace

Queue* foreground_queue() {
  if (S().foreground_thread == 0) return nullptr;
  return find_queue(S().foreground_thread);
}
Window* foreground_window_ptr() {
  Queue* q = foreground_queue();
  return q ? q->input_state->active : nullptr;
}

uint32_t foreground_window() { Window* w = foreground_window_ptr(); return w ? w->handle : 0; }
uint32_t focus_window() {
  Queue* q = foreground_queue();
  return q && q->input_state->focus ? q->input_state->focus->handle : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Focus and activation: the state changes, and the messages they cause (as notes)

void focus_window_gone(Window* w) {
  for (InputState& in : S().input_states) {
    if (in.active == w) in.active = nullptr;
    if (in.capture == w) in.capture = nullptr;
    if (in.caret_window == w) { in.caret_window = nullptr; in.caret_visible = false; }
    if (in.mouse_window == w) in.mouse_window = nullptr;
    if (in.focus == w) in.focus = nullptr;
    if (in.track_hwnd == w->handle) in.track_hwnd = 0;
  }
}

Window* set_focus_internal(Queue& q, Window* w) {
  InputState& in = *q.input_state;
  Window* old = in.focus;
  in.focus = w;
  if (in.caret_window && in.caret_window != w) in.caret_visible = false;
  return old;
}

void focus_changes(Queue& q, Window* new_focus, Notes& notes) {
  InputState& in = *q.input_state;
  Window* old = in.focus;
  if (old == new_focus) return;
  // focus follows activation: a window in another top-level window activates that one
  if (new_focus) {
    Window* top = top_level_of(new_focus);
    if (top != in.active && top != nullptr && top->parent) {
      activation_changes(q, top, false, notes);
    }
  }
  if (in.focus != old) old = in.focus;      // (activation moved focus already)
  if (old == new_focus) return;
  in.focus = new_focus;
  if (in.caret_window) in.caret_visible = false;
  if (old) notes.add(old->handle, wn::WM_KILLFOCUS, new_focus ? new_focus->handle : 0, 0);
  if (new_focus) notes.add(new_focus->handle, wn::WM_SETFOCUS, old ? old->handle : 0, 0);
}

void activation_changes(Queue& q, Window* new_top, bool click, Notes& notes) {
  InputState& in = *q.input_state;
  Window* old = in.active;
  if (old == new_top) return;
  // the window comes to the top of its level
  if (new_top && new_top->parent && !(new_top->ex_style & wn::WS_EX_TOPMOST)) apply_zorder(new_top, kHwndTop);
  else if (new_top && new_top->parent) apply_zorder(new_top, kHwndTopmost);
  if (new_top && (new_top->style & wn::WS_VISIBLE)) {
    invalidate_screen_rect(window_rect_screen(new_top), new_top);
  }
  if (old) {
    notes.add(old->handle, wn::WM_NCACTIVATE, 0, new_top ? new_top->handle : 0);
    notes.add(old->handle, wn::WM_ACTIVATE, WA_INACTIVE, new_top ? new_top->handle : 0);
    old->last_active_popup = old->last_active_popup;
  }
  in.active = new_top;
  if (new_top) {
    notes.add(new_top->handle, wn::WM_NCACTIVATE, 1, 0);
    notes.add(new_top->handle, wn::WM_ACTIVATE, click ? WA_CLICKACTIVE : WA_ACTIVE, old ? old->handle : 0);
    if (new_top->owner) new_top->owner->last_active_popup = new_top;
    // a window that has the focus in the old window loses it
  }
  // the focus window left with the old activation
  if (in.focus && (!new_top || top_level_of(in.focus) != new_top)) {
    Window* f = in.focus;
    in.focus = nullptr;
    notes.add(f->handle, wn::WM_KILLFOCUS, 0, 0);
  }
}

// Makes the queue the foreground one: the queue that was loses its activation and focus.
static void foreground_changes(Queue* new_queue, Notes& notes) {
  State& s = S();
  Queue* old = s.foreground_thread ? find_queue(s.foreground_thread) : nullptr;
  if (old == new_queue) return;
  if (old) {
    InputState& in = *old->input_state;
    if (in.focus) { notes.add(in.focus->handle, wn::WM_KILLFOCUS, 0, 0); in.focus = nullptr; }
    if (in.active) {
      notes.add(in.active->handle, wn::WM_NCACTIVATE, 0, 0);
      notes.add(in.active->handle, wn::WM_ACTIVATE, WA_INACTIVE, 0);
      in.active = nullptr;
    }
  }
  s.foreground_thread = new_queue ? new_queue->thread : 0;
}

bool set_foreground_internal(Window* w) {
  Window* top = top_level_of(w);
  if (!top || !top->parent || !is_visible(top) || (top->style & wn::WS_DISABLED)) return false;
  Queue* q = find_queue(top->thread);
  if (!q) return false;
  Notes notes(S().alloc);
  foreground_changes(q, notes);
  activation_changes(*q, top, false, notes);
  flush_notes(notes, nullptr);
  return true;
}

void activate_window_internal(Call& call, Window* w, bool notify) {
  (void)notify;
  Queue* q = find_queue(w->thread);
  if (!q) return;
  Notes notes(S().alloc);
  if (S().foreground_thread == 0 || find_queue(S().foreground_thread) == nullptr) foreground_changes(q, notes);
  activation_changes(*q, top_level_of(w), false, notes);
  // the notifications go to the operation that is running, if any, else as system messages
  Queue* mine = queue_of(call, false);
  if (mine && mine->ops.count() > 0) {
    if (Op* op = nullptr) (void)op;
  }
  Op* top = nullptr;
  if (mine && mine->ops.count() > 0) {
    for (Op& o : S().ops) if (o.id == mine->ops[mine->ops.count() - 1]) { top = &o; break; }
  }
  flush_notes(notes, top);
}

void queue_destroy_focus_changes(Call& call, Op* op, Window* root) {
  (void)call;
  Queue* q = find_queue(root->thread);
  if (!q) return;
  InputState& in = *q->input_state;
  Notes notes(S().alloc);
  const bool inside_focus = in.focus && (in.focus == root || is_child_of(root, in.focus));
  const bool inside_active = in.active && (in.active == root || is_child_of(root, in.active));
  if (in.capture && (in.capture == root || is_child_of(root, in.capture))) in.capture = nullptr;
  if (inside_focus) {
    Window* f = in.focus;
    in.focus = nullptr;
    notes.add(f->handle, wn::WM_KILLFOCUS, 0, 0);
  }
  if (inside_active) {
    Window* old = in.active;
    // the next top-level window of the thread, if any, takes over
    Window* next = nullptr;
    for (Window* c = root->desktop->root->first_child; c; c = c->next) {
      if (c != root && c->thread == q->thread && (c->style & wn::WS_VISIBLE) && !(c->style & wn::WS_DISABLED) && !(c->ex_style & wn::WS_EX_NOACTIVATE) && !is_child_of(root, c)) { next = c; break; }
    }
    notes.add(old->handle, wn::WM_NCACTIVATE, 0, next ? next->handle : 0);
    notes.add(old->handle, wn::WM_ACTIVATE, WA_INACTIVE, next ? next->handle : 0);
    in.active = next;
    if (next) {
      notes.add(next->handle, wn::WM_NCACTIVATE, 1, 0);
      notes.add(next->handle, wn::WM_ACTIVATE, WA_ACTIVE, old->handle);
    } else if (S().foreground_thread == q->thread) {
      // (the thread keeps the foreground; it has no active window now)
    }
  }
  flush_notes(notes, op);
}

void activate_other_window(Call& call, Op* op, Window* w) {
  (void)call;
  Queue* q = find_queue(w->thread);
  if (!q) return;
  InputState& in = *q->input_state;
  if (in.active != top_level_of(w)) return;
  Window* next = nullptr;
  for (Window* c = w->desktop->root->first_child; c; c = c->next) {
    if (c != w && (c->style & wn::WS_VISIBLE) && !(c->style & wn::WS_DISABLED) && !(c->ex_style & wn::WS_EX_NOACTIVATE) && c->thread == w->thread) { next = c; break; }
  }
  Notes notes(S().alloc);
  activation_changes(*q, next, false, notes);
  flush_notes(notes, op);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Focus, activation, capture services (operations with notes)

namespace {

uint64_t notes_step(Call& call, Op& op) {
  (void)call;
  return op.result;
}

// HWND NtUserSetFocus(HWND)
uint64_t nt_set_focus(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  Window* w = nullptr;
  if (call.arg(0) != 0) {
    w = window_arg(call, 0);
    if (!w) return 0;
    if (w->thread != q->thread) return call.fail(kErrorInvalidParameter);
    if (!is_visible(w) || is_top_level(w) == false ? !is_visible(w) : !is_visible(w)) return 0;
    if (w->style & wn::WS_DISABLED) return 0;
    for (Window* a = w; a && a->parent; a = a->parent) if (a->style & wn::WS_DISABLED) return 0;
  }
  InputState& in = *q->input_state;
  Window* old = in.focus;
  if (old == w) return old ? old->handle : 0;
  Op* op = op_begin(call, notes_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  Notes notes(S().alloc);
  if (w && S().foreground_thread == 0) foreground_changes(q, notes);
  focus_changes(*q, w, notes);
  flush_notes(notes, op);
  op->result = old ? old->handle : 0;
  return op_continue(call, *op);
}

// HWND NtUserSetActiveWindow(HWND)
uint64_t nt_set_active_window(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  Window* w = nullptr;
  if (call.arg(0) != 0) {
    w = window_arg(call, 0);
    if (!w) return 0;
    if (w->thread != q->thread) return call.fail(kErrorInvalidParameter);
    w = top_level_of(w);
    if (w->style & wn::WS_DISABLED) return 0;
  }
  InputState& in = *q->input_state;
  Window* old = in.active;
  Op* op = op_begin(call, notes_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  Notes notes(S().alloc);
  if (w && (S().foreground_thread == 0 || find_queue(S().foreground_thread) == nullptr)) foreground_changes(q, notes);
  activation_changes(*q, w, false, notes);
  flush_notes(notes, op);
  op->result = old ? old->handle : 0;
  return op_continue(call, *op);
}

uint64_t nt_get_foreground_window(Call& call) { (void)call; return foreground_window(); }

// BOOL NtUserSetForegroundWindow(HWND)
uint64_t nt_set_foreground_window(Call& call) {
  Window* w = window_arg(call, 0);
  if (!w) return 0;
  Window* top = top_level_of(w);
  if (!top || !top->parent || !is_visible(top)) return call.fail(kErrorInvalidParameter);
  Queue* q = find_queue(top->thread);
  if (!q) return call.fail(kErrorInvalidThreadId);
  Op* op = op_begin(call, notes_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  Notes notes(S().alloc);
  foreground_changes(q, notes);
  activation_changes(*q, top, false, notes);
  flush_notes(notes, op);
  op->result = 1;
  return op_continue(call, *op);
}

// HWND NtUserSetCapture(HWND)
uint64_t nt_set_capture(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  Window* w = nullptr;
  if (call.arg(0) != 0) {
    w = window_arg(call, 0);
    if (!w) return 0;
    if (w->thread != q->thread) return call.fail(kErrorInvalidParameter);
  }
  InputState& in = *q->input_state;
  Window* old = in.capture;
  in.capture = w;
  Op* op = op_begin(call, notes_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  if (old && old != w) add_note(*op, old->handle, wn::WM_CAPTURECHANGED, 0, w ? w->handle : 0);
  op->result = old ? old->handle : 0;
  return op_continue(call, *op);
}

uint64_t nt_release_capture(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  InputState& in = *q->input_state;
  Window* old = in.capture;
  in.capture = nullptr;
  Op* op = op_begin(call, notes_step);
  if (!op) return 0;
  if (old) add_note(*op, old->handle, wn::WM_CAPTURECHANGED, 0, 0);
  op->result = 1;
  return op_continue(call, *op);
}

// BOOL NtUserAttachThreadInput(DWORD from, DWORD to, BOOL attach)
uint64_t nt_attach_thread_input(Call& call) {
  State& s = S();
  Queue* from = nullptr;
  Queue* to = nullptr;
  for (Queue& q : s.queues) {
    if (uint32_t(q.thread) == call.arg32(0)) from = &q;
    if (uint32_t(q.thread) == call.arg32(1)) to = &q;
  }
  if (!from || !to || from == to) return call.fail(kErrorInvalidParameter);
  if (call.arg_bool(2)) {
    if (from->input_state == to->input_state) return 1;
    InputState* old = from->input_state;
    from->input_state = to->input_state;
    ++to->input_state->refs;
    if (--old->refs == 0) s.input_states.erase(old);
  } else {
    if (from->input_state != to->input_state) return call.fail(kErrorInvalidParameter, 0);
    InputState* fresh = s.input_states.emplace();
    if (!fresh) return call.fail(kErrorNotEnoughMemory);
    fresh->refs = 1;
    --from->input_state->refs;
    from->input_state = fresh;
  }
  return 1;
}

// ULONG_PTR NtUserGetThreadState(USERTHREADSTATECLASS)
uint64_t nt_get_thread_state(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  InputState& in = *q->input_state;
  switch (call.arg32(0)) {
    case 0: return in.focus ? in.focus->handle : 0;
    case 1: return in.active ? in.active->handle : 0;
    case 2: return in.capture ? in.capture->handle : 0;
    case 3: case 4: return 0;
    case 5: return (qs_state(*q, &call) & (wn::QS_KEY | wn::QS_MOUSEBUTTON)) != 0;
    case 6: return 0;
    case 7: return q->extra_info;
    case 8: {      // InSendMessageEx: ISMEX_SEND 1, ISMEX_REPLIED 8
      uint32_t r = 0;
      for (Upcall& u : S().upcalls) if (u.thread == q->thread && u.state == Upcall::Delivered && u.sender_thread != 0 && !u.abandoned) r = 1;
      for (Upcall& u : S().upcalls) if (u.thread == q->thread && u.state == Upcall::Done && u.out_addr == ~uint64_t{0}) r = 9;
      return r;
    }
    case 9: return q->last_time;
    case 10: return q->thread == S().foreground_thread;
    default: return call.fail(kErrorInvalidParameter);
  }
}

// BOOL NtUserGetGUIThreadInfo(DWORD thread, GUITHREADINFO*): cbSize 4, flags 4, then HWNDs active, focus, capture, menu owner, move/size, caret (8 each), rcCaret (16) = 72
uint64_t nt_get_gui_thread_info(Call& call) {
  uint32_t size = 0;
  if (!call.read_u32(call.arg(1), &size) || size != 72) return call.fail(kErrorInvalidParameter);
  Queue* q = nullptr;
  if (call.arg32(0) == 0) q = foreground_queue();
  else for (Queue& x : S().queues) if (uint32_t(x.thread) == call.arg32(0)) { q = &x; break; }
  if (!q) return call.fail(kErrorAccessDenied);
  InputState& in = *q->input_state;
  uint8_t raw[72] = {};
  auto put32 = [&](uint32_t o, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) raw[o + i] = uint8_t(v >> (8 * i)); };
  put32(0, 72);
  put32(4, in.caret_window && in.caret_visible ? 1 : 0);
  put32(8, in.active ? in.active->handle : 0);
  put32(16, in.focus ? in.focus->handle : 0);
  put32(24, in.capture ? in.capture->handle : 0);
  put32(40, 0);
  put32(48, in.caret_window ? in.caret_window->handle : 0);
  if (in.caret_window) {
    put32(56, uint32_t(in.caret_x));
    put32(60, uint32_t(in.caret_y));
    put32(64, uint32_t(in.caret_x + in.caret_w));
    put32(68, uint32_t(in.caret_y + in.caret_h));
  }
  return call.write(call.arg(1), ConstByteView(raw, 72)) ? 1 : call.fail(kErrorInvalidParameter);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Key state

uint64_t nt_get_key_state(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  const uint32_t vk = call.arg32(0);
  if (vk >= 256) return 0;
  const uint8_t k = q->input_state->key_state[vk];
  return uint16_t((k & 0x80 ? 0x8000 : 0) | (k & 1));
}
uint64_t nt_get_async_key_state(Call& call) {
  State& s = S();
  const uint32_t vk = call.arg32(0);
  if (vk >= 256) return 0;
  const uint8_t k = s.async_keys[vk];
  const uint16_t r = uint16_t((k & 0x80 ? 0x8000 : 0) | (k & 1));
  s.async_keys[vk] = uint8_t(k & 0x80);
  return r;
}
uint64_t nt_get_keyboard_state(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  return call.write(call.arg(0), ConstByteView(q->input_state->key_state, 256)) ? 1 : call.fail(kErrorNoaccess);
}
uint64_t nt_set_keyboard_state(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  uint8_t raw[256];
  if (!call.read(call.arg(0), ByteView(raw, 256))) return call.fail(kErrorNoaccess);
  for (uint32_t i = 0; i < 256; ++i) q->input_state->key_state[i] = raw[i];
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Cursor position

uint64_t nt_get_cursor_pos(Call& call) {
  init_cursor();
  return write_point(call, call.arg(0), Point{S().cursor_x, S().cursor_y}) ? 1 : call.fail(kErrorNoaccess);
}
uint64_t nt_set_cursor_pos(Call& call) {
  set_cursor_pos_locked(call.argi(0), call.argi(1));
  return 1;
}
uint64_t nt_clip_cursor(Call& call) {
  State& s = S();
  if (call.arg(0) == 0) { s.clip_cursor_set = 0; return 1; }
  Rect r;
  if (!read_rect(call, call.arg(0), &r)) return call.fail(kErrorNoaccess);
  if (rect_empty(r)) return call.fail(kErrorInvalidParameter);
  s.clip_cursor = r;
  s.clip_cursor_set = 1;
  init_cursor();
  set_cursor_pos_locked(s.cursor_x, s.cursor_y);
  return 1;
}
uint64_t nt_get_clip_cursor(Call& call) {
  State& s = S();
  Rect r = s.clip_cursor_set ? s.clip_cursor : Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())};
  return write_rect(call, call.arg(0), r) ? 1 : call.fail(kErrorNoaccess);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Hot keys

uint32_t modifiers_down() {
  const State& s = S();
  uint32_t m = 0;
  if (s.async_keys[wn::VK_MENU] & 0x80) m |= wn::MOD_ALT;
  if (s.async_keys[wn::VK_CONTROL] & 0x80) m |= wn::MOD_CONTROL;
  if (s.async_keys[wn::VK_SHIFT] & 0x80) m |= wn::MOD_SHIFT;
  if ((s.async_keys[wn::VK_LWIN] | s.async_keys[wn::VK_RWIN]) & 0x80) m |= wn::MOD_WIN;
  return m;
}

// BOOL NtUserRegisterHotKey(HWND, INT id, UINT modifiers, UINT vk)
uint64_t nt_register_hot_key(Call& call) {
  State& s = S();
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  Window* w = nullptr;
  if (call.arg(0) != 0) {
    w = window_arg(call, 0);
    if (!w) return 0;
    if (w->thread != q->thread) return call.fail(kErrorWindowOfOtherThread);
  }
  const uint32_t mods = call.arg32(2) & ~wn::MOD_NOREPEAT, vk = call.arg32(3);
  for (HotKey& h : s.hotkeys) {
    if ((h.hwnd == (w ? w->handle : 0) && h.id == call.argi(1) && h.thread == q->thread) || (h.modifiers == mods && h.vk == vk)) return call.fail(kErrorHotkeyAlreadyRegistered);
  }
  HotKey* h = s.hotkeys.emplace();
  if (!h) return call.fail(kErrorNotEnoughMemory);
  h->thread = q->thread;
  h->hwnd = w ? w->handle : 0;
  h->id = call.argi(1);
  h->modifiers = mods;
  h->vk = vk;
  return 1;
}
uint64_t nt_unregister_hot_key(Call& call) {
  State& s = S();
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  for (HotKey& h : s.hotkeys) {
    if (h.hwnd == call.arg32(0) && h.id == call.argi(1) && h.thread == q->thread) { s.hotkeys.erase(&h); return 1; }
  }
  return call.fail(kErrorHotkeyNotRegistered);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The caret

uint64_t nt_create_caret(Call& call) {
  Queue* q = queue_of(call, true);
  Window* w = window_arg(call, 0);
  if (!q || !w) return 0;
  if (w->thread != q->thread) return call.fail(kErrorAccessDenied);
  InputState& in = *q->input_state;
  in.caret_window = w;
  in.caret_bitmap = call.arg(1);
  in.caret_w = call.argi(2) > 0 ? call.argi(2) : user_misc::system_metrics(5, 96);
  in.caret_h = call.argi(3) > 0 ? call.argi(3) : user_misc::system_metrics(6, 96);
  in.caret_x = in.caret_y = 0;
  in.caret_hide = 1;
  in.caret_visible = false;
  return 1;
}
uint64_t nt_destroy_caret(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  InputState& in = *q->input_state;
  if (!in.caret_window) return call.fail(kErrorAccessDenied);
  in.caret_window = nullptr;
  in.caret_visible = false;
  return 1;
}
uint64_t nt_set_caret_pos(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  InputState& in = *q->input_state;
  if (!in.caret_window) return call.fail(kErrorAccessDenied);
  in.caret_x = call.argi(0);
  in.caret_y = call.argi(1);
  return 1;
}
uint64_t nt_get_caret_pos(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  InputState& in = *q->input_state;
  if (!in.caret_window) return call.fail(kErrorAccessDenied);
  return write_point(call, call.arg(0), Point{in.caret_x, in.caret_y}) ? 1 : call.fail(kErrorNoaccess);
}
uint64_t caret_visibility(Call& call, bool show) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  InputState& in = *q->input_state;
  if (!in.caret_window || (call.arg(0) != 0 && in.caret_window->handle != call.arg32(0))) return call.fail(kErrorAccessDenied);
  if (show) { if (in.caret_hide > 0) --in.caret_hide; } else ++in.caret_hide;
  in.caret_visible = in.caret_hide <= 0 && in.focus == in.caret_window;
  return 1;
}
uint64_t nt_show_caret(Call& call) { return caret_visibility(call, true); }
uint64_t nt_hide_caret(Call& call) { return caret_visibility(call, false); }
uint64_t nt_get_caret_blink_time(Call& call) { (void)call; return S().caret_blink_ms; }
uint64_t nt_set_caret_blink_time(Call& call) {
  if (call.arg32(0) == 0) return call.fail(kErrorInvalidParameter);
  S().caret_blink_ms = call.arg32(0);
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// SendInput

constexpr uint32_t KEYEVENTF_EXTENDEDKEY = 1, KEYEVENTF_KEYUP = 2, KEYEVENTF_UNICODE = 4, KEYEVENTF_SCANCODE = 8;
constexpr uint32_t MOUSEEVENTF_MOVE = 1, MOUSEEVENTF_LEFTDOWN = 2, MOUSEEVENTF_LEFTUP = 4, MOUSEEVENTF_RIGHTDOWN = 8, MOUSEEVENTF_RIGHTUP = 0x10, MOUSEEVENTF_MIDDLEDOWN = 0x20,
                   MOUSEEVENTF_MIDDLEUP = 0x40, MOUSEEVENTF_XDOWN = 0x80, MOUSEEVENTF_XUP = 0x100, MOUSEEVENTF_WHEEL = 0x800, MOUSEEVENTF_HWHEEL = 0x1000, MOUSEEVENTF_ABSOLUTE = 0x8000;

// Scan code set 1 to virtual key for the keys a program names by scan code.
uint32_t vk_from_scan(uint32_t scan) {
  static const uint8_t letters[] = {0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xBD, 0xBB, 0x08, 0x09, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xDB, 0xDD, 0x0D, 0x11,
                                    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA, 0xDE, 0xC0, 0x10, 0xDC, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 0xBC, 0xBE, 0xBF, 0x10, 0x6A, 0x12, 0x20, 0x14};
  return scan < sizeof(letters) ? letters[scan] : 0;
}

uint64_t nt_send_input(Call& call) {
  const uint32_t count = call.arg32(0);
  if (call.argi(2) != 40 || count == 0) return call.fail(kErrorInvalidParameter);
  uint32_t done = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t raw[40];
    if (!call.read(call.arg(1) + 40ull * i, ByteView(raw, 40))) return done ? done : call.fail(kErrorNoaccess);
    auto u32 = [&](uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; };
    const uint32_t type = u32(0);
    if (type == 0) {      // INPUT_MOUSE: dx 8, dy 12, mouseData 16, dwFlags 20, time 24, extraInfo 32
      const int32_t dx = int32_t(u32(8)), dy = int32_t(u32(12));
      const uint32_t data = u32(16), flags = u32(20);
      init_cursor();
      if (flags & MOUSEEVENTF_MOVE) {
        if (flags & MOUSEEVENTF_ABSOLUTE) {
          inject_move_locked(int32_t(int64_t{dx} * int64_t(gdi::screen_width()) / 65536), int32_t(int64_t{dy} * int64_t(gdi::screen_height()) / 65536));
        } else {
          inject_move_locked(S().cursor_x + dx, S().cursor_y + dy);
        }
      }
      if (flags & MOUSEEVENTF_LEFTDOWN) inject_button_locked(1, true);
      if (flags & MOUSEEVENTF_LEFTUP) inject_button_locked(1, false);
      if (flags & MOUSEEVENTF_RIGHTDOWN) inject_button_locked(2, true);
      if (flags & MOUSEEVENTF_RIGHTUP) inject_button_locked(2, false);
      if (flags & MOUSEEVENTF_MIDDLEDOWN) inject_button_locked(3, true);
      if (flags & MOUSEEVENTF_MIDDLEUP) inject_button_locked(3, false);
      if (flags & MOUSEEVENTF_XDOWN) inject_button_locked(data == 2 ? 5 : 4, true);
      if (flags & MOUSEEVENTF_XUP) inject_button_locked(data == 2 ? 5 : 4, false);
      if (flags & MOUSEEVENTF_WHEEL) inject_wheel_locked(int32_t(data), false);
      if (flags & MOUSEEVENTF_HWHEEL) inject_wheel_locked(int32_t(data), true);
    } else if (type == 1) {      // INPUT_KEYBOARD: wVk 8, wScan 10, dwFlags 12, time 16, extraInfo 24
      uint32_t vk = raw[8] | uint32_t(raw[9]) << 8;
      const uint32_t scan = raw[10] | uint32_t(raw[11]) << 8, flags = u32(12);
      const bool up = (flags & KEYEVENTF_KEYUP) != 0;
      if (flags & KEYEVENTF_UNICODE) {
        if (!up) {
          inject_key_locked(wn::VK_PACKET, scan, true, false);
          inject_char_locked(char32_t(scan));
        } else inject_key_locked(wn::VK_PACKET, scan, false, false);
      } else {
        if ((flags & KEYEVENTF_SCANCODE) && vk == 0) vk = vk_from_scan(scan & 0xFF);
        if (vk == 0 || vk > 0xFF) { ++done; continue; }
        inject_key_locked(vk, scan, !up, (flags & KEYEVENTF_EXTENDEDKEY) != 0);
      }
    } else if (type == 2) {
      // hardware input: not injected
    } else {
      return done ? done : call.fail(kErrorInvalidParameter);
    }
    ++done;
  }
  return done;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The machine's input

namespace {

uint32_t generic_modifier(uint32_t vk) {
  switch (vk) {
    case wn::VK_LSHIFT: case wn::VK_RSHIFT: return wn::VK_SHIFT;
    case wn::VK_LCONTROL: case wn::VK_RCONTROL: return wn::VK_CONTROL;
    case wn::VK_LMENU: case wn::VK_RMENU: return wn::VK_MENU;
    default: return 0;
  }
}

Window* keyboard_target() {
  Queue* q = foreground_queue();
  if (!q) return nullptr;
  InputState& in = *q->input_state;
  return in.focus ? in.focus : in.active;
}

Window* capture_window() {
  Queue* fg = foreground_queue();
  if (fg && fg->input_state->capture) return fg->input_state->capture;
  for (InputState& in : S().input_states) if (in.capture) return in.capture;
  return nullptr;
}

void post_input(Window* w, uint32_t message, uint64_t wparam, uint64_t lparam) {
  Queue* q = find_queue(w->thread);
  if (!q) return;
  Msg m;
  m.hwnd = w->handle;
  m.message = message;
  m.wparam = wparam;
  m.lparam = lparam;
  m.time = now_ms_from_ns(S().last_now_ns);
  m.x = S().cursor_x;
  m.y = S().cursor_y;
  post_hardware(*q, m);
}

uint32_t key_flags() {
  const State& s = S();
  uint32_t f = 0;
  if (s.async_keys[wn::VK_LBUTTON] & 0x80) f |= 1;
  if (s.async_keys[wn::VK_RBUTTON] & 0x80) f |= 2;
  if (s.async_keys[wn::VK_SHIFT] & 0x80) f |= 4;
  if (s.async_keys[wn::VK_CONTROL] & 0x80) f |= 8;
  if (s.async_keys[wn::VK_MBUTTON] & 0x80) f |= 0x10;
  if (s.async_keys[wn::VK_XBUTTON1] & 0x80) f |= 0x20;
  if (s.async_keys[wn::VK_XBUTTON2] & 0x80) f |= 0x40;
  return f;
}

// The window the pointer is over (or the capture window) and the hit test code.
Window* mouse_target(int32_t x, int32_t y, uint32_t* hit) {
  Window* cap = capture_window();
  if (cap) { *hit = wn::HTCLIENT; return cap; }
  Window* w = window_at_point(x, y, true, 0);
  if (!w || !w->parent) { *hit = wn::HTNOWHERE; return nullptr; }
  for (Window* a = w; a && a->parent; a = a->parent) if (a->style & wn::WS_DISABLED) { *hit = wn::HTERROR; return nullptr; }
  *hit = hit_test(w, x, y);
  return w;
}

void deliver_mouse(Window* w, uint32_t hit, uint32_t client_message, uint64_t client_wparam) {
  if (hit == wn::HTCLIENT) {
    int32_t ox, oy;
    client_origin(w, &ox, &oy);
    post_input(w, client_message, client_wparam, make_lparam(S().cursor_x - ox, S().cursor_y - oy));
  } else {
    const uint32_t nc = client_message - wn::WM_MOUSEMOVE + wn::WM_NCMOUSEMOVE;
    post_input(w, nc, hit, make_lparam(S().cursor_x, S().cursor_y));
  }
}

}  // namespace

void set_cursor_pos_locked(int32_t x, int32_t y) {
  State& s = S();
  init_cursor();
  Rect limit = s.clip_cursor_set ? s.clip_cursor : Rect{0, 0, int32_t(gdi::screen_width()), int32_t(gdi::screen_height())};
  if (x < limit.left) x = limit.left;
  if (x >= limit.right) x = limit.right - 1;
  if (y < limit.top) y = limit.top;
  if (y >= limit.bottom) y = limit.bottom - 1;
  if (x == s.cursor_x && y == s.cursor_y) return;
  s.cursor_x = x;
  s.cursor_y = y;
  uint32_t hit = 0;
  Window* w = mouse_target(x, y, &hit);
  // leaving a window that asked for WM_MOUSELEAVE
  for (InputState& in : s.input_states) {
    if (in.mouse_window && in.mouse_window != w) {
      if (in.track_hwnd == in.mouse_window->handle && (in.track_flags & 2)) {
        post_input(in.mouse_window, wn::WM_MOUSELEAVE, 0, 0);
        in.track_hwnd = 0;
        in.track_flags = 0;
      }
      in.mouse_window = nullptr;
    }
  }
  if (w) {
    if (Queue* q = find_queue(w->thread)) q->input_state->mouse_window = w;
    deliver_mouse(w, hit, wn::WM_MOUSEMOVE, key_flags());
  }
}

void inject_move_locked(int32_t x, int32_t y) { set_cursor_pos_locked(x, y); }

void inject_button_locked(uint32_t button, bool down) {
  State& s = S();
  init_cursor();
  static const uint32_t vks[6] = {0, wn::VK_LBUTTON, wn::VK_RBUTTON, wn::VK_MBUTTON, wn::VK_XBUTTON1, wn::VK_XBUTTON2};
  if (button < 1 || button > 5) return;
  const uint32_t vk = vks[button];
  const bool was = (s.async_keys[vk] & 0x80) != 0;
  if (down) s.async_keys[vk] = uint8_t(0x80 | (s.async_keys[vk] & 1) | (was ? 0 : 1)); else s.async_keys[vk] = uint8_t(s.async_keys[vk] & 1);
  uint32_t hit = 0;
  Window* w = mouse_target(s.cursor_x, s.cursor_y, &hit);
  if (!w) return;
  uint32_t message;
  uint64_t wparam = key_flags();
  switch (button) {
    case 1: message = down ? wn::WM_LBUTTONDOWN : wn::WM_LBUTTONUP; break;
    case 2: message = down ? wn::WM_RBUTTONDOWN : wn::WM_RBUTTONUP; break;
    case 3: message = down ? wn::WM_MBUTTONDOWN : wn::WM_MBUTTONUP; break;
    default: message = down ? wn::WM_XBUTTONDOWN : wn::WM_XBUTTONUP; wparam |= uint64_t{button == 4 ? 1u : 2u} << 16; break;
  }
  if (down) {
    // a second click in time, nearby, on the same window of a class that wants double clicks
    const uint64_t now = s.last_now_ns;
    const bool near = (s.cursor_x - s.last_click_x) * (s.cursor_x - s.last_click_x) <= 16 && (s.cursor_y - s.last_click_y) * (s.cursor_y - s.last_click_y) <= 16;
    if (s.last_click_button == button && s.last_click_hwnd == w->handle && near && now - s.last_click_ns <= uint64_t{s.double_click_ms} * 1000000u && w->cls && (w->cls->style & wn::CS_DBLCLKS)) {
      message += 2;      // ...DBLCLK follows ...UP in the numbering
      s.last_click_button = 0;
    } else {
      s.last_click_button = button;
      s.last_click_hwnd = w->handle;
      s.last_click_x = s.cursor_x;
      s.last_click_y = s.cursor_y;
      s.last_click_ns = now;
    }
    // a click on a window that is not active activates its top-level window
    Window* top = top_level_of(w);
    if (!capture_window() && top && top->parent && top != foreground_window_ptr() && !(top->ex_style & wn::WS_EX_NOACTIVATE)) {
      Queue* q = find_queue(top->thread);
      if (q) {
        Notes notes(s.alloc);
        foreground_changes(q, notes);
        activation_changes(*q, top, true, notes);
        flush_notes(notes, nullptr);
      }
    }
  }
  deliver_mouse(w, hit, message, wparam);
}

void inject_wheel_locked(int32_t delta, bool horizontal) {
  State& s = S();
  init_cursor();
  Window* w = keyboard_target();
  if (!w) { uint32_t hit; w = mouse_target(s.cursor_x, s.cursor_y, &hit); }
  if (!w) return;
  const uint64_t wparam = (uint64_t(uint16_t(delta)) << 16) | key_flags();
  post_input(w, horizontal ? wn::WM_MOUSEHWHEEL : wn::WM_MOUSEWHEEL, wparam, make_lparam(s.cursor_x, s.cursor_y));
}

void inject_key_locked(uint32_t vk, uint32_t scan, bool down, bool extended) {
  State& s = S();
  if (vk == 0 || vk >= 256) return;
  const bool was_down = (s.async_keys[vk] & 0x80) != 0;
  if (down) {
    s.async_keys[vk] = uint8_t(0x80 | ((s.async_keys[vk] & 1) ^ 0) | (was_down ? 0 : 1));
    if (!was_down && (vk == wn::VK_CAPITAL || vk == wn::VK_NUMLOCK || vk == wn::VK_SCROLL)) s.toggle_keys[vk] ^= 1;
  } else s.async_keys[vk] = uint8_t(s.async_keys[vk] & 1);
  if (const uint32_t g = generic_modifier(vk)) {
    const uint32_t l = g == wn::VK_SHIFT ? wn::VK_LSHIFT : g == wn::VK_CONTROL ? wn::VK_LCONTROL : wn::VK_LMENU;
    const uint32_t r = l + 1;
    const bool any = ((s.async_keys[l] | s.async_keys[r]) & 0x80) != 0;
    s.async_keys[g] = uint8_t((s.async_keys[g] & 1) | (any ? 0x80 : 0));
  }
  // hot keys
  if (down) {
    for (HotKey& h : s.hotkeys) {
      if (h.vk == vk && h.modifiers == modifiers_down()) {
        if (Queue* q = find_queue(h.thread)) {
          Msg m;
          m.hwnd = h.hwnd;
          m.message = wn::WM_HOTKEY;
          m.wparam = uint64_t(uint32_t(h.id));
          m.lparam = uint64_t(h.modifiers) | uint64_t{vk} << 16;
          m.time = now_ms_from_ns(s.last_now_ns);
          m.x = s.cursor_x; m.y = s.cursor_y;
          post_hardware(*q, m);      // (in the order of the other input)
        }
        return;
      }
    }
  }
  Window* target = keyboard_target();
  if (!target) return;
  const bool alt = (s.async_keys[wn::VK_MENU] & 0x80) != 0 && !(s.async_keys[wn::VK_CONTROL] & 0x80);
  const bool sys = alt || vk == 0x79 /* F10 */;
  uint32_t message = down ? (sys ? wn::WM_SYSKEYDOWN : wn::WM_KEYDOWN) : (sys ? wn::WM_SYSKEYUP : wn::WM_KEYUP);
  uint32_t lparam = 1 | ((scan & 0xFF) << 16) | (extended ? 1u << 24 : 0) | ((alt && vk != wn::VK_MENU && vk != wn::VK_LMENU && vk != wn::VK_RMENU) ? 1u << 29 : 0) | ((was_down || !down) ? 1u << 30 : 0) | (down ? 0 : 1u << 31);
  if (!down) lparam = (lparam & ~1u) | 1;
  post_input(target, message, vk, lparam);
}

void inject_char_locked(char32_t c) {
  State& s = S();
  Window* target = keyboard_target();
  if (!target) return;
  const bool alt = (s.async_keys[wn::VK_MENU] & 0x80) != 0 && !(s.async_keys[wn::VK_CONTROL] & 0x80);
  const uint32_t message = alt ? 0x106 /* WM_SYSCHAR */ : wn::WM_CHAR;
  const uint32_t lparam = 1 | (alt ? 1u << 29 : 0);
  if (c >= 0x10000) {
    const uint32_t v = uint32_t(c) - 0x10000;
    post_input(target, message, 0xD800 + (v >> 10), lparam);
    post_input(target, message, 0xDC00 + (v & 0x3FF), lparam);
  } else {
    post_input(target, message, uint32_t(c), lparam);
  }
}

namespace {

struct LockGuard {
  ObjectManager* objects;
  LockGuard() : objects(system_objects()) { if (objects) objects->sync().lock(); }
  ~LockGuard() { if (objects) objects->sync().unlock(); }
};

}  // namespace

void inject_key(uint32_t virtual_key, uint32_t scan_code, bool down, bool extended) { LockGuard g; if (state_or_null() || system_allocator()) inject_key_locked(virtual_key, scan_code, down, extended); }
void inject_char(char32_t character) { LockGuard g; inject_char_locked(character); }
void inject_mouse_move(int32_t x, int32_t y) { LockGuard g; inject_move_locked(x, y); }
void inject_mouse_button(uint32_t button, bool down) { LockGuard g; inject_button_locked(button, down); }
void inject_mouse_wheel(int32_t delta, bool horizontal) { LockGuard g; inject_wheel_locked(delta, horizontal); }
void cursor_position(int32_t* x, int32_t* y) {
  LockGuard g;
  init_cursor();
  *x = S().cursor_x;
  *y = S().cursor_y;
}

void drop_hotkeys_of_window(uint32_t hwnd) {
  State& s = S();
  for (;;) {
    HotKey* h = nullptr;
    for (HotKey& x : s.hotkeys) if (x.hwnd == hwnd && hwnd != 0) { h = &x; break; }
    if (!h) return;
    s.hotkeys.erase(h);
  }
}

namespace {

// BOOL NtUserTrackMouseEvent(TRACKMOUSEEVENT*): cbSize 4, dwFlags 4, hwndTrack 8, dwHoverTime 4 = 24
uint64_t nt_track_mouse_event(Call& call) {
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  uint8_t raw[24];
  if (!call.read(call.arg(0), ByteView(raw, 24))) return call.fail(kErrorNoaccess);
  auto u32 = [&](uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; };
  if (u32(0) != 24) return call.fail(kErrorInvalidParameter);
  const uint32_t flags = u32(4), hwnd = u32(8);
  Window* w = find_window(hwnd);
  if (!w) return call.fail(kErrorInvalidWindowHandle);
  InputState& in = *q->input_state;
  if (flags & 0x80000000u) {      // TME_CANCEL
    if (in.track_hwnd == hwnd) { in.track_hwnd = 0; in.track_flags = 0; }
    return 1;
  }
  if (flags & 2) {      // TME_LEAVE
    in.track_hwnd = hwnd;
    in.track_flags = flags;
    uint32_t hit = 0;
    Window* under = mouse_target(S().cursor_x, S().cursor_y, &hit);
    if (under != w) {      // the pointer is elsewhere already: the leave message is due now
      Msg m;
      m.hwnd = hwnd;
      m.message = wn::WM_MOUSELEAVE;
      post_hardware(*q, m);
      in.track_hwnd = 0;
    } else in.mouse_window = w;
  }
  return 1;
}

uint64_t nt_allow_set_foreground(Call& call) { (void)call; return 1; }

}  // namespace

void register_input_services() {
  register_handler(WindowsSyscallNumber::NtUserSetFocus, nt_set_focus);
  register_handler(WindowsSyscallNumber::NtUserSetActiveWindow, nt_set_active_window);
  register_handler(WindowsSyscallNumber::NtUserGetForegroundWindow, nt_get_foreground_window);
  register_handler(WindowsSyscallNumber::NtUserSetForegroundWindow, nt_set_foreground_window);
  register_handler(WindowsSyscallNumber::NtUserSetCapture, nt_set_capture);
  register_handler(WindowsSyscallNumber::NtUserReleaseCapture, nt_release_capture);
  register_handler(WindowsSyscallNumber::NtUserAttachThreadInput, nt_attach_thread_input);
  register_handler(WindowsSyscallNumber::NtUserGetThreadState, nt_get_thread_state);
  register_handler(WindowsSyscallNumber::NtUserGetGUIThreadInfo, nt_get_gui_thread_info);
  register_handler(WindowsSyscallNumber::NtUserGetKeyState, nt_get_key_state);
  register_handler(WindowsSyscallNumber::NtUserGetAsyncKeyState, nt_get_async_key_state);
  register_handler(WindowsSyscallNumber::NtUserGetKeyboardState, nt_get_keyboard_state);
  register_handler(WindowsSyscallNumber::NtUserSetKeyboardState, nt_set_keyboard_state);
  register_handler(WindowsSyscallNumber::NtUserGetCursorPos, nt_get_cursor_pos);
  register_handler(WindowsSyscallNumber::NtUserSetCursorPos, nt_set_cursor_pos);
  register_handler(WindowsSyscallNumber::NtUserClipCursor, nt_clip_cursor);
  register_handler(WindowsSyscallNumber::NtUserGetClipCursor, nt_get_clip_cursor);
  register_handler(WindowsSyscallNumber::NtUserRegisterHotKey, nt_register_hot_key);
  register_handler(WindowsSyscallNumber::NtUserUnregisterHotKey, nt_unregister_hot_key);
  register_handler(WindowsSyscallNumber::NtUserCreateCaret, nt_create_caret);
  register_handler(WindowsSyscallNumber::NtUserDestroyCaret, nt_destroy_caret);
  register_handler(WindowsSyscallNumber::NtUserSetCaretPos, nt_set_caret_pos);
  register_handler(WindowsSyscallNumber::NtUserGetCaretPos, nt_get_caret_pos);
  register_handler(WindowsSyscallNumber::NtUserShowCaret, nt_show_caret);
  register_handler(WindowsSyscallNumber::NtUserHideCaret, nt_hide_caret);
  register_handler(WindowsSyscallNumber::NtUserGetCaretBlinkTime, nt_get_caret_blink_time);
  register_handler(WindowsSyscallNumber::NtUserSetCaretBlinkTime, nt_set_caret_blink_time);
  register_handler(WindowsSyscallNumber::NtUserSendInput, nt_send_input);
  register_handler(WindowsSyscallNumber::NtUserTrackMouseEvent, nt_track_mouse_event);
  register_handler(WindowsSyscallNumber::NtUserAllowSetForegroundWindow, nt_allow_set_foreground);
  register_handler(WindowsSyscallNumber::NtUserLockSetForegroundWindow, nt_allow_set_foreground);
}

}  // namespace process::win32k::user
