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

#include "Kernel/Delay.hpp"
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"

namespace hardware {

namespace {
// PCI vendor capability (id 0x09) cfg_type values.
constexpr uint8_t kCapCommon = 1, kCapNotify = 2, kCapDevice = 4;
constexpr uint8_t kPciCapabilityVendor = 0x09;

// Common configuration structure offsets (virtio 1.x, section 4.1.4.3).
constexpr uint32_t kFeatureSelect = 0x00, kFeature = 0x04;
constexpr uint32_t kDriverFeatureSelect = 0x08, kDriverFeature = 0x0C;
constexpr uint32_t kMsixConfigVector = 0x10, kStatus = 0x14;
constexpr uint32_t kQueueSelect = 0x16, kQueueSize = 0x18, kQueueMsixVector = 0x1A, kQueueEnable = 0x1C;
constexpr uint16_t kNoVector = 0xFFFF;
constexpr uint8_t kPciCapabilityMsix = 0x11;
constexpr uint32_t kQueueNotifyOffset = 0x1E;
constexpr uint32_t kQueueDesc = 0x20, kQueueDriver = 0x28, kQueueDevice = 0x30;

constexpr uint8_t kStatusAcknowledge = 1, kStatusDriver = 2, kStatusDriverOk = 4, kStatusFeaturesOk = 8;
constexpr uint8_t kStatusFailed = 0x80;
constexpr uint32_t kFeatureVersion1 = 1u << 0;  // bit 32, in feature word 1

constexpr uint32_t kPciCommand = 0x04;
}  // namespace

void VirtioPciDevice::delay_us(uint64_t us) const { hardware::delay_us(us); }

uint8_t VirtioPciDevice::common8(uint32_t o) const {
  return *reinterpret_cast<volatile uint8_t*>(common_.address + o);
}
uint16_t VirtioPciDevice::common16(uint32_t o) const {
  return *reinterpret_cast<volatile uint16_t*>(common_.address + o);
}
uint32_t VirtioPciDevice::common32(uint32_t o) const {
  return *reinterpret_cast<volatile uint32_t*>(common_.address + o);
}
void VirtioPciDevice::set_common8(uint32_t o, uint8_t v) const {
  *reinterpret_cast<volatile uint8_t*>(common_.address + o) = v;
}
void VirtioPciDevice::set_common16(uint32_t o, uint16_t v) const {
  *reinterpret_cast<volatile uint16_t*>(common_.address + o) = v;
}
void VirtioPciDevice::set_common32(uint32_t o, uint32_t v) const {
  *reinterpret_cast<volatile uint32_t*>(common_.address + o) = v;
}
void VirtioPciDevice::set_common64(uint32_t o, uint64_t v) const {
  set_common32(o, static_cast<uint32_t>(v));
  set_common32(o + 4, static_cast<uint32_t>(v >> 32));
}

VirtioPciDevice::Region VirtioPciDevice::region_from_capability(uint16_t cap) const {
  const uint8_t bar = pci_.read32(device_.address, cap + 4) & 0xFF;
  const uint32_t offset = pci_.read32(device_.address, cap + 8);
  const uint32_t length = pci_.read32(device_.address, cap + 12);
  const uint64_t base = pci_.memory_bar(device_.address, bar);
  if (!base) return {};
  return {base + offset, length};
}

bool VirtioPciDevice::find_regions() {
  if (!(pci_.read32(device_.address, 0x04) & (1u << 20))) return false;  // no capability list
  uint16_t cap = pci_.read32(device_.address, 0x34) & 0xFC;
  for (int guard = 0; cap && guard < 48; ++guard) {
    const uint32_t header = pci_.read32(device_.address, cap);
    if ((header & 0xFF) == kPciCapabilityVendor) {
      const uint8_t type = header >> 24;  // cfg_type
      if (type == kCapCommon) {
        common_ = region_from_capability(cap);
      } else if (type == kCapNotify) {
        notify_ = region_from_capability(cap);
        notify_multiplier_ = pci_.read32(device_.address, cap + 16);
      } else if (type == kCapDevice) {
        device_config_ = region_from_capability(cap);
      }
    }
    cap = (header >> 8) & 0xFC;
  }
  return common_.address && notify_.address;
}

bool VirtioPciDevice::initialize() {
  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  // Memory decoding and bus mastering (the device DMAs into our queues).
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 1) | (1u << 2));

  if (!find_regions()) return false;

  set_common8(kStatus, 0);  // reset
  for (int i = 0; i < 100 && common8(kStatus) != 0; ++i) delay_us(1000);
  set_common8(kStatus, kStatusAcknowledge);
  set_common8(kStatus, kStatusAcknowledge | kStatusDriver);

  // Only VIRTIO_F_VERSION_1 (feature bit 32) is wanted; refuse legacy-only devices.
  set_common32(kFeatureSelect, 1);
  if (!(common32(kFeature) & kFeatureVersion1)) return false;
  set_common32(kFeatureSelect, 0);
  accepted_features_ = common32(kFeature) & requested_features_;  // only what both sides have
  set_common32(kDriverFeatureSelect, 0);
  set_common32(kDriverFeature, accepted_features_);
  set_common32(kDriverFeatureSelect, 1);
  set_common32(kDriverFeature, kFeatureVersion1);

  set_common8(kStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk);
  if (!(common8(kStatus) & kStatusFeaturesOk)) {
    set_common8(kStatus, kStatusFailed);
    return false;
  }
  return true;
}

bool VirtioPciDevice::setup_queue(uint16_t index, Virtqueue* queue) {
  if (!queue || index >= 8) return false;
  set_common16(kQueueSelect, index);
  const uint16_t size = common16(kQueueSize);
  if (size == 0 || !queue->init(allocator_, size)) return false;
  set_common64(kQueueDesc, queue->descriptor_address());
  set_common64(kQueueDriver, queue->available_address());
  set_common64(kQueueDevice, queue->used_address());
  queue_notify_offset_[index] = common16(kQueueNotifyOffset);
  set_common16(kQueueEnable, 1);
  return true;
}

bool VirtioPciDevice::start() {
  set_common8(kStatus, common8(kStatus) | kStatusDriverOk);
  return !(common8(kStatus) & 0xC0);  // neither NEEDS_RESET nor FAILED
}

bool VirtioPciDevice::enable_msix(uint8_t vector, uint32_t apic_id, uint16_t queue_index) {
  if (msix_capability_ || queue_index >= 8 || apic_id > 0xFF || !common_.address) return false;
  const uint16_t cap = pci_.find_capability(device_.address, kPciCapabilityMsix);
  if (!cap) return false;
  const uint32_t first = pci_.read32(device_.address, cap);   // id, next, message control (high half)
  const uint32_t table = pci_.read32(device_.address, cap + 4);
  const uint64_t bar = pci_.memory_bar(device_.address, static_cast<uint8_t>(table & 7));
  if (!bar) return false;
  volatile uint32_t* entry = reinterpret_cast<volatile uint32_t*>(bar + (table & ~7u));   // table entry 0: address low, high, data, vector control

  // MSI-X on with the whole function masked while the entry is written, then the entry and the function unmasked.
  const uint32_t control = first >> 16;
  pci_.write32(device_.address, cap, (first & 0xFFFF) | ((control | 0xC000u) << 16));
  entry[3] = 1;                                           // this vector masked
  entry[0] = 0xFEE00000u | (apic_id << 12);               // fixed delivery to that core, physical destination
  entry[1] = 0;
  entry[2] = vector;                                      // edge triggered
  entry[3] = 0;
  pci_.write32(device_.address, cap, (first & 0xFFFF) | (((control | 0x8000u) & ~0x4000u) << 16));

  set_common16(kMsixConfigVector, kNoVector);             // no interrupt for configuration changes
  set_common16(kQueueSelect, queue_index);
  set_common16(kQueueMsixVector, 0);                      // table entry 0
  if (common16(kQueueMsixVector) == kNoVector) {          // the device ran out of vectors
    pci_.write32(device_.address, cap, first);
    return false;
  }
  msix_capability_ = cap;
  msix_queue_ = queue_index;
  return true;
}

void VirtioPciDevice::disable_msix() {
  if (!msix_capability_) return;
  if (common_.address) {
    set_common16(kQueueSelect, msix_queue_);
    set_common16(kQueueMsixVector, kNoVector);
  }
  const uint32_t first = pci_.read32(device_.address, msix_capability_);
  pci_.write32(device_.address, msix_capability_, first & ~(0xC000u << 16));   // MSI-X off, function unmasked
  msix_capability_ = 0;
}

void VirtioPciDevice::notify(uint16_t queue_index) {
  if (queue_index >= 8) return;
  const uint64_t address = notify_.address + uint64_t{queue_notify_offset_[queue_index]} * notify_multiplier_;
  *reinterpret_cast<volatile uint16_t*>(address) = queue_index;
}

uint8_t VirtioPciDevice::read_config8(uint32_t offset) const {
  if (!device_config_.address || offset >= device_config_.length) return 0;
  return *reinterpret_cast<volatile uint8_t*>(device_config_.address + offset);
}

void VirtioPciDevice::write_config8(uint32_t offset, uint8_t value) const {
  if (!device_config_.address || offset >= device_config_.length) return;
  *reinterpret_cast<volatile uint8_t*>(device_config_.address + offset) = value;
}

void VirtioPciDevice::shutdown() {
  disable_msix();
  if (common_.address) set_common8(kStatus, 0);  // reset: the device stops using our memory
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

}  // namespace hardware
