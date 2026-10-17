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

// Host-side tests for src/Crypto/X509: certificates and chains built by OpenSSL (tests/X509Vectors.inc, from
// tests/data/make_x509_vectors.py), good and bad, the host-name rules, and the system's own root certificates.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Crypto/X509.hpp"

using namespace crypto;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "X509Vectors.inc"

struct HeapAllocator final : allocators::IAllocator {
  long live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

typedef std::vector<std::vector<uint8_t>> Der;
template <size_t N>
static Der load(const char* const (&hexes)[N]) {
  Der out;
  for (const char* h : hexes) {
    std::vector<uint8_t> v;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) v.push_back(static_cast<uint8_t>(nib(h[i]) * 16 + nib(h[i + 1])));
    out.push_back(v);
  }
  return out;
}
static std::vector<ConstByteView> views(const Der& d) {
  std::vector<ConstByteView> v;
  for (const auto& c : d) v.push_back(ConstByteView(c.data(), c.size()));
  return v;
}
static CertStatus verify(const Der& chain, const Der& anchors, VerifyOptions options, HeapAllocator& alloc) {
  TrustStore store(alloc);
  for (const auto& a : anchors) CHECK(store.add(ConstByteView(a.data(), a.size())));
  const std::vector<ConstByteView> v = views(chain);
  return verify_chain(v.data(), static_cast<uint32_t>(v.size()), store, options);
}

int main() {
  HeapAllocator alloc;
  VerifyOptions opt;
  opt.now = kNow;

  // dates
  for (const TimePair& p : kTimePairs) CHECK(unix_time(p.y, p.mo, p.d, p.h, p.mi, p.s) == p.t);
  CHECK(unix_time(2023, 2, 29, 0, 0, 0) == 0 && unix_time(2024, 2, 29, 0, 0, 0) != 0 && unix_time(2025, 13, 1, 0, 0, 0) == 0 && unix_time(1969, 12, 31, 0, 0, 0) == 0);

  const Der a = load(kChainA), a_root = load(kChainARoot), a_int = load(kChainAIntermediate), b = load(kChainB), b_root = load(kChainBRoot);

  // parsing
  {
    Certificate c;
    CHECK(parse_certificate(ConstByteView(a[0].data(), a[0].size()), &c) == CertStatus::Ok);
    CHECK(c.key_type == KeyType::EcP256 && c.sig_alg == SignatureAlgorithm::EcdsaSha384 && !c.is_ca && c.has_key_usage && (c.key_usage & Certificate::kDigitalSignature) && !(c.key_usage & Certificate::kKeyCertSign));
    CHECK(c.has_ext_key_usage && c.server_auth && !c.san.empty() && c.not_before < kNow && c.not_after > kNow && !c.self_issued());
    CHECK(certificate_matches_host(c, "printer.example.com") && certificate_matches_host(c, "PRINTER.Example.COM") && certificate_matches_host(c, "foo.lab.example.com"));
    CHECK(!certificate_matches_host(c, "a.b.lab.example.com") && !certificate_matches_host(c, "lab.example.com") && !certificate_matches_host(c, ".lab.example.com") && !certificate_matches_host(c, "example.com") && !certificate_matches_host(c, ""));
    CHECK(certificate_matches_host(c, "printer.example.com.") && !certificate_matches_host(c, "xprinter.example.com") && !certificate_matches_host(c, "printer.example.comx"));
    CHECK(certificate_matches_ipv4(c, 0x0a000002) && !certificate_matches_ipv4(c, 0x0a000003));
    CHECK(parse_certificate(ConstByteView(a[1].data(), a[1].size()), &c) == CertStatus::Ok && c.key_type == KeyType::EcP384 && c.is_ca && c.path_length == 0 && (c.key_usage & Certificate::kKeyCertSign) && c.sig_alg == SignatureAlgorithm::RsaSha256);
    CHECK(parse_certificate(ConstByteView(a[2].data(), a[2].size()), &c) == CertStatus::Ok && c.key_type == KeyType::Rsa && c.is_ca && c.self_issued() && c.path_length == -1);
    CHECK(parse_certificate(ConstByteView(b[0].data(), b[0].size()), &c) == CertStatus::Ok && c.key_type == KeyType::Rsa && c.sig_alg == SignatureAlgorithm::RsaPssSha384);
    Certificate issuer;
    CHECK(parse_certificate(ConstByteView(a[1].data(), a[1].size()), &issuer) == CertStatus::Ok);
    CHECK(parse_certificate(ConstByteView(a[0].data(), a[0].size()), &c) == CertStatus::Ok && verify_signature(c, issuer) == CertStatus::Ok);
    CHECK(verify_signature(c, c) == CertStatus::BadSignature);
  }

  // good chains
  CHECK(verify(a, a_root, opt, alloc) == CertStatus::Ok);
  VerifyOptions named = opt;
  named.hostname = "printer.example.com";
  CHECK(verify(a, a_root, named, alloc) == CertStatus::Ok);
  named.hostname = "printer.lab.example.com";
  CHECK(verify(a, a_root, named, alloc) == CertStatus::Ok);
  named.hostname = "evil.example.com";
  CHECK(verify(a, a_root, named, alloc) == CertStatus::HostnameMismatch);
  VerifyOptions by_ip = opt;
  by_ip.ipv4 = 0x0a000002;
  CHECK(verify(a, a_root, by_ip, alloc) == CertStatus::Ok);
  by_ip.ipv4 = 0x0a000009;
  CHECK(verify(a, a_root, by_ip, alloc) == CertStatus::HostnameMismatch);
  CHECK(verify(load(kChainALeafOnly), a_int, opt, alloc) == CertStatus::Ok);       // anchored at the intermediate
  CHECK(verify(load(kChainALeafOnly), a_root, opt, alloc) == CertStatus::UntrustedRoot);   // the intermediate is missing
  CHECK(verify(load(kChainAShuffled), a_root, opt, alloc) == CertStatus::Ok);
  CHECK(verify(b, b_root, opt, alloc) == CertStatus::Ok);                          // PSS leaf, RSA intermediate under an EC root
  named.hostname = "www.example.org";
  CHECK(verify(b, b_root, named, alloc) == CertStatus::Ok);
  CHECK(verify(a, b_root, opt, alloc) == CertStatus::UntrustedRoot);
  CHECK(verify(a, Der(), opt, alloc) == CertStatus::UntrustedRoot);
  CHECK(verify(Der(), a_root, opt, alloc) == CertStatus::NoCertificates);
  Der both = a_root;
  both.insert(both.end(), b_root.begin(), b_root.end());
  CHECK(verify(a, both, opt, alloc) == CertStatus::Ok && verify(b, both, opt, alloc) == CertStatus::Ok);

  // bad ones
  CHECK(verify(load(kExpired), a_root, opt, alloc) == CertStatus::Expired);
  CHECK(verify(load(kNotYetValid), a_root, opt, alloc) == CertStatus::NotYetValid);
  VerifyOptions no_clock = opt;
  no_clock.now = 0;
  CHECK(verify(load(kExpired), a_root, no_clock, alloc) == CertStatus::Ok);
  VerifyOptions later = opt;
  later.now = kNow + 3500ull * 86400;   // beyond the validity of the whole chain
  CHECK(verify(a, a_root, later, alloc) == CertStatus::Expired);
  CHECK(verify(a, load(kOtherRoot), opt, alloc) == CertStatus::BadSignature);      // same name, another key
  CHECK(verify(load(kNotCa), a_root, opt, alloc) == CertStatus::NotCa);
  CHECK(verify(load(kPathLen), a_root, opt, alloc) == CertStatus::PathLengthExceeded);
  CHECK(verify(load(kNoKeyCertSign), a_root, opt, alloc) == CertStatus::NotCa);
  CHECK(verify(load(kCriticalUnknown), a_root, opt, alloc) == CertStatus::UnknownCriticalExtension);
  CHECK(verify(load(kClientAuthOnly), a_root, opt, alloc) == CertStatus::BadLeafUsage);
  VerifyOptions loose = opt;
  loose.require_server_auth = false;
  CHECK(verify(load(kClientAuthOnly), a_root, loose, alloc) == CertStatus::Ok);
  CHECK(verify(load(kSha1Signed), a_root, opt, alloc) == CertStatus::UnsupportedAlgorithm);
  loose = opt;
  loose.allow_sha1 = true;
  CHECK(verify(load(kSha1Signed), a_root, loose, alloc) == CertStatus::Ok);
  CHECK(verify(load(kRsa1024Leaf), a_root, opt, alloc) == CertStatus::WeakKey);
  loose = opt;
  loose.min_rsa_bits = 1024;
  CHECK(verify(load(kRsa1024Leaf), a_root, loose, alloc) == CertStatus::Ok);
  CHECK(verify(load(kSelfSigned), Der(), opt, alloc) == CertStatus::UntrustedRoot);
  CHECK(verify(load(kSelfSigned), load(kSelfSignedAnchor), opt, alloc) == CertStatus::Ok);   // pinned as it is
  named.hostname = "selfsigned.example.com";
  CHECK(verify(load(kSelfSigned), load(kSelfSignedAnchor), named, alloc) == CertStatus::Ok);
  named.hostname = "other.example.com";
  CHECK(verify(load(kSelfSigned), load(kSelfSignedAnchor), named, alloc) == CertStatus::HostnameMismatch);
  CHECK(verify(load(kCaAsLeafAnchor), load(kCaAsLeafAnchor), opt, alloc) == CertStatus::Ok);
  CHECK(verify(load(kTamperedSignature), a_root, opt, alloc) == CertStatus::BadSignature);
  CHECK(verify(load(kTamperedBody), a_root, opt, alloc) != CertStatus::Ok);
  named.hostname = "nosan.example.com";
  CHECK(verify(load(kNoSan), a_root, opt, alloc) == CertStatus::Ok && verify(load(kNoSan), a_root, named, alloc) == CertStatus::HostnameMismatch);   // the common name is not consulted
  // a chain with a repeated certificate and one that is too long
  {
    Der dup = a;
    for (int i = 0; i < 14; ++i) dup.push_back(a[1]);
    CHECK(verify(dup, a_root, opt, alloc) == CertStatus::ChainTooLong);
  }

  // damaged certificates never read out of bounds and never validate
  {
    const std::vector<uint8_t>& leaf = a[0];
    Certificate c;
    for (size_t cut = 0; cut < leaf.size(); ++cut) CHECK(parse_certificate(ConstByteView(leaf.data(), cut), &c) != CertStatus::Ok);
    uint32_t seed = 7;
    int accepted = 0;
    TrustStore store(alloc);
    for (const auto& r : a_root) store.add(ConstByteView(r.data(), r.size()));
    for (int n = 0; n < 3000; ++n) {
      std::vector<uint8_t> bad = leaf;
      seed = seed * 1664525 + 1013904223;
      bad[(seed >> 8) % bad.size()] ^= static_cast<uint8_t>(1 << ((seed >> 4) & 7));
      const ConstByteView chain[3] = {ConstByteView(bad.data(), bad.size()), ConstByteView(a[1].data(), a[1].size()), ConstByteView(a[2].data(), a[2].size())};
      if (verify_chain(chain, 3, store, opt) == CertStatus::Ok) ++accepted;
    }
    CHECK(accepted == 0);
  }

  // base64 and PEM
  {
    uint8_t out[64];
    CHECK(base64_decode("TWFu", 4, out, sizeof out) == 3 && memcmp(out, "Man", 3) == 0);
    CHECK(base64_decode("TWE=", 4, out, sizeof out) == 2 && base64_decode("TQ==", 4, out, sizeof out) == 1);
    CHECK(base64_decode("TW\nFu\r\n", 7, out, sizeof out) == 3);
    CHECK(base64_decode("TWFu!", 5, out, sizeof out) == -1 && base64_decode("TQ==TQ==", 8, out, sizeof out) == -1 && base64_decode("TWFu", 4, out, 2) == -1 && base64_decode("T", 1, out, sizeof out) == -1);
  }
  {
    // the system's root certificates (skipped where there are none)
    FILE* f = fopen("/etc/ssl/certs/ca-certificates.crt", "rb");
    if (f) {
      std::string text;
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
      fclose(f);
      TrustStore store(alloc);
      const uint32_t added = store.add_pem(text.data(), text.size());
      uint32_t parsed = 0, unsupported = 0, rsa = 0, ec = 0, self_issued = 0;
      for (uint32_t i = 0; i < store.count(); ++i) {
        Certificate c;
        if (parse_certificate(store.at(i), &c) != CertStatus::Ok) continue;
        ++parsed;
        if (c.key_type == KeyType::Unsupported || c.sig_alg == SignatureAlgorithm::Unknown) ++unsupported;
        if (c.key_type == KeyType::Rsa) ++rsa;
        if (c.key_type == KeyType::EcP256 || c.key_type == KeyType::EcP384) ++ec;
        if (c.self_issued()) ++self_issued;
      }
      printf("system roots: %u in the bundle, %u parsed, %u rsa, %u ec, %u with an algorithm we do not do, %u self-issued\n", added, parsed, rsa, ec, unsupported, self_issued);
      CHECK(added > 100 && parsed == added);
      // every self-signed root verifies its own signature
      uint32_t verified = 0;
      for (uint32_t i = 0; i < store.count(); ++i) {
        Certificate c;
        if (parse_certificate(store.at(i), &c) != CertStatus::Ok || !c.self_issued() || c.key_type == KeyType::Unsupported || c.sig_alg == SignatureAlgorithm::Unknown) continue;
        if (verify_signature(c, c) == CertStatus::Ok) ++verified;
      }
      printf("self-signature of %u roots verified\n", verified);
      CHECK(verified + unsupported >= self_issued * 9 / 10);
    }
  }
  CHECK(alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("x509 tests passed\n");
  return 0;
}
