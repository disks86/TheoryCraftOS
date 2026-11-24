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

// One opening of the virtio-gpu render node (Linux: /dev/dri/renderD128; Windows: NtTcosVirtGpu OPEN): what Linux's virtio-gpu DRM driver keeps per open
// file (struct virtio_gpu_fpriv) and its ioctls. A session has at most one context on the device, made by CONTEXT_INIT (any context type the host has: Venus,
// the host's Vulkan, is the one this exists for) or, as Linux does, as a virgl context the first time something needs one. Its buffer objects (GEM objects:
// blob and 3D resources) are named by handles of the session's own, mapped into the process at the offsets MAP hands out, and can be passed to other
// sessions as descriptors (PRIME). Every submission is fenced; each buffer it names remembers the fence, which WAIT and the descriptors' polls look at.
//
// The calls follow Linux's drivers/gpu/drm/virtio (6.12): the same checks in the same order, the same error numbers (returned negative: -EINVAL ...). docs/Venus.md.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Graphics/VirtGpu/IVirtGpuHost.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Process.hpp"

namespace graphics::virtgpu {

// Linux error numbers the calls return (negated).
enum Errno : int32_t {
  kEperm = 1, kEnoent = 2, kEbadf = 9, kEnomem = 12, kEacces = 13, kEfault = 14, kEbusy = 16, kEexist = 17, kEnodev = 19, kEinval = 22, kEnotty = 25,
  kEnospc = 28, kEnosys = 38, kEopnotsupp = 95, kEtimedout = 110,
};

// A buffer object: a device resource and the guest memory or host mapping that shows it. Held by references (the session's handles, the descriptors that
// export it, every mapping of it into a process: it is the SharedMemory of those); the last one gives the resource back to the device.
class Buffer final : public process::SharedMemory {
 public:
  void retain() override { __atomic_add_fetch(&references_, 1, __ATOMIC_ACQ_REL); }
  void release() override;

  uint32_t resource() const { return resource_; }
  uint64_t size() const { return size_; }           // (page multiple)
  uint32_t blob_mem() const { return blob_mem_; }   // 0 for a 3D resource that is not a blob
  uint32_t blob_flags() const { return blob_flags_; }
  bool host_blob() const { return blob_mem_ == 2; }  // memory of the host's context (TCOS_VIRTGPU_BLOB_MEM_HOST3D)
  // Can the process see it (map it)? Guest memory always; host memory once mapped into the device window.
  bool mappable() const { return storage_ != nullptr; }
  // The memory a mapping shows (physical = this address: the kernel maps 1:1), `size()` bytes; null if it has none.
  uint8_t* storage() const { return storage_; }
  uint32_t map_info() const { return map_info_; }   // the host's cache mode for a host mapping (kMapCache*)
  // How a process must map it: as the host said for a host mapping (write-combined or uncached memory read through write-back cache lines would be stale),
  // write-back for guest memory.
  process::MemoryCache cpu_cache() const {
    switch (map_info_ & 0x0F) {
      case 2: return process::MemoryCache::Uncached;         // VIRTIO_GPU_MAP_CACHE_UNCACHED
      case 3: return process::MemoryCache::WriteCombining;   // VIRTIO_GPU_MAP_CACHE_WC
      default: return process::MemoryCache::WriteBack;       // CACHED (or NONE, guest memory)
    }
  }
  // The last fenced work that used it (0: none).
  uint64_t fence() const { return __atomic_load_n(&fence_, __ATOMIC_ACQUIRE); }
  void set_fence(uint64_t fence) { __atomic_store_n(&fence_, fence, __ATOMIC_RELEASE); }
  bool idle() const { const uint64_t f = fence(); return f == 0 || host_.fence_signaled(f); }
  // Makes a deferred host mapping (DRM_VIRTGPU_BLOB_FLAG_HINT_DEFER_MAPPING) now. False if the window is full or the host refused.
  bool map_now();
  IVirtGpuHost& host() const { return host_; }

 private:
  friend class Session;
  Buffer(IVirtGpuHost& host, allocators::IAllocator& allocator) : host_(host), allocator_(allocator) {}
  ~Buffer() = default;
  IVirtGpuHost& host_;
  allocators::IAllocator& allocator_;
  uint32_t resource_ = 0, context_ = 0, blob_mem_ = 0, blob_flags_ = 0, map_info_ = 0;
  uint64_t size_ = 0;
  uint8_t* storage_ = nullptr;
  ByteView guest_;          // guest backing this buffer owns (DMA memory)
  uint64_t host_physical_ = 0;   // where a host blob is mapped in the window (0: not mapped)
  uint64_t fence_ = 0;
  uint32_t references_ = 1;
};

// One use of a buffer (copyable: each copy is a reference).
class BufferRef {
 public:
  BufferRef() = default;
  explicit BufferRef(Buffer* adopted) : buffer_(adopted) {}   // takes over a reference the caller had
  BufferRef(const BufferRef& other) : buffer_(other.buffer_) { if (buffer_) buffer_->retain(); }
  BufferRef(BufferRef&& other) : buffer_(other.buffer_) { other.buffer_ = nullptr; }
  BufferRef& operator=(const BufferRef& other) {
    BufferRef copy(other);
    swap(copy);
    return *this;
  }
  BufferRef& operator=(BufferRef&& other) {
    BufferRef taken(static_cast<BufferRef&&>(other));
    swap(taken);
    return *this;
  }
  ~BufferRef() { reset(); }
  void reset() {
    if (buffer_) buffer_->release();
    buffer_ = nullptr;
  }
  Buffer* get() const { return buffer_; }
  Buffer* operator->() const { return buffer_; }
  explicit operator bool() const { return buffer_ != nullptr; }
  bool operator==(const BufferRef& o) const { return buffer_ == o.buffer_; }

 private:
  void swap(BufferRef& o) { Buffer* t = buffer_; buffer_ = o.buffer_; o.buffer_ = t; }
  Buffer* buffer_ = nullptr;
};

// A fence the device signals (a sync_file on Linux). Value type: fence numbers are never reused.
struct FenceRef {
  uint64_t fence = 0;
  uint32_t ring = ~0u;      // the ring it is on (~0u: the context's shared timeline)
  uint32_t context = 0;
};

// What only the system a session is opened from can do: descriptors (Linux). Windows passes none, and the calls that need one are EOPNOTSUPP there.
class ISessionFiles {
 public:
  // A descriptor for a fence (EXECBUFFER's out fence): the descriptor, or -errno.
  virtual int32_t make_fence_fd(const FenceRef& fence, bool close_on_exec) = 0;
  // The fence of descriptor `fd` (an in fence): false (EINVAL) if it is not a fence descriptor.
  virtual bool fence_of_fd(int32_t fd, FenceRef* out) = 0;
  // A descriptor for a buffer (PRIME_HANDLE_TO_FD): the descriptor, or -errno.
  virtual int32_t make_buffer_fd(const BufferRef& buffer, uint32_t flags) = 0;
  // The buffer of descriptor `fd`, or empty if it is not a buffer of this device (PRIME_FD_TO_HANDLE).
  virtual BufferRef buffer_of_fd(int32_t fd) = 0;

 protected:
  ISessionFiles() = default;
  ~ISessionFiles() = default;
};

class Session {
 public:
  Session(IVirtGpuHost& host, allocators::IAllocator& allocator);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // One ioctl of the render node: `request` (a TCOS_DRM_IOCTL_* number), `argument` the address of its structure in `memory`. 0 or -errno.
  int32_t ioctl(process::IUserMemory& memory, uint32_t request, uint64_t argument, ISessionFiles* files);

  // What mmap(offset, size) of the node maps: the buffer MAP gave `offset` for, if `size` bytes from there fit it and it can be seen (empty otherwise).
  BufferRef buffer_at(uint64_t offset, uint64_t size);
  // The buffer of a handle (empty if there is none).
  BufferRef buffer(uint32_t handle);
  // The session's context (0 until one is made) and its type.
  uint32_t context() const { return context_; }
  uint32_t capset() const { return capset_; }
  uint32_t handle_count();
  // The fence events of the rings CONTEXT_INIT's POLL_RINGS_MASK named (what read() and poll() of the node give: struct drm_event of type
  // VIRTGPU_EVENT_FENCE_SIGNALED, 8 bytes each). Whether one is ready, and taking as many as fit `out` (the bytes written).
  bool event_ready();
  uint32_t take_events(ByteView out);

  static constexpr uint64_t kMapOffsetBase = 0x100000000ull;   // (Linux's DRM_FILE_PAGE_OFFSET_START: 0x100000 pages)
  static constexpr uint32_t kMaxRings = 64;
  static constexpr uint64_t kWaitTimeoutUs = 15000000;        // WAIT gives up after 15 s, as Linux does (-EBUSY)

 private:
  struct Handle {
    BufferRef buffer;
    uint64_t offset = 0;   // what MAP gave (0 before)
  };
  int32_t execbuffer(process::IUserMemory& memory, uint64_t argument, ISessionFiles* files);

  // The context, made as a virgl one if there is none yet (Linux's virtio_gpu_create_context). 0 on failure.
  uint32_t ensure_context();
  // A new handle for `buffer`; 0 if the table cannot grow.
  uint32_t add_handle(const BufferRef& buffer);
  BufferRef make_guest_buffer(uint64_t size);   // guest DMA memory (zeroed), no resource yet

  IVirtGpuHost& host_;
  allocators::IAllocator& allocator_;
  kernel::Mutex lock_;                 // the context and the handle table
  uint32_t context_ = 0;
  uint32_t capset_ = 0;
  uint32_t num_rings_ = 0;
  uint64_t poll_rings_mask_ = 0;
  bool context_initialized_ = false;   // CONTEXT_INIT was given (a second one is -EEXIST)
  char debug_name_[64] = {};
  uint32_t debug_name_length_ = 0;
  Dictionary<uint32_t, Handle> handles_;
  uint32_t next_handle_ = 1;
  uint64_t next_offset_ = kMapOffsetBase;
  static constexpr uint32_t kMaxPolled = 256;
  uint64_t polled_[kMaxPolled] = {};   // fences on polled rings not reported yet
  uint32_t polled_count_ = 0;
};

}  // namespace graphics::virtgpu
