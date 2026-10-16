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

#include "Hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.hpp"

namespace hardware {

namespace {
// Bochs VBE ("dispi") registers.
constexpr uint16_t kIndexId = 0;
constexpr uint16_t kIndexXres = 1;
constexpr uint16_t kIndexYres = 2;
constexpr uint16_t kIndexBpp = 3;
constexpr uint16_t kIndexEnable = 4;
constexpr uint16_t kEnabled = 0x01;
constexpr uint16_t kLinearFramebuffer = 0x40;
constexpr uint16_t kMinId = 0xB0C0;

// Sizes that fit QEMU's default 16 MiB of video memory at 32 bpp.
constexpr DisplayMode kModes[] = {
    {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1920, 1080},
};
constexpr uint32_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);
}  // namespace

uint16_t QemuStdVgaGpu::read_register(uint16_t index) const {
  index_port_.write16(index);
  return data_port_.read16();
}

void QemuStdVgaGpu::write_register(uint16_t index, uint16_t value) const {
  index_port_.write16(index);
  data_port_.write16(value);
}

bool QemuStdVgaGpu::initialize() {
  if (!pci_ || device_.vendor_id != kPciVendor || device_.device_id != kPciDevice) return false;
  if (read_register(kIndexId) < kMinId) return false;

  const uint64_t bar0 = pci_->memory_bar(device_.address, 0);
  if (!bar0) return false;
  framebuffer_ = reinterpret_cast<uint8_t*>(bar0);

  // The firmware normally leaves the card enabled at 32 bpp; adopt that mode
  // if it is one we know, otherwise program the first one.
  const bool enabled = read_register(kIndexEnable) & kEnabled;
  const uint32_t w = read_register(kIndexXres), h = read_register(kIndexYres);
  if (enabled && read_register(kIndexBpp) == 32) {
    for (uint32_t i = 0; i < kModeCount; ++i) {
      if (kModes[i].width == w && kModes[i].height == h) {
        current_ = i;
        set_framebuffer(ByteView(framebuffer_, uint64_t{w} * h * 4), w, h, w * 4, true);
        return true;
      }
    }
  }
  return set_mode(0);
}

uint32_t QemuStdVgaGpu::mode_count() const { return kModeCount; }

bool QemuStdVgaGpu::mode_info(uint32_t index, DisplayMode* out) const {
  if (index >= kModeCount || !out) return false;
  *out = kModes[index];
  return true;
}

bool QemuStdVgaGpu::set_mode(uint32_t index) {
  if (!framebuffer_ || index >= kModeCount) return false;
  const DisplayMode& m = kModes[index];
  write_register(kIndexEnable, 0);  // registers may only change while disabled
  write_register(kIndexXres, static_cast<uint16_t>(m.width));
  write_register(kIndexYres, static_cast<uint16_t>(m.height));
  write_register(kIndexBpp, 32);
  write_register(kIndexEnable, kEnabled | kLinearFramebuffer);
  if (read_register(kIndexXres) != m.width || read_register(kIndexYres) != m.height) return false;
  current_ = index;
  set_framebuffer(ByteView(framebuffer_, uint64_t{m.width} * m.height * 4), m.width, m.height, m.width * 4, true);
  return true;
}

}  // namespace hardware
