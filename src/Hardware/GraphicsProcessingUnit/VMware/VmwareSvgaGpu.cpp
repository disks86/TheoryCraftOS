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

#include "Hardware/GraphicsProcessingUnit/VMware/VmwareSvgaGpu.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint32_t kRegId = 0, kRegEnable = 1, kRegWidth = 2, kRegHeight = 3, kRegMaxWidth = 4, kRegMaxHeight = 5, kRegBitsPerPixel = 7,
                   kRegBytesPerLine = 12, kRegFbOffset = 14, kRegVramSize = 15, kRegFbSize = 16, kRegCapabilities = 17, kRegMemSize = 19,
                   kRegConfigDone = 20, kRegSync = 21, kRegBusy = 22, kRegGuestId = 23;
constexpr uint32_t kId2 = 0x90000002;
constexpr uint32_t kCap3d = 1u << 14, kCapExtendedFifo = 1u << 15;
constexpr uint32_t kFifo3dHwVersion = 7;
constexpr uint32_t kScratchMaxBytes = 8u << 20, kScratchMinBytes = 256u << 10;
constexpr uint32_t kFifoMin = 0, kFifoMax = 1, kFifoNextCommand = 2, kFifoStop = 3;
constexpr uint32_t kFifoRegisters = 293;     // the registers at the start of an extended FIFO
constexpr uint32_t kCommandUpdate = 1;
constexpr uint32_t kGuestIdOther64 = 0x5010;   // "other 64-bit"

constexpr DisplayMode kModes[] = {
    {640, 480},   {800, 600},   {1024, 768},  {1280, 720},  {1280, 800},  {1280, 1024}, {1366, 768},  {1440, 900},
    {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160},
};
constexpr uint32_t kKnownModes = sizeof(kModes) / sizeof(kModes[0]);
}  // namespace

uint32_t VmwareSvgaGpu::read_register(uint32_t index) const {
  IoPort(index_port_).write32(index);
  return IoPort(value_port_).read32();
}

void VmwareSvgaGpu::write_register(uint32_t index, uint32_t value) const {
  IoPort(index_port_).write32(index);
  IoPort(value_port_).write32(value);
}

// The value port sits one dword after the index port; ask for the version the driver speaks and see which port answers.
bool VmwareSvgaGpu::probe_ports() {
  static const uint16_t kOffsets[] = {1, 4};
  for (uint16_t offset : kOffsets) {
    value_port_ = static_cast<uint16_t>(index_port_ + offset);
    write_register(kRegId, kId2);
    if (read_register(kRegId) == kId2) return true;
  }
  return false;
}

bool VmwareSvgaGpu::initialize() {
  if (!pci_ || device_.vendor_id != kPciVendor || device_.device_id != kPciDevice) return false;
  const uint32_t bar0 = pci_->read32(device_.address, 0x10);
  if (!(bar0 & 1)) return false;   // BAR 0 is the register ports
  index_port_ = static_cast<uint16_t>(bar0 & 0xFFFC);
  pci_->write32(device_.address, 0x04, (pci_->read32(device_.address, 0x04) & 0xFFFF) | 0x7);   // I/O, memory, bus master
  if (!probe_ports()) return false;

  const uint64_t bar1 = pci_->memory_bar(device_.address, 1), bar2 = pci_->memory_bar(device_.address, 2);
  if (!bar1) return false;
  framebuffer_ = reinterpret_cast<uint8_t*>(bar1);
  framebuffer_bytes_ = read_register(kRegVramSize);
  fifo_ = bar2 ? reinterpret_cast<volatile uint32_t*>(bar2) : nullptr;
  fifo_bytes_ = read_register(kRegMemSize);
  max_width_ = read_register(kRegMaxWidth);
  max_height_ = read_register(kRegMaxHeight);
  write_register(kRegGuestId, kGuestIdOther64);

  mode_count_ = 0;
  for (uint32_t i = 0; i < kKnownModes && mode_count_ < kMaxModes; ++i)
    if (kModes[i].width <= max_width_ && kModes[i].height <= max_height_ &&
        uint64_t{kModes[i].width} * kModes[i].height * 4 <= framebuffer_bytes_)
      modes_[mode_count_++] = kModes[i];
  if (mode_count_ == 0) return false;

  // Adopt the mode the firmware left if the adapter is on and it is one we know, else program the first.
  const uint32_t w = read_register(kRegWidth), h = read_register(kRegHeight);
  if (read_register(kRegEnable) && read_register(kRegBitsPerPixel) == 32) {
    for (uint32_t i = 0; i < mode_count_; ++i) {
      if (modes_[i].width == w && modes_[i].height == h) {
        current_ = i;
        if (program(w, h)) return true;
      }
    }
  }
  current_ = 0;
  return program(modes_[0].width, modes_[0].height);
}

void VmwareSvgaGpu::init_fifo() {
  if (!fifo_ || fifo_bytes_ < 4096) return;
  const uint32_t min = (read_register(kRegCapabilities) & kCapExtendedFifo) ? kFifoRegisters * 4 : 16;
  fifo_[kFifoMin] = min;
  fifo_[kFifoMax] = fifo_bytes_;
  fifo_[kFifoNextCommand] = min;
  fifo_[kFifoStop] = min;
  fifo_ready_ = true;
}

bool VmwareSvgaGpu::program(uint32_t width, uint32_t height) {
  if (width > max_width_ || height > max_height_ || uint64_t{width} * height * 4 > framebuffer_bytes_) return false;
  write_register(kRegEnable, 0);
  fifo_ready_ = false;
  write_register(kRegWidth, width);
  write_register(kRegHeight, height);
  write_register(kRegBitsPerPixel, 32);
  init_fifo();
  write_register(kRegEnable, 1);
  write_register(kRegConfigDone, 1);
  if (read_register(kRegWidth) != width || read_register(kRegHeight) != height || read_register(kRegBitsPerPixel) != 32) return false;
  const uint32_t pitch = read_register(kRegBytesPerLine), offset = read_register(kRegFbOffset);
  const uint32_t bytes = read_register(kRegFbSize);
  if (pitch < width * 4 || offset + uint64_t{pitch} * height > framebuffer_bytes_ || (bytes && bytes < pitch * height)) return false;
  pitch_ = pitch;
  fb_offset_ = offset;
  set_framebuffer(ByteView(framebuffer_ + offset, uint64_t{pitch} * height), width, height, pitch, true);
  set_video_memory_total(framebuffer_bytes_);
  mark_all_dirty();
  present();
  return true;
}

void VmwareSvgaGpu::sync() {
  write_register(kRegSync, 1);
  for (int i = 0; i < 20000 && read_register(kRegBusy); ++i) delay_us(50);
}

// Appends one command to the FIFO ring, waiting for the host to catch up if it is full.
bool VmwareSvgaGpu::fifo_write(const uint32_t* words, uint32_t count) {
  if (!fifo_ready_) return false;
  const uint32_t min = fifo_[kFifoMin], max = fifo_[kFifoMax], bytes = count * 4;
  uint32_t next = fifo_[kFifoNextCommand];
  if (next < min || next >= max) return false;   // (corrupt)
  const uint32_t stop = fifo_[kFifoStop];
  const uint32_t used = next >= stop ? next - stop : (max - min) - (stop - next);
  if (used + bytes >= max - min) sync();   // full: the host drains it
  for (uint32_t i = 0; i < count; ++i) {
    fifo_[next / 4] = words[i];
    next += 4;
    if (next >= max) next = min;
  }
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  fifo_[kFifoNextCommand] = next;
  return true;
}

bool VmwareSvgaGpu::has_3d() const {
  return fifo_ready_ && (read_register(kRegCapabilities) & kCap3d) && (read_register(kRegCapabilities) & kCapExtendedFifo) &&
         fifo_[kFifoMin] > kFifo3dHwVersion * 4 && fifo_[kFifo3dHwVersion] != 0;
}

uint8_t* VmwareSvgaGpu::scratch(uint32_t* offset, uint32_t* bytes) {
  const uint64_t start = (uint64_t{fb_offset_} + uint64_t{pitch_} * height() + 4095) & ~uint64_t{4095};
  if (!framebuffer_ || start >= framebuffer_bytes_ || framebuffer_bytes_ - start < kScratchMinBytes) return nullptr;
  *offset = static_cast<uint32_t>(start);
  const uint64_t room = framebuffer_bytes_ - start;
  *bytes = static_cast<uint32_t>(room < kScratchMaxBytes ? room : kScratchMaxBytes);
  return framebuffer_ + start;
}

void VmwareSvgaGpu::present() {
  uint32_t x, y, w, h;
  if (!take_dirty(&x, &y, &w, &h)) return;
  const uint32_t update[5] = {kCommandUpdate, x, y, w, h};
  fifo_write(update, 5);
}

bool VmwareSvgaGpu::mode_info(uint32_t index, DisplayMode* out) const {
  if (index >= mode_count_ || !out) return false;
  *out = modes_[index];
  return true;
}

bool VmwareSvgaGpu::set_mode(uint32_t index) {
  if (index >= mode_count_) return false;
  if (!program(modes_[index].width, modes_[index].height)) return false;
  current_ = index;
  return true;
}

bool VmwareSvgaGpu::connector_info(uint32_t connector, ConnectorInfo* out) const {
  if (connector != 0 || !out) return false;
  *out = ConnectorInfo();
  out->connected = framebuffer_ != nullptr;
  out->type = ConnectorType::Virtual;
  const char name[] = "Virtual-1";
  for (uint32_t i = 0; i < sizeof name; ++i) out->name[i] = name[i];
  out->preferred_width = width();
  out->preferred_height = height();
  return true;
}

bool VmwareSvgaGpu::mode_available(uint32_t connector, uint32_t w, uint32_t h) const {
  return connector == 0 && w >= 640 && h >= 480 && w <= max_width_ && h <= max_height_ && uint64_t{w} * h * 4 <= framebuffer_bytes_;
}

uint32_t VmwareSvgaGpu::device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const {
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

// Any size the adapter can hold, not only the listed ones (a VMware window can be dragged to any size).
bool VmwareSvgaGpu::set_display_mode(uint32_t connector, uint32_t w, uint32_t h) {
  if (connector != 0 || !mode_available(connector, w, h)) return false;
  if (w == width() && h == height()) return true;
  if (!program(w, h)) {
    program(modes_[current_].width, modes_[current_].height);   // back to what was shown
    return false;
  }
  for (uint32_t i = 0; i < mode_count_; ++i)
    if (modes_[i].width == w && modes_[i].height == h) current_ = i;
  return true;
}

}  // namespace hardware
