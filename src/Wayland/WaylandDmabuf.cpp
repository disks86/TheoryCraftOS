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

// zwp_linux_dmabuf_v1 (version 4): wl_buffers whose picture is a GPU image a client passed as a descriptor (on this system an export of the kernel's Direct3D
// layer made with /dev/dxg, Process/DxgBufferFile.hpp). The desktop draws one in place (draw_tree), so a client that renders on the GPU shows its frames with
// no copy through the CPU.
//
// What is taken: one plane (every format offered has one), offset 0 and the image's own row pitch, the linear layout (DRM_FORMAT_MOD_LINEAR, or
// DRM_FORMAT_MOD_INVALID: "the driver's choice", which here is linear), no flags (y-inverted and interlaced pictures are refused), and a size no larger than the
// image. The four formats are ARGB8888, XRGB8888, ABGR8888 and XBGR8888: the format says whether the alpha counts; the order of the channels is the image's own
// (the Direct3D layer knows it), so a client's swizzle never shows.
//
// Feedback (version 4): one tranche, the four formats, linear, on the main device: /dev/dxg's number (Process/DxgDevice.h), the one device a client makes its
// buffers on here. The format table is a sealed file in memory of 16-byte entries (format, padding, modifier), the same for every client. Surface feedback is
// the default feedback (one GPU: nothing differs per surface), and it never changes, so each feedback object gets it once.
//
// Explicit sync (wp_linux_drm_syncobj_manager_v1, version 1). A timeline is a fence a client passed (exported with /dev/dxg: TCOS_DXG_IOCTL_EXPORT_SYNC). A
// commit with a buffer on a surface that has a wp_linux_drm_syncobj_surface_v1 names an acquire point (the picture is ready when the timeline reaches it) and a
// release point (the compositor signals it when it is done with that use of the buffer). A commit whose acquire point is not reached yet waits (the surface
// keeps showing what it showed); it is applied as soon as the point is found reached - at the next request of any client or the next time the desktop draws
// (it is asked to come back) - so a client is never shown a picture the GPU is still drawing. One commit waits at a time per surface: a newer one replaces it
// and the older buffer's release point is signalled unused. The release point of a buffer is signalled when another buffer replaces it, the same buffer is
// committed again, or the surface goes. A synchronized subsurface's commit is applied with its parent's without waiting for its acquire point.
#include "Process/DxgDevice.h"
#include "Wayland/WaylandInternal.hpp"

namespace wayland {

namespace {

// The wl_shm code of a DRM format offered (false: not one).
bool shm_format_of(uint32_t drm, uint32_t* shm) {
  switch (drm) {
    case kDrmArgb8888: *shm = kFormatArgb8888; return true;
    case kDrmXrgb8888: *shm = kFormatXrgb8888; return true;
    case kDrmAbgr8888: *shm = kFormatAbgr8888; return true;
    case kDrmXbgr8888: *shm = kFormatXbgr8888; return true;
    default: return false;
  }
}

constexpr uint32_t kMaxPlanes = 4;      // (what the protocol allows)
constexpr uint32_t kTableFormats[4] = {kDrmArgb8888, kDrmXrgb8888, kDrmAbgr8888, kDrmXbgr8888};

}  // namespace

Fd Server::format_table_fd() {
  if (format_table_ || !make_fd_) return format_table_;
  uint8_t table[sizeof(kTableFormats) / sizeof(kTableFormats[0]) * 16] = {};
  ByteWriter out{ByteView(table, sizeof(table))};
  for (uint32_t f : kTableFormats) out.le32(f), out.le32(0), out.le64(kDrmModLinear);
  format_table_ = make_fd_(make_fd_context_, ConstByteView(table, sizeof(table)));
  return format_table_;
}

void Server::send_dmabuf_feedback(Client& c, uint32_t feedback) {
  const Fd table = format_table_fd();
  if (!table) {
    post_error(c, kDisplayId, kErrorNoMemory, "no format table");
    return;
  }
  uint8_t device[8];
  ByteWriter dev{ByteView(device, sizeof(device))};
  dev.le64(TCOS_DXG_DEVICE_NUMBER);      // (dev_t in the host's byte order: little-endian on every CPU here)
  uint8_t indices[sizeof(kTableFormats) / sizeof(kTableFormats[0]) * 2];
  ByteWriter idx{ByteView(indices, sizeof(indices))};
  for (uint16_t i = 0; i < sizeof(kTableFormats) / sizeof(kTableFormats[0]); ++i) idx.le16(i);
  Event t(feedback, 1);      // format_table(fd, size)
  t.fd(table).u(uint32_t(sizeof(kTableFormats) / sizeof(kTableFormats[0]) * 16));
  send(c, t);
  Event main(feedback, 2);      // main_device
  main.array(ConstByteView(device, sizeof(device)));
  send(c, main);
  Event target(feedback, 4);      // tranche_target_device
  target.array(ConstByteView(device, sizeof(device)));
  send(c, target);
  Event formats(feedback, 5);      // tranche_formats
  formats.array(ConstByteView(indices, sizeof(indices)));
  send(c, formats);
  Event flags(feedback, 6);      // tranche_flags: none (not scanout)
  flags.u(0);
  send(c, flags);
  send(c, Event(feedback, 3));      // tranche_done
  send(c, Event(feedback, 0));      // done
}

// The checks of create and create_immed. A wrong request is a protocol error (false, the client is gone); a buffer that cannot be made from what was given is
// `failed` for create, and invalid_wl_buffer for create_immed (false too), as the protocol has it. True with *out filled: the buffer is made.
bool Server::make_dmabuf_buffer(Client& c, uint32_t params_id, DmabufParams& p, int32_t width, int32_t height, uint32_t format, uint32_t flags, bool immediate,
                                BufferInfo* out) {
  if (p.used) {
    post_error(c, params_id, kParamsAlreadyUsed, "the params were used already");
    return false;
  }
  p.used = true;
  if (!p.plane) {
    post_error(c, params_id, kParamsIncomplete, "plane 0 is missing");
    return false;
  }
  uint32_t shm_format = 0;
  if (!shm_format_of(format, &shm_format)) {
    post_error(c, params_id, kParamsInvalidFormat, "unsupported format");
    return false;
  }
  if (width <= 0 || height <= 0 || width > kMaxSize || height > kMaxSize) {
    post_error(c, params_id, kParamsInvalidDimensions, "invalid size");
    return false;
  }
  // what is not a protocol error but cannot be shown: a descriptor that is no GPU image, another layout, flags
  GpuImage* image = p.plane->gpu();
  const bool layout_ok = p.modifier == kDrmModLinear || p.modifier == kDrmModInvalid;
  bool fits = false;
  if (image) {
    if (uint64_t{p.offset} + uint64_t{p.stride} * uint64_t(height) > uint64_t{image->pitch()} * image->height()) {      // (32-bit values: no overflow)
      post_error(c, params_id, kParamsOutOfBounds, "the plane is out of the buffer's bounds");
      return false;
    }
    fits = uint32_t(width) <= image->width() && uint32_t(height) <= image->height() && p.offset == 0 && p.stride == image->pitch();
  }
  if (!image || !layout_ok || !fits || flags != 0) {
    if (immediate) {
      post_error(c, params_id, kParamsInvalidWlBuffer, "the buffer cannot be imported");
    } else {
      send(c, Event(params_id, 1));      // failed
    }
    return false;
  }
  out->gpu = p.plane;
  out->width = width;
  out->height = height;
  out->stride = int32_t(p.stride);
  out->format = shm_format;
  return true;
}

void Server::dmabuf_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (r.iface == Iface::LinuxDmabuf) {
    switch (opcode) {
      case 0: destroy_object(c, id); return;      // destroy
      case 1: {                                   // create_params(new id)
        const uint32_t object = in.u();
        if (!in.ok()) return;
        SharedPointer<DmabufParams> params = SharedPointer<DmabufParams>::make(allocator_);
        if (!params) {
          post_error(c, kDisplayId, kErrorNoMemory, "out of memory");
          return;
        }
        Resource* nr = new_object(c, object, Iface::BufferParams, r.version, id);
        if (nr) nr->params = util::move(params);
        return;
      }
      case 2:        // get_default_feedback(new id) (4)
      case 3: {      // get_surface_feedback(new id, surface) (4)
        const uint32_t object = in.u();
        const uint32_t surface = opcode == 3 ? in.object() : 0;
        if (!in.ok()) return;
        if (r.version < 4) {
          post_error(c, id, kErrorInvalidMethod, "zwp_linux_dmabuf_v1 has no such request");
          return;
        }
        if (opcode == 3 && !object_of(c, surface, Iface::Surface, id)) return;
        if (!new_object(c, object, Iface::DmabufFeedback, r.version, id)) return;
        send_dmabuf_feedback(c, object);
        return;
      }
      default: post_error(c, id, kErrorInvalidMethod, "zwp_linux_dmabuf_v1 has no such request"); return;
    }
  }
  if (r.iface == Iface::DmabufFeedback) {
    if (opcode == 0) destroy_object(c, id);      // destroy
    else post_error(c, id, kErrorInvalidMethod, "zwp_linux_dmabuf_feedback_v1 has no such request");
    return;
  }
  // zwp_linux_buffer_params_v1
  if (!r.params) return;
  DmabufParams& p = *r.params;
  switch (opcode) {
    case 0: destroy_object(c, id); return;      // destroy (a buffer made from it stays)
    case 1: {                                   // add(fd, plane_idx, offset, stride, modifier_hi, modifier_lo)
      Fd fd = in.fd();
      const uint32_t plane = in.u(), offset = in.u(), stride = in.u(), hi = in.u(), lo = in.u();
      if (!in.ok()) return;
      if (p.used) {
        post_error(c, id, kParamsAlreadyUsed, "the params were used already");
        return;
      }
      if (plane >= kMaxPlanes || plane != 0) {      // (every format offered has one plane)
        post_error(c, id, kParamsPlaneIdx, "plane index out of bounds");
        return;
      }
      if (p.plane) {
        post_error(c, id, kParamsPlaneSet, "the plane was set already");
        return;
      }
      p.plane = util::move(fd);
      p.offset = offset;
      p.stride = stride;
      p.modifier = uint64_t{hi} << 32 | lo;
      return;
    }
    case 2:        // create(width, height, format, flags): the answer is created(new wl_buffer) or failed
    case 3: {      // create_immed(new id, width, height, format, flags) (2)
      const bool immediate = opcode == 3;
      if (immediate && r.version < 2) {
        post_error(c, id, kErrorInvalidMethod, "zwp_linux_buffer_params_v1 has no such request");
        return;
      }
      const uint32_t object = immediate ? in.u() : 0;
      const int32_t width = in.i(), height = in.i();
      const uint32_t format = in.u(), flags = in.u();
      if (!in.ok()) return;
      BufferInfo made;
      if (!make_dmabuf_buffer(c, id, p, width, height, format, flags, immediate, &made)) return;
      uint32_t buffer = object;
      Resource* nr = immediate ? new_object(c, object, Iface::Buffer, 1, id) : new_server_object(c, Iface::Buffer, 1, &buffer);
      if (!nr) return;
      nr->buffer = util::move(made);
      if (!immediate) {
        Event e(id, 0);      // created(new wl_buffer)
        e.u(buffer);
        send(c, e);
      }
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "zwp_linux_buffer_params_v1 has no such request"); return;
  }
}

// ------------------------------------------------------------------ wp_linux_drm_syncobj_v1

bool Server::acquired(const State& state) {
  if (!state.acquire.set()) return true;
  GpuTimeline* t = state.acquire.timeline->timeline();
  return t && t->value() >= state.acquire.point;
}

void Server::signal_release(BufferInfo& b) {
  if (!b.release_timeline) return;
  if (GpuTimeline* t = b.release_timeline->timeline()) t->signal(b.release_point);
  b.release_timeline = Fd();
}

void Server::finish_waiting(Surface& s, bool force) {
  if (!s.waiting) return;
  if (!force && !acquired(s.waiting_state)) return;
  Client* c = client_of(s.client);
  State state = util::move(s.waiting_state);
  s.waiting_state = State();
  s.waiting = false;
  --waiting_count_;
  if (c) apply(*c, s, state);
}

void Server::poll_waiting() {
  if (waiting_count_ == 0) return;
  for (Surface& s : surfaces_) {
    if (!s.waiting) continue;
    finish_waiting(s, false);
    if (s.waiting) {      // (still not reached: the desktop comes back for it)
      Surface* top = &s;
      while (top->role == Role::Subsurface && top->parent) top = top->parent;
      note_repaint(*top, {0, 0, 1, 1});
    }
  }
}

// The rules of a commit on a surface with explicit sync (wp_linux_drm_syncobj_surface_v1): points need a buffer, a buffer needs both points, the buffer must be
// a dmabuf, and on one timeline the release point must come after the acquire point.
bool Server::check_sync_points(Client& c, Surface& s) {
  State& p = s.pending;
  if (s.syncobj_id == 0) return true;
  const bool buffer = p.attached && p.buffer.valid();
  if ((p.acquire.set() || p.release.set()) && !buffer) {
    post_error(c, s.syncobj_id, kSyncobjNoBuffer, "sync points without a buffer");
    return false;
  }
  if (!buffer) return true;
  if (!p.acquire.set()) {
    post_error(c, s.syncobj_id, kSyncobjNoAcquirePoint, "a buffer without an acquire point");
    return false;
  }
  if (!p.release.set()) {
    post_error(c, s.syncobj_id, kSyncobjNoReleasePoint, "a buffer without a release point");
    return false;
  }
  if (!p.buffer.gpu) {
    post_error(c, s.syncobj_id, kSyncobjUnsupportedBuffer, "explicit sync needs a dmabuf");
    return false;
  }
  if (p.acquire.timeline->timeline() == p.release.timeline->timeline() && p.acquire.point >= p.release.point) {
    post_error(c, s.syncobj_id, kSyncobjConflictingPoints, "the release point is not after the acquire point");
    return false;
  }
  return true;
}

void Server::syncobj_request(Client& c, uint32_t id, Resource& r, uint16_t opcode, Reader& in) {
  if (r.iface == Iface::SyncobjManager) {
    switch (opcode) {
      case 0: destroy_object(c, id); return;      // destroy
      case 1: {                                   // get_surface(new id, surface)
        const uint32_t object = in.u(), surface = in.object();
        if (!in.ok()) return;
        Resource* sr = object_of(c, surface, Iface::Surface, id);
        if (!sr || !sr->surface) return;
        if (sr->surface->syncobj_id != 0) {
          post_error(c, id, kSyncobjSurfaceExists, "the surface has explicit sync already");
          return;
        }
        Surface* s = sr->surface;
        Resource* nr = new_object(c, object, Iface::SyncobjSurface, 1, id);
        if (!nr) return;
        nr->surface = s;
        s->syncobj_id = object;
        return;
      }
      case 2: {      // import_timeline(new id, fd)
        const uint32_t object = in.u();
        Fd fd = in.fd();
        if (!in.ok()) return;
        if (!fd || !fd->timeline()) {
          post_error(c, id, kSyncobjInvalidTimeline, "not a timeline");
          return;
        }
        Resource* nr = new_object(c, object, Iface::SyncobjTimeline, 1, id);
        if (nr) nr->timeline = util::move(fd);
        return;
      }
      default: post_error(c, id, kErrorInvalidMethod, "wp_linux_drm_syncobj_manager_v1 has no such request"); return;
    }
  }
  if (r.iface == Iface::SyncobjTimeline) {
    if (opcode == 0) destroy_object(c, id);      // destroy (points set with it keep the timeline)
    else post_error(c, id, kErrorInvalidMethod, "wp_linux_drm_syncobj_timeline_v1 has no such request");
    return;
  }
  // wp_linux_drm_syncobj_surface_v1
  Surface* s = r.surface;
  switch (opcode) {
    case 0:      // destroy: back to implicit sync
      if (s) {
        s->syncobj_id = 0;
        s->pending.acquire = SyncPoint();
        s->pending.release = SyncPoint();
      }
      destroy_object(c, id);
      return;
    case 1:        // set_acquire_point(timeline, point_hi, point_lo)
    case 2: {      // set_release_point
      const uint32_t timeline = in.object(), hi = in.u(), lo = in.u();
      if (!in.ok()) return;
      if (!s) {
        post_error(c, id, kSyncobjNoSurface, "the surface is gone");
        return;
      }
      Resource* tr = object_of(c, timeline, Iface::SyncobjTimeline, id);
      if (!tr) return;
      SyncPoint& point = opcode == 1 ? s->pending.acquire : s->pending.release;
      point.timeline = tr->timeline;
      point.point = uint64_t{hi} << 32 | lo;
      return;
    }
    default: post_error(c, id, kErrorInvalidMethod, "wp_linux_drm_syncobj_surface_v1 has no such request"); return;
  }
}

}  // namespace wayland
