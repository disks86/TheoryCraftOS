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
  regs_ = UniquePointer<MmioNvRegisters>::make(allocator_, region);
  if (!regs_) return fail("out of memory");
  const uint32_t boot0 = regs_->read32(kPmcBoot0);
  if (boot0 == 0xFFFFFFFFu || boot0 == 0) return fail("the GPU does not answer (powered down?)");
  chip_ = identify_chip(boot0, regs_->read32(kPmcBoot42));
  if (chip_.family == Family::Unknown) return fail("a chip this driver does not know");
  vram_ = probe_vram(*regs_, chip_);                       // (reads only)
  gpio_ = UniquePointer<NvGpio>::make(allocator_, *regs_, chip_, board_);
  if (gpio_) therm_ = UniquePointer<NvThermMonitor>::make(allocator_, *regs_, chip_, *gpio_);
  if (therm_ && !therm_->initialize()) therm_ = UniquePointer<NvThermMonitor>();
  status_ = "identified";
  return true;
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

}  // namespace nv
}  // namespace hardware
