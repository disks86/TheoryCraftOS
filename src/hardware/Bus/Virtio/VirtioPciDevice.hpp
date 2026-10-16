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
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/Bus/Virtio/Virtqueue.hpp"
#include "hardware/IHardware.hpp"
#include "uefi.hpp"

namespace hardware {

constexpr uint16_t kVirtioPciVendor = 0x1AF4;
constexpr uint16_t kVirtioInputDevice = 0x1052;  // 0x1040 + device type 18

// The transport side of a modern (virtio 1.x) PCI virtio device: locates the
// configuration structures through the PCI vendor capabilities, runs the
// reset/feature-negotiation handshake and sets up virtqueues. Device-specific
// behaviour (what the queues carry) lives in the classes built on top.
class VirtioPciDevice final : public IHardware {
 public:
  VirtioPciDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                  efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  const char* name() const override { return "virtio PCI device"; }
  // Device feature bits 0-31 to ask for in addition to VIRTIO_F_VERSION_1 (which
  // is always required). Call before initialize(); read the outcome from
  // accepted_features() afterwards.
  void request_features(uint32_t low_bits) { requested_features_ = low_bits; }
  uint32_t accepted_features() const { return accepted_features_; }

  // Takes the device from the firmware, resets it and negotiates features.
  // Queues are set up after this, then start().
  bool initialize() override;
  void shutdown() override;

  // Allocates and registers queue `index` at the device's queue size.
  bool setup_queue(uint16_t index, Virtqueue* queue);
  // Sets DRIVER_OK: the device may now use its queues.
  bool start();
  void notify(uint16_t queue_index);

  // Device-specific configuration space.
  uint8_t read_config8(uint32_t offset) const;
  void write_config8(uint32_t offset, uint8_t value) const;

 private:
  struct Region {
    uint64_t address = 0;
    uint32_t length = 0;
  };

  bool find_regions();
  Region region_from_capability(uint16_t capability) const;
  uint8_t common8(uint32_t offset) const;
  uint16_t common16(uint32_t offset) const;
  uint32_t common32(uint32_t offset) const;
  void set_common8(uint32_t offset, uint8_t value) const;
  void set_common16(uint32_t offset, uint16_t value) const;
  void set_common32(uint32_t offset, uint32_t value) const;
  void set_common64(uint32_t offset, uint64_t value) const;
  void delay_us(uint64_t us) const;

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  Region common_, notify_, device_config_;
  uint32_t notify_multiplier_ = 0;
  uint16_t queue_notify_offset_[8] = {};
  uint32_t requested_features_ = 0;
  uint32_t accepted_features_ = 0;
  bool owns_device_ = false;
};

}  // namespace hardware
