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
// DefWindowProc: the default handling of window messages (NtUserMessageCall with NtUserDefWindowProc), and the notes (messages sent after a state change) it uses.
#include "Process/Win32k/Win32kUserInternal.hpp"

namespace process::win32k::user {

namespace {

State& S() { return state(); }
constexpr uint32_t SC_SIZE = 0xF000, SC_MOVE = 0xF010, SC_CLOSE = 0xF060, SC_MINIMIZE = 0xF020, SC_MAXIMIZE = 0xF030, SC_RESTORE = 0xF120, WM_SYSCOMMAND = 0x112, SIZE_RESTORED = 0, SIZE_MINIMIZED = 1, SIZE_MAXIMIZED = 2;
constexpr uint32_t WA_INACTIVE = 0, WM_MOUSEACTIVATE_MA_ACTIVATE = 1;

uint32_t get32(const uint8_t* raw, uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; }

// What a default handler decided: the answer, and the notes it leaves in the operation.
uint64_t def_step(Call& call, Op& op) {
  (void)call;
  return op.result;
}

// Reads a text parameter: wide, or ANSI (ISO 8859-1) when the window is.
bool read_text(Call& call, Window* w, uint64_t address, WideString* out) {
  if (address == 0) return true;
  return w->unicode ? call.read_wide_z(address, 8192, out) : call.read_ansi_z(address, 8192, out);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Notes

void add_note(Op& op, uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam) {
  op.notes.push_back(uint64_t{hwnd} | uint64_t{message} << 32);
  op.notes.push_back(wparam);
  op.notes.push_back(lparam);
}

// A message of the system (not sent by a thread): the window's thread handles it the next time it looks at its queue; nobody waits for the result.
void system_send(uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam) {
  State& s = S();
  Window* w = find_window(hwnd);
  if (!w || w->wndproc == 0) return;
  Queue* q = find_queue(w->thread);
  if (!q) return;
  Upcall* u = s.upcalls.emplace(s.alloc);
  if (!u) return;
  u->id = s.next_upcall++;
  u->thread = q->thread;
  u->kind = kUpcallWindowProc;
  u->proc = w->wndproc;
  u->flags = w->unicode ? 0 : UPF_WNDPROC_ANSI;
  u->hwnd = hwnd;
  u->msg = message;
  u->wparam = wparam;
  u->lparam = lparam;
  u->abandoned = true;
  q->upcalls.push_back(u->id);
  q->changed |= wn::QS_SENDMESSAGE;
  wake_all();
}

void flush_notes(Notes& notes, Op* op) {
  for (uint64_t i = 0; i + 2 < notes.list.count(); i += 3) {
    const uint64_t head = notes.list[i];
    Window* target = op ? find_window(uint32_t(head)) : nullptr;
    if (op && target && target->thread != op->thread) {      // a window of another thread hears of it as a notification: the caller never waits for that thread
      system_send(uint32_t(head), uint32_t(head >> 32), notes.list[i + 1], notes.list[i + 2]);
    } else if (op) {
      op->notes.push_back(head);
      op->notes.push_back(notes.list[i + 1]);
      op->notes.push_back(notes.list[i + 2]);
    } else {
      system_send(uint32_t(head), uint32_t(head >> 32), notes.list[i + 1], notes.list[i + 2]);
    }
  }
  notes.list.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// DefWindowProc

static uint32_t wide_to_ansi_byte(char16_t c) { return c < 0x100 ? uint32_t(c) : uint32_t('?'); }

uint64_t def_window_proc_entry(Call& call, uint32_t hwnd, uint32_t msg, uint64_t wparam, uint64_t lparam, bool ansi) {
  (void)ansi;
  Window* w = find_window(hwnd);
  if (!w) return call.fail(kErrorInvalidWindowHandle);
  Queue* q = queue_of(call, true);
  if (!q) return 0;
  Op* op = op_begin(call, def_step);
  if (!op) return call.fail(kErrorNotEnoughMemory);
  op->result = 0;
  switch (msg) {
    case wn::WM_SETTEXT: {
      WideString text(S().alloc);
      if (lparam != 0 && !read_text(call, w, lparam, &text)) { op->result = 0; break; }
      window_text_set(w, text.view());
      if (w->style & wn::WS_VISIBLE) { w->nc_paint_pending = true; paint_region_changed(w); }
      op->result = 1;
      break;
    }
    case wn::WM_GETTEXT: {
      const uint32_t max = uint32_t(wparam);
      if (max == 0) break;
      if (w->unicode) {
        const int32_t n = write_wide_z(call, lparam, w->text.view(), max);
        op->result = n < 0 ? 0 : uint64_t(n);
      } else {
        uint32_t n = 0;
        for (; n < w->text.size() && n + 1 < max; ++n) {
          const uint8_t b = uint8_t(wide_to_ansi_byte(w->text[n]));
          if (!call.write(lparam + n, ConstByteView(&b, 1))) break;
        }
        const uint8_t zero = 0;
        call.write(lparam + n, ConstByteView(&zero, 1));
        op->result = n;
      }
      break;
    }
    case wn::WM_GETTEXTLENGTH: op->result = w->text.size(); break;
    case wn::WM_ERASEBKGND: op->result = 0; break;      // (painting needs the GDI module; the background stays as it was)
    case wn::WM_PAINT: validate_window(w, nullptr); op->result = 0; break;
    case wn::WM_CLOSE: {
      // DestroyWindow, as a nested operation: finish this one first
      Queue* qq = q;
      (void)qq;
      call.set_last_error(0);
      if (w->thread != q->thread || !w->parent || w->desktop->root == w) { op->result = 0; break; }
      op_continue(call, *op);      // (answers 0 at once: no notes)
      return destroy_window_op(call, w);
    }
    case wn::WM_NCCREATE: {
      // the window text comes from the creation structure
      uint64_t name = 0;
      if (lparam != 0 && call.read_u64(lparam + 56, &name) && name != 0) {
        WideString text(S().alloc);
        if (read_text(call, w, name, &text)) window_text_set(w, text.view());
      }
      op->result = 1;
      break;
    }
    case wn::WM_NCCALCSIZE: {
      // the default: the client rectangle is the window rectangle less the frame (the rectangle is already in rgrc[0])
      if (lparam == 0) break;
      Rect r;
      if (!read_rect(call, lparam, &r)) break;
      Window probe(S().alloc);
      probe.style = w->style;
      probe.ex_style = w->ex_style;
      probe.menu = w->menu;
      probe.bounds = r;
      probe.parent = w->parent;
      Rect c;
      compute_nc(&probe, &c);
      write_rect(call, lparam, c);
      probe.parent = nullptr;
      op->result = 0;
      break;
    }
    case wn::WM_NCHITTEST: {
      const int32_t x = int16_t(lparam & 0xFFFF), y = int16_t((lparam >> 16) & 0xFFFF);
      op->result = hit_test(w, x, y);
      break;
    }
    case wn::WM_WINDOWPOSCHANGED: {
      uint8_t pos[48];
      if (lparam == 0 || !call.read(lparam, ByteView(pos, 48))) break;
      const uint32_t flags = get32(pos, 32);
      if (!(flags & wn::SWP_NOMOVE) || (flags & wn::SWP_FRAMECHANGED)) {
        int32_t px = w->client.left, py = w->client.top;
        add_note(*op, hwnd, wn::WM_MOVE, 0, (uint32_t(py) << 16) | (uint32_t(px) & 0xFFFF));
      }
      if (!(flags & wn::SWP_NOSIZE) || (flags & wn::SWP_FRAMECHANGED)) {
        const uint32_t kind = (w->style & wn::WS_MINIMIZE) ? SIZE_MINIMIZED : (w->style & wn::WS_MAXIMIZE) ? SIZE_MAXIMIZED : SIZE_RESTORED;
        const uint32_t cw = uint32_t(w->client.right - w->client.left), ch = uint32_t(w->client.bottom - w->client.top);
        if (kind == SIZE_MINIMIZED) add_note(*op, hwnd, wn::WM_SIZE, kind, 0);
        else add_note(*op, hwnd, wn::WM_SIZE, kind, (ch << 16) | (cw & 0xFFFF));
      }
      break;
    }
    case wn::WM_NCACTIVATE:
      if (w->style & wn::WS_VISIBLE) { w->nc_paint_pending = true; paint_region_changed(w); }
      op->result = 1;
      break;
    case wn::WM_ACTIVATE: {
      if ((wparam & 0xFFFF) != WA_INACTIVE && !(w->style & wn::WS_MINIMIZE)) {
        InputState& in = input_of(*q);
        if (!in.focus || top_level_of(in.focus) != w) {
          Notes notes(S().alloc);
          focus_changes(*q, w, notes);
          flush_notes(notes, op);
        }
      }
      break;
    }
    case wn::WM_SETREDRAW: w->redraw_disabled = wparam == 0; break;
    case 0x80: {      // WM_SETICON: ICON_SMALL 0, ICON_BIG 1 (ICON_SMALL2 2 is not settable); the previous icon
      uint64_t& slot = wparam == 0 ? w->icon_small : w->icon_big;
      if (wparam > 1) break;
      op->result = slot;
      slot = uint64_t(lparam);
      notify_top_level(w);
      break;
    }
    case 0x7F:        // WM_GETICON: ICON_SMALL2 falls back to the big icon (the class icons are the caller's to ask for)
      op->result = wparam == 0 ? w->icon_small : wparam == 1 ? w->icon_big : (w->icon_small ? w->icon_small : w->icon_big);
      break;
    case wn::WM_MOUSEACTIVATE: op->result = WM_MOUSEACTIVATE_MA_ACTIVATE; break;
    case wn::WM_NCLBUTTONDOWN:      // a press on the caption moves the window, one on a border sizes it (HTLEFT 10 .. HTBOTTOMRIGHT 17 = WMSZ_LEFT 1 .. WMSZ_BOTTOMRIGHT 8)
      if (wparam == wn::HTCAPTION) add_note(*op, hwnd, WM_SYSCOMMAND, SC_MOVE | 2, lparam);
      else if (wparam >= 10 && wparam <= 17) add_note(*op, hwnd, WM_SYSCOMMAND, SC_SIZE | uint32_t(wparam - 9), lparam);
      break;
    case WM_SYSCOMMAND: {
      const uint32_t cmd = uint32_t(wparam) & 0xFFF0;
      if (cmd == (SC_CLOSE & 0xFFF0)) {
        // WM_CLOSE, sent to the window
        add_note(*op, hwnd, wn::WM_CLOSE, 0, 0);
      } else if (cmd == (SC_MINIMIZE & 0xFFF0) || cmd == (SC_MAXIMIZE & 0xFFF0) || cmd == (SC_RESTORE & 0xFFF0)) {
        op_continue(call, *op);
        SwpArgs args;
        args.w = w;
        return swp_op_begin(call, args, cmd == (SC_MINIMIZE & 0xFFF0) ? wn::SW_MINIMIZE : cmd == (SC_MAXIMIZE & 0xFFF0) ? wn::SW_SHOWMAXIMIZED : wn::SW_RESTORE, false, false);
      } else if ((cmd == SC_MOVE || cmd == SC_SIZE) && redirected(top_level_of(w))) {
        // the user moves or sizes the window with the mouse: the desktop drags its frame (the program hears of it as the frame moves)
        const uint32_t edge = cmd == SC_MOVE ? kDragMove : uint32_t(wparam) & 0xF;
        if (edge >= 1 && edge <= kDragMove) drag_requested(top_level_of(w), edge);
      } else if (cmd == kScTcosPlace && w->parent) {
        op_continue(call, *op);
        const uint64_t l = uint64_t(lparam);
        SwpArgs args;
        args.w = w;
        args.x = int16_t(uint16_t(l));
        args.y = int16_t(uint16_t(l >> 16));
        args.cx = int32_t(uint16_t(l >> 32));
        args.cy = int32_t(uint16_t(l >> 48));
        args.flags = wn::SWP_NOZORDER | wn::SWP_NOACTIVATE;
        const Rect now = window_rect_screen(w);      // (what does not change is left out, as SetWindowPos's own fix-up does: no WM_SIZE for a move)
        if (now.right - now.left == args.cx && now.bottom - now.top == args.cy) args.flags |= wn::SWP_NOSIZE;
        if (now.left == args.x && now.top == args.y) args.flags |= wn::SWP_NOMOVE;
        return swp_op_begin(call, args, kSwpNoShow, false, false);
      }
      break;
    }
    default:
      break;
  }
  return op_continue(call, *op);
}

}  // namespace process::win32k::user
