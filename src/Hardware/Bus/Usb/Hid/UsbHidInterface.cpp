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

#include "Hardware/Bus/Usb/Hid/UsbHidInterface.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"

namespace hardware {

namespace {
constexpr uint8_t kClassHid = 3;
constexpr uint8_t kTransferInterrupt = 3;
}  // namespace

uint32_t find_hid_interfaces(ConstByteView configuration, ArrayView<UsbHidInterface> out) {
  UsbDescriptorWalker walker(configuration);
  ConstByteView descriptor;
  UsbConfigurationInfo configuration_info;
  if (!walker.next(&descriptor) || !parse_usb_configuration(descriptor, &configuration_info)) return 0;

  uint32_t found = 0;
  UsbHidInterface current{};
  bool in_hid = false;      // inside a HID interface (alternate setting 0)
  bool have_endpoint = false;
  while (walker.next(&descriptor)) {
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
    if (usb_descriptor_type(descriptor) == kUsbDescriptorInterface && parse_usb_interface(descriptor, &interface)) {
      in_hid = interface.device_class == kClassHid && interface.alternate_setting == 0;
      current = UsbHidInterface{};
      current.configuration_value = configuration_info.value;
      current.number = interface.number;
      current.subclass = interface.subclass;
      current.protocol = interface.protocol;
      have_endpoint = false;
    } else if (in_hid && usb_descriptor_type(descriptor) == kUsbDescriptorHid && descriptor.size() >= 9) {
      ByteReader hid(descriptor);
      hid.skip(7);
      current.report_descriptor_length = hid.le16();  // size of the first class descriptor (the report descriptor)
    } else if (in_hid && !have_endpoint && parse_usb_endpoint(descriptor, &endpoint) && endpoint.is_in() &&
               endpoint.transfer_type() == kTransferInterrupt) {
      current.endpoint_address = endpoint.address;
      current.max_packet = endpoint.max_packet;
      current.interval = endpoint.interval;
      have_endpoint = true;
      if (found < out.size()) out[found] = current;
      ++found;
    }
  }
  return found;
}

}  // namespace hardware
