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

#include "Hardware/Bluetooth/HciPackets.hpp"

namespace hardware {
namespace hci {

bool build_command(uint16_t opcode, ConstByteView params, ByteWriter& out) {
  if (params.size() > kMaxCommandParams) return false;
  out.le16(opcode);
  out.u8(static_cast<uint8_t>(params.size()));
  out.bytes(params);
  return out.ok();
}

bool parse_event(ConstByteView packet, Event* out) {
  ByteReader in(packet);
  const uint8_t code = in.u8();
  const uint8_t length = in.u8();
  const ConstByteView params = in.take(length);
  if (!in.ok()) return false;
  out->code = code;
  out->params = params;
  return true;
}

bool parse_command_complete(const Event& event, CommandComplete* out) {
  if (event.code != kEvCommandComplete) return false;
  ByteReader in(event.params);
  out->packets = in.u8();
  out->opcode = in.le16();
  out->returned = in.take(in.remaining());
  return in.ok();
}

bool parse_command_status(const Event& event, CommandStatus* out) {
  if (event.code != kEvCommandStatus) return false;
  ByteReader in(event.params);
  out->status = in.u8();
  out->packets = in.u8();
  out->opcode = in.le16();
  return in.ok();
}

bool build_acl(uint16_t handle, uint8_t boundary, ConstByteView data, ByteWriter& out) {
  if (handle > 0x0FFF || boundary > 3 || data.size() > kMaxAclData) return false;
  out.le16(static_cast<uint16_t>(handle | (uint16_t{boundary} << 12)));
  out.le16(static_cast<uint16_t>(data.size()));
  out.bytes(data);
  return out.ok();
}

bool parse_acl(ConstByteView packet, AclPacket* out) {
  ByteReader in(packet);
  const uint16_t first = in.le16();
  const uint16_t length = in.le16();
  const ConstByteView data = in.take(length);
  if (!in.ok()) return false;
  out->handle = first & 0x0FFF;
  out->boundary = (first >> 12) & 3;
  out->broadcast = (first >> 14) & 3;
  out->data = data;
  return true;
}

static bool read_address(ByteReader& in, BdAddr* out) { return in.read_into(ByteView(out->b, 6)); }

bool parse_connection_complete(const Event& event, ConnectionComplete* out) {
  if (event.code != kEvConnectionComplete) return false;
  ByteReader in(event.params);
  out->status = in.u8();
  out->handle = in.le16() & 0x0FFF;
  read_address(in, &out->address);
  out->link_type = in.u8();
  out->encryption = in.u8();
  return in.ok();
}

bool parse_connection_request(const Event& event, ConnectionRequest* out) {
  if (event.code != kEvConnectionRequest) return false;
  ByteReader in(event.params);
  read_address(in, &out->address);
  out->class_of_device = in.le16() | (uint32_t{in.u8()} << 16);
  out->link_type = in.u8();
  return in.ok();
}

bool parse_disconnection_complete(const Event& event, DisconnectionComplete* out) {
  if (event.code != kEvDisconnectionComplete) return false;
  ByteReader in(event.params);
  out->status = in.u8();
  out->handle = in.le16() & 0x0FFF;
  out->reason = in.u8();
  return in.ok();
}

// The fields of an inquiry result event are grouped by kind (all the addresses, then all the scan repetition modes, ...), not result by result.
int parse_inquiry_results(const Event& event, ArrayView<InquiryResult> out) {
  ByteReader in(event.params);
  if (event.code == kEvExtendedInquiryResult) {
    const uint8_t count = in.u8();
    if (!in.ok() || count != 1 || out.empty()) return in.ok() && count == 1 ? 0 : -1;
    InquiryResult r;
    read_address(in, &r.address);
    in.skip(2);                                    // page scan repetition mode, reserved
    r.class_of_device = in.le16() | (uint32_t{in.u8()} << 16);
    r.clock_offset = in.le16();
    r.rssi = static_cast<int8_t>(in.u8());
    r.eir = in.take(in.remaining() < 240 ? in.remaining() : 240);
    if (!in.ok()) return -1;
    out[0] = r;
    return 1;
  }
  const bool with_rssi = event.code == kEvInquiryResultWithRssi;
  if (!with_rssi && event.code != kEvInquiryResult) return -1;
  const uint32_t count = in.u8();
  if (!in.ok() || in.remaining() < uint64_t{count} * 14) return -1;
  const ConstByteView block = in.take(uint64_t{count} * 14);
  // address[n*6], page scan repetition mode[n], then reserved: two bytes each for the plain format, one for the one with a signal strength, class of device[n*3], clock offset[n*2], rssi[n]
  const uint32_t cod_at = count * (with_rssi ? 8u : 9u), clock_at = cod_at + count * 3, rssi_at = clock_at + count * 2;
  int stored = 0;
  for (uint32_t i = 0; i < count && static_cast<uint64_t>(stored) < out.size(); ++i) {
    InquiryResult r;
    for (uint32_t k = 0; k < 6; ++k) r.address.b[k] = block[i * 6 + k];
    r.class_of_device = block[cod_at + i * 3] | (uint32_t{block[cod_at + i * 3 + 1]} << 8) | (uint32_t{block[cod_at + i * 3 + 2]} << 16);
    r.clock_offset = static_cast<uint16_t>((block[clock_at + i * 2] | (block[clock_at + i * 2 + 1] << 8)) & 0x7FFF);
    if (with_rssi) r.rssi = static_cast<int8_t>(block[rssi_at + i]);
    out[stored++] = r;
  }
  return stored;
}

// LE meta event: subevent (1), number of reports (1), then the reports one after the other (type, address type, address, data length, data, rssi).
int parse_advertising_reports(const Event& event, ArrayView<AdvertisingReport> out) {
  if (event.code != kEvLeMeta) return -1;
  ByteReader in(event.params);
  if (in.u8() != kLeAdvertisingReport) return -1;
  const uint8_t count = in.u8();
  if (!in.ok()) return -1;
  int stored = 0;
  for (uint32_t i = 0; i < count; ++i) {
    AdvertisingReport r;
    r.event_type = in.u8();
    r.address_type = in.u8();
    read_address(in, &r.address);
    const uint8_t length = in.u8();
    if (length > 31) return -1;
    r.data = in.take(length);
    r.rssi = static_cast<int8_t>(in.u8());
    if (!in.ok()) return -1;
    if (static_cast<uint64_t>(stored) < out.size()) out[stored++] = r;
  }
  return stored;
}

bool parse_le_connection_complete(const Event& event, LeConnectionComplete* out) {
  if (event.code != kEvLeMeta) return false;
  ByteReader in(event.params);
  const uint8_t sub = in.u8();
  if (sub != kLeConnectionComplete && sub != kLeEnhancedConnectionComplete) return false;
  out->status = in.u8();
  out->handle = in.le16() & 0x0FFF;
  out->role = in.u8();
  out->peer_address_type = in.u8();
  read_address(in, &out->peer);
  if (sub == kLeEnhancedConnectionComplete) in.skip(12);       // the local and the peer's resolvable private addresses
  out->interval = in.le16();
  out->latency = in.le16();
  out->timeout = in.le16();
  return in.ok();
}

ConstByteView find_ad_structure(ConstByteView data, uint8_t type) {
  ByteReader in(data);
  while (in.remaining() >= 2) {
    const uint8_t length = in.u8();
    if (length == 0) break;
    const ConstByteView body = in.take(length);
    if (!in.ok()) break;
    if (body[0] == type) return body.dropped(1);
  }
  return ConstByteView();
}

}  // namespace hci
}  // namespace hardware
