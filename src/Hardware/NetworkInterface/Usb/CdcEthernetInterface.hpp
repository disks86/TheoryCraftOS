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

// A USB Ethernet adapter of the communications class: CDC-ECM (one frame per bulk transfer) or CDC-NCM (frames packed into transfer blocks).
// That is most adapters that are not ASIX: the Realtek RTL8152/8153/8156 in their class configuration, phones sharing their connection, docks,
// QEMU's usb-net. Polled: frames are collected from a bulk IN stream when receive() asks, the link state from the notification endpoint.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Hardware/NetworkInterface/Usb/CdcNcm.hpp"
#include "Hardware/NetworkInterface/Usb/UsbNetworkCommon.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

class CdcEthernetInterface final : public INetworkInterface {
 public:
  static constexpr uint32_t kEcmBuffer = 2048;                 // a frame (1514 bytes and whatever padding a device adds)
  static constexpr uint32_t kNtbBuffer = 16384;                // the largest NTB we ask an NCM function for
  static constexpr uint32_t kTransmitBuffer = 2048;

  CdcEthernetInterface(IUsbHostController& host, uint32_t port, const UsbCdcEthernetFunction& function, allocators::IAllocator& allocator)
      : host_(host), port_(port), function_(function), allocator_(allocator), generation_(host.port_generation(port)) {}
  ~CdcEthernetInterface() { release(); }

  const char* name() const override { return function_.protocol == UsbNetworkProtocol::Ncm ? "USB Ethernet (CDC-NCM)" : "USB Ethernet (CDC-ECM)"; }
  bool initialize() override;
  void shutdown() override;

  MacAddress mac_address() const override { return mac_; }
  bool link_up() const override;
  uint32_t link_speed_mbps() const override;
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override;
  NetworkStatistics statistics() const override {
    NetworkStatistics s = stats_;
    s.frames_dropped += assembler_.overflows();
    return s;
  }

  const UsbCdcEthernetFunction& function() const { return function_; }
  // True while the device is still the one this driver took over (it has not been unplugged).
  bool present() const { return host_.port_generation(port_) == generation_; }

 private:
  bool set_packet_filter(bool all_multicast);
  bool setup_ncm();
  void poll_notifications() const;
  void release();

  IUsbHostController& host_;
  uint32_t port_;
  UsbCdcEthernetFunction function_;
  allocators::IAllocator& allocator_;
  uint32_t generation_;

  MacAddress mac_{};
  bool started_ = false;
  ByteView receive_storage_;                                   // the transfer being put together
  ByteView transmit_;                                          // memory the controller reads: one outgoing transfer
  UsbTransferAssembler assembler_;
  NcmNtbParser ntb_;                                           // NCM: the block whose datagrams are being handed out
  bool ntb_open_ = false;
  uint32_t ntb_in_size_ = 0;
  uint16_t out_divisor_ = 4, out_remainder_ = 0;
  uint32_t ntb_out_max_ = 0;
  uint16_t sequence_ = 0;
  kernel::Mutex receive_mutex_, transmit_mutex_;
  NetworkStatistics stats_;

  // The link as the notifications tell it (up until one says otherwise: not every device sends them), and a notification arriving in pieces.
  mutable kernel::Mutex notify_mutex_;
  mutable bool link_ = true;
  mutable uint32_t speed_mbps_ = 0;
  mutable uint8_t notification_[16] = {};
  mutable uint32_t notification_size_ = 0;
};

}  // namespace hardware
