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

// The ASIX AX88179 / AX88179A / AX88178A USB 3 Gigabit Ethernet adapters (and the many sold under other names), after Linux's
// ax88179_178a.c. Vendor-specific: the MAC and its PHY are reached through vendor control requests, frames go out with an 8-byte header,
// and the adapter packs several received frames into one bulk transfer with a table of per-frame headers at its end. Polled.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Hardware/NetworkInterface/Usb/UsbNetworkCommon.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

// The frames of one received AX88179 bulk transfer. The transfer ends with a 32-bit word: the number of frames (low half) and where the
// table of their headers starts (high half). Each header gives a frame's length (bits 28:16) and its errors (bit 29 CRC, bit 31 dropped);
// the frames lie one after the other from the start, each padded to 8 bytes and starting with 2 bytes of padding that align the IP header.
// The transfer comes from the device: the table must lie inside it, and a frame that runs into the table ends the walk.
class Ax88179RxParser {
 public:
  static constexpr uint32_t kHeaderCrcError = 1u << 29, kHeaderDropError = 1u << 31;

  // False (and nothing to hand out) if the trailer or the header table do not fit the transfer.
  bool start(ConstByteView transfer);
  // The next good frame (without the 2-byte pad); false when there are no more. Frames with errors are skipped and counted.
  bool next(ConstByteView* frame);
  uint32_t errors() const { return errors_; }

 private:
  ConstByteView frames_;                                       // the part before the header table
  ConstByteView headers_;
  uint32_t count_ = 0, index_ = 0;
  uint64_t offset_ = 0;
  uint32_t errors_ = 0;
};

// The 8-byte header an outgoing frame carries (length, then the padding flag when header and frame are a whole number of USB packets).
// Writes header and frame into `out`; returns the bytes to send (one more, a zero, in that case), 0 if they do not fit.
uint32_t ax88179_build_tx(ByteView out, ConstByteView frame, uint16_t max_packet);

class Ax88179NetworkInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kReceiveBuffer = 24 * 1024 + 1024;   // the largest aggregate the bulk-in settings below allow, and a packet
  static constexpr uint32_t kTransmitBuffer = 2048;

  static bool supports(uint16_t vendor_id, uint16_t product_id);

  Ax88179NetworkInterface(IUsbHostController& host, uint32_t port, const UsbVendorNetworkEndpoints& endpoints, allocators::IAllocator& allocator)
      : host_(host), port_(port), endpoints_(endpoints), allocator_(allocator), generation_(host.port_generation(port)) {}
  ~Ax88179NetworkInterface() { release(); }

  const char* name() const override { return "ASIX AX88179 USB Ethernet"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  uint32_t link_speed_mbps() const override { return link_up() ? speed_mbps_ : 0; }
  LinkDuplex link_duplex() const override { return link_up() ? duplex_ : LinkDuplex::Unknown; }
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override;
  NetworkStatistics statistics() const override {
    NetworkStatistics s = stats_;
    s.frames_dropped += assembler_.overflows();
    return s;
  }

  bool present() const { return host_.port_generation(port_) == generation_; }

 private:
  bool read_mac(uint16_t reg, ByteView out) const;
  bool write_mac(uint16_t reg, ConstByteView data) const;
  bool write_mac8(uint16_t reg, uint8_t value) const;
  bool write_mac16(uint16_t reg, uint16_t value) const;
  bool read_phy(uint16_t reg, uint16_t* value) const;
  bool write_phy(uint16_t reg, uint16_t value) const;
  // Reads the PHY's status and sets the medium (speed, duplex) and the bulk-in aggregation to match: Linux's ax88179_link_reset.
  void update_link() const;
  void poll_status() const;
  void release();

  IUsbHostController& host_;
  uint32_t port_;
  UsbVendorNetworkEndpoints endpoints_;
  allocators::IAllocator& allocator_;
  uint32_t generation_;

  MacAddress mac_{};
  bool started_ = false;
  uint16_t receive_control_ = 0;
  ByteView receive_storage_, transmit_;
  UsbTransferAssembler assembler_;
  Ax88179RxParser parser_;
  bool parser_open_ = false;
  kernel::Mutex receive_mutex_, transmit_mutex_;
  NetworkStatistics stats_;

  mutable kernel::Mutex link_mutex_;
  mutable bool link_ = false;
  mutable uint32_t speed_mbps_ = 0;
  mutable LinkDuplex duplex_ = LinkDuplex::Unknown;
  mutable uint32_t polls_without_status_ = 0;                  // (no interrupt endpoint: the PHY is asked now and then)
};

}  // namespace hardware
