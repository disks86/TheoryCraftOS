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

#include "Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.hpp"
#include "Utility/ByteStream.hpp"
#include <new>
#include "Kernel/Apic.hpp"
#include "Kernel/Delay.hpp"
#include "console.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

using namespace virtio_gpu;

namespace {
// Sizes that fit comfortably in guest memory at 32 bpp; the host window follows the scanout size.
constexpr DisplayMode kModes[] = {
    {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1920, 1080},
};
constexpr uint32_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);
constexpr uint32_t kDefaultMode = 2;

template <class T>
ConstByteView as_bytes(const T& object) {
  return ConstByteView(reinterpret_cast<const uint8_t*>(&object), sizeof(T));
}

ControlHeader header(uint32_t type) {
  ControlHeader h{};
  h.type = type;
  return h;
}
}  // namespace

bool VirtioGpu::lock_for_command() {
  // A caller with interrupts masked (the console draws under its spinlock) must not block on the mutex while another thread has a command out: it looks
  // for the mutex again instead (blocking there is a kernel panic), and not for ever: the thread that holds it may be waiting for the host's answer on this
  // very core, which cannot switch to it while interrupts are masked - spinning on would hang the machine. It gives up, as for a host that does not answer
  // (the console skips a frame).
  if (alone_) return true;   // (no lock: see take_over_for_panic)
  if (kernel::irq_enabled() || !kernel::running()) {
    lock_.lock();
    return true;
  }
  const uint64_t start = uptime_us();
  while (!lock_.try_lock()) {
    if (uptime_us() - start >= kMaskedLockWaitUs) return false;
    delay_spin_us(20);
  }
  return true;
}

void VirtioGpu::reap_locked(bool* sync_done) {
  uint16_t index;
  uint32_t length;
  while (control_.take_used(&index, &length)) {
    if (index == 0) {
      if (in_flight_) in_flight_ = false;   // (the answer to a command that timed out: its buffers are ours again)
      else if (sync_done) *sync_done = true;
      continue;
    }
    if (index < 2) continue;
    const uint32_t slot = (index - 2) / 2;
    if (slot < kAsyncSlots && async_[slot].busy) {
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      __atomic_store_n(&async_[slot].busy, false, __ATOMIC_RELEASE);
    }
  }
}

uint32_t VirtioGpu::execute(ConstByteView request, ByteView response, bool fence, uint32_t context) {
  if (!ready_ || request.size() < sizeof(ControlHeader) || request.size() > request_.size()) return 0;
  if (!lock_for_command()) return 0;
  struct Release {
    kernel::Mutex& mutex;
    bool locked;
    ~Release() { if (locked) mutex.unlock(); }
  } guard{lock_, !alone_};
  reap_locked(nullptr);
  if (in_flight_) return 0;   // an earlier command never got its answer: the buffers still belong to the device
  request_.copy_from(request);
  auto* head = reinterpret_cast<ControlHeader*>(request_.data());
  uint64_t work = 0;   // (the number of this command, for a 3D one)
  if (head->type >= kCmdCtxCreate) {
    if (context == kContextId) {   // (work_sent/work_done count the virgl context's commands)
      work = work_sent_ + 1;
      __atomic_store_n(&work_sent_, work, __ATOMIC_RELEASE);
    }
    head->context_id = context;
    // With a fence the host answers when the work is finished (see execute() in the header). Either way our buffers are free to reuse once it answers: it has read the request.
    if (fence) {
      head->flags |= kFlagFence;
      head->fence_id = ++fence_;
    }
  }
  auto* reply = reinterpret_cast<ControlHeader*>(response_.data());
  reply->type = 0;

  const Virtqueue::Segment segments[2] = {{request_.data(), static_cast<uint32_t>(request.size()), false},
                                          {response_.data(), static_cast<uint32_t>(response_.size()), true}};
  control_.post_chain(0, segments, 2);
  transport_->notify(0);

  // A quick answer (a copy the host already finished) comes within microseconds: look for it by spinning. A slow one (the host drawing a frame, or holding a
  // flush for its display's refresh) takes milliseconds, and spinning that long is a core at 100% for nothing: after the first look the thread sleeps
  // (delay_us sleeps from 200 us on, where that is allowed) and checks again.
  const uint64_t start = uptime_us();
  const uint64_t limit = head->type >= kCmdCtxCreate ? k3dCommandTimeoutUs : kCommandTimeoutUs;
  bool answered = false;
  for (;;) {
    reap_locked(&answered);
    if (answered) break;
    const uint64_t waited = uptime_us() - start;
    if (waited >= limit) {
      in_flight_ = true;
      if (__atomic_fetch_add(&timeouts_, 1, __ATOMIC_ACQ_REL) % 16 == 0)
        console::debug_puts("virtio-gpu: the host is not answering (display hidden or busy?); skipping frames\n");
      return 0;
    }
    if (waited < kSpinWaitUs) delay_us(10);
    else if (interrupts_ && kernel::running() && kernel::irq_enabled()) completion_.wait(kInterruptWaitUs);   // woken the moment the host answers (a caller with interrupts masked, the console's lock, cannot sleep: it looks again)
    else delay_us(250);
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const uint32_t type = reply->type;
  // (an answer to a fenced command means the host's GPU has finished it and everything before it; an error is answered at once, without waiting for the fence)
  if (work && fence && type >= kRespOkNoData && type < kRespErrUnspec && work > __atomic_load_n(&work_done_, __ATOMIC_RELAXED)) __atomic_store_n(&work_done_, work, __ATOMIC_RELEASE);
  if (!response.empty()) response.copy_from(ConstByteView(response_).at_most(response.size()));
  return type;
}

uint64_t VirtioGpu::submit_async(uint32_t context, ConstByteView commands, uint32_t ring) {
  if (!ready_ || alone_ || context == 0 || context == kContextId || (commands.size() & 3)) return 0;
  if (commands.size() > max_submit_bytes()) return 0;
  if (ring != kNoRing && (ring > 63 || !has_context_init())) return 0;
  if (control_.size() < 2 + 2 * kAsyncSlots) return 0;   // (a queue too small for the slots: everything is synchronous)
  if (!lock_for_command()) return 0;
  struct Release {
    kernel::Mutex& mutex;
    ~Release() { mutex.unlock(); }
  } guard{lock_};
  reap_locked(nullptr);
  uint32_t slot = kAsyncSlots;
  for (uint32_t i = 0; i < kAsyncSlots; ++i)
    if (!__atomic_load_n(&async_[i].busy, __ATOMIC_ACQUIRE)) {
      slot = i;
      break;
    }
  if (slot == kAsyncSlots) return 0;
  AsyncSlot& s = async_[slot];
  const uint64_t needed = sizeof(CmdSubmit) + commands.size() + sizeof(ControlHeader);
  if (s.buffer.size() < needed) {   // (a slot's buffer grows to the largest submission it carried)
    if (!s.buffer.empty()) release_view(*allocator_, s.buffer);
    s.buffer = allocate_dma_view<uint8_t>(*allocator_, needed > kAsyncBufferBytes ? needed : kAsyncBufferBytes, 4096);
    if (s.buffer.empty()) return 0;
  }
  CmdSubmit head{};
  head.header = header(kCmdSubmit3d);
  head.header.context_id = context;
  head.header.flags = kFlagFence;
  head.header.fence_id = fence_ + 1;   // (published below, once the slot says it is outstanding: fence_signaled() must not see the number first)
  if (ring != kNoRing) {
    head.header.flags |= kFlagInfoRingIndex;
    head.header.ring_index = static_cast<uint8_t>(ring);
  }
  head.size = static_cast<uint32_t>(commands.size());
  const uint64_t request_bytes = sizeof(CmdSubmit) + commands.size();
  ByteWriter out(s.buffer.first(request_bytes));
  out.bytes(as_bytes(head));
  out.bytes(commands);
  if (!out.ok()) return 0;
  s.fence = head.header.fence_id;
  s.request_bytes = static_cast<uint32_t>(request_bytes);
  __atomic_store_n(&s.busy, true, __ATOMIC_RELEASE);
  __atomic_store_n(&fence_, s.fence, __ATOMIC_RELEASE);
  ByteView answer = s.buffer.subview(request_bytes, sizeof(ControlHeader));
  const Virtqueue::Segment segments[2] = {{s.buffer.data(), static_cast<uint32_t>(request_bytes), false},
                                          {answer.data(), static_cast<uint32_t>(answer.size()), true}};
  control_.post_chain(static_cast<uint16_t>(2 + 2 * slot), segments, 2);
  transport_->notify(0);
  return s.fence;
}

bool VirtioGpu::submit_virgl_unanswered(ConstByteView commands) {
  if (!ready_ || alone_ || (commands.size() & 3) || commands.size() > max_submit_bytes()) return false;
  if (control_.size() < 2 + 2 * kAsyncSlots) return false;
  if (!lock_for_command()) return false;
  struct Release {
    kernel::Mutex& mutex;
    ~Release() { mutex.unlock(); }
  } guard{lock_};
  reap_locked(nullptr);
  if (in_flight_) return false;   // (an earlier command never got its answer: the host is not taking commands)
  uint32_t slot = kAsyncSlots;
  for (uint32_t i = 0; i < kAsyncSlots; ++i)
    if (!__atomic_load_n(&async_[i].busy, __ATOMIC_ACQUIRE)) {
      slot = i;
      break;
    }
  if (slot == kAsyncSlots) return false;
  AsyncSlot& s = async_[slot];
  const uint64_t needed = sizeof(CmdSubmit) + commands.size() + sizeof(ControlHeader);
  if (s.buffer.size() < needed) {
    if (!s.buffer.empty()) release_view(*allocator_, s.buffer);
    s.buffer = allocate_dma_view<uint8_t>(*allocator_, needed > kAsyncBufferBytes ? needed : kAsyncBufferBytes, 4096);
    if (s.buffer.empty()) return false;
  }
  CmdSubmit head{};
  head.header = header(kCmdSubmit3d);
  head.header.context_id = kContextId;
  head.size = static_cast<uint32_t>(commands.size());
  const uint64_t request_bytes = sizeof(CmdSubmit) + commands.size();
  ByteWriter out(s.buffer.first(request_bytes));
  out.bytes(as_bytes(head));
  out.bytes(commands);
  if (!out.ok()) return false;
  s.fence = 0;   // (no fence: fence_signaled never looks for it)
  s.request_bytes = static_cast<uint32_t>(request_bytes);
  __atomic_store_n(&work_sent_, work_sent_ + 1, __ATOMIC_RELEASE);   // (a 3D command of the virgl context: see execute())
  __atomic_store_n(&s.busy, true, __ATOMIC_RELEASE);
  ByteView answer = s.buffer.subview(request_bytes, sizeof(ControlHeader));
  const Virtqueue::Segment segments[2] = {{s.buffer.data(), static_cast<uint32_t>(request_bytes), false},
                                          {answer.data(), static_cast<uint32_t>(answer.size()), true}};
  control_.post_chain(static_cast<uint16_t>(2 + 2 * slot), segments, 2);
  transport_->notify(0);
  return true;
}

bool VirtioGpu::fence_signaled(uint64_t fence) {
  if (fence == 0 || fence > __atomic_load_n(&fence_, __ATOMIC_ACQUIRE)) return false;
  if (lock_.try_lock()) {   // (collect answers that came in; a holder of the lock does that itself)
    reap_locked(nullptr);
    lock_.unlock();
  }
  for (const AsyncSlot& s : async_)
    if (__atomic_load_n(&s.busy, __ATOMIC_ACQUIRE) && s.fence == fence) return false;
  return true;
}

bool VirtioGpu::wait_fence(uint64_t fence, uint64_t timeout_us) {
  const uint64_t start = uptime_us();
  for (uint64_t waited = 0;; waited = uptime_us() - start) {
    if (fence_signaled(fence)) return true;
    if (!ready_) return false;
    if (timeout_us != ~uint64_t{0} && waited >= timeout_us) return false;
    if (waited < kSpinWaitUs) delay_us(10);
    else if (interrupts_ && kernel::running() && kernel::irq_enabled()) completion_.wait(kInterruptWaitUs);
    else delay_us(250);
  }
}

kernel::Frame* VirtioGpu::on_interrupt(kernel::Frame* frame, void* self) {
  VirtioGpu* gpu = static_cast<VirtioGpu*>(self);
  kernel::apic::eoi();
  __atomic_store_n(&gpu->last_interrupt_core_, kernel::current_core(), __ATOMIC_RELEASE);
  __atomic_fetch_add(&gpu->interrupt_count_, 1, __ATOMIC_ACQ_REL);
  gpu->completion_.set();
  return frame;
}

bool VirtioGpu::enable_interrupts() {
  if (interrupts_) return true;
  if (!ready_ || !transport_ || !kernel::running()) return false;
  // The completions come to this core: the threads that wait for them (the desktop's) run here, and the scheduler sends any other waiter here too.
  const uint32_t core = kernel::current_core();
  io_device_ = kernel::register_io_device("virtio-gpu", static_cast<int32_t>(core));
  if (!kernel::set_device_interrupt_handler(kernel::kVectorVirtioGpu, io_device_, &VirtioGpu::on_interrupt, this))
    kernel::set_interrupt_handler(kernel::kVectorVirtioGpu, &VirtioGpu::on_interrupt, this);
  if (!transport_->enable_msix(kernel::kVectorVirtioGpu, kernel::apic::this_apic_id(), 0)) {
    drop_interrupt_handler();
    return false;
  }
  // Try it out: one command that needs an answer; the interrupt for it must arrive. Without it the device or the machine does not deliver (a message
  // address the interrupt controller ignores, ...) and polling stays.
  completion_.reset();
  const uint32_t before = interrupt_count();
  read_display_info();
  for (int i = 0; i < 50 && interrupt_count() == before; ++i) delay_us(100);
  if (interrupt_count() == before) {
    transport_->disable_msix();
    drop_interrupt_handler();
    return false;
  }
  interrupts_ = true;
  kernel::set_io_device_retarget(io_device_, &VirtioGpu::retarget_interrupt, this);   // (the scheduler may move it to where its waiters run)
  return true;
}

bool VirtioGpu::move_interrupt(uint32_t core) {
  if (!interrupts_) return false;
  if (!retarget_interrupt(this, core, kernel::core_hardware_id(core))) return false;
  kernel::set_io_device_core(io_device_, static_cast<int32_t>(core));
  return true;
}

bool VirtioGpu::retarget_interrupt(void* self, uint32_t, uint64_t hardware_id) {
  VirtioGpu* gpu = static_cast<VirtioGpu*>(self);
  return hardware_id <= 0xFF && gpu->transport_ && gpu->transport_->retarget_msix(static_cast<uint32_t>(hardware_id));
}

void VirtioGpu::disable_interrupts() {
  if (!interrupts_) return;
  interrupts_ = false;                 // commands are waited for by polling from here on
  kernel::set_io_device_retarget(io_device_, nullptr, nullptr);   // (no move while MSI-X goes off)
  if (transport_) transport_->disable_msix();
  drop_interrupt_handler();
}

void VirtioGpu::drop_interrupt_handler() {
  kernel::set_device_interrupt_handler(kernel::kVectorVirtioGpu, io_device_, nullptr, nullptr);
  kernel::unregister_io_device(io_device_);
  io_device_ = -1;
}

bool VirtioGpu::initialize() {
  if (!pci_ || !allocator_ || device_.vendor_id != kPciVendor || device_.device_id != kPciDeviceId) return false;
  transport_ = new (transport_storage_) VirtioPciDevice(*pci_, device_, *allocator_, st_);
  transport_->request_features(kFeatureVirgl | kFeatureEdid | kFeatureResourceBlob | kFeatureContextInit);
  if (!transport_->initialize() || !transport_->setup_queue(0, &control_) || !transport_->start()) {
    transport_->shutdown();
    transport_ = nullptr;
    return false;
  }
  request_ = allocate_dma_view<uint8_t>(*allocator_, kRequestBytes, 4096);
  response_ = allocate_dma_view<uint8_t>(*allocator_, kResponseBytes, 4096);
  if (request_.empty() || response_.empty()) {
    shutdown();
    return false;
  }
  ready_ = true;
  output_count_ = 0;
  for (int i = 0; i < 4; ++i) output_count_ |= uint32_t{transport_->read_config8(8 + i)} << (8 * i);   // num_scanouts
  if (output_count_ == 0) output_count_ = 1;
  if (output_count_ > IMonitorSource::kMaxConnectors) output_count_ = IMonitorSource::kMaxConnectors;
  read_display_info();   // (which outputs have a display, and its size)

  if (!set_mode(kDefaultMode)) {
    shutdown();
    return false;
  }
  read_capsets();
  if (transport_->accepted_features() & kFeatureResourceBlob) {
    host_window_ = transport_->shared_memory(kShmHostVisible);
    if (host_window_.length && !window_.reset(0, host_window_.length)) host_window_ = {};
  }
  if (transport_->accepted_features() & kFeatureVirgl) has_3d_ = setup_3d();
  return true;
}

// Which capability sets the host has (GET_CAPSET_INFO for each of num_capsets): virgl's for the kernel's 3D, Venus and the others for contexts of their own.
void VirtioGpu::read_capsets() {
  capsets_ = 0;
  capset_count_ = 0;
  uint32_t count = 0;
  for (int i = 0; i < 4; ++i) count |= uint32_t{transport_->read_config8(12 + i)} << (8 * i);
  for (uint32_t i = 0; i < count && capset_count_ < kMaxCapsets; ++i) {
    GetCapsetInfo query{header(kCmdGetCapsetInfo), i, 0};
    RespCapsetInfo info{};
    if (execute(as_bytes(query), ByteView(reinterpret_cast<uint8_t*>(&info), sizeof(info))) != kRespOkCapsetInfo) continue;
    capset_infos_[capset_count_++] = CapsetInfo{info.capset_id, info.max_version, info.max_size};
    if (info.capset_id < 64) capsets_ |= uint64_t{1} << info.capset_id;
  }
}

bool VirtioGpu::capset_info(uint32_t id, uint32_t* max_version, uint32_t* max_size) const {
  for (uint32_t i = 0; i < capset_count_; ++i) {
    if (capset_infos_[i].id != id) continue;
    if (max_version) *max_version = capset_infos_[i].max_version;
    if (max_size) *max_size = capset_infos_[i].max_size;
    return true;
  }
  return false;
}

uint32_t VirtioGpu::read_capset(uint32_t id, uint32_t version, ByteView out) {
  uint32_t max_version = 0, max_size = 0;
  if (!capset_info(id, &max_version, &max_size) || version > max_version || out.empty()) return 0;
  if (max_size > kResponseBytes - sizeof(ControlHeader)) return 0;   // (no capset is near that size; the response buffer is the limit)
  GetCapset request{header(kCmdGetCapset), id, version};
  uint8_t reply[kResponseBytes];
  ByteView answer(reply, sizeof(ControlHeader) + max_size);
  if (execute(as_bytes(request), answer) != kRespOkCapset) return 0;
  const ConstByteView data = ConstByteView(answer).subview(sizeof(ControlHeader)).at_most(out.size());
  out.copy_from(data);
  return static_cast<uint32_t>(data.size());
}

uint32_t VirtioGpu::create_context(uint32_t capset_id, ConstByteView name) {
  // (capset 0: the host's default type, a virgl context, which needs no context-init)
  if (!ready_ || capset_id > kContextInitCapsetMask) return 0;
  if (capset_id == 0 ? !has_3d_ : (!has_context_init() || !has_capset(capset_id))) return 0;
  const uint32_t id = __atomic_fetch_add(&next_context_, 1u, __ATOMIC_RELAXED);
  if (id == 0 || id == kContextId) return 0;   // (wrapped: four billion contexts later)
  CtxCreate ctx{};
  ctx.header = header(kCmdCtxCreate);
  ctx.context_init = capset_id;
  const ConstByteView shown = name.at_most(sizeof(ctx.debug_name) - 1);
  ByteView(reinterpret_cast<uint8_t*>(ctx.debug_name), shown.size()).copy_from(shown);
  ctx.name_length = static_cast<uint32_t>(shown.size());
  return execute(as_bytes(ctx), ByteView(), true, id) == kRespOkNoData ? id : 0;
}

void VirtioGpu::destroy_context(uint32_t context) {
  if (!ready_ || context == 0 || context == kContextId) return;
  const ControlHeader destroy = header(kCmdCtxDestroy);
  execute(as_bytes(destroy), ByteView(), true, context);
}

uint32_t VirtioGpu::create_blob(uint32_t context, uint32_t blob_mem, uint32_t flags, uint64_t blob_id, uint64_t size, ArrayView<const MemEntry> pages) {
  if (!ready_ || !has_blob() || size == 0) return 0;
  if (pages.size() > (kRequestBytes - sizeof(ResourceCreateBlob)) / sizeof(MemEntry)) return 0;
  const uint32_t id = allocate_resource_id();
  ResourceCreateBlob create{};
  create.header = header(kCmdResourceCreateBlob);
  create.header.context_id = context;   // (a host blob is the context's memory; the host attaches the resource to it)
  create.resource_id = id;
  create.blob_mem = blob_mem;
  create.blob_flags = flags;
  create.entry_count = static_cast<uint32_t>(pages.size());
  create.blob_id = blob_id;
  create.size = size;
  ByteBuffer request(*allocator_);
  if (!request.append(as_bytes(create))) return 0;
  for (const MemEntry& entry : pages)
    if (!request.append(as_bytes(entry))) return 0;
  return execute(request.view(), ByteView(), true, context) == kRespOkNoData ? id : 0;
}

bool VirtioGpu::map_blob(uint32_t context, uint32_t resource, uint64_t size, uint64_t* physical, uint32_t* map_info) {
  if (!ready_ || !host_window_.length || size == 0 || !physical) return false;
  uint64_t bytes = 0;
  if (size > ~uint64_t{0} - 4095) return false;
  bytes = (size + 4095) & ~uint64_t{4095};
  Optional<uint64_t> offset;
  {
    window_lock_.lock();
    offset = window_.allocate(bytes, 4096);
    window_lock_.unlock();
  }
  if (!offset) return false;
  ResourceMapBlob map{};
  map.header = header(kCmdResourceMapBlob);
  map.resource_id = resource;
  map.offset = *offset;
  RespMapInfo info{};
  if (execute(as_bytes(map), ByteView(reinterpret_cast<uint8_t*>(&info), sizeof(info)), true, context) != kRespOkMapInfo) {
    window_lock_.lock();
    (void)window_.release(*offset);
    window_lock_.unlock();
    return false;
  }
  *physical = host_window_.address + *offset;
  if (map_info) *map_info = info.map_info;
  return true;
}

bool VirtioGpu::unmap_blob(uint32_t context, uint32_t resource, uint64_t physical) {
  if (!ready_ || physical < host_window_.address || physical - host_window_.address >= host_window_.length) return false;
  ResourceUnmapBlob unmap{header(kCmdResourceUnmapBlob), resource, 0};
  if (execute(as_bytes(unmap), ByteView(), true, context) != kRespOkNoData) return false;
  window_lock_.lock();
  const bool released = window_.release(physical - host_window_.address);
  window_lock_.unlock();
  return released;
}

bool VirtioGpu::unref_resource(uint32_t resource) {
  ResourceUnref unref{header(kCmdResourceUnref), resource, 0};
  return command(as_bytes(unref));
}

bool VirtioGpu::attach_resource(uint32_t context, uint32_t resource) {
  CtxResource attach{header(kCmdCtxAttachResource), resource, 0};
  return execute(as_bytes(attach), ByteView(), true, context) == kRespOkNoData;
}

bool VirtioGpu::detach_resource(uint32_t context, uint32_t resource) {
  CtxResource detach{header(kCmdCtxDetachResource), resource, 0};
  return execute(as_bytes(detach), ByteView(), true, context) == kRespOkNoData;
}

uint32_t VirtioGpu::create_resource_3d(uint32_t context, const ResourceCreate3d& description, ArrayView<const MemEntry> pages) {
  if (!ready_ || !has_3d_ || pages.size() > (kRequestBytes - 32) / sizeof(MemEntry)) return 0;
  const uint32_t id = allocate_resource_id();
  ResourceCreate3d create = description;
  create.header = header(kCmdResourceCreate3d);
  create.resource_id = id;
  if (execute(as_bytes(create), ByteView(), true, context) != kRespOkNoData) return 0;
  // (the backing: RESOURCE_ATTACH_BACKING with every run, then the resource is the context's)
  struct AttachHead {
    ControlHeader header;
    uint32_t resource_id, entry_count;
  } head{header(kCmdResourceAttachBacking), id, static_cast<uint32_t>(pages.size())};
  ByteBuffer request(*allocator_);
  bool ok = request.append(as_bytes(head));
  for (const MemEntry& entry : pages) ok = ok && request.append(as_bytes(entry));
  if (!ok || execute(request.view()) != kRespOkNoData || !attach_resource(context, id)) {
    unref_resource(id);
    return 0;
  }
  return id;
}

uint64_t VirtioGpu::transfer_3d(uint32_t context, uint32_t resource, bool to_host, const Box& box, uint32_t level, uint64_t offset, uint32_t stride, uint32_t layer_stride) {
  if (!ready_ || !has_3d_) return 0;
  TransferHost3d transfer{};
  transfer.header = header(to_host ? kCmdTransferToHost3d : kCmdTransferFromHost3d);
  transfer.box = box;
  transfer.offset = offset;
  transfer.resource_id = resource;
  transfer.level = level;
  transfer.stride = stride;
  transfer.layer_stride = layer_stride;
  // Fenced and waited for here: a transfer is short, and its fence is what tells the guest the bytes have arrived (or left).
  if (execute(as_bytes(transfer), ByteView(), true, context) != kRespOkNoData) return 0;
  return __atomic_load_n(&fence_, __ATOMIC_ACQUIRE);
}

uint32_t VirtioGpu::submit(uint32_t context, ConstByteView commands, bool fence, uint32_t ring) {
  if (!ready_ || context == 0 || context == kContextId || commands.size() > max_submit_bytes() || (commands.size() & 3)) return 0;
  CmdSubmit head{};
  head.header = header(kCmdSubmit3d);
  head.size = static_cast<uint32_t>(commands.size());
  if (ring != kNoRing) {
    if (ring > 63 || !has_context_init()) return 0;
    head.header.flags |= kFlagInfoRingIndex;
    head.header.ring_index = static_cast<uint8_t>(ring);
  }
  ByteBuffer request(*allocator_);
  if (!request.append(as_bytes(head)) || !request.append(commands)) return 0;
  return execute(request.view(), ByteView(), fence, context);
}

bool VirtioGpu::setup_3d() {
  // (the second virgl set when the host has it - its version 2 says more -, else the first)
  const CapsetInfo* chosen = nullptr;
  for (uint32_t i = 0; i < capset_count_; ++i)
    if ((capset_infos_[i].id == kCapsetVirgl && !chosen) || capset_infos_[i].id == kCapsetVirgl2) chosen = &capset_infos_[i];
  const bool virgl = chosen != nullptr;
  if (chosen) {
    const CapsetInfo& info = *chosen;
    {
      {
        // (version 1 of either set starts with virgl_caps_v1: max_version, four 64-byte format masks - sampler, render, depth/stencil, vertex buffer -, then bset and glsl_level;
        // version 2 of the second set is virgl_caps_v2, which goes on to max_texture_2d_size at byte 484)
        const uint32_t version = info.id == kCapsetVirgl2 && info.max_version >= 2 ? 2 : 1;
        GetCapset request{header(kCmdGetCapset), info.id, version};
        struct {
          ControlHeader header;
          uint8_t data[1024];
        } caps{};
        if (execute(as_bytes(request), ByteView(reinterpret_cast<uint8_t*>(&caps), sizeof(caps))) == kRespOkCapset) {
          ByteReader in{ConstByteView(caps.data, sizeof(caps.data))};
          in.skip(4);
          for (uint32_t& w : virgl_sampler_mask_) w = in.le32();
          for (uint32_t& w : virgl_render_mask_) w = in.le32();
          in.skip(2 * 64);
          virgl_bset_ = in.le32();
          virgl_glsl_level_ = in.le32();
          in.skip(2 * 4);   // (max_texture_array_layers, max_streamout_buffers)
          virgl_max_dual_source_ = in.le32();
          in.skip(4);       // (max_render_targets)
          virgl_max_samples_ = in.le32();
          in.skip(3 * 4);   // (prim_mask, max_tbo_size, max_uniform_blocks)
          virgl_max_viewports_ = in.le32();
          if (version == 2) {
            ByteReader v2{ConstByteView(caps.data, sizeof(caps.data))};
            v2.skip(484);
            virgl_max_texture_2d_ = v2.le32();
            if (!v2.ok()) virgl_max_texture_2d_ = 0;
          }
          if (!in.ok()) {
            virgl_bset_ = virgl_glsl_level_ = virgl_max_samples_ = virgl_max_texture_2d_ = virgl_max_dual_source_ = virgl_max_viewports_ = 0;
            for (uint32_t& w : virgl_sampler_mask_) w = 0;
            for (uint32_t& w : virgl_render_mask_) w = 0;
          }
        }
      }
    }
  }
  if (!virgl) return false;
  CtxCreate ctx{};
  ctx.header = header(kCmdCtxCreate);
  ctx.name_length = 5;
  __builtin_memcpy(ctx.debug_name, "tcos", 5);
  return command(as_bytes(ctx));
}

void VirtioGpu::release_screen() {
  if (screen_resource_) {
    ResourceUnref unref{header(kCmdResourceUnref), screen_resource_, 0};
    command(as_bytes(unref));
    screen_resource_ = 0;
  }
  if (!backing_.empty()) release_view(*allocator_, backing_);
}

bool VirtioGpu::create_screen(uint32_t width, uint32_t height) {
  ByteView memory = allocate_dma_view<uint8_t>(*allocator_, uint64_t{width} * height * 4, 4096);
  if (memory.empty()) return false;
  memory.fill(0);
  const uint32_t id = allocate_resource_id();
  ResourceCreate2d create{header(kCmdResourceCreate2d), id, kFormatB8G8R8X8Unorm, width, height};
  ResourceAttachBacking attach{header(kCmdResourceAttachBacking), id, 1,
                               {reinterpret_cast<uint64_t>(memory.data()), static_cast<uint32_t>(memory.size()), 0}};
  if (!command(as_bytes(create)) || !command(as_bytes(attach)) || !show_resource(id, width, height)) {
    ResourceUnref unref{header(kCmdResourceUnref), id, 0};
    command(as_bytes(unref));
    release_view(*allocator_, memory);
    return false;
  }
  const uint32_t old = screen_resource_;
  ByteView old_memory = backing_;
  screen_resource_ = id;
  backing_ = memory;
  if (old) {  // the scanout already points at the new resource
    ResourceUnref unref{header(kCmdResourceUnref), old, 0};
    command(as_bytes(unref));
    release_view(*allocator_, old_memory);
  }
  set_framebuffer(backing_, width, height, width * 4, true);
  mark_all_dirty();
  return true;
}

// Points the scanouts at resource `id`: each monitor of the layout at its rectangle of it; scanouts that had a resource and are not in the layout
// are switched off.
bool VirtioGpu::show_layout(uint32_t id) {
  uint32_t shown = 0;
  for (uint32_t i = 0; i < layout_count_; ++i) {
    const ScanoutPlacement& p = layout_[i];
    SetScanout scanout{header(kCmdSetScanout), {p.x, p.y, p.width, p.height}, p.connector, id};
    if (!command(as_bytes(scanout))) return false;
    shown |= 1u << p.connector;
  }
  for (uint32_t s = 0; s < IMonitorSource::kMaxConnectors; ++s)
    if ((used_scanouts_ & ~shown) & (1u << s)) {
      SetScanout off{header(kCmdSetScanout), {0, 0, 0, 0}, s, 0};
      command(as_bytes(off));
    }
  used_scanouts_ = shown;
  return true;
}

bool VirtioGpu::show_resource(uint32_t id, uint32_t width, uint32_t height) {
  if (layout_count_ > 0) {
    if (!show_layout(id)) return false;
  } else {
    SetScanout scanout{header(kCmdSetScanout), {0, 0, width, height}, 0, id};
    if (!command(as_bytes(scanout))) return false;
    used_scanouts_ = 1;
  }
  shown_resource_ = id;
  ResourceFlush flush{header(kCmdResourceFlush), {0, 0, width, height}, id, 0};
  return command(as_bytes(flush));
}

uint32_t VirtioGpu::mode_count() const { return kModeCount; }

bool VirtioGpu::mode_info(uint32_t index, DisplayMode* out) const {
  if (index >= kModeCount || !out) return false;
  *out = kModes[index];
  return true;
}

bool VirtioGpu::set_mode(uint32_t index) {
  if (!ready_ || index >= kModeCount) return false;
  const uint32_t old_layout = layout_count_;
  layout_count_ = 0;   // a mode is a screen for one display
  if (!create_screen(kModes[index].width, kModes[index].height)) {
    layout_count_ = old_layout;
    return false;
  }
  current_ = index;
  return true;
}

// --- Monitors ---------------------------------------------------------------------------------------

bool VirtioGpu::read_display_info() {
  if (!ready_) return false;
  GetDisplayInfo request{header(kCmdGetDisplayInfo)};
  RespDisplayInfo info{};
  if (execute(as_bytes(request), ByteView(reinterpret_cast<uint8_t*>(&info), sizeof(info))) != kRespOkDisplayInfo) return false;
  for (uint32_t i = 0; i < IMonitorSource::kMaxConnectors; ++i) {
    outputs_[i].enabled = info.modes[i].enabled != 0;
    outputs_[i].width = info.modes[i].rect.width;
    outputs_[i].height = info.modes[i].rect.height;
  }
  return true;
}

uint32_t VirtioGpu::connector_count() const { return output_count_; }

bool VirtioGpu::connector_info(uint32_t connector, ConnectorInfo* out) const {
  if (connector >= output_count_ || !out) return false;
  *out = ConnectorInfo();
  const Output& o = outputs_[connector];
  out->connected = o.enabled || (connector == 0 && !o.width);   // (the first one is there even before the host said anything)
  out->type = ConnectorType::Virtual;
  const char prefix[] = "Virtual-";
  uint32_t n = 0;
  for (; prefix[n]; ++n) out->name[n] = prefix[n];
  if (connector >= 10) out->name[n++] = static_cast<char>('0' + connector / 10);
  out->name[n++] = static_cast<char>('0' + connector % 10);
  out->name[n] = 0;
  out->preferred_width = o.width;
  out->preferred_height = o.height;
  return true;
}

uint32_t VirtioGpu::read_edid(uint32_t connector, ByteView out) {
  if (!ready_ || connector >= output_count_ || !transport_ || !(transport_->accepted_features() & kFeatureEdid)) return 0;
  GetEdid request{header(kCmdGetEdid), connector, 0};
  RespEdid response{};
  if (execute(as_bytes(request), ByteView(reinterpret_cast<uint8_t*>(&response), sizeof(response))) != kRespOkEdid) return 0;
  uint32_t n = response.size < sizeof response.edid ? response.size : sizeof response.edid;
  if (n > out.size()) n = static_cast<uint32_t>(out.size());
  for (uint32_t i = 0; i < n; ++i) out[i] = response.edid[i];
  return n;
}

// A virtual display takes any size the guest memory can hold (the host window follows the scanout).
bool VirtioGpu::mode_available(uint32_t connector, uint32_t width, uint32_t height) const {
  return connector < output_count_ && width >= 320 && height >= 200 && width <= 8192 && height <= 8192;
}

// The sizes the driver offers by itself, and the size the host window has.
uint32_t VirtioGpu::device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const {
  if (connector >= output_count_) return 0;
  uint32_t n = 0;
  auto add = [&](uint32_t w, uint32_t h) {
    for (uint32_t k = 0; k < n; ++k)
      if (out[k].width == w && out[k].height == h) return;
    if (n >= capacity || w == 0 || h == 0) return;
    out[n] = MonitorMode();
    out[n].width = w;
    out[n].height = h;
    ++n;
  };
  add(outputs_[connector].width, outputs_[connector].height);
  for (const DisplayMode& m : kModes) add(m.width, m.height);
  return n;
}

bool VirtioGpu::refresh_connectors() {
  Output before[IMonitorSource::kMaxConnectors];
  for (uint32_t i = 0; i < IMonitorSource::kMaxConnectors; ++i) before[i] = outputs_[i];
  if (!read_display_info()) return false;
  for (uint32_t i = 0; i < output_count_; ++i)
    if (before[i].enabled != outputs_[i].enabled) return true;   // (a size change of the host window is not a change of monitors)
  return false;
}

// Replaces the screen by one of the desktop's size and shows on each monitor its rectangle of it.
bool VirtioGpu::apply_layout(const ScanoutPlacement* placements, uint32_t count, uint32_t desktop_width, uint32_t desktop_height) {
  if (!ready_ || !placements || count == 0 || count > IMonitorSource::kMaxConnectors) return false;
  if (desktop_width == 0 || desktop_height == 0 || desktop_width > 16384 || desktop_height > 16384 || uint64_t{desktop_width} * desktop_height > (64u << 20)) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const ScanoutPlacement& p = placements[i];
    if (p.connector >= output_count_ || p.width == 0 || p.height == 0 || uint64_t{p.x} + p.width > desktop_width || uint64_t{p.y} + p.height > desktop_height) return false;
  }
  ScanoutPlacement old[IMonitorSource::kMaxConnectors];
  const uint32_t old_count = layout_count_;
  for (uint32_t i = 0; i < old_count; ++i) old[i] = layout_[i];
  for (uint32_t i = 0; i < count; ++i) layout_[i] = placements[i];
  layout_count_ = count;
  bool ok;
  if (desktop_width == width() && desktop_height == height() && screen_resource_) {   // the same screen, shared out differently
    ok = show_layout(screen_resource_);
    if (ok) {
      ResourceFlush flush{header(kCmdResourceFlush), {0, 0, desktop_width, desktop_height}, screen_resource_, 0};
      command(as_bytes(flush));
    }
  } else {
    ok = create_screen(desktop_width, desktop_height);
  }
  if (!ok) {
    for (uint32_t i = 0; i < old_count; ++i) layout_[i] = old[i];
    layout_count_ = old_count;
  }
  return ok;
}

void VirtioGpu::present() {
  if (!ready_ || !screen_resource_) return;
  uint32_t x, y, w, h;
  const bool dirty = take_dirty(&x, &y, &w, &h);
  if (shown_resource_ != screen_resource_) {  // a 3D image had the screen: this 2D drawing takes it back
    mark_all_dirty();
    take_dirty(&x, &y, &w, &h);
    show_resource(screen_resource_, width(), height());
  } else if (!dirty) {
    return;
  }
  TransferToHost2d transfer{header(kCmdTransferToHost2d), {x, y, w, h}, uint64_t{y} * pitch_bytes() + uint64_t{x} * 4,
                            screen_resource_, 0};
  ResourceFlush flush{header(kCmdResourceFlush), {x, y, w, h}, screen_resource_, 0};
  command(as_bytes(transfer));
  command(as_bytes(flush));
}

// A stopped thread may hold the lock, and is often waiting for the host's answer to a command with it. That answer is awaited here, so that the buffers are ours again.
void VirtioGpu::take_over_for_panic() {
  alone_ = true;
  if (!ready_) return;
  if (control_.unanswered()) transport_->notify(0);   // (the thread may have stopped between posting the command and telling the device)
  const uint64_t start = uptime_us();
  uint16_t index;
  uint32_t length;
  while (control_.unanswered() && uptime_us() - start < kPanicAnswerWaitUs) {
    if (!control_.take_used(&index, &length)) delay_spin_us(20);
  }
  in_flight_ = control_.unanswered();
}

void VirtioGpu::shutdown() {
  if (interrupts_) drop_interrupt_handler();
  interrupts_ = false;
  if (ready_) {
    ready_ = false;
    if (has_3d_) {
      // Unclaimed context: the reset below discards it with everything else.
      has_3d_ = false;
    }
  }
  if (transport_) {
    transport_->shutdown();  // reset: the host stops using our memory
    transport_ = nullptr;
  }
  if (allocator_) {
    ready_ = false;
    if (!backing_.empty()) release_view(*allocator_, backing_);
    release_view(*allocator_, request_);
    release_view(*allocator_, response_);
    for (AsyncSlot& slot : async_) {   // (the device was reset: nothing of ours is in use any more)
      if (!slot.buffer.empty()) release_view(*allocator_, slot.buffer);
      slot.busy = false;
    }
    control_.release(*allocator_);
  }
  screen_resource_ = 0;
}

bool VirtioGpu::video_memory(uint64_t* total_bytes, uint64_t* used_bytes) const {
  uint64_t screen = 0;
  if (!FramebufferGpu::video_memory(nullptr, &screen)) return false;
  if (total_bytes) *total_bytes = 0;
  if (used_bytes) *used_bytes = screen + __atomic_load_n(&resource_bytes_, __ATOMIC_RELAXED);
  return true;
}

}  // namespace hardware
