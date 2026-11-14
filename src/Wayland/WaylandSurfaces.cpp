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

// Surfaces, regions, subsurfaces, shared-memory buffers, and the xdg shell (toplevels, popups, positioners) with the desktop as its window manager.
#include "Graphics/DeviceScale.hpp"
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

// A damage or region rectangle from the client, kept within reach of int32 arithmetic (a client may send INT32_MAX for "everything").
Rect clamp_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
  constexpr int32_t kLimit = 1 << 24;
  const int32_t cx = x < -kLimit ? -kLimit : x > kLimit ? kLimit : x;
  const int32_t cy = y < -kLimit ? -kLimit : y > kLimit ? kLimit : y;
  const int32_t cw = w < 0 ? 0 : w > 2 * kLimit ? 2 * kLimit : w;
  const int32_t ch = h < 0 ? 0 : h > 2 * kLimit ? 2 * kLimit : h;
  return {cx, cy, cw, ch};
}

bool valid_format(uint32_t f) { return f == kFormatArgb8888 || f == kFormatXrgb8888 || f == kFormatAbgr8888 || f == kFormatXbgr8888; }
bool has_alpha(uint32_t f) { return f == kFormatArgb8888 || f == kFormatAbgr8888; }

// xdg_positioner anchors and gravities: which side of the rectangle (or of the point) they name.
int32_t horizontal(uint32_t v) { return v == 3 || v == 5 || v == 6 ? -1 : v == 4 || v == 7 || v == 8 ? 1 : 0; }
int32_t vertical(uint32_t v) { return v == 1 || v == 5 || v == 7 ? -1 : v == 2 || v == 6 || v == 8 ? 1 : 0; }
uint32_t flip_x(uint32_t v) {
  switch (v) {
    case 3: return 4;
    case 4: return 3;
    case 5: return 7;
    case 7: return 5;
    case 6: return 8;
    case 8: return 6;
    default: return v;
  }
}
uint32_t flip_y(uint32_t v) {
  switch (v) {
    case 1: return 2;
    case 2: return 1;
    case 5: return 6;
    case 6: return 5;
    case 7: return 8;
    case 8: return 7;
    default: return v;
  }
}

}  // namespace

bool Server::RegionRects::contains(int32_t x, int32_t y) const {
  bool inside = false;
  for (uint64_t i = 0; i < rects.count(); ++i)
    if (wayland::contains(rects[i], x, y)) inside = add[i];
  return inside;
}

// ------------------------------------------------------------------ helpers

Server::Surface* Server::window_surface(uint32_t window) {
  if (window == 0) return nullptr;
  for (Surface& s : surfaces_)
    if (s.window == window && s.role_id != 0 && (s.role == Role::Toplevel || s.role == Role::Popup || s.role == Role::Layer)) return &s;
  return nullptr;
}

// The window a popup chain hangs from: a toplevel or a layer surface.
Server::Surface* Server::root_toplevel(Surface* s) {
  for (uint32_t guard = 0; s && s->role == Role::Popup && guard < 64; ++guard) s = s->popup_parent;
  return s && (s->role == Role::Toplevel || s->role == Role::Layer) ? s : nullptr;
}

bool Server::decorated(const Surface& s) const { return attached_ && s.role == Role::Toplevel && s.server_decorated && !s.fullscreen; }

void Server::frame_margins(const Surface& s, int32_t* left, int32_t* top, int32_t* right, int32_t* bottom) const {
  const bool d = decorated(s);
  *left = *right = *bottom = d ? frame_border_ : 0;
  *top = d ? frame_border_ + frame_title_ : 0;
}

// The size the window shows: what the last configure asked for until the client has answered it, then the client's window geometry (or its buffer).
void Server::shown_size(const Surface& s, int32_t* w, int32_t* h) const {
  if (s.want_w > 0 && s.want_h > 0 && s.want_serial != 0) {
    *w = s.want_w;
    *h = s.want_h;
    return;
  }
  actual_size(s, w, h);
}

void Server::actual_size(const Surface& s, int32_t* w, int32_t* h) const {
  if (s.geometry_set) {
    *w = s.geometry.w;
    *h = s.geometry.h;
  } else {
    surface_size(s.current, s.mapping, w, h);
  }
}

Rect Server::content_rect(const Surface& s) const {
  int32_t w = 0, h = 0;
  if (s.role == Role::Popup) {
    actual_size(s, &w, &h);
    Rect parent;
    if (s.popup_parent) parent = content_rect(*s.popup_parent);
    return {parent.x + s.placement.x, parent.y + s.placement.y, w, h};
  }
  shown_size(s, &w, &h);
  return {s.x, s.y, w, h};
}

void Server::surface_origin(const Surface& s, int32_t* x, int32_t* y) const {
  if (s.role == Role::Subsurface && s.parent) {
    surface_origin(*s.parent, x, y);
    *x += s.sub_x;
    *y += s.sub_y;
    return;
  }
  const Rect r = content_rect(s);
  *x = r.x - (s.geometry_set ? s.geometry.x : 0);
  *y = r.y - (s.geometry_set ? s.geometry.y : 0);
}

Rect Server::monitor_for(const Surface& s) const {
  const Rect r = s.role == Role::Popup && s.popup_parent ? content_rect(*s.popup_parent) : Rect{s.x, s.y, 1, 1};
  const int32_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  for (uint32_t i = 0; i < monitor_count_; ++i) if (contains(monitors_[i].rect, cx, cy)) return monitors_[i].rect;
  return monitors_[0].rect;
}

Rect Server::work_area_for(const Surface& s) const {
  const Rect r = s.role == Role::Popup && s.popup_parent ? content_rect(*s.popup_parent) : Rect{s.x, s.y, 1, 1};
  const int32_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  for (uint32_t i = 0; i < monitor_count_; ++i) if (contains(monitors_[i].rect, cx, cy)) return monitors_[i].work;
  return monitors_[0].work;
}

// The pixels of a buffer, row by row (b.pitch() words apart). The pool's memory starts on a page and create_buffer made sure the offset and the stride are
// multiples of 4, so the words are aligned; the bytes stay mapped as long as `b.memory` (shared with the pool) lives. A YUV buffer's are its converted
// picture (empty until its first commit converted it).
// The view covers whole rows (stride * height) when the pool holds them, which is what a graphics::Surface of that pitch needs to count as valid; the
// last row's padding need not exist in the pool, and then the view ends at the last pixel (see drawable_pixels).
ArrayView<uint32_t> Server::buffer_pixels(const BufferInfo& b) {
  if (!b.valid()) return ArrayView<uint32_t>();
  if (b.yuv) return b.yuv->rgb.count() == uint64_t(b.width) * uint64_t(b.height) ? b.yuv->rgb.view() : ArrayView<uint32_t>();
  if (!b.memory) return ArrayView<uint32_t>();
  const uint64_t needed = uint64_t(b.stride) * uint64_t(b.height - 1) + uint64_t(b.width) * 4, whole = uint64_t(b.stride) * uint64_t(b.height);
  ByteView all = b.memory->bytes;
  if (uint64_t(b.offset) > all.size() || needed > all.size() - uint64_t(b.offset)) return ArrayView<uint32_t>();
  const uint64_t bytes = whole <= all.size() - uint64_t(b.offset) ? whole : needed;
  ByteView mine = all.subview(uint64_t(b.offset), bytes);
  return ArrayView<uint32_t>(reinterpret_cast<uint32_t*>(mine.data()), (bytes + 3) / 4);
}

bool Server::drawable_pixels(const BufferInfo& b, List<uint32_t>* tight, ArrayView<uint32_t>* pixels, uint32_t* pitch) {
  *pixels = buffer_pixels(b);
  *pitch = b.pitch();
  if (pixels->size() == 0) return false;
  if (pixels->size() >= uint64_t(*pitch) * uint64_t(b.height)) return true;
  // The last row's padding lies past the pool's end: a copy of the rows without their padding (rare: clients allocate whole rows).
  const uint64_t width = uint64_t(b.width), height = uint64_t(b.height);
  tight->clear();
  if (!tight->reserve(width * height)) return false;
  for (uint64_t y = 0; y < height; ++y)
    for (uint64_t x = 0; x < width; ++x)
      if (!tight->push_back((*pixels)[y * *pitch + x])) return false;
  *pixels = tight->view();
  *pitch = uint32_t(width);
  return true;
}

void Server::convert_yuv(BufferInfo& b) {
  if (!b.yuv || b.width <= 0 || b.height <= 0) return;
  YuvPicture& y = *b.yuv;
  const uint64_t count = uint64_t(b.width) * uint64_t(b.height);
  if (y.rgb.count() != count) {
    y.rgb.clear();
    if (!y.rgb.reserve(count)) return;      // (nothing drawn: buffer_pixels is empty)
    for (uint64_t i = 0; i < count; ++i) y.rgb.push_back(0xFF000000u);
  }
  const YuvFormat& f = y.format;
  YuvPlane planes[3];
  bool ok = true;
  if (!y.dmabuf) {
    const ByteView all = y.pool ? y.pool->bytes : ByteView();
    for (uint32_t i = 0; i < f.planes && ok; ++i) {
      const uint64_t need = f.plane_bytes(i, uint32_t(b.width), uint32_t(b.height), y.strides[i]);
      ByteView mine;
      ok = all.try_subview(y.offsets[i], need, &mine);
      planes[i].bytes = mine;
      planes[i].stride = y.strides[i];
    }
  } else {
    // one map per descriptor, as big as every plane in it needs (make_yuv_dmabuf checked that it is)
    ByteView views[3];
    for (uint32_t i = 0; i < f.planes && ok; ++i) {
      uint32_t owner = i;
      for (uint32_t j = 0; j < i; ++j) if (y.planes[j].get() == y.planes[i].get()) { owner = j; break; }
      if (owner == i) {
        uint64_t need = 0;
        for (uint32_t j = i; j < f.planes; ++j) {
          if (y.planes[j].get() != y.planes[i].get()) continue;
          const uint64_t end = y.offsets[j] + f.plane_bytes(j, uint32_t(b.width), uint32_t(b.height), y.strides[j]);
          if (end > need) need = end;
        }
        views[i] = y.planes[i]->map(need);
      }
      ByteView mine;
      ok = views[owner].try_subview(y.offsets[i], f.plane_bytes(i, uint32_t(b.width), uint32_t(b.height), y.strides[i]), &mine);
      planes[i].bytes = mine;
      planes[i].stride = y.strides[i];
    }
  }
  if (!ok || !yuv_to_xrgb(f, uint32_t(b.width), uint32_t(b.height), ArrayView<const YuvPlane>(planes, f.planes), y.rgb.view()))
    for (uint32_t& w : y.rgb.view()) w = 0xFF000000u;
}

Server::Change* Server::change_of(uint32_t window) {
  for (Change& c : changes_.view()) if (c.window == window) return &c;
  if (changes_.count() >= 1024) {
    changes_overflow_ = true;
    return nullptr;
  }
  Change c;
  c.window = window;
  if (!changes_.push_back(c)) {
    changes_overflow_ = true;
    return nullptr;
  }
  return &changes_[changes_.count() - 1];
}

void Server::note_changed(Surface& s) {
  if (s.window == 0) return;
  if (Change* c = change_of(s.window)) c->changed = true;
}

void Server::note_repaint(Surface& s, const Rect& dirty) {
  if (s.window == 0 || dirty.empty()) return;
  if (Change* c = change_of(s.window)) {
    c->dirty = c->repaint ? unite(c->dirty, dirty) : dirty;
    c->repaint = true;
  }
}

// ------------------------------------------------------------------ wl_surface

void Server::copy_region(Client& c, uint32_t region, SharedPointer<RegionRects>* out, uint32_t from) {
  *out = SharedPointer<RegionRects>();
  if (region == 0) return;
  Resource* r = object_of(c, region, Iface::Region, from);
  if (!r || !r->region) return;
  SharedPointer<RegionRects> copy = SharedPointer<RegionRects>::make(allocator_, allocator_);
  if (!copy) return;
  for (uint64_t i = 0; i < r->region->rects.count(); ++i) {
    copy->rects.push_back(r->region->rects[i]);
    copy->add.push_back(r->region->add[i]);
  }
  *out = util::move(copy);
}

void Server::surface_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  Surface* s = r.surface;
  if (!s) return;
  switch (opcode) {
    case 0:      // destroy
      destroy_surface(c, *s);
      destroy_object(c, id);
      return;
    case 1: {      // attach(buffer, x, y)
      const uint32_t buffer = in.object();
      in.i();
      in.i();
      if (!in.ok()) return;
      s->pending.attached = true;
      s->pending.buffer = BufferInfo();
      if (buffer != 0) {
        Resource* b = object_of(c, buffer, Iface::Buffer, id);
        if (!b) return;
        s->pending.buffer = b->buffer;
        s->pending.buffer.id = buffer;
      }
      return;
    }
    case 2:        // damage(x, y, w, h)
    case 9: {      // damage_buffer (the scale is 1 and there is no transform: the same)
      const int32_t x = in.i(), y = in.i(), w = in.i(), h = in.i();
      if (!in.ok()) return;
      const Rect d = clamp_rect(x, y, w, h);
      if (d.empty()) return;
      s->pending.damage = s->pending.damaged ? unite(s->pending.damage, d) : d;
      s->pending.damaged = true;
      return;
    }
    case 3: {      // frame(callback)
      const uint32_t callback = in.u();
      if (!in.ok() || !new_object(c, callback, Iface::Callback, 1, id)) return;
      if (s->pending_callbacks.count() >= 64 || !s->pending_callbacks.push_back(callback)) post_error(c, kDisplayId, kErrorNoMemory, "too many frame callbacks");
      return;
    }
    case 4:        // set_opaque_region(region)
    case 5: {      // set_input_region(region)
      const uint32_t region = in.object();
      if (!in.ok()) return;
      if (opcode == 4) {
        s->pending.opaque_set = true;
        copy_region(c, region, &s->pending.opaque, id);
      } else {
        s->pending.input_set = true;
        copy_region(c, region, &s->pending.input, id);
      }
      return;
    }
    case 6: commit(c, *s); return;
    case 7: {      // set_buffer_transform
      const int32_t t = in.i();
      if (!in.ok()) return;
      if (t < 0 || t > 7) post_error(c, id, kSurfaceInvalidTransform, "invalid transform");
      else s->pending.transform = t;
      return;
    }
    case 8: {      // set_buffer_scale
      const int32_t scale = in.i();
      if (!in.ok()) return;
      if (scale < 1 || scale > 16) post_error(c, id, kSurfaceInvalidScale, "invalid scale");
      else s->pending.scale = scale;
      return;
    }
    case 10:       // offset(x, y)
      in.i();
      in.i();
      return;
    default: post_error(c, id, kErrorInvalidMethod, "wl_surface has no such request"); return;
  }
}

bool Server::synchronized(const Surface& s) const {
  const Surface* at = &s;
  for (uint32_t guard = 0; at && at->role == Role::Subsurface && guard < 64; ++guard) {
    if (at->sync) return true;
    at = at->parent;
  }
  return false;
}

void Server::commit(Client& c, Surface& s) {
  if (!check_sync_points(c, s) || !check_mapping(c, s, s.pending)) return;
  if (s.pending.release.set()) {      // (the buffer's use from this commit ends at that point)
    s.pending.buffer.release_timeline = s.pending.release.timeline;
    s.pending.buffer.release_point = s.pending.release.point;
  }
  if (s.role == Role::Subsurface && s.parent && synchronized(s)) {      // kept until the parent's commit (its acquire point is not waited for)
    State& p = s.pending;
    State& k = s.cached;
    if (p.attached) { k.attached = true; k.buffer = p.buffer; }
    if (p.damaged) { k.damage = k.damaged ? unite(k.damage, p.damage) : p.damage; k.damaged = true; }
    if (p.opaque_set) { k.opaque_set = true; k.opaque = p.opaque; }
    if (p.input_set) { k.input_set = true; k.input = p.input; }
    if (p.scale > 0) k.scale = p.scale;
    if (p.transform >= 0) k.transform = p.transform;
    if (p.source_set) { k.source_set = true; for (int i = 0; i < 4; ++i) k.source[i] = p.source[i]; }
    if (p.destination_set) { k.destination_set = true; k.destination[0] = p.destination[0]; k.destination[1] = p.destination[1]; }
    s.has_cached = true;
    s.pending = State();
    for (uint32_t cb : s.pending_callbacks.view()) s.callbacks.push_back(cb);
    s.pending_callbacks.clear();
    return;
  }
  State state = util::move(s.pending);
  s.pending = State();
  if (s.waiting || (state.acquire.set() && !acquired(state))) {
    // explicit sync: the picture may be used only once its acquire point is reached. One commit waits at a time: a newer one takes the place of the one
    // waiting (whose buffer is let go unused), as a mailbox would. The desktop is asked to come back, which looks again.
    if (s.waiting) signal_release(s.waiting_state.buffer), release_buffer(c, s.waiting_state.buffer);
    else ++waiting_count_;
    s.waiting = true;
    s.waiting_state = util::move(state);
    for (uint32_t cb : s.pending_callbacks.view()) s.callbacks.push_back(cb);
    s.pending_callbacks.clear();
    finish_waiting(s, false);      // (reached meanwhile: applied now)
    if (s.waiting) {
      Surface* top = &s;
      while (top->role == Role::Subsurface && top->parent) top = top->parent;
      note_repaint(*top, {0, 0, 1, 1});
    }
    return;
  }
  apply(c, s, state);
}

void Server::apply(Client& c, Surface& s, State& state) {
  int32_t before_w = 0, before_h = 0;
  actual_size(s, &before_w, &before_h);
  const bool had_buffer = s.current.valid();
  bool full = false;
  if (state.attached) {
    if (s.current.valid() && s.current.id != state.buffer.id) release_buffer(c, s.current);
    else signal_release(s.current);      // (the same buffer again: the earlier commit's use of it is over)
    if (!state.buffer.valid() || !s.current.valid() || state.buffer.width != s.current.width || state.buffer.height != s.current.height) full = true;
    s.current = state.buffer;
    if (!state.damaged) full = true;      // (a new picture with no damage said: all of it)
  }
  // how the buffer maps onto the surface (a change of it repaints all of it)
  const BufferMapping before_mapping = s.mapping;
  if (state.scale > 0) s.mapping.scale = state.scale;
  if (state.transform >= 0) s.mapping.transform = state.transform;
  if (state.source_set) for (int i = 0; i < 4; ++i) s.mapping.source[i] = state.source[i];
  if (state.destination_set) s.mapping.destination[0] = state.destination[0], s.mapping.destination[1] = state.destination[1];
  if (!s.mapping.identity() || !before_mapping.identity()) full = true;
  // a GPU picture the client may have written through a CPU mapping: those writes reach the image now (GpuImage::committed)
  if ((state.attached || state.damaged) && s.current.image()) s.current.image()->committed();
  // a YUV picture is converted now, as the client has it at this commit (what is drawn until the next one)
  if ((state.attached || state.damaged) && s.current.yuv) convert_yuv(s.current);
  if (state.input_set) s.input = state.input;
  if (state.geometry_set) {
    if (!s.geometry_set || s.geometry.x != state.geometry.x || s.geometry.y != state.geometry.y) full = true;
    s.geometry = state.geometry;
    s.geometry_set = true;
  }
  for (uint32_t cb : s.pending_callbacks.view()) s.callbacks.push_back(cb);
  s.pending_callbacks.clear();
  // presentation feedback: a new picture discards what the old one's was waiting for
  if (state.attached && s.feedback.count()) presented(s, false);
  for (uint32_t f : s.pending_feedback.view()) s.feedback.push_back(f);
  s.pending_feedback.clear();
  // the children's positions and synchronized commits come with this one
  for (Surface* child : s.stack.view()) {
    if (!child) continue;
    if (child->sub_x != child->pending_sub_x || child->sub_y != child->pending_sub_y) full = true;
    child->sub_x = child->pending_sub_x;
    child->sub_y = child->pending_sub_y;
    if (child->has_cached) {
      State cached = util::move(child->cached);
      child->cached = State();
      child->has_cached = false;
      apply(c, *child, cached);
    }
  }
  // what of the window to draw again
  Surface* top = &s;
  int32_t ox = 0, oy = 0;
  for (uint32_t guard = 0; top->role == Role::Subsurface && top->parent && guard < 64; ++guard) {
    ox += top->sub_x;
    oy += top->sub_y;
    top = top->parent;
  }
  if (top->geometry_set) {
    ox -= top->geometry.x;
    oy -= top->geometry.y;
  }
  if (s.role == Role::Cursor) {      // the desktop fetches the cursor's picture again; its frame callbacks are answered at once
    if (&s == cursor_surface_) ++cursor_handle_;
    for (uint32_t cb : s.callbacks.view()) {
      Event done(cb, 0);
      done.u(now());
      send(c, done);
      c.objects.remove(cb);
      delete_id(c, cb);
    }
    s.callbacks.clear();
  }
  if (full) {
    int32_t w = 0, h = 0;
    shown_size(*top, &w, &h);
    note_repaint(*top, {0, 0, max32(w, 1), max32(h, 1)});
  } else if (state.damaged) {
    note_repaint(*top, {state.damage.x + ox, state.damage.y + oy, state.damage.w, state.damage.h});
  }
  // the roles
  if (s.role == Role::Layer) {
    commit_layer(c, s, had_buffer);      // (WaylandLayerShell.cpp)
    return;
  }
  if (s.role == Role::Toplevel || s.role == Role::Popup) {
    if (s.role_id == 0 || s.xdg_id == 0) return;
    if (!s.initial_configure) {
      if (s.current.valid()) {
        post_error(c, s.xdg_id, 3, "buffer committed before the first configure");      // xdg_surface.unconfigured_buffer
        return;
      }
      if (s.role == Role::Popup && !s.popup_parent) {      // (made without a parent, and none was given since: zwlr_layer_surface_v1.get_popup)
        uint32_t wm = s.xdg_id;
        ObjectCursor cursor;
        for (bool more = c.objects.first(&cursor); more; more = c.objects.next(&cursor))
          if (c.objects.element(cursor)->value.iface == Iface::WmBase) { wm = c.objects.element(cursor)->key; break; }
        post_error(c, wm, kXdgInvalidPopupParent, "a popup without a parent");
        return;
      }
      s.initial_configure = true;
      if (s.role == Role::Toplevel) configure_toplevel(s);
      else configure_popup(s);
      return;
    }
    if (s.want_serial != 0 && s.acked_serial >= s.want_serial) {      // the client answered the size asked for: what it drew is the size now
      s.want_serial = 0;
      s.want_w = s.want_h = 0;
    }
    if (s.current.valid() && !s.mapped) {
      map_window(s);
    } else if (!s.current.valid() && had_buffer && s.mapped) {      // a null buffer unmaps it; it starts over with a new initial commit
      unmap(s);
      s.initial_configure = false;
    }
    int32_t after_w = 0, after_h = 0;
    actual_size(s, &after_w, &after_h);
    if (after_w != before_w || after_h != before_h) note_changed(s);
  }
}

void Server::release_buffer(Client& c, const BufferInfo& b) {
  if (b.release_timeline) b.release_timeline->timeline()->signal(b.release_point);      // (explicit sync: even when the wl_buffer is gone)
  if (b.id == 0) return;
  Resource* r = c.objects.find(b.id);
  if (!r || r->iface != Iface::Buffer) return;
  send(c, Event(b.id, 0));
}

void Server::forget_surface(Surface& s) {
  constraint_gone(s);
  forget_exports(s);
  if (drag_.icon == &s) drag_.icon = nullptr;
  if (drag_.focus == &s) drag_.focus = nullptr, drag_.offer = 0;
  if (drag_.origin == &s) end_drag(false);      // (its origin went: the drag is cancelled)
  if (pointer_focus_ == &s) pointer_focus_ = nullptr;
  if (keyboard_focus_ == &s) keyboard_focus_ = nullptr;
  if (Client* c = client_of(s.client)) {      // (text inputs entered on it are entered on nothing now; no leave names a surface that is gone)
    ObjectCursor cursor;
    for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
      const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
      if (kv->value.iface == Iface::TextInput && kv->value.text_input && kv->value.text_input->entered == s.id) kv->value.text_input->entered = 0;
    }
  }
  if (cursor_surface_ == &s) cursor_surface_ = nullptr;
}

void Server::destroy_surface(Client& c, Surface& s) {
  // its popups go first, then its window
  dismiss_children(s);
  if (s.mapped) unmap(s);
  forget_surface(s);
  if (s.window != 0) {
    if (Change* ch = change_of(s.window)) ch->changed = true;      // (the desktop finds it gone)
  }
  // subsurfaces: it leaves its parent; its children are left without one (unmapped)
  if (s.parent) {
    for (uint64_t i = 0; i < s.parent->stack.count(); ++i)
      if (s.parent->stack[i] == &s) { s.parent->stack.remove_at(i); break; }
    Surface* top = s.parent;
    while (top->role == Role::Subsurface && top->parent) top = top->parent;
    note_repaint(*top, content_rect(*top).empty() ? Rect{0, 0, 1, 1} : Rect{0, 0, content_rect(*top).w, content_rect(*top).h});
  }
  for (Surface* child : s.stack.view()) if (child) child->parent = nullptr;
  for (Surface& other : surfaces_) if (other.popup_parent == &s) other.popup_parent = nullptr;
  // the objects of its roles are left without a surface
  if (s.waiting) {      // (a commit that never came to be: its buffer is let go)
    release_buffer(c, s.waiting_state.buffer);
    s.waiting = false;
    --waiting_count_;
  }
  const uint32_t role_objects[6] = {s.xdg_id, s.role_id, s.decoration_id, s.syncobj_id, s.viewport_id, s.fractional_id};
  for (uint32_t o : role_objects)
    if (o != 0)
      if (Resource* r = c.objects.find(o)) r->surface = nullptr;
  if (s.current.valid()) release_buffer(c, s.current);
  for (uint32_t f : s.pending_feedback.view()) s.feedback.push_back(f);      // (pictures that will never be shown)
  presented(s, false);
  const bool layer = s.role == Role::Layer;
  surfaces_.erase(&s);
  if (layer) arrange_layers();      // (its exclusive zone is free again)
}

// ------------------------------------------------------------------ wl_region

void Server::region_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (opcode == 0) {
    destroy_object(c, id);
    return;
  }
  if (opcode > 2) {
    post_error(c, id, kErrorInvalidMethod, "wl_region has no such request");
    return;
  }
  const int32_t x = in.i(), y = in.i(), w = in.i(), h = in.i();
  if (!in.ok() || !r.region) return;
  if (r.region->rects.count() >= kMaxRegionRects) {
    post_error(c, kDisplayId, kErrorNoMemory, "region too complex");
    return;
  }
  r.region->rects.push_back(clamp_rect(x, y, w, h));
  r.region->add.push_back(opcode == 1);
}

// ------------------------------------------------------------------ wl_subcompositor, wl_subsurface

void Server::subsurface_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (r.iface == Iface::Subcompositor) {
    if (opcode == 0) {
      destroy_object(c, id);
      return;
    }
    if (opcode != 1) {
      post_error(c, id, kErrorInvalidMethod, "wl_subcompositor has no such request");
      return;
    }
    const uint32_t object = in.u(), surface = in.object(), parent = in.object();
    if (!in.ok()) return;
    Resource* sr = object_of(c, surface, Iface::Surface, id);
    Resource* pr = sr ? object_of(c, parent, Iface::Surface, id) : nullptr;
    if (!pr || !sr->surface || !pr->surface) return;
    Surface& s = *sr->surface;
    Surface& p = *pr->surface;
    if ((s.role != Role::None && s.role != Role::Subsurface) || s.role_id != 0) {
      post_error(c, id, kSubcompositorBadSurface, "the surface has another role");
      return;
    }
    for (Surface* at = &p; at; at = at->parent)      // (no loops: the parent is not the surface or below it)
      if (at == &s) {
        post_error(c, id, kSubcompositorBadParent, "bad parent");
        return;
      }
    Resource* nr = new_object(c, object, Iface::Subsurface, 1, id);
    if (!nr) return;
    if (!p.stack.push_back(&s)) {
      post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
      return;
    }
    nr->surface = &s;
    s.role = Role::Subsurface;
    s.role_id = object;
    s.parent = &p;
    s.sync = true;
    s.sub_x = s.sub_y = s.pending_sub_x = s.pending_sub_y = 0;
    return;
  }
  Surface* s = r.surface;
  switch (opcode) {
    case 0:      // destroy: it leaves its parent (and is no longer shown)
      if (s) {
        if (s->parent) {
          for (uint64_t i = 0; i < s->parent->stack.count(); ++i)
            if (s->parent->stack[i] == s) { s->parent->stack.remove_at(i); break; }
          Surface* top = s->parent;
          while (top->role == Role::Subsurface && top->parent) top = top->parent;
          const Rect rc = content_rect(*top);
          note_repaint(*top, {0, 0, max32(rc.w, 1), max32(rc.h, 1)});
        }
        s->parent = nullptr;
        s->role_id = 0;
        if (pointer_focus_ == s) pointer_focus_ = nullptr;
      }
      destroy_object(c, id);
      return;
    case 1: {      // set_position(x, y): with the parent's next commit
      const int32_t x = in.i(), y = in.i();
      if (!in.ok() || !s) return;
      s->pending_sub_x = max32(-kMaxSize, min32(x, kMaxSize));
      s->pending_sub_y = max32(-kMaxSize, min32(y, kMaxSize));
      return;
    }
    case 2:        // place_above(sibling)
    case 3: {      // place_below(sibling)
      const uint32_t sibling = in.object();
      if (!in.ok() || !s || !s->parent) return;
      Resource* sr = object_of(c, sibling, Iface::Surface, id);
      if (!sr || !sr->surface) return;
      Surface* other = sr->surface;
      Surface* parent = s->parent;
      Surface* marker = other == parent ? nullptr : other;
      if (other != parent && other->parent != parent) {
        post_error(c, id, kSubsurfaceBadSurface, "not a sibling");
        return;
      }
      if (other == s) return;
      for (uint64_t i = 0; i < parent->stack.count(); ++i)
        if (parent->stack[i] == s) { parent->stack.remove_at(i); break; }
      for (uint64_t i = 0; i < parent->stack.count(); ++i) {
        if (parent->stack[i] != marker) continue;
        parent->stack.insert(opcode == 2 ? i + 1 : i, s);
        break;
      }
      return;
    }
    case 4:        // set_sync
      if (s) s->sync = true;
      return;
    case 5:        // set_desync: a commit it kept is applied now (unless an ancestor is still synchronized)
      if (!s) return;
      s->sync = false;
      if (s->has_cached && !synchronized(*s)) {
        State cached = util::move(s->cached);
        s->cached = State();
        s->has_cached = false;
        apply(c, *s, cached);
      }
      return;
    default: post_error(c, id, kErrorInvalidMethod, "wl_subsurface has no such request"); return;
  }
}

// ------------------------------------------------------------------ wl_shm, wl_shm_pool, wl_buffer

void Server::shm_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (r.iface == Iface::Shm) {
    if (opcode == 1) {      // release (2)
      destroy_object(c, id);
      return;
    }
    if (opcode != 0) {
      post_error(c, id, kErrorInvalidMethod, "wl_shm has no such request");
      return;
    }
    const uint32_t object = in.u();
    Fd fd = in.fd();
    const int32_t size = in.i();
    if (!in.ok()) return;
    if (size <= 0) {
      post_error(c, id, kShmInvalidStride, "invalid pool size");
      return;
    }
    SharedPointer<PoolMemory> pool = SharedPointer<PoolMemory>::make(allocator_);
    if (!pool) {
      post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
      return;
    }
    pool->bytes = fd->map(uint64_t(size));
    if (pool->bytes.size() < uint64_t(size)) {
      post_error(c, id, kShmInvalidFd, "the descriptor is not memory of that size");
      return;
    }
    pool->fd = util::move(fd);
    pool->size = uint64_t(size);
    Resource* nr = new_object(c, object, Iface::ShmPool, 1, id);
    if (nr) nr->pool = util::move(pool);
    return;
  }
  if (r.iface == Iface::Buffer) {
    if (opcode != 0) {
      post_error(c, id, kErrorInvalidMethod, "wl_buffer has no such request");
      return;
    }
    for (Surface& s : surfaces_) {      // (no release goes to an id that may be reused)
      if (s.client != c.number) continue;
      if (s.current.id == id) s.current.id = 0;
      if (s.pending.buffer.id == id) s.pending.buffer.id = 0;
      if (s.cached.buffer.id == id) s.cached.buffer.id = 0;
    }
    destroy_object(c, id);
    return;
  }
  // wl_shm_pool
  switch (opcode) {
    case 0: {      // create_buffer(id, offset, width, height, stride, format)
      const uint32_t object = in.u();
      const int32_t offset = in.i(), width = in.i(), height = in.i(), stride = in.i();
      const uint32_t format = in.u();
      if (!in.ok() || !r.pool) return;
      YuvFormat yuv;
      if (yuv_format(format, &yuv)) {      // planes one after another from `offset` (yuv_shm_layout); any alignment: they are read byte by byte
        uint64_t offsets[3], bytes = 0;
        uint32_t strides[3];
        if (width <= 0 || height <= 0 || width > kMaxSize || height > kMaxSize || offset < 0 || stride <= 0 ||
            uint64_t(stride) < yuv.row_bytes(0, uint32_t(width)) || !yuv_shm_layout(yuv, uint32_t(width), uint32_t(height), uint32_t(stride), offsets, strides, &bytes)) {
          post_error(c, id, kShmInvalidStride, "invalid buffer layout");
          return;
        }
        for (uint32_t i = 1; i < yuv.planes; ++i) {
          if (strides[i] < yuv.row_bytes(i, uint32_t(width))) {
            post_error(c, id, kShmInvalidStride, "the stride is too short for the chroma rows");
            return;
          }
        }
        if (bytes > r.pool->size || uint64_t(offset) > r.pool->size - bytes) {
          post_error(c, id, kShmInvalidStride, "the buffer does not fit the pool");
          return;
        }
        SharedPointer<YuvPicture> picture = SharedPointer<YuvPicture>::make(allocator_, allocator_);
        if (!picture) {
          post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
          return;
        }
        picture->format = yuv;
        picture->pool = r.pool;
        for (uint32_t i = 0; i < 3; ++i) picture->offsets[i] = uint64_t(offset) + offsets[i], picture->strides[i] = strides[i];
        Resource* nr = new_object(c, object, Iface::Buffer, 1, id);
        if (!nr) return;
        nr->buffer.yuv = util::move(picture);
        nr->buffer.offset = offset;
        nr->buffer.width = width;
        nr->buffer.height = height;
        nr->buffer.stride = stride;
        nr->buffer.format = format;
        return;
      }
      if (!valid_format(format)) {
        post_error(c, id, kShmInvalidFormat, "unsupported format");
        return;
      }
      if (width <= 0 || height <= 0 || width > kMaxSize || height > kMaxSize || offset < 0 || offset % 4 != 0 || stride % 4 != 0 ||
          int64_t(stride) < int64_t(width) * 4) {
        post_error(c, id, kShmInvalidStride, "invalid buffer layout");
        return;
      }
      const uint64_t need = uint64_t(stride) * uint64_t(height - 1) + uint64_t(width) * 4;
      if (need > r.pool->size || uint64_t(offset) > r.pool->size - need) {
        post_error(c, id, kShmInvalidStride, "the buffer does not fit the pool");
        return;
      }
      SharedPointer<PoolMemory> pool = r.pool;
      Resource* nr = new_object(c, object, Iface::Buffer, 1, id);
      if (!nr) return;
      nr->buffer.memory = util::move(pool);
      nr->buffer.offset = offset;
      nr->buffer.width = width;
      nr->buffer.height = height;
      nr->buffer.stride = stride;
      nr->buffer.format = format;
      return;
    }
    case 1: destroy_object(c, id); return;      // destroy (its buffers keep the memory)
    case 2: {      // resize(size): only bigger
      const int32_t size = in.i();
      if (!in.ok() || !r.pool) return;
      if (size <= 0 || uint64_t(size) < r.pool->size) {
        post_error(c, id, kShmInvalidStride, "a pool cannot shrink");
        return;
      }
      ByteView bytes = r.pool->fd->map(uint64_t(size));
      if (bytes.size() < uint64_t(size)) {
        post_error(c, id, kShmInvalidFd, "the descriptor is not memory of that size");
        return;
      }
      r.pool->bytes = bytes;
      r.pool->size = uint64_t(size);
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "wl_shm_pool has no such request"); return;
  }
}

// ------------------------------------------------------------------ xdg_wm_base, xdg_positioner

void Server::wm_base_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  switch (opcode) {
    case 0: destroy_object(c, id); return;
    case 1: {      // create_positioner
      const uint32_t object = in.u();
      if (in.ok()) new_object(c, object, Iface::Positioner, r.version, id);
      return;
    }
    case 2: {      // get_xdg_surface(id, surface)
      const uint32_t object = in.u(), surface = in.object();
      if (!in.ok()) return;
      Resource* sr = object_of(c, surface, Iface::Surface, id);
      if (!sr || !sr->surface) return;
      Surface& s = *sr->surface;
      if (s.xdg_id != 0 || (s.role != Role::None && s.role != Role::Toplevel && s.role != Role::Popup)) {
        post_error(c, id, kXdgRole, "the surface has another role");
        return;
      }
      Resource* nr = new_object(c, object, Iface::XdgSurface, r.version, id);
      if (!nr) return;
      nr->surface = &s;
      s.xdg_id = object;
      return;
    }
    case 3: {      // pong(serial): it answers
      const uint32_t serial = in.u();
      if (!in.ok() || serial != c.ping_serial) return;
      c.ping_serial = 0;
      if (c.not_responding) {
        c.not_responding = false;
        for (Surface& s : surfaces_) if (s.client == c.number && s.role == Role::Toplevel && s.window) note_changed(s);
      }
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "xdg_wm_base has no such request"); return;
  }
}

void Server::positioner_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  Positioner& p = r.positioner;
  switch (opcode) {
    case 0: destroy_object(c, id); return;
    case 1: {
      const int32_t w = in.i(), h = in.i();
      if (!in.ok()) return;
      if (w <= 0 || h <= 0) { post_error(c, id, 0, "invalid size"); return; }
      p.width = min32(w, kMaxSize);
      p.height = min32(h, kMaxSize);
      return;
    }
    case 2: {
      const int32_t x = in.i(), y = in.i(), w = in.i(), h = in.i();
      if (!in.ok()) return;
      if (w < 0 || h < 0) { post_error(c, id, 0, "invalid anchor rectangle"); return; }
      p.anchor_rect = clamp_rect(x, y, w, h);
      return;
    }
    case 3:
    case 4: {
      const uint32_t v = in.u();
      if (!in.ok()) return;
      if (v > 8) { post_error(c, id, 0, "invalid anchor or gravity"); return; }
      if (opcode == 3) p.anchor = v;
      else p.gravity = v;
      return;
    }
    case 5: p.adjustment = in.u(); return;
    case 6: {
      const int32_t x = in.i(), y = in.i();
      p.offset_x = max32(-kMaxSize, min32(x, kMaxSize));
      p.offset_y = max32(-kMaxSize, min32(y, kMaxSize));
      return;
    }
    case 7: return;                       // set_reactive
    case 8: in.i(); in.i(); return;       // set_parent_size
    case 9: in.u(); return;               // set_parent_configure
    default: post_error(c, id, kErrorInvalidMethod, "xdg_positioner has no such request"); return;
  }
}

// ------------------------------------------------------------------ xdg_surface

void Server::xdg_surface_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  Surface* s = r.surface;
  switch (opcode) {
    case 0:
      if (s) s->xdg_id = 0;
      destroy_object(c, id);
      return;
    case 1: {      // get_toplevel
      const uint32_t object = in.u();
      if (!in.ok() || !s) return;
      if (s->role_id != 0 || (s->role != Role::None && s->role != Role::Toplevel)) {
        post_error(c, id, 2, "already constructed");      // xdg_surface.already_constructed
        return;
      }
      Resource* nr = new_object(c, object, Iface::Toplevel, r.version, id);
      if (!nr) return;
      nr->surface = s;
      s->role = Role::Toplevel;
      s->role_id = object;
      s->window = next_window_++;
      s->initial_configure = s->mapped = false;
      s->maximized = s->fullscreen = s->iconic = s->activated = false;
      s->want_w = s->want_h = 0;
      s->want_serial = 0;
      return;
    }
    case 2: {      // get_popup(id, parent, positioner)
      const uint32_t object = in.u(), parent = in.object(), positioner = in.object();
      if (!in.ok() || !s) return;
      if (s->role_id != 0 || (s->role != Role::None && s->role != Role::Popup)) {
        post_error(c, id, 2, "already constructed");
        return;
      }
      Resource* pr = object_of(c, positioner, Iface::Positioner, id);
      if (!pr) return;
      if (pr->positioner.width <= 0 || pr->positioner.height <= 0) {
        post_error(c, id, kXdgInvalidPositioner, "incomplete positioner");
        return;
      }
      Resource* parent_r = parent ? object_of(c, parent, Iface::XdgSurface, id) : nullptr;
      if (parent && !parent_r) return;
      Surface* ps = parent_r ? parent_r->surface : nullptr;
      // (no parent: another protocol gives it one before the first commit - zwlr_layer_surface_v1.get_popup)
      if (parent && (!ps || (ps->role != Role::Toplevel && ps->role != Role::Popup) || ps->role_id == 0)) {
        post_error(c, id, kXdgInvalidPopupParent, "a popup needs a toplevel or popup parent");
        return;
      }
      Resource* nr = new_object(c, object, Iface::Popup, r.version, id);
      if (!nr) return;
      nr->surface = s;
      s->role = Role::Popup;
      s->role_id = object;
      s->window = next_window_++;
      s->popup_parent = ps;
      s->positioner = pr->positioner;
      s->initial_configure = s->mapped = s->grab = false;
      return;
    }
    case 3: {      // set_window_geometry(x, y, w, h)
      const int32_t x = in.i(), y = in.i(), w = in.i(), h = in.i();
      if (!in.ok() || !s) return;
      if (w <= 0 || h <= 0) {
        post_error(c, id, 5, "invalid window geometry");      // xdg_surface.invalid_size
        return;
      }
      s->pending.geometry_set = true;
      s->pending.geometry = {max32(-kMaxSize, min32(x, kMaxSize)), max32(-kMaxSize, min32(y, kMaxSize)), min32(w, kMaxSize), min32(h, kMaxSize)};
      return;
    }
    case 4: {      // ack_configure(serial)
      const uint32_t serial = in.u();
      if (in.ok() && s) s->acked_serial = serial;
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "xdg_surface has no such request"); return;
  }
}

void Server::send_configure_serial(Surface& s) {
  Client* c = client_of(s.client);
  if (!c || s.xdg_id == 0) return;
  s.last_serial = next_serial();
  Event e(s.xdg_id, 0);
  e.u(s.last_serial);
  send(*c, e);
}

void Server::configure_toplevel(Surface& s) {
  Client* c = client_of(s.client);
  if (!c || s.role_id == 0) return;
  Resource* tr = c->objects.find(s.role_id);
  if (!tr) return;
  int32_t l, t, r, b;
  frame_margins(s, &l, &t, &r, &b);
  int32_t w = s.want_w, h = s.want_h;
  if (s.fullscreen) {
    const Rect m = monitor_for(s);
    w = m.w;
    h = m.h;
  } else if (s.maximized) {
    const Rect work = work_area_for(s);
    w = max32(1, work.w - l - r);
    h = max32(1, work.h - t - b);
  }
  if (!s.initial_configure || tr->version >= 4) {
    if (tr->version >= 4) {      // configure_bounds: the work area inside a frame
      const Rect work = work_area_for(s);
      Event bounds(s.role_id, 2);
      bounds.i(max32(0, work.w - l - r)).i(max32(0, work.h - t - b));
      send(*c, bounds);
    }
  }
  if (tr->version >= 5 && !s.mapped) {      // wm_capabilities (before the first configure): maximize, fullscreen, minimize
    uint8_t caps[12] = {};
    const uint32_t list[3] = {2, 3, 4};
    for (uint32_t i = 0; i < 3; ++i) caps[i * 4] = static_cast<uint8_t>(list[i]);
    Event e(s.role_id, 3);
    e.array(ConstByteView(caps, sizeof(caps)));
    send(*c, e);
  }
  uint8_t states[16] = {};
  uint32_t n = 0;
  auto add = [&](uint32_t state) { states[n * 4] = static_cast<uint8_t>(state); ++n; };
  if (s.maximized) add(kStateMaximized);
  if (s.fullscreen) add(kStateFullscreen);
  if (s.activated) add(kStateActivated);
  if (s.iconic && tr->version >= 6) add(9);      // suspended: minimized, nothing of it is seen
  Event e(s.role_id, 0);
  e.i(w).i(h).array(ConstByteView(states, n * 4));
  send(*c, e);
  if (s.decoration_id != 0) {
    Event d(s.decoration_id, 0);
    d.u(2);      // server_side
    send(*c, d);
  }
  send_configure_serial(s);
  if (w > 0 && h > 0) {
    s.want_w = w;
    s.want_h = h;
    s.want_serial = s.last_serial;
  }
}

// Where the popup goes (relative to its parent's window geometry): its positioner's rule, then kept on the parent's monitor as far as its constraint
// adjustments allow (flip, then slide, then resize).
void Server::place_popup(Surface& s) {
  const Positioner& p = s.positioner;
  const Rect parent = s.popup_parent ? content_rect(*s.popup_parent) : Rect();
  auto layout = [&](uint32_t anchor, uint32_t gravity, int32_t dx, int32_t dy) {
    const Rect& a = p.anchor_rect;
    const int32_t ax = a.x + (horizontal(anchor) < 0 ? 0 : horizontal(anchor) > 0 ? a.w : a.w / 2);
    const int32_t ay = a.y + (vertical(anchor) < 0 ? 0 : vertical(anchor) > 0 ? a.h : a.h / 2);
    const int32_t x = horizontal(gravity) < 0 ? ax - p.width : horizontal(gravity) > 0 ? ax : ax - p.width / 2;
    const int32_t y = vertical(gravity) < 0 ? ay - p.height : vertical(gravity) > 0 ? ay : ay - p.height / 2;
    return Rect{x + dx, y + dy, p.width, p.height};
  };
  Rect at = layout(p.anchor, p.gravity, p.offset_x, p.offset_y);
  const Rect work = work_area_for(s);
  // in screen coordinates for the checks
  auto outside_x = [&](const Rect& r) { return parent.x + r.x < work.x || int64_t{parent.x} + r.x + r.w > int64_t{work.x} + work.w; };
  auto outside_y = [&](const Rect& r) { return parent.y + r.y < work.y || int64_t{parent.y} + r.y + r.h > int64_t{work.y} + work.h; };
  if (outside_x(at) && (p.adjustment & 4)) {
    const Rect flipped = layout(flip_x(p.anchor), flip_x(p.gravity), -p.offset_x, p.offset_y);
    if (!outside_x(flipped)) at.x = flipped.x;
  }
  if (outside_y(at) && (p.adjustment & 8)) {
    const Rect flipped = layout(flip_y(p.anchor), flip_y(p.gravity), p.offset_x, -p.offset_y);
    if (!outside_y(flipped)) at.y = flipped.y;
  }
  if (outside_x(at) && (p.adjustment & 1)) {
    if (int64_t{parent.x} + at.x + at.w > int64_t{work.x} + work.w) at.x = work.x + work.w - at.w - parent.x;
    if (parent.x + at.x < work.x) at.x = work.x - parent.x;
  }
  if (outside_y(at) && (p.adjustment & 2)) {
    if (int64_t{parent.y} + at.y + at.h > int64_t{work.y} + work.h) at.y = work.y + work.h - at.h - parent.y;
    if (parent.y + at.y < work.y) at.y = work.y - parent.y;
  }
  if (outside_x(at) && (p.adjustment & 16)) {
    const int32_t x0 = max32(parent.x + at.x, work.x), x1 = min32(parent.x + at.x + at.w, work.x + work.w);
    if (x1 > x0) { at.x = x0 - parent.x; at.w = x1 - x0; }
  }
  if (outside_y(at) && (p.adjustment & 32)) {
    const int32_t y0 = max32(parent.y + at.y, work.y), y1 = min32(parent.y + at.y + at.h, work.y + work.h);
    if (y1 > y0) { at.y = y0 - parent.y; at.h = y1 - y0; }
  }
  s.placement = at;
}

void Server::configure_popup(Surface& s) {
  Client* c = client_of(s.client);
  if (!c || s.role_id == 0) return;
  place_popup(s);
  Event e(s.role_id, 0);
  e.i(s.placement.x).i(s.placement.y).i(s.placement.w).i(s.placement.h);
  send(*c, e);
  send_configure_serial(s);
}

// The first position of a toplevel: a cascade over the primary monitor's work area.
void Server::place(Surface& s) {
  if (s.placed) return;
  s.placed = true;
  int32_t l, t, r, b;
  frame_margins(s, &l, &t, &r, &b);
  int32_t w = 0, h = 0;
  shown_size(s, &w, &h);
  const Rect work = monitors_[0].work;
  const int32_t fw = w + l + r, fh = h + t + b;
  int32_t x = work.x + (work.w - fw) / 2, y = work.y + (work.h - fh) / 3;
  const int32_t step = 24 * static_cast<int32_t>(place_counter_++ % 8);
  x += step - 84;
  y += step - 84;
  if (x + fw > work.x + work.w) x = work.x + work.w - fw;
  if (y + fh > work.y + work.h) y = work.y + work.h - fh;
  if (x < work.x) x = work.x;
  if (y < work.y) y = work.y;
  s.x = x + l;
  s.y = y + t;
}

void Server::map_window(Surface& s) {
  s.mapped = true;
  if (s.role == Role::Toplevel) place(s);
  for (Surface* w : stack_.view()) if (w == &s) return;
  stack_.push_back(&s);
  note_changed(s);
  int32_t w = 0, h = 0;
  shown_size(s, &w, &h);
  note_repaint(s, {0, 0, max32(w, 1), max32(h, 1)});
  if (Client* c = client_of(s.client)) {      // the surface is on the output now
    ObjectCursor cursor;
    for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
      const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
      if (kv->value.iface != Iface::Output) continue;
      Event e(s.id, 0);
      e.u(kv->key);
      send(*c, e);
    }
  }
  announce_scale();      // (its monitor's scale, if it differs from what it was told)
  if (s.role == Role::Popup && s.grab) set_keyboard_focus(&s);
  // A new window takes the activation and the keyboard, as a program's window does when it is first shown on the desktop (which takes the activation
  // from what the session reports, so a window nobody activates would never get keys until it is clicked).
  if (s.role == Role::Toplevel) activate_locked(&s);
  pointer_moved(false);
}

void Server::unmap(Surface& s) {
  if (!s.mapped) return;
  s.mapped = false;
  for (uint64_t i = 0; i < stack_.count(); ++i)
    if (stack_[i] == &s) { stack_.remove_at(i); break; }
  note_changed(s);
  dismiss_children(s);
  // input that was in it
  for (Surface* at = pointer_focus_; at; at = at->parent) {
    if (at != &s) continue;
    set_pointer_focus(nullptr, 0, 0);
    break;
  }
  if (keyboard_focus_ == &s) {
    Surface* back = s.role == Role::Popup ? root_toplevel(s.popup_parent) : nullptr;
    set_keyboard_focus(back && back->mapped && back->window == active_window_ ? back : nullptr);
  }
  if (drag_window_ == s.window) drag_window_ = 0;
  pointer_moved(false);
}

void Server::dismiss_children(Surface& s) {
  for (uint32_t guard = 0; guard < 64; ++guard) {
    Surface* child = nullptr;
    for (Surface& other : surfaces_)
      if (other.role == Role::Popup && other.popup_parent == &s && other.role_id != 0 && other.mapped) { child = &other; break; }
    if (!child) return;
    dismiss_popup(*child);
  }
}

void Server::dismiss_popup(Surface& s) {
  dismiss_children(s);
  if (Client* c = client_of(s.client); c && s.role_id != 0 && s.mapped) send(*c, Event(s.role_id, 1));      // popup_done
  unmap(s);
}

// Every popup that is not of `keep_root`'s window goes (a click or the focus went elsewhere).
void Server::dismiss_popups(Surface* keep_root) {
  for (uint32_t guard = 0; guard < 256; ++guard) {
    Surface* victim = nullptr;
    for (Surface* w : stack_.view())
      if (w->role == Role::Popup && (!keep_root || root_toplevel(w) != keep_root)) { victim = w; break; }
    if (!victim) return;
    // the bottom-most of a chain first takes its children with it
    dismiss_popup(*victim);
  }
}

void Server::raise(Surface& s) {
  for (uint64_t i = 0; i < stack_.count(); ++i)
    if (stack_[i] == &s) { stack_.remove_at(i); break; }
  stack_.push_back(&s);
  // its popups stay above it
  for (uint32_t guard = 0; guard < 64; ++guard) {
    bool moved = false;
    for (uint64_t i = 0; i + 1 < stack_.count(); ++i) {
      Surface* w = stack_[i];
      if (w->role != Role::Popup || root_toplevel(w) != &s) continue;
      stack_.remove_at(i);
      stack_.push_back(w);
      moved = true;
      break;
    }
    if (!moved) break;
  }
}

void Server::set_maximized(Surface& s, bool on) {
  if (s.maximized == on) return;
  if (on) {
    if (!s.fullscreen) s.restore = content_rect(s);
    s.maximized = true;
    const Rect work = work_area_for(s);
    int32_t l, t, r, b;
    frame_margins(s, &l, &t, &r, &b);
    if (!s.fullscreen) {
      s.x = work.x + l;
      s.y = work.y + t;
    }
  } else {
    s.maximized = false;
    if (!s.fullscreen && !s.restore.empty()) {
      s.x = s.restore.x;
      s.y = s.restore.y;
      s.want_w = s.restore.w;
      s.want_h = s.restore.h;
    }
  }
  if (s.initial_configure) configure_toplevel(s);
  note_changed(s);
}

void Server::set_fullscreen(Surface& s, bool on) {
  if (s.fullscreen == on) return;
  if (on) {
    if (!s.maximized) s.restore = content_rect(s);
    const Rect m = monitor_for(s);
    s.fullscreen = true;
    s.x = m.x;
    s.y = m.y;
  } else {
    s.fullscreen = false;
    if (s.maximized) {
      const Rect work = work_area_for(s);
      int32_t l, t, r, b;
      frame_margins(s, &l, &t, &r, &b);
      s.x = work.x + l;
      s.y = work.y + t;
    } else if (!s.restore.empty()) {
      s.x = s.restore.x;
      s.y = s.restore.y;
      s.want_w = s.restore.w;
      s.want_h = s.restore.h;
    }
  }
  if (s.initial_configure) configure_toplevel(s);
  note_changed(s);
}

// ------------------------------------------------------------------ xdg_toplevel, xdg_popup

void Server::toplevel_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  Surface* s = r.surface;
  switch (opcode) {
    case 0:      // destroy: the window goes
      if (s) {
        dismiss_children(*s);
        unmap(*s);
        if (s->window != 0) note_changed(*s);
        if (active_window_ == s->window) active_window_ = 0;
        s->role_id = 0;
        s->initial_configure = false;
        s->placed = false;
      }
      destroy_object(c, id);
      return;
    case 1: {      // set_parent
      const uint32_t parent = in.object();
      if (!in.ok() || !s) return;
      s->parent_window = 0;
      if (parent != 0) {
        Resource* pr = object_of(c, parent, Iface::Toplevel, id);
        if (pr && pr->surface) s->parent_window = pr->surface->window;
      }
      note_changed(*s);
      return;
    }
    case 2: {      // set_title
      const StringView title = in.string();
      if (!in.ok() || !s) return;
      s->title.clear();
      s->title.append(title.size() > 512 ? StringView(title.data(), 512) : title);
      note_changed(*s);
      return;
    }
    case 3: in.string(); return;      // set_app_id
    case 4: {      // show_window_menu(seat, serial, x, y): the desktop opens the frame's window menu there (x, y: of the window geometry, its client area)
      in.object();
      in.u();
      const int32_t x = in.i(), y = in.i();
      if (!in.ok() || !s || !s->mapped) return;
      menu_window_ = s->window;
      menu_x_ = x;
      menu_y_ = y;
      return;
    }
    case 5:        // move(seat, serial)
    case 6: {      // resize(seat, serial, edges)
      in.object();
      in.u();
      const uint32_t edges = opcode == 6 ? in.u() : 0;
      if (!in.ok() || !s || !s->mapped || s->maximized || s->fullscreen) return;
      // the protocol's edges (top 1, bottom 2, left 4, right 8 and their sums) as the desktop's (left, right, top, top left, top right, bottom, bottom
      // left, bottom right: 1 .. 8; 9 a move)
      uint32_t edge = 9;
      if (opcode == 6) {
        switch (edges) {
          case 4: edge = 1; break;
          case 8: edge = 2; break;
          case 1: edge = 3; break;
          case 5: edge = 4; break;
          case 9: edge = 5; break;
          case 2: edge = 6; break;
          case 6: edge = 7; break;
          case 10: edge = 8; break;
          default: return;
        }
      }
      drag_window_ = s->window;
      drag_edge_ = edge;
      return;
    }
    case 7:        // set_max_size
    case 8: {      // set_min_size
      const int32_t w = in.i(), h = in.i();
      if (!in.ok() || !s) return;
      if (w < 0 || h < 0) {
        post_error(c, id, 2, "invalid size");      // xdg_toplevel.invalid_size
        return;
      }
      if (opcode == 7) { s->max_w = min32(w, kMaxSize); s->max_h = min32(h, kMaxSize); }
      else { s->min_w = min32(w, kMaxSize); s->min_h = min32(h, kMaxSize); }
      note_changed(*s);
      return;
    }
    case 9:
    case 10:
      if (s) set_maximized(*s, opcode == 9);
      return;
    case 11:
      in.object();
      if (in.ok() && s) set_fullscreen(*s, true);
      return;
    case 12:
      if (s) set_fullscreen(*s, false);
      return;
    case 13:      // set_minimized
      if (s && s->mapped) {
        s->iconic = true;
        note_changed(*s);
      }
      return;
    default: post_error(c, id, kErrorInvalidMethod, "xdg_toplevel has no such request"); return;
  }
}

void Server::popup_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  Surface* s = r.surface;
  switch (opcode) {
    case 0:
      if (s) {
        dismiss_children(*s);
        unmap(*s);
        if (s->window != 0) note_changed(*s);
        s->role_id = 0;
        s->initial_configure = false;
      }
      destroy_object(c, id);
      return;
    case 1:      // grab(seat, serial): it takes the keyboard while it is up; a click outside its chain closes it
      in.object();
      in.u();
      if (in.ok() && s) s->grab = true;
      return;
    case 2: {      // reposition(positioner, token)
      const uint32_t positioner = in.object(), token = in.u();
      if (!in.ok() || !s) return;
      Resource* pr = object_of(c, positioner, Iface::Positioner, id);
      if (!pr) return;
      if (pr->positioner.width <= 0 || pr->positioner.height <= 0) {
        post_error(c, id, kXdgInvalidPositioner, "incomplete positioner");
        return;
      }
      s->positioner = pr->positioner;
      if (r.version >= 3) {
        Event e(id, 2);
        e.u(token);
        send(c, e);
      }
      if (s->initial_configure) configure_popup(*s);
      note_changed(*s);
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "xdg_popup has no such request"); return;
  }
}

// ------------------------------------------------------------------ xdg-decoration: the desktop always draws the frame

void Server::decoration_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (r.iface == Iface::DecorationManager) {
    if (opcode == 0) {
      destroy_object(c, id);
      return;
    }
    if (opcode != 1) {
      post_error(c, id, kErrorInvalidMethod, "zxdg_decoration_manager_v1 has no such request");
      return;
    }
    const uint32_t object = in.u(), toplevel = in.object();
    if (!in.ok()) return;
    Resource* tr = object_of(c, toplevel, Iface::Toplevel, id);
    if (!tr || !tr->surface) return;
    Surface& s = *tr->surface;
    if (s.decoration_id != 0) {
      post_error(c, id, 1, "the toplevel already has a decoration object");      // already_constructed
      return;
    }
    Resource* nr = new_object(c, object, Iface::Decoration, 1, id);
    if (!nr) return;
    nr->surface = &s;
    s.decoration_id = object;
    s.server_decorated = true;
    if (s.initial_configure) configure_toplevel(s);      // (a window already shown gets its frame with the next configure)
    note_changed(s);
    return;
  }
  Surface* s = r.surface;
  switch (opcode) {
    case 0:      // destroy: the client draws its own from now on
      if (s) {
        s->decoration_id = 0;
        s->server_decorated = false;
        note_changed(*s);
      }
      destroy_object(c, id);
      return;
    case 1:        // set_mode(mode): the answer is server_side whatever was asked
    case 2: {      // unset_mode
      if (opcode == 1) in.u();
      if (!in.ok() || !s) return;
      Event e(id, 0);
      e.u(2);
      send(c, e);
      if (s->initial_configure) send_configure_serial(*s);
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "zxdg_toplevel_decoration_v1 has no such request"); return;
  }
}

// ------------------------------------------------------------------ drawing

// (x, y) are the surface's place in units; `percent` the renderer's scale (HiDPI, draw_window_scaled: a renderer in device pixels; 100: the desktop's own).
// A buffer made at the device's whole scale with no transform or viewport is drawn 1:1 where its units map to (a GPU buffer where it is); any other goes
// through draw_mapped, which maps it onto the device (1:1 too for a fractional client's buffer through its viewport).
void Server::draw_tree(Surface& s, graphics::IRenderer2D& renderer, int32_t ux, int32_t uy, bool top, List<uint32_t>* done, uint32_t percent) {
  const int32_t x = graphics::units_to_device(ux, percent), y = graphics::units_to_device(uy, percent);
  for (Surface* entry : s.stack.view()) {
    if (entry) {
      if (entry->role == Role::Subsurface) draw_tree(*entry, renderer, ux + entry->sub_x, uy + entry->sub_y, false, done, percent);
      continue;
    }
    for (uint32_t cb : s.callbacks.view()) done->push_back(cb);
    s.callbacks.clear();
    if (s.feedback.count() && s.current.valid()) presented(s, true);      // (shown now: wp_presentation_feedback.presented)
    const bool native = percent % 100 == 0 && s.mapping.scale == int32_t(percent / 100) && s.mapping.transform == 0 && s.mapping.source[0] == -1 &&
                        s.mapping.destination[0] == -1;      // (1:1 on the device)
    if (!native) {      // turned, scaled or cropped, or not at the device's scale: one affine map from the buffer to the screen (WaylandScale.cpp)
      renderer.set_blend(top || !has_alpha(s.current.format) ? graphics::BlendMode::Replace : graphics::BlendMode::Alpha);
      draw_mapped(s, renderer, ux, uy, percent);
      continue;
    }
    if (GpuImage* image = s.current.image()) {      // a dmabuf: drawn where it is (see GpuImage::draw)
      renderer.set_blend(top || !has_alpha(s.current.format) ? graphics::BlendMode::Replace : graphics::BlendMode::Alpha);
      if (!image->draw(renderer, {0, 0, static_cast<uint32_t>(s.current.width), static_cast<uint32_t>(s.current.height)}, x, y)) draw_missed_ = true;
      continue;
    }
    List<uint32_t> tight(allocator_);
    ArrayView<uint32_t> pixels;
    uint32_t pitch = 0;
    if (!drawable_pixels(s.current, &tight, &pixels, &pitch)) continue;
    graphics::Surface picture;
    picture.pixels = pixels;
    picture.width = static_cast<uint32_t>(s.current.width);
    picture.height = static_cast<uint32_t>(s.current.height);
    picture.pitch = pitch;
    picture.format = s.current.format == kFormatAbgr8888 || s.current.format == kFormatXbgr8888 ? graphics::Format::R8G8B8A8Unorm : graphics::Format::B8G8R8A8Unorm;
    // (the window's own picture covers its frame's inside; what is drawn over it blends unless it has no alpha)
    renderer.set_blend(top || !has_alpha(s.current.format) ? graphics::BlendMode::Replace : graphics::BlendMode::Alpha);
    renderer.draw_image(picture, {0, 0, picture.width, picture.height}, x, y);
  }
}

}  // namespace wayland
