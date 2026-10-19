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

// VMware's paravirtual NIC, vmxnet3 (PCI 15AD:07B0; ESXi, Workstation, QEMU's `vmxnet3`). One transmit queue with its completion ring, one receive queue
// (a ring of buffers for the start of a packet, a second ring for the rest that stays empty because every buffer holds a whole frame) with its
// completion ring, and a "driver shared" block that tells the device where they are. Rings change hands through a generation bit that flips
// each lap. Polled; interrupts stay off.
class Vmxnet3NetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kTxRing = 64, kRxRing = 64, kRxRing2 = 32;
  static constexpr uint32_t kBufferBytes = 2048;

  Vmxnet3NetworkInterface(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  static bool supports(uint16_t vendor_id, uint16_t device_id) { return vendor_id == 0x15AD && device_id == 0x07B0; }

  const char* name() const override { return "VMware vmxnet3"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  uint32_t link_speed_mbps() const override;
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool) override { return true; }
  NetworkStatistics statistics() const override { return stats_; }

 private:
  struct Ring {
    volatile uint32_t* words = nullptr;   // 4 words per descriptor
    uint32_t size = 0, next = 0, gen = 1;
    void advance() {
      if (++next == size) { next = 0; gen ^= 1; }
    }
  };

  uint32_t command(uint32_t code) const;
  void post_receive(uint32_t slot);
  void reap_transmit();

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint64_t pt_ = 0, vd_ = 0;      // BAR 0 (producer registers) and BAR 1 (control registers)
  ArrayView<uint8_t> memory_;     // everything the device reads or writes, in one allocation
  uint8_t* shared_ = nullptr;
  Ring tx_, tx_comp_, rx_, rx_comp_;
  uint8_t* tx_buffers_ = nullptr;
  uint8_t* rx_buffers_ = nullptr;
  uint32_t tx_inflight_ = 0;
  MacAddress mac_{};
  bool owns_device_ = false;
  bool started_ = false;
  kernel::Mutex mutex_;
  NetworkStatistics stats_;
};

}  // namespace hardware
