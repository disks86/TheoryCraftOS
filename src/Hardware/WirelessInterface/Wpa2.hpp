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


// WPA2-Personal (IEEE 802.11i, WPA2 with CCMP and a pre-shared key) for a client: the pairwise master key from a passphrase (PBKDF2-HMAC-SHA1, 4096 rounds), the pairwise transient
// key (the 802.11 PRF over both addresses and both nonces), the EAPOL-Key frames of the four-way handshake and of the group key handshake (key descriptor version 2: HMAC-SHA1-128
// MIC, AES key wrap for the key data), and the supplicant that answers them. Independent of any chipset: it is given EAPOL frames and returns the replies and the keys to
// install. Frames from the air are untrusted: every length is checked, a MIC that fails drops the frame, and replay counters only move forward.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace hardware {
namespace wpa2 {

constexpr uint32_t kPmkBytes = 32, kPtkBytes = 48, kKckBytes = 16, kKekBytes = 16, kTkBytes = 16, kNonceBytes = 32, kMicBytes = 16, kMaxGtkBytes = 32;
constexpr uint32_t kMaxPassphrase = 63, kMinPassphrase = 8, kMaxSsid = 32;

// PBKDF2-HMAC-SHA1 (RFC 8018) of `password` and `salt`: `out.size()` bytes (any length).
void pbkdf2_sha1(ConstByteView password, ConstByteView salt, uint32_t iterations, ByteView out);
// The PMK of a passphrase (8 to 63 printable ASCII characters) and an SSID. False for a passphrase that is not one.
bool derive_pmk(StringView passphrase, ConstByteView ssid, uint8_t pmk[kPmkBytes]);
// The 802.11 PRF (IEEE 802.11-2020 12.7.1.2) with HMAC-SHA1: `out.size()` bytes (at most 64) from key, label and data.
void prf_sha1(ConstByteView key, StringView label, ConstByteView data, ByteView out);
// The PTK (KCK 16, KEK 16, TK 16) for the authenticator `aa` and the supplicant `spa`.
void derive_ptk(const uint8_t pmk[kPmkBytes], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[kNonceBytes], const uint8_t snonce[kNonceBytes], uint8_t ptk[kPtkBytes]);
// RFC 3394 AES key wrap / unwrap with a 128-bit key. wrap: `plain` (a multiple of 8, at least 16 bytes) to plain.size() + 8 bytes. unwrap: the reverse; false if the integrity check fails.
bool aes_key_wrap(ConstByteView kek, ConstByteView plain, ByteView wrapped);
bool aes_key_unwrap(ConstByteView kek, ConstByteView wrapped, ByteView plain);

// ---- EAPOL-Key frames ---------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kEapolHeaderBytes = 4, kKeyFixedBytes = 95, kMaxKeyData = 256;
constexpr uint16_t kInfoVersionMask = 0x0007, kInfoVersionHmacSha1Aes = 2, kInfoPairwise = 1u << 3, kInfoInstall = 1u << 6, kInfoAck = 1u << 7, kInfoMic = 1u << 8, kInfoSecure = 1u << 9,
                   kInfoError = 1u << 10, kInfoRequest = 1u << 11, kInfoEncryptedData = 1u << 12;

struct EapolKey {
  uint16_t info = 0;
  uint16_t key_length = 0;
  uint8_t replay_counter[8] = {};
  uint8_t nonce[kNonceBytes] = {};
  uint8_t iv[16] = {};
  uint8_t rsc[8] = {};
  uint8_t mic[kMicBytes] = {};
  ConstByteView data;                      // the key data field (encrypted when kInfoEncryptedData)
  uint64_t counter() const {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | replay_counter[i];
    return v;
  }
};

// Parses an EAPOL frame (the 802.11 data payload after the LLC/SNAP header: version, type, length, then the key descriptor). False unless it is a WPA2 (RSN) EAPOL-Key frame
// that fits. `mic_view` is the frame with the MIC field zeroed in a copy for checking: use verify_mic().
bool parse_eapol_key(ConstByteView frame, EapolKey* out);
// Serialises a key frame into `out` (the whole EAPOL frame); returns its length or 0 if `out` is too small or the data is too long. The MIC field is written as given.
uint32_t build_eapol_key(const EapolKey& key, ByteView out);
// Computes the MIC over a whole EAPOL frame (with its MIC field zeroed) under the KCK and writes it into the frame; or checks it.
bool set_mic(ConstByteView kck, ByteView frame);
bool verify_mic(ConstByteView kck, ConstByteView frame);

// ---- the supplicant -----------------------------------------------------------------------------------------------------------------------------------------------------

// The group temporal key to install (key id 1 to 3).
struct GroupKey {
  uint8_t id = 0;
  uint8_t length = 0;
  uint8_t key[kMaxGtkBytes] = {};
  uint8_t rsc[8] = {};
};

class Supplicant {
 public:
  enum class State : uint8_t { Idle, WaitMessage3, Established, Failed };
  // What on_eapol() asks of the caller.
  struct Result {
    uint32_t reply_length = 0;             // bytes written to the reply buffer (an EAPOL frame to send), 0: none
    bool install_ptk = false;              // the pairwise key is ready: install tk() (after the reply is sent, as the AP's message 3 asks)
    bool install_gtk = false;              // a group key arrived: install gtk()
    bool handshake_complete = false;
  };

  // `rsn_ie` is our RSN information element (id 48 included) as sent in the association request. `snonce` is 32 random bytes; the caller supplies them.
  void start(const uint8_t pmk[kPmkBytes], const uint8_t aa[6], const uint8_t spa[6], ConstByteView rsn_ie, const uint8_t snonce[kNonceBytes]);
  // Feeds an EAPOL frame from the access point; writes any reply into `reply`. False when the frame is dropped (not ours to answer, bad MIC, replayed, malformed).
  bool on_eapol(ConstByteView frame, ByteView reply, Result* result);
  State state() const { return state_; }
  const uint8_t* tk() const { return ptk_ + kKckBytes + kKekBytes; }
  const GroupKey& gtk() const { return gtk_; }
  const char* failure() const { return failure_; }

 private:
  bool message1(const EapolKey& key, ByteView reply, Result* result);
  bool message3(ConstByteView frame, const EapolKey& key, ByteView reply, Result* result);
  bool group_message1(ConstByteView frame, const EapolKey& key, ByteView reply, Result* result);
  bool parse_key_data(ConstByteView data, bool pairwise);
  uint32_t build_reply(uint16_t info, const EapolKey& request, ConstByteView data, ByteView reply, bool with_nonce);

  State state_ = State::Idle;
  uint8_t pmk_[kPmkBytes] = {}, aa_[6] = {}, spa_[6] = {}, snonce_[kNonceBytes] = {}, anonce_[kNonceBytes] = {}, ptk_[kPtkBytes] = {};
  uint8_t rsn_ie_[64] = {};
  uint32_t rsn_ie_length_ = 0;
  uint64_t last_counter_ = 0;
  bool have_counter_ = false, have_ptk_ = false;
  GroupKey gtk_;
  const char* failure_ = "";
};

}  // namespace wpa2
}  // namespace hardware
