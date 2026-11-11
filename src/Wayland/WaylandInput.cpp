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

// The seat: the desktop's pointer and keyboard as wl_pointer and wl_keyboard events, and the cursor the clients set.
//
// Pointer focus is the surface under the pointer (subsurfaces and input regions included); while a button is held it stays where the press went (the
// implicit grab), and the desktop keeps sending the pointer here (capture_window). Keyboard focus is the active toplevel, or the topmost popup of it that
// grabbed the keyboard. Keys repeat on the client's side (repeat_info), so the desktop's repeated presses of a key already down are dropped.
#include "Wayland/WaylandInternal.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace wayland {

namespace {

constexpr uint32_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112;

}  // namespace

// ------------------------------------------------------------------ where the pointer is

Server::Surface* Server::hit(Surface& s, int32_t x, int32_t y, int32_t* local_x, int32_t* local_y) {
  for (uint64_t i = s.stack.count(); i > 0; --i) {
    Surface* entry = s.stack[i - 1];
    if (entry) {
      if (entry->role != Role::Subsurface) continue;
      if (Surface* found = hit(*entry, x - entry->sub_x, y - entry->sub_y, local_x, local_y)) return found;
      continue;
    }
    int32_t sw = 0, sh = 0;
    surface_size(s.current, s.mapping, &sw, &sh);
    if (!s.current.valid() || x < 0 || y < 0 || x >= sw || y >= sh) continue;
    if (s.input && !s.input->contains(x, y)) continue;
    *local_x = x;
    *local_y = y;
    return &s;
  }
  return nullptr;
}

Server::Surface* Server::surface_at(int32_t x, int32_t y, int32_t* local_x, int32_t* local_y) {
  for (uint64_t i = stack_.count(); i > 0; --i) {
    Surface& w = *stack_[i - 1];
    if (!w.mapped || w.iconic) continue;
    if (!contains(content_rect(w), x, y)) continue;
    int32_t ox = 0, oy = 0;
    surface_origin(w, &ox, &oy);
    return hit(w, x - ox, y - oy, local_x, local_y);      // (inside the window but outside its input: nobody's)
  }
  return nullptr;
}

void Server::pointer_frame(Client& c) {
  ObjectCursor cursor;
  for (bool more = c.objects.first(&cursor); more; more = c.objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c.objects.element(cursor);
    if (kv->value.iface == Iface::Pointer && kv->value.version >= 5) send(c, Event(kv->key, 5));
  }
}

void Server::set_pointer_focus(Surface* s, int32_t lx, int32_t ly) {
  if (s == pointer_focus_) return;
  if (pointer_focus_) {
    Surface& old = *pointer_focus_;
    if (Client* c = client_of(old.client)) {
      const uint32_t serial = next_serial();
      ObjectCursor cursor;
      for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
        const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
        if (kv->value.iface != Iface::Pointer) continue;
        Event e(kv->key, 1);
        e.u(serial).u(old.id);
        send(*c, e);
      }
      pointer_frame(*c);
    }
  }
  pointer_focus_ = s;
  cursor_set_ = false;      // (the system arrow until the new client sets one)
  cursor_surface_ = nullptr;
  ++cursor_handle_;
  pointer_lx_ = lx;
  pointer_ly_ = ly;
  if (!s) return;
  Client* c = client_of(s->client);
  if (!c) return;
  const uint32_t serial = next_serial();
  ObjectCursor cursor;
  for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
    if (kv->value.iface != Iface::Pointer) continue;
    Event e(kv->key, 0);
    e.u(serial).u(s->id).fixed(lx).fixed(ly);
    send(*c, e);
  }
  pointer_frame(*c);
}

// The pointer's surface again (after it moved, or the windows changed under it); `motion`: tell the focus where the pointer is now.
void Server::pointer_moved(bool motion) {
  if (drag_.active) {      // (a drag has the pointer: the data devices hear of it, not the pointers)
    drag_moved();
    return;
  }
  int32_t lx = 0, ly = 0;
  Surface* under = nullptr;
  if (buttons_ != 0 && pointer_focus_) {      // the implicit grab: the press's surface keeps it
    under = pointer_focus_;
    int32_t ox = 0, oy = 0;
    surface_origin(*under, &ox, &oy);
    lx = pointer_x_ - ox;
    ly = pointer_y_ - oy;
  } else {
    under = surface_at(pointer_x_, pointer_y_, &lx, &ly);
  }
  if (under != pointer_focus_) {
    set_pointer_focus(under, lx, ly);
    return;
  }
  if (!under || !motion || (lx == pointer_lx_ && ly == pointer_ly_)) return;
  pointer_lx_ = lx;
  pointer_ly_ = ly;
  Client* c = client_of(under->client);
  if (!c) return;
  const uint32_t time = now();
  ObjectCursor cursor;
  for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
    if (kv->value.iface != Iface::Pointer) continue;
    Event e(kv->key, 2);
    e.u(time).fixed(lx).fixed(ly);
    send(*c, e);
  }
  pointer_frame(*c);
}

// ------------------------------------------------------------------ the desktop's pointer (desktop::IWindowsSession)

void Server::mouse_motion(int32_t dx, int32_t dy) {
  {
    kernel::ScopedMutex lock(mutex_);
    pointer_x_ = max32(0, min32(pointer_x_ + dx, static_cast<int32_t>(screen_w_) - 1));
    pointer_y_ = max32(0, min32(pointer_y_ + dy, static_cast<int32_t>(screen_h_) - 1));
    pointer_moved(true);
  }
  wake();
}

void Server::mouse_position(int32_t x, int32_t y) {
  {
    kernel::ScopedMutex lock(mutex_);
    pointer_x_ = max32(0, min32(x, static_cast<int32_t>(screen_w_) - 1));
    pointer_y_ = max32(0, min32(y, static_cast<int32_t>(screen_h_) - 1));
    pointer_moved(true);
  }
  wake();
}

void Server::mouse_button(uint32_t button, bool down) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (button < 1 || button > 3) return;
    const uint32_t bit = 1u << (button - 1);
    if (down == ((buttons_ & bit) != 0)) return;
    if (drag_.active) {      // a drag: its end is the buttons' release (no button events go to the clients meanwhile)
      if (down) buttons_ |= bit;
      else buttons_ &= ~bit;
      if (buttons_ == 0) drag_button_released();
      goto done;
    }
    if (down) {
      // a press outside the chain of the open popups closes them (a press in the chain's toplevel too, as menus expect)
      Surface* w = pointer_focus_;
      for (uint32_t guard = 0; w && w->role == Role::Subsurface && guard < 64; ++guard) w = w->parent;
      if (!w || w->role != Role::Popup) {
        bool any = false;
        for (Surface* p : stack_.view()) if (p->role == Role::Popup) any = true;
        if (any) dismiss_popups(nullptr);
      }
      if (buttons_ == 0) pointer_moved(false);      // (the focus is where the press is)
    }
    if (Surface* s = pointer_focus_) {
      if (Client* c = client_of(s->client)) {
        const uint32_t serial = next_serial(), time = now();
        const uint32_t code = button == 1 ? kBtnLeft : button == 2 ? kBtnRight : kBtnMiddle;      // (the desktop's 2 is the right button)
        ObjectCursor cursor;
        for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
          const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
          if (kv->value.iface != Iface::Pointer) continue;
          Event e(kv->key, 3);
          e.u(serial).u(time).u(code).u(down ? 1 : 0);
          send(*c, e);
        }
        pointer_frame(*c);
      }
    }
    if (down) buttons_ |= bit;
    else buttons_ &= ~bit;
    if (buttons_ == 0) pointer_moved(false);      // the grab is over: the focus may move on
  }
done:
  wake();
}

void Server::mouse_wheel(int32_t notches) {
  {
    kernel::ScopedMutex lock(mutex_);
    Surface* s = pointer_focus_;
    Client* c = s ? client_of(s->client) : nullptr;
    if (!c || notches == 0) return;
    const int32_t n = max32(-64, min32(-notches, 64));      // (away from the user, +: content up, which is a negative axis value)
    const uint32_t time = now();
    ObjectCursor cursor;
    for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
      const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
      if (kv->value.iface != Iface::Pointer) continue;
      const uint32_t v = kv->value.version;
      if (v >= 5) {
        Event source(kv->key, 6);
        source.u(0);      // wheel
        send(*c, source);
      }
      if (v >= 8) {
        Event value120(kv->key, 9);
        value120.u(0).i(n * 120);
        send(*c, value120);
      } else if (v >= 5) {
        Event discrete(kv->key, 8);
        discrete.u(0).i(n);
        send(*c, discrete);
      }
      Event axis(kv->key, 4);
      axis.u(time).u(0).fixed(n * 15);
      send(*c, axis);
    }
    pointer_frame(*c);
  }
  wake();
}

void Server::mouse_left() {
  {
    kernel::ScopedMutex lock(mutex_);
    if (buttons_ == 0) set_pointer_focus(nullptr, 0, 0);
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
  if (buttons_ == 0 || !pointer_focus_) return 0;
  Surface* w = pointer_focus_;
  for (uint32_t guard = 0; w && w->role == Role::Subsurface && guard < 64; ++guard) w = w->parent;
  return w ? w->window : 0;
}

void Server::pointer(Pointer* out) {
  kernel::ScopedMutex lock(mutex_);
  *out = Pointer();
  if (!pointer_focus_ || !cursor_set_) return;      // the system arrow
  if (!cursor_surface_ || !cursor_surface_->current.valid()) {
    out->hidden = true;
    return;
  }
  out->cursor = cursor_handle_;
}

bool Server::cursor_picture(uint32_t handle, uint32_t* system_id, List<uint32_t>* argb, uint32_t* width, uint32_t* height, int32_t* hot_x, int32_t* hot_y) {
  kernel::ScopedMutex lock(mutex_);
  argb->clear();
  *width = *height = 0;
  *hot_x = *hot_y = 0;
  *system_id = 0;
  if (handle != cursor_handle_ || !cursor_surface_ || !cursor_surface_->current.valid()) return false;
  const BufferInfo& b = cursor_surface_->current;
  if (b.width > 256 || b.height > 256) return false;
  ArrayView<uint32_t> pixels = buffer_pixels(b);
  uint64_t row = uint64_t(b.stride / 4);
  // a GPU cursor is read out first (as 0xAARRGGBB, rows of its width)
  List<uint32_t> gpu_pixels(allocator_);
  GpuImage* image = b.image();
  if (image) {
    const uint64_t count = uint64_t(b.width) * uint64_t(b.height);
    if (!gpu_pixels.reserve(count)) return false;
    for (uint64_t i = 0; i < count; ++i) gpu_pixels.push_back(0);
    if (!image->read({0, 0, static_cast<uint32_t>(b.width), static_cast<uint32_t>(b.height)}, gpu_pixels.view()))
      return false;
    pixels = gpu_pixels.view();
    row = uint64_t(b.width);
  }
  if (pixels.size() == 0 || !argb->reserve(uint64_t(b.width) * uint64_t(b.height))) return false;
  const bool swap = !image && (b.format == kFormatAbgr8888 || b.format == kFormatXbgr8888);
  const bool alpha = b.format == kFormatArgb8888 || b.format == kFormatAbgr8888;
  for (int32_t y = 0; y < b.height; ++y) {
    for (int32_t x = 0; x < b.width; ++x) {
      uint32_t p = pixels[uint64_t(y) * row + uint64_t(x)];
      if (swap) p = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
      if (!alpha) {
        p |= 0xFF000000u;
      } else if (const uint32_t a = p >> 24; a != 0 && a != 255) {      // premultiplied -> straight
        auto un = [a](uint32_t c) { const uint32_t v = (c * 255 + a / 2) / a; return v > 255 ? 255u : v; };
        p = (a << 24) | (un((p >> 16) & 0xFF) << 16) | (un((p >> 8) & 0xFF) << 8) | un(p & 0xFF);
      }
      argb->push_back(p);
    }
  }
  *width = static_cast<uint32_t>(b.width);
  *height = static_cast<uint32_t>(b.height);
  *hot_x = cursor_hot_x_;
  *hot_y = cursor_hot_y_;
  return true;
}

// ------------------------------------------------------------------ the keyboard

uint32_t Server::modifier_mask(bool locked) const {
  if (locked) return (caps_lock_ ? 2u : 0u) | (num_lock_ ? 16u : 0u);      // Lock, Mod2 (NumLock)
  uint32_t m = 0;
  if (modifiers_ & hardware::modifier::kShift) m |= 1;       // Shift
  if (modifiers_ & hardware::modifier::kControl) m |= 4;     // Control
  if (modifiers_ & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt)) m |= 8;       // Mod1 (Alt)
  if (modifiers_ & (hardware::modifier::kLeftGui | hardware::modifier::kRightGui)) m |= 64;      // Mod4 (Super)
  return m;
}

void Server::send_modifiers(Client& c) {
  const uint32_t depressed = modifier_mask(false), locked = modifier_mask(true);
  if (last_mods_[0] == depressed && last_mods_[1] == locked) return;
  last_mods_[0] = depressed;
  last_mods_[1] = locked;
  const uint32_t serial = next_serial();
  ObjectCursor cursor;
  for (bool more = c.objects.first(&cursor); more; more = c.objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c.objects.element(cursor);
    if (kv->value.iface != Iface::Keyboard) continue;
    Event e(kv->key, 4);
    e.u(serial).u(depressed).u(0).u(locked).u(0);
    send(c, e);
  }
}

void Server::keyboard_enter(Client& c, Resource& keyboard, uint32_t keyboard_id, Surface& s) {
  (void)keyboard;
  uint8_t keys[4 * 64] = {};
  uint32_t n = 0;
  for (uint32_t usage = 0; usage < 256 && n < 64; ++usage) {
    if (!(keys_[usage >> 3] & (1u << (usage & 7)))) continue;
    const uint32_t code = evdev_code(static_cast<uint8_t>(usage));
    if (code == 0) continue;
    for (uint32_t k = 0; k < 4; ++k) keys[n * 4 + k] = static_cast<uint8_t>(code >> (8 * k));
    ++n;
  }
  Event e(keyboard_id, 1);
  e.u(next_serial()).u(s.id).array(ConstByteView(keys, n * 4));
  send(c, e);
  Event m(keyboard_id, 4);
  m.u(next_serial()).u(modifier_mask(false)).u(0).u(modifier_mask(true)).u(0);
  send(c, m);
}

void Server::set_keyboard_focus(Surface* s) {
  if (s == keyboard_focus_) return;
  if (keyboard_focus_) {
    Surface& old = *keyboard_focus_;
    if (Client* c = client_of(old.client)) {
      const uint32_t serial = next_serial();
      ObjectCursor cursor;
      for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
        const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
        if (kv->value.iface != Iface::Keyboard) continue;
        Event e(kv->key, 2);
        e.u(serial).u(old.id);
        send(*c, e);
      }
    }
  }
  keyboard_focus_ = s;
  last_mods_[0] = last_mods_[1] = ~0u;
  if (!s) return;
  Client* c = client_of(s->client);
  if (!c) return;
  if (sel_sent_client_ != c->number) send_selection(*c);      // (a client that gets the keyboard is offered the selection)
  List<uint32_t> keyboards(allocator_);
  ObjectCursor cursor;
  for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
    const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
    if (kv->value.iface == Iface::Keyboard) keyboards.push_back(kv->key);
  }
  for (uint32_t k : keyboards.view())
    if (Resource* r = c->objects.find(k)) keyboard_enter(*c, *r, k, *s);
  last_mods_[0] = modifier_mask(false);
  last_mods_[1] = modifier_mask(true);
}

void Server::key(uint8_t hid_usage, bool pressed, uint8_t modifiers) {
  {
    kernel::ScopedMutex lock(mutex_);
    const uint32_t code = evdev_code(hid_usage);
    const uint8_t bit = static_cast<uint8_t>(1u << (hid_usage & 7));
    uint8_t& slot = keys_[hid_usage >> 3];
    if (pressed == ((slot & bit) != 0)) return;      // (a repeat of a key held, or a release whose press was not ours)
    if (pressed) {
      slot = static_cast<uint8_t>(slot | bit);
      if (hid_usage == 0x39) caps_lock_ = !caps_lock_;
      if (hid_usage == 0x53) num_lock_ = !num_lock_;
    } else {
      slot = static_cast<uint8_t>(slot & ~bit);
    }
    modifiers_ = modifiers;
    Surface* s = keyboard_focus_;
    Client* c = s ? client_of(s->client) : nullptr;
    if (!c || code == 0) return;
    const uint32_t serial = next_serial(), time = now();
    ObjectCursor cursor;
    for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
      const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
      if (kv->value.iface != Iface::Keyboard) continue;
      Event e(kv->key, 3);
      e.u(serial).u(time).u(code).u(pressed ? 1 : 0);
      send(*c, e);
    }
    send_modifiers(*c);
  }
  wake();
}

// ------------------------------------------------------------------ wl_seat, wl_pointer, wl_keyboard, wl_touch

void Server::seat_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (r.iface != Iface::Seat) {      // wl_keyboard / wl_touch: release
    if (opcode == 0) {
      if (r.iface == Iface::Keyboard || r.iface == Iface::Touch) destroy_object(c, id);
    } else {
      post_error(c, id, kErrorInvalidMethod, "no such request");
    }
    return;
  }
  if (opcode == 3) {      // release (5)
    destroy_object(c, id);
    return;
  }
  if (opcode > 2) {
    post_error(c, id, kErrorInvalidMethod, "wl_seat has no such request");
    return;
  }
  const uint32_t object = in.u();
  if (!in.ok()) return;
  const Iface kinds[3] = {Iface::Pointer, Iface::Keyboard, Iface::Touch};
  const uint32_t version = r.version;
  Resource* nr = new_object(c, object, kinds[opcode], version, id);
  if (!nr) return;
  if (opcode == 0) {
    if (pointer_focus_ && pointer_focus_->client == c.number) {      // (the pointer is over this client already)
      Event e(object, 0);
      e.u(next_serial()).u(pointer_focus_->id).fixed(pointer_lx_).fixed(pointer_ly_);
      send(c, e);
      if (version >= 5) send(c, Event(object, 5));
    }
  } else if (opcode == 1) {
    const ConstByteView text = keymap_text();
    if (Fd fd = keymap_fd()) {
      Event e(object, 0);
      e.u(1).fd(fd).u(static_cast<uint32_t>(text.size()));      // xkb_v1, the text with its NUL
      send(c, e);
    }      // (no way to make a descriptor: the client gets no keymap and keeps its own)
    if (version >= 4) {
      Event repeat(object, 5);
      repeat.i(25).i(600);      // 25 per second after 600 ms
      send(c, repeat);
    }
    if (keyboard_focus_ && keyboard_focus_->client == c.number) keyboard_enter(c, *nr, object, *keyboard_focus_);
  }
}

void Server::pointer_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  (void)r;
  if (opcode == 1) {      // release
    destroy_object(c, id);
    return;
  }
  if (opcode != 0) {
    post_error(c, id, kErrorInvalidMethod, "wl_pointer has no such request");
    return;
  }
  in.u();      // serial (of the enter it answers)
  const uint32_t surface = in.object();
  const int32_t hx = in.i(), hy = in.i();
  if (!in.ok()) return;
  Surface* s = nullptr;
  if (surface != 0) {
    Resource* sr = object_of(c, surface, Iface::Surface, id);
    if (!sr || !sr->surface) return;
    s = sr->surface;
    if (s->role != Role::None && s->role != Role::Cursor) {
      post_error(c, id, 0, "the surface has another role");      // wl_pointer.role
      return;
    }
    s->role = Role::Cursor;
  }
  if (!pointer_focus_ || pointer_focus_->client != c.number) return;      // (only the client the pointer is over sets it)
  cursor_set_ = true;
  cursor_surface_ = s;
  cursor_hot_x_ = max32(-256, min32(hx, 256));
  cursor_hot_y_ = max32(-256, min32(hy, 256));
  ++cursor_handle_;
}

}  // namespace wayland
