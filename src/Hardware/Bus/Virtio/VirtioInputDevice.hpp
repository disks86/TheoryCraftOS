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

#pragma once
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"

namespace hardware {

// One virtio-input event, in Linux input-subsystem terms (virtio spec 5.8).
struct VirtioInputEvent {
  uint16_t type;   // 0 sync, 1 key/button, 2 relative axis, 3 absolute axis
  uint16_t code;
  uint32_t value;  // key: 0 up, 1 down, 2 repeat; axes: signed amount or position
};

enum class VirtioInputKind : uint8_t { Unknown, Keyboard, Mouse, Tablet };

constexpr uint16_t kEvSyn = 0, kEvKey = 1, kEvRel = 2, kEvAbs = 3;

// A virtio-input device: asks the device what it can do, then receives its
// events through the event queue. Wrapped by VirtioKeyboard and VirtioMouse.
class VirtioInputDevice final : public IHardware {
 public:
  static constexpr uint16_t kEventBuffers = 64;  // events in flight

  VirtioInputDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                    efi::SystemTable* st)
      : transport_(pci, device, allocator, st), allocator_(allocator) {}

  const char* name() const override { return "virtio input device"; }
  bool initialize() override;
  void shutdown() override;

  VirtioInputKind kind() const { return kind_; }
  // Largest value of an absolute axis (0 = X, 1 = Y); 0 if unknown.
  uint32_t absolute_max(uint8_t axis) const;

  // The next event from the device, or false if none is waiting.
  bool next_event(VirtioInputEvent* out);

 private:
  // Reads one virtio_input_config entry into `data`; returns its size.
  uint8_t query(uint8_t select, uint8_t subselect, uint8_t* data, uint8_t capacity) const;
  bool supports(uint8_t subselect, uint16_t code) const;
  VirtioInputKind classify() const;

  VirtioPciDevice transport_;
  allocators::IAllocator& allocator_;
  Virtqueue events_;
  VirtioInputEvent* buffers_ = nullptr;
  VirtioInputKind kind_ = VirtioInputKind::Unknown;
  uint32_t abs_max_[2] = {};
  bool started_ = false;
};

}  // namespace hardware
