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

#include "Hardware/Bus/Virtio/VirtioInputDevice.hpp"

namespace hardware {

namespace {
// virtio_input_config (device-specific configuration space).
constexpr uint32_t kCfgSelect = 0, kCfgSubselect = 1, kCfgSize = 2, kCfgData = 8;
constexpr uint8_t kCfgEvBits = 0x11;   // bitmap of supported codes for event type `subselect`
constexpr uint8_t kCfgAbsInfo = 0x12;  // virtio_input_absinfo for axis `subselect`

constexpr uint16_t kKeyA = 30;
constexpr uint16_t kRelX = 0, kAbsX = 0, kAbsY = 1;
}  // namespace

uint8_t VirtioInputDevice::query(uint8_t select, uint8_t subselect, ByteView data) const {
  transport_.write_config8(kCfgSelect, select);
  transport_.write_config8(kCfgSubselect, subselect);
  const uint64_t size = transport_.read_config8(kCfgSize) < data.size() ? transport_.read_config8(kCfgSize) : data.size();
  for (uint64_t i = 0; i < size; ++i) data[i] = transport_.read_config8(kCfgData + static_cast<uint32_t>(i));
  return static_cast<uint8_t>(size);
}

bool VirtioInputDevice::supports(uint8_t type, uint16_t code) const {
  uint8_t bits[128];
  const uint8_t size = query(kCfgEvBits, type, ByteView(bits));
  return code / 8 < size && (bits[code / 8] & (1u << (code % 8)));
}

VirtioInputKind VirtioInputDevice::classify() const {
  if (supports(kEvAbs, kAbsX) && supports(kEvAbs, kAbsY)) return VirtioInputKind::Tablet;
  if (supports(kEvRel, kRelX)) return VirtioInputKind::Mouse;
  if (supports(kEvKey, kKeyA)) return VirtioInputKind::Keyboard;
  return VirtioInputKind::Unknown;
}

bool VirtioInputDevice::initialize() {
  if (!transport_.initialize()) {
    transport_.shutdown();
    return false;
  }
  kind_ = classify();
  if (kind_ == VirtioInputKind::Unknown) {
    transport_.shutdown();
    return false;
  }
  if (kind_ == VirtioInputKind::Tablet) {
    for (uint8_t axis = 0; axis < 2; ++axis) {
      uint8_t info[20];  // struct { u32 min, max, fuzz, flat, res; }
      if (query(kCfgAbsInfo, axis, ByteView(info)) >= 8)
        abs_max_[axis] = info[4] | (info[5] << 8) | (info[6] << 16) | (uint32_t{info[7]} << 24);
    }
  }

  // Queue 0 carries events to us; fill it with empty buffers for the device to write.
  if (!transport_.setup_queue(0, &events_)) {
    transport_.shutdown();
    return false;
  }
  const uint16_t count = events_.size() < kEventBuffers ? events_.size() : kEventBuffers;
  buffers_ = allocate_dma_view<VirtioInputEvent>(allocator_, count, 64);
  if (buffers_.empty()) {
    shutdown();
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) events_.post(i, &buffers_[i], sizeof(VirtioInputEvent));

  if (!transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;
  transport_.notify(0);
  return true;
}

uint32_t VirtioInputDevice::absolute_max(uint8_t axis) const { return axis < 2 ? abs_max_[axis] : 0; }

bool VirtioInputDevice::next_event(VirtioInputEvent* out) {
  if (!started_ || !out) return false;
  uint16_t id;
  uint32_t length;
  if (!events_.take_used(&id, &length)) return false;
  if (!buffers_.in_range(id)) return false;  // an id that is not one of our buffers: the device is confused
  const volatile VirtioInputEvent& e = buffers_[id];
  *out = {e.type, e.code, e.value};
  events_.post(id, &buffers_[id], sizeof(VirtioInputEvent));  // recycle the buffer
  transport_.notify(0);
  return true;
}

void VirtioInputDevice::shutdown() {
  started_ = false;
  transport_.shutdown();  // resets the device first: it must stop writing to our queue
  events_.release(allocator_);
  release_view(allocator_, buffers_);
}

}  // namespace hardware
