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


#include "Hardware/WirelessInterface/Ieee80211Association.hpp"

namespace hardware {

using namespace ieee80211;

namespace {
bool five_ghz(uint8_t channel) { return channel > 14; }
}

WifiResult Ieee80211Association::start(const WifiNetwork& network, ConstByteView ap_rsn_element, StringView passphrase, const uint8_t snonce[wpa2::kNonceBytes],
                                       const uint8_t our_address[6]) {
  state_ = State::Idle;
  failure_ = "";
  handshake_silent_ = false;
  association_id_ = 0;
  rsn_ie_length_ = 0;
  secure_ = false;
  if (network.ssid[0] == 0) return WifiResult::InvalidArgument;
  network_ = network;
  for (uint32_t i = 0; i < 6; ++i) our_address_[i] = our_address[i];
  uint32_t ssid_length = 0;
  while (ssid_length < kMaxSsidLength && network.ssid[ssid_length]) ++ssid_length;
  if (network.security == WifiSecurity::Enterprise || network.security == WifiSecurity::Wpa3 || network.security == WifiSecurity::Wep || network.security == WifiSecurity::Wpa) return WifiResult::Unsupported;
  if (network.security == WifiSecurity::Wpa2) {
    secure_ = true;
    uint8_t pmk[wpa2::kPmkBytes];
    if (!wpa2::derive_pmk(passphrase, ConstByteView(reinterpret_cast<const uint8_t*>(network.ssid), ssid_length), pmk)) return WifiResult::BadPassphrase;
    const char* why = nullptr;
    rsn_ie_length_ = negotiate_rsn(ap_rsn_element, ByteView(rsn_ie_, sizeof rsn_ie_), &why);
    if (rsn_ie_length_ == 0) {
      failure_ = why ? why : "no usable RSN element";
      return WifiResult::Unsupported;
    }
    supplicant_.start(pmk, network.bssid.bytes, our_address, ConstByteView(rsn_ie_, rsn_ie_length_), snonce);
    for (uint32_t i = 0; i < wpa2::kPmkBytes; ++i) pmk[i] = 0;
  }
  state_ = State::Authenticating;
  attempts_ = 0;
  send_step();
  return WifiResult::Ok;
}

void Ieee80211Association::fail(const char* why) {
  state_ = State::Failed;
  failure_ = why;
}

void Ieee80211Association::send_step() {
  uint8_t frame[160];
  uint32_t n = 0;
  uint32_t ssid_length = 0;
  while (ssid_length < kMaxSsidLength && network_.ssid[ssid_length]) ++ssid_length;
  if (state_ == State::Authenticating) n = build_authentication(network_.bssid.bytes, our_address_, ByteView(frame, sizeof frame));
  else if (state_ == State::Associating)
    n = build_association_request(network_.bssid.bytes, our_address_, ConstByteView(reinterpret_cast<const uint8_t*>(network_.ssid), ssid_length), five_ghz(network_.channel),
                                  ConstByteView(rsn_ie_, rsn_ie_length_), ByteView(frame, sizeof frame));
  step_started_ms_ = link_.now_ms();
  ++attempts_;
  if (n) link_.transmit(ConstByteView(frame, n));       // (a refused transmit is retried by the time-out)
}

void Ieee80211Association::begin_association() {
  state_ = State::Associating;
  attempts_ = 0;
  send_step();
}

void Ieee80211Association::begin_handshake_wait() {
  state_ = State::Handshaking;
  step_started_ms_ = link_.now_ms();
}

void Ieee80211Association::on_frame(ConstByteView frame) {
  if (state_ == State::Idle || state_ == State::Failed) return;
  MgmtFrame m;
  if (parse_management(frame, network_.bssid.bytes, our_address_, &m)) {
    if (m.subtype == kSubtypeDeauthentication || m.subtype == kSubtypeDisassociation) {
      fail(m.subtype == kSubtypeDeauthentication ? "the access point deauthenticated us" : "the access point disassociated us");
      return;
    }
    if (m.subtype == kSubtypeAuthentication && state_ == State::Authenticating) {
      if (m.algorithm != 0 || m.sequence != 2) return;
      if (m.status != kStatusSuccess) return fail("the access point refused authentication");
      begin_association();
    } else if (m.subtype == kSubtypeAssocResponse && state_ == State::Associating) {
      if (m.status != kStatusSuccess) return fail("the access point refused the association");
      association_id_ = m.association_id;
      if (secure_) begin_handshake_wait();
      else {
        state_ = State::Connected;
        link_.port_opened(association_id_);
      }
    }
    return;
  }
  ConstByteView eapol = parse_eapol_data(frame, network_.bssid.bytes, our_address_);
  if (eapol.size() == 0) return;
  on_eapol(eapol);
}

void Ieee80211Association::on_eapol(ConstByteView eapol) {
  if (state_ != State::Handshaking && state_ != State::Connected) return;
  uint8_t reply[wpa2::kEapolHeaderBytes + wpa2::kKeyFixedBytes + wpa2::kMaxKeyData];
  wpa2::Supplicant::Result result;
  if (!supplicant_.on_eapol(eapol, ByteView(reply, sizeof reply), &result)) {
    if (supplicant_.state() == wpa2::Supplicant::State::Failed) fail(supplicant_.failure());
    return;
  }
  if (result.reply_length) {
    uint8_t data[200 + 64];
    const uint32_t n = build_eapol_data(network_.bssid.bytes, our_address_, ConstByteView(reply, result.reply_length), ByteView(data, sizeof data));
    if (n) link_.transmit(ConstByteView(data, n));
  }
  // the keys go in after message 4 is out (it is not encrypted)
  if (result.install_ptk && !link_.install_pairwise_key(supplicant_.tk())) return fail("the radio would not take the pairwise key");
  if (result.install_gtk && !link_.install_group_key(supplicant_.gtk())) return fail("the radio would not take the group key");
  if (result.handshake_complete) {
    state_ = State::Connected;
    link_.port_opened(association_id_);
  }
}

void Ieee80211Association::poll() {
  const uint64_t now = link_.now_ms();
  switch (state_) {
    case State::Authenticating:
    case State::Associating:
      if (now - step_started_ms_ < kStepTimeoutMs) return;
      if (attempts_ >= kStepAttempts) return fail(state_ == State::Authenticating ? "the access point did not answer the authentication" : "the access point did not answer the association request");
      send_step();
      break;
    case State::Handshaking:
      if (now - step_started_ms_ >= kHandshakeTimeoutMs) {
        handshake_silent_ = true;
        fail("the key handshake did not finish (wrong passphrase?)");
      }
      break;
    default:
      break;
  }
}

void Ieee80211Association::leave() {
  if (state_ == State::Associating || state_ == State::Handshaking || state_ == State::Connected) {
    uint8_t frame[40];
    const uint32_t n = build_deauthentication(network_.bssid.bytes, our_address_, 3 /* leaving */, ByteView(frame, sizeof frame));
    if (n) link_.transmit(ConstByteView(frame, n));
  }
  state_ = State::Idle;
}

}  // namespace hardware
