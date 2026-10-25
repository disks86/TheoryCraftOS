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

#include "Rtc/Srtp.hpp"
#include "Rtc/Rtp.hpp"

namespace rtc {

uint32_t srtp_key_size(SrtpProfile p) { return p == SrtpProfile::AeadAes256Gcm ? 32 : 16; }
uint32_t srtp_salt_size(SrtpProfile p) { return (p == SrtpProfile::AeadAes128Gcm || p == SrtpProfile::AeadAes256Gcm) ? 12 : 14; }
bool srtp_profile_known(uint16_t id) { return id == 1 || id == 2 || id == 7 || id == 8; }

void Srtp::aes_cm(const crypto::Aes& aes, const uint8_t iv_in[16], ByteView data) {
  uint8_t counter[16], block[16];
  for (int i = 0; i < 16; ++i) counter[i] = iv_in[i];
  for (uint64_t at = 0; at < data.size(); at += 16) {
    aes.encrypt_block(counter, block);
    const uint64_t n = data.size() - at < 16 ? data.size() - at : 16;
    for (uint64_t i = 0; i < n; ++i) data[at + i] ^= block[i];
    if (++counter[15] == 0) ++counter[14];   // (the block counter is the last 16 bits)
  }
}

namespace {

// RFC 3711 4.3.1: the key of `label` from the master key and salt (key_derivation_rate 0): AES-CM of zeros under the master key with IV = (salt XOR label<<48) * 2^16.
void derive(const crypto::Aes& master, const uint8_t* salt, uint32_t salt_size, uint8_t label, ByteView out) {
  uint8_t iv[16] = {};
  for (uint32_t i = 0; i < salt_size && i < 14; ++i) iv[i] = salt[i];
  iv[7] ^= label;
  for (uint64_t i = 0; i < out.size(); ++i) out[i] = 0;
  Srtp::aes_cm(master, iv, out);
}

}  // namespace

bool Srtp::derive_key(ConstByteView key, ConstByteView salt, uint8_t label, ByteView out) {
  crypto::Aes master;
  if (salt.size() > 14 || !master.set_key(key)) return false;
  derive(master, salt.data(), static_cast<uint32_t>(salt.size()), label, out);
  return true;
}

bool Srtp::init(SrtpProfile profile, ConstByteView key, ConstByteView salt) {
  ready_ = false;
  profile_ = profile;
  if (!srtp_profile_known(static_cast<uint16_t>(profile)) || key.size() != srtp_key_size(profile) || salt.size() != srtp_salt_size(profile)) return false;
  crypto::Aes master;
  if (!master.set_key(key)) return false;
  uint8_t session_key[32], salt_out[14] = {};
  const uint32_t ks = srtp_key_size(profile), ss = srtp_salt_size(profile);
  derive(master, salt.data(), ss, 0, ByteView(session_key, ks));
  const bool aead = gcm();
  if (aead) {
    if (!rtp_gcm_.set_key(ConstByteView(session_key, ks))) return false;
  } else if (!rtp_aes_.set_key(ConstByteView(session_key, ks))) {
    return false;
  }
  derive(master, salt.data(), ss, 2, ByteView(salt_out, ss));
  for (uint32_t i = 0; i < 14; ++i) rtp_salt_[i] = salt_out[i];
  if (!aead) derive(master, salt.data(), ss, 1, ByteView(rtp_auth_, 20));
  derive(master, salt.data(), ss, 3, ByteView(session_key, ks));
  if (aead) {
    if (!rtcp_gcm_.set_key(ConstByteView(session_key, ks))) return false;
  } else if (!rtcp_aes_.set_key(ConstByteView(session_key, ks))) {
    return false;
  }
  for (uint32_t i = 0; i < 14; ++i) salt_out[i] = 0;
  derive(master, salt.data(), ss, 5, ByteView(salt_out, ss));
  for (uint32_t i = 0; i < 14; ++i) rtcp_salt_[i] = salt_out[i];
  if (!aead) derive(master, salt.data(), ss, 4, ByteView(rtcp_auth_, 20));
  ready_ = true;
  return true;
}

uint32_t Srtp::tag_size(bool) const { return gcm() ? 16 : profile_ == SrtpProfile::Aes128CmSha1_32 ? 4 : 10; }

Srtp::Stream& Srtp::stream_for(List<Stream>& list, uint32_t ssrc) {
  for (Stream& s : list)
    if (s.ssrc == ssrc) return s;
  if (list.count() >= 256) list.remove_at(0);
  Stream fresh;
  fresh.ssrc = ssrc;
  list.push_back(fresh);
  return list[list.count() - 1];
}

void Srtp::rtp_iv(uint32_t ssrc, uint64_t index, uint8_t iv[16]) const {
  if (gcm()) {   // RFC 7714 8.1: 12 bytes, the salt XOR (0, SSRC, ROC, SEQ)
    for (int i = 0; i < 16; ++i) iv[i] = 0;
    for (int i = 0; i < 12; ++i) iv[i] = rtp_salt_[i];
    iv[2] ^= ssrc >> 24; iv[3] ^= ssrc >> 16; iv[4] ^= ssrc >> 8; iv[5] ^= ssrc;
    for (int i = 0; i < 6; ++i) iv[6 + i] ^= static_cast<uint8_t>(index >> (8 * (5 - i)));   // ROC (4 bytes) and SEQ (2): the 48 bit index
    return;
  }
  for (int i = 0; i < 16; ++i) iv[i] = i < 14 ? rtp_salt_[i] : 0;
  iv[4] ^= ssrc >> 24; iv[5] ^= ssrc >> 16; iv[6] ^= ssrc >> 8; iv[7] ^= ssrc;
  for (int i = 0; i < 6; ++i) iv[8 + i] ^= static_cast<uint8_t>(index >> (8 * (5 - i)));
}

void Srtp::rtcp_iv(uint32_t ssrc, uint32_t index, uint8_t iv[16]) const {
  for (int i = 0; i < 16; ++i) iv[i] = 0;
  if (gcm()) {   // RFC 7714 9.1: the salt XOR (0, SSRC, 0, SRTCP index)
    for (int i = 0; i < 12; ++i) iv[i] = rtcp_salt_[i];
    iv[2] ^= ssrc >> 24; iv[3] ^= ssrc >> 16; iv[4] ^= ssrc >> 8; iv[5] ^= ssrc;
    iv[8] ^= index >> 24; iv[9] ^= index >> 16; iv[10] ^= index >> 8; iv[11] ^= index;
    return;
  }
  for (int i = 0; i < 14; ++i) iv[i] = rtcp_salt_[i];
  iv[4] ^= ssrc >> 24; iv[5] ^= ssrc >> 16; iv[6] ^= ssrc >> 8; iv[7] ^= ssrc;
  iv[10] ^= index >> 24; iv[11] ^= index >> 16; iv[12] ^= index >> 8; iv[13] ^= index;
}

bool Srtp::replayed(Stream& s, uint64_t index) const {
  if (!s.seen) return false;
  if (index > s.highest) return false;
  const uint64_t d = s.highest - index;
  return d >= 64 || ((s.window >> d) & 1);   // too old, or seen
}

void Srtp::accept(Stream& s, uint64_t index) const {
  if (!s.seen || index > s.highest) {
    const uint64_t shift = s.seen ? index - s.highest : 0;
    s.window = (s.seen && shift < 64) ? (s.window << shift) | 1 : 1;
    s.highest = index;
    s.seen = true;
  } else {
    s.window |= uint64_t{1} << (s.highest - index);
  }
}

namespace {

constexpr uint64_t kMaxPacket = 2048;

// HMAC-SHA1 of `data` followed by a 4 byte trailer (the roll-over counter; empty for SRTCP, whose index is part of the data).
bool mac_of(const uint8_t key[20], ConstByteView data, bool with_trailer, uint32_t trailer, uint8_t out[20]) {
  uint8_t buffer[kMaxPacket + 4];
  if (data.size() > kMaxPacket) return false;
  ByteView(buffer, sizeof(buffer)).first(data.size()).copy_from(data);
  uint64_t n = data.size();
  if (with_trailer) {
    store_be32(ByteView(buffer, sizeof(buffer)), n, trailer);
    n += 4;
  }
  crypto::hmac(crypto::HashKind::Sha1, ConstByteView(key, 20), ConstByteView(buffer, n), out);
  return true;
}

// The header of an SRTP packet whose payload is still encrypted (the padding flag is cleared while it is parsed: its count is inside the encrypted part).
bool header_of(ByteBuffer* packet, uint64_t end, RtpHeader* h) {
  ByteView all = packet->writable_view();
  const uint8_t first = all[0];
  all[0] = first & ~0x20;
  const bool ok = rtp_parse(packet->view().first(end), h);
  all[0] = first;
  return ok;
}

}  // namespace

bool Srtp::protect_rtp(ByteBuffer* packet) {
  RtpHeader h;
  if (!ready_ || !header_of(packet, packet->size(), &h)) return false;
  Stream& s = stream_for(streams_, h.ssrc);
  if (s.seen && h.sequence < s.last_sequence && s.last_sequence - h.sequence > 0x8000) ++s.roc;   // the sender's sequence number wrapped
  s.last_sequence = h.sequence;
  s.seen = true;
  const uint64_t index = (uint64_t{s.roc} << 16) | h.sequence, header = h.header_size, end = packet->size(), tag = tag_size(false);
  uint8_t iv[16];
  rtp_iv(h.ssrc, index, iv);
  if (gcm()) {
    if (!packet->put_zeros(tag)) return false;
    uint8_t t[16];
    ByteView all = packet->writable_view();
    rtp_gcm_.seal(iv, ConstByteView(all.data(), header), all.subview(header, end - header), t);
    all.subview(end, 16).copy_from(ConstByteView(t, 16));
    return true;
  }
  aes_cm(rtp_aes_, iv, packet->writable_view().subview(header, end - header));
  uint8_t mac[20];
  if (!mac_of(rtp_auth_, packet->view(), true, s.roc, mac)) return false;
  return packet->append(ConstByteView(mac, tag));
}

bool Srtp::unprotect_rtp(ByteBuffer* packet) {
  const uint64_t tag = tag_size(false);
  if (!ready_ || packet->size() < kRtpHeader + tag) return false;
  const uint64_t body_end = packet->size() - tag;
  RtpHeader h;
  if (!header_of(packet, body_end, &h)) return false;
  Stream& s = stream_for(streams_, h.ssrc);
  uint32_t roc = s.roc;
  if (s.seen) {   // RFC 3711 appendix A: the packet index nearest to the highest one so far
    if (s.last_sequence < 0x8000) {
      if (h.sequence > s.last_sequence + 0x8000 && roc > 0) --roc;
    } else if (h.sequence < s.last_sequence - 0x8000) {
      ++roc;
    }
  }
  const uint64_t index = (uint64_t{roc} << 16) | h.sequence;
  if (replayed(s, index)) return false;
  uint8_t iv[16];
  rtp_iv(h.ssrc, index, iv);
  ByteView all = packet->writable_view();
  if (gcm()) {
    uint8_t t[16];
    ByteView(t, 16).copy_from(all.subview(body_end, 16));
    if (!rtp_gcm_.open(iv, ConstByteView(all.data(), h.header_size), all.subview(h.header_size, body_end - h.header_size), t)) return false;
  } else {
    uint8_t mac[20];
    if (!mac_of(rtp_auth_, packet->view().first(body_end), true, roc, mac) || !crypto::equal_constant_time(mac, all.data() + body_end, static_cast<uint32_t>(tag))) return false;
    aes_cm(rtp_aes_, iv, all.subview(h.header_size, body_end - h.header_size));
  }
  accept(s, index);
  if (index == s.highest) {   // the newest packet so far: the counter and sequence number follow it
    s.roc = roc;
    s.last_sequence = h.sequence;
  }
  packet->truncate(body_end);
  return true;
}

bool Srtp::protect_rtcp(ByteBuffer* packet) {
  if (!ready_ || packet->size() < 8 || (packet->view()[0] >> 6) != 2) return false;
  const uint32_t ssrc = load_be32(packet->view(), 4), index = rtcp_index_++ & 0x7FFFFFFF, word = 0x80000000u | index;
  const uint64_t end = packet->size(), tag = tag_size(true);
  uint8_t iv[16];
  rtcp_iv(ssrc, index, iv);
  if (gcm()) {
    uint8_t aad[12], t[16];
    ByteView(aad, 12).first(8).copy_from(packet->view().first(8));
    store_be32(ByteView(aad, 12), 8, word);
    if (!packet->put_zeros(16)) return false;
    ByteView all = packet->writable_view();
    rtcp_gcm_.seal(iv, ConstByteView(aad, 12), all.subview(8, end - 8), t);
    all.subview(end, 16).copy_from(ConstByteView(t, 16));
    return put_be32(packet, word);
  }
  aes_cm(rtcp_aes_, iv, packet->writable_view().subview(8, end - 8));
  if (!put_be32(packet, word)) return false;
  uint8_t mac[20];
  if (!mac_of(rtcp_auth_, packet->view(), false, 0, mac)) return false;
  return packet->append(ConstByteView(mac, tag));
}

bool Srtp::unprotect_rtcp(ByteBuffer* packet) {
  const uint64_t tag = tag_size(true), trailer = gcm() ? 4 : 4 + tag;   // GCM: the tag sits before the index word
  if (!ready_ || packet->size() < 8 + trailer + (gcm() ? tag : 0) || (packet->view()[0] >> 6) != 2) return false;
  const uint64_t size = packet->size(), word_at = gcm() ? size - 4 : size - tag - 4;
  const uint32_t word = load_be32(packet->view(), word_at), index = word & 0x7FFFFFFF, ssrc = load_be32(packet->view(), 4);
  Stream& s = stream_for(rtcp_streams_, ssrc);
  if (replayed(s, index)) return false;
  uint8_t iv[16];
  rtcp_iv(ssrc, index, iv);
  ByteView all = packet->writable_view();
  uint64_t payload_end;
  if (gcm()) {
    uint8_t aad[12], t[16];
    payload_end = size - 4 - 16;
    ByteView(aad, 12).first(8).copy_from(packet->view().first(8));
    store_be32(ByteView(aad, 12), 8, word);
    ByteView(t, 16).copy_from(all.subview(payload_end, 16));
    if (!(word & 0x80000000u)) return false;   // (an unencrypted GCM packet carries no use here)
    if (!rtcp_gcm_.open(iv, ConstByteView(aad, 12), all.subview(8, payload_end - 8), t)) return false;
  } else {
    payload_end = word_at;
    uint8_t mac[20];
    if (!mac_of(rtcp_auth_, packet->view().first(size - tag), false, 0, mac) || !crypto::equal_constant_time(mac, all.data() + size - tag, static_cast<uint32_t>(tag))) return false;
    if (word & 0x80000000u) aes_cm(rtcp_aes_, iv, all.subview(8, payload_end - 8));
  }
  accept(s, index);
  packet->truncate(payload_end);
  return true;
}

}  // namespace rtc
