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

// Host-side test for src/Network/Tls: plays back what real servers (openssl s_server) sent to our deterministic TLS client - TLS 1.3 and 1.2,
// RSA / P-256 / P-384 certificates, AES-GCM and ChaCha20-Poly1305, a HelloRetryRequest, key updates - delivered in awkward pieces, and checks the
// handshake completes and the application data comes out right. Then damages the recordings in many ways (bit flips, cut-offs, noise) and checks
// the client never accepts altered data and never reads outside its buffers. tests/tls_live.sh does the same against live servers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "TlsDeterministic.hpp"
#include "Network/Tls/TlsSocket.hpp"

using namespace network;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "TlsVectors.inc"

struct HeapAllocator final : allocators::IAllocator {
  long live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

struct FakeStack final : INetworkStack {
  kernel::RecursiveMutex m;
  mutable uint64_t now = 0;
  kernel::RecursiveMutex& mutex() override { return m; }
  void poll() override {}
  uint64_t now_ms() const override { return now += 1; }
  IpConfiguration configuration() const override { return {}; }
  void configure(const IpConfiguration&) override {}
  void start_dhcp() override {}
  const IDhcpClient& dhcp() const override { return *static_cast<IDhcpClient*>(nullptr); }
  bool wait_for_address(uint32_t) override { return true; }
  bool resolve(StringView, uint32_t, Ipv4Address*) override { return false; }
  IDnsResolver& dns() override { return *static_cast<IDnsResolver*>(nullptr); }
  bool ping(Ipv4Address, uint32_t, uint32_t*) override { return false; }
  SharedPointer<IUdpSocket> udp_open(uint16_t) override { return SharedPointer<IUdpSocket>(); }
  SharedPointer<ITcpSocket> tcp_open() override { return SharedPointer<ITcpSocket>(); }
};

// An established connection whose peer says what was recorded, in odd-sized pieces with gaps, then closes.
struct ReplaySocket final : ITcpSocket {
  std::vector<uint8_t> stream;
  size_t offset = 0;
  uint32_t lcg = 12345;
  uint32_t calls = 0;
  std::vector<uint8_t> sent;
  bool aborted = false, closed = false;
  TcpState state() const override { return TcpState::Established; }
  bool connect(Ipv4Address, uint16_t) override { return true; }
  bool listen(uint16_t) override { return false; }
  SharedPointer<ITcpSocket> accept() override { return SharedPointer<ITcpSocket>(); }
  uint32_t send(ConstByteView data) override { sent.insert(sent.end(), data.data(), data.data() + data.size()); return static_cast<uint32_t>(data.size()); }
  uint32_t receive(ByteView buffer) override {
    ++calls;
    if (calls % 7 == 3) return 0;   // nothing there yet
    lcg = lcg * 1664525 + 1013904223;
    size_t n = 1 + (lcg >> 16) % 1500;
    if (n > buffer.size()) n = buffer.size();
    if (n > stream.size() - offset) n = stream.size() - offset;
    for (size_t i = 0; i < n; ++i) buffer[i] = stream[offset + i];
    offset += n;
    return static_cast<uint32_t>(n);
  }
  uint32_t available() const override { return static_cast<uint32_t>(stream.size() - offset); }
  uint32_t unsent_or_unacknowledged() const override { return 0; }
  bool is_connected() const override { return !aborted; }
  bool peer_closed() const override { return offset == stream.size(); }
  bool failed() const override { return false; }
  void close() override { closed = true; }
  void abort() override { aborted = true; }
  uint16_t local_port() const override { return 1; }
  Ipv4Address remote_address() const override { return Ipv4Address::make(10, 0, 0, 2); }
  uint16_t remote_port() const override { return 443; }
};

static std::vector<uint8_t> unhex(const char* s) {
  std::vector<uint8_t> v;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) v.push_back(static_cast<uint8_t>(nib(s[i]) * 16 + nib(s[i + 1])));
  return v;
}

static uint64_t g_now = 0;
static uint64_t fixed_clock(void*) { return g_now; }

struct Outcome {
  bool handshake = false, failed = false, peer_closed = false, clean_close = false;
  std::vector<uint8_t> plaintext;
  TlsError error = TlsError::None;
  std::string suite;
  uint16_t version = 0;
};

static Outcome run(const TlsVector& v, const std::vector<uint8_t>& stream, HeapAllocator& alloc) {
  Outcome o;
  FakeStack stack;
  SeededEntropy entropy(v.seed);
  crypto::DrbgRandom rng(entropy);
  rng.initialize();
  crypto::TrustStore trust(alloc);
  trust.add_pem(v.ca_pem, strlen(v.ca_pem));
  TlsConfig config;
  config.random = &rng;
  config.server_name = "localhost";
  config.trust = &trust;
  config.clock = fixed_clock;
  g_now = v.now;
  const bool tls13 = v.version == 0x0304;
  config.allow_tls12 = !tls13;
  config.allow_tls13 = tls13;
  config.handshake_timeout_ms = 1000000;
  SharedPointer<ReplaySocket> replay = SharedPointer<ReplaySocket>::make(alloc);
  replay->stream = stream;
  {
    SharedPointer<ITcpSocket> inner(replay);
    SharedPointer<TlsSocket> tls = SharedPointer<TlsSocket>::make(alloc, stack, alloc, config, inner);
    for (int i = 0; i < 20000 && !tls->failed(); ++i) {
      if (tls->is_connected()) o.handshake = true;
      uint8_t buf[700];
      const uint32_t n = tls->receive(ByteView(buf, sizeof buf));
      o.plaintext.insert(o.plaintext.end(), buf, buf + n);
      if (tls->peer_closed() && replay->offset == replay->stream.size()) break;
    }
    o.failed = tls->failed();
    o.peer_closed = tls->peer_closed();
    o.error = tls->error();
    o.clean_close = o.error == TlsError::None;
    o.suite = tls->cipher_suite_name();
    o.version = tls->protocol_version();
    tls->abort();
  }
  return o;
}


// ---- TLS 1.2 AES-CBC records (the cipher suites of servers that have no AEAD): against records made by an independent implementation (Python's hmac and
// cryptography), round trips of every length, and records that were damaged

namespace {

struct FixedIv final : crypto::IRandom {
  uint8_t next = 0xa0;
  bool fill(ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = next++;
    return true;
  }
};

std::string hex_of(const uint8_t* p, size_t n) {
  std::string out;
  char b[3];
  for (size_t i = 0; i < n; ++i) {
    snprintf(b, sizeof b, "%02x", p[i]);
    out += b;
  }
  return out;
}

void test_cbc_records() {
  const char* text = "hello world, this is a TLS record";
  const ConstByteView plain(reinterpret_cast<const uint8_t*>(text), strlen(text));
  uint8_t key[32], mac_key[48];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
  for (int i = 0; i < 48; ++i) mac_key[i] = static_cast<uint8_t>(0x10 + i);
  const struct {
    const char* name;
    tls::AeadKind kind;
    crypto::HashKind mac;
    bool etm;
    const char* record;
  } kats[] = {
      {"sha1 mac-then-encrypt", tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha1, false,
       "1703030050a0a1a2a3a4a5a6a7a8a9aaabacadaeafcac502260009a8a5332d88008b7e0bb3cc47fc2f15c463ba5f157b64f219fcaf70e5d03c2dcc3d14300cb956e05bcb0eddf7e2a77f920a930e582f2f81f4b15c"},
      {"sha1 encrypt-then-mac", tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha1, true,
       "1703030054a0a1a2a3a4a5a6a7a8a9aaabacadaeafcac502260009a8a5332d88008b7e0bb3cc47fc2f15c463ba5f157b64f219fcafa5b5e76aa0dca494f3d6ad5d3ddc090fc01afae189ecb6727313e0dbf8336e8d4320b40c"},
      {"sha256 mac-then-encrypt", tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha256, false,
       "1703030060a0a1a2a3a4a5a6a7a8a9aaabacadaeafcac502260009a8a5332d88008b7e0bb3cc47fc2f15c463ba5f157b64f219fcaf5abb60061ba43030b4f77aec7a2e284f6d50ee0970447b9bc68a94e51602ec094b8f76c06035a7ca23b893eb837dcaea"},
      {"sha384 encrypt-then-mac (AES-256)", tls::AeadKind::Aes256Cbc, crypto::HashKind::Sha384, true,
       "1703030070a0a1a2a3a4a5a6a7a8a9aaabacadaeaf5218e8f47adb6dded5498b9df62b4c5a1a0e518f0c88228f3f5970d9ba82c71b16f7c787b95a1044f8c179e2fd659f9d11801ff3a5a731a737c0ffcec911d77ed9ace5da27bd8c86d718427d4dfbc720b6c201825842fe675314177c3edc5b23"}};
  for (const auto& kat : kats) {
    FixedIv iv;
    tls::RecordProtection w, r;
    CHECK(w.set_cbc_keys(kat.kind, kat.mac, key, mac_key, kat.etm, &iv) && r.set_cbc_keys(kat.kind, kat.mac, key, mac_key, kat.etm, &iv));
    uint8_t out[1024];
    const uint32_t n = w.seal(tls::kApplicationData, plain, out, sizeof out);
    CHECK(n > 0 && hex_of(out, n) == kat.record);
    ConstByteView got;
    uint8_t type = 0;
    CHECK(n > 5 && r.open(tls::kApplicationData, out + 5, n - 5, &got, &type) && type == tls::kApplicationData && got.size() == plain.size() && memcmp(got.data(), text, got.size()) == 0);
  }
  // every length, both directions of every mode, several records in a row (the sequence number is part of the MAC)
  const struct { tls::AeadKind kind; crypto::HashKind mac; } modes[] = {{tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha1},
                                                                         {tls::AeadKind::Aes256Cbc, crypto::HashKind::Sha256},
                                                                         {tls::AeadKind::Aes256Cbc, crypto::HashKind::Sha384}};
  for (const auto& m : modes)
    for (int etm = 0; etm < 2; ++etm) {
      FixedIv iv;
      tls::RecordProtection w, r;
      CHECK(w.set_cbc_keys(m.kind, m.mac, key, mac_key, etm, &iv) && r.set_cbc_keys(m.kind, m.mac, key, mac_key, etm, &iv));
      std::vector<uint8_t> data(tls::kMaxPlaintext);
      for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + 3);
      for (uint32_t length = 0; length <= 100 || length == 255 || length == 4096 || length == tls::kMaxPlaintext; length = length < 100 ? length + 1 : (length == 100 ? 255 : (length == 255 ? 4096 : (length == 4096 ? tls::kMaxPlaintext : tls::kMaxPlaintext + 1)))) {
        std::vector<uint8_t> out(tls::kRecordHeader + tls::kMaxPlaintext + 512);
        const uint32_t n = w.seal(tls::kHandshake, ConstByteView(data.data(), length), out.data(), static_cast<uint32_t>(out.size()));
        ConstByteView got;
        uint8_t type = 0;
        CHECK(n >= tls::kRecordHeader + 32 && out[0] == tls::kHandshake && (out[3] * 256u + out[4]) == n - 5);
        CHECK(r.open(tls::kHandshake, out.data() + 5, n - 5, &got, &type) && got.size() == length && memcmp(got.data(), data.data(), length) == 0);
      }
    }
  // damage: every single bit of a record is flipped, and the record cut short at every length; nothing is accepted (and nothing is read outside the record)
  for (int etm = 0; etm < 2; ++etm) {
    FixedIv iv;
    tls::RecordProtection w;
    CHECK(w.set_cbc_keys(tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha256, key, mac_key, etm, &iv));
    uint8_t record[512];
    const uint32_t n = w.seal(tls::kApplicationData, plain, record, sizeof record);
    for (uint32_t bit = 0; bit < (n - 5) * 8; ++bit) {
      std::vector<uint8_t> copy(record + 5, record + n);
      copy[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      FixedIv unused;
      tls::RecordProtection r;
      CHECK(r.set_cbc_keys(tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha256, key, mac_key, etm, &unused));
      ConstByteView got;
      uint8_t type = 0;
      CHECK(!r.open(tls::kApplicationData, copy.data(), static_cast<uint32_t>(copy.size()), &got, &type));
    }
    for (uint32_t length = 0; length < n - 5; ++length) {
      std::vector<uint8_t> copy(record + 5, record + 5 + length);
      FixedIv unused;
      tls::RecordProtection r;
      r.set_cbc_keys(tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha256, key, mac_key, etm, &unused);
      ConstByteView got;
      uint8_t type = 0;
      CHECK(!r.open(tls::kApplicationData, copy.data(), length, &got, &type));
    }
    // the wrong content type, a record out of order (a different sequence number)
    {
      std::vector<uint8_t> copy(record + 5, record + n);
      FixedIv unused;
      tls::RecordProtection r;
      r.set_cbc_keys(tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha256, key, mac_key, etm, &unused);
      ConstByteView got;
      uint8_t type = 0;
      CHECK(!r.open(tls::kHandshake, copy.data(), static_cast<uint32_t>(copy.size()), &got, &type));
      uint8_t skip[16];
      FixedIv iv2;
      tls::RecordProtection w2, r2;
      w2.set_cbc_keys(tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha256, key, mac_key, etm, &iv2);
      r2.set_cbc_keys(tls::AeadKind::Aes128Cbc, crypto::HashKind::Sha256, key, mac_key, etm, &iv2);
      (void)skip;
      uint8_t first[512], second[512];
      const uint32_t n1 = w2.seal(tls::kApplicationData, plain, first, sizeof first), n2 = w2.seal(tls::kApplicationData, plain, second, sizeof second);
      CHECK(!r2.open(tls::kApplicationData, second + 5, n2 - 5, &got, &type) && n1 > 0);   // the second one first: wrong sequence number
    }
  }
}

}  // namespace

int main() {
  test_cbc_records();
  HeapAllocator alloc;
  for (const TlsVector& v : kTlsVectors) {
    const std::vector<uint8_t> stream = unhex(v.server_stream), plain = unhex(v.plaintext);
    // the recording as it was
    const Outcome good = run(v, stream, alloc);
    printf("%-18s %s: handshake %s, %zu bytes of data, %s\n", v.name, good.suite.c_str(), good.handshake ? "ok" : "FAILED", good.plaintext.size(), good.clean_close ? "close_notify" : "no close_notify");
    CHECK(good.handshake && !good.failed && good.suite == v.suite && good.version == v.version);
    CHECK(good.plaintext == plain);
    const bool recorded_without_close = std::string(v.name) == "Tls13KeyUpdate";   // that server was stopped rather than closed
    CHECK(good.peer_closed && good.clean_close != recorded_without_close);
    // damage: flip a bit at sampled positions; the client may fail or (for harmless changes) succeed, but never hands out different data
    const size_t step = stream.size() / 50 > 0 ? stream.size() / 50 : 1;
    uint32_t seed = 99;
    int failures = 0, harmless = 0;
    for (size_t pos = 0; pos < stream.size(); pos += step) {
      std::vector<uint8_t> bad = stream;
      seed = seed * 1664525 + 1013904223;
      bad[pos] ^= static_cast<uint8_t>(1u << ((seed >> 16) & 7));
      const Outcome o = run(v, bad, alloc);
      const bool prefix = o.plaintext.size() <= plain.size() && std::equal(o.plaintext.begin(), o.plaintext.end(), plain.begin());
      CHECK(prefix);
      if (o.failed) ++failures;
      else if (o.handshake && o.plaintext == plain && o.clean_close) ++harmless;
      else if (o.handshake && o.plaintext == plain) ++harmless;
    }
    // a record's version bytes are the only places a flipped bit can be ignored; nearly everything else must fail
    CHECK(failures >= static_cast<int>(stream.size() / step) * 9 / 10);
    printf("  %d of %zu bit flips refused, %d harmless\n", failures, stream.size() / step + 1, harmless);
    // cut short at sampled positions: what came out is a prefix, and a handshake that did not finish is never reported as done
    for (size_t cut = 0; cut < stream.size(); cut += step * 2 + 7) {
      std::vector<uint8_t> shorter(stream.begin(), stream.begin() + cut);
      const Outcome o = run(v, shorter, alloc);
      CHECK(o.plaintext.size() <= plain.size() && std::equal(o.plaintext.begin(), o.plaintext.end(), plain.begin()));
      if (!recorded_without_close && cut + 64 < stream.size() && o.plaintext == plain) CHECK(!o.clean_close);   // the final close_notify is missing
    }
    // noise instead of a server, and a server that sends one record header over and over
    {
      std::vector<uint8_t> noise(3000);
      for (size_t i = 0; i < noise.size(); ++i) { seed = seed * 1664525 + 1013904223; noise[i] = static_cast<uint8_t>(seed >> 16); }
      const Outcome o = run(v, noise, alloc);
      CHECK(!o.handshake && o.plaintext.empty());
      std::vector<uint8_t> headers;
      for (int i = 0; i < 400; ++i) { const uint8_t h[5] = {22, 3, 3, 0, 4}; headers.insert(headers.end(), h, h + 5); const uint8_t body[4] = {2, 0, 0, 0}; headers.insert(headers.end(), body, body + 4); }
      const Outcome o2 = run(v, headers, alloc);
      CHECK(!o2.handshake && o2.plaintext.empty());
    }
  }
  CHECK(alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("tls replay tests passed\n");
  return 0;
}
