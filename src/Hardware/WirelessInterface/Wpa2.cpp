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


#include "Hardware/WirelessInterface/Wpa2.hpp"
#include "Crypto/Crypto.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace wpa2 {

using crypto::HashKind;

namespace {
constexpr uint32_t kMicOffset = 81;                          // the MIC field in an EAPOL-Key frame
constexpr uint32_t kMaxFrame = kEapolHeaderBytes + kKeyFixedBytes + kMaxKeyData;
constexpr uint32_t kHmacBytes = 20;

void xor_into(uint8_t* a, const uint8_t* b, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) a[i] ^= b[i];
}
int compare(const uint8_t* a, const uint8_t* b, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
}  // namespace

void pbkdf2_sha1(ConstByteView password, ConstByteView salt, uint32_t iterations, ByteView out) {
  uint32_t produced = 0;
  for (uint32_t block = 1; produced < out.size(); ++block) {
    // U1 = HMAC(P, salt || INT(block))
    uint8_t message[kMaxSsid + 4 + 16];
    uint32_t n = 0;
    const uint64_t salt_bytes = salt.size() < sizeof message - 4 ? salt.size() : sizeof message - 4;
    for (uint64_t i = 0; i < salt_bytes; ++i) message[n++] = salt[i];
    message[n++] = static_cast<uint8_t>(block >> 24);
    message[n++] = static_cast<uint8_t>(block >> 16);
    message[n++] = static_cast<uint8_t>(block >> 8);
    message[n++] = static_cast<uint8_t>(block);
    uint8_t u[kHmacBytes], t[kHmacBytes];
    crypto::hmac(HashKind::Sha1, password, ConstByteView(message, n), u);
    for (uint32_t i = 0; i < kHmacBytes; ++i) t[i] = u[i];
    for (uint32_t it = 1; it < iterations; ++it) {
      crypto::hmac(HashKind::Sha1, password, ConstByteView(u, kHmacBytes), u);
      xor_into(t, u, kHmacBytes);
    }
    for (uint32_t i = 0; i < kHmacBytes && produced < out.size(); ++i) out[produced++] = t[i];
  }
}

bool derive_pmk(StringView passphrase, ConstByteView ssid, uint8_t pmk[kPmkBytes]) {
  if (passphrase.size() < kMinPassphrase || passphrase.size() > kMaxPassphrase || ssid.size() == 0 || ssid.size() > kMaxSsid) return false;
  uint8_t pass[kMaxPassphrase];
  for (uint64_t i = 0; i < passphrase.size(); ++i) {
    const uint8_t c = static_cast<uint8_t>(passphrase[i]);
    if (c < 32 || c > 126) return false;
    pass[i] = c;
  }
  pbkdf2_sha1(ConstByteView(pass, passphrase.size()), ssid, 4096, ByteView(pmk, kPmkBytes));
  return true;
}

void prf_sha1(ConstByteView key, StringView label, ConstByteView data, ByteView out) {
  uint8_t message[64 + 1 + 76 + 1];
  uint32_t n = 0;
  for (uint64_t i = 0; i < label.size() && n < 64; ++i) message[n++] = static_cast<uint8_t>(label[i]);
  message[n++] = 0;
  for (uint64_t i = 0; i < data.size() && n < sizeof message - 1; ++i) message[n++] = data[i];
  uint32_t produced = 0;
  for (uint32_t counter = 0; produced < out.size(); ++counter) {
    message[n] = static_cast<uint8_t>(counter);
    uint8_t block[kHmacBytes];
    crypto::hmac(HashKind::Sha1, key, ConstByteView(message, n + 1), block);
    for (uint32_t i = 0; i < kHmacBytes && produced < out.size(); ++i) out[produced++] = block[i];
  }
}

void derive_ptk(const uint8_t pmk[kPmkBytes], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[kNonceBytes], const uint8_t snonce[kNonceBytes], uint8_t ptk[kPtkBytes]) {
  uint8_t data[6 + 6 + kNonceBytes + kNonceBytes];
  const bool aa_low = compare(aa, spa, 6) < 0;
  for (uint32_t i = 0; i < 6; ++i) {
    data[i] = aa_low ? aa[i] : spa[i];
    data[6 + i] = aa_low ? spa[i] : aa[i];
  }
  const bool an_low = compare(anonce, snonce, kNonceBytes) < 0;
  for (uint32_t i = 0; i < kNonceBytes; ++i) {
    data[12 + i] = an_low ? anonce[i] : snonce[i];
    data[12 + kNonceBytes + i] = an_low ? snonce[i] : anonce[i];
  }
  prf_sha1(ConstByteView(pmk, kPmkBytes), StringView("Pairwise key expansion"), ConstByteView(data, sizeof data), ByteView(ptk, kPtkBytes));
}

// RFC 3394
bool aes_key_wrap(ConstByteView kek, ConstByteView plain, ByteView wrapped) {
  const uint64_t n = plain.size() / 8;
  if (plain.size() % 8 != 0 || n < 2 || n > 34 || wrapped.size() < plain.size() + 8) return false;
  crypto::Aes aes;
  if (!aes.set_key(kek)) return false;
  uint8_t a[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
  uint8_t r[34][8];
  for (uint64_t i = 0; i < n; ++i)
    for (uint32_t k = 0; k < 8; ++k) r[i][k] = plain[i * 8 + k];
  for (uint32_t j = 0; j < 6; ++j)
    for (uint64_t i = 0; i < n; ++i) {
      uint8_t b[16], out[16];
      for (uint32_t k = 0; k < 8; ++k) { b[k] = a[k]; b[8 + k] = r[i][k]; }
      aes.encrypt_block(b, out);
      const uint64_t t = n * j + i + 1;
      for (uint32_t k = 0; k < 8; ++k) a[k] = out[k] ^ static_cast<uint8_t>(t >> (8 * (7 - k)));
      for (uint32_t k = 0; k < 8; ++k) r[i][k] = out[8 + k];
    }
  for (uint32_t k = 0; k < 8; ++k) wrapped[k] = a[k];
  for (uint64_t i = 0; i < n; ++i)
    for (uint32_t k = 0; k < 8; ++k) wrapped[8 + i * 8 + k] = r[i][k];
  return true;
}

bool aes_key_unwrap(ConstByteView kek, ConstByteView wrapped, ByteView plain) {
  if (wrapped.size() % 8 != 0 || wrapped.size() < 24 || wrapped.size() > 8 * 35) return false;
  const uint64_t n = wrapped.size() / 8 - 1;
  if (plain.size() < n * 8) return false;
  crypto::Aes aes;
  if (!aes.set_key(kek)) return false;
  uint8_t a[8];
  uint8_t r[34][8];
  for (uint32_t k = 0; k < 8; ++k) a[k] = wrapped[k];
  for (uint64_t i = 0; i < n; ++i)
    for (uint32_t k = 0; k < 8; ++k) r[i][k] = wrapped[8 + i * 8 + k];
  for (int32_t j = 5; j >= 0; --j)
    for (int64_t i = static_cast<int64_t>(n) - 1; i >= 0; --i) {
      uint8_t b[16], out[16];
      const uint64_t t = n * static_cast<uint64_t>(j) + static_cast<uint64_t>(i) + 1;
      for (uint32_t k = 0; k < 8; ++k) { b[k] = a[k] ^ static_cast<uint8_t>(t >> (8 * (7 - k))); b[8 + k] = r[i][k]; }
      aes.decrypt_block(b, out);
      for (uint32_t k = 0; k < 8; ++k) { a[k] = out[k]; r[i][k] = out[8 + k]; }
    }
  static const uint8_t kIv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
  if (!crypto::equal_constant_time(a, kIv, 8)) return false;
  for (uint64_t i = 0; i < n; ++i)
    for (uint32_t k = 0; k < 8; ++k) plain[i * 8 + k] = r[i][k];
  return true;
}

// ---- EAPOL-Key ----------------------------------------------------------------------------------------------------------------------------------------------------------

bool parse_eapol_key(ConstByteView frame, EapolKey* out) {
  ByteReader r(frame);
  const uint8_t version = r.u8();
  const uint8_t type = r.u8();
  const uint16_t body_length = r.be16();
  if (!r.ok() || version == 0 || version > 3 || type != 3 || body_length < kKeyFixedBytes || body_length > r.remaining()) return false;
  ByteReader b(r.take(body_length));
  if (b.u8() != 2) return false;                             // key descriptor type: RSN
  EapolKey k;
  k.info = b.be16();
  k.key_length = b.be16();
  b.read_into(ByteView(k.replay_counter));
  b.read_into(ByteView(k.nonce));
  b.read_into(ByteView(k.iv));
  b.read_into(ByteView(k.rsc));
  b.skip(8);
  b.read_into(ByteView(k.mic));
  const uint16_t data_length = b.be16();
  if (!b.ok() || data_length > kMaxKeyData) return false;
  k.data = b.take(data_length);
  if (!b.ok()) return false;
  *out = k;
  return true;
}

uint32_t build_eapol_key(const EapolKey& key, ByteView out) {
  if (key.data.size() > kMaxKeyData) return 0;
  ByteWriter w(out);
  w.u8(2);                                                   // 802.1X-2004
  w.u8(3);                                                   // EAPOL-Key
  w.be16(static_cast<uint16_t>(kKeyFixedBytes + key.data.size()));
  w.u8(2);                                                   // RSN key descriptor
  w.be16(key.info);
  w.be16(key.key_length);
  w.bytes(ConstByteView(key.replay_counter));
  w.bytes(ConstByteView(key.nonce));
  w.bytes(ConstByteView(key.iv));
  w.bytes(ConstByteView(key.rsc));
  w.zeros(8);
  w.bytes(ConstByteView(key.mic));
  w.be16(static_cast<uint16_t>(key.data.size()));
  w.bytes(key.data);
  return w.ok() ? static_cast<uint32_t>(w.position()) : 0;
}

static void compute_mic(ConstByteView kck, ConstByteView frame, uint8_t mic[kMicBytes]) {
  uint8_t copy[kMaxFrame];
  const uint64_t n = frame.size() < sizeof copy ? frame.size() : sizeof copy;
  for (uint64_t i = 0; i < n; ++i) copy[i] = frame[i];
  for (uint32_t i = 0; i < kMicBytes; ++i) copy[kMicOffset + i] = 0;
  uint8_t full[kHmacBytes];
  crypto::hmac(HashKind::Sha1, kck, ConstByteView(copy, n), full);
  for (uint32_t i = 0; i < kMicBytes; ++i) mic[i] = full[i];
}

bool set_mic(ConstByteView kck, ByteView frame) {
  if (frame.size() < kMicOffset + kMicBytes || frame.size() > kMaxFrame) return false;
  uint8_t mic[kMicBytes];
  compute_mic(kck, frame, mic);
  for (uint32_t i = 0; i < kMicBytes; ++i) frame[kMicOffset + i] = mic[i];
  return true;
}

bool verify_mic(ConstByteView kck, ConstByteView frame) {
  if (frame.size() < kMicOffset + kMicBytes || frame.size() > kMaxFrame) return false;
  uint8_t mic[kMicBytes];
  compute_mic(kck, frame, mic);
  uint8_t have[kMicBytes];
  for (uint32_t i = 0; i < kMicBytes; ++i) have[i] = frame[kMicOffset + i];
  return crypto::equal_constant_time(mic, have, kMicBytes);
}

// ---- the supplicant -----------------------------------------------------------------------------------------------------------------------------------------------------

void Supplicant::start(const uint8_t pmk[kPmkBytes], const uint8_t aa[6], const uint8_t spa[6], ConstByteView rsn_ie, const uint8_t snonce[kNonceBytes]) {
  *this = Supplicant();
  for (uint32_t i = 0; i < kPmkBytes; ++i) pmk_[i] = pmk[i];
  for (uint32_t i = 0; i < 6; ++i) { aa_[i] = aa[i]; spa_[i] = spa[i]; }
  for (uint32_t i = 0; i < kNonceBytes; ++i) snonce_[i] = snonce[i];
  if (rsn_ie.size() > sizeof rsn_ie_) {
    state_ = State::Failed;
    failure_ = "our RSN element is too long";
    return;
  }
  rsn_ie_length_ = static_cast<uint32_t>(rsn_ie.size());
  for (uint32_t i = 0; i < rsn_ie_length_; ++i) rsn_ie_[i] = rsn_ie[i];
}

uint32_t Supplicant::build_reply(uint16_t info, const EapolKey& request, ConstByteView data, ByteView reply, bool with_nonce) {
  EapolKey k;
  k.info = info;
  k.key_length = 0;
  for (uint32_t i = 0; i < 8; ++i) k.replay_counter[i] = request.replay_counter[i];
  if (with_nonce)
    for (uint32_t i = 0; i < kNonceBytes; ++i) k.nonce[i] = snonce_[i];
  k.data = data;
  const uint32_t n = build_eapol_key(k, reply);
  if (n == 0 || !set_mic(ConstByteView(ptk_, kKckBytes), reply.subview(0, n))) return 0;
  return n;
}

bool Supplicant::message1(const EapolKey& key, ByteView reply, Result* result) {
  if (state_ == State::Established || state_ == State::Failed) return false;
  for (uint32_t i = 0; i < kNonceBytes; ++i) anonce_[i] = key.nonce[i];
  derive_ptk(pmk_, aa_, spa_, anonce_, snonce_, ptk_);
  have_ptk_ = true;
  last_counter_ = key.counter();
  have_counter_ = true;
  state_ = State::WaitMessage3;
  const uint32_t n = build_reply(kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoMic, key, ConstByteView(rsn_ie_, rsn_ie_length_), reply, true);
  if (n == 0) return false;
  result->reply_length = n;
  return true;
}

// The key data of message 3 / the group message: elements (id, length, body) where the vendor element 00-0F-AC type 1 carries the GTK.
bool Supplicant::parse_key_data(ConstByteView data, bool pairwise) {
  (void)pairwise;
  ByteReader r(data);
  bool found = false;
  while (r.remaining() >= 2) {
    const uint8_t id = r.u8();
    const uint8_t length = r.u8();
    if (id == 0 && length == 0) break;                                // padding
    ConstByteView body = r.take(length);
    if (!r.ok()) return false;
    if (id == 0xDD && length >= 6 && body[0] == 0x00 && body[1] == 0x0F && body[2] == 0xAC && body[3] == 1) {
      const uint32_t key_bytes = length - 6;
      if (key_bytes != 16 && key_bytes != 32) return false;          // CCMP: 16 (TKIP groups are not supported)
      if (key_bytes != 16) return false;
      GroupKey g;
      g.id = body[4] & 3;
      g.length = static_cast<uint8_t>(key_bytes);
      for (uint32_t i = 0; i < key_bytes; ++i) g.key[i] = body[6 + i];
      if (g.id == 0) return false;
      gtk_ = g;
      found = true;
    }
  }
  return found;
}

bool Supplicant::message3(ConstByteView frame, const EapolKey& key, ByteView reply, Result* result) {
  if (!have_ptk_ || (state_ != State::WaitMessage3 && state_ != State::Established)) return false;
  if (!verify_mic(ConstByteView(ptk_, kKckBytes), frame)) return false;
  if (!have_counter_ || key.counter() <= last_counter_) return false;
  // the nonce must be the one of message 1 (a different one would be a new handshake, started with message 1)
  if (!crypto::equal_constant_time(key.nonce, anonce_, kNonceBytes)) return false;
  const uint16_t want = kInfoPairwise | kInfoInstall | kInfoAck | kInfoMic | kInfoSecure | kInfoEncryptedData;
  if ((key.info & want) != want) return false;
  if (key.data.size() < 24 || key.data.size() % 8 != 0 || key.data.size() > kMaxKeyData) return false;
  uint8_t plain[kMaxKeyData];
  if (!aes_key_unwrap(ConstByteView(ptk_ + kKckBytes, kKekBytes), key.data, ByteView(plain, sizeof plain))) return false;
  const bool first = state_ != State::Established;
  GroupKey saved = gtk_;
  if (!parse_key_data(ConstByteView(plain, key.data.size() - 8), true)) {
    gtk_ = saved;
    state_ = State::Failed;
    failure_ = "message 3 carries no usable group key";
    return false;
  }
  last_counter_ = key.counter();
  const uint32_t n = build_reply(kInfoVersionHmacSha1Aes | kInfoPairwise | kInfoMic | kInfoSecure, key, ConstByteView(), reply, false);
  if (n == 0) return false;
  result->reply_length = n;
  if (first) {                                                       // a retransmitted message 3 is answered but nothing is installed twice
    result->install_ptk = true;
    result->install_gtk = true;
    result->handshake_complete = true;
  }
  state_ = State::Established;
  return true;
}

bool Supplicant::group_message1(ConstByteView frame, const EapolKey& key, ByteView reply, Result* result) {
  if (state_ != State::Established) return false;
  if (!verify_mic(ConstByteView(ptk_, kKckBytes), frame)) return false;
  if (key.counter() <= last_counter_) return false;
  const uint16_t want = kInfoAck | kInfoMic | kInfoSecure | kInfoEncryptedData;
  if ((key.info & want) != want || key.data.size() < 24 || key.data.size() % 8 != 0) return false;
  uint8_t plain[kMaxKeyData];
  if (!aes_key_unwrap(ConstByteView(ptk_ + kKckBytes, kKekBytes), key.data, ByteView(plain, sizeof plain))) return false;
  GroupKey saved = gtk_;
  if (!parse_key_data(ConstByteView(plain, key.data.size() - 8), false)) {
    gtk_ = saved;
    return false;
  }
  last_counter_ = key.counter();
  const uint32_t n = build_reply(kInfoVersionHmacSha1Aes | kInfoMic | kInfoSecure | (key.info & 0x30), key, ConstByteView(), reply, false);
  if (n == 0) return false;
  result->reply_length = n;
  result->install_gtk = true;
  return true;
}

bool Supplicant::on_eapol(ConstByteView frame, ByteView reply, Result* result) {
  *result = Result();
  if (state_ == State::Failed) return false;
  EapolKey key;
  if (!parse_eapol_key(frame, &key)) return false;
  if ((key.info & kInfoVersionMask) != kInfoVersionHmacSha1Aes || !(key.info & kInfoAck) || (key.info & kInfoError) || (key.info & kInfoRequest)) return false;
  if (key.info & kInfoPairwise) {
    if (!(key.info & kInfoMic)) return (key.info & (kInfoInstall | kInfoSecure | kInfoEncryptedData)) ? false : message1(key, reply, result);
    return message3(frame, key, reply, result);
  }
  return group_message1(frame, key, reply, result);
}

}  // namespace wpa2
}  // namespace hardware
