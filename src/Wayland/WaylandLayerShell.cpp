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

// zwlr_layer_shell_v1 (version 4): panels, docks, backgrounds, notifications and lock-screen-like overlays. A layer surface is a desktop window in one of
// the bands of the desktop's stack (gui::Frame::Layer: background and bottom below the normal windows, top and overlay above them), placed by the
// compositor - not by the user - from its anchors, size and margins inside its monitor's panel area (the work area before any window's reservation: the
// desktop's taskbar is never covered).
//
// Exclusive zones: a surface anchored to one edge (or to one edge and both of its neighbours) with a positive zone keeps zone + that edge's margin along the
// edge. The desktop leaves it out of every session's work area (WindowsWindowInfo::reserve), so maximized Wayland, Windows and X windows stay clear of it.
// Among the layer surfaces, those with zone 0 are placed inside the panel area less the other surfaces' zones; those with a positive zone or -1 use all of
// the panel area (as wlroots does it, without the order of arrival).
//
// Keyboard interactivity: none - the desktop never activates it (a popup to the desktop: clicks reach it, keys never do); on_demand - it is activated like
// any window when clicked; exclusive - it is activated when it is mapped (in the top or overlay layer: the protocol's rule for taking the keyboard).
//
// What is checked: a surface with another role or with a buffer (role, already_constructed), the layer (invalid_layer, also for set_layer), the anchor bits
// (invalid_anchor), the keyboard interactivity (invalid_keyboard_interactivity, on_demand from version 4), a size of 0 without both anchors of that axis at
// commit (invalid_size), a buffer before the first configure (invalid_surface_state). get_popup gives an xdg_popup made without a parent this surface as
// its parent. One wl_output: the output argument is taken (it must be one) and the primary monitor is used.
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

constexpr uint32_t kAnchorTop = 1, kAnchorBottom = 2, kAnchorLeft = 4, kAnchorRight = 8;
// zwlr_layer_shell_v1 errors; zwlr_layer_surface_v1 errors
constexpr uint32_t kShellRole = 0, kShellInvalidLayer = 1, kShellAlreadyConstructed = 2;
constexpr uint32_t kLayerInvalidSurfaceState = 0, kLayerInvalidSize = 1, kLayerInvalidAnchor = 2, kLayerInvalidKeyboard = 3;
constexpr int32_t kMaxMargin = 1 << 20;

int32_t clamp_margin(int32_t m) { return m < -kMaxMargin ? -kMaxMargin : m > kMaxMargin ? kMaxMargin : m; }

// The edge a surface keeps (Frame::Edge: 1 left, 2 top, 3 right, 4 bottom), 0 for none: anchored to that edge alone, or to it and both its neighbours.
uint8_t exclusive_edge(uint32_t anchor) {
  switch (anchor) {
    case kAnchorTop: case kAnchorTop | kAnchorLeft | kAnchorRight: return 2;
    case kAnchorBottom: case kAnchorBottom | kAnchorLeft | kAnchorRight: return 4;
    case kAnchorLeft: case kAnchorLeft | kAnchorTop | kAnchorBottom: return 1;
    case kAnchorRight: case kAnchorRight | kAnchorTop | kAnchorBottom: return 3;
    default: return 0;
  }
}

// One axis: where a surface of `size` goes in [start, start + length) with the margins of its two ends and whether it is anchored to each.
int32_t place_axis(int32_t start, int32_t length, int32_t size, bool low, bool high, int32_t margin_low, int32_t margin_high) {
  if (low && !high) return start + margin_low;
  if (high && !low) return start + length - margin_high - size;
  if (low && high) return start + margin_low + (length - margin_low - margin_high - size) / 2;      // (stretched, or centred between its margins)
  return start + (length - size) / 2;
}

}  // namespace

void Server::arrange_layers() {
  const Rect panel = monitors_[0].panel.empty() ? monitors_[0].work : monitors_[0].panel;
  // what the surfaces with exclusive zones keep, from each edge of the panel area (several on one edge: they add up, as in wlroots)
  int32_t kept[5] = {0, 0, 0, 0, 0};
  for (Surface& s : surfaces_) {
    s.reserve_edge = 0;
    s.reserve = 0;
    if (s.role != Role::Layer || s.role_id == 0 || !s.mapped) continue;
    const LayerConfig& k = s.layer_current;
    const uint8_t edge = exclusive_edge(k.anchor);
    if (k.zone <= 0 || edge == 0) continue;
    const int32_t margin = edge == 1 ? k.margin[3] : edge == 2 ? k.margin[0] : edge == 3 ? k.margin[1] : k.margin[2];
    const int64_t total = int64_t{k.zone} + (margin > 0 ? margin : 0);
    s.reserve_edge = edge;
    s.reserve = static_cast<uint32_t>(total > kMaxSize ? kMaxSize : total);
    kept[edge] = static_cast<int32_t>(kept[edge] + int64_t{s.reserve} > kMaxSize ? kMaxSize : kept[edge] + int64_t{s.reserve});
  }
  Rect usable = panel;
  usable.x += kept[1];
  usable.y += kept[2];
  usable.w = max32(1, usable.w - kept[1] - kept[3]);
  usable.h = max32(1, usable.h - kept[2] - kept[4]);
  for (Surface& s : surfaces_) {
    if (s.role != Role::Layer || s.role_id == 0 || !s.initial_configure) continue;
    const LayerConfig& k = s.layer_current;
    const Rect area = k.zone == 0 ? usable : panel;
    const bool top = k.anchor & kAnchorTop, bottom = k.anchor & kAnchorBottom, left = k.anchor & kAnchorLeft, right = k.anchor & kAnchorRight;
    // the size asked for: the client's, or what its two anchors span
    int32_t w = k.width ? static_cast<int32_t>(k.width) : area.w - (left ? k.margin[3] : 0) - (right ? k.margin[1] : 0);      // (set_size keeps them in range)
    int32_t h = k.height ? static_cast<int32_t>(k.height) : area.h - (top ? k.margin[0] : 0) - (bottom ? k.margin[2] : 0);
    w = max32(1, min32(w, kMaxSize));
    h = max32(1, min32(h, kMaxSize));
    if (w != s.layer_w || h != s.layer_h) {
      s.layer_w = w;
      s.layer_h = h;
      if (Client* c = client_of(s.client)) {
        Event e(s.role_id, 0);      // configure(serial, width, height)
        s.last_serial = next_serial();
        e.u(s.last_serial).u(uint32_t(w)).u(uint32_t(h));
        send(*c, e);
      }
    }
    // the place, for what it shows (its own size once it has drawn one)
    int32_t sw = w, sh = h;
    if (s.mapped) actual_size(s, &sw, &sh);
    const int32_t x = place_axis(area.x, area.w, sw, left, right, left ? k.margin[3] : 0, right ? k.margin[1] : 0);
    const int32_t y = place_axis(area.y, area.h, sh, top, bottom, top ? k.margin[0] : 0, bottom ? k.margin[2] : 0);
    if (x != s.x || y != s.y) {
      s.x = x;
      s.y = y;
      if (s.mapped) note_changed(s);
      for (Surface* p : stack_.view()) if (p->role == Role::Popup && root_toplevel(p) == &s) note_changed(*p);      // (they move with it)
    }
  }
}

bool Server::commit_layer(Client& c, Surface& s, bool had_buffer) {
  if (s.role_id == 0) return true;      // (its zwlr_layer_surface_v1 is gone: the surface shows nothing)
  const LayerConfig& k = s.layer_pending;
  if (k.width == 0 && !((k.anchor & kAnchorLeft) && (k.anchor & kAnchorRight))) {
    post_error(c, s.role_id, kLayerInvalidSize, "width 0 needs both the left and right anchors");
    return false;
  }
  if (k.height == 0 && !((k.anchor & kAnchorTop) && (k.anchor & kAnchorBottom))) {
    post_error(c, s.role_id, kLayerInvalidSize, "height 0 needs both the top and bottom anchors");
    return false;
  }
  const bool changed = !(s.layer_current == k);
  const bool layer_changed = s.layer_current.layer != k.layer || s.layer_current.keyboard != k.keyboard;
  s.layer_current = k;
  if (!s.initial_configure) {
    if (s.current.valid()) {
      post_error(c, s.role_id, kLayerInvalidSurfaceState, "a buffer before the first configure");
      return false;
    }
    s.initial_configure = true;
    s.layer_w = s.layer_h = 0;
    arrange_layers();      // (sends the first configure)
    return true;
  }
  int32_t before_w = 0, before_h = 0;
  if (s.mapped) actual_size(s, &before_w, &before_h);
  if (s.current.valid() && !s.mapped) {
    map_window(s);
    if (s.layer_current.keyboard == 1 && s.layer_current.layer >= 2) activate_locked(&s);      // (exclusive: it takes the keyboard)
  } else if (!s.current.valid() && had_buffer && s.mapped) {      // a null buffer unmaps it; it starts over with a new initial commit
    unmap(s);
    s.initial_configure = false;
  }
  int32_t after_w = 0, after_h = 0;
  if (s.mapped) actual_size(s, &after_w, &after_h);
  if (changed || after_w != before_w || after_h != before_h || !s.mapped) arrange_layers();
  if (changed || layer_changed) note_changed(s);
  return true;
}

void Server::layer_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (r.iface == Iface::LayerShell) {
    switch (opcode) {
      case 0: {      // get_layer_surface(new id, surface, output, layer, namespace)
        const uint32_t object = in.u(), surface = in.object(), output = in.object(), layer = in.u();
        const StringView name = in.string();
        if (!in.ok()) return;
        Resource* sr = object_of(c, surface, Iface::Surface, id);
        if (!sr || !sr->surface) return;
        if (output != 0 && !object_of(c, output, Iface::Output, id)) return;
        Surface* s = sr->surface;
        if (s->role != Role::None || s->role_id != 0) {
          post_error(c, id, kShellRole, "the surface has another role");
          return;
        }
        if (s->current.valid() || (s->pending.attached && s->pending.buffer.valid())) {
          post_error(c, id, kShellAlreadyConstructed, "the surface has a buffer");
          return;
        }
        if (layer > 3) {
          post_error(c, id, kShellInvalidLayer, "no such layer");
          return;
        }
        Resource* nr = new_object(c, object, Iface::LayerSurface, r.version, id);
        if (!nr) return;
        nr->surface = s;
        s->role = Role::Layer;
        s->role_id = object;
        s->window = next_window_++;
        s->initial_configure = s->mapped = false;
        s->layer_pending = LayerConfig();
        s->layer_pending.layer = layer;
        s->layer_current = s->layer_pending;
        s->title.clear();
        s->title.append(name.size() > 256 ? name.first(256) : name);      // (the namespace: what the desktop shows as its title)
        return;
      }
      case 1: destroy_object(c, id); return;      // destroy (3): its layer surfaces stay
      default: post_error(c, id, kErrorInvalidMethod, "zwlr_layer_shell_v1 has no such request"); return;
    }
  }
  // zwlr_layer_surface_v1
  Surface* s = r.surface;
  LayerConfig* k = s ? &s->layer_pending : nullptr;
  switch (opcode) {
    case 0: {      // set_size(width, height)
      const uint32_t w = in.u(), h = in.u();
      if (!in.ok() || !k) return;
      k->width = w > uint32_t(kMaxSize) ? uint32_t(kMaxSize) : w;
      k->height = h > uint32_t(kMaxSize) ? uint32_t(kMaxSize) : h;
      return;
    }
    case 1: {      // set_anchor(anchor)
      const uint32_t anchor = in.u();
      if (!in.ok() || !k) return;
      if (anchor > 15) {
        post_error(c, id, kLayerInvalidAnchor, "invalid anchor");
        return;
      }
      k->anchor = anchor;
      return;
    }
    case 2: {      // set_exclusive_zone(zone)
      const int32_t zone = in.i();
      if (!in.ok() || !k) return;
      k->zone = zone < -1 ? -1 : zone > kMaxSize ? kMaxSize : zone;
      return;
    }
    case 3: {      // set_margin(top, right, bottom, left)
      const int32_t t = in.i(), rr = in.i(), b = in.i(), l = in.i();
      if (!in.ok() || !k) return;
      k->margin[0] = clamp_margin(t), k->margin[1] = clamp_margin(rr), k->margin[2] = clamp_margin(b), k->margin[3] = clamp_margin(l);
      return;
    }
    case 4: {      // set_keyboard_interactivity(mode)
      const uint32_t mode = in.u();
      if (!in.ok() || !k) return;
      if (mode > 2 || (mode == 2 && r.version < 4)) {
        post_error(c, id, kLayerInvalidKeyboard, "invalid keyboard interactivity");
        return;
      }
      k->keyboard = mode;
      return;
    }
    case 5: {      // get_popup(xdg_popup)
      const uint32_t popup = in.object();
      if (!in.ok()) return;
      Resource* pr = object_of(c, popup, Iface::Popup, id);
      if (!pr || !pr->surface || !s) return;
      Surface* p = pr->surface;
      if (p->popup_parent || p->initial_configure) {
        post_error(c, id, kLayerInvalidSurfaceState, "the popup has a parent already");
        return;
      }
      p->popup_parent = s;
      return;
    }
    case 6: {      // ack_configure(serial)
      const uint32_t serial = in.u();
      if (in.ok() && s) s->acked_serial = serial;
      return;
    }
    case 7:      // destroy: the surface shows nothing from now on (it keeps its role)
      if (s) {
        dismiss_children(*s);
        if (s->mapped) unmap(*s);
        s->role_id = 0;
        s->initial_configure = false;
        arrange_layers();
        if (Change* ch = change_of(s->window)) ch->changed = true;
      }
      destroy_object(c, id);
      return;
    case 8: {      // set_layer(layer) (2)
      const uint32_t layer = in.u();
      if (!in.ok() || !k) return;
      if (r.version < 2) {
        post_error(c, id, kErrorInvalidMethod, "zwlr_layer_surface_v1 has no such request");
        return;
      }
      if (layer > 3) {
        post_error(c, id, kShellInvalidLayer, "no such layer");      // (the shell's code, as wlroots posts it)
        return;
      }
      k->layer = layer;
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "zwlr_layer_surface_v1 has no such request"); return;
  }
}

}  // namespace wayland
