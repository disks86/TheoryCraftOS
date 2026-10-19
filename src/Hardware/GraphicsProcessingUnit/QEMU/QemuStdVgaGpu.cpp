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

constexpr uint16_t kIndexVideoMemory64K = 0x0A;

// The sizes offered, as far as the video memory holds them at 32 bpp (QEMU's default is 16 MiB: up to 2560x1440).
constexpr DisplayMode kModes[] = {
    {640, 480},   {800, 600},   {1024, 768},  {1280, 720},  {1280, 800},  {1280, 1024}, {1366, 768},  {1440, 900},
    {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160},
};
constexpr uint32_t kKnownModes = sizeof(kModes) / sizeof(kModes[0]);
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

  // The modes that fit: the card says how much video memory it has (in units of 64 KiB).
  uint64_t memory = uint64_t{read_register(kIndexVideoMemory64K)} * 65536;
  if (memory == 0) memory = 16u << 20;
  mode_count_ = 0;
  for (uint32_t i = 0; i < kKnownModes && mode_count_ < kMaxModes; ++i)
    if (uint64_t{kModes[i].width} * kModes[i].height * 4 <= memory) modes_[mode_count_++] = kModes[i];
  if (mode_count_ == 0) return false;

  // The firmware normally leaves the card enabled at 32 bpp; adopt that mode
  // if it is one we know, otherwise program the first one.
  const bool enabled = read_register(kIndexEnable) & kEnabled;
  const uint32_t w = read_register(kIndexXres), h = read_register(kIndexYres);
  if (enabled && read_register(kIndexBpp) == 32) {
    for (uint32_t i = 0; i < mode_count_; ++i) {
      if (modes_[i].width == w && modes_[i].height == h) {
        current_ = i;
        set_framebuffer(ByteView(framebuffer_, uint64_t{w} * h * 4), w, h, w * 4, true);
        return true;
      }
    }
  }
  return set_mode(0);
}

uint32_t QemuStdVgaGpu::mode_count() const { return mode_count_; }

bool QemuStdVgaGpu::mode_info(uint32_t index, DisplayMode* out) const {
  if (index >= mode_count_ || !out) return false;
  *out = modes_[index];
  return true;
}

bool QemuStdVgaGpu::set_mode(uint32_t index) {
  if (!framebuffer_ || index >= mode_count_) return false;
  const DisplayMode m = modes_[index];
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

bool QemuStdVgaGpu::connector_info(uint32_t connector, ConnectorInfo* out) const {
  if (connector != 0 || !out) return false;
  *out = ConnectorInfo();
  out->connected = framebuffer_ != nullptr;
  out->type = ConnectorType::Vga;
  const char name[] = "VGA-1";
  for (uint32_t i = 0; i < sizeof name; ++i) out->name[i] = name[i];
  out->preferred_width = width();
  out->preferred_height = height();
  return true;
}

bool QemuStdVgaGpu::mode_available(uint32_t connector, uint32_t width, uint32_t height) const {
  if (connector != 0) return false;
  for (uint32_t i = 0; i < mode_count_; ++i)
    if (modes_[i].width == width && modes_[i].height == height) return true;
  return false;
}

uint32_t QemuStdVgaGpu::device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const {
  if (connector != 0) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < mode_count_ && n < capacity; ++i) {
    out[n] = MonitorMode();
    out[n].width = modes_[i].width;
    out[n].height = modes_[i].height;
    ++n;
  }
  return n;
}

bool QemuStdVgaGpu::set_display_mode(uint32_t connector, uint32_t width, uint32_t height) {
  if (connector != 0) return false;
  for (uint32_t i = 0; i < mode_count_; ++i)
    if (modes_[i].width == width && modes_[i].height == height) return i == current_ || set_mode(i);
  return false;
}

}  // namespace hardware
