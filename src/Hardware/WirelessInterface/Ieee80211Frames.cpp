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


#include "Hardware/WirelessInterface/Ieee80211Frames.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace ieee80211 {

namespace {
void header(ByteWriter& w, uint16_t frame_control, const uint8_t bssid[6], const uint8_t sa[6]) {
  w.le16(frame_control);
  w.le16(0);                                               // duration: the hardware fills it
  for (uint32_t i = 0; i < 6; ++i) w.u8(bssid[i]);          // destination
  for (uint32_t i = 0; i < 6; ++i) w.u8(sa[i]);
  for (uint32_t i = 0; i < 6; ++i) w.u8(bssid[i]);
  w.le16(0);                                               // sequence control: the hardware numbers management frames
}
bool equal6(const uint8_t* a, const uint8_t* b) {
  for (uint32_t i = 0; i < 6; ++i)
    if (a[i] != b[i]) return false;
  return true;
}
}  // namespace

uint32_t build_authentication(const uint8_t bssid[6], const uint8_t sa[6], ByteView out) {
  ByteWriter w(out);
  header(w, kSubtypeAuthentication, bssid, sa);
  w.le16(0);                                               // open system
  w.le16(1);                                               // first frame of the exchange
  w.le16(kStatusSuccess);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t build_association_request(const uint8_t bssid[6], const uint8_t sa[6], ConstByteView ssid, bool five_ghz, ConstByteView rsn_ie, ByteView out) {
  if (ssid.size() == 0 || ssid.size() > 32 || rsn_ie.size() > 64) return 0;
  ByteWriter w(out);
  header(w, kSubtypeAssocRequest, bssid, sa);
  w.le16(kCapabilityEss | (rsn_ie.size() ? kCapabilityPrivacy : 0));
  w.le16(10);                                              // listen interval
  w.u8(0); w.u8(static_cast<uint8_t>(ssid.size())); w.bytes(ssid);
  if (five_ghz) {
    const uint8_t rates[] = {1, 8, 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};          // 6 (basic) 9 12 (basic) 18 24 (basic) 36 48 54
    w.bytes(ConstByteView(rates, sizeof rates));
  } else {
    const uint8_t rates[] = {1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24,            // 1 2 5.5 11 (basic) 6 9 12 18
                             50, 4, 0x30, 0x48, 0x60, 0x6C};                                   // extended: 24 36 48 54
    w.bytes(ConstByteView(rates, sizeof rates));
  }
  if (rsn_ie.size()) w.bytes(rsn_ie);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t build_deauthentication(const uint8_t bssid[6], const uint8_t sa[6], uint16_t reason, ByteView out) {
  ByteWriter w(out);
  header(w, kSubtypeDeauthentication, bssid, sa);
  w.le16(reason);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

uint32_t build_eapol_data(const uint8_t bssid[6], const uint8_t sa[6], ConstByteView eapol, ByteView out) {
  ByteWriter w(out);
  w.le16(0x0108);                                          // data, to the distribution system
  w.le16(0);
  for (uint32_t i = 0; i < 6; ++i) w.u8(bssid[i]);          // receiver: the access point
  for (uint32_t i = 0; i < 6; ++i) w.u8(sa[i]);             // transmitter
  for (uint32_t i = 0; i < 6; ++i) w.u8(bssid[i]);          // destination: the authenticator
  w.le16(0);
  const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
  w.bytes(ConstByteView(snap, sizeof snap));
  w.bytes(eapol);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

bool parse_management(ConstByteView frame, const uint8_t bssid[6], const uint8_t our_address[6], MgmtFrame* out) {
  ByteReader r(frame);
  const uint16_t control = r.le16();
  r.skip(2);
  uint8_t a1[6], a2[6], a3[6];
  r.read_into(ByteView(a1));
  r.read_into(ByteView(a2));
  r.read_into(ByteView(a3));
  r.skip(2);
  if (!r.ok() || (control & 0x000C) != 0 || (control & 0x0003) != 0) return false;           // management frames only
  if (!equal6(a2, bssid) || !equal6(a3, bssid)) return false;                                  // from the access point we talk to
  if (!equal6(a1, our_address)) return false;                                                  // to us (a broadcast deauthentication is not trusted here)
  MgmtFrame m;
  m.subtype = control & 0x00F0;
  switch (m.subtype) {
    case kSubtypeAuthentication:
      m.algorithm = r.le16();
      m.sequence = r.le16();
      m.status = r.le16();
      break;
    case kSubtypeAssocResponse:
      r.skip(2);
      m.status = r.le16();
      m.association_id = r.le16() & 0x3FFF;
      break;
    case kSubtypeDeauthentication:
    case kSubtypeDisassociation:
      m.reason = r.le16();
      break;
    default:
      return false;
  }
  if (!r.ok()) return false;
  *out = m;
  return true;
}

ConstByteView parse_eapol_data(ConstByteView frame, const uint8_t bssid[6], const uint8_t our_address[6]) {
  ByteReader r(frame);
  const uint16_t control = r.le16();
  r.skip(2);
  uint8_t a1[6], a2[6];
  r.read_into(ByteView(a1));
  r.read_into(ByteView(a2));
  r.skip(6 + 2);                                           // address 3, sequence control
  if (!r.ok()) return ConstByteView();
  const uint16_t type = control & 0x000C, subtype = control & 0x00F0;
  const bool from_ds = (control & 0x0300) == 0x0200;
  if ((control & 0x0003) != 0 || type != 0x0008 || !from_ds || (control & 0x4000)) return ConstByteView();      // data from the AP, not protected
  if (!equal6(a1, our_address) || !equal6(a2, bssid)) return ConstByteView();
  if (subtype == 0x80) {                                   // QoS data: two bytes of QoS control, and four of HT control when the order bit says so
    r.skip(2);
    if (control & 0x8000) r.skip(4);
  } else if (subtype != 0x00) {
    return ConstByteView();
  }
  ConstByteView snap = r.take(8);
  if (!r.ok() || snap[0] != 0xAA || snap[1] != 0xAA || snap[2] != 0x03 || snap[3] != 0 || snap[4] != 0 || snap[5] != 0 || snap[6] != 0x88 || snap[7] != 0x8E) return ConstByteView();
  return r.rest();
}

ConstByteView find_rsn_element(ConstByteView frame) {
  ByteReader r(frame);
  r.skip(kMgmtHeaderBytes + 8 + 2 + 2);                    // header, timestamp, interval, capability
  while (r.remaining() >= 2) {
    const uint8_t id = r.u8();
    const uint8_t length = r.u8();
    ConstByteView body = r.take(length);
    if (!r.ok()) return ConstByteView();
    if (id == 48) return frame.subview(r.position() - length - 2, length + 2u);
    (void)body;
  }
  return ConstByteView();
}

uint32_t negotiate_rsn(ConstByteView element, ByteView out, const char** why) {
  const char* local = nullptr;
  if (!why) why = &local;
  ByteReader r(element);
  if (r.u8() != 48) { *why = "no RSN element"; return 0; }
  const uint8_t length = r.u8();
  ByteReader b(r.take(length));
  if (!r.ok() || b.le16() != 1) { *why = "bad RSN element"; return 0; }
  ConstByteView group = b.take(4);
  const uint16_t pairwise_count = b.le16();
  bool ccmp = false;
  for (uint16_t i = 0; i < pairwise_count && b.ok(); ++i) {
    ConstByteView s = b.take(4);
    if (b.ok() && s[0] == 0x00 && s[1] == 0x0F && s[2] == 0xAC && s[3] == 4) ccmp = true;
  }
  const uint16_t akm_count = b.le16();
  bool psk = false;
  for (uint16_t i = 0; i < akm_count && b.ok(); ++i) {
    ConstByteView s = b.take(4);
    if (b.ok() && s[0] == 0x00 && s[1] == 0x0F && s[2] == 0xAC && s[3] == 2) psk = true;
  }
  uint16_t capabilities = 0;
  if (b.remaining() >= 2) capabilities = b.le16();
  if (!b.ok()) { *why = "bad RSN element"; return 0; }
  if (group[0] != 0x00 || group[1] != 0x0F || group[2] != 0xAC || group[3] != 4) { *why = "the group cipher is not CCMP"; return 0; }
  if (!ccmp) { *why = "the network offers no CCMP"; return 0; }
  if (!psk) { *why = "the network offers no pre-shared key"; return 0; }
  if (capabilities & 0x0040) { *why = "the network requires protected management frames"; return 0; }
  static const uint8_t ours[22] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0};
  if (out.size() < sizeof ours) { *why = "no room"; return 0; }
  for (uint32_t i = 0; i < sizeof ours; ++i) out[i] = ours[i];
  return sizeof ours;
}

}  // namespace ieee80211
}  // namespace hardware
