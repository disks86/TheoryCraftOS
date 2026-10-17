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

#include "Hardware/AudioDevice/SystemAudioDevices.hpp"
#include "Hardware/AudioDevice/Intel/Ac97AudioDevice.hpp"
#include "Hardware/AudioDevice/Intel/IntelHdaAudioDevice.hpp"
#include "Hardware/AudioDevice/Software/SoftwareAudioDevice.hpp"
#include "Hardware/AudioDevice/Virtio/VirtioSoundDevice.hpp"

namespace hardware {

namespace {
constexpr uint8_t kPciClassMultimedia = 0x04;
constexpr uint8_t kPciSubclassAudio = 0x01, kPciSubclassHda = 0x03;

// Builds a T, initializes it, and keeps it in `list` only if that all works.
template <class T, class... Args>
void try_add(List<UniquePointer<IAudioDevice>>& list, allocators::IAllocator& allocator, Args&&... args) {
  UniquePointer<T> device = UniquePointer<T>::make(allocator, static_cast<Args&&>(args)...);
  if (!device) return;
  if (!device->initialize()) {
    device->shutdown();  // hands the device back to the firmware
    return;
  }
  list.push_back(UniquePointer<IAudioDevice>(util::move(device)));
}
}  // namespace

void SystemAudioDevices::initialize() {
  PciDeviceInfo d;
  for (uint32_t i = 0; pci_.pci_device(i, &d); ++i) {
    if (d.vendor_id == kVirtioPciVendor && d.device_id == kVirtioSoundDevice)
      try_add<VirtioSoundDevice>(devices_, allocator_, pci_, d, allocator_, st_);
    else if (d.class_code == kPciClassMultimedia && d.subclass == kPciSubclassHda)
      try_add<IntelHdaAudioDevice>(devices_, allocator_, pci_, d, allocator_, st_);
    else if (d.class_code == kPciClassMultimedia && d.subclass == kPciSubclassAudio &&
             Ac97AudioDevice::supports(d.vendor_id, d.device_id))
      try_add<Ac97AudioDevice>(devices_, allocator_, pci_, d, allocator_, st_);
  }
  if (devices_.count() == 0) {
    try_add<SoftwareAudioDevice>(devices_, allocator_, clock_, allocator_);
    fallback_ = devices_.count() > 0;
  }
}

void SystemAudioDevices::shutdown() {
  for (uint64_t i = devices_.count(); i > 0; --i) devices_[i - 1]->shutdown();  // newest first
  devices_.clear();
  fallback_ = false;
}

}  // namespace hardware
