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

// The front ends of the kernel's OpenGL over GlService: NtTcosOpenGl for Windows programs and /dev/dxg's TCOS_DXG_IOCTL_GL for Linux programs, one request layout
// (src/Process/WindowsOpenGl.hpp) for both. They differ in where the memory handed to the program comes from and in that a Linux window is never shown by the kernel.
#include "OpenGl/WindowsGl.hpp"
#include "OpenGl/GlService.hpp"
#include "Process/LinuxFiles.hpp"
#include "Process/WindowsMemory.hpp"
#include "Process/WindowsOpenGl.hpp"

namespace opengl {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNoMemory = 0xC0000017u, kNotSupported = 0xC00000BBu,
                   kInvalidDeviceState = 0xC0000184u, kResourceInUse = 0xC0000708u;
constexpr uint32_t kRequestSize = 64;
enum : uint32_t { kCreate = 1, kDelete, kMakeCurrent, kExecute, kSwap, kReadFront, kShareLists, kFlushed, kDestroySurface };

uint32_t le32(ConstByteView b, uint32_t at) { return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24; }
uint64_t le64(ConstByteView b, uint32_t at) { return uint64_t(le32(b, at)) | uint64_t(le32(b, at + 4)) << 32; }

uint32_t to_nt(GlStatus s) {
  switch (s) {
    case GlStatus::Ok: return kSuccess;
    case GlStatus::InvalidHandle: return kInvalidHandle;
    case GlStatus::InvalidParameter: return kInvalidParameter;
    case GlStatus::NoMemory: return kNoMemory;
    case GlStatus::NoContext: return kInvalidDeviceState;
    case GlStatus::Busy: return kResourceInUse;
  }
  return kInvalidParameter;
}

// The program's memory as the GL reaches it.
class ProcessMemory final : public IClientMemory {
 public:
  ProcessMemory(process::Process& p, bool linux_process) : p_(p), linux_(linux_process) {}
  bool read(uint64_t address, ByteView out) override { return p_.read_user(address, out); }
  bool write(uint64_t address, ConstByteView data) override { return p_.write_user(address, data); }
  uint64_t allocate(uint64_t bytes) override {
    return linux_ ? process::linuxabi::map_user_memory(p_, bytes) : process::windowsabi::allocate_user_memory(p_, bytes);
  }
  void release(uint64_t address, uint64_t bytes) override {
    if (linux_) process::linuxabi::unmap_user_memory(p_, address, bytes);
    else process::windowsabi::free_user_memory(p_, address);
  }

 private:
  process::Process& p_;
  bool linux_;
};

struct PresentState {
  bool shown = false;
};

void present_to_window(void* context, uint64_t window, ConstByteView rgba, uint32_t width, uint32_t height) {
  PresentState& st = *static_cast<PresentState*>(context);
  process::windowsabi::GlWindowPresent present = process::windowsabi::gl_window_present();
  st.shown = present && present(window, rgba, width, height);
}

uint64_t thread_of(process::Process& p) {
  process::IProcessRuntime* rt = p.runtime();
  return rt ? rt->current_thread_id() : 1;
}

}  // namespace

uint32_t gl_request(process::Process& p, uint64_t at, bool linux_process) {
  GlService* service = system_gl();
  if (!service) return kNotSupported;
  uint8_t storage[kRequestSize];
  if (!p.read_user(at, ByteView(storage, kRequestSize))) return kAccessViolation;
  const ConstByteView q(storage, kRequestSize);
  if (le32(q, 0) != kRequestSize) return kInvalidParameter;
  const uint32_t context = le32(q, 8), share = le32(q, 12), length = le32(q, 32), width = le32(q, 36), height = le32(q, 40), flags = le32(q, 44);
  const uint64_t window = le64(q, 16), data = le64(q, 24);
  const uint64_t pid = p.id();
  const auto put_result = [&](uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = uint8_t(value >> (8 * i));
    return p.write_user(at + 48, ConstByteView(b, 8));
  };
  switch (le32(q, 4)) {
    case kCreate: {
      ContextRequest cr;
      cr.major = le32(q, 56);
      cr.minor = le32(q, 60);
      cr.share = share;
      cr.core_profile = flags & 1;
      cr.forward_compatible = flags & 2;
      cr.debug = flags & 4;
      cr.es = flags & 8;
      uint32_t handle = 0;
      const GlStatus s = service->create_context(pid, cr, &handle);
      if (s != GlStatus::Ok) return to_nt(s);
      return put_result(handle) ? kSuccess : kAccessViolation;
    }
    case kDelete: return to_nt(service->delete_context(pid, context));
    case kShareLists: return to_nt(service->share_objects(pid, share, context));
    case kMakeCurrent: {
      SurfaceRequest sr;
      sr.window = window;
      sr.width = width ? width : 1;
      sr.height = height ? height : 1;
      if (sr.width > 16384 || sr.height > 16384) return kInvalidParameter;
      sr.double_buffered = flags & 1;
      sr.depth_bits = flags & 2 ? 24 : 0;
      sr.stencil_bits = flags & 4 ? 8 : 0;
      sr.accum_bits = flags & 8 ? 64 : 0;
      sr.alpha_bits = flags & 16 ? 8 : 0;
      return to_nt(service->make_current(pid, thread_of(p), context, sr));
    }
    case kExecute: {
      if (length > kMaxStreamBytes || (length & 3)) return kInvalidParameter;
      ByteBuffer stream(service->allocator());
      if (!stream.put_zeros(length)) return kNoMemory;
      if (!p.read_user(data, stream.writable_view())) return kAccessViolation;
      ProcessMemory memory(p, linux_process);
      uint64_t result = 0;
      const GlStatus s = service->execute(pid, thread_of(p), stream.view(), memory, &result);
      if (s != GlStatus::Ok) return to_nt(s);
      return put_result(result) ? kSuccess : kAccessViolation;
    }
    case kSwap: case kFlushed: {
      if (width > 16384 || height > 16384) return kInvalidParameter;
      PresentState st;
      if (le32(q, 4) == kFlushed && !service->take_flushed(pid, window)) return put_result(2) ? kSuccess : kAccessViolation;
      const GlStatus s = service->swap(pid, window, width, height, le32(q, 4) == kSwap, linux_process ? nullptr : &present_to_window, &st);
      if (s != GlStatus::Ok) return to_nt(s);
      return put_result(st.shown ? 1 : 0) ? kSuccess : kAccessViolation;
    }
    case kReadFront: {
      uint32_t w = 0, h = 0;
      GlStatus s = service->read_front(pid, window, ByteView(), 0, &w, &h);
      if (s != GlStatus::Ok) return to_nt(s);
      uint8_t wh[8];
      for (int i = 0; i < 4; ++i) { wh[i] = uint8_t(w >> (8 * i)); wh[4 + i] = uint8_t(h >> (8 * i)); }
      if (!p.write_user(at + 36, ConstByteView(wh, 8))) return kAccessViolation;
      if (data == 0) return kSuccess;
      const uint64_t bytes = uint64_t{w} * h * 4;
      if (length < bytes) return kInvalidParameter;
      ByteBuffer pixels(service->allocator());
      if (!pixels.put_zeros(bytes)) return kNoMemory;
      s = service->read_front(pid, window, pixels.writable_view(), w * 4, &w, &h);
      if (s != GlStatus::Ok) return to_nt(s);
      return p.write_user(data, pixels.view()) ? kSuccess : kAccessViolation;
    }
    case kDestroySurface: return to_nt(service->destroy_surface(pid, window));
    default: return kInvalidParameter;
  }
}

uint32_t windows_gl_service(process::Process& p, const process::SyscallRequest& r) { return gl_request(p, r.args[0], false); }

namespace {
uint32_t linux_gl_request(process::Process& p, uint64_t request) { return gl_request(p, request, true); }
}  // namespace

void install_windows_gl(GlService& service) {
  set_system_gl(&service);
  process::windowsabi::set_opengl_handler(&windows_gl_service);
}

void install_linux_gl(GlService& service) {
  set_system_gl(&service);
  process::linuxabi::set_gl_request_handler(&linux_gl_request);
}

}  // namespace opengl
