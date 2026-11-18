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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaNative.hpp"

namespace hardware {
namespace nv {

namespace {
constexpr uint64_t kAlign = 1u << 20;
uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// A mapped BAR as bytes: the one place an MMIO window becomes a pointer. Safe because the window stays mapped (uncached) for as long as the
// driver lives and the view is exactly as long as the mapping.
ByteView mmio_bytes(const MmioRegion& region) {
  return ByteView(reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(region.address())), region.length());
}
}  // namespace

bool Bar1VramMemory::init() {
  if (first_ >= window_.size()) return false;
  VramInfo info;
  info.bytes = window_.size();
  info.reserved_head = first_;
  return fb_.init(info);
}

GpuMemory Bar1VramMemory::allocate(MemoryTarget target, uint64_t bytes, uint64_t alignment) {
  GpuMemory m;
  if (target != MemoryTarget::Vram || bytes == 0) return m;   // (no system memory here: the display falls back or refuses)
  const Optional<uint64_t> at = fb_.allocate(bytes, alignment);
  if (!at) return m;
  ByteView view;
  if (!window_.try_subview(*at, bytes, &view)) {
    (void)fb_.release(*at);
    return m;
  }
  view.fill(0);
  m.gpu_address = *at;
  m.bytes = bytes;
  m.target = MemoryTarget::Vram;
  m.cpu = view;
  return m;
}

void Bar1VramMemory::release(GpuMemory& memory) {
  if (memory.valid() && memory.target == MemoryTarget::Vram) (void)fb_.release(memory.gpu_address);
  memory = GpuMemory();
}

NvidiaNative::~NvidiaNative() {
  disengage();
  display_ = UniquePointer<NvNativeDisplay>();
  if (therm_) therm_->shutdown();
}

bool NvidiaNative::start() {
  if (!context_.registers || !context_.pci) return fail("no register mapper");
  const uint64_t bar0 = context_.pci->memory_bar(context_.device.address, 0);
  MmioRegion region;
  if (!bar0 || !context_.registers->map_registers(bar0, MmioNvRegisters::kBar0Bytes, &region)) return fail("BAR0 not mapped");
  bar0_ = region;
  regs_ = UniquePointer<MmioNvRegisters>::make(allocator_, region);
  if (!regs_) return fail("out of memory");
  const uint32_t boot0 = regs_->read32(kPmcBoot0);
  if (boot0 == 0xFFFFFFFFu || boot0 == 0) return fail("the GPU does not answer (powered down?)");
  chip_ = identify_chip(boot0, regs_->read32(kPmcBoot42));
  if (chip_.family == Family::Unknown) return fail("a chip this driver does not know");
  vram_ = probe_vram(*regs_, chip_);                       // (reads only)
  (void)read_bios();                                       // (without one: no board tables, no connectors; the rest works)
  if (!boot_display_) (void)run_post();                     // (the boot display was POSTed by the firmware that shows it)
  gpio_ = UniquePointer<NvGpio>::make(allocator_, *regs_, chip_, board_);
  if (gpio_) therm_ = UniquePointer<NvThermMonitor>::make(allocator_, *regs_, chip_, *gpio_);
  if (therm_ && !therm_->initialize()) therm_ = UniquePointer<NvThermMonitor>();
  (void)refresh_connectors(true);
  status_ = "identified";
  return true;
}

bool NvidiaNative::resume() {
  if (!regs_) return fail("not started");
  const uint32_t boot0 = regs_->read32(kPmcBoot0);
  if (boot0 == 0xFFFFFFFFu || boot0 == 0) return fail("the GPU does not answer (powered down?)");
  if (boot0 != chip_.boot0) return fail("a different chip answers after the power cycle");
  if (!boot_display_) (void)run_post();                     // (after a power cut the GPU is as un-POSTed as a cold boot)
  if (therm_) {
    therm_->shutdown();
    if (!therm_->initialize()) therm_ = UniquePointer<NvThermMonitor>();
  }
  status_ = "identified";
  return true;
}

bool NvidiaNative::run_post() {
  if (!bios_image_) {
    post_ = PostReport{};
    post_.reason = "no video BIOS";
    return false;
  }
  const NvBios bios(bios_image_->view());
  MmioNvVgaIo vga(bar0_);
  PostInputs in;
  in.bios = &bios;
  in.registers = regs_.get();
  in.vga = &vga;
  in.chip = chip_;
  in.board = &board_;
#ifdef TCOS_NVIDIA_NATIVE
  in.dry_run = false;
#else
  in.dry_run = true;                                        // (MMIO reads only: what would be written is counted, not written)
#endif
  post_ = NvDevinit::post(in);
  return post_.outcome == PostOutcome::Ran || post_.outcome == PostOutcome::AlreadyPosted || post_.outcome == PostOutcome::DryRun;
}

bool NvidiaNative::read_bios() {
  NvBiosSources sources;
  sources.registers = regs_.get();
  sources.chip = chip_;
  sources.mapper = context_.registers;
  sources.function = context_.device.address;
  sources.boot_display = boot_display_;                     // (NvidiaGpu is the boot display's driver; a GpuPool probe is not)
  sources.acpi_rom = acpi_rom_;
  sources.device_id = context_.device.device_id;
  sources.allocator = &allocator_;
  // (No expansion ROM BAR: switching it on is a write to the function's configuration space, which the GPU context does not give.)
  bios_image_ = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
  if (!bios_image_) return false;
  NvBiosFetchReport report;
  bios_source_ = NvBiosShadow::fetch(sources, bios_image_.get(), &report);
  if (bios_source_ == BiosSource::None) {
    bios_image_ = UniquePointer<ByteBuffer>();
    return false;
  }
  const NvBios bios(bios_image_->view());
  board_result_ = parse_board(bios, chip_, &board_);
  return board_result_.has_dcb;
}

bool NvidiaNative::read_connector(uint32_t index) {
  Connector& c = connectors_[index];
  ByteBuffer edid(allocator_);
  c.edid_source = read_connector_edid(*regs_, chip_, board_, index, &edid);
  c.edid_bytes = 0;
  if (c.edid_source != EdidSource::None) {
    const ConstByteView got = edid.view().at_most(kMaxEdid);
    ByteView(c.edid).first(got.size()).copy_from(got);
    c.edid_bytes = static_cast<uint32_t>(got.size());
  }
  c.connected = c.edid_bytes != 0 || (c.hpd != 0xff && c.hpd_state);
  return true;
}

bool NvidiaNative::refresh_connectors(bool all) {
  if (!regs_) return false;
  bool changed = false;
  if (all) {
    connector_count_ = board_.connector_count < BoardInfo::kMaxConnectors ? board_.connector_count : BoardInfo::kMaxConnectors;
    for (uint32_t i = 0; i < connector_count_; ++i) {
      Connector& c = connectors_[i];
      c = Connector();
      c.dcb_type = board_.connectors[i].type;
      c.hpd = board_.connectors[i].hpd;
      c.displayport = connector_route(board_, i).displayport;
    }
    changed = true;
  }
  for (uint32_t i = 0; i < connector_count_; ++i) {
    Connector& c = connectors_[i];
    bool look = all;
    if (c.hpd != 0xff && gpio_) {
      const Optional<bool> plugged = gpio_->hot_plug(c.hpd);
      if (plugged && *plugged != c.hpd_state) {
        c.hpd_state = *plugged;
        look = true;
      } else if (all && plugged) {
        c.hpd_state = *plugged;
      }
    }
    if (!look) continue;
    const bool was = c.connected;
    const uint32_t bytes_before = c.edid_bytes;
    (void)read_connector(i);
    changed = changed || was != c.connected || bytes_before != c.edid_bytes;
  }
  return changed;
}

int32_t NvidiaNative::temperature_millicelsius() {
  if (!therm_) return -1;
  const Optional<int32_t> t = therm_->temperature_millicelsius(0);
  return t ? *t : -1;
}

bool NvidiaNative::engage(const NvFirmwareFramebuffer& firmware, uint64_t firmware_physical, ByteView* memory) {
  if (!regs_) return fail("not started");
  if (!chip_.display_native) return fail(chip_.needs_gsp ? "the display needs the GSP firmware (Ampere and later)" : "no display engine to drive");
  if (display_ && display_->display_engaged()) return true;
  if (!memory_) {
    const uint64_t bar1 = context_.pci->memory_bar(context_.device.address, 1);
    if (!bar1 || firmware_physical < bar1 || firmware_physical - bar1 >= kBar1Visible) return fail("the firmware's framebuffer is not in BAR1");
    const uint64_t first = align_up(firmware_physical - bar1 + firmware.pixels.size(), kAlign);
    const uint64_t window = first + kWindowBytes;
    if (window > kBar1Visible || (vram_.ok() && window > vram_.info.bytes)) return fail("no room in BAR1 after the firmware's framebuffer");
    MmioRegion bar1_region;
    if (!context_.registers->map_registers(bar1, window, &bar1_region)) return fail("BAR1 not mapped");
    memory_ = UniquePointer<Bar1VramMemory>::make(allocator_, mmio_bytes(bar1_region), first, vram_.ok() ? vram_.info.bytes : window, memory_lock_);
    if (!memory_ || !memory_->init()) {
      memory_ = UniquePointer<Bar1VramMemory>();
      return fail("no VRAM for the takeover");
    }
  }
  if (!display_) display_ = UniquePointer<NvNativeDisplay>::make(allocator_, *regs_, *memory_, chip_);
  if (!display_) return fail("out of memory");
  if (!display_->engage_display(firmware, memory)) return fail(display_->display_status());
  status_ = "display engaged";
  return true;
}

void NvidiaNative::disengage() {
  if (display_ && display_->display_engaged()) display_->disengage_display();
}

ConnectorType connector_type_for_dcb(uint8_t dcb) {
  switch (dcb) {
    case 0x00: return ConnectorType::Vga;
    case 0x10: case 0x11: case 0x12: case 0x13: return ConnectorType::Composite;   // TV (composite, S-video, component ...)
    case 0x20: return ConnectorType::Component;
    case 0x30: case 0x31: return ConnectorType::Dvi;
    case 0x40: case 0x41: case 0x47: return ConnectorType::Internal;               // LVDS, LVDS SPWG, eDP
    case 0x46: case 0x48: return ConnectorType::DisplayPort;                       // DP, mini DP
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: return ConnectorType::Hdmi;
    default: return ConnectorType::Unknown;
  }
}

uint32_t order_connectors(ArrayView<const NvidiaNative::Connector> connectors, ConstByteView firmware_edid, ArrayView<uint8_t> order) {
  const uint32_t count = static_cast<uint32_t>(connectors.size());
  uint32_t n = 0;
  int first = -1;
  if (firmware_edid.size() >= 128)
    for (uint32_t i = 0; i < count && first < 0; ++i)
      if (connectors[i].edid_bytes >= 128 && ConstByteView(connectors[i].edid).first(128).equals(firmware_edid.first(128))) first = static_cast<int>(i);
  if (first >= 0 && n < order.size()) order[n++] = static_cast<uint8_t>(first);
  for (uint32_t pass = 0; pass < 2; ++pass)                 // the connected ones, then the rest
    for (uint32_t i = 0; i < count && n < order.size(); ++i) {
      if (static_cast<int>(i) == first || connectors[i].connected != (pass == 0)) continue;
      order[n++] = static_cast<uint8_t>(i);
    }
  return n;
}

void connector_label(ConnectorType type, uint32_t ordinal, ArrayView<char> out) {
  if (out.empty()) return;
  const char* name = type == ConnectorType::DisplayPort ? "DP" : type == ConnectorType::Internal ? "eDP" : connector_type_name(type);
  char digits[10];
  uint32_t d = 0;
  do { digits[d++] = static_cast<char>('0' + ordinal % 10); ordinal /= 10; } while (ordinal && d < sizeof digits);
  uint64_t at = 0;
  for (const char* p = name; *p && at + 1 < out.size(); ++p) out[at++] = *p;
  if (at + 1 < out.size()) out[at++] = '-';
  while (d && at + 1 < out.size()) out[at++] = digits[--d];
  out[at] = 0;
}

}  // namespace nv
}  // namespace hardware
