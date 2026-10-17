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

#include "Crypto/Rsa.hpp"
#include "Crypto/BigInt.hpp"

namespace crypto {

namespace {

// DigestInfo prefixes (RFC 8017 9.2 note 1)
struct DigestInfo {
  HashKind hash;
  const uint8_t* prefix;
  uint32_t length;
};
const uint8_t kSha1Prefix[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
const uint8_t kSha224Prefix[] = {0x30, 0x2d, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x04, 0x05, 0x00, 0x04, 0x1c};
const uint8_t kSha256Prefix[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
const uint8_t kSha384Prefix[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
const uint8_t kSha512Prefix[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

bool digest_info(HashKind hash, const uint8_t** prefix, uint32_t* length) {
  switch (hash) {
    case HashKind::Sha1: *prefix = kSha1Prefix; *length = sizeof kSha1Prefix; return true;
    case HashKind::Sha224: *prefix = kSha224Prefix; *length = sizeof kSha224Prefix; return true;
    case HashKind::Sha256: *prefix = kSha256Prefix; *length = sizeof kSha256Prefix; return true;
    case HashKind::Sha384: *prefix = kSha384Prefix; *length = sizeof kSha384Prefix; return true;
    case HashKind::Sha512: *prefix = kSha512Prefix; *length = sizeof kSha512Prefix; return true;
    default: return false;
  }
}

// MGF1 (RFC 8017 B.2.1): XORs the mask for `seed` into `out`.
void mgf1_xor(HashKind hash, const uint8_t* seed, uint32_t seed_length, ByteView out) {
  const uint32_t size = hash_size(hash);
  for (uint32_t counter = 0; static_cast<uint64_t>(counter) * size < out.size(); ++counter) {
    Hasher h(hash);
    h.update(ConstByteView(seed, seed_length));
    const uint8_t c[4] = {static_cast<uint8_t>(counter >> 24), static_cast<uint8_t>(counter >> 16), static_cast<uint8_t>(counter >> 8), static_cast<uint8_t>(counter)};
    h.update(ConstByteView(c, 4));
    uint8_t block[kMaxHashSize];
    h.finish(block);
    for (uint32_t i = 0; i < size && static_cast<uint64_t>(counter) * size + i < out.size(); ++i) out[static_cast<uint64_t>(counter) * size + i] ^= block[i];
  }
}

constexpr uint32_t kMaxModulusBytes = 512;

}  // namespace

uint32_t rsa_modulus_bits(ConstByteView modulus) {
  uint64_t i = 0;
  while (i < modulus.size() && modulus[i] == 0) ++i;
  if (i == modulus.size()) return 0;
  uint32_t bits = static_cast<uint32_t>((modulus.size() - i - 1) * 8);
  for (uint8_t top = modulus[i]; top; top >>= 1) ++bits;
  return bits;
}

bool rsa_verify(RsaPadding padding, HashKind hash, ConstByteView modulus, ConstByteView exponent, const uint8_t* digest, ConstByteView signature) {
  const uint32_t mod_bits = rsa_modulus_bits(modulus);
  if (mod_bits < 512 || mod_bits > 4096) return false;
  const uint32_t k = (mod_bits + 7) / 8;
  uint64_t skip = 0;
  while (skip < exponent.size() && exponent[skip] == 0) ++skip;
  if (exponent.size() - skip == 0 || exponent.size() - skip > 4) return false;
  if (exponent.size() - skip == 1 && exponent[skip] < 3) return false;   // e of 0, 1 or 2
  if (signature.size() != k) return false;
  MontRsa mont;
  if (!mont.init(modulus)) return false;
  MontRsa::Num s, sm, result;
  if (!mont.load(signature, &s) || !mont.below_modulus(s)) return false;
  mont.to_mont(s, &sm);
  mont.pow(sm, exponent.subview(skip), &result);
  mont.from_mont(result, &result);
  uint8_t em[kMaxModulusBytes];
  if (!mont.store(result, ByteView(em, k))) return false;
  const uint32_t hlen = hash_size(hash);

  if (padding == RsaPadding::Pkcs1V15) {
    const uint8_t* prefix;
    uint32_t prefix_length;
    if (!digest_info(hash, &prefix, &prefix_length)) return false;
    const uint32_t t_length = prefix_length + hlen;
    if (k < t_length + 11) return false;
    uint32_t diff = em[0] | (em[1] ^ 1);
    const uint32_t ps_end = k - t_length - 1;
    for (uint32_t i = 2; i < ps_end; ++i) diff |= em[i] ^ 0xff;
    diff |= em[ps_end];
    for (uint32_t i = 0; i < prefix_length; ++i) diff |= em[ps_end + 1 + i] ^ prefix[i];
    for (uint32_t i = 0; i < hlen; ++i) diff |= em[ps_end + 1 + prefix_length + i] ^ digest[i];
    return diff == 0;
  }

  // PSS (RFC 8017 9.1.2) with the salt as long as the hash
  const uint32_t em_bits = mod_bits - 1;
  const uint32_t em_len = (em_bits + 7) / 8;
  const uint8_t* e = em + (k - em_len);   // when mod_bits - 1 is a multiple of 8, EM has one byte less than the modulus
  if (em_len < hlen + hlen + 2 || e[em_len - 1] != 0xbc) return false;
  if (k != em_len && em[0] != 0) return false;
  const uint32_t db_len = em_len - hlen - 1;
  uint8_t db[kMaxModulusBytes];
  for (uint32_t i = 0; i < db_len; ++i) db[i] = e[i];
  const uint8_t* h = e + db_len;
  const uint32_t top_bits = 8 * em_len - em_bits;   // bits that must be zero in the first byte
  if (top_bits && (db[0] >> (8 - top_bits))) return false;
  mgf1_xor(hash, h, hlen, ByteView(db, db_len));
  if (top_bits) db[0] &= static_cast<uint8_t>(0xff >> top_bits);
  const uint32_t salt_start = db_len - hlen;
  uint32_t diff = 0;
  for (uint32_t i = 0; i + 1 < salt_start; ++i) diff |= db[i];
  diff |= db[salt_start - 1] ^ 1;
  if (diff) return false;
  Hasher m(hash);
  const uint8_t zeros[8] = {0};
  m.update(ConstByteView(zeros, 8));
  m.update(ConstByteView(digest, hlen));
  m.update(ConstByteView(db + salt_start, hlen));
  uint8_t expected[kMaxHashSize];
  m.finish(expected);
  return equal_constant_time(expected, h, hlen);
}

}  // namespace crypto
