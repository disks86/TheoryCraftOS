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

// Host-side tests for QUIC: the pieces (variable length integers, number ranges, packet protection against the RFC 9001 and an independent
// implementation), the stream buffers, the frames, loss recovery through a scripted network, and whole connections between the client and a
// server written for the test. tests/quic_live.sh runs it against real servers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "HostNetwork.hpp"
#include "Crypto/Random.hpp"
#include "Network/Quic/QuicCommon.hpp"
#include "Network/Quic/QuicConnection.hpp"
#include "Network/Quic/QuicCrypto.hpp"
#include "Network/Tls/TlsKeys.hpp"
#include "Network/Tls/TlsSession.hpp"

using namespace network;
using namespace network::quic;

static int g_failed = 0;
static long g_live = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { ++g_live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --g_live; free(p); } }
};

static std::string hex(ConstByteView v) {
  std::string out;
  char b[4];
  for (uint64_t i = 0; i < v.size(); ++i) {
    snprintf(b, sizeof b, "%02x", v[i]);
    out += b;
  }
  return out;
}
static std::vector<uint8_t> unhex(const char* s) {
  std::vector<uint8_t> out;
  for (; s[0] && s[1]; s += 2) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    out.push_back(static_cast<uint8_t>(nib(s[0]) * 16 + nib(s[1])));
  }
  return out;
}

static void test_varints() {
  const struct { uint64_t value; const char* bytes; } cases[] = {
      {37, "25"}, {15293, "7bbd"}, {494878333, "9d7f3e7d"}, {151288809941952652ull, "c2197c5eff14e88c"}, {0, "00"}, {63, "3f"}, {64, "4040"}, {16383, "7fff"}, {16384, "80004000"},
      {1073741823, "bfffffff"}, {1073741824, "c000000040000000"}, {kMaxVarint, "ffffffffffffffff"}};
  for (const auto& c : cases) {
    uint8_t buffer[8];
    ByteWriter w{ByteView(buffer)};
    write_varint(w, c.value);
    CHECK(w.ok() && hex(w.written()) == c.bytes && varint_size(c.value) == w.position());
    const std::vector<uint8_t> raw = unhex(c.bytes);
    ByteReader r{ConstByteView(raw.data(), raw.size())};
    CHECK(read_varint(r) == c.value && r.ok() && r.at_end());
  }
  uint8_t buffer[8];
  ByteWriter too_big{ByteView(buffer)};
  write_varint(too_big, kMaxVarint + 1);
  CHECK(!too_big.ok());
  const uint8_t cut[2] = {0x80, 0x00};   // says 4 bytes, has 2
  ByteReader r{ConstByteView(cut)};
  read_varint(r);
  CHECK(!r.ok());
  ByteReader empty{ConstByteView()};
  read_varint(empty);
  CHECK(!empty.ok());
  uint8_t two[2];
  ByteWriter w2{ByteView(two)};
  write_varint2(w2, 1200);
  CHECK(hex(w2.written()) == "44b0");
}

static void test_ranges() {
  TestAllocator a;
  {
    RangeSet s(a);
    CHECK(s.empty() && !s.contains(0));
    s.add(5, 7);
    s.add(1, 2);
    s.add(10, 10);
    CHECK(s.count() == 3 && s.smallest() == 1 && s.largest() == 10 && s.contains(6) && !s.contains(3) && !s.contains(8) && s.total() == 6);
    s.add(3, 4);   // joins the first two and the third range
    CHECK(s.count() == 2 && s.at(0).first == 1 && s.at(0).last == 7 && s.at(1).first == 10);
    s.add(8, 9);
    CHECK(s.count() == 1 && s.at(0).first == 1 && s.at(0).last == 10);
    s.add(0, 0);
    s.add(11, 12);
    CHECK(s.count() == 1 && s.at(0).first == 0 && s.at(0).last == 12);
    s.add(5, 6);   // inside
    CHECK(s.count() == 1);
    s.remove(4, 6);
    CHECK(s.count() == 2 && s.at(0).last == 3 && s.at(1).first == 7 && !s.contains(5));
    s.remove(0, 1);
    CHECK(s.at(0).first == 2);
    s.remove(8, 100);
    CHECK(s.largest() == 7);
    s.remove_below(3);
    CHECK(s.at(0).first == 3 && s.at(0).last == 3 && s.count() == 2);
    s.remove(0, 100);
    CHECK(s.empty());
    for (uint64_t i = 0; i < 20; ++i) s.add(i * 10, i * 10 + 4);
    s.trim(5);
    CHECK(s.count() == 5 && s.smallest() == 150);
    RangeSet t(a);
    t.add(10, 20);
    t.add(5, 5);
    t.add(30, 40);
    t.add(21, 29);
    CHECK(t.count() == 2);
    CHECK(t.at(0).first == 5 && t.at(0).last == 5 && t.at(1).first == 10 && t.at(1).last == 40);
  }
  CHECK(g_live == 0);
}

static void test_crypto() {
  // RFC 9001 A.1: the Initial secrets for a destination connection ID
  PacketKeys client, server;
  const std::vector<uint8_t> dcid = unhex("8394c8f03e515708");
  CHECK(derive_initial_keys(ConstByteView(dcid.data(), dcid.size()), &client, &server) && client.valid() && server.valid());
  // protect a packet: the header of an Initial packet with a 4 byte packet number 2, and a payload of the beginning of a ClientHello and padding; the result
  // must be what an independent implementation (aioquic) makes of it
  std::vector<uint8_t> packet = unhex("c300000001088394c8f03e5157080000449e00000002");
  const std::vector<uint8_t> payload = unhex("060040f1010000ed0303ebf8fa56f12939b9584a3896472ec40bb863cfd3e86804fe3a47f06a2b69484c00000413011302010000c0");
  const uint32_t pn_offset = static_cast<uint32_t>(packet.size()) - 4;
  packet.insert(packet.end(), payload.begin(), payload.end());
  packet.resize(1200 - 16, 0);
  packet.resize(1200, 0);
  CHECK(protect_packet(client, 2, ByteView(packet.data(), packet.size()), pn_offset, 4));
  CHECK(hex(ConstByteView(packet.data(), 40)) == "c000000001088394c8f03e5157080000449e7b9aec34d1b1c98dd7689fb8ec11d242b123dc9bd8ba" && hex(ConstByteView(packet.data() + 1184, 16)) == "e10395e07209d7f06c7b7dfb752285aa");
  // and back: header protection comes off, the packet number is recovered, the payload decrypts
  uint32_t pn_length = 0;
  uint64_t truncated = 0;
  std::vector<uint8_t> received = packet;
  CHECK(remove_header_protection(client, ByteView(received.data(), received.size()), pn_offset, true, &pn_length, &truncated) && pn_length == 4 && truncated == 2 && received[0] == 0xc3);
  CHECK(expand_packet_number(-1, truncated, pn_length) == 2 && decrypt_packet(client, 2, ByteView(received.data(), received.size()), pn_offset + pn_length));
  CHECK(std::vector<uint8_t>(received.begin() + pn_offset + 4, received.begin() + pn_offset + 4 + payload.size()) == payload);
  // wrong key, wrong packet number, a changed bit: refused
  std::vector<uint8_t> again = packet;
  remove_header_protection(client, ByteView(again.data(), again.size()), pn_offset, true, &pn_length, &truncated);
  CHECK(!decrypt_packet(server, 2, ByteView(again.data(), again.size()), pn_offset + 4) && !decrypt_packet(client, 3, ByteView(again.data(), again.size()), pn_offset + 4));
  again[100] ^= 1;
  CHECK(!decrypt_packet(client, 2, ByteView(again.data(), again.size()), pn_offset + 4));
  // a packet that is too short to sample
  uint8_t tiny[20] = {0xc3};
  CHECK(!remove_header_protection(client, ByteView(tiny), 8, true, &pn_length, &truncated));
  // packet number recovery, RFC 9000 A.3
  CHECK(expand_packet_number(0xa82f30ea, 0x9b32, 2) == 0xa82f9b32 && expand_packet_number(-1, 5, 1) == 5 && expand_packet_number(255, 0, 1) == 256 && expand_packet_number(0x3ff, 0x2, 1) == 0x402);
  CHECK(expand_packet_number(0x100, 0xff, 1) == 0xff && expand_packet_number(0x1f0, 0x10, 1) == 0x210);
  // key update: a different key and IV, the same header protection
  PacketKeys next;
  CHECK(client.next_generation(&next) && next.valid());
  std::vector<uint8_t> sealed = unhex("c300000001088394c8f03e5157080000449e00000002");
  sealed.resize(sealed.size() + 40, 0x55);
  std::vector<uint8_t> sealed2 = sealed;
  CHECK(protect_packet(client, 7, ByteView(sealed.data(), sealed.size()), pn_offset, 4) && protect_packet(next, 7, ByteView(sealed2.data(), sealed2.size()), pn_offset, 4) && sealed != sealed2);
  // ChaCha20-Poly1305 packets (header protection by ChaCha20)
  PacketKeys chacha;
  uint8_t secret[32];
  for (int i = 0; i < 32; ++i) secret[i] = static_cast<uint8_t>(i + 1);
  CHECK(chacha.set_secret(tls::AeadKind::ChaCha20Poly1305, crypto::HashKind::Sha256, ConstByteView(secret)));
  std::vector<uint8_t> short_packet = {0x40, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 9};   // flags, an 8 byte connection ID, a packet number of 4 bytes
  short_packet[0] = 0x43;
  short_packet.resize(short_packet.size() + 30, 0x42);
  const std::vector<uint8_t> original = short_packet;
  CHECK(protect_packet(chacha, 9, ByteView(short_packet.data(), short_packet.size()), 9, 4) && short_packet != original);
  CHECK(remove_header_protection(chacha, ByteView(short_packet.data(), short_packet.size()), 9, false, &pn_length, &truncated) && pn_length == 4 && truncated == 9 && short_packet[0] == 0x43 &&
        decrypt_packet(chacha, 9, ByteView(short_packet.data(), short_packet.size()), 13));
  // the Retry integrity tag of RFC 9001 A.4
  const std::vector<uint8_t> odcid = unhex("8394c8f03e515708");
  const std::vector<uint8_t> retry = unhex("ff000000010008f067a5502a4262b5746f6b656e");
  uint8_t tag[16];
  CHECK(retry_integrity_tag(ConstByteView(odcid.data(), odcid.size()), ConstByteView(retry.data(), retry.size()), tag) && hex(ConstByteView(tag)) == "04a265ba2eff4d829058fb3f0f2496ba");
}

// A network that only records what is sent and lets the test move the clock.
struct ScriptedUdp final : IUdpSocket {
  std::vector<std::vector<uint8_t>> sent;
  uint16_t local_port() const override { return 40000; }
  bool send_to(Ipv4Address, uint16_t, ConstByteView data) override {
    sent.emplace_back(data.data(), data.data() + data.size());
    return true;
  }
  bool receive_from(ByteView, Ipv4Address*, uint16_t*, uint32_t*) override { return false; }
  uint32_t pending() const override { return 0; }
  void close() override {}
};

struct ScriptedStack final : INetworkStack {
  explicit ScriptedStack(allocators::IAllocator& a) : allocator(a), dns_(a) {}
  allocators::IAllocator& allocator;
  kernel::RecursiveMutex mutex_;
  hostnet::HostDns dns_;
  hostnet::HostDhcp dhcp_;
  uint64_t now = 1000;
  ScriptedUdp* udp = nullptr;
  kernel::RecursiveMutex& mutex() override { return mutex_; }
  void poll() override {}
  uint64_t now_ms() const override { return now; }
  IpConfiguration configuration() const override { return IpConfiguration{}; }
  void configure(const IpConfiguration&) override {}
  void start_dhcp() override {}
  const IDhcpClient& dhcp() const override { return dhcp_; }
  bool wait_for_address(uint32_t) override { return true; }
  bool resolve(StringView, uint32_t, Ipv4Address*) override { return false; }
  IDnsResolver& dns() override { return dns_; }
  bool ping(Ipv4Address, uint32_t, uint32_t*) override { return false; }
  SharedPointer<IUdpSocket> udp_open(uint16_t) override {
    auto s = SharedPointer<ScriptedUdp>::make(allocator);
    udp = s.get();
    return SharedPointer<IUdpSocket>(s);
  }
  SharedPointer<ITcpSocket> tcp_open() override { return SharedPointer<ITcpSocket>(); }
};

struct FixedEntropy final : crypto::IEntropySource {
  bool read(ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(counter++ * 37 + 11);
    return true;
  }
  uint32_t counter = 0;
};

// What the first Initial packet of a datagram carries: whether it has a CRYPTO frame, and at which offset the first one starts.
static bool initial_crypto(const std::vector<uint8_t>& datagram, const PacketKeys& keys, bool* has_crypto, uint64_t* offset) {
  std::vector<uint8_t> packet = datagram;
  ByteReader r{ConstByteView(packet.data(), packet.size())};
  const uint8_t flags = r.u8();
  r.be32();
  r.take(r.u8());   // destination connection id
  r.take(r.u8());   // source connection id
  const uint64_t token = read_varint(r);
  r.take(token);
  const uint64_t length = read_varint(r);
  if (!r.ok() || (flags & 0xf0) != 0xc0 || length > r.remaining()) return false;
  const uint32_t pn_offset = static_cast<uint32_t>(r.position());
  uint32_t pn_length = 0;
  uint64_t truncated = 0;
  const uint32_t total = pn_offset + static_cast<uint32_t>(length);
  if (!remove_header_protection(keys, ByteView(packet.data(), total), pn_offset, true, &pn_length, &truncated)) return false;
  if (!decrypt_packet(keys, expand_packet_number(-1, truncated, pn_length), ByteView(packet.data(), total), pn_offset + pn_length)) return false;
  ByteReader f{ConstByteView(packet.data() + pn_offset + pn_length, length - pn_length - 16)};
  *has_crypto = false;
  while (f.remaining() > 0 && f.ok()) {
    const uint64_t type = read_varint(f);
    if (type == 0 || type == 1) continue;   // PADDING, PING
    if (type != 6) return false;
    *offset = read_varint(f);
    f.take(read_varint(f));
    *has_crypto = true;
    return f.ok();
  }
  return true;
}

// The first flight is lost. Each of the two packets sent when the probe timer fires carries the ClientHello again, from the start: the server must be able
// to start from either (a server may refuse an Initial packet without CRYPTO data as the first it hears of the connection).
static void test_probe_packets() {
  TestAllocator a;
  {
    ScriptedStack stack(a);
    FixedEntropy entropy;
    crypto::DrbgRandom random(entropy);
    CHECK(random.initialize());
    crypto::TrustStore trust(a);
    QuicConfig config;
    config.tls.trust = &trust;
    config.tls.random = &random;
    QuicConnection quic(stack, a, config);
    CHECK(quic.connect(Ipv4Address::make(10, 1, 2, 3), 443, "example.com"));
    quic.poll();
    CHECK(stack.udp && stack.udp->sent.size() == 1 && stack.udp->sent[0].size() >= 1200);
    for (int i = 0; i < 400 && stack.udp->sent.size() < 3; ++i) {
      stack.now += 25;
      quic.poll();
    }
    CHECK(stack.udp->sent.size() >= 3);
    // the Initial keys come from the destination connection ID the client chose, which every first-flight packet carries
    const std::vector<uint8_t>& first = stack.udp->sent[0];
    PacketKeys client, server;
    CHECK(first.size() > 6 && derive_initial_keys(ConstByteView(first.data() + 6, first[5]), &client, &server));
    for (size_t k = 0; k < 3 && k < stack.udp->sent.size(); ++k) {
      bool has = false;
      uint64_t offset = ~0ull;
      CHECK(initial_crypto(stack.udp->sent[k], client, &has, &offset) && has && offset == 0);
    }
    const auto st = quic.stats();
    CHECK(st.ptos >= 1 && !quic.established());
  }
  CHECK(g_live == 0);
}

// ---- TLS 1.3 session tickets

static std::vector<uint8_t> ticket_message(uint32_t lifetime, uint32_t age_add, const std::vector<uint8_t>& nonce, const std::vector<uint8_t>& ticket) {
  std::vector<uint8_t> m = {static_cast<uint8_t>(lifetime >> 24), static_cast<uint8_t>(lifetime >> 16), static_cast<uint8_t>(lifetime >> 8), static_cast<uint8_t>(lifetime),
                            static_cast<uint8_t>(age_add >> 24), static_cast<uint8_t>(age_add >> 16), static_cast<uint8_t>(age_add >> 8), static_cast<uint8_t>(age_add),
                            static_cast<uint8_t>(nonce.size())};
  m.insert(m.end(), nonce.begin(), nonce.end());
  m.push_back(static_cast<uint8_t>(ticket.size() >> 8));
  m.push_back(static_cast<uint8_t>(ticket.size()));
  m.insert(m.end(), ticket.begin(), ticket.end());
  m.push_back(0);   // no extensions
  m.push_back(0);
  return m;
}

static void test_tickets() {
  TestAllocator a;
  const std::vector<uint8_t> master(32, 0x42), nonce = {1, 2}, blob = {9, 8, 7, 6, 5};
  {
    TlsTicket t;
    const std::vector<uint8_t> m = ticket_message(3600, 0x01020304, nonce, blob);
    CHECK(tls::make_ticket(crypto::HashKind::Sha256, 0x1301, ConstByteView(master.data(), master.size()), ConstByteView(m.data(), m.size()), "Example.COM", true, 5000, &t));
    CHECK(std::string(t.server_name) == "example.com" && t.quic && t.suite_id == 0x1301 && t.lifetime_s == 3600 && t.age_add == 0x01020304 && t.received_ms == 5000);
    CHECK(t.ticket_length == 5 && t.ticket[0] == 9 && t.ticket[4] == 5 && t.psk_length == 32);
    uint8_t expected[32];
    CHECK(tls::hkdf_expand_label(crypto::HashKind::Sha256, ConstByteView(master.data(), master.size()), "resumption", ConstByteView(nonce.data(), nonce.size()), ByteView(expected, 32)));
    CHECK(memcmp(expected, t.psk, 32) == 0);
    // what the ClientHello carries for it
    uint8_t ext[400];
    const uint32_t modes = tls::write_psk_modes(ext, sizeof ext);
    CHECK(modes == 6 && hex(ConstByteView(ext, 6)) == "002d00020101");
    const uint32_t n = tls::write_pre_shared_key(t, crypto::HashKind::Sha256, 5000 + 7000, ext, sizeof ext);
    // type, length, identities { length, identity { length, ticket, age } }, binders { length, binder { length, 32 bytes } }
    CHECK(n == 2 + 2 + 2 + 2 + 5 + 4 + 2 + 1 + 32 && ext[0] == 0x00 && ext[1] == 0x29 && ext[2] * 256u + ext[3] == n - 4);
    CHECK(ext[4] * 256u + ext[5] == 2 + 5 + 4 && ext[6] * 256u + ext[7] == 5 && ext[8] == 9);
    const uint32_t age = (7000u + 0x01020304u);
    CHECK(ext[13] == (age >> 24) && ext[14] == ((age >> 16) & 255) && ext[15] == ((age >> 8) & 255) && ext[16] == (age & 255));
    CHECK(ext[17] * 256u + ext[18] == 33 && ext[19] == 32);
    CHECK(tls::write_pre_shared_key(t, crypto::HashKind::Sha256, 5000, ext, 20) == 0);   // does not fit
    // the binder depends on the PSK and on the message
    uint8_t b1[32], b2[32], b3[32];
    const std::vector<uint8_t> hello = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(tls::psk_binder(crypto::HashKind::Sha256, ConstByteView(t.psk, 32), ConstByteView(hello.data(), hello.size()), b1));
    CHECK(tls::psk_binder(crypto::HashKind::Sha256, ConstByteView(t.psk, 32), ConstByteView(hello.data(), hello.size()), b2) && memcmp(b1, b2, 32) == 0);
    std::vector<uint8_t> other = hello;
    other[3] ^= 1;
    CHECK(tls::psk_binder(crypto::HashKind::Sha256, ConstByteView(t.psk, 32), ConstByteView(other.data(), other.size()), b3) && memcmp(b1, b3, 32) != 0);
    uint8_t psk2[32];
    memcpy(psk2, t.psk, 32);
    psk2[0] ^= 1;
    CHECK(tls::psk_binder(crypto::HashKind::Sha256, ConstByteView(psk2, 32), ConstByteView(hello.data(), hello.size()), b3) && memcmp(b1, b3, 32) != 0);
    // a SHA-384 suite: 48 byte secrets
    const std::vector<uint8_t> master384(48, 7);
    TlsTicket t2;
    CHECK(tls::make_ticket(crypto::HashKind::Sha384, 0x1302, ConstByteView(master384.data(), 48), ConstByteView(m.data(), m.size()), "b.test", false, 1, &t2) && t2.psk_length == 48);
  }
  {   // damaged or useless messages
    TlsTicket t;
    const ConstByteView key(master.data(), master.size());
    std::vector<uint8_t> m = ticket_message(3600, 1, nonce, blob);
    CHECK(!tls::make_ticket(crypto::HashKind::Sha256, 0x1301, key, ConstByteView(m.data(), m.size() - 1), "x", false, 0, &t));   // cut short
    m.push_back(0);
    CHECK(!tls::make_ticket(crypto::HashKind::Sha256, 0x1301, key, ConstByteView(m.data(), m.size()), "x", false, 0, &t));       // trailing byte
    m = ticket_message(0, 1, nonce, blob);
    CHECK(!tls::make_ticket(crypto::HashKind::Sha256, 0x1301, key, ConstByteView(m.data(), m.size()), "x", false, 0, &t));       // lifetime 0: do not use
    m = ticket_message(60, 1, nonce, {});
    CHECK(!tls::make_ticket(crypto::HashKind::Sha256, 0x1301, key, ConstByteView(m.data(), m.size()), "x", false, 0, &t));       // no ticket
    m = ticket_message(60, 1, nonce, std::vector<uint8_t>(kMaxTicketBytes + 1, 1));
    CHECK(!tls::make_ticket(crypto::HashKind::Sha256, 0x1301, key, ConstByteView(m.data(), m.size()), "x", false, 0, &t));       // too long to keep
    m = ticket_message(60, 1, nonce, blob);
    CHECK(!tls::make_ticket(crypto::HashKind::Sha256, 0x1301, key, ConstByteView(m.data(), m.size()), "", false, 0, &t));        // no server name
    m = ticket_message(99999999, 1, nonce, blob);
    CHECK(tls::make_ticket(crypto::HashKind::Sha256, 0x1301, key, ConstByteView(m.data(), m.size()), "x", false, 0, &t) && t.lifetime_s == 604800);   // (cut to a week)
  }
  {   // the cache
    TlsSessionCache cache(a);
    auto make = [&](const char* name, bool quic, uint32_t lifetime, uint64_t at, uint8_t id) {
      TlsTicket t;
      const std::vector<uint8_t> m = ticket_message(lifetime, 5, nonce, {id, id});
      CHECK(tls::make_ticket(crypto::HashKind::Sha256, 0x1301, ConstByteView(master.data(), master.size()), ConstByteView(m.data(), m.size()), name, quic, at, &t));
      return t;
    };
    TlsTicket out;
    CHECK(!cache.take("a.test", false, 0, &out) && cache.count() == 0);
    CHECK(cache.store(make("a.test", false, 100, 1000, 1)) && cache.store(make("A.Test", false, 100, 2000, 2)) && cache.store(make("a.test", true, 100, 1500, 3)) && cache.count() == 3);
    CHECK(cache.take("A.TEST", false, 3000, &out) && out.ticket[0] == 2 && cache.count() == 2);   // the newest, any case; it is used up
    CHECK(cache.take("a.test", false, 3000, &out) && out.ticket[0] == 1 && !cache.take("a.test", false, 3000, &out));
    CHECK(cache.take("a.test", true, 3000, &out) && out.ticket[0] == 3 && !cache.take("a.test", true, 3000, &out) && cache.count() == 0);
    // expiry (and an expired ticket is dropped, not handed out); a clock that went backwards is no good either
    CHECK(cache.store(make("b.test", false, 10, 1000, 4)));
    CHECK(!cache.take("b.test", false, 1000 + 10000, &out) && cache.count() == 0);
    CHECK(cache.store(make("b.test", false, 10, 1000, 5)) && cache.take("b.test", false, 1000 + 9999, &out));
    CHECK(cache.store(make("b.test", false, 10, 5000, 6)) && !cache.take("b.test", false, 1000, &out));
    cache.clear();
    // limits: four per server, the oldest goes; 32 in all
    for (uint8_t i = 1; i <= 6; ++i) cache.store(make("c.test", false, 1000, i, i));
    CHECK(cache.count() == TlsSessionCache::kPerServer);
    CHECK(cache.take("c.test", false, 10, &out) && out.ticket[0] == 6);
    cache.clear();
    for (int i = 0; i < 50; ++i) {
      char name[32];
      snprintf(name, sizeof name, "h%d.test", i);
      cache.store(make(name, false, 1000, 1, static_cast<uint8_t>(i)));
    }
    CHECK(cache.count() == TlsSessionCache::kMaxTickets && cache.take("h49.test", false, 10, &out) && !cache.take("h0.test", false, 10, &out));
  }
  CHECK(g_live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_varints();
  test_ranges();
  test_crypto();
  test_probe_packets();
  test_tickets();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("quic tests passed\n");
  return 0;
}
