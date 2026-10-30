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

// Bluetooth over USB (Core spec vol 4 part B, and the class "Wireless Controller / RF Controller / Bluetooth Programming Interface": class 0xE0, subclass 1, protocol 1). The interface has
// an interrupt IN endpoint for events and bulk IN and OUT endpoints for ACL data; commands go to the control endpoint as class requests to the interface. (The alternate settings of the
// second interface carry SCO audio and are not used.)
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bluetooth/IHciTransport.hpp"
#include "Hardware/Bluetooth/HciTypes.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct UsbBluetoothInterface {
  uint8_t configuration = 0;
  uint8_t interface_number = 0;
  uint8_t event_endpoint = 0, event_max_packet = 0, event_interval = 0;      // interrupt IN
  uint8_t acl_in = 0, acl_out = 0;                                           // bulk
  uint16_t acl_max_packet = 0;
  bool valid() const { return event_endpoint != 0 && acl_in != 0 && acl_out != 0; }
};
// The Bluetooth interface (class 0xE0/1/1, alternate setting 0) of a configuration descriptor; false if there is none or it lacks an endpoint.
bool find_bluetooth_interface(ConstByteView configuration, UsbBluetoothInterface* out);

// The product ids of controllers that need their firmware loaded (Intel). `name` tells which kind.
enum class BluetoothVendor : uint8_t { Generic, Intel };
BluetoothVendor bluetooth_vendor(uint16_t vendor_id, uint16_t product_id);

// Packets arrive in pieces of at most the endpoint's packet size (an event of 40 bytes on a 16 byte endpoint is three transfers): the pieces are put together until the length in the
// packet's header is reached. A piece that would make the packet longer than it can be is dropped with the packet (the controller and the host are out of step).
class HciReassembler {
 public:
  // `header` bytes precede the length field; the length is `length_bytes` wide (1 for events, 2 for ACL data) and counts what follows the header.
  HciReassembler(ArrayView<uint8_t> storage, uint32_t header, uint32_t length_bytes) : storage_(storage), header_(header), length_bytes_(length_bytes) {}
  // Adds a piece; true when a whole packet is there (take it with packet(), then release()).
  bool push(ConstByteView piece);
  ConstByteView packet() const { return ConstByteView(storage_).first(size_); }
  void release() { size_ = 0; }
  void reset() { size_ = 0; }
  uint32_t dropped() const { return dropped_; }

 private:
  bool complete() const;
  ArrayView<uint8_t> storage_;
  uint32_t header_, length_bytes_;
  uint32_t size_ = 0, dropped_ = 0;
};

class UsbHciTransport final : public IHciTransport {
 public:
  static constexpr uint32_t kBuffer = 1100;
  // Takes over the Bluetooth controller on a root port (configuration set, endpoints started); null if it is none or does not come up.
  static UniquePointer<UsbHciTransport> open(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator);

  UsbHciTransport(IUsbHostController& host, uint32_t port, const UsbBluetoothInterface& interface, allocators::IAllocator& allocator);
  ~UsbHciTransport();
  bool start();

  bool send_command(ConstByteView packet) override;
  bool send_acl(ConstByteView packet) override;
  bool poll_event(ByteView buffer, uint16_t* length) override;
  bool poll_acl(ByteView buffer, uint16_t* length) override;
  bool present() const override { return host_.port_generation(port_) == generation_; }
  uint32_t port() const { return port_; }
  uint32_t generation() const { return generation_; }
  const UsbBluetoothInterface& interface() const { return interface_; }

 private:
  IUsbHostController& host_;
  uint32_t port_;
  UsbBluetoothInterface interface_;
  allocators::IAllocator& allocator_;
  uint32_t generation_ = 0;
  ByteView transmit_;               // memory the controller can reach: one outgoing packet
  bool acl_stream_ = false;
  uint8_t event_storage_[2 + hci::kMaxEventParams] = {};
  uint8_t acl_storage_[4 + hci::kMaxAclData + 8] = {};
  HciReassembler events_{ArrayView<uint8_t>(event_storage_), 1, 1};      // event: code, length
  HciReassembler acl_{ArrayView<uint8_t>(acl_storage_), 2, 2};           // ACL: handle and flags, length (2)
};

}  // namespace hardware
