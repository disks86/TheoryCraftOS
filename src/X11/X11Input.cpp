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

// The X server's input: the desktop hands it the keyboard and the mouse while one of the X windows has them (Win32Windows' taps), and they become core
// device events. The pointer's window is the deepest mapped one under it; key events go to the focus window (or the pointer's window inside it); events
// propagate up to the first window a client selected them on (do-not-propagate stops them); a button press starts the implicit grab that sends the
// pointer's events to that window until every button is up; GrabPointer / GrabKeyboard redirect them (owner-events as the protocol says). Enter and Leave
// follow the protocol's crossing rules (Ancestor, Virtual, Inferior, Nonlinear, NonlinearVirtual).

#include "X11/X11Internal.hpp"

namespace x11 {

namespace {

constexpr uint8_t kAncestor = 0, kVirtual = 1, kInferior = 2, kNonlinear = 3, kNonlinearVirtual = 4;

}  // namespace

Server::Window* Server::window_at(int32_t x, int32_t y) {
  Window* w = root_;
  int32_t ox = 0, oy = 0;      // w's inside, in root coordinates
  for (;;) {
    if (w != root_ && !contains({0, 0, static_cast<int32_t>(w->width), static_cast<int32_t>(w->height)}, x - ox, y - oy)) return w;      // (on its border)
    Window* hit = nullptr;
    for (uint64_t i = w->children.count(); i > 0; --i) {
      Window* ch = w->children[i - 1];
      if (!ch->mapped || (w == root_ && ch->iconic)) continue;
      const int32_t b = w == root_ ? 0 : static_cast<int32_t>(ch->border);
      if (contains({ch->x, ch->y, static_cast<int32_t>(ch->width) + 2 * b, static_cast<int32_t>(ch->height) + 2 * b}, x - ox, y - oy)) {
        hit = ch;
        break;
      }
    }
    if (!hit) return w;
    const int32_t b = w == root_ ? 0 : static_cast<int32_t>(hit->border);
    ox += hit->x + b;
    oy += hit->y + b;
    w = hit;
  }
}

uint16_t Server::key_state() const {
  uint16_t s = buttons_;
  if (modifiers_ & (1u << 1 | 1u << 5)) s |= state::kShift;
  if (caps_lock_) s |= state::kLock;
  if (modifiers_ & (1u << 0 | 1u << 4)) s |= state::kControl;
  if (modifiers_ & (1u << 2 | 1u << 6)) s |= state::kMod1;
  if (num_lock_) s |= state::kMod2;
  if (modifiers_ & (1u << 3 | 1u << 7)) s |= state::kMod4;
  return s;
}

Server::Window* Server::child_toward(Window& w, Window* source) {
  for (Window* s = source; s && s != &w; s = s->parent)
    if (s->parent == &w) return s;
  return nullptr;
}

void Server::fill_device_event(Packet& p, uint8_t type, uint8_t detail, Window& event, Window* child) {
  int32_t ox, oy;
  inside_origin(&event, &ox, &oy);
  p.u8(0, type);
  p.u8(1, detail);
  p.u32(4, now());
  p.u32(8, kRootWindow);
  p.u32(12, event.id);
  p.u32(16, child ? child->id : 0);
  p.i16(20, pointer_x_);
  p.i16(22, pointer_y_);
  p.i16(24, pointer_x_ - ox);
  p.i16(26, pointer_y_ - oy);
  p.u16(28, key_state());
  p.u8(30, 1);      // same screen
}

bool Server::propagate(uint8_t type, uint8_t detail, uint32_t wanted, Window* source, Window* stop, uint8_t only_client) {
  Window* child = nullptr;
  for (Window* w = source; w; w = w->parent) {
    bool any = false;
    for (const Selection& s : w->selections.view())
      if ((s.mask & wanted) && (!only_client || s.client == only_client)) any = true;
    if (any) {
      Packet p;
      fill_device_event(p, type, detail, *w, child);
      deliver(*w, wanted, p, only_client);
      if (type == event::kButtonPress && !pointer_grab_.active) {      // the implicit grab of the client that selected the press
        for (const Selection& s : w->selections.view()) {
          if (!(s.mask & mask::kButtonPress) || (only_client && s.client != only_client)) continue;
          pointer_grab_ = Grab();
          pointer_grab_.active = true;
          pointer_grab_.implicit = true;
          pointer_grab_.owner_events = (s.mask & mask::kOwnerGrabButton) != 0;
          pointer_grab_.client = s.client;
          pointer_grab_.window = w->id;
          pointer_grab_.mask = s.mask;
          break;
        }
      }
      return true;
    }
    if (w == stop || (w->do_not_propagate & wanted)) break;
    child = w;
  }
  return false;
}

void Server::device_event(uint8_t type, uint8_t detail, uint32_t wanted, Window* source, Window* stop, bool keyboard) {
  Grab& g = keyboard ? keyboard_grab_ : pointer_grab_;
  if (g.active) {
    Window* gw = find_window(g.window);
    if (gw && g.client < kMaxClients && clients_[g.client]) {
      if (g.owner_events && source && propagate(type, detail, wanted, source, stop, g.client)) return;
      const uint32_t grabbed = keyboard ? (mask::kKeyPress | mask::kKeyRelease) : g.mask;
      if (!(grabbed & wanted)) return;
      Packet p;
      fill_device_event(p, type, detail, *gw, child_toward(*gw, source));
      if (clients_[g.client]->setup_done) send(*clients_[g.client], p);
      return;
    }
    g = Grab();
  }
  if (source) propagate(type, detail, wanted, source, stop, 0);
}

void Server::crossing_event(Window& w, uint8_t type, uint8_t detail, uint8_t mode, Window* child) {
  const uint32_t wanted = type == event::kEnterNotify ? mask::kEnterWindow : mask::kLeaveWindow;
  if (!selected(w, wanted)) return;
  Packet p;
  fill_device_event(p, type, detail, w, child);
  p.u8(30, mode);
  Window* f = focus_ > 1 ? find_window(focus_) : nullptr;
  p.u8(31, static_cast<uint8_t>(2 | (f && is_ancestor(f, &w) ? 1 : 0)));      // same screen, focus
  deliver(w, wanted, p);
}

void Server::crossing(Window* from, Window* to, uint8_t mode) {
  if (!from) from = root_;
  if (!to) to = root_;
  if (from == to) return;
  List<Window*> path(allocator_);
  if (is_ancestor(from, to)) {      // into an inferior
    crossing_event(*from, event::kLeaveNotify, kInferior, mode, child_toward(*from, to));
    for (Window* w = to->parent; w && w != from; w = w->parent) path.push_back(w);
    for (uint64_t i = path.count(); i > 0; --i) crossing_event(*path[i - 1], event::kEnterNotify, kVirtual, mode, child_toward(*path[i - 1], to));
    crossing_event(*to, event::kEnterNotify, kAncestor, mode, nullptr);
  } else if (is_ancestor(to, from)) {      // out to an ancestor
    crossing_event(*from, event::kLeaveNotify, kAncestor, mode, nullptr);
    for (Window* w = from->parent; w && w != to; w = w->parent) crossing_event(*w, event::kLeaveNotify, kVirtual, mode, child_toward(*w, from));
    crossing_event(*to, event::kEnterNotify, kInferior, mode, child_toward(*to, from));
  } else {
    Window* common = from->parent;
    while (common && !is_ancestor(common, to)) common = common->parent;
    crossing_event(*from, event::kLeaveNotify, kNonlinear, mode, nullptr);
    for (Window* w = from->parent; w && w != common; w = w->parent) crossing_event(*w, event::kLeaveNotify, kNonlinearVirtual, mode, child_toward(*w, from));
    for (Window* w = to->parent; w && w != common; w = w->parent) path.push_back(w);
    for (uint64_t i = path.count(); i > 0; --i)
      crossing_event(*path[i - 1], event::kEnterNotify, kNonlinearVirtual, mode, child_toward(*path[i - 1], to));
    crossing_event(*to, event::kEnterNotify, kNonlinear, mode, nullptr);
  }
}

void Server::pointer_moved() {
  if (!root_) return;
  Window* now_in = window_at(pointer_x_, pointer_y_);
  Window* before = find_window(pointer_window_);
  if (now_in != before) {
    crossing(before, now_in, 0);
    pointer_window_ = now_in ? now_in->id : kRootWindow;
  }
}

void Server::motion() {
  uint32_t wanted = mask::kPointerMotion;
  if (buttons_) wanted |= mask::kButtonMotion;
  for (uint32_t b = 0; b < 5; ++b)
    if (buttons_ & (state::kButton1 << b)) wanted |= mask::kButton1Motion << b;
  device_event(event::kMotionNotify, 0, wanted, find_window(pointer_window_), nullptr, false);
}

void Server::release_grabs_of(uint8_t client) {
  if (pointer_grab_.active && pointer_grab_.client == client) pointer_grab_ = Grab();
  if (keyboard_grab_.active && keyboard_grab_.client == client) keyboard_grab_ = Grab();
}

// ------------------------------------------------------------------ the desktop's input (desktop::IWindowsSession)

void Server::key(uint8_t hid_usage, bool pressed, uint8_t modifiers) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (hid_usage > kMaxKeycode - 8) return;
    const uint8_t keycode = static_cast<uint8_t>(hid_usage + 8);
    const uint8_t bit = static_cast<uint8_t>(1u << (keycode & 7));
    uint8_t& slot = keys_[keycode >> 3];
    if (!pressed && !(slot & bit)) return;      // (a release whose press was not ours)
    // the state the event carries is the one before it: a modifier key's own bit is not in its press and is in its release
    uint8_t before = modifiers;
    if (hid_usage >= 0xE0 && hid_usage <= 0xE7) {
      const uint8_t own = static_cast<uint8_t>(1u << (hid_usage - 0xE0));
      before = pressed ? static_cast<uint8_t>(modifiers & ~own) : static_cast<uint8_t>(modifiers | own);
    }
    modifiers_ = before;
    Window* source = nullptr;
    Window* stop = nullptr;
    Window* under = find_window(pointer_window_);
    if (focus_ == 1) {
      source = under;
    } else if (focus_ > 1) {
      Window* f = find_window(focus_);
      source = f && under && is_ancestor(f, under) ? under : f;
      stop = f;
    }
    device_event(pressed ? event::kKeyPress : event::kKeyRelease, keycode, pressed ? mask::kKeyPress : mask::kKeyRelease, source, stop, true);
    modifiers_ = modifiers;
    if (pressed) {
      slot = static_cast<uint8_t>(slot | bit);
      if (hid_usage == 0x39) caps_lock_ = !caps_lock_;
      if (hid_usage == 0x53) num_lock_ = !num_lock_;
    } else {
      slot = static_cast<uint8_t>(slot & ~bit);
    }
  }
  wake();
}

void Server::mouse_motion(int32_t dx, int32_t dy) {
  {
    kernel::ScopedMutex lock(mutex_);
    pointer_x_ = max32(0, min32(pointer_x_ + dx, static_cast<int32_t>(screen_w_) - 1));
    pointer_y_ = max32(0, min32(pointer_y_ + dy, static_cast<int32_t>(screen_h_) - 1));
    pointer_moved();
    motion();
  }
  wake();
}

void Server::mouse_position(int32_t x, int32_t y) {
  {
    kernel::ScopedMutex lock(mutex_);
    pointer_x_ = max32(0, min32(x, static_cast<int32_t>(screen_w_) - 1));
    pointer_y_ = max32(0, min32(y, static_cast<int32_t>(screen_h_) - 1));
    pointer_moved();
    motion();
  }
  wake();
}

void Server::press(uint8_t xbutton, bool down) {
  const uint16_t bit = static_cast<uint16_t>(state::kButton1 << (xbutton - 1));
  if (down == ((buttons_ & bit) != 0)) return;
  Window* under = find_window(pointer_window_);
  if (down) {
    device_event(event::kButtonPress, xbutton, mask::kButtonPress, under, nullptr, false);
    buttons_ = static_cast<uint16_t>(buttons_ | bit);
  } else {
    device_event(event::kButtonRelease, xbutton, mask::kButtonRelease, under, nullptr, false);
    buttons_ = static_cast<uint16_t>(buttons_ & ~bit);
    if ((buttons_ & 0x1F00) == 0 && pointer_grab_.active && pointer_grab_.implicit) {
      pointer_grab_ = Grab();
      pointer_moved();
    }
  }
}

void Server::mouse_button(uint32_t button, bool down) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (button < 1 || button > 3) return;
    press(button == 2 ? 3 : button == 3 ? 2 : 1, down);      // (the desktop's 2 is the right button: X's 3)
  }
  wake();
}

void Server::mouse_wheel(int32_t notches) {
  {
    kernel::ScopedMutex lock(mutex_);
    const uint8_t b = notches > 0 ? 4 : 5;      // (away from the user: button 4)
    for (int32_t n = notches > 0 ? notches : -notches; n > 0 && n < 64; --n) {
      press(b, true);
      press(b, false);
    }
  }
  wake();
}

void Server::cursor(int32_t* x, int32_t* y) {
  kernel::ScopedMutex lock(mutex_);
  *x = pointer_x_;
  *y = pointer_y_;
}

bool Server::cursor_confined() { return false; }

uint32_t Server::capture_window() {
  kernel::ScopedMutex lock(mutex_);
  if (!pointer_grab_.active) return 0;
  Window* top = top_of(find_window(pointer_grab_.window));
  return top ? top->id : kRootWindow;
}

}  // namespace x11
