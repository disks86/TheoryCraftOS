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

// What the USB network drivers share: finding the network function in a configuration descriptor (the class ones, CDC-ECM and CDC-NCM, and the
// endpoints of a vendor-specific one), reading the device's other configurations, the MAC address string, and putting bulk transfers that
// arrive one packet at a time back together. Devices are untrusted: every length is checked against the bytes that came.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/NetworkInterface/NetworkTypes.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

enum class UsbNetworkProtocol : uint8_t { None, Ecm, Ncm };

// A CDC Ethernet function: the communication interface (class 2, subclass 6 for ECM, 13 for NCM) with its functional descriptors and
// notification endpoint, and the data interface (class 10) alternate setting that has the bulk endpoints.
struct UsbCdcEthernetFunction {
  UsbNetworkProtocol protocol = UsbNetworkProtocol::None;
  uint8_t configuration = 0;                                   // bConfigurationValue
  uint8_t control_interface = 0, data_interface = 0, data_alternate = 0;
  uint8_t mac_string = 0;                                      // iMACAddress of the Ethernet Networking functional descriptor
  uint16_t max_segment = 0;                                    // wMaxSegmentSize
  uint8_t notify_endpoint = 0, notify_interval = 0;            // interrupt IN (0: none)
  uint16_t notify_max_packet = 0;
  uint8_t bulk_in = 0, bulk_out = 0;
  uint16_t in_max_packet = 0, out_max_packet = 0;
  bool valid() const { return protocol != UsbNetworkProtocol::None && mac_string != 0 && bulk_in != 0 && bulk_out != 0 && in_max_packet != 0 && out_max_packet != 0; }
};

// The first complete function of that protocol in one configuration; false if there is none.
bool find_cdc_ethernet_function(ConstByteView configuration, UsbNetworkProtocol protocol, UsbCdcEthernetFunction* out);
// Either: ECM if the configuration has it (the simpler of the two, and what most devices do well), else NCM.
bool find_cdc_ethernet_function(ConstByteView configuration, UsbCdcEthernetFunction* out);

// The endpoints of a vendor-specific network interface (ASIX and the like): the first interface, alternate setting 0, with a bulk IN and a
// bulk OUT endpoint; its interrupt IN endpoint if it has one.
struct UsbVendorNetworkEndpoints {
  uint8_t configuration = 0, interface_number = 0;
  uint8_t bulk_in = 0, bulk_out = 0;
  uint16_t in_max_packet = 0, out_max_packet = 0;
  uint8_t interrupt_in = 0, interrupt_interval = 0;
  uint16_t interrupt_max_packet = 0;
  bool valid() const { return bulk_in != 0 && bulk_out != 0 && in_max_packet != 0 && out_max_packet != 0; }
};
bool find_vendor_network_endpoints(ConstByteView configuration, UsbVendorNetworkEndpoints* out);

// True if no interface of the configuration has a driver in this OS: each is vendor-specific (class 0xFF), RNDIS control, or CDC data (which
// belongs to one of those). Looking for a class function in another configuration then takes nothing from anyone. (QEMU's usb-net and many
// phones put RNDIS first and CDC-ECM second, Realtek's RTL815x their vendor mode first; Linux skips the same configurations.)
bool usb_configuration_without_driver(ConstByteView configuration);

// Reads configuration `index` (0-based, GET_DESCRIPTOR) of the device on a port into `buffer`: the 9-byte header first, then all of it
// (wTotalLength, at most the buffer). *out is the part that came.
bool read_usb_configuration(IUsbHostController& host, uint32_t port, uint8_t index, ByteView buffer, ConstByteView* out);

// The MAC address string (a string descriptor of 12 hexadecimal digits in UTF-16LE, as iMACAddress names). False unless it is exactly that
// and a unicast address.
bool parse_usb_mac_string(ConstByteView string_descriptor, MacAddress* out);

// A bulk IN stream delivers a transfer in pieces of at most the endpoint's packet size; a piece shorter than that (an empty one included) ends
// the transfer. The pieces are put together in `storage`; a transfer that does not fit is dropped whole (counted in overflows()).
class UsbTransferAssembler {
 public:
  UsbTransferAssembler() = default;
  explicit UsbTransferAssembler(ByteView storage) : storage_(storage) {}

  // Adds one piece. True when it ended a transfer that has bytes: take them with data(), then release().
  bool push(ConstByteView piece, uint16_t max_packet);
  ConstByteView data() const { return ConstByteView(storage_).first(size_); }
  void release() { size_ = 0; }
  void reset() { size_ = 0; discarding_ = false; }
  uint32_t overflows() const { return overflows_; }

 private:
  ByteView storage_;
  uint64_t size_ = 0;
  bool discarding_ = false;     // the transfer under way did not fit: its remaining pieces go nowhere
  uint32_t overflows_ = 0;
};

}  // namespace hardware
