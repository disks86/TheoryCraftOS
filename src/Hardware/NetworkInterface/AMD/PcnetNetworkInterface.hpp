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
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"
#include "uefi.hpp"

namespace hardware {

// The AMD PCnet-PCI II / PCnet-FAST III (Am79C970A..Am79C973; QEMU's `pcnet`) - the adapter VirtualBox gives a new VM by default. Programmed through
// its I/O ports in 32-bit mode with the 32-bit descriptor layout (software style 2): an initialization block names a ring of receive descriptors and
// a ring of transmit descriptors, each pointing at a frame buffer. Polled. All DMA is 32-bit.
class PcnetNetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kReceiveBuffers = 16;   // (a power of two: the chip is told the logarithm)
  static constexpr uint32_t kTransmitBuffers = 8;
  static constexpr uint32_t kBufferBytes = 1536;

  PcnetNetworkInterface(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  static bool supports(uint16_t vendor_id, uint16_t device_id) { return vendor_id == 0x1022 && device_id == 0x2000; }

  const char* name() const override { return "AMD PCnet"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override { return started_; }   // (the chip has no link status of its own; a virtual cable is always plugged in)
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool) override { return true; }   // (the logical address filter is wide open)
  NetworkStatistics statistics() const override { return stats_; }

 private:
  struct Descriptor {   // software style 2
    uint32_t buffer;
    uint32_t status_length;   // OWN and flags in the top byte, the buffer length (negated) in the low 12 bits
    uint32_t message_length;
    uint32_t reserved;
  };

  uint32_t read_csr(uint32_t index) const;
  void write_csr(uint32_t index, uint32_t value) const;
  uint32_t read_bcr(uint32_t index) const;
  void write_bcr(uint32_t index, uint32_t value) const;

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint16_t io_ = 0;
  ArrayView<uint8_t> memory_;       // init block, descriptors and buffers in one DMA allocation
  volatile Descriptor* receive_ring_ = nullptr;
  volatile Descriptor* transmit_ring_ = nullptr;
  uint8_t* receive_buffers_ = nullptr;
  uint8_t* transmit_buffers_ = nullptr;
  uint32_t receive_next_ = 0, transmit_next_ = 0;
  MacAddress mac_{};
  bool owns_device_ = false;
  bool started_ = false;
  kernel::Mutex mutex_;
  NetworkStatistics stats_;
};

}  // namespace hardware
