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

#include "Hardware/Bluetooth/UsbHciTransport.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"

namespace hardware {

namespace {
constexpr uint8_t kClassWireless = 0xE0, kSubclassRf = 0x01, kProtocolBluetooth = 0x01;
constexpr uint8_t kClassRequestType = 0x20;     // host to device, class, interface
constexpr uint8_t kEndpointTransferMask = 3, kTransferBulk = 2, kTransferInterrupt = 3;
}

bool find_bluetooth_interface(ConstByteView configuration, UsbBluetoothInterface* out) {
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  UsbBluetoothInterface found;
  uint8_t configuration_value = 0;
  bool in_interface = false, done = false;
  while (walker.next(&d) && !done) {
    const uint8_t type = usb_descriptor_type(d);
    if (type == kUsbDescriptorConfiguration && d.size() >= 6) {
      configuration_value = d[5];
    } else if (type == kUsbDescriptorInterface && d.size() >= 9) {
      if (in_interface && found.valid()) done = true;                      // (the next interface: the first one had what is needed)
      in_interface = !found.valid() && d[3] == 0 && d[5] == kClassWireless && d[6] == kSubclassRf && d[7] == kProtocolBluetooth;
      if (in_interface) {
        found = UsbBluetoothInterface();
        found.configuration = configuration_value;
        found.interface_number = d[2];
      }
    } else if (type == kUsbDescriptorEndpoint && d.size() >= 7 && in_interface) {
      const uint8_t address = d[2], transfer = d[3] & kEndpointTransferMask;
      const uint16_t max_packet = static_cast<uint16_t>((d[4] | (d[5] << 8)) & 0x7FF);
      if (transfer == kTransferInterrupt && (address & 0x80) && !found.event_endpoint) {
        found.event_endpoint = address;
        found.event_max_packet = static_cast<uint8_t>(max_packet > 255 ? 255 : max_packet);
        found.event_interval = d[6];
      } else if (transfer == kTransferBulk) {
        if ((address & 0x80) && !found.acl_in) { found.acl_in = address; found.acl_max_packet = max_packet; }
        else if (!(address & 0x80) && !found.acl_out) found.acl_out = address;
      }
    }
  }
  if (!found.valid()) return false;
  *out = found;
  return true;
}

BluetoothVendor bluetooth_vendor(uint16_t vendor_id, uint16_t) { return vendor_id == 0x8087 ? BluetoothVendor::Intel : BluetoothVendor::Generic; }

UniquePointer<UsbHciTransport> UsbHciTransport::open(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator) {
  UniquePointer<UsbHciTransport> none;
  uint8_t blob[1024];
  uint16_t length = 0;
  UsbBluetoothInterface interface;
  if (!host.configuration_descriptor(port, ByteView(blob), &length) || !find_bluetooth_interface(ConstByteView(blob).at_most(length), &interface)) return none;
  UniquePointer<UsbHciTransport> transport = UniquePointer<UsbHciTransport>::make(allocator, host, port, interface, allocator);
  if (!transport || !transport->start()) return none;
  return transport;
}

UsbHciTransport::UsbHciTransport(IUsbHostController& host, uint32_t port, const UsbBluetoothInterface& interface, allocators::IAllocator& allocator)
    : host_(host), port_(port), interface_(interface), allocator_(allocator), generation_(host.port_generation(port)) {}

UsbHciTransport::~UsbHciTransport() { release_view(allocator_, transmit_); }

bool UsbHciTransport::start() {
  transmit_ = allocate_dma_view<uint8_t>(allocator_, kBuffer, 64);
  if (transmit_.empty()) return false;
  if (!host_.set_configuration(port_, interface_.configuration)) return false;
  if (!host_.start_interrupt_in(port_, interface_.event_endpoint, interface_.event_max_packet, interface_.event_interval)) return false;
  if (!host_.start_bulk_endpoint(port_, interface_.acl_out, interface_.acl_max_packet ? interface_.acl_max_packet : 64)) return false;
  acl_stream_ = host_.start_bulk_in_stream(port_, interface_.acl_in, interface_.acl_max_packet ? interface_.acl_max_packet : 64);
  return true;
}

bool UsbHciTransport::send_command(ConstByteView packet) {
  return present() && packet.size() >= 3 && packet.size() <= 3 + hci::kMaxCommandParams && host_.control_out_data(port_, kClassRequestType, 0, 0, interface_.interface_number, packet);
}

bool UsbHciTransport::send_acl(ConstByteView packet) {
  if (!present() || packet.empty() || packet.size() > transmit_.size()) return false;
  transmit_.first(packet.size()).copy_from(packet);
  uint32_t moved = 0;
  if (!host_.bulk_transfer(port_, interface_.acl_out, transmit_.first(packet.size()), &moved) || moved != packet.size()) {
    host_.reset_bulk_endpoint(port_, interface_.acl_out);
    host_.control_out(port_, 0x02, 1, 0, interface_.acl_out);      // CLEAR_FEATURE(ENDPOINT_HALT)
    return false;
  }
  return true;
}

// Whole packets are handed out; the pieces of one that is still arriving are kept for the next call.
bool UsbHciTransport::poll_event(ByteView buffer, uint16_t* length) {
  if (!present()) return false;
  for (uint32_t pieces = 0; pieces < 64; ++pieces) {
    uint8_t piece[256];
    uint16_t got = 0;
    if (!host_.poll_interrupt_in(port_, interface_.event_endpoint, ByteView(piece), &got) || got == 0) return false;
    if (got > sizeof piece) got = sizeof piece;
    if (events_.push(ConstByteView(piece, got))) {
      const ConstByteView packet = events_.packet();
      if (packet.size() > buffer.size()) { events_.release(); return false; }
      buffer.first(packet.size()).copy_from(packet);
      *length = static_cast<uint16_t>(packet.size());
      events_.release();
      return true;
    }
  }
  return false;
}

bool UsbHciTransport::poll_acl(ByteView buffer, uint16_t* length) {
  if (!present() || !acl_stream_) return false;
  for (uint32_t pieces = 0; pieces < 64; ++pieces) {
    uint8_t piece[1100];
    uint16_t got = 0;
    if (!host_.poll_interrupt_in(port_, interface_.acl_in, ByteView(piece), &got) || got == 0) return false;
    if (got > sizeof piece) got = sizeof piece;
    if (acl_.push(ConstByteView(piece, got))) {
      const ConstByteView packet = acl_.packet();
      if (packet.size() > buffer.size()) { acl_.release(); return false; }
      buffer.first(packet.size()).copy_from(packet);
      *length = static_cast<uint16_t>(packet.size());
      acl_.release();
      return true;
    }
  }
  return false;
}

bool HciReassembler::complete() const {
  if (size_ < header_ + length_bytes_) return false;
  uint32_t length = storage_[header_];
  if (length_bytes_ == 2) length |= uint32_t{storage_[header_ + 1]} << 8;
  return size_ >= header_ + length_bytes_ + length;
}

bool HciReassembler::push(ConstByteView piece) {
  if (piece.size() > storage_.size() - size_) {                // too much for a packet of this kind: start over with this piece
    ++dropped_;
    size_ = 0;
    if (piece.size() > storage_.size()) return false;
  }
  storage_.subview(size_, piece.size()).copy_from(piece);
  size_ += static_cast<uint32_t>(piece.size());
  if (!complete()) return false;
  uint32_t length = storage_[header_];
  if (length_bytes_ == 2) length |= uint32_t{storage_[header_ + 1]} << 8;
  const uint32_t whole = header_ + length_bytes_ + length;
  if (size_ > whole) size_ = whole;                           // (padding after the packet is not part of it)
  return true;
}

}  // namespace hardware
