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
#include <new>
#include "Kernel/Delay.hpp"
#include "console.hpp"
#include "Utility/AllocatedView.hpp"

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

uint32_t VirtioGpu::execute(ConstByteView request, ByteView response) {
  if (!ready_ || request.size() < sizeof(ControlHeader) || request.size() > request_.size()) return 0;
  kernel::ScopedMutex guard(lock_);
  uint16_t index;
  uint32_t length;
  if (in_flight_) {  // an earlier command never got its answer: the buffers still belong to the device
    if (!control_.take_used(&index, &length)) return 0;
    in_flight_ = false;
  }
  request_.copy_from(request);
  auto* head = reinterpret_cast<ControlHeader*>(request_.data());
  if (head->type >= kCmdCtxCreate) {
    head->context_id = kContextId;
    // Wait for the host to finish the work before answering: a later readback or flush
    // then sees its results, and our buffers are free to reuse.
    head->flags |= kFlagFence;
    head->fence_id = ++fence_;
  }
  auto* reply = reinterpret_cast<ControlHeader*>(response_.data());
  reply->type = 0;

  const Virtqueue::Segment segments[2] = {{request_.data(), static_cast<uint32_t>(request.size()), false},
                                          {response_.data(), static_cast<uint32_t>(response_.size()), true}};
  control_.post_chain(0, segments, 2);
  transport_->notify(0);

  for (uint64_t waited = 0; !control_.take_used(&index, &length); waited += 100) {
    if (waited >= kCommandTimeoutUs) {
      in_flight_ = true;
      if (__atomic_fetch_add(&timeouts_, 1, __ATOMIC_ACQ_REL) % 16 == 0)
        console::debug_puts("virtio-gpu: the host is not answering (display hidden or busy?); skipping frames\n");
      return 0;
    }
    delay_us(100);  // sleeps the thread where that is allowed instead of spinning with the core
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const uint32_t type = reply->type;
  if (!response.empty()) response.copy_from(ConstByteView(response_).at_most(response.size()));
  return type;
}

bool VirtioGpu::initialize() {
  if (!pci_ || !allocator_ || device_.vendor_id != kPciVendor || device_.device_id != kPciDeviceId) return false;
  transport_ = new (transport_storage_) VirtioPciDevice(*pci_, device_, *allocator_, st_);
  transport_->request_features(kFeatureVirgl);
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

  if (!set_mode(kDefaultMode)) {
    shutdown();
    return false;
  }
  if (transport_->accepted_features() & kFeatureVirgl) has_3d_ = setup_3d();
  return true;
}

bool VirtioGpu::setup_3d() {
  uint32_t capsets = 0;
  for (int i = 0; i < 4; ++i) capsets |= uint32_t{transport_->read_config8(12 + i)} << (8 * i);
  bool virgl = false;
  for (uint32_t i = 0; i < capsets && i < 16; ++i) {
    GetCapsetInfo query{header(kCmdGetCapsetInfo), i, 0};
    RespCapsetInfo info{};
    if (execute(as_bytes(query), ByteView(reinterpret_cast<uint8_t*>(&info), sizeof(info))) != kRespOkCapsetInfo)
      continue;
    if (info.capset_id == kCapsetVirgl || info.capset_id == kCapsetVirgl2) virgl = true;
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

bool VirtioGpu::show_resource(uint32_t id, uint32_t width, uint32_t height) {
  SetScanout scanout{header(kCmdSetScanout), {0, 0, width, height}, 0, id};
  if (!command(as_bytes(scanout))) return false;
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
  if (!create_screen(kModes[index].width, kModes[index].height)) return false;
  current_ = index;
  return true;
}

void VirtioGpu::present() {
  if (!ready_ || !screen_resource_) return;
  uint32_t x, y, w, h;
  const bool dirty = take_dirty(&x, &y, &w, &h);
  if (shown_resource_ != screen_resource_) {  // a 3D image had the screen: bring the console back
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

void VirtioGpu::shutdown() {
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
    control_.release(*allocator_);
  }
  screen_resource_ = 0;
}

}  // namespace hardware
