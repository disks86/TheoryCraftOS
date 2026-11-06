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

#include "Hardware/NetworkInterface/Usb/CdcEthernetInterface.hpp"

namespace hardware {

namespace {
// Requests (CDC 1.2 ECM section 6.2, NCM 1.0 section 6.2) and their request types.
constexpr uint8_t kStandardInterfaceOut = 0x01, kClassInterfaceOut = 0x21, kClassInterfaceIn = 0xA1, kStandardEndpointOut = 0x02;
constexpr uint8_t kRequestSetInterface = 0x0B, kRequestClearFeature = 0x01, kRequestGetDescriptor = 0x06;
constexpr uint8_t kSetEthernetPacketFilter = 0x43, kGetNtbParameters = 0x80, kSetNtbFormat = 0x84, kSetNtbInputSize = 0x86;
constexpr uint16_t kFilterAllMulticast = 1u << 1, kFilterDirected = 1u << 2, kFilterBroadcast = 1u << 3;
// Notifications on the interrupt endpoint: an 8-byte header (bmRequestType 0xA1, bNotification, wValue, wIndex, wLength), then the data.
constexpr uint8_t kNotificationNetworkConnection = 0x00, kNotificationSpeedChange = 0x2A;
constexpr uint32_t kNotificationHeader = 8;
constexpr uint32_t kMaxBulkPacket = 1024;                      // SuperSpeed bulk endpoints; anything larger is not a real device
constexpr uint32_t kPiecesPerPoll = 64;
}  // namespace

bool CdcEthernetInterface::initialize() {
  if (started_ || !function_.valid() || function_.in_max_packet > kMaxBulkPacket || function_.out_max_packet > kMaxBulkPacket || !present()) return false;
  const bool ncm = function_.protocol == UsbNetworkProtocol::Ncm;
  receive_storage_ = allocate_view<uint8_t>(allocator_, ncm ? kNtbBuffer : kEcmBuffer);
  transmit_ = allocate_dma_view<uint8_t>(allocator_, kTransmitBuffer, 4096);   // (inside one 64 KiB window, as bulk_transfer wants)
  if (receive_storage_.empty() || transmit_.empty() || !host_.set_configuration(port_, function_.configuration)) {
    release();
    return false;
  }

  uint8_t string[64] = {};
  if (!host_.control_in(port_, 0x80, kRequestGetDescriptor, static_cast<uint16_t>((3u << 8) | function_.mac_string), 0x0409, ByteView(string)) ||
      !parse_usb_mac_string(ConstByteView(string), &mac_)) {
    release();
    return false;
  }

  // The data interface's alternate setting 0 has no endpoints: the function is idle there, which is where NCM's parameters are set. The
  // alternate setting with the endpoints starts it.
  host_.control_out(port_, kStandardInterfaceOut, kRequestSetInterface, 0, function_.data_interface);
  if ((ncm && !setup_ncm()) || !host_.control_out(port_, kStandardInterfaceOut, kRequestSetInterface, function_.data_alternate, function_.data_interface) ||
      !host_.start_bulk_endpoint(port_, function_.bulk_out, function_.out_max_packet) ||
      !host_.start_bulk_in_stream(port_, function_.bulk_in, function_.in_max_packet)) {   // (controllers that cannot receive without waiting refuse)
    release();
    return false;
  }
  if (function_.notify_endpoint &&
      !host_.start_interrupt_in(port_, function_.notify_endpoint, function_.notify_max_packet ? function_.notify_max_packet : 8, function_.notify_interval))
    function_.notify_endpoint = 0;                             // (then the link is taken to be up)
  set_packet_filter(false);                                    // (optional for the device: one that refuses it filters as it likes)
  assembler_ = UsbTransferAssembler(receive_storage_);
  started_ = true;
  return true;
}

bool CdcEthernetInterface::setup_ncm() {
  uint8_t data[28] = {};
  NcmNtbParameters p;
  if (!host_.control_in(port_, kClassInterfaceIn, kGetNtbParameters, 0, function_.control_interface, ByteView(data)) || !parse_ncm_ntb_parameters(ConstByteView(data), &p))
    return false;
  if (p.formats & 2) host_.control_out(port_, kClassInterfaceOut, kSetNtbFormat, 0, function_.control_interface);   // 16-bit blocks (the default)
  ntb_in_size_ = p.in_max_size;
  if (ntb_in_size_ > kNtbBuffer) {                             // the device would send blocks bigger than the buffer: ask for smaller ones
    uint8_t size[4];
    ByteWriter w{ByteView(size)};
    w.le32(kNtbBuffer);
    if (!host_.control_out_data(port_, kClassInterfaceOut, kSetNtbInputSize, 0, function_.control_interface, ConstByteView(size))) return false;
    ntb_in_size_ = kNtbBuffer;
  }
  out_divisor_ = p.out_divisor;
  out_remainder_ = p.out_remainder;
  ntb_out_max_ = p.out_max_size;
  return true;
}

bool CdcEthernetInterface::set_packet_filter(bool all_multicast) {
  const uint16_t filter = kFilterDirected | kFilterBroadcast | (all_multicast ? kFilterAllMulticast : 0);
  return host_.control_out(port_, kClassInterfaceOut, kSetEthernetPacketFilter, filter, function_.control_interface);
}

void CdcEthernetInterface::release() {
  release_view(allocator_, receive_storage_);
  release_view(allocator_, transmit_);
  assembler_ = UsbTransferAssembler();
  ntb_open_ = false;
}

void CdcEthernetInterface::shutdown() {
  kernel::ScopedMutex receive_guard(receive_mutex_);
  kernel::ScopedMutex transmit_guard(transmit_mutex_);
  if (started_ && present()) host_.control_out(port_, kStandardInterfaceOut, kRequestSetInterface, 0, function_.data_interface);   // the function stops
  started_ = false;
  release();
}

void CdcEthernetInterface::poll_notifications() const {
  if (!function_.notify_endpoint) return;
  kernel::ScopedMutex guard(notify_mutex_);
  const ByteView pending(notification_);
  for (uint32_t i = 0; i < 8; ++i) {
    uint8_t piece[64];
    uint16_t got = 0;
    if (!host_.poll_interrupt_in(port_, function_.notify_endpoint, ByteView(piece), &got)) return;
    const ConstByteView data = ConstByteView(piece).at_most(got);
    if (data.size() > pending.size() - notification_size_) notification_size_ = 0;            // out of step: start over with this piece
    if (data.size() > pending.size()) continue;
    pending.subview(notification_size_, data.size()).copy_from(data);
    notification_size_ += static_cast<uint32_t>(data.size());
    if (notification_size_ < kNotificationHeader) continue;
    ByteReader in(ConstByteView(pending).first(notification_size_));
    const uint8_t type = in.u8(), code = in.u8();
    const uint16_t value = in.le16();
    in.skip(2);
    const uint16_t length = in.le16();
    if (type != kClassInterfaceIn) {
      notification_size_ = 0;
      continue;
    }
    const uint32_t need = kNotificationHeader + (length < 8 ? length : 8);
    if (notification_size_ < need) continue;                   // the rest comes in the next piece
    if (code == kNotificationNetworkConnection) {
      link_ = value != 0;
      if (!link_) speed_mbps_ = 0;
    } else if (code == kNotificationSpeedChange && length >= 8) {
      speed_mbps_ = in.le32() / 1000000;                       // the downstream bit rate
    }
    notification_size_ = 0;
  }
}

bool CdcEthernetInterface::link_up() const {
  if (!started_ || !present()) return false;
  poll_notifications();
  return link_;
}

uint32_t CdcEthernetInterface::link_speed_mbps() const { return link_up() ? speed_mbps_ : 0; }

bool CdcEthernetInterface::accept_all_multicast(bool enable) {
  if (!started_ || !present()) return false;
  return set_packet_filter(enable) && enable;
}

bool CdcEthernetInterface::transmit(ConstByteView frame) {
  kernel::ScopedMutex guard(transmit_mutex_);
  if (!started_ || !present() || frame.empty() || frame.size() > kEthernetMaxFrame) {
    ++stats_.send_failures;
    return false;
  }
  uint8_t padded_frame[kEthernetMinFrame] = {};
  ConstByteView payload = frame;
  if (frame.size() < kEthernetMinFrame) {
    ByteView(padded_frame).copy_from(frame);
    payload = ConstByteView(padded_frame);
  }
  uint32_t length = 0;
  if (function_.protocol == UsbNetworkProtocol::Ncm) {
    length = build_ncm_ntb16(transmit_, payload, sequence_, out_divisor_, out_remainder_, ntb_out_max_);
    if (!length) {
      ++stats_.send_failures;
      return false;
    }
    ++sequence_;
  } else {
    transmit_.first(payload.size()).copy_from(payload);
    length = static_cast<uint32_t>(payload.size());
  }
  // A transfer that is a whole number of packets would need a zero-length packet to end it: one byte more makes the last packet short
  // instead (the receiver ignores bytes after the frame, NCM ones after the block).
  if (length % function_.out_max_packet == 0 && length < transmit_.size()) transmit_[length++] = 0;
  uint32_t moved = 0;
  if (!host_.bulk_transfer(port_, function_.bulk_out, transmit_.first(length), &moved) || moved != length) {
    host_.reset_bulk_endpoint(port_, function_.bulk_out);
    host_.control_out(port_, kStandardEndpointOut, kRequestClearFeature, 0, function_.bulk_out);   // CLEAR_FEATURE(ENDPOINT_HALT)
    ++stats_.send_failures;
    return false;
  }
  ++stats_.frames_sent;
  return true;
}

bool CdcEthernetInterface::receive(ByteView buffer, uint32_t* length) {
  kernel::ScopedMutex guard(receive_mutex_);
  if (!started_ || !length || !present()) return false;
  poll_notifications();
  for (uint32_t pieces = 0; pieces <= kPiecesPerPoll;) {
    if (ntb_open_) {                                           // NCM: the frames of the block that came last
      ConstByteView datagram;
      if (ntb_.next(&datagram)) {
        buffer.copy_from(datagram.at_most(buffer.size()));
        *length = static_cast<uint32_t>(datagram.size());
        ++stats_.frames_received;
        return true;
      }
      stats_.frames_dropped += ntb_.malformed();
      ntb_open_ = false;
      assembler_.release();
    }
    if (pieces++ == kPiecesPerPoll) break;
    uint8_t piece[kMaxBulkPacket];
    uint16_t got = 0;
    if (!host_.poll_interrupt_in(port_, function_.bulk_in, ByteView(piece), &got)) return false;
    if (!assembler_.push(ConstByteView(piece).at_most(got), function_.in_max_packet)) continue;
    const ConstByteView transfer = assembler_.data();
    if (function_.protocol == UsbNetworkProtocol::Ncm) {
      ntb_open_ = ntb_.start(transfer);
      if (!ntb_open_) {
        ++stats_.frames_dropped;
        assembler_.release();
      }
      continue;
    }
    if (transfer.size() < kEthernetHeaderSize) {
      ++stats_.frames_dropped;
      assembler_.release();
      continue;
    }
    buffer.copy_from(transfer.at_most(buffer.size()));
    *length = static_cast<uint32_t>(transfer.size());
    assembler_.release();
    ++stats_.frames_received;
    return true;
  }
  return false;
}

}  // namespace hardware
