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

// Host-side tests for SNMPv3 message security: key localization against RFC 3414's published keys, answers captured from a pysnmp
// agent (MD5-DES, SHA-AES, SHA-256-AES, authentication only), round trips for every algorithm pair, and tampering.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "Network/Snmp/SnmpUsm.hpp"

using namespace network;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "SnmpV3Vectors.inc"

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

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
static Oid O(const char* t) { Oid o; Oid::parse(t, &o); return o; }

static UsmAuth auth_of(const std::string& s) { return s == "md5" ? UsmAuth::HmacMd5 : s == "sha" ? UsmAuth::HmacSha1 : s == "sha224" ? UsmAuth::HmacSha224 : s == "sha256" ? UsmAuth::HmacSha256 : UsmAuth::None; }
static UsmPriv priv_of(const std::string& s) { return s == "des" ? UsmPriv::Des : s == "aes" ? UsmPriv::Aes128 : UsmPriv::None; }

int main() {
  TestAllocator a;
  // RFC 3414 A.3.1 / A.3.2: the password "maplesyrup" at engine 00 00 00 00 00 00 00 00 00 00 00 02.
  {
    const uint8_t engine[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
    uint8_t key[32];
    CHECK(usm_localize_password(crypto::HashKind::Md5, "maplesyrup", engine, 12, key) && hex(key, 16) == "526f5eed9fcce26f8964c2930787d82b");
    CHECK(usm_localize_password(crypto::HashKind::Sha1, "maplesyrup", engine, 12, key) && hex(key, 20) == "6695febc9288e36282235fc7151f128497b38f3f");
    CHECK(!usm_localize_password(crypto::HashKind::Md5, "", engine, 12, key));
    UsmKeys keys;
    CHECK(usm_make_keys(UsmAuth::HmacSha1, "maplesyrup", UsmPriv::Aes128, "maplesyrup", engine, 12, &keys) && keys.auth_key_length == 20 && keys.priv_key_length == 16);
    CHECK(!usm_make_keys(UsmAuth::None, "x", UsmPriv::Des, "y", engine, 12, &keys) && !usm_make_keys(UsmAuth::HmacMd5, "", UsmPriv::None, "", engine, 12, &keys));
    CHECK(usm_auth_length(UsmAuth::HmacMd5) == 12 && usm_auth_length(UsmAuth::HmacSha256) == 24 && usm_auth_length(UsmAuth::None) == 0);
  }
  // Answers a pysnmp agent gave: decoded, authenticated and decrypted with keys made from the users' passwords.
  for (const V3Vector& v : kV3Vectors) {
    const std::vector<uint8_t> report = unhex(v.report), response = unhex(v.response);
    SnmpMessage m;
    CHECK(decode_snmp_v3(ConstByteView(report.data(), report.size()), nullptr, &m) == UsmStatus::Ok && m.pdu == SnmpPdu::Report && m.v3.engine_id_length > 4 && m.v3.boots >= 1);
    const uint8_t engine_length = m.v3.engine_id_length;
    uint8_t engine[32];
    memcpy(engine, m.v3.engine_id, engine_length);
    UsmKeys keys;
    CHECK(usm_make_keys(auth_of(v.auth), v.auth_password, priv_of(v.priv), v.priv_password, engine, engine_length, &keys));
    CHECK(decode_snmp_v3(ConstByteView(response.data(), response.size()), &keys, &m) == UsmStatus::Ok);
    CHECK(m.pdu == SnmpPdu::Response && m.count == 1 && m.binds[0].oid == O("1.3.6.1.2.1.1.1.0") && m.binds[0].value.type == SnmpType::OctetString &&
          strncmp(reinterpret_cast<const char*>(m.binds[0].value.bytes), "PySNMP engine", 13) == 0 && strcmp(m.v3.user, v.user) == 0);
    CHECK(m.v3.engine_id_length == engine_length && memcmp(m.v3.engine_id, engine, engine_length) == 0 && m.v3.context_engine_id_length == engine_length);
    // Without the keys the level cannot be met; with the wrong ones the tag fails; every bit of damage is caught.
    if (keys.auth != UsmAuth::None) {
      CHECK(decode_snmp_v3(ConstByteView(response.data(), response.size()), nullptr, &m) == UsmStatus::WrongLevel || keys.priv == UsmPriv::None);
      UsmKeys wrong = keys;
      wrong.auth_key[0] ^= 1;
      CHECK(decode_snmp_v3(ConstByteView(response.data(), response.size()), &wrong, &m) == UsmStatus::AuthFailed);
      if (keys.priv != UsmPriv::None) {
        UsmKeys wrong_priv = keys;
        wrong_priv.priv_key[3] ^= 0x10;   // (not a DES parity bit)
        const UsmStatus s = decode_snmp_v3(ConstByteView(response.data(), response.size()), &wrong_priv, &m);
        CHECK(s == UsmStatus::DecryptFailed || s == UsmStatus::Malformed);   // (the tag is fine; the plain text is garbage)
      }
      int caught = 0;
      for (size_t i = 0; i < response.size(); ++i) {
        std::vector<uint8_t> bad = response;
        bad[i] ^= 0x04;
        if (decode_snmp_v3(ConstByteView(bad.data(), bad.size()), &keys, &m) != UsmStatus::Ok) ++caught;
      }
      CHECK(caught == static_cast<int>(response.size()));   // authenticated: no single-bit change gets through
    }
    // Truncations never crash.
    for (size_t n = 0; n < response.size(); ++n) decode_snmp_v3(ConstByteView(response.data(), n), &keys, &m);
  }
  // Round trips for every algorithm pair, both directions of the privacy IV (boots and time matter for AES).
  {
    const uint8_t engine[9] = {0x80, 0, 0x1f, 0x88, 0x80, 1, 2, 3, 4};
    const UsmAuth auths[] = {UsmAuth::None, UsmAuth::HmacMd5, UsmAuth::HmacSha1, UsmAuth::HmacSha224, UsmAuth::HmacSha256};
    const UsmPriv privs[] = {UsmPriv::None, UsmPriv::Des, UsmPriv::Aes128};
    for (UsmAuth au : auths) {
      for (UsmPriv pr : privs) {
        if (au == UsmAuth::None && pr != UsmPriv::None) continue;
        UsmKeys keys;
        CHECK(usm_make_keys(au, "authpassword", pr, "privpassword", engine, sizeof engine, &keys));
        SnmpMessage req;
        req.version = 3;
        req.pdu = SnmpPdu::Get;
        req.request_id = 1234;
        req.count = 2;
        req.binds[0].oid = O("1.3.6.1.2.1.1.1.0");
        req.binds[1].oid = O("1.3.6.1.2.1.43.10.2.1.4.1.1");
        req.v3.msg_id = 99;
        req.v3.flags = 4;
        req.v3.engine_id_length = sizeof engine;
        memcpy(req.v3.engine_id, engine, sizeof engine);
        memcpy(req.v3.context_engine_id, engine, sizeof engine);
        req.v3.context_engine_id_length = sizeof engine;
        req.v3.boots = 3;
        req.v3.time = 1000;
        strcpy(req.v3.user, "printer-admin");
        strcpy(req.v3.context_name, "ctx");
        ByteBuffer out(a);
        const UsmKeys* use = au == UsmAuth::None ? nullptr : &keys;
        CHECK(encode_snmp_v3(req, use, 0x1122334455667788ull, a, &out));
        SnmpMessage back;
        CHECK(decode_snmp_v3(out.view(), use, &back) == UsmStatus::Ok && back.request_id == 1234 && back.count == 2 && back.binds[1].oid == req.binds[1].oid &&
              back.v3.msg_id == 99 && back.v3.boots == 3 && back.v3.time == 1000 && strcmp(back.v3.user, "printer-admin") == 0 && strcmp(back.v3.context_name, "ctx") == 0 &&
              back.v3.engine_id_length == sizeof engine && (back.v3.flags & 4) && (back.v3.flags & 3) == ((au != UsmAuth::None) | (pr != UsmPriv::None) << 1));
        // Changing a byte of the encrypted or authenticated message fails it.
        if (au != UsmAuth::None) {
          std::vector<uint8_t> bytes(out.view().data(), out.view().data() + out.size());
          bytes[bytes.size() - 3] ^= 0x10;
          CHECK(decode_snmp_v3(ConstByteView(bytes.data(), bytes.size()), &keys, &back) != UsmStatus::Ok);
          // A message that was not protected is refused by a receiver that expects protection.
          SnmpMessage open = req;
          ByteBuffer plain(a);
          CHECK(encode_snmp_v3(open, nullptr, 0, a, &plain) && decode_snmp_v3(plain.view(), &keys, &back) == UsmStatus::WrongLevel);
        }
        // The same request twice differs when it is encrypted (new salt), and the clear text of a private message is not in it.
        if (pr != UsmPriv::None) {
          ByteBuffer again(a);
          CHECK(encode_snmp_v3(req, use, 0x1122334455667789ull, a, &again) && !(again.view().size() == out.size() && memcmp(again.view().data(), out.view().data(), out.size()) == 0));
          std::string text(reinterpret_cast<const char*>(out.view().data()), out.size());
          CHECK(text.find("ctx") == std::string::npos);
        }
      }
    }
  }
  // Privacy without authentication is not allowed; v1/v2c bytes are not v3.
  {
    UsmKeys odd;
    odd.priv = UsmPriv::Aes128;
    SnmpMessage m;
    m.version = 3;
    ByteBuffer out(a);
    CHECK(!encode_snmp_v3(m, &odd, 1, a, &out));
    SnmpMessage v2;
    v2.version = 1;
    v2.count = 1;
    v2.binds[0].oid = O("1.3.6.1.2.1.1.1.0");
    CHECK(encode_snmp(v2, a, &out) && decode_snmp_v3(out.view(), nullptr, &m) == UsmStatus::Malformed);
  }
  CHECK(a.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("snmp security tests passed\n");
  return 0;
}
