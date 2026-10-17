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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Utility/AllocatedView.hpp"
#include "Kernel/Sync.hpp"
#include "uefi.hpp"

namespace hardware {

// The Intel 8254x "e1000" Gigabit Ethernet family (82540EM, 82544GC, 82545EM:
// the NIC QEMU emulates as `e1000`). Programmed through its memory-mapped
// registers with legacy descriptor rings, polled, interrupts off. Newer parts
// (e1000e/82574, igb) differ enough to need their own drivers.
class E1000NetworkInterface final : public INetworkInterface {
 public:
  // The receive ring is deep because the card drops frames it has no buffer for:
  // a burst arriving faster than we poll must fit.
  static constexpr uint32_t kReceiveDescriptors = 256;
  static constexpr uint32_t kTransmitDescriptors = 64;
  static constexpr uint32_t kBufferSize = 2048;

  E1000NetworkInterface(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                        efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  // Whether this is a device the driver knows how to run.
  static bool supports(uint16_t vendor_id, uint16_t device_id) {
    return vendor_id == 0x8086 && (device_id == 0x100E || device_id == 0x100C || device_id == 0x100F);
  }

  const char* name() const override { return "Intel e1000"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override;
  NetworkStatistics statistics() const override { return stats_; }

 private:
  struct ReceiveDescriptor {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
  };
  struct TransmitDescriptor {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
  };

  uint32_t read(uint32_t reg) const;
  void write(uint32_t reg, uint32_t value) const;
  ByteView rx_buffer(uint32_t i) const { return buffers_.subview(uint64_t{i} * kBufferSize, kBufferSize); }
  ByteView tx_buffer(uint32_t i) const { return buffers_.subview(uint64_t{kReceiveDescriptors + i} * kBufferSize, kBufferSize); }

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint64_t registers_ = 0;
  ArrayView<ReceiveDescriptor> rx_ring_;
  ArrayView<TransmitDescriptor> tx_ring_;
  ByteView buffers_;  // receive buffers, then transmit buffers
  uint32_t rx_next_ = 0;        // next receive descriptor to inspect
  uint32_t tx_next_ = 0;        // next transmit descriptor to fill
  MacAddress mac_{};
  bool owns_device_ = false;
  bool started_ = false;
  uint32_t receive_control_ = 0;
  kernel::Mutex mutex_;  // transmit and receive may come from different threads
  NetworkStatistics stats_;
};

}  // namespace hardware
