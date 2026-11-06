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

// The Realtek PCI Express Ethernet controllers that most desktop boards and many laptops have on board, after Linux's r8169: the RTL8169/8110 (PCI gigabit), the
// RTL8101/8102/8103/8105/8106/8402 (Fast Ethernet), the RTL8111/8168/8411/8117 (gigabit, B to H and EP) and the RTL8125 (2.5 Gb/s, also sold as Killer E3000)
// and RTL8126 (5 Gb/s). They share the descriptor rings (16-byte descriptors with an ownership bit) and most registers; the 2.5G and 5G parts moved the
// interrupt and transmit-poll registers. Which chip it is comes from the version bits of TxConfig, not the PCI id (one id covers a dozen chips).
//
// Interrupts (MSI) when the caller enables them, each masking the chip until rearm_interrupts() (the chips' own moderation timers are not used), checked with a forced software interrupt (the RTL8125/8126 have none, so theirs are taken on
// trust); transport checksums checked on receive and finished on transmit by the chip (the descriptor's protocol bits; frames shorter than the
// Ethernet minimum get theirs in software, as r8169 does for the chips that pad them wrongly). The chip's PHY is left as the firmware configured it (r8169 also loads PHY firmware and applies per-chip PHY
// fixes; that is not done here).
enum class RtlFamily : uint8_t { Rtl8169, Rtl8101, Rtl8168, Rtl8125, Rtl8126 };

struct RtlChip {
  uint16_t mask, value;   // matched against the version bits ((TxConfig >> 20) & 0xFCF)
  RtlFamily family;
  uint32_t rx_config;     // the FIFO and DMA bits of RxConfig this chip wants
  bool auto_fifo;         // TxConfig's automatic FIFO bit (RTL8168E-VL and later)
  bool rxdv_gate;         // has the RXDV gate (RTL8168G and later), which blocks reception while it is set
  const char* name;
};

// The chip whose version bits these are, or nullptr for one r8169 does not know either.
const RtlChip* rtl8169_identify(uint32_t tx_config);

class Rtl8169NetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kReceiveDescriptors = 256;
  static constexpr uint32_t kTransmitDescriptors = 64;
  static constexpr uint32_t kBufferSize = 2048;

  Rtl8169NetworkInterface(UniquePointer<INicDevice> device, allocators::IAllocator& allocator)
      : device_(util::move(device)), allocator_(allocator) {}
  ~Rtl8169NetworkInterface() { shutdown(); }

  static bool supports(uint16_t vendor_id, uint16_t device_id);

  const char* name() const override { return chip_ ? chip_->name : "Realtek RTL8169 family"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  uint32_t link_speed_mbps() const override;
  LinkDuplex link_duplex() const override;
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override;
  NetworkOffloads offloads() const override { return NetworkOffloads{true, true}; }
  bool receive_frame(ByteView buffer, ReceivedFrame* frame) override;
  bool transmit_frame(ConstByteView frame, TransmitChecksum checksum) override;
  bool enable_interrupts(NetworkWake wake, void* context) override;
  void disable_interrupts() override;
  bool interrupts_enabled() const override { return interrupts_; }
  void rearm_interrupts() override;
  NetworkStatistics statistics() const override {
    NetworkStatistics s = stats_;
    s.interrupts = __atomic_load_n(&interrupt_count_, __ATOMIC_ACQUIRE);
    return s;
  }

  const RtlChip* chip() const { return chip_; }

 private:
  struct Descriptor {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t address;
  };
  static_assert(sizeof(Descriptor) == 16);

  INicRegisters& regs() const { return device_->registers(); }
  bool new_layout() const { return chip_->family == RtlFamily::Rtl8125 || chip_->family == RtlFamily::Rtl8126; }
  uint16_t phy_status() const;
  void mask_interrupts();
  bool reset();
  void set_receive_mode();
  void write_interrupt_mask(uint32_t mask);
  static void on_interrupt(void* self);
  uint32_t rx_opts1(uint32_t i) const;
  ByteView rx_buffer(uint32_t i) const { return buffers_.subview(uint64_t{i} * kBufferSize, kBufferSize); }
  ByteView tx_buffer(uint32_t i) const { return buffers_.subview(uint64_t{kReceiveDescriptors + i} * kBufferSize, kBufferSize); }

  UniquePointer<INicDevice> device_;
  allocators::IAllocator& allocator_;
  const RtlChip* chip_ = nullptr;
  bool claimed_ = false;
  bool started_ = false;
  bool all_multicast_ = false;
  bool interrupts_ = false;
  NetworkWake wake_ = nullptr;      // read by the interrupt handler (atomic)
  void* wake_context_ = nullptr;
  uint64_t interrupt_count_ = 0;    // atomic
  ArrayView<Descriptor> rx_ring_;
  ArrayView<Descriptor> tx_ring_;
  ByteView buffers_;  // receive buffers, then transmit buffers
  uint32_t rx_next_ = 0;
  uint32_t tx_next_ = 0;
  MacAddress mac_{};
  kernel::Mutex mutex_;  // transmit and receive may come from different threads
  NetworkStatistics stats_;
};

}  // namespace hardware
