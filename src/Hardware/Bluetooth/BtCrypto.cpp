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

#include "Hardware/Bluetooth/BtCrypto.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/Ecc.hpp"

namespace hardware {
namespace bt {

namespace {
void swap_copy(const uint8_t* src, uint8_t* dst, uint32_t length) {
  for (uint32_t i = 0; i < length; ++i) dst[length - 1 - i] = src[i];
}

// AES-CMAC with the key and message given least significant byte first.
bool cmac(const uint8_t key[16], const uint8_t* message, uint32_t length, uint8_t mac[16]) {
  uint8_t k[16], m[80], out[16];
  if (length > sizeof m) return false;
  swap_copy(key, k, 16);
  swap_copy(message, m, length);
  if (!crypto::aes_cmac(ConstByteView(k, 16), ConstByteView(m, length), out)) return false;
  swap_copy(out, mac, 16);
  return true;
}
}

bool sm_e(const uint8_t key[16], uint8_t data[16]) {
  uint8_t k[16], block[16], out[16];
  swap_copy(key, k, 16);
  swap_copy(data, block, 16);
  crypto::Aes aes;
  if (!aes.set_key(ConstByteView(k, 16))) return false;
  aes.encrypt_block(block, out);
  swap_copy(out, data, 16);
  return true;
}

bool sm_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7], uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16]) {
  uint8_t p1[16] = {};
  p1[0] = iat;
  p1[1] = rat;
  for (uint32_t i = 0; i < 7; ++i) { p1[2 + i] = preq[i]; p1[9 + i] = pres[i]; }
  for (uint32_t i = 0; i < 16; ++i) out[i] = r[i] ^ p1[i];
  if (!sm_e(k, out)) return false;
  uint8_t p2[16] = {};
  for (uint32_t i = 0; i < 6; ++i) { p2[i] = ra[i]; p2[6 + i] = ia[i]; }
  for (uint32_t i = 0; i < 16; ++i) out[i] ^= p2[i];
  return sm_e(k, out);
}

bool sm_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16]) {
  for (uint32_t i = 0; i < 8; ++i) { out[i] = r2[i]; out[8 + i] = r1[i]; }       // (the least significant halves of the two numbers)
  return sm_e(k, out);
}

bool sm_ah(const uint8_t irk[16], const uint8_t prand[3], uint8_t hash[3]) {
  uint8_t block[16] = {};
  for (uint32_t i = 0; i < 3; ++i) block[i] = prand[i];
  if (!sm_e(irk, block)) return false;
  for (uint32_t i = 0; i < 3; ++i) hash[i] = block[i];
  return true;
}

bool sm_irk_matches(const uint8_t irk[16], const uint8_t address[6]) {
  if ((address[5] & 0xC0) != 0x40) return false;                        // (a resolvable private address: the top two bits are 01)
  uint8_t hash[3];
  if (!sm_ah(irk, address + 3, hash)) return false;
  return hash[0] == address[0] && hash[1] == address[1] && hash[2] == address[2];
}

bool sm_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z, uint8_t out[16]) {
  uint8_t m[65];
  m[0] = z;
  for (uint32_t i = 0; i < 32; ++i) { m[1 + i] = v[i]; m[33 + i] = u[i]; }
  return cmac(x, m, sizeof m, out);
}

bool sm_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16], const uint8_t a1[7], const uint8_t a2[7], uint8_t mackey[16], uint8_t ltk[16]) {
  static const uint8_t btle[4] = {0x65, 0x6C, 0x74, 0x62};              // "btle"
  static const uint8_t salt[16] = {0xBE, 0x83, 0x60, 0x5A, 0xDB, 0x0B, 0x37, 0x60, 0x38, 0xA5, 0xF5, 0xAA, 0x91, 0x83, 0x88, 0x6C};
  uint8_t t[16], m[53];
  if (!cmac(salt, w, 32, t)) return false;
  m[0] = 0x00;                                                          // the length, 256, least significant first
  m[1] = 0x01;
  for (uint32_t i = 0; i < 7; ++i) { m[2 + i] = a2[i]; m[9 + i] = a1[i]; }
  for (uint32_t i = 0; i < 16; ++i) { m[16 + i] = n2[i]; m[32 + i] = n1[i]; }
  for (uint32_t i = 0; i < 4; ++i) m[48 + i] = btle[i];
  m[52] = 0;
  if (!cmac(t, m, sizeof m, mackey)) return false;
  m[52] = 1;
  return cmac(t, m, sizeof m, ltk);
}

bool sm_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16], const uint8_t r[16], const uint8_t io_cap[3], const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16]) {
  uint8_t m[65];
  for (uint32_t i = 0; i < 7; ++i) { m[i] = a2[i]; m[7 + i] = a1[i]; }
  for (uint32_t i = 0; i < 3; ++i) m[14 + i] = io_cap[i];
  for (uint32_t i = 0; i < 16; ++i) { m[17 + i] = r[i]; m[33 + i] = n2[i]; m[49 + i] = n1[i]; }
  return cmac(w, m, sizeof m, out);
}

bool sm_g2(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], const uint8_t y[16], uint32_t* value) {
  uint8_t m[80], mac[16];
  for (uint32_t i = 0; i < 16; ++i) m[i] = y[i];
  for (uint32_t i = 0; i < 32; ++i) { m[16 + i] = v[i]; m[48 + i] = u[i]; }
  if (!cmac(x, m, sizeof m, mac)) return false;
  *value = (mac[0] | (uint32_t{mac[1]} << 8) | (uint32_t{mac[2]} << 16) | (uint32_t{mac[3]} << 24)) % 1000000;
  return true;
}

bool sm_h6(const uint8_t w[16], const uint8_t key_id[4], uint8_t out[16]) { return cmac(w, key_id, 4, out); }
bool sm_h7(const uint8_t w[16], const uint8_t salt[16], uint8_t out[16]) { return cmac(salt, w, 16, out); }

bool sm_public_from_private(const uint8_t private_key[32], uint8_t public_key[64]) {
  uint8_t sk[32], point[crypto::kMaxEcPoint];
  swap_copy(private_key, sk, 32);
  if (!crypto::ec_public_key(crypto::EcCurve::P256, sk, point)) return false;
  swap_copy(point + 1, public_key, 32);
  swap_copy(point + 33, public_key + 32, 32);
  return true;
}

bool sm_generate_key_pair(crypto::IRandom& random, SmKeyPair* out) {
  uint8_t sk[32];
  if (!crypto::ec_generate_private(crypto::EcCurve::P256, random, sk)) return false;
  swap_copy(sk, out->private_key, 32);
  return sm_public_from_private(out->private_key, out->public_key);
}

bool sm_dhkey(const uint8_t private_key[32], const uint8_t peer_public[64], uint8_t dhkey[32]) {
  uint8_t sk[32], point[crypto::kMaxEcPoint], shared[32];
  swap_copy(private_key, sk, 32);
  point[0] = 0x04;
  swap_copy(peer_public, point + 1, 32);
  swap_copy(peer_public + 32, point + 33, 32);
  if (!crypto::ec_ecdh(crypto::EcCurve::P256, sk, ConstByteView(point, 65), shared)) return false;
  swap_copy(shared, dhkey, 32);
  return true;
}

}  // namespace bt
}  // namespace hardware
