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

// The wire formats TLS handshakes are written and read in, shared by TlsSocket (TCP records) and Tls13Client (QUIC): a reader and a writer for the
// big-endian integers and length-prefixed vectors of RFC 8446, and the numbers of messages, extensions, groups, alerts and signature schemes.
#pragma once
#include <stdint.h>
#include "Crypto/Crypto.hpp"
#include "Crypto/Ecc.hpp"
#include "Utility/ArrayView.hpp"

namespace network::tlswire {

// ---- wire helpers

struct Reader {
  const uint8_t* p;
  uint32_t n;
  bool ok = true;
  explicit Reader(ConstByteView v) : p(v.data()), n(static_cast<uint32_t>(v.size())) {}
  uint8_t u8() {
    if (n < 1) { ok = false; return 0; }
    --n;
    return *p++;
  }
  uint16_t u16() {
    const uint32_t hi = u8();
    return static_cast<uint16_t>(hi << 8 | u8());
  }
  uint32_t u24() {
    const uint32_t hi = u8();
    const uint32_t mid = u8();
    return hi << 16 | mid << 8 | u8();
  }
  ConstByteView take(uint32_t len) {
    if (n < len) { ok = false; return ConstByteView(); }
    ConstByteView v(p, len);
    p += len;
    n -= len;
    return v;
  }
  bool done() const { return ok && n == 0; }
};

struct Writer {
  uint8_t* p;
  uint32_t cap;
  uint32_t n = 0;
  bool ok = true;
  Writer(uint8_t* buffer, uint32_t capacity) : p(buffer), cap(capacity) {}
  void u8(uint32_t v) {
    if (n + 1 > cap) { ok = false; return; }
    p[n++] = static_cast<uint8_t>(v);
  }
  void u16(uint32_t v) { u8(v >> 8); u8(v); }
  void u24(uint32_t v) { u8(v >> 16); u8(v >> 8); u8(v); }
  void bytes(const uint8_t* b, uint32_t len) {
    if (n + len > cap) { ok = false; return; }
    for (uint32_t i = 0; i < len; ++i) p[n + i] = b[i];
    n += len;
  }
  uint32_t mark16() { const uint32_t at = n; u16(0); return at; }
  uint32_t mark24() { const uint32_t at = n; u24(0); return at; }
  void end16(uint32_t at) {
    if (!ok) return;
    const uint32_t len = n - at - 2;
    p[at] = static_cast<uint8_t>(len >> 8);
    p[at + 1] = static_cast<uint8_t>(len);
  }
  void end24(uint32_t at) {
    if (!ok) return;
    const uint32_t len = n - at - 3;
    p[at] = static_cast<uint8_t>(len >> 16);
    p[at + 1] = static_cast<uint8_t>(len >> 8);
    p[at + 2] = static_cast<uint8_t>(len);
  }
};

constexpr uint8_t kHsClientHello = 1, kHsServerHello = 2, kHsNewSessionTicket = 4, kHsEncryptedExtensions = 8, kHsCertificate = 11, kHsServerKeyExchange = 12,
                  kHsCertificateRequest = 13, kHsServerHelloDone = 14, kHsCertificateVerify = 15, kHsClientKeyExchange = 16, kHsFinished = 20, kHsKeyUpdate = 24, kHsMessageHash = 254;
constexpr uint16_t kExtServerName = 0, kExtGroups = 10, kExtPointFormats = 11, kExtSigAlgs = 13, kExtAlpn = 16, kExtEncryptThenMac = 22, kExtEms = 23, kExtPreSharedKey = 41, kExtVersions = 43, kExtCookie = 44, kExtPskModes = 45, kExtKeyShare = 51, kExtRenegotiation = 0xff01;
constexpr uint16_t kGroupX25519 = 0x001d, kGroupP256 = 0x0017, kGroupP384 = 0x0018;
constexpr uint8_t kAlertCloseNotify = 0, kAlertUnexpected = 10, kAlertBadMac = 20, kAlertOverflow = 22, kAlertHandshakeFailure = 40, kAlertBadCertificate = 42, kAlertUnsupportedCertificate = 43,
                  kAlertCertificateExpired = 45, kAlertCertificateUnknown = 46, kAlertIllegalParameter = 47, kAlertUnknownCa = 48, kAlertDecode = 50, kAlertDecrypt = 51, kAlertProtocolVersion = 70,
                  kAlertInsufficientSecurity = 71, kAlertInternal = 80, kAlertNoRenegotiation = 100, kAlertMissingExtension = 109, kAlertUnsupportedExtension = 110;
inline constexpr uint8_t kHelloRetryMagic[32] = {0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
                                      0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};

// signature schemes
constexpr uint16_t kSigEcdsaP256 = 0x0403, kSigEcdsaP384 = 0x0503, kSigPssSha256 = 0x0804, kSigPssSha384 = 0x0805, kSigPssSha512 = 0x0806,
                   kSigPkcs1Sha256 = 0x0401, kSigPkcs1Sha384 = 0x0501, kSigPkcs1Sha512 = 0x0601;

inline bool group_supported(uint16_t g) { return g == kGroupX25519 || g == kGroupP256 || g == kGroupP384; }
inline crypto::EcCurve curve_of(uint16_t g) { return g == kGroupP256 ? crypto::EcCurve::P256 : crypto::EcCurve::P384; }

inline uint32_t length_of(const char* s) {
  uint32_t n = 0;
  while (s && s[n]) ++n;
  return n;
}

inline crypto::HashKind scheme_hash(uint16_t s) {
  switch (s) {
    case kSigEcdsaP384: case kSigPssSha384: case kSigPkcs1Sha384: return crypto::HashKind::Sha384;
    case kSigPssSha512: case kSigPkcs1Sha512: return crypto::HashKind::Sha512;
    default: return crypto::HashKind::Sha256;
  }
}

}  // namespace network::tlswire
