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
    // AES-CBC with an HMAC (RFC 5289, RFC 4492): for servers that have nothing better
    {0xc027, "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256", AeadKind::Aes128Cbc, HashKind::Sha256, false, SuiteAuth::Rsa, HashKind::Sha256},
    {0xc028, "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384", AeadKind::Aes256Cbc, HashKind::Sha384, false, SuiteAuth::Rsa, HashKind::Sha384},
    {0xc023, "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256", AeadKind::Aes128Cbc, HashKind::Sha256, false, SuiteAuth::Ecdsa, HashKind::Sha256},
    {0xc024, "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384", AeadKind::Aes256Cbc, HashKind::Sha384, false, SuiteAuth::Ecdsa, HashKind::Sha384},
    {0xc013, "TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA", AeadKind::Aes128Cbc, HashKind::Sha256, false, SuiteAuth::Rsa, HashKind::Sha1},
    {0xc014, "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA", AeadKind::Aes256Cbc, HashKind::Sha256, false, SuiteAuth::Rsa, HashKind::Sha1},
    {0xc009, "TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA", AeadKind::Aes128Cbc, HashKind::Sha256, false, SuiteAuth::Ecdsa, HashKind::Sha1},
    {0xc00a, "TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA", AeadKind::Aes256Cbc, HashKind::Sha256, false, SuiteAuth::Ecdsa, HashKind::Sha1},
};

// HMAC over the concatenation of three pieces (a record's pseudo-header and its data, which are not next to each other in memory).
void hmac3(HashKind kind, ConstByteView key, ConstByteView a, ConstByteView b, uint8_t* out) {
  const uint32_t block = hash_block_size(kind), size = hash_size(kind);
  uint8_t k[kMaxHashBlock] = {0}, pad[kMaxHashBlock], inner[kMaxHashSize];
  for (uint64_t i = 0; i < key.size() && i < block; ++i) k[i] = key[i];   // (the MAC keys of the suites are no longer than the block)
  for (uint32_t i = 0; i < block; ++i) pad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
  Hasher h(kind);
  h.update(ConstByteView(pad, block));
  h.update(a);
  h.update(b);
  h.finish(inner);
  for (uint32_t i = 0; i < block; ++i) pad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
  Hasher o(kind);
  o.update(ConstByteView(pad, block));
  o.update(ConstByteView(inner, size));
  o.finish(out);
}
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
  if (kind == AeadKind::Aes128Cbc || kind == AeadKind::Aes256Cbc) return false;   // (set_cbc_keys())
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

bool RecordProtection::set_cbc_keys(AeadKind kind, HashKind mac, const uint8_t* key, const uint8_t* mac_key, bool encrypt_then_mac, crypto::IRandom* random) {
  if ((kind != AeadKind::Aes128Cbc && kind != AeadKind::Aes256Cbc) || !random || !aes_.set_key(ConstByteView(key, aead_key_size(kind)))) return false;
  kind_ = kind;
  tls13_ = false;
  mac_hash_ = mac;
  mac_size_ = hash_size(mac);
  for (uint32_t i = 0; i < mac_size_; ++i) mac_key_[i] = mac_key[i];
  etm_ = encrypt_then_mac;
  random_ = random;
  iv_length_ = 0;
  sequence_ = 0;
  active_ = true;
  return true;
}

// The MAC of a record: HMAC(mac_key, sequence number | type | version | length | data) (RFC 5246 6.2.3.1); `length_field` is what the length field says.
void RecordProtection::cbc_mac(uint8_t type, uint32_t length_field, ConstByteView data, uint8_t* out) const {
  uint8_t header[13];
  for (int i = 0; i < 8; ++i) header[i] = static_cast<uint8_t>(sequence_ >> (56 - 8 * i));
  header[8] = type;
  header[9] = 3;
  header[10] = 3;
  header[11] = static_cast<uint8_t>(length_field >> 8);
  header[12] = static_cast<uint8_t>(length_field);
  hmac3(mac_hash_, ConstByteView(mac_key_, mac_size_), ConstByteView(header, 13), data, out);
}

// MAC-then-encrypt: IV | CBC(data | MAC | padding); encrypt-then-MAC (RFC 7366): IV | CBC(data | padding) | MAC over IV and ciphertext. The padding is 1 to 16 bytes, each
// holding the number of them less one. The IV is random for every record (a predictable one would allow chosen-plaintext attacks).
uint32_t RecordProtection::seal_cbc(uint8_t type, ConstByteView plain, uint8_t* out, uint32_t capacity) {
  const uint32_t n = static_cast<uint32_t>(plain.size());
  const uint32_t body_plain = etm_ ? n : n + mac_size_;
  const uint32_t pad = 16 - (body_plain % 16);
  const uint32_t cipher_length = 16 + body_plain + pad;
  const uint32_t payload = cipher_length + (etm_ ? mac_size_ : 0);
  if (capacity < kRecordHeader + payload || payload > 0xffff) return 0;
  uint8_t* iv = out + kRecordHeader;
  uint8_t* data = iv + 16;
  if (!random_->fill(ByteView(iv, 16))) return 0;
  for (uint32_t i = 0; i < n; ++i) data[i] = plain[i];
  if (!etm_) cbc_mac(type, n, plain, data + n);
  for (uint32_t i = 0; i < pad; ++i) data[body_plain + i] = static_cast<uint8_t>(pad - 1);
  aes_.cbc(iv, ByteView(data, body_plain + pad), false);
  if (etm_) cbc_mac(type, cipher_length, ConstByteView(iv, cipher_length), iv + cipher_length);
  out[0] = type;
  out[1] = 3;
  out[2] = 3;
  out[3] = static_cast<uint8_t>(payload >> 8);
  out[4] = static_cast<uint8_t>(payload);
  ++sequence_;
  return kRecordHeader + payload;
}

// The checks run over the same amount of data whether the padding is right or not, and a record that fails is reported one way (the caller sends bad_record_mac)
// whatever was wrong, as RFC 5246 6.2.3.2 asks; the time the HMAC takes still depends on the length the padding claims (the "Lucky Thirteen" leak), which is why
// encrypt-then-MAC is offered first and AEAD suites ahead of both.
bool RecordProtection::open_cbc(uint8_t header_type, uint8_t* payload, uint32_t length, ConstByteView* plain) {
  uint8_t expected[kMaxHashSize];
  if (etm_) {
    if (length < 32 + mac_size_ || (length - mac_size_) % 16 != 0) return false;
    const uint32_t cipher_length = length - mac_size_;
    cbc_mac(header_type, cipher_length, ConstByteView(payload, cipher_length), expected);
    if (!equal_constant_time(expected, payload + cipher_length, mac_size_)) return false;
    uint8_t* data = payload + 16;
    const uint32_t data_length = cipher_length - 16;
    aes_.cbc(payload, ByteView(data, data_length), true);
    const uint32_t pad = data[data_length - 1];
    if (pad + 1 > data_length) return false;
    uint8_t diff = 0;
    for (uint32_t i = 0; i <= pad; ++i) diff = static_cast<uint8_t>(diff | (data[data_length - 1 - i] ^ pad));
    if (diff) return false;
    *plain = ConstByteView(data, data_length - pad - 1);
    return true;
  }
  if (length < 32 || (length - 16) % 16 != 0 || length - 16 < mac_size_ + 1) return false;
  uint8_t* data = payload + 16;
  const uint32_t data_length = length - 16;
  aes_.cbc(payload, ByteView(data, data_length), true);
  uint32_t pad = data[data_length - 1];
  bool good = pad + 1 + mac_size_ <= data_length;
  if (!good) pad = 0;   // (MAC the data as if the padding were a single byte, then fail)
  uint8_t diff = 0;
  for (uint32_t i = 0; i <= pad; ++i) diff = static_cast<uint8_t>(diff | (data[data_length - 1 - i] ^ pad));
  const uint32_t plain_length = data_length - mac_size_ - pad - 1;
  cbc_mac(header_type, plain_length, ConstByteView(data, plain_length), expected);
  const bool mac_ok = equal_constant_time(expected, data + plain_length, mac_size_);
  if (!(good & (diff == 0) & mac_ok)) return false;
  *plain = ConstByteView(data, plain_length);
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
  if (cbc()) return seal_cbc(type, plain, out, capacity);
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
  if (cbc()) {
    if (!open_cbc(header_type, payload, length, plain)) return false;
    ++sequence_;
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
