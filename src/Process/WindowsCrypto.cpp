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
#include "Process/WindowsCrypto.hpp"
#include "Crypto/Aead.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/Ecc.hpp"
#include "Crypto/Random.hpp"
#include "Crypto/Rsa.hpp"
#include "Utility/ByteBuffer.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidParameter = 0xC000000Du, kNoResources = 0xC000009Au, kNotSupported = 0xC00000BBu,
                   kInvalidSignature = 0xC000A000u, kAuthTagMismatch = 0xC000A002u;
constexpr uint32_t kWords = 14;
constexpr uint64_t kMaxData = 64ull << 20, kMaxIterations = 50000000ull, kMaxDerived = 4096;

// Some bytes of the program's memory, copied.
class UserBytes {
 public:
  explicit UserBytes(Process& p) : buffer_(p.files().allocator()), process_(p) {}
  // 0 or a status. A length of 0 leaves the buffer empty and does not look at the address.
  uint32_t fetch(uint64_t address, uint64_t length, uint64_t limit = kMaxData) {
    if (length > limit) return kInvalidParameter;
    if (length == 0) return kSuccess;
    if (!buffer_.put_zeros(length)) return kNoResources;
    return process_.read_user(address, buffer_.writable_view()) ? kSuccess : kAccessViolation;
  }
  ConstByteView view() const { return buffer_.view(); }
  ByteView writable() { return buffer_.writable_view(); }
  uint64_t size() const { return buffer_.size(); }
  bool zeroed(uint64_t length) { return buffer_.put_zeros(length); }

 private:
  ByteBuffer buffer_;
  Process& process_;
};

bool hash_kind(uint64_t code, crypto::HashKind* out) {
  switch (code) {
    case 0: *out = crypto::HashKind::Md5; return true;
    case 1: *out = crypto::HashKind::Sha1; return true;
    case 2: *out = crypto::HashKind::Sha256; return true;
    case 3: *out = crypto::HashKind::Sha384; return true;
    case 4: *out = crypto::HashKind::Sha512; return true;
    case 5: *out = crypto::HashKind::Sha224; return true;
    default: return false;
  }
}

class HostRandom final : public crypto::IRandom {
 public:
  explicit HostRandom(Process& p) : process_(p) {}
  bool fill(ByteView out) override { return process_.host().random_bytes(out); }

 private:
  Process& process_;
};

uint32_t put(Process& p, uint64_t address, ConstByteView bytes) { return p.write_user(address, bytes) ? kSuccess : kAccessViolation; }

uint32_t op_hash(Process& p, const uint64_t* w) {
  uint8_t digest[crypto::kMaxHashSize];
  UserBytes data(p), key(p);
  if (uint32_t s = data.fetch(w[3], w[4])) return s;
  uint32_t size = 0;
  if (w[1] == 6) {      // MD4
    if (w[2] & 1) return kNotSupported;
    crypto::md4(data.view(), digest);
    size = 16;
  } else {
    crypto::HashKind kind;
    if (!hash_kind(w[1], &kind)) return kInvalidParameter;
    size = crypto::hash_size(kind);
    if (w[2] & 1) {
      if (uint32_t s = key.fetch(w[5], w[6])) return s;
      crypto::hmac(kind, key.view(), data.view(), digest);
    } else {
      crypto::hash(kind, data.view(), digest);
    }
  }
  return put(p, w[7], ConstByteView(digest, size));
}

uint32_t op_pbkdf2(Process& p, const uint64_t* w) {
  crypto::HashKind kind;
  if (!hash_kind(w[1], &kind) || w[6] == 0 || w[6] > kMaxIterations || w[8] == 0 || w[8] > kMaxDerived) return kInvalidParameter;
  UserBytes password(p), salt(p);
  if (uint32_t s = password.fetch(w[2], w[3], 1 << 20)) return s;
  if (uint32_t s = salt.fetch(w[4], w[5], 1 << 20)) return s;
  ByteBuffer derived(p.files().allocator());
  if (!derived.put_zeros(w[8])) return kNoResources;
  const uint32_t hlen = crypto::hash_size(kind);
  ByteBuffer message(p.files().allocator());      // salt | INT(i)
  if (!message.put_zeros(salt.size() + 4)) return kNoResources;
  ByteView m = message.writable_view();
  for (uint64_t i = 0; i < salt.size(); ++i) m[i] = salt.view()[i];
  ByteView o = derived.writable_view();
  uint8_t u[crypto::kMaxHashSize], t[crypto::kMaxHashSize];
  for (uint64_t offset = 0, index = 1; offset < o.size(); offset += hlen, ++index) {
    m[salt.size()] = uint8_t(index >> 24); m[salt.size() + 1] = uint8_t(index >> 16); m[salt.size() + 2] = uint8_t(index >> 8); m[salt.size() + 3] = uint8_t(index);
    crypto::hmac(kind, password.view(), ConstByteView(m.data(), m.size()), u);
    for (uint32_t j = 0; j < hlen; ++j) t[j] = u[j];
    for (uint64_t it = 1; it < w[6]; ++it) {
      crypto::hmac(kind, password.view(), ConstByteView(u, hlen), u);
      for (uint32_t j = 0; j < hlen; ++j) t[j] ^= u[j];
    }
    for (uint32_t j = 0; j < hlen && offset + j < o.size(); ++j) o[offset + j] = t[j];
  }
  return put(p, w[7], derived.view());
}

// w[1..]: hash, mode (1: the input is the PRK already), ikm, len, salt, len, info, len, out length, out
uint32_t op_hkdf(Process& p, const uint64_t* w) {
  crypto::HashKind kind;
  if (!hash_kind(w[1], &kind) || w[9] == 0 || w[9] > kMaxDerived) return kInvalidParameter;
  UserBytes ikm(p), salt(p), info(p), out(p);
  if (uint32_t s = ikm.fetch(w[3], w[4], 1 << 20)) return s;
  if (uint32_t s = salt.fetch(w[5], w[6], 1 << 20)) return s;
  if (uint32_t s = info.fetch(w[7], w[8], 1 << 20)) return s;
  if (!out.zeroed(w[9])) return kNoResources;
  uint8_t prk[crypto::kMaxHashSize];
  const uint32_t hlen = crypto::hash_size(kind);
  if (w[2] & 1) {
    if (ikm.size() != hlen) return kInvalidParameter;
    for (uint32_t i = 0; i < hlen; ++i) prk[i] = ikm.view()[i];
  } else {
    crypto::hkdf_extract(kind, salt.view(), ikm.view(), prk);
  }
  if (!crypto::hkdf_expand(kind, ConstByteView(prk, hlen), info.view(), out.writable())) return kInvalidParameter;
  return put(p, w[10], out.view());
}

// w[1..]: mode (0 ECB, 1 CBC, 2 CFB-128, 3 CFB-8), encrypt, key, key len, iv (16 bytes, updated; 0 for ECB), in, out, length
uint32_t op_aes(Process& p, const uint64_t* w) {
  UserBytes key(p), data(p);
  if (w[1] > 3 || w[8] > kMaxData) return kInvalidParameter;
  if (uint32_t s = key.fetch(w[3], w[4], 32)) return s;
  crypto::Aes aes;
  if (!aes.set_key(key.view())) return kInvalidParameter;
  if (uint32_t s = data.fetch(w[6], w[8])) return s;
  const bool encrypt = w[2] != 0;
  uint8_t iv[16] = {};
  if (w[1] != 0) {
    if (w[5] == 0 || !p.read_user(w[5], ByteView(iv, 16))) return w[5] == 0 ? kInvalidParameter : kAccessViolation;
  }
  ByteView d = data.writable();
  if (w[1] < 2 && d.size() % 16 != 0) return kInvalidParameter;
  uint8_t next[16];      // the chaining value for the next call: the last block of ciphertext
  for (uint32_t i = 0; i < 16; ++i) next[i] = iv[i];
  if (w[1] == 0) {
    for (uint64_t off = 0; off < d.size(); off += 16) {
      uint8_t in[16], out[16];
      for (uint32_t i = 0; i < 16; ++i) in[i] = d[off + i];
      if (encrypt) aes.encrypt_block(in, out); else aes.decrypt_block(in, out);
      for (uint32_t i = 0; i < 16; ++i) d[off + i] = out[i];
    }
  } else if (w[1] == 3) {      // CFB with an 8 bit feedback: the register shifts by one byte of ciphertext
    for (uint64_t i = 0; i < d.size(); ++i) {
      uint8_t ks[16];
      aes.encrypt_block(next, ks);
      const uint8_t c = encrypt ? uint8_t(d[i] ^ ks[0]) : d[i];
      d[i] = uint8_t(d[i] ^ ks[0]);
      for (uint32_t j = 0; j < 15; ++j) next[j] = next[j + 1];
      next[15] = c;
    }
  } else {
    if (!encrypt && d.size() >= 16) for (uint32_t i = 0; i < 16; ++i) next[i] = d[d.size() - 16 + i];
    if (w[1] == 1) aes.cbc(iv, d, !encrypt); else aes.cfb128(iv, d, !encrypt);
    if (encrypt && d.size() >= 16) for (uint32_t i = 0; i < 16; ++i) next[i] = d[d.size() - 16 + i];
  }
  if (uint32_t s = put(p, w[7], data.view())) return s;
  if (w[1] != 0 && w[5] != 0) return put(p, w[5], ConstByteView(next, 16));
  return kSuccess;
}

// w: encrypt, key, key len, nonce, nonce len, aad, aad len, in, length, out, tag (16 bytes: written when encrypting, checked when decrypting)
uint32_t op_gcm(Process& p, const uint64_t* w) {
  UserBytes key(p), nonce(p), aad(p), data(p);
  if (uint32_t s = key.fetch(w[2], w[3], 32)) return s;
  if (w[5] != 12) return kNotSupported;
  if (uint32_t s = nonce.fetch(w[4], w[5])) return s;
  if (uint32_t s = aad.fetch(w[6], w[7], 1 << 20)) return s;
  if (uint32_t s = data.fetch(w[8], w[9])) return s;
  crypto::AesGcm gcm;
  if (!gcm.set_key(key.view())) return kInvalidParameter;      // (16 and 32 byte keys; AES-192 has no GCM here)
  uint8_t tag[16];
  if (w[1]) {
    gcm.seal(nonce.view().data(), aad.view(), data.writable(), tag);
    if (uint32_t s = put(p, w[10], data.view())) return s;
    return put(p, w[11], ConstByteView(tag, 16));
  }
  if (!p.read_user(w[11], ByteView(tag, 16))) return kAccessViolation;
  if (!gcm.open(nonce.view().data(), aad.view(), data.writable(), tag)) return kAuthTagMismatch;
  return put(p, w[10], data.view());
}

// w: sub-operation, curve, A, A len, B, B len, C, C len, out
// 1 new private scalar -> out   2 public point of A (private)   3 ECDH of A (private) with B (peer point) -> X   4 sign digest B with A (private) -> r|s   5 verify: public A, digest B, signature C
uint32_t op_ec(Process& p, const uint64_t* w) {
  if (w[2] > 1) return kInvalidParameter;
  const crypto::EcCurve curve = w[2] == 0 ? crypto::EcCurve::P256 : crypto::EcCurve::P384;
  const uint32_t fs = crypto::ec_field_size(curve), ps = crypto::ec_point_size(curve);
  UserBytes a(p), b(p), c(p);
  if (uint32_t s = a.fetch(w[3], w[4], 256)) return s;
  if (uint32_t s = b.fetch(w[5], w[6], 256)) return s;
  if (uint32_t s = c.fetch(w[7], w[8], 256)) return s;
  uint8_t out[crypto::kMaxEcPoint];
  switch (w[1]) {
    case 1: {
      HostRandom random(p);
      if (!crypto::ec_generate_private(curve, random, out)) return kNotSupported;
      return put(p, w[9], ConstByteView(out, fs));
    }
    case 2:
      if (a.size() != fs || !crypto::ec_public_key(curve, a.view().data(), out)) return kInvalidParameter;
      return put(p, w[9], ConstByteView(out, ps));
    case 3:
      if (a.size() != fs || !crypto::ec_ecdh(curve, a.view().data(), b.view(), out)) return kInvalidParameter;
      return put(p, w[9], ConstByteView(out, fs));
    case 4: {
      HostRandom random(p);
      if (a.size() != fs || b.size() == 0) return kInvalidParameter;
      if (!crypto::ecdsa_sign(curve, a.view().data(), b.view().data(), uint32_t(b.size()), random, out)) return kInvalidParameter;
      return put(p, w[9], ConstByteView(out, 2 * fs));
    }
    case 5:
      if (a.size() != ps || b.size() == 0 || c.size() != 2 * fs) return kInvalidSignature;
      return crypto::ecdsa_verify(curve, a.view(), b.view().data(), uint32_t(b.size()), c.view().first(fs), c.view().subview(fs, fs)) ? kSuccess : kInvalidSignature;
    default: return kInvalidParameter;
  }
}

// w: padding (0 PKCS #1 v1.5, 1 PSS), hash, modulus, len, exponent, len, digest, signature, signature len
uint32_t op_rsa(Process& p, const uint64_t* w) {
  crypto::HashKind kind;
  if (w[1] > 1 || !hash_kind(w[2], &kind)) return kInvalidParameter;
  UserBytes modulus(p), exponent(p), digest(p), signature(p);
  if (uint32_t s = modulus.fetch(w[3], w[4], 1024)) return s;
  if (uint32_t s = exponent.fetch(w[5], w[6], 16)) return s;
  if (uint32_t s = digest.fetch(w[7], crypto::hash_size(kind))) return s;
  if (uint32_t s = signature.fetch(w[8], w[9], 1024)) return s;
  if (modulus.size() == 0 || exponent.size() == 0 || digest.size() != crypto::hash_size(kind)) return kInvalidParameter;
  return crypto::rsa_verify(w[1] == 0 ? crypto::RsaPadding::Pkcs1V15 : crypto::RsaPadding::Pss, kind, modulus.view(), exponent.view(), digest.view().data(), signature.view())
             ? kSuccess : kInvalidSignature;
}

}  // namespace

uint32_t nt_tcos_crypto(Process& p, const SyscallRequest& r) {
  if (!p.files().attached()) return kNotSupported;      // (no allocator: a process always has one)
  uint64_t w[kWords];
  for (uint32_t i = 0; i < kWords; ++i) if (!p.read_user_u64(r.args[0] + 8 * i, &w[i])) return kAccessViolation;
  switch (w[0]) {
    case 1: return op_hash(p, w);
    case 2: return op_pbkdf2(p, w);
    case 3: return op_hkdf(p, w);
    case 4: return op_aes(p, w);
    case 5: return op_gcm(p, w);
    case 6: return op_ec(p, w);
    case 7: return op_rsa(p, w);
    default: return kInvalidParameter;
  }
}

}  // namespace process::windowsabi
