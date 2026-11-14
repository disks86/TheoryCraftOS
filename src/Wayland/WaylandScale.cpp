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

// How a buffer maps onto its surface: wl_surface.set_buffer_transform (the eight rotations and flips), set_buffer_scale, and wp_viewporter's crop
// (set_source, in the transformed and scaled buffer's coordinates, 24.8) and size (set_destination). The surface's size follows from them (surface_size),
// and the desktop draws the buffer through one affine map from the buffer to the screen (draw_mapped: IRenderer2D::draw_image_transformed, which a GPU
// renderer does with its texture units). Also wp_fractional_scale_v1: the preferred scale is the output's (the desktop's device scale, output_scale_ in percent, in 120ths).
//
// The transforms follow wl_output.transform as Weston maps surface points to buffer points (weston_transformed_coord): the client rotated its content by
// the transform, the compositor undoes it. A buffer with a mapping that is not the identity is drawn from the CPU's view when it is a GPU buffer (read out
// once a draw; rare: video players and cameras), and its damage is the whole surface.
#include "Graphics/DeviceScale.hpp"
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

constexpr uint32_t kViewportBadValue = 0, kViewportBadSize = 1, kViewportOutOfBuffer = 2, kViewportNoSurface = 3;

}  // namespace

// The buffer's size once transformed (a quarter turn swaps the sides) and scaled down: the surface's size without a viewport.
void Server::unviewported(const BufferInfo& b, const BufferMapping& m, float* w, float* h) {
  const bool swap = (m.transform & 1) != 0;
  const float scale = m.scale > 0 ? float(m.scale) : 1.0f;
  *w = float(swap ? b.height : b.width) / scale;
  *h = float(swap ? b.width : b.height) / scale;
}

void Server::surface_size(const BufferInfo& b, const BufferMapping& m, int32_t* w, int32_t* h) {
  *w = *h = 0;
  if (!b.valid()) return;
  if (m.destination[0] > 0) {
    *w = m.destination[0];
    *h = m.destination[1];
    return;
  }
  if (m.source[0] >= 0) {
    *w = m.source[2] / 256;
    *h = m.source[3] / 256;
    return;
  }
  float uw = 0, uh = 0;
  unviewported(b, m, &uw, &uh);
  *w = int32_t(uw);
  *h = int32_t(uh);
}

bool Server::surface_to_buffer(const BufferInfo& b, const BufferMapping& m, float sx, float sy, float* bx, float* by) {
  if (!b.valid()) return false;
  float uw = 0, uh = 0;
  unviewported(b, m, &uw, &uh);
  int32_t sw = 0, sh = 0;
  surface_size(b, m, &sw, &sh);
  // the viewport: the surface is the crop stretched over the destination
  float ux = sx, uy = sy;
  if (m.source[0] >= 0 || m.destination[0] > 0) {
    const float cx = m.source[0] >= 0 ? float(m.source[0]) / 256 : 0, cy = m.source[0] >= 0 ? float(m.source[1]) / 256 : 0;
    const float cw = m.source[0] >= 0 ? float(m.source[2]) / 256 : uw, ch = m.source[0] >= 0 ? float(m.source[3]) / 256 : uh;
    ux = cx + (sw > 0 ? sx * cw / float(sw) : 0);
    uy = cy + (sh > 0 ? sy * ch / float(sh) : 0);
  }
  // the transform undone (as Weston's weston_transformed_coord), then the scale
  float x = ux, y = uy;
  switch (m.transform) {
    case 1: x = uy, y = uw - ux; break;           // 90
    case 2: x = uw - ux, y = uh - uy; break;      // 180
    case 3: x = uh - uy, y = ux; break;           // 270
    case 4: x = uw - ux, y = uy; break;           // flipped
    case 5: x = uy, y = ux; break;                // flipped 90
    case 6: x = ux, y = uh - uy; break;           // flipped 180
    case 7: x = uh - uy, y = uw - ux; break;      // flipped 270
    default: break;
  }
  const float scale = m.scale > 0 ? float(m.scale) : 1.0f;
  *bx = x * scale;
  *by = y * scale;
  return *bx >= 0 && *by >= 0 && *bx < float(b.width) && *by < float(b.height);
}

bool Server::check_mapping(Client& c, Surface& s, const State& state) {
  BufferMapping m = s.mapping;
  if (state.scale > 0) m.scale = state.scale;
  if (state.transform >= 0) m.transform = state.transform;
  if (state.source_set) for (int i = 0; i < 4; ++i) m.source[i] = state.source[i];
  if (state.destination_set) m.destination[0] = state.destination[0], m.destination[1] = state.destination[1];
  const BufferInfo& b = state.attached ? state.buffer : s.current;
  if (!b.valid()) return true;
  Resource* surface = c.objects.find(s.id);
  const bool swap = (m.transform & 1) != 0;
  if (surface && surface->version >= 6 && ((swap ? b.height : b.width) % m.scale != 0 || (swap ? b.width : b.height) % m.scale != 0)) {
    post_error(c, s.id, kSurfaceInvalidSize, "the buffer's size is not a multiple of its scale");
    return false;
  }
  if (m.source[0] >= 0 && s.viewport_id != 0) {
    if (m.destination[0] <= 0 && (m.source[2] % 256 != 0 || m.source[3] % 256 != 0)) {
      post_error(c, s.viewport_id, kViewportBadSize, "a source of a fractional size without a destination");
      return false;
    }
    float uw = 0, uh = 0;
    unviewported(b, m, &uw, &uh);
    if (int64_t{m.source[0]} + m.source[2] > int64_t(uw * 256) || int64_t{m.source[1]} + m.source[3] > int64_t(uh * 256)) {
      post_error(c, s.viewport_id, kViewportOutOfBuffer, "the source is outside the buffer");
      return false;
    }
  }
  return true;
}

// The output scale changed (Server::set_display): each client's outputs were announced again; its fractional scale objects get the preferred scale and
// its surfaces of version 6 the preferred buffer scale, so a client draws at the desktop's density.
void Server::announce_scale() {
  for (Surface& s : surfaces_) tell_scale(s);
}

// Monitors at different scales: a surface draws at the scale of the monitor its window is on (the centre of its root's window geometry: the toplevel,
// layer surface or whatever it hangs from; a subsurface is its parent's, a popup its parent's window's). One scale for all monitors: that one.
uint32_t Server::scale_for(const Surface& s) const {
  const uint32_t i = monitor_index_for(s);
  return i < monitor_count_ ? monitors_[i].scale : output_scale_;
}

uint32_t Server::monitor_index_for(const Surface& s) const {
  const Surface* root = &s;
  for (uint32_t depth = 0; depth < 64; ++depth) {
    if (root->parent) root = root->parent;
    else if (root->role == Role::Popup && root->popup_parent) root = root->popup_parent;
    else break;
  }
  const Rect r = root->role == Role::Toplevel || root->role == Role::Popup ? content_rect(*root) : Rect{root->x, root->y, 1, 1};
  const int32_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  for (uint32_t i = 0; i < monitor_count_; ++i) if (contains(monitors_[i].rect, cx, cy)) return i;
  return 0;
}

void Server::tell_scale(Surface& s) {
  Client* c = client_of(s.client);
  if (!c) return;
  // a shown window is on its monitor's outputs: wl_surface.enter, and leave for the monitor it was on
  const int32_t index = s.mapped ? static_cast<int32_t>(monitor_index_for(s)) : -1;
  if (index != s.entered) {
    ObjectCursor cursor;
    List<uint32_t> left(allocator_), entered(allocator_);
    for (bool more = c->objects.first(&cursor); more; more = c->objects.next(&cursor)) {
      const KeyValue<uint32_t, Resource>* kv = c->objects.element(cursor);
      if (kv->value.iface != Iface::Output) continue;
      if (s.entered >= 0 && kv->value.output == s.entered) left.push_back(kv->key);
      if (index >= 0 && kv->value.output == index) entered.push_back(kv->key);
    }
    for (uint32_t o : left.view()) {
      Event e(s.id, 1);      // leave(output)
      e.u(o);
      send(*c, e);
    }
    for (uint32_t o : entered.view()) {
      Event e(s.id, 0);      // enter(output)
      e.u(o);
      send(*c, e);
    }
    s.entered = index;
  }
  const uint32_t k = scale_for(s);
  if (k == s.told_scale) return;
  s.told_scale = k;
  if (s.fractional_id != 0) {
    Event e(s.fractional_id, 0);      // preferred_scale (120ths)
    e.u(k * 120 / 100);
    send(*c, e);
  }
  Resource* r = c->objects.find(s.id);
  if (r && r->version >= 6) {
    Event e(s.id, 2);      // preferred_buffer_scale: the whole scale at or above it
    e.i(static_cast<int32_t>((k + 99) / 100));
    send(*c, e);
  }
}

void Server::draw_mapped(Surface& s, graphics::IRenderer2D& renderer, int32_t ux, int32_t uy, uint32_t percent) {
  const int32_t x = graphics::units_to_device(ux, percent), y = graphics::units_to_device(uy, percent);      // (the device place of the surface)
  const BufferInfo& b = s.current;
  int32_t sw = 0, sh = 0;
  surface_size(b, s.mapping, &sw, &sh);
  if (sw <= 0 || sh <= 0) return;
  // the affine map from the surface to the buffer, from three points; its inverse takes the buffer to the screen
  float p0x, p0y, p1x, p1y, p2x, p2y;
  surface_to_buffer(b, s.mapping, 0, 0, &p0x, &p0y);
  surface_to_buffer(b, s.mapping, 1, 0, &p1x, &p1y);
  surface_to_buffer(b, s.mapping, 0, 1, &p2x, &p2y);
  const float ax = p1x - p0x, ay = p1y - p0y, cx = p2x - p0x, cy = p2y - p0y;      // buffer = p0 + sx * (ax, ay) + sy * (cx, cy)
  const float det = ax * cy - cx * ay;
  if (det == 0) return;
  // the buffer's rectangle the surface shows (the crop): the corners' bounding box
  float minx = p0x, maxx = p0x, miny = p0y, maxy = p0y;
  const float corners[3][2] = {{float(sw), 0}, {0, float(sh)}, {float(sw), float(sh)}};
  for (const auto& k : corners) {
    float bx, by;
    surface_to_buffer(b, s.mapping, k[0], k[1], &bx, &by);
    minx = bx < minx ? bx : minx, maxx = bx > maxx ? bx : maxx;
    miny = by < miny ? by : miny, maxy = by > maxy ? by : maxy;
  }
  const int32_t rx = max32(0, int32_t(minx)), ry = max32(0, int32_t(miny));
  const int32_t rr = min32(b.width, int32_t(maxx + 0.999f)), rb = min32(b.height, int32_t(maxy + 0.999f));
  if (rr <= rx || rb <= ry) return;
  // m: a point of the rectangle (from its corner) to the screen: surface = inverse(buffer - p0), screen = surface + (x, y)
  const float ia = cy / det, ib = -ay / det, ic = -cx / det, id = ax / det;      // the inverse of [ax cx; ay cy]
  const float k = float(percent) / 100.0f;      // (device pixels per surface unit)
  graphics::Matrix2D m;
  m.a = ia * k, m.b = ib * k, m.c = ic * k, m.d = id * k;
  const float ox = float(rx) - p0x, oy = float(ry) - p0y;
  m.e = (ia * ox + ic * oy) * k + float(x);
  m.f = (ib * ox + id * oy) * k + float(y);
  // the picture: the client's memory, or the CPU's view of a GPU buffer
  graphics::Surface picture;
  List<uint32_t> gpu_pixels(allocator_);
  if (GpuImage* image = b.image()) {
    const uint64_t count = uint64_t(b.width) * uint64_t(b.height);
    if (!gpu_pixels.reserve(count)) return;
    for (uint64_t i = 0; i < count; ++i) gpu_pixels.push_back(0);
    if (!image->read({0, 0, uint32_t(b.width), uint32_t(b.height)}, gpu_pixels.view())) {
      draw_missed_ = true;
      return;
    }
    picture.pixels = gpu_pixels.view();
    picture.pitch = uint32_t(b.width);
    picture.format = graphics::Format::B8G8R8A8Unorm;      // (read gives 0xAARRGGBB)
  } else {
    if (!drawable_pixels(b, &gpu_pixels, &picture.pixels, &picture.pitch)) return;      // (gpu_pixels holds a tight copy when one is needed)
    picture.format = b.format == kFormatAbgr8888 || b.format == kFormatXbgr8888 ? graphics::Format::R8G8B8A8Unorm : graphics::Format::B8G8R8A8Unorm;
  }
  picture.width = uint32_t(b.width);
  picture.height = uint32_t(b.height);
  // (1:1 on the target - a buffer made at the device's scale, fractional ones through their viewport - or an exact whole enlargement of an upright buffer:
  // no filtering; the factors are snapped when they are that within rounding)
  auto snap = [](float& v) {
    const float r = float(int32_t(v + (v < 0 ? -0.5f : 0.5f)));
    if (r != 0 && (v - r) < 1e-4f && (r - v) < 1e-4f) v = r;
  };
  snap(m.a), snap(m.b), snap(m.c), snap(m.d);
  const float sx2 = m.a * m.a + m.b * m.b, sy2 = m.c * m.c + m.d * m.d;
  const bool whole = m.b == 0 && m.c == 0 && m.a >= 1 && m.d >= 1 && m.a == float(int32_t(m.a)) && m.d == float(int32_t(m.d));
  const bool stretched = !(sx2 == 1.0f && sy2 == 1.0f) && !whole;
  const graphics::ImageFilter filter = stretched ? graphics::ImageFilter::Bilinear : graphics::ImageFilter::Nearest;
  const uint32_t cw = graphics::units_span_to_device(ux, uint32_t(sw), percent), ch = graphics::units_span_to_device(uy, uint32_t(sh), percent);
  const bool clipped = renderer.push_clip_rect({x, y, cw, ch});
  if (!renderer.draw_image_transformed(picture, {rx, ry, uint32_t(rr - rx), uint32_t(rb - ry)}, m, filter)) {
    // a renderer that cannot: an upright crop is stretched; a turned or flipped picture is not drawn
    if (ay == 0 && cx == 0 && ax > 0 && cy > 0)
      renderer.draw_image_scaled(picture, {minx, miny, maxx - minx, maxy - miny}, {x, y, cw, ch}, filter);
  }
  if (clipped) renderer.pop_clip();
}

void Server::viewport_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  switch (r.iface) {
    case Iface::Viewporter:
    case Iface::FractionalScaleManager: {
      const bool viewporter = r.iface == Iface::Viewporter;
      if (opcode == 0) {      // destroy
        destroy_object(c, id);
        return;
      }
      if (opcode != 1) {
        post_error(c, id, kErrorInvalidMethod, viewporter ? "wp_viewporter has no such request" : "wp_fractional_scale_manager_v1 has no such request");
        return;
      }
      const uint32_t object = in.u(), surface = in.object();      // get_viewport / get_fractional_scale(new id, surface)
      if (!in.ok()) return;
      Resource* sr = object_of(c, surface, Iface::Surface, id);
      if (!sr || !sr->surface) return;
      Surface* s = sr->surface;
      if ((viewporter ? s->viewport_id : s->fractional_id) != 0) {
        post_error(c, id, 0, viewporter ? "the surface has a viewport" : "the surface has a fractional scale object");
        return;
      }
      Resource* nr = new_object(c, object, viewporter ? Iface::Viewport : Iface::FractionalScale, 1, id);
      if (!nr) return;
      nr->surface = s;
      if (viewporter) {
        s->viewport_id = object;
      } else {
        s->fractional_id = object;
        Event e(object, 0);      // preferred_scale: the output's (in 120ths)
        e.u(scale_for(*s) * 120 / 100);      // (its window's monitor's)
        send(c, e);
      }
      return;
    }
    case Iface::FractionalScale:
      if (opcode == 0) {
        if (r.surface) r.surface->fractional_id = 0;
        destroy_object(c, id);
      } else {
        post_error(c, id, kErrorInvalidMethod, "wp_fractional_scale_v1 has no such request");
      }
      return;
    case Iface::Viewport: {
      Surface* s = r.surface;
      switch (opcode) {
        case 0:      // destroy: the crop and the size go with the next commit
          if (s) {
            s->viewport_id = 0;
            s->pending.source_set = s->pending.destination_set = true;
            for (int32_t& v : s->pending.source) v = -1;
            s->pending.destination[0] = s->pending.destination[1] = -1;
          }
          destroy_object(c, id);
          return;
        case 1: {      // set_source(x, y, w, h: fixed)
          const int32_t x = in.i(), y = in.i(), w = in.i(), h = in.i();
          if (!in.ok()) return;
          if (!s) {
            post_error(c, id, kViewportNoSurface, "the surface is gone");
            return;
          }
          const int32_t minus_one = -256;
          if (x == minus_one && y == minus_one && w == minus_one && h == minus_one) {
            s->pending.source_set = true;
            for (int32_t& v : s->pending.source) v = -1;
            return;
          }
          if (x < 0 || y < 0 || w <= 0 || h <= 0) {
            post_error(c, id, kViewportBadValue, "invalid source");
            return;
          }
          s->pending.source_set = true;
          s->pending.source[0] = x, s->pending.source[1] = y, s->pending.source[2] = w, s->pending.source[3] = h;
          return;
        }
        case 2: {      // set_destination(w, h)
          const int32_t w = in.i(), h = in.i();
          if (!in.ok()) return;
          if (!s) {
            post_error(c, id, kViewportNoSurface, "the surface is gone");
            return;
          }
          if (w == -1 && h == -1) {
            s->pending.destination_set = true;
            s->pending.destination[0] = s->pending.destination[1] = -1;
            return;
          }
          if (w <= 0 || h <= 0 || w > kMaxSize || h > kMaxSize) {
            post_error(c, id, kViewportBadValue, "invalid destination");
            return;
          }
          s->pending.destination_set = true;
          s->pending.destination[0] = w, s->pending.destination[1] = h;
          return;
        }
        default: post_error(c, id, kErrorInvalidMethod, "wp_viewport has no such request"); return;
      }
    }
    default: return;
  }
}

}  // namespace wayland
