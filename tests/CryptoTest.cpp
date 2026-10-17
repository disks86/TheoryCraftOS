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

// Host-side tests for src/Crypto: hashes, HMAC, AES-CFB and DES-CBC against OpenSSL's output (tests/CryptoVectors.inc), the
// standards' published vectors, and the streaming and tamper checks.
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include "Crypto/Crypto.hpp"

using namespace crypto;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "CryptoVectors.inc"

static std::vector<uint8_t> unhex(const char* s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    v.push_back(static_cast<uint8_t>(nib(s[i]) * 16 + nib(s[i + 1])));
  }
  return v;
}
static std::string hex(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) { char b[3]; snprintf(b, sizeof b, "%02x", p[i]); s += b; }
  return s;
}
static ConstByteView view(const std::vector<uint8_t>& v) { return ConstByteView(v.data(), v.size()); }
static ConstByteView text(const char* s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

int main() {
  // Published vectors.
  uint8_t d[32];
  hash(HashKind::Md5, text("abc"), d);
  CHECK(hex(d, 16) == "900150983cd24fb0d6963f7d28e17f72");
  hash(HashKind::Sha1, text("abc"), d);
  CHECK(hex(d, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  hash(HashKind::Sha256, text("abc"), d);
  CHECK(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  hash(HashKind::Sha224, text("abc"), d);
  CHECK(hex(d, 28) == "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");
  hmac(HashKind::Md5, view(unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")), text("Hi There"), d);   // RFC 2202 test 1
  CHECK(hex(d, 16) == "9294727a3638bb1c13f48ef8158bfc9d");
  hmac(HashKind::Sha1, view(unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")), text("Hi There"), d);
  CHECK(hex(d, 20) == "b617318655057264e28bc0b6fb378c8ef146be00");
  // FIPS-197 C.1, C.2, C.3
  {
    Aes aes;
    uint8_t out[16];
    std::vector<uint8_t> pt = unhex("00112233445566778899aabbccddeeff");
    CHECK(aes.set_key(view(unhex("000102030405060708090a0b0c0d0e0f"))));
    aes.encrypt_block(pt.data(), out);
    CHECK(hex(out, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");
    CHECK(aes.set_key(view(unhex("000102030405060708090a0b0c0d0e0f1011121314151617"))));
    aes.encrypt_block(pt.data(), out);
    CHECK(hex(out, 16) == "dda97ca4864cdfe06eaf70a0ec0d7191");
    CHECK(aes.set_key(view(unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"))));
    aes.encrypt_block(pt.data(), out);
    CHECK(hex(out, 16) == "8ea2b7ca516745bfeafc49904b496089");
    CHECK(!aes.set_key(view(unhex("0001020304"))));
  }
  // DES: the classic example (Stallings / FIPS 81)
  {
    Des des;
    uint8_t out[8], back[8];
    std::vector<uint8_t> key = unhex("133457799bbcdff1"), pt = unhex("0123456789abcdef");
    des.set_key(key.data());
    des.encrypt_block(pt.data(), out);
    CHECK(hex(out, 8) == "85e813540f0ab405");
    des.decrypt_block(out, back);
    CHECK(memcmp(back, pt.data(), 8) == 0);
  }
  // OpenSSL's answers, with the message fed whole and in odd pieces.
  for (const HashVector& v : kHashVectors) {
    const std::vector<uint8_t> m = unhex(v.message);
    const char* want[4] = {v.md5, v.sha1, v.sha224, v.sha256};
    const HashKind kinds[4] = {HashKind::Md5, HashKind::Sha1, HashKind::Sha224, HashKind::Sha256};
    for (int k = 0; k < 4; ++k) {
      hash(kinds[k], view(m), d);
      CHECK(hex(d, hash_size(kinds[k])) == want[k]);
      Hasher h(kinds[k]);
      for (size_t pos = 0, step = 1; pos < m.size(); pos += step, step = step * 3 % 70 + 1) h.update(ConstByteView(m.data() + pos, std::min(step, m.size() - pos)));
      h.finish(d);
      CHECK(hex(d, hash_size(kinds[k])) == want[k]);
      h.update(view(m));   // reusable after finish()
      h.finish(d);
      CHECK(hex(d, hash_size(kinds[k])) == want[k]);
    }
  }
  for (const HmacVector& v : kHmacVectors) {
    const std::vector<uint8_t> key = unhex(v.key), m = unhex(v.message);
    const char* want[4] = {v.md5, v.sha1, v.sha224, v.sha256};
    const HashKind kinds[4] = {HashKind::Md5, HashKind::Sha1, HashKind::Sha224, HashKind::Sha256};
    for (int k = 0; k < 4; ++k) {
      hmac(kinds[k], view(key), view(m), d);
      CHECK(hex(d, hash_size(kinds[k])) == want[k]);
    }
  }
  for (const AesVector& v : kAesVectors) {
    const std::vector<uint8_t> key = unhex(v.key), iv = unhex(v.iv), plain = unhex(v.plain), cipher = unhex(v.cfb);
    Aes aes;
    CHECK(aes.set_key(view(key)));
    std::vector<uint8_t> buf = plain;
    aes.cfb128(iv.data(), ByteView(buf.data(), buf.size()), false);
    CHECK(buf == cipher);
    aes.cfb128(iv.data(), ByteView(buf.data(), buf.size()), true);
    CHECK(buf == plain);
  }
  for (const DesVector& v : kDesVectors) {
    const std::vector<uint8_t> key = unhex(v.key), iv = unhex(v.iv), plain = unhex(v.plain), cipher = unhex(v.cbc), ecb = unhex(v.ecb_first_block);
    Des des;
    des.set_key(key.data());
    std::vector<uint8_t> buf = plain;
    des.cbc(iv.data(), ByteView(buf.data(), buf.size()), false);
    CHECK(buf == cipher);
    des.cbc(iv.data(), ByteView(buf.data(), buf.size()), true);
    CHECK(buf == plain);
    uint8_t out[8];
    des.encrypt_block(plain.data(), out);
    CHECK(memcmp(out, ecb.data(), 8) == 0);
  }
  CHECK(equal_constant_time(d, d, 20));
  uint8_t other[32];
  memcpy(other, d, 32);
  other[19] ^= 1;
  CHECK(!equal_constant_time(d, other, 20) && equal_constant_time(d, other, 19));
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("crypto tests passed\n");
  return 0;
}
