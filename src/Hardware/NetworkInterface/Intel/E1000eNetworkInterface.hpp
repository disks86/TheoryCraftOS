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
#include "Hardware/NetworkInterface/Intel/IntelEthernetRegisters.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Hardware/NetworkInterface/InterruptModeration.hpp"
#include "Hardware/NetworkInterface/NicDevice.hpp"
#include "Hardware/NetworkInterface/TcpSegmentation.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The Intel PCI Express gigabit controllers Linux drives with e1000e: the 82571/82572/82573/82574/82583 cards and, above all, the controllers built into
// Intel's chipsets that nearly every Intel desktop and laptop board of the last fifteen years has (ICH8/9/10 with the 82566/82567 PHY, the 82577/82578,
// 82579, I217, I218 and the I219 of Skylake to Panther Lake). The MAC lives in the chipset and talks to a separate PHY chip; the management engine may
// share both. Legacy descriptor rings as on the 8254x. Interrupts (MSI) when the caller enables them; IP and TCP/UDP checksums checked on
// receive (RXCSUM) and the transport checksum inserted on transmit through a TCP/IP context descriptor in front of an extended data
// descriptor; TCP segmentation offload (TSE) for segments of up to 32 KiB, chained over 2 KiB buffers. An interrupt masks itself until
// rearm_interrupts(), and the throttle (ITR) follows the traffic. One queue (the 82574's second queue and MSI-X are not used). QEMU's
// `e1000e` device is an 82574L.
//
// Kept from e1000e: the MAC-only reset (the PHY keeps the link the firmware negotiated), the station address taken back from before the reset when the
// NVM does not reload it, waking a PHY left powered down (LANPHYPC toggle) when the management engine allows it, and the I219 transmit hang workaround.
// Not done: the PHY workarounds per PHY revision, ultra-low-power exit through the ME on I217 and later, flushing a ring the firmware left running on I219.
enum class E1000eKind : uint8_t { Mac8257x, Ich, Pch, Pch2, PchLpt, PchSpt };

struct E1000eModel {
  uint16_t device_id;
  E1000eKind kind;
  const char* name;
};
// The model with this Intel PCI device id, or nullptr.
const E1000eModel* e1000e_find_model(uint16_t device_id);

class E1000eNetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kReceiveDescriptors = 256;
  static constexpr uint32_t kTransmitDescriptors = 256;
  static constexpr uint32_t kBufferSize = 2048;
  static constexpr uint32_t kMaxSegmentation = 32768;

  E1000eNetworkInterface(UniquePointer<INicDevice> device, allocators::IAllocator& allocator)
      : device_(util::move(device)), allocator_(allocator) {}
  ~E1000eNetworkInterface() { shutdown(); }

  static bool supports(uint16_t vendor_id, uint16_t device_id) { return vendor_id == 0x8086 && e1000e_find_model(device_id); }

  const char* name() const override { return model_ ? model_->name : "Intel e1000e"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  uint32_t link_speed_mbps() const override;
  LinkDuplex link_duplex() const override;
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override;
  NetworkOffloads offloads() const override { return NetworkOffloads{true, true, kMaxSegmentation, 1}; }
  bool receive_frame(ByteView buffer, ReceivedFrame* frame) override;
  bool transmit_frame(ConstByteView frame, TransmitChecksum checksum) override;
  bool transmit_segmented(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) override;
  bool enable_interrupts(NetworkWake wake, void* context) override;
  void disable_interrupts() override { interrupts_.disable(*device_, claimed_); }
  bool interrupts_enabled() const override { return interrupts_.enabled(); }
  void rearm_interrupts() override;
  NetworkStatistics statistics() const override {
    NetworkStatistics s = stats_;
    s.interrupts = interrupts_.count();
    s.interrupt_rate = interrupts_.enabled() ? moderator_.interrupts_per_second() : 0;
    return s;
  }

  const E1000eModel* model() const { return model_; }
  // Whether the PHY had to be powered up with the LANPHYPC toggle during initialize().
  bool phy_was_woken() const { return phy_woken_; }

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
  static_assert(sizeof(ReceiveDescriptor) == 16 && sizeof(TransmitDescriptor) == 16);

  INicRegisters& regs() const { return device_->registers(); }
  bool chipset_mac() const { return model_->kind != E1000eKind::Mac8257x; }
  bool pch() const { return model_->kind >= E1000eKind::Pch; }
  bool acquire_phy();
  void release_phy();
  bool phy_read(uint32_t reg, uint16_t* value);
  bool phy_write(uint32_t reg, uint16_t value);
  bool phy_accessible();
  void wake_phy();
  void enable_autonegotiation();
  bool reset();
  void reclaim_transmitted();
  bool send(ConstByteView frame, TransmitChecksum checksum, uint16_t mss);
  void write_throttle();
  ByteView rx_buffer(uint32_t i) const { return buffers_.subview(uint64_t{i} * kBufferSize, kBufferSize); }
  ByteView tx_buffer(uint32_t i) const { return buffers_.subview(uint64_t{kReceiveDescriptors + i} * kBufferSize, kBufferSize); }

  UniquePointer<INicDevice> device_;
  allocators::IAllocator& allocator_;
  const E1000eModel* model_ = nullptr;
  bool claimed_ = false;
  bool started_ = false;
  bool phy_woken_ = false;
  ArrayView<ReceiveDescriptor> rx_ring_;
  ArrayView<TransmitDescriptor> tx_ring_;
  ByteView buffers_;  // receive buffers, then transmit buffers
  uint32_t rx_next_ = 0;
  uint32_t tx_next_ = 0;   // the next descriptor to fill
  uint32_t tx_clean_ = 0;  // the first descriptor the card may still be using
  uint8_t tx_frame_end_[kTransmitDescriptors] = {};  // for the descriptor a frame starts at: the one it ends at
  static_assert(kTransmitDescriptors <= 256);
  uint32_t receive_control_ = 0;
  intel_eth::IntelInterrupts interrupts_;
  InterruptModerator moderator_;
  MacAddress mac_{};
  kernel::Mutex mutex_;  // transmit and receive may come from different threads
  NetworkStatistics stats_;
};

}  // namespace hardware
