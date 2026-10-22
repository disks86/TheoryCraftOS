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

#include "Network/Quic/QuicCrypto.hpp"

namespace network::quic {

using namespace crypto;
using namespace tls;

bool PacketKeys::set_secret(AeadKind kind, HashKind hash, ConstByteView secret) {
  const uint32_t hl = hash_size(hash);
  if (secret.size() != hl) return false;
  uint8_t key[32];
  const uint32_t key_length = aead_key_size(kind);
  if (!hkdf_expand_label(hash, secret, "quic key", ConstByteView(), ByteView(key, key_length)) || !hkdf_expand_label(hash, secret, "quic iv", ConstByteView(), ByteView(iv_, 12)) ||
      !hkdf_expand_label(hash, secret, "quic hp", ConstByteView(), ByteView(hp_key_, key_length)))
    return false;
  if (kind == AeadKind::ChaCha20Poly1305) {
    if (!chacha_.set_key(ConstByteView(key, 32))) return false;
  } else {
    if (!gcm_.set_key(ConstByteView(key, key_length)) || !hp_aes_.set_key(ConstByteView(hp_key_, key_length))) return false;
  }
  for (uint8_t& b : key) b = 0;
  kind_ = kind;
  hash_ = hash;
  for (uint32_t i = 0; i < hl; ++i) secret_[i] = secret[i];
  valid_ = true;
  return true;
}

bool PacketKeys::next_generation(PacketKeys* next) const {
  if (!valid_) return false;
  const uint32_t hl = hash_size(hash_);
  uint8_t next_secret[kMaxHashSize];
  if (!hkdf_expand_label(hash_, ConstByteView(secret_, hl), "quic ku", ConstByteView(), ByteView(next_secret, hl))) return false;
  // key and IV come from the new secret; the header protection key is kept
  uint8_t keep[32];
  for (uint32_t i = 0; i < 32; ++i) keep[i] = hp_key_[i];
  if (!next->set_secret(kind_, hash_, ConstByteView(next_secret, hl))) return false;
  for (uint32_t i = 0; i < 32; ++i) next->hp_key_[i] = keep[i];
  if (kind_ != AeadKind::ChaCha20Poly1305 && !next->hp_aes_.set_key(ConstByteView(keep, aead_key_size(kind_)))) return false;
  for (uint8_t& b : next_secret) b = 0;
  return true;
}

void PacketKeys::wipe() {
  valid_ = false;
  for (uint8_t& b : iv_) b = 0;
  for (uint8_t& b : hp_key_) b = 0;
  for (uint8_t& b : secret_) b = 0;
}

void PacketKeys::nonce(uint64_t packet_number, uint8_t out[12]) const {
  for (int i = 0; i < 12; ++i) out[i] = iv_[i];
  for (int i = 0; i < 8; ++i) out[4 + i] ^= static_cast<uint8_t>(packet_number >> (56 - 8 * i));
}

void PacketKeys::seal(uint64_t packet_number, ConstByteView header, ByteView payload, uint8_t tag[16]) const {
  uint8_t n[12];
  nonce(packet_number, n);
  if (kind_ == AeadKind::ChaCha20Poly1305) chacha_.seal(n, header, payload, tag);
  else gcm_.seal(n, header, payload, tag);
}

bool PacketKeys::open(uint64_t packet_number, ConstByteView header, ByteView payload, const uint8_t tag[16]) const {
  uint8_t n[12];
  nonce(packet_number, n);
  return kind_ == AeadKind::ChaCha20Poly1305 ? chacha_.open(n, header, payload, tag) : gcm_.open(n, header, payload, tag);
}

void PacketKeys::header_mask(const uint8_t sample[16], uint8_t mask[5]) const {
  if (kind_ == AeadKind::ChaCha20Poly1305) {
    // the first four sample bytes are the block counter (little endian), the rest the nonce; the mask is the first five bytes of that block
    const uint32_t counter = uint32_t{sample[0]} | (uint32_t{sample[1]} << 8) | (uint32_t{sample[2]} << 16) | (uint32_t{sample[3]} << 24);
    uint8_t zeros[5] = {};
    chacha20_xor(hp_key_, counter, sample + 4, ByteView(zeros));
    for (int i = 0; i < 5; ++i) mask[i] = zeros[i];
  } else {
    uint8_t block[16];
    hp_aes_.encrypt_block(sample, block);
    for (int i = 0; i < 5; ++i) mask[i] = block[i];
  }
}

bool protect_packet(const PacketKeys& keys, uint64_t packet_number, ByteView packet, uint32_t pn_offset, uint32_t pn_length) {
  const uint32_t header_length = pn_offset + pn_length;
  if (pn_length < 1 || pn_length > 4 || packet.size() < header_length + 16) return false;
  const uint64_t payload_length = packet.size() - header_length - 16;
  // header protection samples 16 bytes starting 4 bytes after the start of the packet number: the payload must reach that far
  if (pn_offset + 4 + 16 > packet.size()) return false;
  keys.seal(packet_number, ConstByteView(packet.data(), header_length), packet.subview(header_length, payload_length), packet.data() + header_length + payload_length);
  uint8_t mask[5];
  keys.header_mask(packet.data() + pn_offset + 4, mask);
  const bool long_header = (packet[0] & 0x80) != 0;
  packet[0] = static_cast<uint8_t>(packet[0] ^ (mask[0] & (long_header ? 0x0F : 0x1F)));
  for (uint32_t i = 0; i < pn_length; ++i) packet[pn_offset + i] = static_cast<uint8_t>(packet[pn_offset + i] ^ mask[1 + i]);
  return true;
}

bool remove_header_protection(const PacketKeys& keys, ByteView packet, uint32_t pn_offset, bool long_header, uint32_t* pn_length, uint64_t* truncated) {
  if (pn_offset + 4 + 16 > packet.size()) return false;
  uint8_t mask[5];
  keys.header_mask(packet.data() + pn_offset + 4, mask);
  packet[0] = static_cast<uint8_t>(packet[0] ^ (mask[0] & (long_header ? 0x0F : 0x1F)));
  const uint32_t n = (packet[0] & 3) + 1;
  uint64_t value = 0;
  for (uint32_t i = 0; i < n; ++i) {
    packet[pn_offset + i] = static_cast<uint8_t>(packet[pn_offset + i] ^ mask[1 + i]);
    value = (value << 8) | packet[pn_offset + i];
  }
  *pn_length = n;
  *truncated = value;
  return true;
}

uint64_t expand_packet_number(int64_t largest, uint64_t truncated, uint32_t pn_length) {
  const uint64_t expected = static_cast<uint64_t>(largest + 1);
  const uint64_t window = uint64_t{1} << (8 * pn_length), half = window / 2, mask = window - 1;
  uint64_t candidate = (expected & ~mask) | truncated;
  if (candidate + half <= expected && candidate < (uint64_t{1} << 62) - window) return candidate + window;
  if (candidate > expected + half && candidate >= window) return candidate - window;
  return candidate;
}

bool decrypt_packet(const PacketKeys& keys, uint64_t packet_number, ByteView packet, uint32_t header_length) {
  if (packet.size() < header_length + 16) return false;
  const uint64_t payload_length = packet.size() - header_length - 16;
  return keys.open(packet_number, ConstByteView(packet.data(), header_length), packet.subview(header_length, payload_length), packet.data() + header_length + payload_length);
}

bool derive_initial_keys(ConstByteView dcid, PacketKeys* client_write, PacketKeys* server_write) {
  static const uint8_t kSalt[20] = {0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17, 0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a};
  uint8_t initial[32], client[32], server[32];
  hkdf_extract(HashKind::Sha256, ConstByteView(kSalt), dcid, initial);
  if (!hkdf_expand_label(HashKind::Sha256, ConstByteView(initial, 32), "client in", ConstByteView(), ByteView(client, 32)) ||
      !hkdf_expand_label(HashKind::Sha256, ConstByteView(initial, 32), "server in", ConstByteView(), ByteView(server, 32)))
    return false;
  return client_write->set_secret(AeadKind::Aes128Gcm, HashKind::Sha256, ConstByteView(client, 32)) && server_write->set_secret(AeadKind::Aes128Gcm, HashKind::Sha256, ConstByteView(server, 32));
}

bool retry_integrity_tag(ConstByteView odcid, ConstByteView retry, uint8_t tag[16]) {
  static const uint8_t kKey[16] = {0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a, 0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e};
  static const uint8_t kNonce[12] = {0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63, 0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb};
  // the associated data is the "retry pseudo-packet": the length and bytes of the original ID, then the Retry packet without its tag
  uint8_t storage[1 + 20 + 1500];
  ByteWriter w{ByteView(storage)};
  if (odcid.size() > 20 || retry.size() > 1500) return false;
  w.u8(static_cast<uint8_t>(odcid.size()));
  w.bytes(odcid);
  w.bytes(retry);
  AesGcm gcm;
  if (!w.ok() || !gcm.set_key(ConstByteView(kKey))) return false;
  gcm.seal(kNonce, w.written(), ByteView(), tag);
  return true;
}

}  // namespace network::quic
