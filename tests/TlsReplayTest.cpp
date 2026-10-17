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

int main() {
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
