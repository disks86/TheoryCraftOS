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

#include "Hardware/NetworkInterface/Usb/Ax88179NetworkInterface.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// Vendor requests: the MAC's registers (value = register, index = how many bytes) and the PHY's (value = PHY address, index = register).
constexpr uint8_t kVendorIn = 0xC0, kVendorOut = 0x40, kAccessMac = 0x01, kAccessPhy = 0x02;
constexpr uint16_t kPhyAddress = 0x03;
// MAC registers.
constexpr uint16_t kPhysicalLinkStatus = 0x02, kRxControl = 0x0B, kNodeId = 0x10, kMediumStatusMode = 0x22, kPhyPowerResetControl = 0x26,
                   kRxBulkInQueueControl = 0x2E, kClockSelect = 0x33, kRxChecksumControl = 0x34, kTxChecksumControl = 0x35,
                   kPauseWaterLevelHigh = 0x54, kPauseWaterLevelLow = 0x55;
constexpr uint8_t kLinkStatusUsbSuper = 0x04, kLinkStatusUsbHigh = 0x02;
constexpr uint16_t kRxControlAllMulticast = 0x0002, kRxControlBroadcast = 0x0008, kRxControlPhysical = 0x0020,
                   kRxControlStart = 0x0080, kRxControlDropCrcError = 0x0100, kRxControlIpAlign = 0x0200;
constexpr uint16_t kMediumGiga = 0x0001, kMediumFullDuplex = 0x0002, kMedium125MHz = 0x0008, kMediumRxFlow = 0x0010, kMediumTxFlow = 0x0020,
                   kMediumReceiveEnable = 0x0100, kMedium100 = 0x0200;
constexpr uint16_t kPhyPowerIprl = 0x0020;
constexpr uint8_t kClockAcs = 0x01, kClockBcs = 0x02;
// PHY registers: the basic control register (MII) and the specific status one.
constexpr uint16_t kPhyBasicControl = 0x00, kPhySpecificStatus = 0x11;
constexpr uint16_t kBasicControlAutoNegotiate = 0x1000, kBasicControlRestartAutoNegotiate = 0x0200;
constexpr uint16_t kPhyStatusSpeedMask = 0xC000, kPhyStatusGiga = 0x8000, kPhyStatus100 = 0x4000, kPhyStatusFullDuplex = 0x2000, kPhyStatusLink = 0x0400;
// The bulk-in aggregation settings for each combination of link and USB speed (Linux's AX88179_BULKIN_SIZE): control, timer low/high,
// size in KiB, inter-frame gap.
constexpr uint8_t kBulkInSettings[4][5] = {{7, 0x4F, 0, 0x12, 0xFF}, {7, 0x20, 3, 0x16, 0xFF}, {7, 0xAE, 7, 0x18, 0xFF}, {7, 0xCC, 0x4C, 0x18, 8}};
constexpr uint32_t kTxHeader = 8, kTxPaddingFlag = 0x80008000u;
constexpr uint32_t kMaxBulkPacket = 1024;
constexpr uint32_t kPiecesPerPoll = 64;
constexpr uint32_t kMinFrame = 2 + kEthernetHeaderSize;        // the pad and an Ethernet header

struct UsbId { uint16_t vendor, product; };
constexpr UsbId kDevices[] = {
    {0x0B95, 0x1790},   // ASIX AX88179, AX88179A
    {0x0B95, 0x178A},   // ASIX AX88178A
    {0x04B4, 0x3610},   // Cypress GX3
    {0x2001, 0x4A00},   // D-Link DUB-1312, DUB-1332
    {0x0DF6, 0x0072},   // Sitecom LN-032
    {0x04E8, 0xA100},   // Samsung
    {0x17EF, 0x304B},   // Lenovo OneLinkDock
    {0x0930, 0x0A13},   // Toshiba
    {0x050D, 0x0128},   // Belkin
    {0x0711, 0x0179},   // Magic Control Technology
};
}  // namespace

bool Ax88179RxParser::start(ConstByteView transfer) {
  *this = Ax88179RxParser();
  if (transfer.size() < 4) {
    ++errors_;
    return false;
  }
  ByteReader trailer(transfer.last(4));
  const uint32_t word = trailer.le32();
  const uint32_t count = word & 0xFFFF, table = word >> 16;
  const ConstByteView body = transfer.first(transfer.size() - 4);
  if (uint64_t{table} + uint64_t{count} * 4 > body.size()) {   // the table must lie in front of the trailer
    ++errors_;
    return false;
  }
  frames_ = body.first(table);
  headers_ = body.subview(table, uint64_t{count} * 4);
  count_ = count;
  return true;
}

bool Ax88179RxParser::next(ConstByteView* frame) {
  while (index_ < count_) {
    ByteReader in(headers_.subview(uint64_t{index_} * 4, 4));
    const uint32_t header = in.le32();
    ++index_;
    const uint32_t length = (header >> 16) & 0x1FFF, padded = (length + 7) & ~7u;
    if (length == 0) continue;                                 // a dummy entry for alignment
    if (padded > frames_.size() - offset_) {                   // runs into the table: the rest of it cannot be trusted
      ++errors_;
      index_ = count_;
      return false;
    }
    const uint64_t at = offset_;
    offset_ += padded;
    if ((header & (kHeaderCrcError | kHeaderDropError)) || length < kMinFrame) {
      ++errors_;
      continue;
    }
    *frame = frames_.subview(at + 2, length - 2);
    return true;
  }
  return false;
}

uint32_t ax88179_build_tx(ByteView out, ConstByteView frame, uint16_t max_packet) {
  if (frame.empty() || frame.size() > kEthernetMaxFrame || max_packet == 0) return 0;
  const uint32_t length = static_cast<uint32_t>(frame.size());
  const bool whole_packets = (length + kTxHeader) % max_packet == 0;
  const uint32_t total = kTxHeader + length + (whole_packets ? 1 : 0);
  if (total > out.size()) return 0;
  ByteWriter w(out);
  w.le32(length);
  w.le32(whole_packets ? kTxPaddingFlag : 0);
  w.bytes(frame);
  if (whole_packets) w.u8(0);
  return w.ok() ? total : 0;
}

bool Ax88179NetworkInterface::supports(uint16_t vendor_id, uint16_t product_id) {
  for (const UsbId& id : kDevices)
    if (id.vendor == vendor_id && id.product == product_id) return true;
  return false;
}

bool Ax88179NetworkInterface::read_mac(uint16_t reg, ByteView out) const {
  return host_.control_in(port_, kVendorIn, kAccessMac, reg, static_cast<uint16_t>(out.size()), out);
}
bool Ax88179NetworkInterface::write_mac(uint16_t reg, ConstByteView data) const {
  return host_.control_out_data(port_, kVendorOut, kAccessMac, reg, static_cast<uint16_t>(data.size()), data);
}
bool Ax88179NetworkInterface::write_mac8(uint16_t reg, uint8_t value) const {
  const uint8_t data[1] = {value};
  return write_mac(reg, ConstByteView(data));
}
bool Ax88179NetworkInterface::write_mac16(uint16_t reg, uint16_t value) const {
  const uint8_t data[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  return write_mac(reg, ConstByteView(data));
}
bool Ax88179NetworkInterface::read_phy(uint16_t reg, uint16_t* value) const {
  uint8_t data[2] = {};
  if (!host_.control_in(port_, kVendorIn, kAccessPhy, kPhyAddress, reg, ByteView(data))) return false;
  *value = static_cast<uint16_t>(data[0] | (data[1] << 8));
  return true;
}
bool Ax88179NetworkInterface::write_phy(uint16_t reg, uint16_t value) const {
  const uint8_t data[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  return host_.control_out_data(port_, kVendorOut, kAccessPhy, kPhyAddress, reg, ConstByteView(data));
}

bool Ax88179NetworkInterface::initialize() {
  if (started_ || !endpoints_.valid() || endpoints_.in_max_packet > kMaxBulkPacket || !present()) return false;
  receive_storage_ = allocate_view<uint8_t>(allocator_, kReceiveBuffer);
  transmit_ = allocate_dma_view<uint8_t>(allocator_, kTransmitBuffer, 4096);   // (inside one 64 KiB window, as bulk_transfer wants)
  if (receive_storage_.empty() || transmit_.empty() || !host_.set_configuration(port_, endpoints_.configuration)) {
    release();
    return false;
  }

  // Power the PHY up, select the clocks, then read the address the adapter loaded from its EEPROM.
  bool ok = write_mac16(kPhyPowerResetControl, 0) && write_mac16(kPhyPowerResetControl, kPhyPowerIprl);
  delay_us(200 * 1000);
  ok = ok && write_mac8(kClockSelect, kClockAcs | kClockBcs);
  delay_us(100 * 1000);
  uint8_t mac[6] = {};
  ok = ok && read_mac(kNodeId, ByteView(mac));
  bool zero = true;
  for (uint8_t b : mac) zero = zero && b == 0;
  if (!ok || zero || (mac[0] & 1)) {
    release();
    return false;
  }
  ByteView(mac_.bytes).copy_from(ConstByteView(mac));

  // Bulk-in aggregation, flow control levels, no checksum offload, then receiving on, at gigabit until the link says otherwise.
  receive_control_ = kRxControlDropCrcError | kRxControlIpAlign | kRxControlStart | kRxControlPhysical | kRxControlBroadcast;
  ok = write_mac(kRxBulkInQueueControl, ConstByteView(kBulkInSettings[0])) && write_mac8(kPauseWaterLevelLow, 0x34) &&
       write_mac8(kPauseWaterLevelHigh, 0x52) && write_mac8(kRxChecksumControl, 0) && write_mac8(kTxChecksumControl, 0) &&
       write_mac16(kRxControl, receive_control_) &&
       write_mac16(kMediumStatusMode, kMediumReceiveEnable | kMediumTxFlow | kMediumRxFlow | kMediumFullDuplex | kMediumGiga);
  uint16_t control = 0;
  if (ok && read_phy(kPhyBasicControl, &control)) write_phy(kPhyBasicControl, control | kBasicControlAutoNegotiate | kBasicControlRestartAutoNegotiate);
  if (!ok || !host_.start_bulk_endpoint(port_, endpoints_.bulk_out, endpoints_.out_max_packet) ||
      !host_.start_bulk_in_stream(port_, endpoints_.bulk_in, endpoints_.in_max_packet)) {   // (controllers that cannot receive without waiting refuse)
    release();
    return false;
  }
  if (endpoints_.interrupt_in &&
      !host_.start_interrupt_in(port_, endpoints_.interrupt_in, endpoints_.interrupt_max_packet ? endpoints_.interrupt_max_packet : 8, endpoints_.interrupt_interval))
    endpoints_.interrupt_in = 0;
  assembler_ = UsbTransferAssembler(receive_storage_);
  started_ = true;
  kernel::ScopedMutex guard(link_mutex_);
  update_link();
  return true;
}

void Ax88179NetworkInterface::release() {
  release_view(allocator_, receive_storage_);
  release_view(allocator_, transmit_);
  assembler_ = UsbTransferAssembler();
  parser_open_ = false;
}

void Ax88179NetworkInterface::shutdown() {
  kernel::ScopedMutex receive_guard(receive_mutex_);
  kernel::ScopedMutex transmit_guard(transmit_mutex_);
  if (started_ && present()) write_mac16(kRxControl, 0);      // receiving off
  started_ = false;
  release();
}

void Ax88179NetworkInterface::update_link() const {
  uint16_t status = 0;
  if (!read_phy(kPhySpecificStatus, &status)) return;
  if (!(status & kPhyStatusLink)) {
    link_ = false;
    speed_mbps_ = 0;
    duplex_ = LinkDuplex::Unknown;
    return;
  }
  uint8_t usb[1] = {};
  read_mac(kPhysicalLinkStatus, ByteView(usb));
  const bool super = usb[0] & kLinkStatusUsbSuper, high = usb[0] & kLinkStatusUsbHigh;
  uint16_t mode = kMediumReceiveEnable | kMediumTxFlow | kMediumRxFlow;
  uint32_t settings = 3;
  const uint16_t speed = status & kPhyStatusSpeedMask;
  if (speed == kPhyStatusGiga) {
    mode |= kMediumGiga | kMedium125MHz;
    settings = super ? 0 : high ? 1 : 3;
    speed_mbps_ = 1000;
  } else if (speed == kPhyStatus100) {
    mode |= kMedium100;
    settings = super || high ? 2 : 3;
    speed_mbps_ = 100;
  } else {
    speed_mbps_ = 10;
  }
  if (status & kPhyStatusFullDuplex) mode |= kMediumFullDuplex;
  duplex_ = (status & kPhyStatusFullDuplex) ? LinkDuplex::Full : LinkDuplex::Half;
  write_mac(kRxBulkInQueueControl, ConstByteView(kBulkInSettings[settings]));
  write_mac16(kMediumStatusMode, mode);
  link_ = true;
}

// The interrupt endpoint sends 8 bytes of status; bit 16 of the first word is the link. When it changes, the PHY says at what speed.
void Ax88179NetworkInterface::poll_status() const {
  kernel::ScopedMutex guard(link_mutex_);
  if (!endpoints_.interrupt_in) {
    if (++polls_without_status_ % 64 == 0) update_link();
    return;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    uint8_t status[16] = {};
    uint16_t got = 0;
    if (!host_.poll_interrupt_in(port_, endpoints_.interrupt_in, ByteView(status), &got)) return;
    if (got < 4) continue;
    const bool link = status[2] & 1;
    if (link != link_) update_link();
  }
}

bool Ax88179NetworkInterface::link_up() const {
  if (!started_ || !present()) return false;
  poll_status();
  return link_;
}

bool Ax88179NetworkInterface::accept_all_multicast(bool enable) {
  if (!started_ || !present()) return false;
  const uint16_t control = enable ? (receive_control_ | kRxControlAllMulticast) : (receive_control_ & ~kRxControlAllMulticast);
  if (!write_mac16(kRxControl, control)) return false;
  receive_control_ = control;
  return enable;
}

bool Ax88179NetworkInterface::transmit(ConstByteView frame) {
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
  const uint32_t length = ax88179_build_tx(transmit_, payload, endpoints_.out_max_packet);
  uint32_t moved = 0;
  if (!length || !host_.bulk_transfer(port_, endpoints_.bulk_out, transmit_.first(length), &moved) || moved != length) {
    if (length) {
      host_.reset_bulk_endpoint(port_, endpoints_.bulk_out);
      host_.control_out(port_, 0x02, 0x01, 0, endpoints_.bulk_out);   // CLEAR_FEATURE(ENDPOINT_HALT)
    }
    ++stats_.send_failures;
    return false;
  }
  ++stats_.frames_sent;
  return true;
}

bool Ax88179NetworkInterface::receive(ByteView buffer, uint32_t* length) {
  kernel::ScopedMutex guard(receive_mutex_);
  if (!started_ || !length || !present()) return false;
  for (uint32_t pieces = 0; pieces <= kPiecesPerPoll;) {
    if (parser_open_) {                                        // the frames of the aggregate that came last
      ConstByteView frame;
      if (parser_.next(&frame)) {
        buffer.copy_from(frame.at_most(buffer.size()));
        *length = static_cast<uint32_t>(frame.size());
        ++stats_.frames_received;
        return true;
      }
      stats_.frames_dropped += parser_.errors();
      parser_open_ = false;
      assembler_.release();
    }
    if (pieces++ == kPiecesPerPoll) break;
    uint8_t piece[kMaxBulkPacket];
    uint16_t got = 0;
    if (!host_.poll_interrupt_in(port_, endpoints_.bulk_in, ByteView(piece), &got)) return false;
    if (!assembler_.push(ConstByteView(piece).at_most(got), endpoints_.in_max_packet)) continue;
    parser_open_ = parser_.start(assembler_.data());
    if (!parser_open_) {
      stats_.frames_dropped += parser_.errors();
      assembler_.release();
    }
  }
  return false;
}

}  // namespace hardware
