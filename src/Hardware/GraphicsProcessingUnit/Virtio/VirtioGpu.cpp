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
  // A caller with interrupts masked (the console draws under its spinlock) must not block on the mutex while another thread has a command out: it looks
  // for the mutex again instead. (Blocking there is a kernel panic.)
  if (kernel::irq_enabled() || !kernel::running()) {
    lock_.lock();
  } else {
    while (!lock_.try_lock()) delay_spin_us(20);
  }
  struct Release {
    kernel::Mutex& mutex;
    ~Release() { mutex.unlock(); }
  } guard{lock_};
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

  // A quick answer (a copy the host already finished) comes within microseconds: look for it by spinning. A slow one (the host drawing a frame, or holding a
  // flush for its display's refresh) takes milliseconds, and spinning that long is a core at 100% for nothing: after the first look the thread sleeps
  // (delay_us sleeps from 200 us on, where that is allowed) and checks again.
  const uint64_t start = uptime_us();
  while (!control_.take_used(&index, &length)) {
    const uint64_t waited = uptime_us() - start;
    if (waited >= kCommandTimeoutUs) {
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
  if (!response.empty()) response.copy_from(ConstByteView(response_).at_most(response.size()));
  return type;
}

kernel::Frame* VirtioGpu::on_interrupt(kernel::Frame* frame, void* self) {
  VirtioGpu* gpu = static_cast<VirtioGpu*>(self);
  kernel::apic::eoi();
  __atomic_fetch_add(&gpu->interrupt_count_, 1, __ATOMIC_ACQ_REL);
  gpu->completion_.set();
  return frame;
}

bool VirtioGpu::enable_interrupts() {
  if (interrupts_) return true;
  if (!ready_ || !transport_ || !kernel::running()) return false;
  kernel::set_interrupt_handler(kernel::kVectorVirtioGpu, &VirtioGpu::on_interrupt, this);
  if (!transport_->enable_msix(kernel::kVectorVirtioGpu, kernel::apic::this_apic_id(), 0)) return false;
  // Try it out: one command that needs an answer; the interrupt for it must arrive. Without it the device or the machine does not deliver (a message
  // address the interrupt controller ignores, ...) and polling stays.
  completion_.reset();
  const uint32_t before = interrupt_count();
  read_display_info();
  for (int i = 0; i < 50 && interrupt_count() == before; ++i) delay_us(100);
  if (interrupt_count() == before) {
    transport_->disable_msix();
    return false;
  }
  interrupts_ = true;
  return true;
}

void VirtioGpu::disable_interrupts() {
  if (!interrupts_) return;
  interrupts_ = false;                 // commands are waited for by polling from here on
  if (transport_) transport_->disable_msix();
  kernel::set_interrupt_handler(kernel::kVectorVirtioGpu, nullptr, nullptr);
}

bool VirtioGpu::initialize() {
  if (!pci_ || !allocator_ || device_.vendor_id != kPciVendor || device_.device_id != kPciDeviceId) return false;
  transport_ = new (transport_storage_) VirtioPciDevice(*pci_, device_, *allocator_, st_);
  transport_->request_features(kFeatureVirgl | kFeatureEdid);
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
    if (info.capset_id == kCapsetVirgl || info.capset_id == kCapsetVirgl2) {
      virgl = true;
      if (!virgl_bset_) {
        // (version 1 of either set starts with virgl_caps_v1: max_version, four 64-byte format masks, then bset and glsl_level)
        GetCapset request{header(kCmdGetCapset), info.capset_id, 1};
        RespCapset caps{};
        if (execute(as_bytes(request), ByteView(reinterpret_cast<uint8_t*>(&caps), sizeof(caps))) == kRespOkCapset) {
          ByteReader in{ConstByteView(caps.data, sizeof(caps.data))};
          in.skip(4 + 4 * 64);
          virgl_bset_ = in.le32();
          virgl_glsl_level_ = in.le32();
          in.skip(4 * 4);   // (max_texture_array_layers, max_streamout_buffers, max_dual_source_render_targets, max_render_targets)
          virgl_max_samples_ = in.le32();
          if (!in.ok()) virgl_bset_ = virgl_glsl_level_ = virgl_max_samples_ = 0;
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
  if (interrupts_) kernel::set_interrupt_handler(kernel::kVectorVirtioGpu, nullptr, nullptr);
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
