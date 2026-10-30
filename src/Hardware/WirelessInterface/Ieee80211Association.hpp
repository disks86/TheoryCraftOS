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


// Joining a network as a client (the state machine of a station): open system authentication, association, and for WPA2-Personal the four-way handshake, with retries and
// timeouts. It is driven by the frames the radio receives and by the clock, and acts through a link: transmit a frame, install keys, say the port is open. Independent of the
// chipset. States follow IEEE 802.11: Authenticating, Associating, (Handshaking), Connected; a refusal or a silence ends in Failed with a reason.
#pragma once
#include <stdint.h>
#include "Hardware/WirelessInterface/Ieee80211Frames.hpp"
#include "Hardware/WirelessInterface/Wpa2.hpp"
#include "Hardware/WirelessInterface/WirelessTypes.hpp"

namespace hardware {

// What the state machine needs from the radio.
class IWifiLink {
 public:
  // Sends an 802.11 frame (management or data, from the frame control field on, no FCS). False if the radio would not take it.
  virtual bool transmit(ConstByteView frame) = 0;
  // Keys for the hardware to use (CCMP). The pairwise key is for the access point, the group key for broadcast / multicast frames from it.
  virtual bool install_pairwise_key(const uint8_t tk[wpa2::kTkBytes]) = 0;
  virtual bool install_group_key(const wpa2::GroupKey& key) = 0;
  // The association is up (and for WPA2 the keys are in): data may flow.
  virtual void port_opened(uint16_t association_id) = 0;
  virtual uint64_t now_ms() = 0;

 protected:
  constexpr IWifiLink() = default;
  ~IWifiLink() = default;
};

class Ieee80211Association {
 public:
  enum class State : uint8_t { Idle, Authenticating, Associating, Handshaking, Connected, Failed };
  static constexpr uint32_t kStepTimeoutMs = 1000, kStepAttempts = 3, kHandshakeTimeoutMs = 5000;

  explicit Ieee80211Association(IWifiLink& link) : link_(link) {}

  // Starts joining `network` (its BSSID, SSID and channel from a scan). `ap_rsn_element` is the RSN element of its beacon (empty for an open network), `passphrase` the WPA2
  // passphrase, `snonce` 32 random bytes for the handshake, `our_address` our MAC address. WifiResult::Ok when under way; BadPassphrase, Unsupported, InvalidArgument otherwise.
  WifiResult start(const WifiNetwork& network, ConstByteView ap_rsn_element, StringView passphrase, const uint8_t snonce[wpa2::kNonceBytes], const uint8_t our_address[6]);
  // A frame the radio received from the air (the 802.11 frame without FCS).
  void on_frame(ConstByteView frame);
  // An EAPOL frame the radio already took out of a data frame (the hardware may have decrypted it).
  void on_eapol(ConstByteView eapol);
  // The clock: retries and time-outs.
  void poll();
  // Leaves the network (a deauthentication frame goes out if joined).
  void leave();

  State state() const { return state_; }
  const char* failure() const { return failure_; }
  uint16_t association_id() const { return association_id_; }
  bool secure() const { return secure_; }
  // The handshake died of silence: the usual sign of a wrong passphrase (the access point drops message 2 when its MIC is wrong).
  bool bad_passphrase_suspected() const { return handshake_silent_; }

 private:
  void fail(const char* why);
  void send_step();
  void begin_association();
  void begin_handshake_wait();

  IWifiLink& link_;
  State state_ = State::Idle;
  WifiNetwork network_;
  uint8_t our_address_[6] = {};
  uint8_t rsn_ie_[24] = {};
  uint32_t rsn_ie_length_ = 0;
  bool secure_ = false, handshake_silent_ = false;
  wpa2::Supplicant supplicant_;
  uint64_t step_started_ms_ = 0;
  uint32_t attempts_ = 0;
  uint16_t association_id_ = 0;
  const char* failure_ = "";
};

}  // namespace hardware
