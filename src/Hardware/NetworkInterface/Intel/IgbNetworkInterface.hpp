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
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The Intel controllers with per-queue registers and "advanced" descriptors: the gigabit 82575, 82576, 82580, I350, I354, I210 and I211 (Linux's igb; the
// I210/I211 is the second port of many desktop boards and the usual add-in card) and the 2.5 Gb/s I225 and I226 (Linux's igc; the on-board port of most
// Intel desktop boards since 2020). Both use queue 0's registers at 0xC000 (receive) and 0xE000 (transmit) and the same 16-byte advanced descriptors, which
// the device overwrites with its write-back format. The I225/I226 reset with DEV_RST instead of RST and report 2.5 Gb/s in a STATUS bit of its own.
// One queue, polled, interrupts off, no offloads. QEMU's `igb` device is an 82576.
enum class IgbKind : uint8_t { I82575, I82576, I82580, I350, I354, I210, I211, I225, I226 };

struct IgbModel {
  uint16_t device_id;
  IgbKind kind;
  const char* name;
};
const IgbModel* igb_find_model(uint16_t device_id);

class IgbNetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kReceiveDescriptors = 256;
  static constexpr uint32_t kTransmitDescriptors = 64;
  static constexpr uint32_t kBufferSize = 2048;

  IgbNetworkInterface(UniquePointer<INicDevice> device, allocators::IAllocator& allocator) : device_(util::move(device)), allocator_(allocator) {}
  ~IgbNetworkInterface() { shutdown(); }

  static bool supports(uint16_t vendor_id, uint16_t device_id) { return vendor_id == 0x8086 && igb_find_model(device_id); }

  const char* name() const override { return model_ ? model_->name : "Intel igb"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  uint32_t link_speed_mbps() const override;
  LinkDuplex link_duplex() const override;
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override;
  NetworkStatistics statistics() const override { return stats_; }

  const IgbModel* model() const { return model_; }
  // Whether initialize() could restart the PHY's autonegotiation.
  bool autonegotiation_restarted() const { return autonegotiation_restarted_; }

 private:
  // Read format: the buffer's address (and a header buffer, unused). Write-back: packet info, then status/errors (offset 8) and the length (offset 12).
  struct ReceiveDescriptor {
    uint64_t packet_address;
    uint64_t header_address;
  };
  // Read format: address, command/type/length, payload length and options. Write-back: the status (DD) lands in bit 0 of the last dword.
  struct TransmitDescriptor {
    uint64_t address;
    uint32_t command_type_length;
    uint32_t offload_status;
  };
  static_assert(sizeof(ReceiveDescriptor) == 16 && sizeof(TransmitDescriptor) == 16);

  INicRegisters& regs() const { return device_->registers(); }
  bool igc() const { return model_->kind >= IgbKind::I225; }
  bool reset();
  void restart_autonegotiation();
  void post_receive(uint32_t i);
  ByteView rx_buffer(uint32_t i) const { return buffers_.subview(uint64_t{i} * kBufferSize, kBufferSize); }
  ByteView tx_buffer(uint32_t i) const { return buffers_.subview(uint64_t{kReceiveDescriptors + i} * kBufferSize, kBufferSize); }

  UniquePointer<INicDevice> device_;
  allocators::IAllocator& allocator_;
  const IgbModel* model_ = nullptr;
  bool claimed_ = false;
  bool started_ = false;
  bool autonegotiation_restarted_ = false;
  ArrayView<ReceiveDescriptor> rx_ring_;
  ArrayView<TransmitDescriptor> tx_ring_;
  ByteView buffers_;  // receive buffers, then transmit buffers
  uint32_t rx_next_ = 0;
  uint32_t tx_next_ = 0;
  uint32_t receive_control_ = 0;
  MacAddress mac_{};
  kernel::Mutex mutex_;  // transmit and receive may come from different threads
  NetworkStatistics stats_;
};

}  // namespace hardware
