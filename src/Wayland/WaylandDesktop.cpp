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

// The desktop's view of the compositor's windows (desktop::IWindowsSession): what they are, where, drawn how, and what the user does to their frames.
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

void Server::attach(int32_t frame_border, int32_t frame_title) {
  kernel::ScopedMutex lock(mutex_);
  attached_ = true;
  frame_border_ = frame_border;
  frame_title_ = frame_title;
  for (Surface* s : stack_.view()) note_changed(*s);
}

void Server::set_display(uint32_t width, uint32_t height, ArrayView<const Monitor> monitors) {
  {
    kernel::ScopedMutex lock(mutex_);
    if (width == 0 || height == 0 || width > 32767 || height > 32767) return;
    screen_w_ = width;
    screen_h_ = height;
    monitor_count_ = 0;
    for (const Monitor& m : monitors) {
      if (monitor_count_ == 8) break;
      monitors_[monitor_count_++] = {{m.rect.x, m.rect.y, static_cast<int32_t>(m.rect.width), static_cast<int32_t>(m.rect.height)},
                                     {m.work.x, m.work.y, static_cast<int32_t>(m.work.width), static_cast<int32_t>(m.work.height)}};
    }
    if (monitor_count_ == 0) {
      monitors_[0] = {{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)}, {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)}};
      monitor_count_ = 1;
    }
    // the outputs say so again; maximized and full-screen windows get the new size
    for (uint32_t i = 1; i < kMaxClients; ++i) {
      Client* c = clients_[i].get();
      if (!c) continue;
      List<uint32_t> outputs(allocator_);
      List<uint32_t> versions(allocator_);
      ObjectCursor cursor;
      for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
        const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
        if (kv->value.iface != Iface::Output) continue;
        outputs.push_back(kv->key);
        versions.push_back(kv->value.version);
      }
      for (uint64_t k = 0; k < outputs.count(); ++k) announce(*c, outputs[k], Iface::Output, versions[k]);
    }
    for (Surface* s : stack_.view())
      if (s->role == Role::Toplevel && (s->maximized || s->fullscreen)) {
        const Rect area = s->fullscreen ? monitor_for(*s) : work_area_for(*s);
        int32_t l, t, r, b;
        frame_margins(*s, &l, &t, &r, &b);
        s->x = area.x + (s->fullscreen ? 0 : l);
        s->y = area.y + (s->fullscreen ? 0 : t);
        configure_toplevel(*s);
        note_changed(*s);
      }
  }
  wake();
}

void Server::take_changes(List<uint32_t>* changed, List<Repaint>* repainted) {
  kernel::ScopedMutex lock(mutex_);
  changed->clear();
  repainted->clear();
  if (changes_overflow_) {
    for (Surface* s : stack_.view()) {
      changed->push_back(s->window);
      const Rect r = content_rect(*s);
      repainted->push_back({s->window, {0, 0, static_cast<uint32_t>(max32(r.w, 1)), static_cast<uint32_t>(max32(r.h, 1))}});
    }
  }
  for (const Change& c : changes_.view()) {
    if (c.changed) changed->push_back(c.window);
    if (c.repaint && !c.dirty.empty())
      repainted->push_back({c.window, {c.dirty.x, c.dirty.y, static_cast<uint32_t>(c.dirty.w), static_cast<uint32_t>(c.dirty.h)}});
  }
  changes_.clear();
  changes_overflow_ = false;
}

uint32_t Server::take_drag(uint32_t* edge) {
  kernel::ScopedMutex lock(mutex_);
  const uint32_t w = drag_window_;
  *edge = drag_edge_;
  drag_window_ = 0;
  return w;
}

void Server::windows(List<uint32_t>* out) {
  kernel::ScopedMutex lock(mutex_);
  out->clear();
  for (Surface* s : stack_.view()) out->push_back(s->window);
}

bool Server::window_info(uint32_t window, desktop::WindowsWindowInfo* out, String* title) {
  kernel::ScopedMutex lock(mutex_);
  *out = desktop::WindowsWindowInfo();
  title->clear();
  Surface* s = window_surface(window);
  if (!s) return false;
  const bool toplevel = s->role == Role::Toplevel;
  out->exists = true;
  out->visible = s->mapped;
  out->minimized = s->iconic;
  out->maximized = s->maximized && !s->fullscreen;
  out->active = toplevel && s->window == active_window_;
  out->caption = decorated(*s);
  const bool fixed = s->min_w > 0 && s->min_h > 0 && s->min_w == s->max_w && s->min_h == s->max_h;
  out->sizable = toplevel && !fixed && !s->fullscreen;
  out->minimize_box = toplevel;
  out->maximize_box = out->sizable;
  out->popup = s->role == Role::Popup;
  out->owned = out->popup || s->parent_window != 0;
  int32_t l, t, r, b;
  frame_margins(*s, &l, &t, &r, &b);
  const Rect c = content_rect(*s);
  out->surface = {c.x, c.y, static_cast<uint32_t>(max32(c.w, 0)), static_cast<uint32_t>(max32(c.h, 0))};
  out->window = {c.x - l, c.y - t, static_cast<uint32_t>(max32(c.w, 0) + l + r), static_cast<uint32_t>(max32(c.h, 0) + t + b)};
  if (toplevel) title->append(s->title.view());
  return true;
}

bool Server::draw_window(uint32_t window, graphics::IRenderer2D& renderer, int32_t x, int32_t y, uint32_t* width, uint32_t* height) {
  {
    kernel::ScopedMutex lock(mutex_);
    Surface* s = window_surface(window);
    if (!s || !s->mapped || !s->current.valid()) return false;
    List<uint32_t> done(allocator_);
    const int32_t gx = s->geometry_set ? s->geometry.x : 0, gy = s->geometry_set ? s->geometry.y : 0;
    draw_missed_ = false;
    draw_tree(*s, renderer, x - gx, y - gy, true, &done);
    renderer.set_blend(graphics::BlendMode::Alpha);
    int32_t w = 0, h = 0;
    actual_size(*s, &w, &h);
    if (draw_missed_) note_repaint(*s, {0, 0, w, h});      // (a GPU picture that was still being drawn: the desktop comes back for it)
    *width = static_cast<uint32_t>(max32(w, 0));
    *height = static_cast<uint32_t>(max32(h, 0));
    // the frame callbacks of what was drawn: the client may draw the next frame
    if (Client* c = client_of(s->client); c && done.count() > 0) {
      const uint32_t time = now();
      for (uint32_t cb : done.view()) {
        Event e(cb, 0);
        e.u(time);
        send(*c, e);
        c->objects.remove(cb);
        delete_id(*c, cb);
      }
    }
  }
  wake();
  return true;
}

void Server::move_window(uint32_t window, const desktop::WindowsRect& rect) {
  {
    kernel::ScopedMutex lock(mutex_);
    Surface* s = window_surface(window);
    if (!s || s->role != Role::Toplevel || s->maximized || s->fullscreen) return;
    int32_t l, t, r, b;
    frame_margins(*s, &l, &t, &r, &b);
    int32_t w = static_cast<int32_t>(rect.width) - l - r, h = static_cast<int32_t>(rect.height) - t - b;
    if (s->min_w > 0 && w < s->min_w) w = s->min_w;
    if (s->min_h > 0 && h < s->min_h) h = s->min_h;
    if (s->max_w > 0 && w > s->max_w) w = s->max_w;
    if (s->max_h > 0 && h > s->max_h) h = s->max_h;
    w = max32(1, min32(w, kMaxSize));
    h = max32(1, min32(h, kMaxSize));
    s->x = rect.x + l;
    s->y = rect.y + t;
    int32_t shown_w = 0, shown_h = 0;
    shown_size(*s, &shown_w, &shown_h);
    if (w != shown_w || h != shown_h) {      // the client is asked to draw that size; the frame shows it meanwhile
      s->want_w = w;
      s->want_h = h;
      if (s->initial_configure) configure_toplevel(*s);
    }
    note_changed(*s);
    for (Surface* p : stack_.view()) if (p->role == Role::Popup && root_toplevel(p) == s) note_changed(*p);      // (they move with it)
  }
  wake();
}

void Server::activate_window(uint32_t window) {
  {
    kernel::ScopedMutex lock(mutex_);
    Surface* s = window_surface(window);
    if (s && s->role != Role::Toplevel) s = nullptr;      // (popups are not activated: their toplevel stays active)
    if (window != 0 && (!s || !s->mapped)) return;
    Surface* old = window_surface(active_window_);
    if (s) raise(*s);
    if (old != s) {
      if (old && old->role == Role::Toplevel) {
        old->activated = false;
        if (old->initial_configure) configure_toplevel(*old);
        note_changed(*old);
      }
      dismiss_popups(s);
      active_window_ = s ? s->window : 0;
      if (s) {
        s->activated = true;
        if (s->initial_configure) configure_toplevel(*s);
        note_changed(*s);
      }
    }
    // the keyboard: the topmost popup that grabbed it, else the window
    Surface* focus = s;
    for (Surface* w : stack_.view()) if (s && w->role == Role::Popup && w->grab && root_toplevel(w) == s) focus = w;
    set_keyboard_focus(focus);
  }
  wake();
}

void Server::window_command(uint32_t window, Command command) {
  {
    kernel::ScopedMutex lock(mutex_);
    Surface* s = window_surface(window);
    if (!s || s->role != Role::Toplevel) return;
    switch (command) {
      case Command::Close:
        if (Client* c = client_of(s->client)) send(*c, Event(s->role_id, 1));
        break;
      case Command::Minimize:
        s->iconic = true;
        dismiss_children(*s);
        note_changed(*s);
        break;
      case Command::Maximize:
        s->iconic = false;
        set_maximized(*s, true);
        note_changed(*s);
        break;
      case Command::Restore:
        if (s->iconic) s->iconic = false;
        else if (s->fullscreen) set_fullscreen(*s, false);
        else set_maximized(*s, false);
        note_changed(*s);
        break;
    }
  }
  wake();
}

uint32_t Server::window_pixel(uint32_t window, int32_t x, int32_t y) {
  kernel::ScopedMutex lock(mutex_);
  Surface* s = window_surface(window);
  if (!s || !s->current.valid()) return 0;
  const int32_t bx = x + (s->geometry_set ? s->geometry.x : 0), by = y + (s->geometry_set ? s->geometry.y : 0);
  if (bx < 0 || by < 0 || bx >= s->current.width || by >= s->current.height) return 0;
  if (GpuImage* image = s->current.image()) {      // (0xAARRGGBB whatever the image's order)
    uint32_t pixel = 0;
    return image->read({bx, by, 1, 1}, ArrayView<uint32_t>(&pixel, 1)) ? pixel : 0;
  }
  ArrayView<uint32_t> pixels = buffer_pixels(s->current);
  const uint64_t at = uint64_t(by) * uint64_t(s->current.stride / 4) + uint64_t(bx);
  return at < pixels.size() ? pixels[at] : 0;
}

}  // namespace wayland
