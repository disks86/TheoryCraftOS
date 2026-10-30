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

#include "Hardware/Bluetooth/HciController.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace hci;

namespace {
constexpr uint8_t kPbStart = 0x02, kPbContinue = 0x01;
constexpr uint32_t kMaxDrain = 64;

void copy_text(char* out, uint32_t capacity, ConstByteView text) {
  uint32_t n = 0;
  for (; n < text.size() && n + 1 < capacity && text[n] != 0; ++n) out[n] = static_cast<char>(text[n]);
  out[n] = 0;
}
}

// ------------------------------------------------------------------ commands

HciResult HciController::command(uint16_t opcode, ConstByteView params, uint32_t timeout_ms) {
  if (!transport_.present()) return HciResult::Transport;
  uint8_t packet[3 + kMaxCommandParams];
  ByteWriter out{ByteView(packet)};
  if (!build_command(opcode, params, out)) return HciResult::NoRoom;
  waiting_ = true;
  answered_ = false;
  waiting_opcode_ = opcode;
  answer_ = HciResult::Timeout;
  reply_size_ = 0;
  status_ = 0;
  if (!transport_.send_command(out.written())) {
    waiting_ = false;
    return HciResult::Transport;
  }
  const uint64_t limit = uint64_t{timeout_ms} * 10;                  // (100 microseconds a round)
  for (uint64_t round = 0; round < limit && !answered_; ++round) {
    uint16_t length = 0;
    uint8_t event_bytes[2 + kMaxEventParams];
    if (transport_.poll_event(ByteView(event_bytes), &length)) {
      Event event;
      if (parse_event(ConstByteView(event_bytes, length), &event)) {
        ++events_seen_;
        handle_event(event);
      } else {
        ++malformed_;
      }
      continue;
    }
    if (!transport_.present()) { waiting_ = false; return HciResult::Transport; }
    delay_us(100);
  }
  waiting_ = false;
  return answered_ ? answer_ : HciResult::Timeout;
}

// ------------------------------------------------------------------ running

uint32_t HciController::poll() {
  uint32_t count = 0;
  for (uint32_t i = 0; i < kMaxDrain; ++i) {
    uint8_t buffer[2 + kMaxEventParams];
    uint16_t length = 0;
    if (!transport_.poll_event(ByteView(buffer), &length)) break;
    Event event;
    ++count;
    if (parse_event(ConstByteView(buffer, length), &event)) {
      ++events_seen_;
      handle_event(event);
    } else {
      ++malformed_;
    }
  }
  for (uint32_t i = 0; i < kMaxDrain; ++i) {
    uint8_t buffer[4 + kMaxAclData + 8];
    uint16_t length = 0;
    if (!transport_.poll_acl(ByteView(buffer), &length)) break;
    ++count;
    AclPacket packet;
    if (!parse_acl(ConstByteView(buffer, length), &packet)) { ++malformed_; continue; }
    ++acl_received_;
    const HciLink* l = link(packet.handle);
    if (l && acl_handler_) acl_handler_(acl_context_, *l, packet.boundary, packet.data);
  }
  ++ticks_;
  process_incoming();
  return count;
}

bool HciController::wait_for(bool (*done)(void*), void* context, uint32_t timeout_ms) {
  const uint64_t limit = uint64_t{timeout_ms} * 10;
  for (uint64_t round = 0; round < limit; ++round) {
    if (done(context)) return true;
    if (poll() == 0) delay_us(100);
  }
  return done(context);
}

void HciController::process_incoming() {
  if (waiting_) return;
  for (Incoming& request : incoming_) {
    if (!request.used) continue;
    request.used = false;
    uint8_t params[7];
    ByteWriter out{ByteView(params)};
    out.bytes(ConstByteView(request.address.b, 6));
    if (accept_incoming_) {
      out.u8(1);                                                   // stay the peripheral of the link
      command(kOpAcceptConnection, out.written(), 1000);
    } else {
      out.u8(0x0F);                                                // rejected: unacceptable address
      ++incoming_refused_;
      command(kOpRejectConnection, out.written(), 1000);
    }
  }
}

// ------------------------------------------------------------------ events

void HciController::handle_event(const Event& event) {
  if (hook_ && hook_(hook_context_, event)) return;
  switch (event.code) {
    case kEvCommandComplete: {
      CommandComplete c;
      if (!parse_command_complete(event, &c)) { ++malformed_; return; }
      if (waiting_ && c.opcode == waiting_opcode_) {
        const uint32_t n = c.returned.size() < sizeof reply_ ? static_cast<uint32_t>(c.returned.size()) : sizeof reply_;
        for (uint32_t i = 0; i < n; ++i) reply_[i] = c.returned[i];
        reply_size_ = n;
        status_ = n ? reply_[0] : 0;
        answer_ = status_ == 0 ? HciResult::Ok : HciResult::Status;
        answered_ = true;
      }
      break;
    }
    case kEvCommandStatus: {
      CommandStatus s;
      if (!parse_command_status(event, &s)) { ++malformed_; return; }
      if (waiting_ && s.opcode == waiting_opcode_) {
        status_ = s.status;
        reply_size_ = 0;
        answer_ = s.status == 0 ? HciResult::Ok : HciResult::Status;
        answered_ = true;
      }
      break;
    }
    case kEvInquiryComplete:
      inquiring_ = false;
      break;
    case kEvInquiryResult:
    case kEvInquiryResultWithRssi:
    case kEvExtendedInquiryResult: {
      InquiryResult results[16];
      const int n = parse_inquiry_results(event, ArrayView<InquiryResult>(results));
      if (n < 0) { ++malformed_; return; }
      for (int i = 0; i < n; ++i) note_device(results[i].address, false, 0, results[i].class_of_device, results[i].rssi, results[i].eir, 0);
      break;
    }
    case kEvRemoteNameRequestComplete: {
      ByteReader in(event.params);
      const uint8_t status = in.u8();
      BdAddr address;
      in.read_into(ByteView(address.b, 6));
      const ConstByteView name = in.take(in.remaining());
      if (!in.ok()) { ++malformed_; return; }
      for (uint32_t i = 0; i < device_count_; ++i)
        if (status == 0 && !devices_[i].le && devices_[i].address == address) copy_text(devices_[i].name, sizeof devices_[i].name, name);
      break;
    }
    case kEvConnectionComplete: {
      ConnectionComplete c;
      if (!parse_connection_complete(event, &c)) { ++malformed_; return; }
      if (c.status == 0) add_link(c.handle, c.address, false, 0, true);
      break;
    }
    case kEvConnectionRequest: {
      ConnectionRequest r;
      if (!parse_connection_request(event, &r)) { ++malformed_; return; }
      for (Incoming& slot : incoming_)
        if (!slot.used) { slot.used = true; slot.address = r.address; break; }
      break;
    }
    case kEvDisconnectionComplete: {
      DisconnectionComplete d;
      if (!parse_disconnection_complete(event, &d)) { ++malformed_; return; }
      if (d.status == 0) remove_link(d.handle);
      break;
    }
    case kEvNumberOfCompletedPackets: {
      ByteReader in(event.params);
      const uint8_t count = in.u8();
      uint16_t handles[16], counts[16];
      if (!in.ok() || count > 16 || in.remaining() < uint64_t{count} * 4) { ++malformed_; return; }
      for (uint32_t i = 0; i < count; ++i) handles[i] = in.le16() & 0x0FFF;
      for (uint32_t i = 0; i < count; ++i) counts[i] = in.le16();
      for (uint32_t i = 0; i < count; ++i) give_credits(handles[i], counts[i]);
      break;
    }
    case kEvHardwareError:
      ++hardware_errors_;
      break;
    case kEvLeMeta:
      handle_le_meta(event);
      break;
    default:
      break;
  }
}

void HciController::handle_le_meta(const Event& event) {
  if (event.params.empty()) { ++malformed_; return; }
  switch (event.params[0]) {
    case kLeAdvertisingReport: {
      AdvertisingReport reports[8];
      const int n = parse_advertising_reports(event, ArrayView<AdvertisingReport>(reports));
      if (n < 0) { ++malformed_; return; }
      for (int i = 0; i < n; ++i) note_device(reports[i].address, true, reports[i].address_type, 0, reports[i].rssi, reports[i].data, reports[i].event_type);
      break;
    }
    case kLeConnectionComplete:
    case kLeEnhancedConnectionComplete: {
      LeConnectionComplete c;
      if (!parse_le_connection_complete(event, &c)) { ++malformed_; return; }
      if (c.status == 0) add_link(c.handle, c.peer, true, c.peer_address_type, c.role == 0);
      break;
    }
    default:
      break;
  }
}

void HciController::note_device(const BdAddr& address, bool le, uint8_t address_type, uint32_t class_of_device, int8_t rssi, ConstByteView data, uint8_t advertising_type) {
  HciDevice* d = nullptr;
  for (uint32_t i = 0; i < device_count_ && !d; ++i)
    if (devices_[i].le == le && devices_[i].address == address && (!le || devices_[i].address_type == address_type)) d = &devices_[i];
  if (!d) {
    if (device_count_ == kMaxDevices) return;                    // (the table is full: the strongest are not chosen, the first stay)
    d = &devices_[device_count_++];
    *d = HciDevice();
    d->address = address;
    d->le = le;
    d->address_type = address_type;
  }
  d->last_seen = ticks_;
  if (rssi != 127) d->rssi = rssi;
  if (class_of_device) d->class_of_device = class_of_device;
  ConstByteView name = find_ad_structure(data, kAdName);
  if (name.empty()) name = find_ad_structure(data, kAdShortName);
  if (!name.empty()) copy_text(d->name, sizeof d->name, name);
  const ConstByteView appearance = find_ad_structure(data, kAdAppearance);
  if (appearance.size() >= 2) d->appearance = static_cast<uint16_t>(appearance[0] | (appearance[1] << 8));
  if (le && advertising_type == 0) d->connectable = true;
}

// ------------------------------------------------------------------ links

HciLink* HciController::add_link(uint16_t handle, const BdAddr& address, bool le, uint8_t address_type, bool central) {
  HciLink* slot = nullptr;
  for (HciLink& l : links_) {
    if (l.used && l.handle == handle) { slot = &l; break; }
    if (!l.used && !slot) slot = &l;
  }
  if (!slot) return nullptr;
  slot->used = true;
  slot->handle = handle;
  slot->address = address;
  slot->le = le;
  slot->address_type = address_type;
  slot->central = central;
  return slot;
}

void HciController::remove_link(uint16_t handle) {
  for (HciLink& l : links_)
    if (l.used && l.handle == handle) l.used = false;
}

const HciLink* HciController::link(uint16_t handle) const {
  for (const HciLink& l : links_)
    if (l.used && l.handle == handle) return &l;
  return nullptr;
}

const HciLink* HciController::find_link(const BdAddr& address) const {
  for (const HciLink& l : links_)
    if (l.used && l.address == address) return &l;
  return nullptr;
}

uint32_t HciController::link_count() const {
  uint32_t n = 0;
  for (const HciLink& l : links_) n += l.used;
  return n;
}

uint16_t* HciController::credits_for(const HciLink& l) { return l.le && info_.le_packets != 0 ? &le_credits_ : &acl_credits_; }

void HciController::give_credits(uint16_t handle, uint16_t count) {
  const HciLink* l = link(handle);
  if (!l) return;
  uint16_t* credits = credits_for(*l);
  const uint16_t limit = l->le && info_.le_packets != 0 ? info_.le_packets : info_.acl_packets;
  *credits = static_cast<uint16_t>(count > limit - *credits ? limit : *credits + count);       // (never more than the controller has)
}

HciResult HciController::connect(const BdAddr& address) {
  uint8_t params[13];
  ByteWriter out{ByteView(params)};
  out.bytes(ConstByteView(address.b, 6));
  out.le16(0xCC18);                                  // DM1 DH1 DM3 DH3 DM5 DH5
  out.u8(2);                                         // page scan repetition mode R2
  out.u8(0);
  out.le16(0);
  out.u8(1);                                         // a role switch is fine
  return command(kOpCreateConnection, out.written());
}

HciResult HciController::connect_le(const BdAddr& address, uint8_t address_type) {
  uint8_t params[25];
  ByteWriter out{ByteView(params)};
  out.le16(0x0060);                                  // scan interval (60 ms) and window (30 ms)
  out.le16(0x0030);
  out.u8(0);                                         // no accept list: connect to this address
  out.u8(address_type);
  out.bytes(ConstByteView(address.b, 6));
  out.u8(0);                                         // our address is the public one
  out.le16(0x0018);                                  // connection interval 30 to 50 ms
  out.le16(0x0028);
  out.le16(0);                                       // latency
  out.le16(0x00C8);                                  // supervision timeout 2 s
  out.le16(0);
  out.le16(0);
  return command(kOpLeCreateConnection, out.written());
}

HciResult HciController::disconnect(uint16_t handle, uint8_t reason) {
  uint8_t params[3] = {static_cast<uint8_t>(handle), static_cast<uint8_t>(handle >> 8), reason};
  return command(kOpDisconnect, ConstByteView(params, 3));
}

HciResult HciController::send_acl(uint16_t handle, ConstByteView data, uint32_t timeout_ms) {
  const HciLink* l = link(handle);
  if (!l || data.empty()) return HciResult::Malformed;
  const bool own_buffers = l->le && info_.le_packets != 0;
  const uint32_t mtu = own_buffers ? info_.le_mtu : info_.acl_mtu;
  if (mtu == 0) return HciResult::NoRoom;
  uint16_t* credits = credits_for(*l);
  uint64_t sent = 0;
  while (sent < data.size()) {
    const uint64_t rest = data.size() - sent;
    const uint32_t chunk = static_cast<uint32_t>(rest < mtu ? rest : mtu);
    const uint64_t limit = uint64_t{timeout_ms} * 10;
    uint64_t round = 0;
    while (*credits == 0 && round < limit) {                       // (the controller has no free buffer: wait for it to say so)
      if (poll() == 0) delay_us(100);
      ++round;
      if (!link(handle)) return HciResult::Transport;
    }
    if (*credits == 0) return HciResult::Timeout;
    uint8_t packet[4 + kMaxAclData];
    ByteWriter out{ByteView(packet)};
    if (!build_acl(handle, sent == 0 ? kPbStart : kPbContinue, data.subview(sent, chunk), out)) return HciResult::NoRoom;
    --*credits;
    if (!transport_.send_acl(out.written())) return HciResult::Transport;
    sent += chunk;
  }
  return HciResult::Ok;
}

// ------------------------------------------------------------------ finding

const HciDevice* HciController::find_device(const BdAddr& address) const {
  for (uint32_t i = 0; i < device_count_; ++i)
    if (devices_[i].address == address) return &devices_[i];
  return nullptr;
}

HciResult HciController::start_inquiry(uint8_t length) {
  if (inquiring_) return HciResult::Ok;
  const uint8_t params[5] = {0x33, 0x8B, 0x9E, length, 0};         // the general inquiry access code, no limit on the answers
  const HciResult r = command(kOpInquiry, ConstByteView(params, 5));
  inquiring_ = r == HciResult::Ok;
  return r;
}

HciResult HciController::cancel_inquiry() {
  const HciResult r = command(kOpInquiryCancel);
  if (r == HciResult::Ok) inquiring_ = false;
  return r;
}

HciResult HciController::start_le_scan(bool active) {
  if (le_scanning_) return HciResult::Ok;
  if (!info_.le_supported) return HciResult::Status;
  uint8_t params[7];
  ByteWriter out{ByteView(params)};
  out.u8(active ? 1 : 0);
  out.le16(0x0060);
  out.le16(0x0030);
  out.u8(0);                                                       // public address, accept every advertiser
  out.u8(0);
  HciResult r = command(kOpLeSetScanParameters, out.written());
  if (r != HciResult::Ok) return r;
  const uint8_t enable[2] = {1, 0};
  r = command(kOpLeSetScanEnable, ConstByteView(enable, 2));
  le_scanning_ = r == HciResult::Ok;
  return r;
}

HciResult HciController::stop_le_scan() {
  const uint8_t enable[2] = {0, 0};
  const HciResult r = command(kOpLeSetScanEnable, ConstByteView(enable, 2));
  if (r == HciResult::Ok) le_scanning_ = false;
  return r;
}

HciResult HciController::request_name(const BdAddr& address) {
  uint8_t params[10];
  ByteWriter out{ByteView(params)};
  out.bytes(ConstByteView(address.b, 6));
  out.u8(2);
  out.u8(0);
  out.le16(0);
  return command(kOpRemoteNameRequest, out.written());
}

// ------------------------------------------------------------------ bring-up

HciResult HciController::initialize() {
  initialized_ = false;
  info_ = HciInfo();
  device_count_ = 0;
  for (HciLink& l : links_) l.used = false;
  HciResult r = command(kOpReset, ConstByteView(), 5000);
  if (r != HciResult::Ok) return r;
  inquiring_ = le_scanning_ = false;
  r = command(kOpReadLocalVersion);
  if (r != HciResult::Ok) return r;
  {
    ByteReader in(reply());
    in.skip(1);
    info_.hci_version = in.u8();
    info_.hci_revision = in.le16();
    info_.lmp_version = in.u8();
    info_.manufacturer = in.le16();
    info_.lmp_subversion = in.le16();
    if (!in.ok()) return HciResult::Malformed;
  }
  r = command(kOpReadBdAddr);
  if (r != HciResult::Ok) return r;
  {
    ByteReader in(reply());
    in.skip(1);
    in.read_into(ByteView(info_.address.b, 6));
    if (!in.ok()) return HciResult::Malformed;
  }
  r = command(kOpReadBufferSize);
  if (r != HciResult::Ok) return r;
  {
    ByteReader in(reply());
    in.skip(1);
    info_.acl_mtu = in.le16();
    info_.sco_mtu = in.u8();
    info_.acl_packets = in.le16();
    info_.sco_packets = in.le16();
    if (!in.ok() || info_.acl_mtu == 0 || info_.acl_packets == 0) return HciResult::Malformed;
  }
  r = command(kOpReadLocalFeatures);
  if (r == HciResult::Ok) {
    ByteReader in(reply());
    in.skip(1);
    in.read_into(ByteView(info_.features));
  }
  info_.le_supported = (info_.features[4] & 0x40) != 0;
  info_.simple_pairing = (info_.features[6] & 0x08) != 0;
  const uint8_t event_mask[8] = {0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D};
  command(kOpSetEventMask, ConstByteView(event_mask, 8));
  const uint8_t extended = 2;
  command(kOpWriteInquiryMode, ConstByteView(&extended, 1));      // (inquiry results with the signal strength and the name when the controller can)
  if (info_.simple_pairing) {
    const uint8_t on = 1;
    command(kOpWriteSimplePairingMode, ConstByteView(&on, 1));
  }
  if (info_.le_supported) {
    r = command(kOpLeReadBufferSize);
    if (r == HciResult::Ok) {
      ByteReader in(reply());
      in.skip(1);
      info_.le_mtu = in.le16();
      info_.le_packets = in.u8();
    }
    r = command(kOpLeReadLocalFeatures);
    if (r == HciResult::Ok) {
      ByteReader in(reply());
      in.skip(1);
      in.read_into(ByteView(info_.le_features));
    }
    const uint8_t host[2] = {1, 0};
    command(kOpWriteLeHostSupported, ConstByteView(host, 2));
    const uint8_t le_mask[8] = {0x1F, 0, 0, 0, 0, 0, 0, 0};          // connection complete, advertising reports, update complete, remote features, long term key request
    command(kOpLeSetEventMask, ConstByteView(le_mask, 8));
  }
  const uint8_t not_discoverable = 0;
  command(kOpWriteScanEnable, ConstByteView(&not_discoverable, 1)); // (the machine neither answers inquiries nor accepts connections until it is told to)
  acl_credits_ = info_.acl_packets;
  le_credits_ = info_.le_packets;
  initialized_ = true;
  return HciResult::Ok;
}

}  // namespace hardware
