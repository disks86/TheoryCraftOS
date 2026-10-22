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

// QUIC packet protection (RFC 9001 5): the keys of one direction at one encryption level - AEAD key, IV and header protection key made from a TLS
// traffic secret - sealing and opening packet payloads, masking the header bytes that are protected, the Initial secrets that everyone can compute, the key
// update, and the integrity tag of a Retry packet.
#pragma once
#include "Crypto/Aead.hpp"
#include "Crypto/Crypto.hpp"
#include "Network/Tls/TlsKeys.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteStream.hpp"

namespace network::quic {

class PacketKeys {
 public:
  bool valid() const { return valid_; }
  // Derives the keys from a traffic secret (hash length bytes): "quic key", "quic iv" and "quic hp" expansions of it.
  bool set_secret(tls::AeadKind kind, crypto::HashKind hash, ConstByteView secret);
  // The keys of the next key phase (RFC 9001 6): a new secret, key and IV; the header protection key stays.
  bool next_generation(PacketKeys* next) const;
  void wipe();

  // Encrypts `payload` where it is and writes the 16-byte tag; `header` (as it is on the wire before header protection) is the associated data.
  void seal(uint64_t packet_number, ConstByteView header, ByteView payload, uint8_t tag[16]) const;
  // Checks the tag and decrypts `payload` where it is. False if it does not authenticate (nothing is changed then).
  bool open(uint64_t packet_number, ConstByteView header, ByteView payload, const uint8_t tag[16]) const;
  // The five mask bytes for the 16-byte ciphertext sample of a packet: the first protects the flags, the others the packet number.
  void header_mask(const uint8_t sample[16], uint8_t mask[5]) const;

 private:
  void nonce(uint64_t packet_number, uint8_t out[12]) const;

  bool valid_ = false;
  tls::AeadKind kind_ = tls::AeadKind::Aes128Gcm;
  crypto::HashKind hash_ = crypto::HashKind::Sha256;
  crypto::AesGcm gcm_;
  crypto::ChaCha20Poly1305 chacha_;
  crypto::Aes hp_aes_;
  uint8_t hp_key_[32] = {};
  uint8_t iv_[12] = {};
  uint8_t secret_[48] = {};
};

// Packets as a whole. A packet in a buffer is: the header (flags, IDs and so on, ending with the packet number of `pn_length` bytes at `pn_offset`), the
// payload, and 16 bytes of tag.
// Seals the payload and applies header protection; `packet` is header + payload + 16 spare bytes, the header written as it is to be sent.
bool protect_packet(const PacketKeys& keys, uint64_t packet_number, ByteView packet, uint32_t pn_offset, uint32_t pn_length);
// Removes header protection from a received packet (changing its flags byte and packet number bytes where they are): the packet number length and the truncated
// number that were in it. `long_header` says how many flag bits are protected. False if the packet is too short to hold a sample.
bool remove_header_protection(const PacketKeys& keys, ByteView packet, uint32_t pn_offset, bool long_header, uint32_t* pn_length, uint64_t* truncated_packet_number);
// The full packet number from its truncated form, given the largest one received so far (RFC 9000 A.3); `largest` is -1 before any.
uint64_t expand_packet_number(int64_t largest, uint64_t truncated, uint32_t pn_length);
// Checks and decrypts the payload (the bytes after `header_length` up to the 16-byte tag at the end), whose header protection has been removed.
bool decrypt_packet(const PacketKeys& keys, uint64_t packet_number, ByteView packet, uint32_t header_length);

// The keys both sides use for the first packets, from the destination connection ID the client chose (RFC 9001 5.2): what the client writes and what the server writes.
bool derive_initial_keys(ConstByteView destination_connection_id, PacketKeys* client_write, PacketKeys* server_write);

// The tag that ends a Retry packet (RFC 9001 5.8): `retry_without_tag` is the packet up to the tag, `original_destination_connection_id` the one the client first used.
bool retry_integrity_tag(ConstByteView original_destination_connection_id, ConstByteView retry_without_tag, uint8_t tag[16]);

}  // namespace network::quic
