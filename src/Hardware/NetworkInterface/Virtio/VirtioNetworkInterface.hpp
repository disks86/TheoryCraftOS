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
#include "Hardware/NetworkInterface/INetworkInterface.hpp"

namespace hardware {

constexpr uint16_t kVirtioNetDevice = 0x1041;             // 0x1040 + device type 1 (modern only)
constexpr uint16_t kVirtioNetTransitionalDevice = 0x1000;  // also speaks the old interface; the modern one is used

// A virtio-net device (QEMU's virtio-net-pci, also the NIC of most cloud
// hypervisors), modern virtio 1.x: one receive queue and one transmit queue.
// Each frame travels with a 12-byte virtio_net_hdr in front, left all-zero
// (no checksum offload, no segmentation).
class VirtioNetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint16_t kBuffers = 32;        // per queue
  static constexpr uint32_t kBufferSize = 2048;   // header + a full frame

  VirtioNetworkInterface(const IPciBus& pci, const PciDeviceInfo& device,
                         allocators::IAllocator& allocator, efi::SystemTable* st)
      : transport_(pci, device, allocator, st), allocator_(allocator) {}

  const char* name() const override { return "virtio-net"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  bool transmit(const uint8_t* frame, uint32_t length) override;
  bool receive(uint8_t* buffer, uint32_t capacity, uint32_t* length) override;
  NetworkStatistics statistics() const override { return stats_; }

 private:
  static constexpr uint32_t kHeaderSize = 12;  // virtio_net_hdr with VERSION_1
  uint8_t* rx_buffer(uint16_t i) const { return buffers_ + uint32_t{i} * kBufferSize; }
  uint8_t* tx_buffer(uint16_t i) const { return buffers_ + (uint32_t{kBuffers} + i) * kBufferSize; }
  void reclaim_transmitted();

  VirtioPciDevice transport_;
  allocators::IAllocator& allocator_;
  Virtqueue rx_, tx_;
  uint8_t* buffers_ = nullptr;  // kBuffers receive buffers, then kBuffers transmit buffers
  uint32_t tx_busy_ = 0;        // bit i set while transmit buffer i is owned by the device
  MacAddress mac_{};
  bool has_status_ = false;
  bool started_ = false;
  NetworkStatistics stats_;
};

}  // namespace hardware
