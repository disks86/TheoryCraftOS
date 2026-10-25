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

#include "Rtc/Stun.hpp"
#include "Crypto/Crypto.hpp"
#include "Utility/Crc32.hpp"

namespace rtc {

namespace {

constexpr uint32_t kFingerprintXor = 0x5354554e;

uint64_t padded(uint64_t n) { return (n + 3) & ~uint64_t{3}; }

void set_length(ByteView message, uint64_t length) { store_be16(message, 2, static_cast<uint32_t>(length)); }

}  // namespace

bool stun_looks_like(ConstByteView d) {
  if (d.size() < kStunHeader || (d[0] & 0xC0) != 0 || load_be32(d, 4) != kStunCookie) return false;
  return (load_be16(d, 2) & 3) == 0 && load_be16(d, 2) + uint64_t{kStunHeader} <= d.size();
}

bool stun_parse(ConstByteView d, StunMessage* out) {
  if (!stun_looks_like(d)) return false;
  out->type = load_be16(d, 0);
  for (uint32_t i = 0; i < 12; ++i) out->transaction[i] = d[8 + i];
  out->has_mapped = out->use_candidate = out->has_priority = out->has_controlling = out->has_controlled = out->has_error = out->has_integrity = out->fingerprint_ok = false;
  out->username.clear();
  out->software.clear();
  const uint64_t end = kStunHeader + load_be16(d, 2);
  uint64_t at = kStunHeader;
  while (at < end) {
    if (end - at < 4) return false;
    const uint16_t type = load_be16(d, at);
    const uint64_t length = load_be16(d, at + 2), body = at + 4;
    if (length > end - body) return false;
    const ConstByteView value = d.subview(body, length);
    switch (type) {
      case kStunUsername:
        if (length > 513 || !out->username.assign(StringView(reinterpret_cast<const char*>(value.data()), value.size()))) return false;
        break;
      case kStunSoftware:
        if (length > 763 || !out->software.assign(StringView(reinterpret_cast<const char*>(value.data()), value.size()))) return false;
        break;
      case kStunPriority:
        if (length != 4) return false;
        out->has_priority = true;
        out->priority = load_be32(value, 0);
        break;
      case kStunUseCandidate: out->use_candidate = true; break;
      case kStunIceControlling:
      case kStunIceControlled:
        if (length != 8) return false;
        (type == kStunIceControlling ? out->has_controlling : out->has_controlled) = true;
        out->tie_breaker = (uint64_t{load_be32(value, 0)} << 32) | load_be32(value, 4);
        break;
      case kStunXorMapped:
        if (length < 8 || value[1] != 1) break;   // (IPv4 only: the stack has no IPv6)
        out->has_mapped = true;
        out->mapped_port = static_cast<uint16_t>(load_be16(value, 2) ^ (kStunCookie >> 16));
        out->mapped = network::Ipv4Address::from_value(load_be32(value, 4) ^ kStunCookie);
        break;
      case kStunErrorCode:
        if (length < 4) return false;
        out->has_error = true;
        out->error_code = static_cast<uint16_t>((value[2] & 7) * 100 + value[3]);
        break;
      case kStunIntegrity:
        if (length != 20) return false;
        out->has_integrity = true;
        out->integrity_offset = at;
        break;
      case kStunFingerprint: {
        if (length != 4 || body + 4 != end) return false;   // (it is the last attribute)
        uint8_t copy[1024];
        if (at > sizeof(copy)) return false;
        ByteView(copy, at).copy_from(d.first(at));
        store_be16(ByteView(copy, at), 2, static_cast<uint32_t>(at - kStunHeader + 8));
        out->fingerprint_ok = (crc32(ConstByteView(copy, at)) ^ kFingerprintXor) == load_be32(value, 0);
        break;
      }
      default: break;
    }
    at = body + padded(length);
  }
  return at == end;
}

bool stun_check_integrity(ConstByteView d, const StunMessage& parsed, ConstByteView key) {
  if (!parsed.has_integrity || parsed.integrity_offset + 24 > d.size()) return false;
  uint8_t copy[1024], mac[20];
  const uint64_t at = parsed.integrity_offset;
  if (at > sizeof(copy)) return false;
  ByteView(copy, at).copy_from(d.first(at));
  store_be16(ByteView(copy, at), 2, static_cast<uint32_t>(at - kStunHeader + 24));   // (the length counts the integrity attribute but nothing after it)
  crypto::hmac(crypto::HashKind::Sha1, key, ConstByteView(copy, at), mac);
  return crypto::equal_constant_time(mac, d.data() + at + 4, 20);
}

bool stun_begin(ByteBuffer* out, uint16_t type, const uint8_t transaction[12]) {
  out->clear();
  return put_be16(out, type) && put_be16(out, 0) && put_be32(out, kStunCookie) && out->append(ConstByteView(transaction, 12));
}

bool stun_attribute(ByteBuffer* out, uint16_t type, ConstByteView value) {
  if (value.size() > 0xFFFF || !put_be16(out, type) || !put_be16(out, static_cast<uint32_t>(value.size())) || !out->append(value)) return false;
  return out->put_zeros(padded(value.size()) - value.size());
}

bool stun_text_attribute(ByteBuffer* out, uint16_t type, StringView text) { return stun_attribute(out, type, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size())); }

bool stun_xor_address(ByteBuffer* out, uint16_t type, network::Ipv4Address address, uint16_t port, const uint8_t[12]) {
  uint8_t v[8] = {0, 1};
  store_be16(ByteView(v, 8), 2, port ^ (kStunCookie >> 16));
  store_be32(ByteView(v, 8), 4, address.value() ^ kStunCookie);
  return stun_attribute(out, type, ConstByteView(v, 8));
}

bool stun_error_attribute(ByteBuffer* out, uint16_t code, StringView reason) {
  uint8_t v[4 + 128] = {0, 0, static_cast<uint8_t>(code / 100), static_cast<uint8_t>(code % 100)};
  const uint64_t n = reason.size() < 128 ? reason.size() : 128;
  ByteView(v, sizeof(v)).subview(4, n).copy_from(ConstByteView(reinterpret_cast<const uint8_t*>(reason.data()), n));
  return stun_attribute(out, kStunErrorCode, ConstByteView(v, 4 + n));
}

bool stun_finish(ByteBuffer* out, ConstByteView key, bool fingerprint) {
  if (out->size() < kStunHeader) return false;
  if (!key.empty()) {
    set_length(out->writable_view(), out->size() - kStunHeader + 24);
    uint8_t mac[20];
    crypto::hmac(crypto::HashKind::Sha1, key, out->view(), mac);
    if (!stun_attribute(out, kStunIntegrity, ConstByteView(mac, 20))) return false;
  }
  if (fingerprint) {
    set_length(out->writable_view(), out->size() - kStunHeader + 8);
    uint8_t v[4];
    store_be32(ByteView(v, 4), 0, crc32(out->view()) ^ kFingerprintXor);
    if (!stun_attribute(out, kStunFingerprint, ConstByteView(v, 4))) return false;
  }
  set_length(out->writable_view(), out->size() - kStunHeader);
  return true;
}

}  // namespace rtc
