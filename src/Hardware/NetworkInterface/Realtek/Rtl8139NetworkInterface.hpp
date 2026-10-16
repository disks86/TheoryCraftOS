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

// The Realtek RTL8139 Fast Ethernet chip (QEMU's `rtl8139`). One receive ring
// buffer the chip fills with back-to-back frames, and four transmit slots used
// in turn. Programmed through the memory-mapped copy of its registers (BAR 1),
// polled. The chip only does 32-bit DMA, so its buffers must be below 4 GiB.
class Rtl8139NetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kReceiveRingSize = 8192;
  static constexpr uint32_t kTransmitSlots = 4;
  static constexpr uint32_t kTransmitBufferSize = 2048;

  Rtl8139NetworkInterface(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                          efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  static bool supports(uint16_t vendor_id, uint16_t device_id) {
    return vendor_id == 0x10EC && device_id == 0x8139;
  }

  const char* name() const override { return "Realtek RTL8139"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  NetworkStatistics statistics() const override { return stats_; }

 private:
  uint8_t read8(uint32_t reg) const;
  uint16_t read16(uint32_t reg) const;
  uint32_t read32(uint32_t reg) const;
  void write8(uint32_t reg, uint8_t value) const;
  void write16(uint32_t reg, uint16_t value) const;
  void write32(uint32_t reg, uint32_t value) const;

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  uint64_t registers_ = 0;
  ByteView receive_ring_;       // ring plus slack so a frame can run past the end
  ByteView transmit_buffers_;
  uint32_t receive_offset_ = 0;       // where the next frame starts in the ring
  uint32_t transmit_next_ = 0;        // slot to use next
  bool transmit_used_[kTransmitSlots] = {};
  MacAddress mac_{};
  bool owns_device_ = false;
  bool started_ = false;
  kernel::Mutex mutex_;  // transmit and receive may come from different threads
  NetworkStatistics stats_;
};

}  // namespace hardware
