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

// Follows the i915 and xe drivers (Copyright Intel Corporation; MIT license), Linux v6.12: gt/intel_ggtt.c (ggtt_probe_common, gen8_ggtt_pte_encode),
// xe_device.c (the LMEM_INIT wait), display/intel_display.c (the commit order), with the parts named in IntelDg2Display.hpp.
#include "Hardware/GraphicsProcessingUnit/Intel/IntelSecondaryGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdMonitor.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGtt.hpp"
#include "Hardware/Monitor/Edid.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {
namespace intel_secondary {

using intel_dg2::kPorts;
using intel_dg2::PortInfo;

namespace {
bool same_size(const CrtcMode& a, const CrtcMode& b) { return a.h_display == b.h_display && a.v_display == b.v_display; }

bool common_ok(const CrtcMode& m) {
  if (m.interlaced || m.double_scan || m.h_display == 0 || m.v_display == 0) return false;
  if (uint64_t{stride_of(m.h_display)} * m.v_display > kSurfaceBytes) return false;
  return intel_dg2::cdclk_for(m.pixel_clock_khz) != 0;
}

// The one place the local memory window becomes a view of bytes: BAR 2 was mapped for this driver alone, identity mapped, and outlives the view
// (the driver never unmaps it while the display exists).
[[maybe_unused]] ByteView bar_view(const MmioRegion& window) { return ByteView(reinterpret_cast<uint8_t*>(window.address()), window.length()); }
}  // namespace

uint64_t UptimeClock::now_us() const { return uptime_us(); }

uint32_t stride_of(uint32_t width) { return (width * 4 + kStrideAlignBytes - 1) / kStrideAlignBytes * kStrideAlignBytes; }

uint32_t refresh_millihz(const CrtcMode& m) {
  const uint64_t pixels = uint64_t{m.h_total} * m.v_total;
  return pixels ? static_cast<uint32_t>(uint64_t{m.pixel_clock_khz} * 1000000 / pixels) : 0;
}

bool hdmi_mode_ok(const CrtcMode& m) {
  return common_ok(m) && m.pixel_clock_khz <= kMaxHdmiTmdsKhz && intel_dg2::find_hdmi_mpllb(m.pixel_clock_khz) != nullptr;
}

bool dp_mode_ok(const CrtcMode& m, const Dpcd::Capabilities& caps, uint32_t* lanes, uint32_t* link_khz) {
  if (!common_ok(m)) return false;
  const DpLinkConfig link = DpLinkConfig::choose(caps, m.pixel_clock_khz, 24);
  if (link.lanes == 0 || link.rate_index != DpLinkConfig::kNoRateIndex || !intel_dg2::find_dp_mpllb(link.rate_khz)) return false;
  *lanes = link.lanes;
  *link_khz = link.rate_khz;
  return true;
}

uint32_t edid_modes(ConstByteView edid, const Dpcd::Capabilities* caps, ArrayView<Mode> out) {
  uint32_t count = 0;
  for (uint32_t slot = 0; slot < 4 && count < out.size(); ++slot) {
    CrtcMode m;
    if (!edid_detailed_timing(edid, slot, &m)) continue;
    bool seen = false;
    for (uint32_t i = 0; i < count; ++i) seen = seen || same_size(out[i].timing, m);
    if (seen) continue;
    uint32_t lanes = 0, link = 0;
    if (caps ? !dp_mode_ok(m, *caps, &lanes, &link) : !hdmi_mode_ok(m)) continue;
    Mode& o = out[count++];
    o.timing = m;
    o.refresh_millihz = refresh_millihz(m);
    o.preferred = slot == 0;
    o.lanes = lanes;
    o.link_khz = link;
  }
  return count;
}

Optional<uint32_t> choose_mode(ArrayView<const Mode> modes) {
  Optional<uint32_t> best;
  for (uint32_t i = 0; i < modes.size(); ++i)
    if (modes[i].preferred) return Optional<uint32_t>(i);
  for (uint32_t i = 0; i < modes.size(); ++i) {
    if (!best) {
      best = Optional<uint32_t>(i);
      continue;
    }
    const Mode& a = modes[i];
    const Mode& b = modes[*best];
    const uint64_t pa = uint64_t{a.timing.h_display} * a.timing.v_display, pb = uint64_t{b.timing.h_display} * b.timing.v_display;
    if (pa > pb || (pa == pb && a.refresh_millihz > b.refresh_millihz)) best = Optional<uint32_t>(i);
  }
  return best;
}

const char* status_name(Status status) {
  switch (status) {
    case Status::Ok: return "running";
    case Status::NoAccess: return "no access to the card";
    case Status::NoLocalMemory: return "local memory is not initialised (LMEM_INIT clear)";
    case Status::TooLittleMemory: return "the local memory or GGTT window is too small";
    case Status::DisplayInit: return "the display engine did not come up";
    case Status::NoMonitor: return "no monitor on DDI A to D";
    case Status::NoMode: return "no mode of the monitor fits the output";
    case Status::ModeSet: return "the mode set failed";
    case Status::NotRunning: return "not running";
    case Status::BadIndex: return "no such mode";
  }
  return "?";
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The display
// ---------------------------------------------------------------------------------------------------------------------------------------
Display::Display(const Inputs& inputs) : in_(inputs), engine_(*inputs.registers, *inputs.clock) {}

Display::~Display() { shutdown(); }

Status Display::fail(Status status, const char* why) {
  status_ = why;
  return status;
}

void Display::look(Connector& c) {
  const PortInfo& port = *c.port;
  if (!engine_.enable_aux_power(port)) return;
  c.looked = true;
  ByteBuffer edid(*in_.allocator);
  intel_dg2::DpAux aux(*in_.registers, *in_.clock, port);
  Dpcd::Capabilities caps;
  if (Dpcd::read_capabilities(aux, false, &caps)) {
    c.dp = true;
    c.caps = caps;
    if (!i2c_edid::read(aux, &edid)) edid.clear();
  } else {
    engine_.disable_aux_power(port);           // (an HDMI port's DDC is GMBUS, which needs no AUX power)
    intel_dg2::Gmbus ddc(*in_.registers, *in_.clock, port.gmbus_pin);
    if (!i2c_edid::read(ddc, &edid)) edid.clear();
  }
  const ConstByteView bytes = edid.view().at_most(kEdidBytes);
  if (!ByteView(c.edid).first(bytes.size()).copy_from(bytes)) return;
  c.edid_size = static_cast<uint32_t>(bytes.size());
  if (!c.dp && c.edid_size) {
    EdidInfo info;
    c.hdmi = parse_edid(bytes, &info) && info.hdmi;
  }
}

bool Display::map_surface() {
  // gen8_ggtt_insert_entries with the LM bit: each 4 KiB page of the surface's graphics range points at its page of local memory; then the GGTT
  // write-combining buffer flushed (gen8_ggtt_invalidate)
  const uint64_t pages = kSurfaceBytes / intel_gt::kPageSize;
  const intel_gt::Ggtt ggtt(in_.ggtt, kSurfaceGgtt / intel_gt::kPageSize, pages, intel_dg2::kGgttPteLm);
  if (!ggtt.valid()) return false;
  for (uint64_t i = 0; i < pages; ++i)
    if (!ggtt.map(i, kSurfaceLmemOffset + i * intel_gt::kPageSize)) return false;
  intel_gt::ggtt_invalidate(*in_.registers);
  return true;
}

intel_dg2::Stage Display::program(const Mode& mode) {
  const Connector& c = connectors_[driven_];
  intel_dg2::ModeRequest q;
  q.port = c.port;
  q.output = c.dp ? intel_dg2::Output::DisplayPort : c.hdmi ? intel_dg2::Output::Hdmi : intel_dg2::Output::Dvi;
  q.mode = mode.timing;
  q.surface = kSurfaceGgtt;
  q.stride_bytes = stride_of(mode.timing.h_display);
  intel_dg2::DpAux aux(*in_.registers, *in_.clock, *c.port);
  if (c.dp) {
    q.lanes = mode.lanes;
    q.link_khz = mode.link_khz;
    q.aux = &aux;
    q.caps = c.caps;
  }
  // the picture cleared before the plane shows it
  const ByteView surface = in_.lmem.subview(kSurfaceLmemOffset, uint64_t{q.stride_bytes} * mode.timing.v_display);
  surface.fill(0);
  const intel_dg2::Stage stage = engine_.enable(q);
  if (stage == intel_dg2::Stage::Done) {
    pixels_ = surface;
    width_ = mode.timing.h_display;
    height_ = mode.timing.v_display;
    pitch_ = q.stride_bytes;
  }
  return stage;
}

Status Display::start() {
  shutdown();
  if (!in_.allocator) return fail(Status::NoAccess, "no allocator");
  if (in_.lmem.size() < kLmemViewBytes || in_.ggtt.length() < (kSurfaceGgtt + kSurfaceBytes) / intel_gt::kPageSize * 8)
    return fail(Status::TooLittleMemory, status_name(Status::TooLittleMemory));
  const uint32_t gu = in_.registers->read32(intel_dg2::kGuCntl);
  if (gu == 0xFFFFFFFFu || !(gu & intel_dg2::kLmemInit)) return fail(Status::NoLocalMemory, status_name(Status::NoLocalMemory));
  lmem_bytes_ = IntelGpuProbe::tile_bytes(in_.registers->read32(IntelGpuProbe::kTileAddrRange0));
  if (!engine_.init()) return fail(Status::DisplayInit, engine_.why());

  connector_count_ = 0;
  for (const PortInfo& p : kPorts) {
    Connector& c = connectors_[connector_count_++];
    c = Connector();
    c.port = &p;
    if (p.drivable) look(c);
  }
  bool any_edid = false;
  for (uint32_t i = 0; i < connector_count_ && driven_ < 0; ++i) {
    const Connector& c = connectors_[i];
    if (!c.edid_size) continue;
    any_edid = true;
    const uint32_t n = edid_modes(ConstByteView(c.edid).first(c.edid_size), c.dp ? &c.caps : nullptr, ArrayView<Mode>(modes_));
    if (n) {
      mode_count_ = n;
      driven_ = static_cast<int32_t>(i);
    }
  }
  if (driven_ < 0) return fail(any_edid ? Status::NoMode : Status::NoMonitor, status_name(any_edid ? Status::NoMode : Status::NoMonitor));
  // (the AUX wells of the DisplayPort sinks not driven are left on: they are where the next look goes)
  if (!map_surface()) {
    driven_ = -1;
    return fail(Status::TooLittleMemory, "the GGTT window does not hold the surface's range");
  }
  mapped_ = true;
  const Optional<uint32_t> first = choose_mode(ArrayView<const Mode>(modes_, mode_count_));
  current_ = first ? *first : 0;
  stage_ = program(modes_[current_]);
  if (stage_ != intel_dg2::Stage::Done) {
    driven_ = -1;
    mode_count_ = 0;
    return fail(Status::ModeSet, intel_dg2::stage_name(stage_));
  }
  running_ = true;
  status_ = status_name(Status::Ok);
  return Status::Ok;
}

Status Display::set_mode(uint32_t index) {
  if (!running_) return Status::NotRunning;
  if (index >= mode_count_) return Status::BadIndex;
  if (index == current_) return Status::Ok;
  engine_.disable();
  pixels_ = ByteView();
  stage_ = program(modes_[index]);
  if (stage_ == intel_dg2::Stage::Done) {
    current_ = index;
    return Status::Ok;
  }
  // the old mode again
  if (program(modes_[current_]) != intel_dg2::Stage::Done) {
    running_ = false;
    status_ = "the mode set failed, and the old mode could not be set again";
  }
  return Status::ModeSet;
}

void Display::shutdown() {
  engine_.disable();
  running_ = false;
  pixels_ = ByteView();
  width_ = height_ = pitch_ = 0;
  driven_ = -1;
  mode_count_ = 0;
}

}  // namespace intel_secondary

// ---------------------------------------------------------------------------------------------------------------------------------------
// IntelSecondaryGpu
// ---------------------------------------------------------------------------------------------------------------------------------------
bool IntelSecondaryGpu::supports(const PciDeviceInfo& device) {
  return device.vendor_id == kPciVendor && device.class_code == kPciClassDisplay && IntelGpuProbe::platform_of(device.device_id) != IntelGpuProbe::Platform::None;
}

IntelSecondaryGpu::IntelSecondaryGpu(const GpuContext& context) : context_(context) {
  const IntelGpuInfo info = intel_gpu_identify(context.device.device_id);
  const ArrayView<char> out(name_);
  uint32_t n = 0;
  for (const char* p = info.name; p && *p && n + 1 < out.size(); ++p) out[n++] = *p;
  out[n] = 0;
}

IntelSecondaryGpu::~IntelSecondaryGpu() { shutdown(); }

bool IntelSecondaryGpu::fail(const char* why) {
  status_ = why;
  display_ = UniquePointer<intel_secondary::Display>();
  registers_ = UniquePointer<intel_gt::MmioRegisterBank>();
  ordered_ = 0;
  set_framebuffer(ByteView(), 0, 0, 0, true);
  return false;
}

bool IntelSecondaryGpu::refuse_platform(uint16_t device_id) {
  switch (IntelGpuProbe::platform_of(device_id)) {
    case IntelGpuProbe::Platform::Dg2: return false;
    case IntelGpuProbe::Platform::Dg1: return !fail("DG1 (display version 12: combo and Dekel PHYs) is not driven yet");
    case IntelGpuProbe::Platform::Battlemage: return !fail("Battlemage (display version 14: C10/C20 PHYs) is not driven yet");
    default: return !fail("not a discrete Intel GPU");
  }
}

bool IntelSecondaryGpu::start_display(const intel_secondary::Display::Inputs& inputs) {
  if (!context_.allocator) return fail("no allocator");
  if (!inputs.registers || !inputs.clock) return fail("no registers or clock");
  intel_secondary::Display::Inputs in = inputs;
  if (!in.allocator) in.allocator = context_.allocator;
  display_ = UniquePointer<intel_secondary::Display>::make(*context_.allocator, in);
  if (!display_) return fail("out of memory");
  if (display_->start() != intel_secondary::Status::Ok) return fail(display_->status());
  set_video_memory_total(display_->local_memory_bytes());
  order();
  show();
  status_ = "running";
  return true;
}

bool IntelSecondaryGpu::initialize_with(const intel_secondary::Display::Inputs& inputs, uint16_t device_id) {
#ifndef TCOS_INTEL_DGPU_NATIVE
  (void)inputs;
  (void)device_id;
  return fail("built without TCOS_INTEL_DGPU_NATIVE: the display of a secondary Intel GPU is not driven");
#else
  if (refuse_platform(device_id)) return false;
  return start_display(inputs);
#endif
}

bool IntelSecondaryGpu::initialize() {
#ifndef TCOS_INTEL_DGPU_NATIVE
  // (Nothing is read or written: the pool's probe does the read-only part.)
  return fail("built without TCOS_INTEL_DGPU_NATIVE: the display of a secondary Intel GPU is not driven");
#else
  if (refuse_platform(context_.device.device_id)) return false;
  if (!context_.allocator || !context_.registers || !context_.pci) return fail("no allocator, register mapper or PCI bus");
  allocators::IAllocator& allocator = *context_.allocator;
  const IPciBus& pci = *context_.pci;
  const PciAddress address = context_.device.address;
  constexpr uint16_t kPciCommand = 0x04;
  constexpr uint32_t kMemorySpace = 1u << 1;
  const uint32_t command = pci.read32(address, kPciCommand) & 0xFFFF;     // (not the status bits above, which clear when written)
  if (!(command & kMemorySpace)) pci.write32(address, kPciCommand, command | kMemorySpace);
  const uint64_t bar0 = pci.memory_bar(address, 0), bar2 = pci.memory_bar(address, 2);
  MmioRegion register_window, ggtt_window, lmem_window;
  if (!bar0 || !context_.registers->map_registers(bar0, intel_secondary::kRegisterBytes, &register_window)) return fail("BAR 0 not mapped");
  if (!context_.registers->map_registers(bar0 + intel_secondary::kGgttWindowOffset, intel_secondary::kGgttWindowBytes, &ggtt_window))
    return fail("the GGTT window of BAR 0 not mapped");
  if (!bar2 || !context_.registers->map_registers(bar2, intel_secondary::kLmemViewBytes, &lmem_window) || lmem_window.length() < intel_secondary::kLmemViewBytes)
    return fail("BAR 2 (local memory) not mapped");
  registers_ = UniquePointer<intel_gt::MmioRegisterBank>::make(allocator, register_window);
  if (!registers_) return fail("out of memory");
  intel_secondary::Display::Inputs in;
  in.registers = registers_.get();
  in.clock = &clock_;
  in.allocator = &allocator;
  in.ggtt = ggtt_window;
  in.lmem = intel_secondary::bar_view(lmem_window);
  return start_display(in);
#endif
}

void IntelSecondaryGpu::shutdown() {
  if (display_) display_->shutdown();
  display_ = UniquePointer<intel_secondary::Display>();
  registers_ = UniquePointer<intel_gt::MmioRegisterBank>();
  ordered_ = 0;
  set_framebuffer(ByteView(), 0, 0, 0, true);
}

void IntelSecondaryGpu::show() {
  if (display_ && display_->running()) set_framebuffer(display_->pixels(), display_->width(), display_->height(), display_->pitch_bytes(), true);
  else set_framebuffer(ByteView(), 0, 0, 0, true);
}

uint32_t IntelSecondaryGpu::mode_count() const { return display_ && display_->running() ? display_->mode_count() : 0; }

bool IntelSecondaryGpu::mode_info(uint32_t index, DisplayMode* out) const {
  if (!out || index >= mode_count()) return false;
  const intel_secondary::Mode* m = display_->mode(index);
  out->width = m->timing.h_display;
  out->height = m->timing.v_display;
  return true;
}

uint32_t IntelSecondaryGpu::current_mode() const { return display_ ? display_->current_mode() : 0; }

bool IntelSecondaryGpu::set_mode(uint32_t index) {
  if (!display_ || !display_->running()) return false;
  const bool ok = display_->set_mode(index) == intel_secondary::Status::Ok;
  show();
  return ok;
}

bool IntelSecondaryGpu::pci_location(PciAddress* out) const {
  if (!out) return false;
  *out = context_.device.address;
  return true;
}

// ------------------------------------------------------------------ the monitors

void IntelSecondaryGpu::order() {
  ordered_ = 0;
  if (!display_) return;
  const ArrayView<const intel_secondary::Connector> list = display_->connectors();
  const ArrayView<uint8_t> out(order_);
  const int32_t shown = display_->connector();
  if (shown >= 0) out[ordered_++] = static_cast<uint8_t>(shown);
  for (uint32_t pass = 0; pass < 2; ++pass)
    for (uint32_t i = 0; i < list.size() && ordered_ < out.size(); ++i) {
      if (static_cast<int32_t>(i) == shown || !list[i].port->drivable) continue;
      const bool has_monitor = list[i].edid_size != 0;
      if (has_monitor == (pass == 0)) out[ordered_++] = static_cast<uint8_t>(i);
    }
}

bool IntelSecondaryGpu::driven(uint32_t connector) const {
  return display_ && display_->running() && connector < ordered_ && static_cast<int32_t>(order_[connector]) == display_->connector();
}

bool IntelSecondaryGpu::connector_info(uint32_t connector, ConnectorInfo* out) const {
  if (!out || !display_ || connector >= ordered_) return false;
  const intel_secondary::Connector& c = display_->connectors()[order_[connector]];
  *out = ConnectorInfo();
  out->connected = c.edid_size != 0;
  out->type = c.dp ? ConnectorType::DisplayPort : c.hdmi ? ConnectorType::Hdmi : c.edid_size ? ConnectorType::Dvi : ConnectorType::Unknown;
  const ArrayView<char> name(out->name);
  uint32_t n = 0;
  for (const char* p = c.port->name; *p && n + 1 < name.size(); ++p) name[n++] = *p;
  name[n] = 0;
  if (driven(connector)) {
    for (uint32_t i = 0; i < display_->mode_count(); ++i)
      if (display_->mode(i)->preferred) {
        out->preferred_width = display_->mode(i)->timing.h_display;
        out->preferred_height = display_->mode(i)->timing.v_display;
      }
  }
  return true;
}

uint32_t IntelSecondaryGpu::read_edid(uint32_t connector, ByteView out) {
  if (!display_ || connector >= ordered_) return 0;
  const intel_secondary::Connector& c = display_->connectors()[order_[connector]];
  const ConstByteView edid = ConstByteView(c.edid).first(c.edid_size).at_most(out.size());
  if (!out.first(edid.size()).copy_from(edid)) return 0;
  return static_cast<uint32_t>(edid.size());
}

bool IntelSecondaryGpu::mode_available(uint32_t connector, uint32_t width, uint32_t height) const {
  if (!driven(connector)) return false;
  for (uint32_t i = 0; i < display_->mode_count(); ++i)
    if (display_->mode(i)->timing.h_display == width && display_->mode(i)->timing.v_display == height) return true;
  return false;
}

uint32_t IntelSecondaryGpu::device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const {
  if (!driven(connector) || !out) return 0;
  const ArrayView<MonitorMode> list(out, capacity);
  uint32_t n = 0;
  for (uint32_t i = 0; i < display_->mode_count() && n < list.size(); ++i) {
    const intel_secondary::Mode& m = *display_->mode(i);
    MonitorMode& o = list[n++];
    o = MonitorMode();
    o.width = m.timing.h_display;
    o.height = m.timing.v_display;
    o.refresh_millihz = m.refresh_millihz;
    o.preferred = m.preferred;
  }
  return n;
}

bool IntelSecondaryGpu::set_display_mode(uint32_t connector, uint32_t width, uint32_t height) {
  if (!driven(connector)) return false;
  for (uint32_t i = 0; i < display_->mode_count(); ++i)
    if (display_->mode(i)->timing.h_display == width && display_->mode(i)->timing.v_display == height) return set_mode(i);
  return false;
}

}  // namespace hardware
