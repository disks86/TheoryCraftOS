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
#include "Allocators/IAllocator.hpp"
#include "Hardware/AudioDevice/IAudioDevice.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// Finds every audio output the machine has and brings them up: Intel High
// Definition Audio controllers (any vendor), Intel AC'97 and virtio-sound, each
// through a native driver. A machine with none gets a SoftwareAudioDevice that
// discards what it is given, so there is always something to play to.
class SystemAudioDevices {
 public:
  SystemAudioDevices(IPciBus& pci, const IClock& clock, allocators::IAllocator& allocator, efi::SystemTable* st, IUsbBus* usb = nullptr)
      : pci_(pci), usb_(usb), clock_(clock), allocator_(allocator), st_(st), devices_(allocator) {}

  void initialize();
  // Stops every device and frees them.
  void shutdown();

  uint32_t count() const { return static_cast<uint32_t>(devices_.count()); }
  IAudioDevice* at(uint32_t index) const { return index < devices_.count() ? devices_[index].get() : nullptr; }
  // True for a device that is real hardware, false for the software fallback.
  bool is_hardware(uint32_t index) const { return index < devices_.count() && static_cast<int32_t>(index) != fallback_index_; }

 private:
  IPciBus& pci_;
  IUsbBus* usb_;
  const IClock& clock_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  List<UniquePointer<IAudioDevice>> devices_;
  int32_t fallback_index_ = -1;  // where the software fallback sits, if there is one
};

}  // namespace hardware
