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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpu.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04, kGmchControl = 0x50;
constexpr uint32_t kMemorySpace = 1u << 1;
constexpr uint64_t kRegisterBytes = 0x200000, kBarSize = 16u << 20;
}  // namespace

bool IntelGpu::initialize() {
  if (!UefiGopGpu::initialize()) return false;
  try_native();
  return true;
}

// Finds what the display engine needs (its registers, the global GTT, the pipe the firmware is using). Any failure leaves the firmware in charge.
void IntelGpu::try_native() {
  const PciDeviceInfo& d = context_.device;
  if (!context_.registers || !context_.pci || !context_.allocator || d.vendor_id == 0 || !intel_display::supported(info_.architecture, info_.discrete)) return;
  if (!(context_.pci->read32(d.address, kPciCommand) & kMemorySpace)) return;
  const uint64_t base = context_.pci->memory_bar(d.address, 0);
  intel_gt::GgttGeometry geometry;
  if (base == 0 || !intel_gt::ggtt_geometry(static_cast<uint16_t>(context_.pci->read32(d.address, kGmchControl)), kBarSize, &geometry)) return;
  MmioRegion registers, table;
  if (!context_.registers->map_registers(base, kRegisterBytes, &registers) || !context_.registers->map_registers(base + geometry.table_offset, geometry.table_bytes, &table)) return;
  bank_ = intel_gt::MmioRegisterBank(registers);
  scanout_ = UniquePointer<intel_display::Scanout>::make(*context_.allocator, bank_, table, info_.architecture, *context_.allocator);
  if (scanout_ && !scanout_->initialize()) scanout_ = UniquePointer<intel_display::Scanout>();
}

bool IntelGpu::set_mode(uint32_t index) {
  DisplayMode m;
  if (scanout_ && scanout_->ready() && mode_info(index, &m)) {
    ByteView memory;
    uint32_t pitch = 0;
    if (scanout_->set_mode(m.width, m.height, &memory, &pitch)) {
      native_memory_ = memory;
      native_pitch_ = pitch;
      native_mode_ = index;
      native_active_ = true;
      set_framebuffer(memory, m.width, m.height, pitch, /*bgr=*/true);
      return true;
    }
    // The display engine is showing our framebuffer already: the firmware's SetMode would change a buffer nobody scans out, so the screen stays as it is.
    if (native_active_) return false;
  }
  return UefiGopGpu::set_mode(index);
}

void IntelGpu::shutdown() {
  if (native_active_) {
    native_active_ = false;
    scanout_->shutdown();       // the firmware's plane is back, and our memory is gone
    apply_current_mode();       // so draw into the firmware's framebuffer again
  }
  scanout_ = UniquePointer<intel_display::Scanout>();
}

void IntelGpu::flush_dirty() {
  if (!native_active_) return;
  uint32_t x, y, w, h;
  while (take_dirty(&x, &y, &w, &h)) intel_display::Scanout::flush(native_memory_, native_pitch_, x, y, w, h);
}

}  // namespace hardware
