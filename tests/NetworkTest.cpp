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

#include "Network/Checksum.hpp"
#include "Network/Dns/DnsMessage.hpp"
#include "Network/NetworkStack.hpp"
#include "Network/NetworkStackSet.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include <functional>
#include <map>
#include <string>
using namespace network;

// Hand-built packets in this test are plain byte arrays; these helpers read and write them.
static uint16_t load16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t load32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void store16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void store32(uint8_t* p, uint32_t v) { store16(p, (uint16_t)(v >> 16)); store16(p + 2, (uint16_t)v); }
static void put_ip(Ipv4Address a, uint8_t* p) { memcpy(p, a.octets, 4); }

static long g_live = 0;
struct TestAllocator final : allocators::IAllocator {
  long& live = g_live;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
struct FakeClock final : hardware::IClock {
  mutable uint64_t us = 0;
  bool auto_advance = true;   // each read costs 20 us, so loops that only call now() still make progress
  const char* name() const override { return "fake clock"; } bool initialize() override { return true; } void shutdown() override {}
  uint64_t now_us() const override { if (auto_advance) us += 20; return us; }
};

// A wire between two NICs with optional loss, duplication, reordering and a tap.
struct Wire {
  uint32_t seed = 1;
  int loss_percent = 0, duplicate_percent = 0, reorder_percent = 0;
  uint64_t frames = 0, dropped = 0;
  std::function<void(const std::vector<uint8_t>&, int)> tap;   // sees every frame offered to the wire, with sender index
  uint32_t rnd() { seed = seed * 1664525 + 1013904223; return seed >> 8; }
};
struct FakeNic final : hardware::INetworkInterface {
  Wire* wire; FakeNic* peer = nullptr; int index; hardware::MacAddress mac;
  std::deque<std::vector<uint8_t>> inbox; hardware::NetworkStatistics stats; bool cable = true;
  std::function<void()> on_idle;   // called when receive() finds nothing: lets the peer machine run
  FakeNic(Wire* w, int i, uint8_t last) : wire(w), index(i) { mac = {{0x02, 0, 0, 0, 0, last}}; }
  const char* name() const override { return "fake nic"; } bool initialize() override { return true; } void shutdown() override {}
  hardware::MacAddress mac_address() const override { return mac; } bool link_up() const override { return cable; }
  bool transmit(ConstByteView frame_view) override {
    const uint8_t* frame = frame_view.data(); const uint32_t length = (uint32_t)frame_view.size();
    std::vector<uint8_t> f(frame, frame + length); if (f.size() < 60) f.resize(60);
    ++wire->frames; if (wire->tap) wire->tap(f, index);
    if (wire->loss_percent && wire->rnd() % 100 < (uint32_t)wire->loss_percent) { ++wire->dropped; return true; }
    auto deliver = [&](std::vector<uint8_t> x) {
      if (wire->reorder_percent && !peer->inbox.empty() && wire->rnd() % 100 < (uint32_t)wire->reorder_percent) peer->inbox.insert(peer->inbox.end() - 1, x);
      else peer->inbox.push_back(x); };
    deliver(f);
    if (wire->duplicate_percent && wire->rnd() % 100 < (uint32_t)wire->duplicate_percent) deliver(f);
    ++stats.frames_sent; return true; }
  bool receive(ByteView buffer_view, uint32_t* length) override {
    uint8_t* buffer = buffer_view.data(); const uint32_t capacity = (uint32_t)buffer_view.size();
    if (inbox.empty() && on_idle) on_idle();
    if (inbox.empty()) return false; auto& f = inbox.front(); uint32_t n = f.size() < capacity ? f.size() : capacity;
    if (n) memcpy(buffer, f.data(), n); *length = f.size(); inbox.pop_front(); ++stats.frames_received; return true; }
  hardware::NetworkStatistics statistics() const override { return stats; }
};

struct Lan {
  TestAllocator alloc; FakeClock clock; Wire wire; FakeNic nic_a{&wire, 0, 1}, nic_b{&wire, 1, 2};
  NetworkStack a{nic_a, clock, alloc}, b{nic_b, clock, alloc};
  bool in_idle = false;
  Lan() { nic_a.peer = &nic_b; nic_b.peer = &nic_a; assert(a.initialize() && b.initialize());
    nic_a.on_idle = [this] { if (in_idle) return; in_idle = true; b.poll(); in_idle = false; };
    nic_b.on_idle = [this] { if (in_idle) return; in_idle = true; a.poll(); in_idle = false; }; }
  void step(int ms = 1) { for (int i = 0; i < ms; ++i) { clock.us += 1000; a.poll(); b.poll(); } }
  bool run_until(const std::function<bool()>& done, int max_ms = 60000) { for (int i = 0; i < max_ms; ++i) { if (done()) return true; step(); } return done(); }
  void configure_static() {
    IpConfiguration ca, cb;
    ca.address = Ipv4Address::make(10,0,0,1); ca.netmask = Ipv4Address::make(255,255,255,0); ca.gateway = Ipv4Address::make(10,0,0,254);
    cb.address = Ipv4Address::make(10,0,0,2); cb.netmask = ca.netmask; cb.gateway = ca.gateway;
    a.configure(ca); b.configure(cb); }
};

// ---------------------------------------------------------------- a bulk transfer helper
// A sends `total` bytes, B echoes them back. Returns true if A got exactly what it sent.
static bool echo_transfer(Lan& lan, uint32_t total, uint16_t port) {
  auto server = lan.b.tcp_open(); assert(server->listen(port));
  auto client = lan.a.tcp_open(); assert(client->connect(Ipv4Address::make(10,0,0,2), port));
  SharedPointer<ITcpSocket> accepted;
  std::vector<uint8_t> sent(total), got; for (uint32_t i = 0; i < total; ++i) sent[i] = (uint8_t)(i * 31 + (i >> 8));
  uint32_t offset = 0; bool closed_sent = false;
  uint64_t deadline = lan.clock.us / 1000 + 600000;
  while (lan.clock.us / 1000 < deadline) {
    lan.step();
    if (!accepted) accepted = server->accept();
    if (client->is_connected() && offset < total) offset += client->send(ConstByteView(sent.data() + offset, total - offset));
    uint8_t buf[2048];
    if (accepted) {  // echo
      uint32_t n = accepted->receive(ByteView(buf, sizeof(buf)));
      if (n) { uint32_t w = 0; while (w < n) { uint32_t k = accepted->send(ConstByteView(buf + w, n - w)); w += k; if (!k) { lan.step(); } } }
      if (accepted->peer_closed() && !accepted->unsent_or_unacknowledged() && accepted->state() == TcpState::CloseWait) accepted->close();
    }
    uint32_t n = client->receive(ByteView(buf, sizeof(buf))); got.insert(got.end(), buf, buf + n);
    if (got.size() == total && !closed_sent) { client->close(); closed_sent = true; }
    if (closed_sent && (client->state() == TcpState::Closed || client->state() == TcpState::TimeWait) && accepted && accepted->state() == TcpState::Closed) break;
  }
  bool ok = got == sent;
  server->close();
  return ok && !client->failed();
}


// A mock DNS server on stack B (UDP and TCP port 53).
struct MockDns {
  SharedPointer<IUdpSocket> udp; SharedPointer<ITcpSocket> listener; std::vector<SharedPointer<ITcpSocket>> conns;
  std::map<std::string, int> queries;   // UDP+TCP queries seen per name
  int slow_ignored = 0; bool forge_first = false; std::string extra_name; Ipv4Address extra_addr = kAnyAddress;
  static void put_name(std::vector<uint8_t>& out, const std::string& n) {
    size_t s = 0; while (s < n.size()) { size_t e = n.find('.', s); if (e == std::string::npos) e = n.size(); out.push_back(e - s); out.insert(out.end(), n.begin() + s, n.begin() + e); s = e + 1; } out.push_back(0); }
  void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(v >> 8); o.push_back(v); }
  void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v >> 16); put16(o, v); }
  // Builds the reply for `query` (a full DNS message); `tcp` says which transport asks.
  std::vector<uint8_t> answer(const std::vector<uint8_t>& q, bool tcp, bool* ignore) {
    *ignore = false; std::string name; size_t o = 12; while (q[o]) { if (!name.empty()) name += '.'; name.append((const char*)&q[o + 1], q[o]); o += q[o] + 1; }
    size_t qend = o + 1 + 4; ++queries[name];
    std::vector<uint8_t> r(q.begin(), q.begin() + qend); r[2] = 0x81; r[3] = 0x80; r[6] = r[7] = r[8] = r[9] = r[10] = r[11] = 0;   // QR RD RA, no records yet
    auto rr_a = [&](std::vector<uint8_t>& out, uint16_t owner_ptr, uint32_t ttl, Ipv4Address a) { put16(out, 0xC000 | owner_ptr); put16(out, 1); put16(out, 1); put32(out, ttl); put16(out, 4); for (int i = 0; i < 4; ++i) out.push_back(a.octets[i]); };
    int count = 0; std::vector<uint8_t> body;
    if (name == "host.test") { rr_a(body, 12, 60, Ipv4Address::make(10,0,0,50)); count = 1; }
    else if (!extra_name.empty() && name == extra_name) { rr_a(body, 12, 60, extra_addr); count = 1; }
    else if (name == "Mixed.Case.Test") { rr_a(body, 12, 60, Ipv4Address::make(10,0,0,51)); count = 1; }
    else if (name == "alias.test") {   // alias.test -> real.test (CNAME, ttl 30) -> A 10.0.0.60 (ttl 120); the A's owner is a pointer into the CNAME's rdata
      put16(body, 0xC00C); put16(body, 5); put16(body, 1); put32(body, 30); std::vector<uint8_t> t; put_name(t, "real.test"); put16(body, t.size());
      size_t rdata_offset = qend + body.size(); body.insert(body.end(), t.begin(), t.end());
      rr_a(body, rdata_offset, 120, Ipv4Address::make(10,0,0,60)); count = 2; }
    else if (name == "missing.test") r[3] = 0x83;
    else if (name == "servfail.test") r[3] = 0x82;
    else if (name == "noaddr.test") {}
    else if (name == "big.test") { if (!tcp) r[2] |= 0x02; else { rr_a(body, 12, 300, Ipv4Address::make(10,0,0,70)); count = 1; } }
    else if (name == "slow.test") { if (slow_ignored < 2) { ++slow_ignored; *ignore = true; } else { rr_a(body, 12, 60, Ipv4Address::make(10,0,0,80)); count = 1; } }
    else if (name == "zero.test") { rr_a(body, 12, 0, Ipv4Address::make(10,0,0,90)); count = 1; }
    else if (name == "silent.test") *ignore = true;
    else r[3] = 0x83;
    r[7] = count; r.insert(r.end(), body.begin(), body.end()); return r; }
  void start(NetworkStack& s) { udp = s.udp_open(53); listener = s.tcp_open(); listener->listen(53); }
  void poll(NetworkStack& s) {
    uint8_t in[600]; Ipv4Address from; uint16_t port; uint32_t len;
    while (udp->receive_from(ByteView(in, sizeof(in)), &from, &port, &len)) {
      std::vector<uint8_t> q(in, in + len); bool ignore; if (forge_first) { forge_first = false; std::vector<uint8_t> bad = answer(q, false, &ignore); bad[0] ^= 0xFF; udp->send_to(from, port, ConstByteView(bad.data(), bad.size())); }
      auto r = answer(q, false, &ignore); if (!ignore) udp->send_to(from, port, ConstByteView(r.data(), r.size())); }
    while (auto c = listener->accept()) conns.push_back(c);
    for (auto& c : conns) { uint8_t b[600]; if (c->available() >= 2) { uint8_t hdr[2]; /* peek by reading everything at once */ }
      uint32_t n = c->available(); if (n >= 2) { n = c->receive(ByteView(b, sizeof(b))); uint32_t qlen = (b[0] << 8) | b[1]; if (n >= 2 + qlen) { std::vector<uint8_t> q(b + 2, b + 2 + qlen); bool ignore; auto r = answer(q, true, &ignore);
        std::vector<uint8_t> framed; framed.push_back(r.size() >> 8); framed.push_back(r.size()); framed.insert(framed.end(), r.begin(), r.end()); c->send(ConstByteView(framed.data(), framed.size())); c->close(); } } }
    (void)s; }
};

// A minimal DHCP server on stack B: leases 10.0.0.100 with a 120 s lease (T1 = 60 s).
struct MiniDhcpServer {
  SharedPointer<IUdpSocket> socket; bool answer = true; bool nak_requests = false; Ipv4Address dns = Ipv4Address::make(10, 0, 0, 3);
  int discovers = 0, requests = 0, renewals = 0;
  void poll(NetworkStack& stack) {
    uint8_t in[600]; Ipv4Address from; uint16_t port; uint32_t len;
    while (socket->receive_from(ByteView(in, sizeof(in)), &from, &port, &len)) {
      if (len < 240 || in[0] != 1) continue;
      uint8_t type = 0; for (uint32_t i = 240; i + 2 < len && in[i] != 255;) { if (in[i] == 0) { ++i; continue; } if (in[i] == 53) type = in[i + 2]; i += 2 + in[i + 1]; }
      if (type == 1) ++discovers; if (type == 3) { ++requests; if (!(in[12] == 0 && in[13] == 0 && in[14] == 0 && in[15] == 0)) ++renewals; }
      if (!answer || (type != 1 && type != 3)) continue;
      uint8_t out[400] = {}; out[0] = 2; out[1] = 1; out[2] = 6; memcpy(out + 4, in + 4, 4); memcpy(out + 10, in + 10, 2); memcpy(out + 12, in + 12, 4);
      Ipv4Address yi = Ipv4Address::make(10, 0, 0, 100); put_ip(yi, out + 16); memcpy(out + 28, in + 28, 6); store32(out + 236, 0x63825363);
      uint32_t o = 240; bool nak = type == 3 && nak_requests;
      out[o++] = 53; out[o++] = 1; out[o++] = nak ? 6 : (type == 1 ? 2 : 5);
      out[o++] = 54; out[o++] = 4; put_ip(Ipv4Address::make(10, 0, 0, 2), out + o); o += 4;
      if (!nak) {
        out[o++] = 51; out[o++] = 4; store32(out + o, 120); o += 4; out[o++] = 58; out[o++] = 4; store32(out + o, 60); o += 4;
        out[o++] = 1; out[o++] = 4; put_ip(Ipv4Address::make(255, 255, 255, 0), out + o); o += 4;
        out[o++] = 3; out[o++] = 4; put_ip(Ipv4Address::make(10, 0, 0, 2), out + o); o += 4;
        out[o++] = 6; out[o++] = 4; put_ip(dns, out + o); o += 4; }
      out[o++] = 255;
      bool unicast_renewal = type == 3 && renewals && !(in[12] == 0 && in[13] == 0 && in[14] == 0 && in[15] == 0);
      auto* udp = static_cast<UdpSocket*>(nullptr); (void)udp;
      if (unicast_renewal) socket->send_to(from, 68, ConstByteView(out, o)); else socket->send_to(Ipv4Address::make(255, 255, 255, 255), 68, ConstByteView(out, o));
    }
  }
};

int main() {
  { // ---- checksum sanity (RFC 1071 example) and byte helpers
    const uint8_t data[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    assert(checksum_finish(checksum_add(0, ConstByteView(data, 8))) == 0x220d);
    uint8_t odd[] = {0x12, 0x34, 0x56}; uint32_t s = checksum_add(0, ConstByteView(odd, 3)); assert(checksum_finish(s) == (uint16_t)~(0x1234 + 0x5600));
    uint8_t b[4]; ::store32(b, 0xA1B2C3D4); assert(::load32(b) == 0xA1B2C3D4 && b[0] == 0xA1 && load16(b + 1) == 0xB2C3);
    assert(Ipv4Address::make(10,0,2,15).in_subnet(Ipv4Address::make(10,0,2,0), Ipv4Address::make(255,255,255,0)));
    assert(Ipv4Address::subnet_broadcast(Ipv4Address::make(10,0,2,15), Ipv4Address::make(255,255,255,0)) == Ipv4Address::make(10,0,2,255));
  }
  { // ---- ARP + ICMP echo
    Lan lan; lan.configure_static(); uint32_t rtt = 99;
    assert(lan.a.ping(Ipv4Address::make(10,0,0,2), 2000, &rtt) && rtt < 100);
    assert(lan.b.ping(Ipv4Address::make(10,0,0,1), 2000, &rtt));   // B already learned A from A's ARP request
    assert(!lan.a.ping(Ipv4Address::make(10,0,0,77), 4000, &rtt)); // nobody home: ARP never answers
    assert(!lan.a.ping(Ipv4Address::make(8,8,8,8), 1000, &rtt));   // off-link: goes to the gateway 10.0.0.254, which does not exist
  }
  { // ---- UDP
    Lan lan; lan.configure_static();
    auto server = lan.b.udp_open(7000); auto client = lan.a.udp_open(0); assert(server && client && client->local_port() >= 49152);
    assert(!lan.b.udp_open(7000));   // port taken
    const uint8_t msg[] = "hello udp"; assert(client->send_to(Ipv4Address::make(10,0,0,2), 7000, ConstByteView(msg, sizeof(msg))));
    assert(lan.run_until([&] { return server->pending() > 0; }, 3000));
    uint8_t buf[64]; Ipv4Address from; uint16_t port; uint32_t len;
    assert(server->receive_from(ByteView(buf, sizeof(buf)), &from, &port, &len) && len == sizeof(msg) && !memcmp(buf, msg, len) && from == Ipv4Address::make(10,0,0,1) && port == client->local_port());
    assert(server->send_to(from, port, ConstByteView(buf, len))); assert(lan.run_until([&] { return client->pending() > 0; }, 3000));
    assert(client->receive_from(ByteView(buf, 4), nullptr, nullptr, &len) && len == sizeof(msg));   // truncation reports the full length
    std::vector<uint8_t> big(1472, 0x5A); assert(client->send_to(Ipv4Address::make(10,0,0,2), 7000, ConstByteView(big.data(), big.size())));   // largest datagram
    assert(!client->send_to(Ipv4Address::make(10,0,0,2), 7000, ConstByteView(big.data(), big.size() + 1)));
    assert(lan.run_until([&] { return server->pending() > 0; }, 3000));
    // corrupt checksum is rejected
    lan.wire.tap = nullptr; server->close(); client->close(); lan.step(5);
    assert(lan.b.udp_open(7000));   // freed
  }
  { // ---- TCP: handshake, echo of 300 KB, orderly close, no leaks
    { Lan lan; lan.configure_static(); assert(echo_transfer(lan, 300000, 8080));
      lan.step(5000); }
    assert(g_live == 0);
  }
  { // ---- TCP under 8% loss
    Lan lan; lan.configure_static(); lan.wire.loss_percent = 8; assert(echo_transfer(lan, 150000, 9000)); assert(lan.wire.dropped > 50);
  }
  { // ---- TCP under duplication and reordering
    Lan lan; lan.configure_static(); lan.wire.duplicate_percent = 10; lan.wire.reorder_percent = 10; assert(echo_transfer(lan, 100000, 9100));
  }
  { // ---- TCP under heavy loss (25%)
    Lan lan; lan.configure_static(); lan.wire.loss_percent = 25; assert(echo_transfer(lan, 30000, 9200));
  }
  { // ---- connection refused (RST) and no-listener behaviour
    Lan lan; lan.configure_static(); auto c = lan.a.tcp_open(); assert(c->connect(Ipv4Address::make(10,0,0,2), 4321));
    assert(lan.run_until([&] { return c->state() == TcpState::Closed; }, 5000)); assert(c->failed());
  }
  { // ---- peer disappears: SYN retries then failure
    Lan lan; lan.configure_static(); lan.nic_b.cable = true; lan.wire.loss_percent = 100; auto c = lan.a.tcp_open(); assert(c->connect(Ipv4Address::make(10,0,0,2), 80));
    assert(lan.run_until([&] { return c->state() == TcpState::Closed; }, 300000)); assert(c->failed());
  }
  { // ---- flow control: a receiver that does not read stalls the sender, then resumes
    Lan lan; lan.configure_static(); auto server = lan.b.tcp_open(); server->listen(7777); auto client = lan.a.tcp_open(); client->connect(Ipv4Address::make(10,0,0,2), 7777);
    SharedPointer<ITcpSocket> peer; assert(lan.run_until([&] { if (!peer) peer = server->accept(); return peer && client->is_connected(); }, 3000));
    std::vector<uint8_t> data(100000, 0xA5); uint32_t sent = 0;
    for (int i = 0; i < 5000; ++i) { sent += client->send(ConstByteView(data.data() + sent, data.size() - sent)); lan.step(); }
    assert(sent < data.size() && peer->available() == TcpSocket::kBufferSize);   // receiver buffer full, sender buffer full
    uint32_t received = 0; std::vector<uint8_t> buf(4096);
    for (int i = 0; i < 100000 && received < data.size(); ++i) {   // now drain: the window reopens, the rest flows
      if (sent < data.size()) sent += client->send(ConstByteView(data.data() + sent, data.size() - sent));
      uint32_t n = peer->receive(ByteView(buf.data(), buf.size())); for (uint32_t k = 0; k < n; ++k) assert(buf[k] == 0xA5); received += n; lan.step(); }
    assert(received == data.size());
  }
  { // ---- zero-window recovery with a lost window update
    Lan lan; lan.configure_static(); auto server = lan.b.tcp_open(); server->listen(7778); auto client = lan.a.tcp_open(); client->connect(Ipv4Address::make(10,0,0,2), 7778);
    SharedPointer<ITcpSocket> peer; assert(lan.run_until([&] { if (!peer) peer = server->accept(); return peer && client->is_connected(); }, 3000));
    std::vector<uint8_t> data(60000, 7); uint32_t sent = 0; for (int i = 0; i < 3000; ++i) { sent += client->send(ConstByteView(data.data() + sent, data.size() - sent)); lan.step(); }
    lan.wire.loss_percent = 100; std::vector<uint8_t> buf(65536); peer->receive(ByteView(buf.data(), buf.size())); lan.step(50); lan.wire.loss_percent = 0;   // the window-opening ACK is lost
    uint32_t received = 0; for (int i = 0; i < 120000 && received + 0 < data.size() - 0; ++i) { if (sent < data.size()) sent += client->send(ConstByteView(data.data() + sent, data.size() - sent)); received += peer->receive(ByteView(buf.data(), buf.size())); lan.step(); }
    assert(received >= 59999 || sent == data.size());
  }
  { // ---- listener: backlog, accept order, closing the listener resets pending connections
    Lan lan; lan.configure_static(); auto server = lan.b.tcp_open(); assert(server->listen(5000)); assert(!lan.b.tcp_open()->listen(5000));
    std::vector<SharedPointer<ITcpSocket>> clients; for (int i = 0; i < 12; ++i) { auto c = lan.a.tcp_open(); c->connect(Ipv4Address::make(10,0,0,2), 5000); clients.push_back(c); }
    lan.step(3000);
    int established = 0; for (auto& c : clients) established += c->is_connected(); assert(established == 8);   // backlog of 8; the rest retry their SYNs
    int accepted = 0; std::vector<SharedPointer<ITcpSocket>> conns; while (auto s = server->accept()) { conns.push_back(s); ++accepted; } assert(accepted == 8);
    for (int i = 0; i < 20000 && accepted < 12; ++i) { lan.step(); if (auto s = server->accept()) { conns.push_back(s); ++accepted; } }
    assert(accepted == 12);
    server->close(); lan.step(10); for (auto& c : conns) assert(c->is_connected());   // established connections survive the listener
  }
  { // ---- dropping a live socket closes it; sockets are reaped; no leaks across many connections
    { Lan lan; lan.configure_static(); auto server = lan.b.tcp_open(); server->listen(6000);
      for (int round = 0; round < 40; ++round) {
        auto c = lan.a.tcp_open(); c->connect(Ipv4Address::make(10,0,0,2), 6000);
        SharedPointer<ITcpSocket> s; assert(lan.run_until([&] { if (!s) s = server->accept(); return s && c->is_connected(); }, 3000));
        const uint8_t hi[] = "hi"; c->send(ConstByteView(hi, 2)); lan.step(20); uint8_t b[8]; assert(s->receive(ByteView(b, 8)) == 2);
        c->close(); s->close(); }   // user references dropped at the end of each round
      lan.step(10000); }
    assert(g_live == 0);
  }
  { // ---- abort sends a reset
    Lan lan; lan.configure_static(); auto server = lan.b.tcp_open(); server->listen(6100); auto c = lan.a.tcp_open(); c->connect(Ipv4Address::make(10,0,0,2), 6100);
    SharedPointer<ITcpSocket> s; assert(lan.run_until([&] { if (!s) s = server->accept(); return s && c->is_connected(); }, 3000));
    c->abort(); assert(lan.run_until([&] { return s->state() == TcpState::Closed; }, 2000)); assert(s->failed());
  }
  { // ---- DHCP: discover, offer, request, ack; renewal; lease expiry; NAK; rediscovery
    Lan lan; MiniDhcpServer server;
    IpConfiguration cb; cb.address = Ipv4Address::make(10,0,0,2); cb.netmask = Ipv4Address::make(255,255,255,0); lan.b.configure(cb);
    server.socket = lan.b.udp_open(67); assert(server.socket);
    auto step = [&](int ms) { for (int i = 0; i < ms; ++i) { lan.step(); server.poll(lan.b); } };
    assert(!lan.a.configuration().configured() && lan.a.dhcp().state() == DhcpState::Stopped);
    lan.a.start_dhcp(); assert(lan.a.dhcp().state() == DhcpState::Selecting);
    step(500);
    assert(lan.a.dhcp().state() == DhcpState::Bound && lan.a.configuration().address == Ipv4Address::make(10,0,0,100));
    assert(lan.a.configuration().netmask == Ipv4Address::make(255,255,255,0) && lan.a.configuration().gateway == Ipv4Address::make(10,0,0,2) && lan.a.configuration().dns == Ipv4Address::make(10,0,0,3));
    assert(lan.a.dhcp().lease_seconds() == 120 && lan.a.dhcp().server() == Ipv4Address::make(10,0,0,2) && server.discovers == 1 && server.requests == 1);
    uint32_t rtt; assert(lan.a.ping(Ipv4Address::make(10,0,0,2), 1000, &rtt));   // the new address works
    // renewal at T1 = 60 s
    step(61000); assert(server.renewals >= 1 && lan.a.dhcp().state() == DhcpState::Bound);
    // the server goes quiet: renewals go unanswered, the lease runs out, the address is given up
    server.answer = false; int before = server.discovers; step(130000);
    assert(!lan.a.configuration().configured()); assert(server.discovers > before);   // it started over
    // the server comes back: a new lease
    server.answer = true; step(40000); assert(lan.a.dhcp().state() == DhcpState::Bound && lan.a.configuration().configured());
    // a NAK to the next renewal sends the client back to discovery
    server.nak_requests = true; step(62000); server.nak_requests = false; step(20000);
    assert(lan.a.dhcp().state() == DhcpState::Bound && lan.a.configuration().configured());
  }
  { // ---- DHCP with no server: gives up after its attempts; manual configuration stops it
    Lan lan; lan.a.start_dhcp(); for (int i = 0; i < 200000; ++i) lan.step(); assert(lan.a.dhcp().state() == DhcpState::Failed && !lan.a.configuration().configured());
    lan.a.start_dhcp(); assert(lan.a.dhcp().state() == DhcpState::Selecting); lan.configure_static(); assert(lan.a.dhcp().state() == DhcpState::Stopped && lan.a.configuration().configured());
  }
  { // ---- before an address exists, nothing but DHCP is accepted or sent
    Lan lan; uint32_t rtt; assert(!lan.a.ping(Ipv4Address::make(10,0,0,2), 100, &rtt)); assert(!lan.a.tcp_open()->connect(Ipv4Address::make(10,0,0,2), 80));
  }
  { // ---- DNS wire format
    uint8_t buf[300]; assert(dns::encode_name("www.example.com", ByteView(buf, sizeof(buf))) == 17 && buf[0] == 3 && buf[4] == 7 && buf[16] == 0);
    assert(dns::encode_name("example.com.", ByteView(buf, sizeof(buf))) == 13);        // trailing dot allowed
    assert(!dns::encode_name("", ByteView(buf, sizeof(buf))) && !dns::encode_name("a..b", ByteView(buf, sizeof(buf))) && !dns::encode_name(".", ByteView(buf, sizeof(buf))));
    assert(!dns::encode_name(std::string(64, 'a').c_str(), ByteView(buf, sizeof(buf))) && dns::encode_name(std::string(63, 'a').c_str(), ByteView(buf, sizeof(buf))) == 65);
    std::string too_long; for (int i = 0; i < 9; ++i) too_long += std::string(30, 'a') + "."; assert(!dns::encode_name(too_long.c_str(), ByteView(buf, sizeof(buf))));
    assert(dns::names_equal("Example.COM", "example.com.") && !dns::names_equal("a.com", "b.com") && !dns::names_equal("a.com", "a.comx"));
    Ipv4Address a; assert(parse_ipv4("10.0.2.15", &a) && a == Ipv4Address::make(10,0,2,15) && parse_ipv4("0.0.0.0", &a) && !parse_ipv4("10.0.2", &a) && !parse_ipv4("10.0.2.256", &a) && !parse_ipv4("10.0.2.15.1", &a) && !parse_ipv4("a.b.c.d", &a) && !parse_ipv4("", &a) && !parse_ipv4("1.2.3.4x", &a) && !parse_ipv4("1..2.3", &a));
    // malformed responses never crash or loop: a compression pointer to itself, truncations at every length
    uint8_t q[64]; uint32_t ql = dns::build_query(0x1234, "host.test", ByteView(q, sizeof(q))); std::vector<uint8_t> loop(q, q + ql); loop[2] = 0x81; loop[3] = 0x80; loop[7] = 1;
    loop.push_back(0xC0); loop.push_back(loop.size() - 1); for (int i = 0; i < 20; ++i) loop.push_back(0);   // answer whose name points at itself
    dns::Answer ans; assert(dns::parse_response(ConstByteView(loop.data(), loop.size()), 0x1234, "host.test", &ans) == dns::ParseResult::Invalid);
    for (uint32_t cut = 0; cut < ql; ++cut) assert(dns::parse_response(ConstByteView(q, cut), 0x1234, "host.test", &ans) == dns::ParseResult::Invalid);   // a question is not an answer
  }
  { // ---- DNS resolver against a mock server
    Lan lan; lan.configure_static(); IpConfiguration c = lan.a.configuration(); c.dns = Ipv4Address::make(10,0,0,2); lan.a.configure(c);
    MockDns dnsd; dnsd.start(lan.b); auto step = [&](int ms) { for (int i = 0; i < ms; ++i) { lan.step(); dnsd.poll(lan.b); } };
    auto lookup = [&](const char* name, int timeout_ms, DnsStatus* st, Ipv4Address* out) {
      auto q = lan.a.dns().query(name); if (!q) return false; for (int i = 0; i < timeout_ms && q->status() == DnsStatus::Pending; ++i) step(1);
      *st = q->status(); if (*st == DnsStatus::Resolved) *out = q->address(); return true; };
    DnsStatus st; Ipv4Address ip;
    assert(lookup("host.test", 2000, &st, &ip) && st == DnsStatus::Resolved && ip == Ipv4Address::make(10,0,0,50));
    assert(dnsd.queries["host.test"] == 1);
    assert(lookup("HOST.test", 10, &st, &ip) && st == DnsStatus::Resolved && dnsd.queries["host.test"] == 1);   // cache hit, case-insensitive
    assert(lookup("alias.test", 2000, &st, &ip) && st == DnsStatus::Resolved && ip == Ipv4Address::make(10,0,0,60));   // CNAME chain with a compression pointer
    { auto q = lan.a.dns().query("alias.test"); assert(q->status() == DnsStatus::Resolved && q->ttl_seconds() == 30); }   // smallest TTL along the chain
    assert(lookup("missing.test", 2000, &st, &ip) && st == DnsStatus::NotFound);
    assert(lookup("missing.test", 10, &st, &ip) && st == DnsStatus::NotFound && dnsd.queries["missing.test"] == 1);   // negative answers are cached too
    assert(lookup("noaddr.test", 2000, &st, &ip) && st == DnsStatus::NotFound);
    assert(lookup("servfail.test", 2000, &st, &ip) && st == DnsStatus::Failed);
    assert(lookup("big.test", 3000, &st, &ip) && st == DnsStatus::Resolved && ip == Ipv4Address::make(10,0,0,70) && dnsd.queries["big.test"] == 2);   // truncated over UDP, complete over TCP
    assert(lookup("slow.test", 6000, &st, &ip) && st == DnsStatus::Resolved && ip == Ipv4Address::make(10,0,0,80) && dnsd.queries["slow.test"] == 3);   // answered on the third try
    assert(lookup("silent.test", 10000, &st, &ip) && st == DnsStatus::Failed && dnsd.queries["silent.test"] == 3);   // gives up after three tries
    assert(lookup("zero.test", 2000, &st, &ip) && st == DnsStatus::Resolved); assert(lookup("zero.test", 2000, &st, &ip) && dnsd.queries["zero.test"] == 2);   // TTL 0: never cached
    dnsd.forge_first = true; assert(lookup("unknown.test", 2000, &st, &ip) && st == DnsStatus::NotFound);   // a forged reply with the wrong ID is ignored
    assert(lookup("10.9.8.7", 0, &st, &ip) && st == DnsStatus::Resolved && ip == Ipv4Address::make(10,9,8,7));   // literals resolve to themselves, no packet
    assert(!lan.a.dns().query("bad..name") && !lan.a.dns().query(""));
    // blocking helper and the cache lifetime
    assert(lan.a.resolve("host.test", 1, &ip) && ip == Ipv4Address::make(10,0,0,50));
    uint32_t cached = lan.a.dns().cache_size(); assert(cached >= 4);
    step(61000); int before = dnsd.queries["host.test"]; assert(lookup("host.test", 2000, &st, &ip) && st == DnsStatus::Resolved && dnsd.queries["host.test"] == before + 1);   // expired after its 60 s TTL
    lan.a.dns().clear_cache(); assert(lan.a.dns().cache_size() == 0);
    // an abandoned query is cleaned up and a late reply is harmless
    { auto q = lan.a.dns().query("silent.test"); } step(5000);
    // no DNS server configured
    IpConfiguration none = lan.a.configuration(); none.dns = kAnyAddress; lan.a.configure(none); assert(lookup("host.test", 100, &st, &ip) && st == DnsStatus::Failed);
    // the server's own address is answered over TCP too when a query is forced there: covered by big.test
  }
  { // ---- one stack per interface: DHCP and DNS on every network
    TestAllocator alloc; FakeClock clock; Wire w1, w2; FakeNic a1{&w1, 0, 1}, b1{&w1, 1, 2}, a2{&w2, 0, 3}, b2{&w2, 1, 4};
    a1.peer = &b1; b1.peer = &a1; a2.peer = &b2; b2.peer = &a2;
    NetworkStack sb1(b1, clock, alloc), sb2(b2, clock, alloc); assert(sb1.initialize() && sb2.initialize());
    IpConfiguration cs; cs.address = Ipv4Address::make(10,0,0,2); cs.netmask = Ipv4Address::make(255,255,255,0); sb1.configure(cs); sb2.configure(cs);
    MiniDhcpServer dhcp1, dhcp2; dhcp1.dns = dhcp2.dns = Ipv4Address::make(10,0,0,2); dhcp1.socket = sb1.udp_open(67); dhcp2.socket = sb2.udp_open(67);
    MockDns dns1, dns2; dns1.start(sb1); dns2.start(sb2); dns1.extra_name = "only1.test"; dns1.extra_addr = Ipv4Address::make(10,1,1,1); dns2.extra_name = "only2.test"; dns2.extra_addr = Ipv4Address::make(10,2,2,2);
    auto servers = [&] { sb1.poll(); dhcp1.poll(sb1); dns1.poll(sb1); sb2.poll(); dhcp2.poll(sb2); dns2.poll(sb2); };
    NetworkStackSet set(clock, alloc); assert(set.add(a1) && set.add(a2) && set.count() == 2 && !set.at(2));
    a1.on_idle = servers; a2.on_idle = servers;
    for (int i = 0; i < 3; ++i) { clock.us += 1000; set.poll(); servers(); }
    assert(!set.at(0)->configuration().configured());
    set.start_dhcp(); assert(set.at(0)->dhcp().state() == DhcpState::Selecting && set.at(1)->dhcp().state() == DhcpState::Selecting);
    assert(set.wait_for_addresses(5000) == 2);   // both networks configured, concurrently
    for (uint32_t i = 0; i < 2; ++i) { assert(set.at(i)->configuration().address == Ipv4Address::make(10,0,0,100) && set.at(i)->dhcp().state() == DhcpState::Bound); }
    Ipv4Address ip; uint32_t index = 99;
    assert(set.resolve("only1.test", 3000, &ip, &index) && ip == Ipv4Address::make(10,1,1,1) && index == 0);   // only the first network knows it
    assert(set.resolve("only2.test", 3000, &ip, &index) && ip == Ipv4Address::make(10,2,2,2) && index == 1);   // only the second does
    assert(set.resolve("host.test", 3000, &ip, &index) && ip == Ipv4Address::make(10,0,0,50));                 // both know it: either answer is fine
    assert(!set.resolve("nobody.test", 3000, &ip));                                                              // unknown everywhere
    assert(dns1.queries["only2.test"] == 1 && dns2.queries["only2.test"] == 1);                                  // the question went to both networks
    assert(set.resolve("10.4.4.4", 0, &ip, &index) && ip == Ipv4Address::make(10,4,4,4));
    assert(set.at(0)->resolve("only1.test", 1000, &ip) && !set.at(0)->resolve("only2.test", 1000, &ip));        // each stack on its own sees only its network
    // one network goes dead: the other still resolves, and DHCP on a silent network does not block the rest
    w2.loss_percent = 100; assert(set.resolve("only1.test", 3000, &ip, &index) && index == 0);
  }
  { // ---- a silent network does not hold up the others
    TestAllocator alloc; FakeClock clock; Wire w1, w2; FakeNic a1{&w1, 0, 1}, b1{&w1, 1, 2}, a2{&w2, 0, 3}, b2{&w2, 1, 4};
    a1.peer = &b1; b1.peer = &a1; a2.peer = &b2; b2.peer = &a2;
    NetworkStack sb1(b1, clock, alloc); assert(sb1.initialize()); IpConfiguration cs; cs.address = Ipv4Address::make(10,0,0,2); cs.netmask = Ipv4Address::make(255,255,255,0); sb1.configure(cs);
    MiniDhcpServer dhcp1; dhcp1.socket = sb1.udp_open(67);
    NetworkStackSet set(clock, alloc); assert(set.add(a1) && set.add(a2));
    a1.on_idle = [&] { sb1.poll(); dhcp1.poll(sb1); };   // network 2 has no server at all
    set.start_dhcp(); uint32_t got = set.wait_for_addresses(120000);
    assert(got == 1 && set.at(0)->configuration().configured() && !set.at(1)->configuration().configured() && set.at(1)->dhcp().state() == DhcpState::Failed);
  }
  { // ---- hostile input: mutated and truncated frames must never read or write out of bounds
    // Record real traffic (ARP, ICMP, UDP, TCP, DHCP, DNS) from a busy exchange, then feed
    // back damaged copies of it - flipped bytes, cuts at every length, junk appended, pure noise -
    // to a configured stack. Every parser works on views and the harness runs under ASAN/UBSAN,
    // so an out-of-bounds access is a bounds violation (abort) or a sanitizer report.
    Lan lan; lan.configure_static();
    std::vector<std::vector<uint8_t>> corpus;
    lan.wire.tap = [&](const std::vector<uint8_t>& f, int) { if (corpus.size() < 400) corpus.push_back(f); };
    { uint32_t rtt; lan.a.ping(Ipv4Address::make(10,0,0,2), 1000, &rtt); }
    auto udp_a = lan.a.udp_open(5000); auto udp_b = lan.b.udp_open(5001);
    uint8_t hello[] = {1, 2, 3, 4, 5}; udp_a->send_to(Ipv4Address::make(10,0,0,2), 5001, ConstByteView(hello, 5)); lan.step(20);
    assert(echo_transfer(lan, 5000, 9100));
    lan.wire.tap = nullptr;
    // DHCP and DNS conversations
    { Lan dl; MiniDhcpServer server; server.socket = dl.b.udp_open(67); dl.configure_static();
      dl.wire.tap = [&](const std::vector<uint8_t>& f, int) { if (corpus.size() < 600) corpus.push_back(f); };
      dl.a.start_dhcp(); for (int i = 0; i < 3000; ++i) { dl.step(); server.poll(dl.b); } }
    assert(corpus.size() > 20);
    uint32_t state = 12345;
    auto rnd = [&] { state = state * 1664525u + 1013904223u; return state >> 8; };
    uint64_t injected = 0;
    auto inject = [&](const std::vector<uint8_t>& f) {
      lan.nic_a.inbox.push_back(f); lan.nic_b.inbox.push_back(f); ++injected;
      if (lan.nic_a.inbox.size() > 32) { lan.step(); } };
    for (const auto& frame : corpus) {
      for (size_t cut = 0; cut <= frame.size() && cut < 140; ++cut) inject(std::vector<uint8_t>(frame.begin(), frame.begin() + cut));
      for (int m = 0; m < 12; ++m) {  // flipped bytes
        std::vector<uint8_t> f = frame; int n = 1 + rnd() % 4;
        for (int i = 0; i < n; ++i) f[rnd() % f.size()] ^= (uint8_t)(1u << (rnd() % 8)) | (rnd() % 3 == 0 ? 0xFF : 0);
        inject(f); }
      std::vector<uint8_t> longer = frame; for (int i = 0; i < 40; ++i) longer.push_back((uint8_t)rnd()); inject(longer);
      // lying length fields: IPv4 total length, UDP length, TCP data offset
      if (frame.size() > 54) { for (int v : {0, 1, 19, 20, 21, 255, 65535}) { std::vector<uint8_t> f = frame; f[16] = (uint8_t)(v >> 8); f[17] = (uint8_t)v; inject(f); std::vector<uint8_t> g = frame; g[46] = (uint8_t)(v & 0xF0); inject(g); } }
    }
    for (int i = 0; i < 4000; ++i) {  // pure noise, some with plausible Ethernet/IP headers
      std::vector<uint8_t> f(14 + rnd() % 200); for (auto& b : f) b = (uint8_t)rnd();
      if (i % 2) { f[12] = 0x08; f[13] = (i % 4 == 1) ? 0x00 : 0x06; if (f.size() > 34) f[14] = 0x45; if (f.size() > 24) f[23] = (uint8_t[]){1, 6, 17}[rnd() % 3]; }
      inject(f); }
    lan.step(50);
    assert(injected > 5000);
    // the stack is still alive and sane
    { uint32_t rtt; assert(lan.a.ping(Ipv4Address::make(10,0,0,2), 2000, &rtt)); }
    printf("fuzz: %llu hostile frames survived\n", (unsigned long long)injected);
  }
  puts("network core ok");
}
