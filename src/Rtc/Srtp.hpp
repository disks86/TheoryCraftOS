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

// SRTP and SRTCP (RFC 3711, with AES-GCM from RFC 7714): the profiles DTLS-SRTP negotiates (RFC 5764 / 7714). One Srtp protects the packets of one direction (or
// unprotects the other's): it holds the session keys derived from the master key and salt, the roll-over counter and replay window of every SSRC it has seen.
#pragma once
#include "Crypto/Aead.hpp"
#include "Crypto/Crypto.hpp"
#include "ObjectContainer/List.hpp"
#include "Rtc/RtcBytes.hpp"

namespace rtc {

// The numbers are the ones of the DTLS "use_srtp" extension.
enum class SrtpProfile : uint16_t { Aes128CmSha1_80 = 1, Aes128CmSha1_32 = 2, AeadAes128Gcm = 7, AeadAes256Gcm = 8 };
uint32_t srtp_key_size(SrtpProfile p);
uint32_t srtp_salt_size(SrtpProfile p);
bool srtp_profile_known(uint16_t id);

class Srtp {
 public:
  explicit Srtp(allocators::IAllocator& allocator) : streams_(allocator), rtcp_streams_(allocator) {}
  // Derives the session keys (RFC 3711 section 4.3).
  bool init(SrtpProfile profile, ConstByteView master_key, ConstByteView master_salt);
  bool ready() const { return ready_; }
  SrtpProfile profile() const { return profile_; }

  // Whole RTP / RTCP packets, in place: protect adds the authentication tag (and the SRTCP index), unprotect checks it, refuses a replayed packet, decrypts and cuts it off.
  bool protect_rtp(ByteBuffer* packet);
  bool unprotect_rtp(ByteBuffer* packet);
  bool protect_rtcp(ByteBuffer* packet);
  bool unprotect_rtcp(ByteBuffer* packet);

  // The key stream of the AES counter mode with this IV (RFC 3711 section 4.1.1), XORed into `data`. Exposed for the tests.
  // The session key / salt / auth key of `label` (0 RTP key, 1 RTP auth, 2 RTP salt, 3 RTCP key, 4 RTCP auth, 5 RTCP salt) from the master key and salt (RFC 3711 4.3).
  static bool derive_key(ConstByteView master_key, ConstByteView master_salt, uint8_t label, ByteView out);
  static void aes_cm(const crypto::Aes& aes, const uint8_t iv[16], ByteView data);

 private:
  struct Stream {
    uint32_t ssrc = 0;
    uint32_t roc = 0;
    uint16_t last_sequence = 0;
    bool seen = false;
    uint64_t highest = 0, window = 0;   // the replay window: the highest index, and which of the 64 below it came
  };
  Stream& stream_for(List<Stream>& list, uint32_t ssrc);
  bool gcm() const { return profile_ == SrtpProfile::AeadAes128Gcm || profile_ == SrtpProfile::AeadAes256Gcm; }
  uint32_t tag_size(bool rtcp) const;
  void rtp_iv(uint32_t ssrc, uint64_t index, uint8_t iv[16]) const;
  void rtcp_iv(uint32_t ssrc, uint32_t index, uint8_t iv[16]) const;
  bool replayed(Stream& s, uint64_t index) const;
  void accept(Stream& s, uint64_t index) const;

  bool ready_ = false;
  SrtpProfile profile_ = SrtpProfile::Aes128CmSha1_80;
  crypto::Aes rtp_aes_, rtcp_aes_;
  crypto::AesGcm rtp_gcm_, rtcp_gcm_;
  uint8_t rtp_auth_[20] = {}, rtcp_auth_[20] = {}, rtp_salt_[14] = {}, rtcp_salt_[14] = {};
  uint32_t rtcp_index_ = 0;   // the next SRTCP index to send
  List<Stream> streams_, rtcp_streams_;
};

}  // namespace rtc
