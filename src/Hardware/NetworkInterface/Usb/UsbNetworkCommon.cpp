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

#include "Hardware/NetworkInterface/Usb/UsbNetworkCommon.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"

namespace hardware {

namespace {
constexpr uint8_t kClassCommunications = 0x02, kClassData = 0x0A, kClassVendor = 0xFF;
constexpr uint8_t kSubclassEcm = 0x06, kSubclassNcm = 0x0D;
constexpr uint8_t kDescriptorCsInterface = 0x24, kFunctionalUnion = 0x06, kFunctionalEthernet = 0x0F;
constexpr uint8_t kTransferBulk = 2, kTransferInterrupt = 3;
constexpr uint8_t kRequestGetDescriptor = 6;
constexpr uint32_t kConfigurationHeader = 9;

int hex_digit(uint16_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
}  // namespace

bool find_cdc_ethernet_function(ConstByteView configuration, UsbNetworkProtocol protocol, UsbCdcEthernetFunction* out) {
  const uint8_t subclass = protocol == UsbNetworkProtocol::Ecm ? kSubclassEcm : protocol == UsbNetworkProtocol::Ncm ? kSubclassNcm : 0;
  if (!subclass || !out) return false;
  UsbCdcEthernetFunction f;
  f.protocol = protocol;

  // The communication interface: its functional descriptors and notification endpoint.
  {
    UsbDescriptorWalker walker(configuration);
    ConstByteView d;
    bool in_control = false, have_control = false;
    while (walker.next(&d)) {
      const uint8_t type = usb_descriptor_type(d);
      if (type == kUsbDescriptorConfiguration) {
        UsbConfigurationInfo c;
        if (parse_usb_configuration(d, &c)) f.configuration = c.value;
      } else if (type == kUsbDescriptorInterface) {
        UsbInterfaceInfo i;
        if (!parse_usb_interface(d, &i)) continue;
        if (in_control) break;                                  // past the communication interface
        in_control = !have_control && i.device_class == kClassCommunications && i.subclass == subclass && i.alternate_setting == 0;
        if (in_control) {
          have_control = true;
          f.control_interface = i.number;
          f.data_interface = static_cast<uint8_t>(i.number + 1);   // (when there is no union descriptor: the usual layout)
        }
      } else if (type == kDescriptorCsInterface && in_control && d.size() >= 3) {
        if (d[2] == kFunctionalUnion && d.size() >= 5 && d[3] == f.control_interface) {
          f.data_interface = d[4];
        } else if (d[2] == kFunctionalEthernet && d.size() >= 13) {
          ByteReader in(d);
          in.skip(3);
          f.mac_string = in.u8();
          in.skip(4);                                           // bmEthernetStatistics
          f.max_segment = in.le16();
        }
      } else if (type == kUsbDescriptorEndpoint && in_control) {
        UsbEndpointInfo e;
        if (parse_usb_endpoint(d, &e) && e.transfer_type() == kTransferInterrupt && e.is_in() && !f.notify_endpoint) {
          f.notify_endpoint = e.address;
          f.notify_max_packet = e.max_packet;
          f.notify_interval = e.interval;
        }
      }
    }
    if (!have_control || f.data_interface == f.control_interface) return false;
  }

  // The data interface: the first alternate setting with a bulk endpoint each way (alternate setting 0 normally has none).
  {
    UsbDescriptorWalker walker(configuration);
    ConstByteView d;
    bool in_data = false, found = false;
    UsbCdcEthernetFunction candidate = f;
    while (walker.next(&d) && !found) {
      const uint8_t type = usb_descriptor_type(d);
      if (type == kUsbDescriptorInterface) {
        UsbInterfaceInfo i;
        if (!parse_usb_interface(d, &i)) continue;
        in_data = i.device_class == kClassData && i.number == f.data_interface;
        candidate = f;
        candidate.data_alternate = i.alternate_setting;
      } else if (type == kUsbDescriptorEndpoint && in_data) {
        UsbEndpointInfo e;
        if (!parse_usb_endpoint(d, &e) || e.transfer_type() != kTransferBulk || e.max_packet == 0) continue;
        if (e.is_in() && !candidate.bulk_in) {
          candidate.bulk_in = e.address;
          candidate.in_max_packet = e.max_packet;
        } else if (!e.is_in() && !candidate.bulk_out) {
          candidate.bulk_out = e.address;
          candidate.out_max_packet = e.max_packet;
        }
        found = candidate.bulk_in && candidate.bulk_out;
      }
    }
    if (!found) return false;
    f = candidate;
  }
  if (!f.valid()) return false;
  *out = f;
  return true;
}

bool find_cdc_ethernet_function(ConstByteView configuration, UsbCdcEthernetFunction* out) {
  return find_cdc_ethernet_function(configuration, UsbNetworkProtocol::Ecm, out) || find_cdc_ethernet_function(configuration, UsbNetworkProtocol::Ncm, out);
}

bool find_vendor_network_endpoints(ConstByteView configuration, UsbVendorNetworkEndpoints* out) {
  if (!out) return false;
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  UsbVendorNetworkEndpoints found, candidate;
  bool in_interface = false;
  while (walker.next(&d)) {
    const uint8_t type = usb_descriptor_type(d);
    if (type == kUsbDescriptorConfiguration) {
      UsbConfigurationInfo c;
      if (parse_usb_configuration(d, &c)) found.configuration = c.value;
    } else if (type == kUsbDescriptorInterface) {
      if (in_interface && candidate.valid()) break;
      UsbInterfaceInfo i;
      if (!parse_usb_interface(d, &i)) continue;
      in_interface = i.alternate_setting == 0;
      candidate = UsbVendorNetworkEndpoints();
      candidate.interface_number = i.number;
    } else if (type == kUsbDescriptorEndpoint && in_interface) {
      UsbEndpointInfo e;
      if (!parse_usb_endpoint(d, &e) || e.max_packet == 0) continue;
      if (e.transfer_type() == kTransferBulk && e.is_in() && !candidate.bulk_in) {
        candidate.bulk_in = e.address;
        candidate.in_max_packet = e.max_packet;
      } else if (e.transfer_type() == kTransferBulk && !e.is_in() && !candidate.bulk_out) {
        candidate.bulk_out = e.address;
        candidate.out_max_packet = e.max_packet;
      } else if (e.transfer_type() == kTransferInterrupt && e.is_in() && !candidate.interrupt_in) {
        candidate.interrupt_in = e.address;
        candidate.interrupt_max_packet = e.max_packet;
        candidate.interrupt_interval = e.interval;
      }
    }
  }
  if (!in_interface || !candidate.valid()) return false;
  candidate.configuration = found.configuration;
  *out = candidate;
  return true;
}

bool usb_configuration_without_driver(ConstByteView configuration) {
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  bool any = false;
  while (walker.next(&d)) {
    if (usb_descriptor_type(d) != kUsbDescriptorInterface) continue;
    UsbInterfaceInfo i;
    if (!parse_usb_interface(d, &i)) continue;
    // RNDIS control: CDC "abstract control" with the vendor protocol, the wireless-controller and miscellaneous spellings of it
    const bool rndis = (i.device_class == 0x02 && i.subclass == 0x02 && i.protocol == 0xFF) || (i.device_class == 0xE0 && i.subclass == 0x01 && i.protocol == 0x03) ||
                       (i.device_class == 0xEF && i.subclass == 0x04 && i.protocol == 0x01);
    if (i.device_class != kClassVendor && i.device_class != 0x0A && !rndis) return false;
    any = true;
  }
  return any;
}

bool read_usb_configuration(IUsbHostController& host, uint32_t port, uint8_t index, ByteView buffer, ConstByteView* out) {
  if (buffer.size() < kConfigurationHeader || !out) return false;
  const uint16_t value = static_cast<uint16_t>((uint16_t{kUsbDescriptorConfiguration} << 8) | index);
  if (!host.control_in(port, 0x80, kRequestGetDescriptor, value, 0, buffer.first(kConfigurationHeader))) return false;
  UsbConfigurationInfo info;
  if (!parse_usb_configuration(ConstByteView(buffer).first(kConfigurationHeader), &info) || info.total_length < kConfigurationHeader) return false;
  const uint64_t want = info.total_length < buffer.size() ? info.total_length : buffer.size();
  if (!host.control_in(port, 0x80, kRequestGetDescriptor, value, 0, buffer.first(want))) return false;
  // What came must still be a configuration descriptor (a device may answer the second request differently).
  if (!parse_usb_configuration(ConstByteView(buffer).first(want), &info)) return false;
  *out = ConstByteView(buffer).first(want);
  return true;
}

bool parse_usb_mac_string(ConstByteView string_descriptor, MacAddress* out) {
  ByteReader in(string_descriptor);
  const uint8_t length = in.u8();
  if (in.u8() != 3 || length != 2 + 12 * 2 || string_descriptor.size() < length) return false;
  MacAddress mac{};
  for (int i = 0; i < 6; ++i) {
    const int high = hex_digit(in.le16()), low = hex_digit(in.le16());
    if (high < 0 || low < 0) return false;
    mac.bytes[i] = static_cast<uint8_t>(high << 4 | low);
  }
  if (!in.ok() || (mac.bytes[0] & 1)) return false;            // multicast (broadcast too) is no interface's own address
  bool zero = true;
  for (uint8_t b : mac.bytes) zero = zero && b == 0;
  if (zero) return false;
  *out = mac;
  return true;
}

bool UsbTransferAssembler::push(ConstByteView piece, uint16_t max_packet) {
  const bool last = piece.size() < max_packet;
  if (discarding_) {
    if (last) discarding_ = false;
    return false;
  }
  if (piece.size() > storage_.size() - size_) {                 // the transfer is bigger than anything this device should send
    ++overflows_;
    size_ = 0;
    discarding_ = !last;
    return false;
  }
  storage_.subview(size_, piece.size()).copy_from(piece);
  size_ += piece.size();
  return last && size_ > 0;
}

}  // namespace hardware
