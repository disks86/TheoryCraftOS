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
#include "Crypto/Aead.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/Ecc.hpp"
#include "Crypto/Random.hpp"
#include "Crypto/Rsa.hpp"

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
static bool same_bytes(const void* a, const void* b, size_t n) { return n == 0 || memcmp(a, b, n) == 0; }
static ConstByteView view(const std::vector<uint8_t>& v) { return ConstByteView(v.data(), v.size()); }
static ConstByteView text(const char* s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

// MD4 (RFC 1320 appendix A.5), AES-CMAC (RFC 4493 section 4) and the SP 800-108 counter KDF (checked against an independent computation).
static void test_smb_primitives() {
  uint8_t d[32];
  md4(text(""), d); CHECK(hex(d, 16) == "31d6cfe0d16ae931b73c59d7e0c089c0");
  md4(text("a"), d); CHECK(hex(d, 16) == "bde52cb31de33e46245e05fbdbd6fb24");
  md4(text("abc"), d); CHECK(hex(d, 16) == "a448017aaf21d8525fc10ae87aa6729d");
  md4(text("message digest"), d); CHECK(hex(d, 16) == "d9130a8164549fe818874806e1c7014b");
  md4(text("12345678901234567890123456789012345678901234567890123456789012345678901234567890"), d); CHECK(hex(d, 16) == "e33b4ddc9c38f2199c3e7b164fcc0536");
  md4(text("abcdefghijklmnopqrstuvwxyz"), d); CHECK(hex(d, 16) == "d79e1c308aa5bbcdeea8ed63df412da9");
  const std::vector<uint8_t> key = unhex("2b7e151628aed2a6abf7158809cf4f3c");
  const std::vector<uint8_t> msg = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
  uint8_t mac[16];
  CHECK(aes_cmac(view(key), ConstByteView(), mac) && hex(mac, 16) == "bb1d6929e95937287fa37d129b756746");
  CHECK(aes_cmac(view(key), ConstByteView(msg.data(), 16), mac) && hex(mac, 16) == "070a16b46b4d4144f79bdd9dd04a287c");
  CHECK(aes_cmac(view(key), ConstByteView(msg.data(), 40), mac) && hex(mac, 16) == "dfa66747de9ae63030ca32611497c827");
  CHECK(aes_cmac(view(key), view(msg), mac) && hex(mac, 16) == "51f0bebf7e3b9d92fc49741779363cfe");
  CHECK(!aes_cmac(ConstByteView(key.data(), 15), view(msg), mac));
  // KDF: HMAC-SHA256(key, 00000001 || label || 00 || context || 00000080), SMB passing its label and context with their terminators
  uint8_t out[16];
  const std::vector<uint8_t> k = unhex("000102030405060708090a0b0c0d0e0f");
  CHECK(kdf_hmac_sha256(view(k), ConstByteView(reinterpret_cast<const uint8_t*>("SMB2AESCMAC"), 12), ConstByteView(reinterpret_cast<const uint8_t*>("SmbSign"), 8), ByteView(out)));
  uint8_t expect[32];
  std::vector<uint8_t> input = unhex("00000001");
  for (const char* p = "SMB2AESCMAC"; *p; ++p) input.push_back(static_cast<uint8_t>(*p));
  input.push_back(0);   // (the label's own terminator, as SMB passes it)
  input.push_back(0);   // the separator
  for (const char* p = "SmbSign"; *p; ++p) input.push_back(static_cast<uint8_t>(*p));
  input.push_back(0);   // (the context's terminator)
  const std::vector<uint8_t> tail = unhex("00000080");
  input.insert(input.end(), tail.begin(), tail.end());
  hmac(HashKind::Sha256, view(k), view(input), expect);
  CHECK(same_bytes(out, expect, 16));
  CHECK(!kdf_hmac_sha256(view(k), text("a"), text("b"), ByteView(expect, 33)) );
}

int main() {
  test_smb_primitives();
  // Published vectors.
  uint8_t d[64];
  hash(HashKind::Md5, text("abc"), d);
  CHECK(hex(d, 16) == "900150983cd24fb0d6963f7d28e17f72");
  hash(HashKind::Sha1, text("abc"), d);
  CHECK(hex(d, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  hash(HashKind::Sha256, text("abc"), d);
  CHECK(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  hash(HashKind::Sha512, text("abc"), d);
  CHECK(hex(d, 64) == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
  hash(HashKind::Sha384, text("abc"), d);
  CHECK(hex(d, 48) == "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
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
    // and back (FIPS-197 C.1 - C.3 read the other way)
    const char* keys[3] = {"000102030405060708090a0b0c0d0e0f", "000102030405060708090a0b0c0d0e0f1011121314151617", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
    const char* cts[3] = {"69c4e0d86a7b0430d8cdb78070b4c55a", "dda97ca4864cdfe06eaf70a0ec0d7191", "8ea2b7ca516745bfeafc49904b496089"};
    for (int i = 0; i < 3; ++i) {
      CHECK(aes.set_key(view(unhex(keys[i]))));
      std::vector<uint8_t> ct = unhex(cts[i]);
      aes.decrypt_block(ct.data(), out);
      CHECK(hex(out, 16) == "00112233445566778899aabbccddeeff");
    }
    // CBC, NIST SP 800-38A F.2.1 / F.2.2 (AES-128) and F.2.5 / F.2.6 (AES-256)
    const std::vector<uint8_t> iv = unhex("000102030405060708090a0b0c0d0e0f");
    const std::string plain = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
    const struct { const char* key; const char* cipher; } cbc[2] = {
        {"2b7e151628aed2a6abf7158809cf4f3c", "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"},
        {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b"}};
    for (const auto& c : cbc) {
      CHECK(aes.set_key(view(unhex(c.key))));
      std::vector<uint8_t> data = unhex(plain.c_str());
      aes.cbc(iv.data(), ByteView(data.data(), data.size()), false);
      CHECK(hex(data.data(), data.size()) == c.cipher);
      aes.cbc(iv.data(), ByteView(data.data(), data.size()), true);
      CHECK(hex(data.data(), data.size()) == plain);
    }
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
    const char* want[6] = {v.md5, v.sha1, v.sha224, v.sha256, v.sha384, v.sha512};
    const HashKind kinds[6] = {HashKind::Md5, HashKind::Sha1, HashKind::Sha224, HashKind::Sha256, HashKind::Sha384, HashKind::Sha512};
    for (int k = 0; k < 6; ++k) {
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
    const char* want[6] = {v.md5, v.sha1, v.sha224, v.sha256, v.sha384, v.sha512};
    const HashKind kinds[6] = {HashKind::Md5, HashKind::Sha1, HashKind::Sha224, HashKind::Sha256, HashKind::Sha384, HashKind::Sha512};
    for (int k = 0; k < 6; ++k) {
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
  // AEADs against OpenSSL, with every tampering refused.
  for (const GcmVector& v : kGcmVectors) {
    const std::vector<uint8_t> key = unhex(v.key), nonce = unhex(v.nonce), aad = unhex(v.aad), plain = unhex(v.plain), sealed = unhex(v.sealed);
    AesGcm gcm;
    CHECK(gcm.set_key(view(key)));
    std::vector<uint8_t> buf = plain;
    uint8_t tag[16];
    gcm.seal(nonce.data(), view(aad), ByteView(buf.data(), buf.size()), tag);
    CHECK(buf.size() + 16 == sealed.size() && same_bytes(buf.data(), sealed.data(), buf.size()) && memcmp(tag, sealed.data() + buf.size(), 16) == 0);
    CHECK(gcm.open(nonce.data(), view(aad), ByteView(buf.data(), buf.size()), tag) && buf == plain);
    if (!buf.empty()) {
      std::vector<uint8_t> bad = sealed;
      bad[0] ^= 1;
      CHECK(!gcm.open(nonce.data(), view(aad), ByteView(bad.data(), plain.size()), bad.data() + plain.size()) && bad[0] == (sealed[0] ^ 1));   // refused, and not decrypted
    }
    tag[3] ^= 0x20;
    CHECK(!gcm.open(nonce.data(), view(aad), ByteView(buf.data(), buf.size()), tag));
  }
  for (const ChaPolyVector& v : kChaPolyVectors) {
    const std::vector<uint8_t> key = unhex(v.key), nonce = unhex(v.nonce), aad = unhex(v.aad), plain = unhex(v.plain), sealed = unhex(v.sealed);
    ChaCha20Poly1305 aead;
    CHECK(aead.set_key(view(key)));
    std::vector<uint8_t> buf = plain;
    uint8_t tag[16];
    aead.seal(nonce.data(), view(aad), ByteView(buf.data(), buf.size()), tag);
    CHECK(buf.size() + 16 == sealed.size() && same_bytes(buf.data(), sealed.data(), buf.size()) && memcmp(tag, sealed.data() + buf.size(), 16) == 0);
    CHECK(aead.open(nonce.data(), view(aad), ByteView(buf.data(), buf.size()), tag) && buf == plain);
    std::vector<uint8_t> wrong_aad = aad;
    wrong_aad.push_back(0);
    CHECK(!aead.open(nonce.data(), view(wrong_aad), ByteView(buf.data(), buf.size()), tag));
  }
  // RFC 8439 2.8.2 and RFC 7748 6.1, from the standards.
  {
    const std::vector<uint8_t> key = unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"), nonce = unhex("070000004041424344454647"), aad = unhex("50515253c0c1c2c3c4c5c6c7");
    const char* msg = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    std::vector<uint8_t> buf(msg, msg + strlen(msg));
    ChaCha20Poly1305 aead;
    uint8_t tag[16];
    aead.set_key(view(key));
    aead.seal(nonce.data(), view(aad), ByteView(buf.data(), buf.size()), tag);
    CHECK(hex(tag, 16) == "1ae10b594f09e26a7e902ecbd0600691" && hex(buf.data(), 8) == "d31a8d34648e60db");
    uint8_t pub[32];
    const std::vector<uint8_t> alice = unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"), bob_pub = unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    x25519_public_key(alice.data(), pub);
    CHECK(hex(pub, 32) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    uint8_t secret[32];
    CHECK(x25519_shared_secret(alice.data(), bob_pub.data(), secret) && hex(secret, 32) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    const uint8_t zero_point[32] = {0};
    CHECK(!x25519_shared_secret(alice.data(), zero_point, secret));
  }
  {   // Ed25519 (RFC 8032 section 7.1, tests 1 and 2)
    const std::vector<uint8_t> pk1 = unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const std::vector<uint8_t> sig1 = unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    CHECK(ed25519_verify(pk1.data(), ConstByteView(), sig1.data()));
    const uint8_t other = 0x72;
    CHECK(!ed25519_verify(pk1.data(), ConstByteView(&other, 1), sig1.data()));
    const std::vector<uint8_t> pk2 = unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    std::vector<uint8_t> sig2 = unhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    CHECK(ed25519_verify(pk2.data(), ConstByteView(&other, 1), sig2.data()));
    sig2[40] ^= 1;
    CHECK(!ed25519_verify(pk2.data(), ConstByteView(&other, 1), sig2.data()));
    sig2[40] ^= 1;
    sig2[3] ^= 0x80;
    CHECK(!ed25519_verify(pk2.data(), ConstByteView(&other, 1), sig2.data()));
    uint8_t junk_key[32];
    memset(junk_key, 0xFF, 32);
    CHECK(!ed25519_verify(junk_key, ConstByteView(&other, 1), sig2.data()));
  }
  for (const X25519Vector& v : kX25519Vectors) {
    const std::vector<uint8_t> a = unhex(v.priv_a), b = unhex(v.priv_b), pa = unhex(v.pub_a), pb = unhex(v.pub_b), shared = unhex(v.shared);
    uint8_t out[32], s1[32], s2[32];
    x25519_public_key(a.data(), out);
    CHECK(memcmp(out, pa.data(), 32) == 0);
    x25519_public_key(b.data(), out);
    CHECK(memcmp(out, pb.data(), 32) == 0);
    CHECK(x25519_shared_secret(a.data(), pb.data(), s1) && x25519_shared_secret(b.data(), pa.data(), s2) && memcmp(s1, shared.data(), 32) == 0 && memcmp(s2, shared.data(), 32) == 0);
  }
  for (const HkdfVector& v : kHkdfVectors) {
    const HashKind kind = std::string(v.hash) == "sha256" ? HashKind::Sha256 : std::string(v.hash) == "sha384" ? HashKind::Sha384 : HashKind::Sha512;
    const std::vector<uint8_t> salt = unhex(v.salt), ikm = unhex(v.ikm), info = unhex(v.info), okm = unhex(v.okm);
    uint8_t prk[kMaxHashSize];
    hkdf_extract(kind, view(salt), view(ikm), prk);
    std::vector<uint8_t> out(atoi(v.length));
    CHECK(hkdf_expand(kind, ConstByteView(prk, hash_size(kind)), view(info), ByteView(out.data(), out.size())) && out == okm);
  }
  {
    uint8_t prk[32] = {0}, out[255 * 32 + 1];
    CHECK(!hkdf_expand(HashKind::Sha256, ConstByteView(prk, 32), ConstByteView(), ByteView(out, sizeof out)));
  }
  // HMAC-DRBG against an independent implementation (written in Python from the specification) with the same entropy.
  {
    struct FixedEntropy final : IEntropySource {
      std::vector<uint8_t> bytes;
      bool fail = false;
      bool read(ByteView out) override {
        if (fail || bytes.size() < out.size()) return false;
        for (size_t i = 0; i < out.size(); ++i) out[i] = bytes[i];
        bytes.erase(bytes.begin(), bytes.begin() + out.size());
        return true;
      }
    };
    for (const DrbgVector& v : kDrbgVectors) {
      FixedEntropy e;
      e.bytes = unhex(v.entropy);
      DrbgRandom rng(e);
      CHECK(!rng.ready());
      uint8_t probe[4];
      CHECK(!rng.fill(ByteView(probe, 4)));
      const std::vector<uint8_t> pers = unhex(v.personalization);
      CHECK(rng.initialize(view(pers)) && rng.ready());
      uint8_t a[48], b[100];
      CHECK(rng.fill(ByteView(a, 48)) && hex(a, 48) == v.first && rng.fill(ByteView(b, 100)) && hex(b, 100) == v.second);
    }
    FixedEntropy dead;
    dead.fail = true;
    DrbgRandom rng(dead);
    CHECK(!rng.initialize() && !rng.ready());
    FixedEntropy fine;
    fine.bytes.assign(48, 7);
    DrbgRandom rng2(fine);
    CHECK(rng2.initialize());
    uint8_t x[32], y[32];
    CHECK(rng2.fill(ByteView(x, 32)) && rng2.fill(ByteView(y, 32)) && memcmp(x, y, 32) != 0);
  }
  // Elliptic curves and RSA against OpenSSL.
  for (const EcdhVector& v : kEcdhVectors) {
    const EcCurve curve = std::string(v.curve) == "p256" ? EcCurve::P256 : EcCurve::P384;
    const std::vector<uint8_t> a = unhex(v.priv_a), pa = unhex(v.pub_a), b = unhex(v.priv_b), pb = unhex(v.pub_b), shared = unhex(v.shared);
    uint8_t point[kMaxEcPoint], x[48];
    CHECK(ec_public_key(curve, a.data(), point) && hex(point, ec_point_size(curve)) == v.pub_a);
    CHECK(ec_public_key(curve, b.data(), point) && hex(point, ec_point_size(curve)) == v.pub_b);
    CHECK(ec_ecdh(curve, a.data(), view(pb), x) && hex(x, ec_field_size(curve)) == v.shared);
    CHECK(ec_ecdh(curve, b.data(), view(pa), x) && hex(x, ec_field_size(curve)) == v.shared);
    // a point off the curve, a malformed one and a zero scalar are refused
    std::vector<uint8_t> bad = pb;
    bad[bad.size() - 1] ^= 1;
    CHECK(!ec_point_valid(curve, view(bad)) && !ec_ecdh(curve, a.data(), view(bad), x));
    bad = pb;
    bad[0] = 2;
    CHECK(!ec_point_valid(curve, view(bad)));
    bad.pop_back();
    CHECK(!ec_point_valid(curve, view(bad)) && ec_point_valid(curve, view(pb)));
    std::vector<uint8_t> zero(a.size(), 0);
    CHECK(!ec_ecdh(curve, zero.data(), view(pb), x) && !ec_public_key(curve, zero.data(), point));
    // coordinates that are not reduced
    std::vector<uint8_t> big(pb.size(), 0xff);
    big[0] = 4;
    CHECK(!ec_point_valid(curve, view(big)));
  }
  {
    // (n-1) * G is -G: the same X, the negated Y; the generator's own coordinates are on the curve.
    const std::vector<uint8_t> n_minus_1 = unhex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550");
    uint8_t g[kMaxEcPoint], minus_g[kMaxEcPoint], one[32] = {0};
    one[31] = 1;
    CHECK(ec_public_key(EcCurve::P256, one, g) && ec_point_valid(EcCurve::P256, ConstByteView(g, 65)));
    CHECK(hex(g + 1, 32) == "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
    CHECK(ec_public_key(EcCurve::P256, n_minus_1.data(), minus_g) && memcmp(g + 1, minus_g + 1, 32) == 0 && memcmp(g + 33, minus_g + 33, 32) != 0);
    std::vector<uint8_t> n = unhex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    CHECK(!ec_public_key(EcCurve::P256, n.data(), minus_g));   // the order itself is out of range
    // random scalars come from the generator in range
    struct Counter final : IRandom {
      uint8_t next = 1;
      bool fill(ByteView out) override { for (size_t i = 0; i < out.size(); ++i) out[i] = next++; return true; }
    } counter;
    uint8_t priv[48], pub[kMaxEcPoint];
    CHECK(ec_generate_private(EcCurve::P384, counter, priv) && ec_public_key(EcCurve::P384, priv, pub) && ec_point_valid(EcCurve::P384, ConstByteView(pub, 97)));
  }
  for (const EcdsaVector& v : kEcdsaVectors) {
    const EcCurve curve = std::string(v.curve) == "p256" ? EcCurve::P256 : EcCurve::P384;
    const std::vector<uint8_t> pub = unhex(v.pub), digest = unhex(v.digest), r = unhex(v.r), s = unhex(v.s);
    CHECK(ecdsa_verify(curve, view(pub), digest.data(), digest.size(), view(r), view(s)));
    std::vector<uint8_t> d2 = digest;
    d2[0] ^= 0x80;
    CHECK(!ecdsa_verify(curve, view(pub), d2.data(), d2.size(), view(r), view(s)));
    std::vector<uint8_t> s2 = s;
    s2[s2.size() - 1] ^= 1;
    CHECK(!ecdsa_verify(curve, view(pub), digest.data(), digest.size(), view(r), view(s2)));
    const std::vector<uint8_t> zero(1, 0);
    CHECK(!ecdsa_verify(curve, view(pub), digest.data(), digest.size(), view(zero), view(s)) && !ecdsa_verify(curve, view(pub), digest.data(), digest.size(), view(r), view(zero)));
    std::vector<uint8_t> huge(49, 0xff);   // r or s not below the order
    CHECK(!ecdsa_verify(curve, view(pub), digest.data(), digest.size(), view(huge), view(s)) && !ecdsa_verify(curve, view(pub), digest.data(), digest.size(), view(r), view(huge)));
    std::vector<uint8_t> other = pub;
    other[1] ^= 1;   // not on the curve (almost surely)
    CHECK(!ecdsa_verify(curve, view(other), digest.data(), digest.size(), view(r), view(s)));
  }
  for (const RsaVector& v : kRsaVectors) {
    const HashKind kind = std::string(v.hash) == "sha1" ? HashKind::Sha1 : std::string(v.hash) == "sha256" ? HashKind::Sha256 : std::string(v.hash) == "sha384" ? HashKind::Sha384 : HashKind::Sha512;
    const RsaPadding padding = std::string(v.padding) == "pss" ? RsaPadding::Pss : RsaPadding::Pkcs1V15;
    const std::vector<uint8_t> n = unhex(v.n), e = unhex(v.e), digest = unhex(v.digest), sig = unhex(v.sig);
    CHECK(rsa_modulus_bits(view(n)) == (uint32_t)atoi(v.bits));
    CHECK(rsa_verify(padding, kind, view(n), view(e), digest.data(), view(sig)));
    std::vector<uint8_t> d2 = digest;
    d2[d2.size() - 1] ^= 1;
    CHECK(!rsa_verify(padding, kind, view(n), view(e), d2.data(), view(sig)));
    std::vector<uint8_t> s2 = sig;
    s2[s2.size() / 2] ^= 0x04;
    CHECK(!rsa_verify(padding, kind, view(n), view(e), digest.data(), view(s2)));
    CHECK(!rsa_verify(padding, kind, view(n), view(e), digest.data(), view(std::vector<uint8_t>(sig.begin() + 1, sig.end()))));   // wrong length
    CHECK(!rsa_verify(padding == RsaPadding::Pss ? RsaPadding::Pkcs1V15 : RsaPadding::Pss, kind, view(n), view(e), digest.data(), view(sig)));
    std::vector<uint8_t> ones = n;   // a signature equal to the modulus is out of range
    CHECK(!rsa_verify(padding, kind, view(n), view(e), digest.data(), view(ones)));
  }
  CHECK(equal_constant_time(d, d, 20));
  uint8_t other[64];
  memcpy(other, d, 64);
  other[19] ^= 1;
  CHECK(!equal_constant_time(d, other, 20) && equal_constant_time(d, other, 19));
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("crypto tests passed\n");
  return 0;
}
