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

#include "Hardware/GameController/Usb/XboxWirelessFrames.hpp"
#include "Hardware/GameController/XboxProtocol.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace xbox_wireless {

namespace {

uint32_t finish(const ByteWriter& w) { return w.ok() ? static_cast<uint32_t>(w.position()) : 0; }

uint32_t round_up4(uint32_t n) { return (n + 3u) & ~3u; }   // (callers keep n far below 2^32)

void write_header(ByteWriter& w, uint8_t type, uint8_t subtype, uint8_t flags, const MacAddress& a1, const MacAddress& a2, const MacAddress& a3,
                  uint16_t sequence) {
  w.u8(static_cast<uint8_t>((subtype << 4) | (type << 2)));
  w.u8(flags);
  w.le16(0);                                         // duration: the radio fills it in
  a1.write(w);
  a2.write(w);
  a3.write(w);
  w.le16(static_cast<uint16_t>((sequence & 0x0FFF) << 4));
}

constexpr uint8_t kFromDs = 0x02;

}  // namespace

uint32_t encode_management(uint8_t subtype, const MacAddress& destination, const MacAddress& bssid, uint16_t sequence, ConstByteView body, ByteView out) {
  ByteWriter w(out);
  write_header(w, kTypeManagement, subtype, 0, destination, bssid, bssid, sequence);
  w.bytes(body);
  return finish(w);
}

uint32_t encode_qos_data(const MacAddress& destination, const MacAddress& bssid, uint16_t sequence, ConstByteView payload, ByteView out) {
  ByteWriter w(out);
  write_header(w, kTypeData, kSubtypeQosData, kFromDs, destination, bssid, bssid, sequence);
  w.le16(0);                                         // QoS control: TID 0, normal acknowledgement
  w.bytes(payload);
  return finish(w);
}

bool decode_frame(ConstByteView frame, bool l2_pad, Frame* out) {
  ByteReader r(frame);
  const uint8_t control = r.u8();
  (void)r.u8();                                      // flags
  r.skip(2);                                         // duration
  Frame f;
  f.type = static_cast<uint8_t>((control >> 2) & 3);
  f.subtype = static_cast<uint8_t>(control >> 4);
  f.destination = MacAddress::read(r);
  f.source = MacAddress::read(r);
  f.bssid = MacAddress::read(r);
  r.skip(2);                                         // sequence control
  if (!r.ok() || (control & 3) != 0) return false;   // (protocol version 0 only)
  if (f.type == kTypeData && (f.subtype & 0x08)) {  // a QoS subtype: two more header bytes
    r.skip(2);
    if (l2_pad) r.skip(2);
  }
  if (!r.ok()) return false;
  f.body = r.rest();
  *out = f;
  return true;
}

uint32_t encode_beacon_body(uint8_t channel, bool pairing, ByteView out) {
  ByteWriter w(out);
  w.le64(0);                                         // timestamp: the radio fills it in
  w.le16(mt::kBeaconIntervalTu);
  w.le16(xbox_table::kCapability);
  w.u8(0x00);                                        // SSID element, empty (hidden)
  w.u8(0x00);
  w.u8(0x03);                                        // DS parameter set: the channel
  w.u8(0x01);
  w.u8(channel);
  const uint64_t vendor = w.position();
  w.bytes(ConstByteView(xbox_table::kBeaconVendorElement));
  if (w.ok()) {
    const uint8_t flag = pairing ? 0x01 : 0x00;
    w.patch_bytes(vendor + xbox_table::kBeaconPairingIndex, ConstByteView(&flag, 1));
  }
  return finish(w);
}

uint32_t encode_association_response_body(uint8_t wcid, ByteView out) {
  ByteWriter w(out);
  w.le16(xbox_table::kCapability);
  w.le16(0x0000);                                    // status: success
  w.le16(static_cast<uint16_t>(0xC000 | wcid));      // association id (the two top bits are set, as 802.11 has it)
  return finish(w);
}

uint32_t encode_mcu_message(uint8_t command, uint8_t sequence, ConstByteView payload, ByteView out) {
  if (payload.size() > mt::kInfoLengthMask - 3) return 0;
  const uint32_t length = round_up4(static_cast<uint32_t>(payload.size()));
  const uint32_t info = length | (static_cast<uint32_t>(sequence & 0x0F) << mt::kMsgSequenceShift) |
                        (static_cast<uint32_t>(command & 0x7F) << mt::kMsgCommandShift) | (mt::kCpuTxPort << mt::kInfoPortShift) | mt::kMsgTypeCmd;
  ByteWriter w(out);
  w.le32(info);
  w.bytes(payload);
  w.zeros(length - payload.size());
  w.zeros(4);                                        // (the USB DMA engine wants four zero bytes after every packet)
  return finish(w);
}

uint32_t encode_txwi(uint8_t wcid, uint32_t frame_length, bool ack, ByteView out) {
  ByteWriter w(out);
  w.le16(0);                                         // flags
  w.le16(mt::kTxwiRateOfdm6);                        // OFDM, MCS 0 (6 Mbit/s)
  w.u8(ack ? mt::kTxwiAckRequest : 0);
  w.u8(wcid);
  w.le16(static_cast<uint16_t>(frame_length & 0x0FFF));   // MPDU length
  w.le32(0);                                         // IV
  w.le32(0);                                         // EIV
  w.u8(0);                                           // AID
  w.u8(0);                                           // TX stream
  w.u8(0);
  w.u8(0);                                           // packet id
  return finish(w);
}

uint32_t encode_wlan_packet(uint8_t wcid, ConstByteView frame, ByteView out) {
  if (frame.size() > 0x0FFF) return 0;
  const uint32_t length = round_up4(static_cast<uint32_t>(mt::kTxwiBytes + frame.size()));
  const uint32_t info = length | mt::kInfo80211 | mt::kInfoWiv | (mt::kQselEdca << mt::kInfoQselShift) | (mt::kWlanPort << mt::kInfoPortShift);
  ByteWriter w(out);
  w.le32(info);
  const ByteView txwi = w.reserve(mt::kTxwiBytes);
  if (!w.ok() || encode_txwi(wcid, static_cast<uint32_t>(frame.size()), wcid != 0xFF, txwi) != mt::kTxwiBytes) return 0;
  w.bytes(frame);
  w.zeros(length - mt::kTxwiBytes - frame.size());
  w.zeros(4);
  return finish(w);
}

uint32_t encode_burst_write(uint32_t register_offset, ConstByteView data, ByteView out) {
  if (data.size() % 4) return 0;
  ByteWriter w(out);
  w.le32(mt::kMcuMemmapWlan + register_offset);
  w.bytes(data);
  return finish(w);
}

uint32_t encode_switch_channel(uint8_t channel, ByteView out) {
  ByteWriter w(out);
  w.u8(channel);
  w.u8(0);                                           // not a scan
  w.u8(0);                                           // 20 MHz
  w.u8(0);
  w.le16(0x0202);                                    // chain mask: two transmit, two receive chains (MT7612U is 2x2). UNCERTAIN for this adapter
  w.u8(0xE0);                                        // extension channel: none (0xE0 + bandwidth index 0)
  w.u8(0);
  return finish(w);
}

bool decode_rx(ConstByteView packet, RxPacket* out) {
  *out = RxPacket();
  ByteReader r(packet);
  const uint32_t length = r.le32() & mt::kInfoLengthMask;
  const ConstByteView body = r.take(length);
  const uint32_t fce = r.le32();
  if (!r.ok()) return false;
  const uint32_t port = (fce >> mt::kFcePortShift) & mt::kFcePortMask;
  const uint8_t event = static_cast<uint8_t>((fce >> mt::kFceEventShift) & mt::kFceEventMask);
  if (port == mt::kCpuRxPort) {
    out->event = event;
    if (event == xbox_table::kEventButtonPress) {
      out->kind = RxKind::ButtonPress;
      return true;
    }
    if (event == xbox_table::kEventClientLost) {
      if (!body.try_get(0, &out->wcid)) return false;
      out->kind = RxKind::ClientLost;
      return true;
    }
    if (event != xbox_table::kEventPacketRx) {
      out->kind = RxKind::OtherEvent;
      return true;
    }
    // (a frame the MCU passes on: RXWI and frame, as on the WLAN port)
  } else if (port != mt::kWlanPort) {
    return false;
  }
  ByteReader w(body);
  const uint32_t rxinfo = w.le32();
  const uint32_t control = w.le32();
  w.skip(mt::kRxwiBytes - 8);
  if (!w.ok()) return false;
  const uint32_t mpdu = (control >> mt::kRxwiMpduShift) & mt::kRxwiMpduMask;
  const bool l2_pad = (rxinfo & mt::kRxInfoL2Pad) != 0;
  const uint32_t padding = l2_pad ? 2 : 0;
  if (mpdu > UINT32_MAX - padding) return false;
  const ConstByteView frame = w.take(mpdu + padding);
  if (!w.ok() || !decode_frame(frame, l2_pad, &out->frame)) return false;
  out->wcid = static_cast<uint8_t>(control & 0xFF);
  out->kind = RxKind::Frame;
  return true;
}

bool decode_gip_battery(ConstByteView message, gamepad::State* state) {
  uint8_t command = 0, length = 0, value = 0;
  if (!message.try_get(0, &command) || command != xbox::kGipStatus || !message.try_get(3, &length) || length < 1 || !message.try_get(4, &value))
    return false;
  const uint8_t kind = (value >> 2) & 3, level = value & 3;
  switch (kind) {
    case 0: state->battery_kind = gamepad::BatteryKind::Wired; break;          // (the cable is in, or no battery)
    case 1: state->battery_kind = gamepad::BatteryKind::Alkaline; break;
    default: state->battery_kind = gamepad::BatteryKind::Rechargeable; break;  // the play-and-charge kit, a built-in pack
  }
  static constexpr uint8_t kPercent[4] = {5, 33, 66, 100};                     // critical, low, medium, full
  state->battery_percent = kind == 0 ? 0 : kPercent[level];
  state->charging = false;                                                     // (not in this message)
  return true;
}

uint8_t choose_channel(ArrayView<const ChannelSample> samples) {
  uint8_t best = 0;
  uint64_t best_busy = 0, best_total = 0;
  for (const ChannelSample& s : samples) {
    if (s.channel == 0) continue;
    const uint64_t total = uint64_t{s.busy} + s.idle;
    // busy/total < best_busy/best_total, by cross-multiplying (both fit in 64 bits); an unmeasured channel counts as busy 0 of 1.
    const uint64_t t = total ? total : 1, bt = best_total ? best_total : 1;
    if (best == 0 || uint64_t{s.busy} * bt < best_busy * t) {
      best = s.channel;
      best_busy = s.busy;
      best_total = total;
    }
  }
  return best;
}

}  // namespace xbox_wireless
}  // namespace hardware
