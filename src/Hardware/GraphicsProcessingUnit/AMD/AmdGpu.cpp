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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpu.hpp"

namespace hardware {

namespace {
constexpr uint16_t kPciCommand = 0x04, kPciRevision = 0x08;
constexpr uint32_t kMemorySpace = 1u << 1;
constexpr uint64_t kMiB = 1ull << 20;
}  // namespace

bool AmdGpu::initialize() {
  if (!UefiGopGpu::initialize()) return false;
  prepare_native();
  engage();
  return true;
}

// Raven-family APUs only: finds the register block, reads the memory map from it, and works out how much of video memory the framebuffer needs (the largest mode the
// firmware lists). Any failure leaves the firmware's framebuffer in use.
void AmdGpu::prepare_native() {
  const PciDeviceInfo& d = context_.device;
  if (!context_.registers || !context_.pci || !context_.allocator || d.vendor_id == 0) return;
  const uint8_t revision = static_cast<uint8_t>(context_.pci->read32(d.address, kPciRevision) & 0xFF);
  const uint32_t golden = amd_raven_gb_addr_config(d.device_id, revision);
  if (golden == 0 || !(context_.pci->read32(d.address, kPciCommand) & kMemorySpace)) return;
  const uint64_t bar = context_.pci->memory_bar(d.address, 5);
  MmioRegion window;
  if (bar == 0 || !context_.registers->map_registers(bar, kAmdRavenRegisterBytes, &window)) return;
  registers_ = MmioVcnRegisters(window);
  const AmdRavenMemoryMap map = amd_read_raven_memory_map(registers_);
  if (!map.read || amd_check_gb_addr_config(golden, map.hardware_gb_addr_config) == AmdGbAddrConfigCheck::Differs) return;

  uint64_t largest = 0;
  for (uint32_t i = 0; i < mode_count(); ++i) {
    DisplayMode m;
    if (!mode_info(i, &m)) continue;
    const uint64_t bytes = (uint64_t{m.width} + 64) * 4 * m.height;   // (a pitch a little over the width)
    if (bytes > largest) largest = bytes;
  }
  if (largest == 0) return;
  reserved_ = amd_dcn1::Scanout::region_bytes(largest) + kMiB;
  scanout_ = UniquePointer<amd_dcn1::Scanout>::make(*context_.allocator, registers_, *context_.registers, map, reserved_);
  if (!scanout_) reserved_ = 0;
}

// Takes the display engine over for the firmware's current mode.
void AmdGpu::engage() {
  if (!scanout_ || scanout_->engaged()) return;
  uint64_t base = 0, bytes = 0;
  if (!UefiGopGpu::framebuffer_range(&base, &bytes) || pitch_bytes() % 4 != 0) return;
  amd_dcn1::FirmwareFramebuffer fb;
  fb.physical = base;
  fb.bytes = bytes;
  fb.width = width();
  fb.height = height();
  fb.pitch_pixels = pitch_bytes() / 4;
  ByteView memory;
  if (!scanout_->engage(fb, &memory)) return;
  firmware_base_ = base;
  firmware_bytes_ = bytes;
  native_memory_ = memory;
  native_pitch_ = pitch_bytes();
  set_framebuffer(memory, fb.width, fb.height, native_pitch_, bgr());
}

bool AmdGpu::set_mode(uint32_t index) {
  if (!scanout_) return UefiGopGpu::set_mode(index);
  // The firmware changes the mode, so the display engine goes back to its framebuffer first and is taken over again afterwards.
  if (scanout_->engaged()) {
    scanout_->disengage();
    apply_current_mode();
  }
  const bool ok = UefiGopGpu::set_mode(index);
  engage();
  return ok;
}

bool AmdGpu::framebuffer_range(uint64_t* base, uint64_t* bytes) const {
  if (native_scanout() && base && bytes) {
    *base = firmware_base_;
    *bytes = firmware_bytes_;
    return true;
  }
  return UefiGopGpu::framebuffer_range(base, bytes);
}

void AmdGpu::shutdown() {
  if (scanout_ && scanout_->engaged()) {
    scanout_->disengage();
    apply_current_mode();
  }
  scanout_ = UniquePointer<amd_dcn1::Scanout>();
}

void AmdGpu::flush_dirty() {
  if (!native_scanout()) return;
  uint32_t x, y, w, h;
  while (take_dirty(&x, &y, &w, &h)) amd_dcn1::Scanout::flush(native_memory_, native_pitch_, x, y, w, h);
}

}  // namespace hardware
