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

// zwp_linux_dmabuf_v1 (version 3): wl_buffers whose picture is a GPU image a client passed as a descriptor (on this system an export of the kernel's Direct3D
// layer made with /dev/dxg, Process/DxgBufferFile.hpp). The desktop draws one in place (draw_tree), so a client that renders on the GPU shows its frames with
// no copy through the CPU.
//
// What is taken: one plane (every format offered has one), offset 0 and the image's own row pitch, the linear layout (DRM_FORMAT_MOD_LINEAR, or
// DRM_FORMAT_MOD_INVALID: "the driver's choice", which here is linear), no flags (y-inverted and interlaced pictures are refused), and a size no larger than the
// image. The four formats are ARGB8888, XRGB8888, ABGR8888 and XBGR8888: the format says whether the alpha counts; the order of the channels is the image's own
// (the Direct3D layer knows it), so a client's swizzle never shows.
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

}  // namespace

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
      default: post_error(c, id, kErrorInvalidMethod, "zwp_linux_dmabuf_v1 has no such request"); return;
    }
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

}  // namespace wayland
