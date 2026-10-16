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
#include <stdint.h>
#include "Utility/ByteStream.hpp"

namespace hardware {

// Walking and decoding the descriptors of a USB configuration (the blob a device
// sends for GET_DESCRIPTOR(configuration): the configuration descriptor followed by
// its interfaces, endpoints and class-specific descriptors). Devices are untrusted:
// every length is checked against the blob, and a descriptor that does not fit
// ends the walk instead of being read past the end.

constexpr uint8_t kUsbDescriptorConfiguration = 2, kUsbDescriptorInterface = 4, kUsbDescriptorEndpoint = 5,
                  kUsbDescriptorHid = 0x21;

// Hands out the descriptors of a configuration one by one, each as a view of exactly
// its own bytes (length byte and type byte included). The first one is the
// configuration descriptor itself.
class UsbDescriptorWalker {
 public:
  explicit UsbDescriptorWalker(ConstByteView configuration) : rest_(configuration) {}

  // False at the end of the blob or at a malformed descriptor.
  bool next(ConstByteView* descriptor) {
    if (rest_.size() < 2) return false;
    const uint8_t length = rest_[0];
    if (length < 2 || length > rest_.size()) return false;  // too short to be one, or runs past the end
    *descriptor = rest_.first(length);
    rest_ = rest_.dropped(length);
    return true;
  }

 private:
  ConstByteView rest_;
};

inline uint8_t usb_descriptor_type(ConstByteView descriptor) { return descriptor.size() >= 2 ? descriptor[1] : 0; }

struct UsbConfigurationInfo {
  uint16_t total_length = 0;
  uint8_t interface_count = 0;
  uint8_t value = 0;  // bConfigurationValue, for SET_CONFIGURATION
};
inline bool parse_usb_configuration(ConstByteView descriptor, UsbConfigurationInfo* out) {
  ByteReader in(descriptor);
  in.skip(1);
  if (in.u8() != kUsbDescriptorConfiguration) return false;
  out->total_length = in.le16();
  out->interface_count = in.u8();
  out->value = in.u8();
  return in.ok();
}

struct UsbInterfaceInfo {
  uint8_t number = 0;
  uint8_t alternate_setting = 0;
  uint8_t endpoint_count = 0;
  uint8_t device_class = 0, subclass = 0, protocol = 0;
};
inline bool parse_usb_interface(ConstByteView descriptor, UsbInterfaceInfo* out) {
  ByteReader in(descriptor);
  in.skip(1);
  if (in.u8() != kUsbDescriptorInterface) return false;
  out->number = in.u8();
  out->alternate_setting = in.u8();
  out->endpoint_count = in.u8();
  out->device_class = in.u8();
  out->subclass = in.u8();
  out->protocol = in.u8();
  return in.ok();
}

struct UsbEndpointInfo {
  uint8_t address = 0;  // bit 7 set: IN
  uint8_t attributes = 0;
  uint16_t max_packet = 0;
  uint8_t interval = 0;

  bool is_in() const { return address & 0x80; }
  uint8_t transfer_type() const { return attributes & 3; }  // 0 control, 1 isochronous, 2 bulk, 3 interrupt
};
inline bool parse_usb_endpoint(ConstByteView descriptor, UsbEndpointInfo* out) {
  ByteReader in(descriptor);
  in.skip(1);
  if (in.u8() != kUsbDescriptorEndpoint) return false;
  out->address = in.u8();
  out->attributes = in.u8();
  out->max_packet = in.le16() & 0x7FF;
  out->interval = in.u8();
  return in.ok();
}

}  // namespace hardware
