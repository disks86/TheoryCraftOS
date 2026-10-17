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

#include "Network/Tls/TlsKeys.hpp"

namespace network::tls {

using namespace crypto;

namespace {
const CipherSuite kSuites[] = {
    {0x1301, "TLS_AES_128_GCM_SHA256", AeadKind::Aes128Gcm, HashKind::Sha256, true, SuiteAuth::Any},
    {0x1302, "TLS_AES_256_GCM_SHA384", AeadKind::Aes256Gcm, HashKind::Sha384, true, SuiteAuth::Any},
    {0x1303, "TLS_CHACHA20_POLY1305_SHA256", AeadKind::ChaCha20Poly1305, HashKind::Sha256, true, SuiteAuth::Any},
    {0xc02b, "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256", AeadKind::Aes128Gcm, HashKind::Sha256, false, SuiteAuth::Ecdsa},
    {0xc02c, "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", AeadKind::Aes256Gcm, HashKind::Sha384, false, SuiteAuth::Ecdsa},
    {0xc02f, "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256", AeadKind::Aes128Gcm, HashKind::Sha256, false, SuiteAuth::Rsa},
    {0xc030, "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384", AeadKind::Aes256Gcm, HashKind::Sha384, false, SuiteAuth::Rsa},
    {0xcca8, "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256", AeadKind::ChaCha20Poly1305, HashKind::Sha256, false, SuiteAuth::Rsa},
    {0xcca9, "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256", AeadKind::ChaCha20Poly1305, HashKind::Sha256, false, SuiteAuth::Ecdsa},
};
}  // namespace

const CipherSuite* find_suite(uint16_t id) {
  for (const CipherSuite& s : kSuites)
    if (s.id == id) return &s;
  return nullptr;
}

void RecordProtection::reset() {
  active_ = false;
  sequence_ = 0;
  for (int i = 0; i < 12; ++i) iv_[i] = 0;
}

bool RecordProtection::set_keys(AeadKind kind, bool tls13, const uint8_t* key, const uint8_t* iv, uint32_t iv_length) {
  if ((tls13 && iv_length != 12) || (!tls13 && kind == AeadKind::ChaCha20Poly1305 && iv_length != 12) || (!tls13 && kind != AeadKind::ChaCha20Poly1305 && iv_length != 4)) return false;
  const ConstByteView k(key, aead_key_size(kind));
  if (kind == AeadKind::ChaCha20Poly1305 ? !chacha_.set_key(k) : !gcm_.set_key(k)) return false;
  kind_ = kind;
  tls13_ = tls13;
  iv_length_ = iv_length;
  for (uint32_t i = 0; i < iv_length; ++i) iv_[i] = iv[i];
  sequence_ = 0;
  active_ = true;
  return true;
}

uint32_t RecordProtection::seal(uint8_t type, ConstByteView plain, uint8_t* out, uint32_t capacity) {
  const uint32_t n = static_cast<uint32_t>(plain.size());
  if (!active_) {
    if (capacity < kRecordHeader + n) return 0;
    out[0] = type;
    out[1] = 3;
    out[2] = 3;
    out[3] = static_cast<uint8_t>(n >> 8);
    out[4] = static_cast<uint8_t>(n);
    for (uint32_t i = 0; i < n; ++i) out[kRecordHeader + i] = plain[i];
    return kRecordHeader + n;
  }
  uint8_t nonce[12], aad[13];
  const uint32_t explicit_size = (!tls13_ && kind_ != AeadKind::ChaCha20Poly1305) ? 8 : 0;
  const uint32_t inner = n + (tls13_ ? 1 : 0);   // TLS 1.3 appends the real content type
  const uint32_t payload = explicit_size + inner + kAeadTagSize;
  if (capacity < kRecordHeader + payload || payload > 0xffff) return 0;
  uint8_t* body = out + kRecordHeader + explicit_size;
  for (uint32_t i = 0; i < n; ++i) body[i] = plain[i];
  if (tls13_) body[n] = type;
  // nonce and additional data
  if (!tls13_ && kind_ != AeadKind::ChaCha20Poly1305) {
    for (int i = 0; i < 4; ++i) nonce[i] = iv_[i];
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(sequence_ >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) out[kRecordHeader + i] = nonce[4 + i];
  } else {
    for (int i = 0; i < 12; ++i) nonce[i] = iv_[i];
    for (int i = 0; i < 8; ++i) nonce[4 + i] ^= static_cast<uint8_t>(sequence_ >> (56 - 8 * i));
  }
  if (tls13_) {
    aad[0] = kApplicationData; aad[1] = 3; aad[2] = 3; aad[3] = static_cast<uint8_t>(payload >> 8); aad[4] = static_cast<uint8_t>(payload);
  } else {
    for (int i = 0; i < 8; ++i) aad[i] = static_cast<uint8_t>(sequence_ >> (56 - 8 * i));
    aad[8] = type; aad[9] = 3; aad[10] = 3; aad[11] = static_cast<uint8_t>(n >> 8); aad[12] = static_cast<uint8_t>(n);
  }
  const ConstByteView aad_view(aad, tls13_ ? 5 : 13);
  uint8_t* tag = body + inner;
  if (kind_ == AeadKind::ChaCha20Poly1305) chacha_.seal(nonce, aad_view, ByteView(body, inner), tag);
  else gcm_.seal(nonce, aad_view, ByteView(body, inner), tag);
  out[0] = tls13_ ? kApplicationData : type;
  out[1] = 3;
  out[2] = 3;
  out[3] = static_cast<uint8_t>(payload >> 8);
  out[4] = static_cast<uint8_t>(payload);
  ++sequence_;
  return kRecordHeader + payload;
}

bool RecordProtection::open(uint8_t header_type, uint8_t* payload, uint32_t length, ConstByteView* plain, uint8_t* type) {
  if (!active_) {
    *plain = ConstByteView(payload, length);
    *type = header_type;
    return true;
  }
  const uint32_t explicit_size = (!tls13_ && kind_ != AeadKind::ChaCha20Poly1305) ? 8 : 0;
  if (length < explicit_size + kAeadTagSize) return false;
  const uint32_t inner = length - explicit_size - kAeadTagSize;
  uint8_t nonce[12], aad[13];
  if (explicit_size) {
    for (int i = 0; i < 4; ++i) nonce[i] = iv_[i];
    for (int i = 0; i < 8; ++i) nonce[4 + i] = payload[i];
  } else {
    for (int i = 0; i < 12; ++i) nonce[i] = iv_[i];
    for (int i = 0; i < 8; ++i) nonce[4 + i] ^= static_cast<uint8_t>(sequence_ >> (56 - 8 * i));
  }
  if (tls13_) {
    aad[0] = header_type; aad[1] = 3; aad[2] = 3; aad[3] = static_cast<uint8_t>(length >> 8); aad[4] = static_cast<uint8_t>(length);
  } else {
    for (int i = 0; i < 8; ++i) aad[i] = static_cast<uint8_t>(sequence_ >> (56 - 8 * i));
    aad[8] = header_type; aad[9] = 3; aad[10] = 3; aad[11] = static_cast<uint8_t>(inner >> 8); aad[12] = static_cast<uint8_t>(inner);
  }
  const ConstByteView aad_view(aad, tls13_ ? 5 : 13);
  uint8_t* body = payload + explicit_size;
  const uint8_t* tag = body + inner;
  const bool ok = kind_ == AeadKind::ChaCha20Poly1305 ? chacha_.open(nonce, aad_view, ByteView(body, inner), tag) : gcm_.open(nonce, aad_view, ByteView(body, inner), tag);
  if (!ok) return false;
  ++sequence_;
  if (!tls13_) {
    *plain = ConstByteView(body, inner);
    *type = header_type;
    return true;
  }
  // TLS 1.3: strip the zero padding, the last non-zero byte is the content type
  uint32_t end = inner;
  while (end > 0 && body[end - 1] == 0) --end;
  if (end == 0) return false;
  *type = body[end - 1];
  *plain = ConstByteView(body, end - 1);
  return true;
}

void tls12_prf(HashKind hash, ConstByteView secret, const char* label, ConstByteView seed1, ConstByteView seed2, ByteView out) {
  uint8_t seed[13 + 64 + 64];
  uint32_t n = 0;
  for (const char* c = label; *c; ++c) seed[n++] = static_cast<uint8_t>(*c);
  for (uint64_t i = 0; i < seed1.size() && n < sizeof seed; ++i) seed[n++] = seed1[i];
  for (uint64_t i = 0; i < seed2.size() && n < sizeof seed; ++i) seed[n++] = seed2[i];
  const uint32_t size = hash_size(hash);
  uint8_t a[kMaxHashSize];
  hmac(hash, secret, ConstByteView(seed, n), a);   // A(1)
  for (uint64_t pos = 0; pos < out.size();) {
    uint8_t input[kMaxHashSize + sizeof seed];
    for (uint32_t i = 0; i < size; ++i) input[i] = a[i];
    for (uint32_t i = 0; i < n; ++i) input[size + i] = seed[i];
    uint8_t block[kMaxHashSize];
    hmac(hash, secret, ConstByteView(input, size + n), block);
    const uint64_t take = out.size() - pos < size ? out.size() - pos : size;
    for (uint64_t i = 0; i < take; ++i) out[pos + i] = block[i];
    pos += take;
    hmac(hash, secret, ConstByteView(a, size), a);   // A(i+1)
  }
}

bool hkdf_expand_label(HashKind hash, ConstByteView secret, const char* label, ConstByteView context, ByteView out) {
  uint8_t info[2 + 1 + 6 + 12 + 1 + 64];
  uint32_t n = 0;
  uint32_t label_length = 0;
  while (label[label_length]) ++label_length;
  if (out.size() > 0xffff || label_length > 12 || context.size() > 64) return false;
  info[n++] = static_cast<uint8_t>(out.size() >> 8);
  info[n++] = static_cast<uint8_t>(out.size());
  info[n++] = static_cast<uint8_t>(6 + label_length);
  for (const char* c = "tls13 "; *c; ++c) info[n++] = static_cast<uint8_t>(*c);
  for (uint32_t i = 0; i < label_length; ++i) info[n++] = static_cast<uint8_t>(label[i]);
  info[n++] = static_cast<uint8_t>(context.size());
  for (uint64_t i = 0; i < context.size(); ++i) info[n++] = context[i];
  return hkdf_expand(hash, secret, ConstByteView(info, n), out);
}

bool derive_secret(HashKind hash, ConstByteView secret, const char* label, ConstByteView transcript_hash, uint8_t* out) {
  return hkdf_expand_label(hash, secret, label, transcript_hash, ByteView(out, hash_size(hash)));
}

}  // namespace network::tls
