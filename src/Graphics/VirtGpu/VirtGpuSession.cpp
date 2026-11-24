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

#include "Graphics/VirtGpu/VirtGpuSession.hpp"
#include <new>
#include "Process/VirtGpuDevice.h"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

namespace graphics::virtgpu {

namespace {
constexpr uint64_t kPage = 4096;
constexpr uint32_t kMaxCapsetId = 63;
constexpr uint32_t kBlobFlagMask = TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE | TCOS_VIRTGPU_BLOB_FLAG_USE_SHAREABLE | TCOS_VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE;
constexpr uint32_t kDeferMapping = 1;   // DRM_VIRTGPU_BLOB_FLAG_HINT_DEFER_MAPPING
constexpr uint64_t kMaxBufferBytes = 1ull << 36;   // (no program asks for 64 GiB at once; a size above it is a garbage field)

int32_t fail(Errno e) { return -static_cast<int32_t>(e); }

bool page_round(uint64_t size, uint64_t* out) {
  if (size == 0 || size > kMaxBufferBytes) return false;
  *out = (size + kPage - 1) & ~(kPage - 1);
  return true;
}

uint64_t length_of(const char* text) {
  uint64_t n = 0;
  while (text[n]) ++n;
  return n;
}

// drm_copy_field: as much of `value` as fits the program's buffer; the length is the whole value's.
bool copy_field(process::IUserMemory& memory, uint64_t* length, uint64_t buffer, const char* value) {
  const uint64_t full = length_of(value);
  const uint64_t n = full < *length ? full : *length;
  *length = full;
  if (n && buffer) return memory.write(buffer, ConstByteView(reinterpret_cast<const uint8_t*>(value), n));
  return true;
}
}  // namespace

// ---- Buffer ---------------------------------------------------------------------------------------------------------------------------------------------

void Buffer::release() {
  if (__atomic_sub_fetch(&references_, 1, __ATOMIC_ACQ_REL) != 0) return;
  // The host may still read or write the guest backing for work it has not finished: that memory is freed only after its fence (or, if the host stopped
  // answering, after Session::kWaitTimeoutUs: by then the device is lost anyway).
  const uint64_t f = fence();
  if (f) host_.wait_fence(f, Session::kWaitTimeoutUs);
  if (host_physical_) host_.unmap_blob(context_, resource_, host_physical_);
  if (resource_) host_.unref_resource(resource_);
  if (!guest_.empty()) release_view(allocator_, guest_);
  allocators::IAllocator& allocator = allocator_;
  this->~Buffer();
  allocator.deallocate(this);
}

bool Buffer::map_now() {
  if (storage_) return true;
  if (!host_blob() || !(blob_flags_ & TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE)) return false;
  uint64_t physical = 0;
  uint32_t info = 0;
  if (!host_.map_blob(context_, resource_, size_, &physical, &info)) return false;
  host_physical_ = physical;
  map_info_ = info;
  storage_ = reinterpret_cast<uint8_t*>(physical);   // (the kernel maps physical memory 1:1: the window's pages are at their own addresses)
  return true;
}

// ---- Session --------------------------------------------------------------------------------------------------------------------------------------------

Session::Session(IVirtGpuHost& host, allocators::IAllocator& allocator) : host_(host), allocator_(allocator), handles_(allocator) {}

Session::~Session() {
  handles_.clear();   // (each handle's reference goes; buffers that descriptors or mappings still hold stay until those go)
  if (context_) host_.destroy_context(context_);
}

uint32_t Session::handle_count() {
  lock_.lock();
  const uint32_t n = static_cast<uint32_t>(handles_.count());
  lock_.unlock();
  return n;
}

BufferRef Session::buffer(uint32_t handle) {
  lock_.lock();
  BufferRef found;
  if (Handle* h = handles_.find(handle)) found = h->buffer;
  lock_.unlock();
  return found;
}

BufferRef Session::buffer_at(uint64_t offset, uint64_t size) {
  BufferRef found;
  lock_.lock();
  for (auto& entry : handles_) {
    const Handle& h = entry.value;
    if (h.offset == 0 || offset < h.offset || offset - h.offset >= h.buffer->size()) continue;
    if (offset != h.offset || size > h.buffer->size()) break;   // (a mapping starts at the buffer's offset and fits in it, as Linux's vma offset lookup wants)
    if (h.buffer->mappable()) found = h.buffer;
    break;
  }
  lock_.unlock();
  return found;
}

uint32_t Session::add_handle(const BufferRef& buffer) {
  lock_.lock();
  uint32_t handle = next_handle_;
  for (uint32_t tries = 0; tries < 16 && (handle == 0 || handles_.find(handle)); ++tries) handle = ++next_handle_;
  next_handle_ = handle + 1;
  Handle h;
  h.buffer = buffer;
  const bool ok = handle != 0 && !handles_.find(handle) && handles_.put(handle, h);
  lock_.unlock();
  return ok ? handle : 0;
}

BufferRef Session::make_guest_buffer(uint64_t size) {
  void* memory = allocator_.allocate_aligned(sizeof(Buffer), alignof(Buffer));
  if (!memory) return BufferRef();
  BufferRef buffer(new (memory) Buffer(host_, allocator_));
  buffer->guest_ = allocate_dma_view<uint8_t>(allocator_, size, kPage);
  if (buffer->guest_.empty()) return BufferRef();
  buffer->guest_.fill(0);
  buffer->storage_ = buffer->guest_.data();
  buffer->size_ = size;
  return buffer;
}

uint32_t Session::ensure_context() {
  lock_.lock();
  if (!context_) {
    // (Linux's virtio_gpu_create_context: the type CONTEXT_INIT chose, or a virgl context; named by the program's debug name or "tcos")
    const char fallback[] = "tcos";
    const ConstByteView name = debug_name_length_ ? ConstByteView(reinterpret_cast<const uint8_t*>(debug_name_), debug_name_length_)
                                                  : ConstByteView(reinterpret_cast<const uint8_t*>(fallback), sizeof(fallback) - 1);
    context_ = host_.create_context(capset_, name);
  }
  const uint32_t c = context_;
  lock_.unlock();
  return c;
}

int32_t Session::ioctl(process::IUserMemory& memory, uint32_t request, uint64_t argument, ISessionFiles* files) {
  // drm_ioctl: the number picks the call (its type and direction bits are not looked at); the structure is copied in as far as the program's size and the
  // kernel's allow, the rest zero, and copied out the same way.
  const uint32_t nr = TCOS_IOC_NR(request);
  const uint32_t user_size = TCOS_IOC_SIZE(request);
  struct Call {
    uint32_t size;
    bool in, out;
  };
  Call call{0, false, false};
  enum class Which : uint8_t { None, Version, GemClose, GetCap, PrimeToFd, PrimeToHandle, Getparam, GetCaps, ContextInit, CreateBlob, CreateResource,
                               ResourceInfo, TransferFrom, TransferTo, Wait, Map, Execbuffer, Denied, NoSyncobj };
  Which which = Which::None;
  switch (nr) {
    case 0x00: which = Which::Version; call = {sizeof(TcosDrmVersion), true, true}; break;
    case 0x09: which = Which::GemClose; call = {sizeof(TcosDrmGemClose), true, false}; break;
    case 0x0c: which = Which::GetCap; call = {sizeof(TcosDrmGetCap), true, true}; break;
    case 0x2d: which = Which::PrimeToFd; call = {sizeof(TcosDrmPrimeHandle), true, true}; break;
    case 0x2e: which = Which::PrimeToHandle; call = {sizeof(TcosDrmPrimeHandle), true, true}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_MAP: which = Which::Map; call = {sizeof(TcosVirtgpuMap), true, true}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_EXECBUFFER: which = Which::Execbuffer; call = {sizeof(TcosVirtgpuExecbuffer), true, true}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_GETPARAM: which = Which::Getparam; call = {sizeof(TcosVirtgpuGetparam), true, true}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_RESOURCE_CREATE: which = Which::CreateResource; call = {sizeof(TcosVirtgpuResourceCreate), true, true}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_RESOURCE_INFO: which = Which::ResourceInfo; call = {sizeof(TcosVirtgpuResourceInfo), true, true}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_TRANSFER_FROM_HOST: which = Which::TransferFrom; call = {sizeof(TcosVirtgpuTransfer), true, false}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_TRANSFER_TO_HOST: which = Which::TransferTo; call = {sizeof(TcosVirtgpuTransfer), true, false}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_WAIT: which = Which::Wait; call = {sizeof(TcosVirtgpuWait), true, false}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_GET_CAPS: which = Which::GetCaps; call = {sizeof(TcosVirtgpuGetCaps), true, true}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_RESOURCE_CREATE_BLOB: which = Which::CreateBlob; call = {sizeof(TcosVirtgpuResourceCreateBlob), true, true}; break;
    case TCOS_DRM_COMMAND_BASE + TCOS_VIRTGPU_CONTEXT_INIT: which = Which::ContextInit; call = {sizeof(TcosVirtgpuContextInit), true, true}; break;
    default:
      if (nr >= 0xBF && nr <= 0xCF && nr != 0xCE) which = Which::NoSyncobj;   // (the syncobj calls: a driver without DRIVER_SYNCOBJ answers EOPNOTSUPP)
      else if (nr < TCOS_DRM_COMMAND_BASE || (nr >= 0xA0 && nr <= 0xD2)) which = Which::Denied;   // core calls a render node may not make (Linux: EACCES)
      break;
  }
  if (which == Which::None) return fail(kEinval);
  if (which == Which::Denied) return fail(kEacces);
  if (which == Which::NoSyncobj) return fail(kEopnotsupp);

  alignas(8) uint8_t data[64] = {};
  const uint32_t copy = user_size < call.size ? user_size : call.size;
  if (call.in && copy && !memory.read(argument, ByteView(data, copy))) return fail(kEfault);
  int32_t r = 0;
  switch (which) {
    case Which::Version: {
      auto* v = reinterpret_cast<TcosDrmVersion*>(data);
      v->version_major = TCOS_VIRTGPU_VERSION_MAJOR;
      v->version_minor = TCOS_VIRTGPU_VERSION_MINOR;
      v->version_patchlevel = TCOS_VIRTGPU_VERSION_PATCH;
      if (!copy_field(memory, &v->name_len, v->name, TCOS_VIRTGPU_DRIVER_NAME) || !copy_field(memory, &v->date_len, v->date, TCOS_VIRTGPU_DRIVER_DATE) ||
          !copy_field(memory, &v->desc_len, v->desc, TCOS_VIRTGPU_DRIVER_DESC))
        r = fail(kEfault);
      break;
    }
    case Which::GemClose: {
      const auto* c = reinterpret_cast<const TcosDrmGemClose*>(data);
      lock_.lock();
      const bool removed = handles_.remove(c->handle);
      lock_.unlock();
      r = removed ? 0 : fail(kEinval);
      break;
    }
    case Which::GetCap: {
      auto* c = reinterpret_cast<TcosDrmGetCap*>(data);
      c->value = 0;
      switch (c->capability) {
        case TCOS_DRM_CAP_TIMESTAMP_MONOTONIC: c->value = 1; break;
        case TCOS_DRM_CAP_PRIME: c->value = TCOS_DRM_PRIME_CAP_IMPORT | TCOS_DRM_PRIME_CAP_EXPORT; break;
        case TCOS_DRM_CAP_SYNCOBJ: case TCOS_DRM_CAP_SYNCOBJ_TIMELINE: c->value = 0; break;   // (no DRM sync objects: docs/Venus.md)
        // The mode-setting capabilities of virtio-gpu's KMS side (the values Linux's virtio-gpu reports)
        case TCOS_DRM_CAP_DUMB_BUFFER: c->value = 1; break;
        case 0x2: c->value = 1; break;      // VBLANK_HIGH_CRTC
        case 0x3: c->value = 0; break;      // DUMB_PREFERRED_DEPTH (not set by virtio-gpu)
        case 0x4: c->value = 0; break;      // DUMB_PREFER_SHADOW
        case 0x7: c->value = 0; break;      // ASYNC_PAGE_FLIP
        case 0x8: case 0x9: c->value = 64; break;   // CURSOR_WIDTH / HEIGHT
        case 0x10: c->value = 1; break;     // ADDFB2_MODIFIERS
        case 0x11: c->value = 0; break;     // PAGE_FLIP_TARGET
        case 0x12: c->value = 1; break;     // CRTC_IN_VBLANK_EVENT
        case 0x15: c->value = 0; break;     // ATOMIC_ASYNC_PAGE_FLIP
        default: r = fail(kEinval); break;
      }
      break;
    }
    case Which::PrimeToFd: {
      auto* p = reinterpret_cast<TcosDrmPrimeHandle*>(data);
      if (!files) { r = fail(kEopnotsupp); break; }
      if (p->flags & ~(TCOS_DRM_CLOEXEC | TCOS_DRM_RDWR)) { r = fail(kEinval); break; }
      const BufferRef b = buffer(p->handle);
      if (!b) { r = fail(kEnoent); break; }
      const int32_t fd = files->make_buffer_fd(b, p->flags);
      if (fd < 0) r = fd;
      else p->fd = fd;
      break;
    }
    case Which::PrimeToHandle: {
      auto* p = reinterpret_cast<TcosDrmPrimeHandle*>(data);
      if (!files) { r = fail(kEopnotsupp); break; }
      const BufferRef b = files->buffer_of_fd(p->fd);
      if (!b) { r = fail(kEinval); break; }
      if (&b->host() != &host_) { r = fail(kEinval); break; }
      uint32_t handle = 0;
      lock_.lock();
      for (auto& entry : handles_)
        if (entry.value.buffer == b) {   // (one handle per buffer and session, as drm_gem_prime_fd_to_handle keeps it)
          handle = entry.key;
          break;
        }
      lock_.unlock();
      if (!handle) handle = add_handle(b);
      if (!handle) r = fail(kEnomem);
      else p->handle = handle;
      break;
    }
    case Which::Getparam: {
      auto* g = reinterpret_cast<TcosVirtgpuGetparam*>(data);
      int32_t value = 0;
      switch (g->param) {
        case TCOS_VIRTGPU_PARAM_3D_FEATURES: value = host_.has_3d() ? 1 : 0; break;
        case TCOS_VIRTGPU_PARAM_CAPSET_QUERY_FIX: value = 1; break;
        case TCOS_VIRTGPU_PARAM_RESOURCE_BLOB: value = host_.has_blob() ? 1 : 0; break;
        case TCOS_VIRTGPU_PARAM_HOST_VISIBLE: value = host_.has_host_visible() ? 1 : 0; break;
        case TCOS_VIRTGPU_PARAM_CROSS_DEVICE: value = host_.has_resource_uuid() ? 1 : 0; break;
        case TCOS_VIRTGPU_PARAM_CONTEXT_INIT: value = host_.has_context_init() ? 1 : 0; break;
        case TCOS_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS: value = static_cast<int32_t>(host_.capset_mask()); break;   // (an int, as Linux copies it)
        case TCOS_VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME: value = host_.has_context_init() ? 1 : 0; break;
        default: r = fail(kEinval); break;
      }
      if (r == 0) {
        const uint8_t out[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
        if (!memory.write(g->value, ConstByteView(out, 4))) r = fail(kEfault);
      }
      break;
    }
    case Which::GetCaps: {
      const auto* c = reinterpret_cast<const TcosVirtgpuGetCaps*>(data);
      if (host_.capset_mask() == 0) { r = fail(kEnosys); break; }
      if (c->size == 0) { r = fail(kEinval); break; }
      uint32_t max_version = 0, max_size = 0;
      if (!host_.capset_info(c->cap_set_id, &max_version, &max_size) || c->cap_set_ver > max_version) { r = fail(kEinval); break; }
      const uint32_t want = c->size < max_size ? c->size : max_size;
      uint8_t caps[4096];
      if (want > sizeof(caps)) { r = fail(kEinval); break; }
      const uint32_t got = host_.read_capset(c->cap_set_id, c->cap_set_ver, ByteView(caps, want));
      if (got == 0) { r = fail(kEinval); break; }
      if (!memory.write(c->addr, ConstByteView(caps, got))) r = fail(kEfault);
      break;
    }
    case Which::ContextInit: {
      const auto* init = reinterpret_cast<const TcosVirtgpuContextInit*>(data);
      if (!host_.has_context_init()) { r = fail(kEinval); break; }
      if (init->num_params > 4) { r = fail(kEinval); break; }   // (the four parameters there are)
      TcosVirtgpuContextSetParam params[4] = {};
      if (init->num_params && !memory.read(init->ctx_set_params, ByteView(reinterpret_cast<uint8_t*>(params), init->num_params * sizeof(params[0])))) {
        r = fail(kEfault);
        break;
      }
      lock_.lock();
      if (context_initialized_ || context_) {
        lock_.unlock();
        r = fail(kEexist);
        break;
      }
      uint32_t capset = 0, rings = 0;
      uint64_t poll = 0;
      bool has_capset = false, has_rings = false, has_poll = false, has_name = false;
      char name[64] = {};
      uint32_t name_length = 0;
      for (uint32_t i = 0; i < init->num_params && r == 0; ++i) {
        const uint64_t value = params[i].value;
        switch (params[i].param) {
          case TCOS_VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
            if (value > kMaxCapsetId || !((host_.capset_mask() >> value) & 1) || has_capset) r = fail(kEinval);
            else capset = static_cast<uint32_t>(value), has_capset = true;
            break;
          case TCOS_VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
            if (has_rings || value > kMaxRings) r = fail(kEinval);
            else rings = static_cast<uint32_t>(value), has_rings = true;
            break;
          case TCOS_VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
            if (has_poll) r = fail(kEinval);
            else poll = value, has_poll = true;
            break;
          case TCOS_VIRTGPU_CONTEXT_PARAM_DEBUG_NAME: {
            if (has_name) { r = fail(kEinval); break; }
            // (strncpy_from_user of at most 63 bytes: up to the terminator, which must be readable byte by byte)
            for (name_length = 0; name_length < sizeof(name) - 1; ++name_length) {
              uint8_t c = 0;
              if (!memory.read(value + name_length, ByteView(&c, 1))) { r = fail(kEfault); break; }
              if (!c) break;
              name[name_length] = static_cast<char>(c);
            }
            has_name = true;
            break;
          }
          default: r = fail(kEinval); break;
        }
      }
      if (r == 0 && poll) {
        const uint64_t valid = rings >= 64 ? ~uint64_t{0} : (uint64_t{1} << rings) - 1;
        if (poll & ~valid) r = fail(kEinval);
      }
      if (r == 0) {
        capset_ = capset;
        num_rings_ = rings;
        poll_rings_mask_ = poll;
        if (has_name) {
          for (uint32_t i = 0; i < name_length; ++i) debug_name_[i] = name[i];
          debug_name_length_ = name_length;
        }
        const char fallback[] = "tcos";
        const ConstByteView shown = debug_name_length_ ? ConstByteView(reinterpret_cast<const uint8_t*>(debug_name_), debug_name_length_)
                                                       : ConstByteView(reinterpret_cast<const uint8_t*>(fallback), sizeof(fallback) - 1);
        context_ = host_.create_context(capset_, shown);
        if (!context_) r = fail(kEinval);   // (the host refused the type)
        else context_initialized_ = true;
      }
      lock_.unlock();
      break;
    }
    case Which::CreateBlob: {
      auto* b = reinterpret_cast<TcosVirtgpuResourceCreateBlob*>(data);
      if (user_size < TCOS_VIRTGPU_RESOURCE_CREATE_BLOB_SIZE_V1) { b->blob_hints = 0; }
      // verify_blob
      if (!host_.has_blob()) { r = fail(kEinval); break; }
      if ((b->blob_flags & ~kBlobFlagMask) || !b->blob_flags) { r = fail(kEinval); break; }
      if ((b->blob_flags & TCOS_VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE) && !host_.has_resource_uuid()) { r = fail(kEinval); break; }
      bool guest = false, host3d = false;
      switch (b->blob_mem) {
        case TCOS_VIRTGPU_BLOB_MEM_GUEST: guest = true; break;
        case TCOS_VIRTGPU_BLOB_MEM_HOST3D_GUEST: guest = true; host3d = true; break;
        case TCOS_VIRTGPU_BLOB_MEM_HOST3D: host3d = true; break;
        default: r = fail(kEinval); break;
      }
      if (r) break;
      if (host3d) {
        if (!host_.has_3d() || (b->cmd_size % 4) != 0) { r = fail(kEinval); break; }
      } else if (b->blob_id != 0 || b->cmd_size != 0) {
        r = fail(kEinval);
        break;
      }
      uint64_t size = 0;
      if (!page_round(b->size, &size)) { r = fail(kEinval); break; }
      if (!guest && (b->blob_flags & TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE) && !host_.has_host_visible()) { r = fail(kEinval); break; }
      const uint32_t context = host_.has_3d() ? ensure_context() : 0;
      if (host_.has_3d() && !context) { r = fail(kEnodev); break; }
      if (b->cmd_size) {
        if (b->cmd_size > host_.max_submit_bytes()) { r = fail(kEinval); break; }
        ByteBuffer commands(allocator_);
        if (!commands.put_zeros(b->cmd_size)) { r = fail(kEnomem); break; }
        if (!memory.read(b->cmd, commands.writable_view())) { r = fail(kEfault); break; }
        if (!host_.submit(context, commands.view(), ~0u)) { r = fail(kEnodev); break; }
      }
      BufferRef buffer;
      if (guest) {
        buffer = make_guest_buffer(size);
        if (!buffer) { r = fail(kEnomem); break; }
      } else {
        void* memory_for = allocator_.allocate_aligned(sizeof(Buffer), alignof(Buffer));
        if (!memory_for) { r = fail(kEnomem); break; }
        buffer = BufferRef(new (memory_for) Buffer(host_, allocator_));
        buffer->size_ = size;
      }
      buffer->blob_mem_ = b->blob_mem;
      buffer->blob_flags_ = b->blob_flags;
      buffer->context_ = context;
      const GuestRun run{reinterpret_cast<uint64_t>(buffer->guest_.data()), static_cast<uint32_t>(size)};
      if (guest && size > 0xFFFFFFFFull) { r = fail(kEinval); break; }   // (one run: a virtio memory entry is 32 bits long)
      buffer->resource_ = host_.create_blob(context, b->blob_mem, b->blob_flags, b->blob_id, size,
                                            guest ? ArrayView<const GuestRun>(&run, 1) : ArrayView<const GuestRun>());
      if (!buffer->resource_) { r = fail(kEinval); break; }
      if (!guest && (b->blob_flags & TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE) && !(b->blob_hints & kDeferMapping) && !buffer->map_now()) {
        r = fail(kEnomem);
        break;
      }
      const uint32_t handle = add_handle(buffer);
      if (!handle) { r = fail(kEnomem); break; }
      b->bo_handle = handle;
      b->res_handle = buffer->resource_;
      break;
    }
    case Which::CreateResource: {
      auto* c = reinterpret_cast<TcosVirtgpuResourceCreate*>(data);
      if (!host_.has_3d()) { r = fail(kEnodev); break; }   // (a render node exists only with 3D; the 2D path is the card node's)
      const uint32_t context = ensure_context();
      if (!context) { r = fail(kEnodev); break; }
      uint64_t size = 0;
      if (!page_round(c->size ? c->size : kPage, &size)) { r = fail(kEinval); break; }
      BufferRef buffer = make_guest_buffer(size);
      if (!buffer) { r = fail(kEnomem); break; }
      buffer->context_ = context;
      Resource3d d;
      d.target = c->target, d.format = c->format, d.bind = c->bind, d.width = c->width, d.height = c->height, d.depth = c->depth;
      d.array_size = c->array_size, d.last_level = c->last_level, d.samples = c->nr_samples, d.flags = c->flags;
      const GuestRun run{reinterpret_cast<uint64_t>(buffer->guest_.data()), static_cast<uint32_t>(size)};
      buffer->resource_ = host_.create_resource_3d(context, d, ArrayView<const GuestRun>(&run, 1));
      if (!buffer->resource_) { r = fail(kEinval); break; }
      const uint32_t handle = add_handle(buffer);
      if (!handle) { r = fail(kEnomem); break; }
      c->bo_handle = handle;
      c->res_handle = buffer->resource_;
      break;
    }
    case Which::ResourceInfo: {
      auto* i = reinterpret_cast<TcosVirtgpuResourceInfo*>(data);
      const BufferRef b = buffer(i->bo_handle);
      if (!b) { r = fail(kEnoent); break; }
      i->res_handle = b->resource();
      i->size = static_cast<uint32_t>(b->size());
      i->blob_mem = b->blob_mem();
      break;
    }
    case Which::TransferFrom:
    case Which::TransferTo: {
      const auto* t = reinterpret_cast<const TcosVirtgpuTransfer*>(data);
      if (!host_.has_3d()) { r = fail(kEnosys); break; }
      const BufferRef b = buffer(t->bo_handle);
      if (!b) { r = fail(kEnoent); break; }
      if (b->guest_.empty() || b->blob_mem() == TCOS_VIRTGPU_BLOB_MEM_GUEST) { r = fail(kEinval); break; }   // (nothing on one of the two sides)
      const uint32_t context = ensure_context();
      if (!context) { r = fail(kEnodev); break; }
      Box box{t->box.x, t->box.y, t->box.z, t->box.w, t->box.h, t->box.d};
      const uint64_t f = host_.transfer(context, b->resource(), which == Which::TransferTo, box, t->level, t->offset, t->stride, t->layer_stride);
      if (!f) { r = fail(kEnomem); break; }
      b->set_fence(f);
      break;
    }
    case Which::Wait: {
      const auto* w = reinterpret_cast<const TcosVirtgpuWait*>(data);
      const BufferRef b = buffer(w->handle);
      if (!b) { r = fail(kEnoent); break; }
      if (b->idle()) break;
      if (w->flags & TCOS_VIRTGPU_WAIT_NOWAIT) { r = fail(kEbusy); break; }
      if (!host_.wait_fence(b->fence(), kWaitTimeoutUs)) r = fail(kEbusy);
      break;
    }
    case Which::Map: {
      auto* m = reinterpret_cast<TcosVirtgpuMap*>(data);
      lock_.lock();
      Handle* h = handles_.find(m->handle);
      if (!h) {
        lock_.unlock();
        r = fail(kEnoent);
        break;
      }
      BufferRef b = h->buffer;
      if (!h->offset) {
        h->offset = next_offset_;
        next_offset_ += b->size();
      }
      m->offset = h->offset;
      lock_.unlock();
      // A host blob whose mapping was put off is mapped now (a non-mappable one gets its offset too; mmap() of it then fails, as on Linux).
      if (b->host_blob() && (b->blob_flags() & TCOS_VIRTGPU_BLOB_FLAG_USE_MAPPABLE) && !b->mappable() && !b->map_now()) r = fail(kEnomem);
      break;
    }
    case Which::Execbuffer: r = execbuffer(memory, argument, files); break;
    default: break;
  }
  if (which == Which::Execbuffer) return r;   // (it reads and writes its own structure)
  if (r == 0 && call.out && copy && !memory.write(argument, ConstByteView(data, copy))) return fail(kEfault);
  return r;
}

int32_t Session::execbuffer(process::IUserMemory& memory, uint64_t argument, ISessionFiles* files) {
  TcosVirtgpuExecbuffer e{};
  if (!memory.read(argument, bytes_of(e))) return fail(kEfault);
  if (!host_.has_3d()) return fail(kEnosys);
  if (e.flags & ~TCOS_VIRTGPU_EXECBUF_FLAGS) return fail(kEinval);
  uint32_t ring = ~0u;
  if (e.flags & TCOS_VIRTGPU_EXECBUF_RING_IDX) {
    lock_.lock();
    const uint32_t rings = num_rings_;
    lock_.unlock();
    if (e.ring_idx >= rings || rings == 0) return fail(kEinval);
    ring = e.ring_idx;
  }
  if (e.num_in_syncobjs || e.num_out_syncobjs) return fail(kEinval);   // (no sync objects exist: DRM_CAP_SYNCOBJ is 0)
  if ((e.flags & (TCOS_VIRTGPU_EXECBUF_FENCE_FD_IN | TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT)) && !files) return fail(kEopnotsupp);
  const uint32_t context = ensure_context();
  if (!context) return fail(kEnodev);
  // The in fence: work on the same ring is ordered by the host already; anything else is waited for here (dma_fence_wait).
  if (e.flags & TCOS_VIRTGPU_EXECBUF_FENCE_FD_IN) {
    FenceRef in;
    if (!files->fence_of_fd(e.fence_fd, &in)) return fail(kEinval);
    if (!(in.context == context && in.ring == ring && ring != ~0u) && in.fence) host_.wait_fence(in.fence, ~uint64_t{0});
  }
  // The buffers it names (each gets the submission's fence).
  constexpr uint32_t kMaxBuffers = 4096;
  if (e.num_bo_handles > kMaxBuffers) return fail(kEinval);
  ByteBuffer handle_bytes(allocator_);
  if (e.num_bo_handles) {
    if (!handle_bytes.put_zeros(uint64_t{e.num_bo_handles} * 4)) return fail(kEnomem);
    if (!memory.read(e.bo_handles, handle_bytes.writable_view())) return fail(kEfault);
  }
  ByteReader handle_reader(handle_bytes.view());
  for (uint32_t i = 0; i < e.num_bo_handles; ++i)
    if (!buffer(handle_reader.le32())) return fail(kEnoent);
  if (e.size > host_.max_submit_bytes() || (e.size & 3)) return fail(kEinval);
  ByteBuffer commands(allocator_);
  if (e.size) {
    if (!commands.put_zeros(e.size)) return fail(kEnomem);
    if (!memory.read(e.command, commands.writable_view())) return fail(kEfault);
  }
  const uint64_t fence = host_.submit(context, commands.view(), ring);
  if (!fence) return fail(kEnodev);
  ByteReader again(handle_bytes.view());
  for (uint32_t i = 0; i < e.num_bo_handles; ++i)
    if (const BufferRef b = buffer(again.le32())) b->set_fence(fence);
  if (ring != ~0u) {   // (a ring the program asked to hear about: an event on the node when the fence signals)
    lock_.lock();
    if ((poll_rings_mask_ >> ring) & 1) {
      if (polled_count_ == kMaxPolled) {   // (full: the oldest is reported as soon as it signals anyway; make room by dropping the oldest wait)
        for (uint32_t i = 1; i < polled_count_; ++i) polled_[i - 1] = polled_[i];
        --polled_count_;
      }
      polled_[polled_count_++] = fence;
    }
    lock_.unlock();
  }
  if (e.flags & TCOS_VIRTGPU_EXECBUF_FENCE_FD_OUT) {
    const int32_t fd = files->make_fence_fd(FenceRef{fence, ring, context}, true);
    if (fd < 0) return fd;
    e.fence_fd = fd;
    if (!memory.write(argument, bytes_of(e))) return fail(kEfault);
  }
  return 0;
}

bool Session::event_ready() {
  lock_.lock();
  bool ready = false;
  for (uint32_t i = 0; i < polled_count_ && !ready; ++i) ready = host_.fence_signaled(polled_[i]);
  lock_.unlock();
  return ready;
}

uint32_t Session::take_events(ByteView out) {
  uint32_t written = 0;
  lock_.lock();
  uint32_t kept = 0;
  for (uint32_t i = 0; i < polled_count_; ++i) {
    if (written + 8 <= out.size() && host_.fence_signaled(polled_[i])) {
      // struct drm_event { __u32 type; __u32 length; }: VIRTGPU_EVENT_FENCE_SIGNALED has no payload
      const uint32_t type = TCOS_VIRTGPU_EVENT_FENCE_SIGNALED, length = 8;
      const uint8_t event[8] = {uint8_t(type), uint8_t(type >> 8), uint8_t(type >> 16), uint8_t(type >> 24), uint8_t(length), 0, 0, 0};
      out.subview(written, 8).copy_from(ConstByteView(event, 8));
      written += 8;
    } else {
      polled_[kept++] = polled_[i];
    }
  }
  polled_count_ = kept;
  lock_.unlock();
  return written;
}

}  // namespace graphics::virtgpu

namespace graphics::virtgpu {
namespace {
IVirtGpuHost* g_system_host = nullptr;
}
void set_system_host(IVirtGpuHost* host) { __atomic_store_n(&g_system_host, host, __ATOMIC_RELEASE); }
IVirtGpuHost* system_host() { return __atomic_load_n(&g_system_host, __ATOMIC_ACQUIRE); }
}  // namespace graphics::virtgpu
