// Host tests of the stack's IPv6: address text, stateless autoconfiguration with duplicate address detection, neighbor discovery, router
// advertisements (prefix, router, MTU, RDNSS), ICMPv6 echo, UDP and TCP over IPv6 between two stacks on the fake wire, the rules that
// protect neighbor discovery (hop limit 255, checksums), AAAA in DNS messages, and hostile IPv6 frames.
#include "Network/Checksum.hpp"
#include "Network/Dns/DnsMessage.hpp"
#include "Network/NetworkStack.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace network;

static uint16_t load16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void store16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

#include "NetworkHarness.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "Ipv6Harness.hpp"

static void step_until(Lan& lan, const std::function<bool()>& done, int max_ms) { lan.run_until(done, max_ms); }

static void test_text() {
  struct { const char* in; const char* out; } good[] = {
      {"::", "::"}, {"::1", "::1"}, {"fe80::1", "fe80::1"}, {"2001:db8::8:800:200c:417a", "2001:db8::8:800:200c:417a"},
      {"2001:0DB8:0000:0000:0008:0800:200C:417A", "2001:db8::8:800:200c:417a"}, {"ff02::1:ff00:1", "ff02::1:ff00:1"},
      {"2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"},   // the first of two equal runs
      {"2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"}, // one zero group stays
      {"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"}, {"::ffff:10.0.2.15", "::ffff:a00:20f"}, {"1::", "1::"}, {"1:2:3:4:5:6:7::", "1:2:3:4:5:6:7:0"},
  };
  for (const auto& g : good) {
    Ipv6Address a;
    CHECK(parse_ipv6(StringView(g.in), &a));
    CHECK(text(a) == g.out);
    Ipv6Address again;
    CHECK(parse_ipv6(StringView(text(a).c_str()), &again) && again == a);
  }
  const char* bad[] = {"", ":", ":::", "1:2:3:4:5:6:7:8:9", "1::2::3", "12345::", "g::1", "1:2:3:4:5:6:7", "::1:", "1.2.3.4", "fe80::1%eth0",
                       "[::1]", "::ffff:1.2.3", "::ffff:1.2.3.4.5", "1:2:3:4:5:6:7:1.2.3.4", " ::1", "::1 "};
  for (const char* b : bad) CHECK(!parse_ipv6(StringView(b), nullptr));
  // random text: never accepted wrongly into something that formats differently, never out of bounds
  std::mt19937 rng(3);
  const char alphabet[] = "0123456789abcdefABCDEF:.%g ";
  uint32_t accepted = 0;
  for (int i = 0; i < 200000; ++i) {
    char buffer[50];
    const size_t n = rng() % 46;
    for (size_t j = 0; j < n; ++j) buffer[j] = alphabet[rng() % (sizeof(alphabet) - 1)];
    buffer[n] = 0;
    Ipv6Address a;
    if (parse_ipv6(StringView(buffer, n), &a)) {
      ++accepted;
      Ipv6Address again;
      CHECK(parse_ipv6(StringView(text(a).c_str()), &again) && again == a);
    }
  }
  CHECK(accepted > 0);
  char small[39];
  CHECK(format_ipv6(v6("::1"), ArrayView<char>(small)) == 0);  // (needs 40)
  // address helpers
  CHECK(v6("fe80::1").is_link_local() && !v6("fec0::1").is_link_local() && v6("ff02::1").is_multicast() && v6("::1").is_loopback());
  CHECK(v6("2001:db8::1234:5678").solicited_node() == v6("ff02::1:ff34:5678"));
  const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  CHECK(Ipv6Address::from_prefix_and_mac(kIpv6LinkLocalPrefix, mac) == v6("fe80::5054:ff:fe12:3456"));
  CHECK(v6("2001:db8:1:2::5").in_prefix(v6("2001:db8:1:2::"), 64) && !v6("2001:db8:1:3::5").in_prefix(v6("2001:db8:1:2::"), 64));
  CHECK(v6("2001:db8::1").in_prefix(v6("2001:db9::"), 31) && !v6("2001:db8::1").in_prefix(v6("2001:db9::"), 32));
}

// Both stacks give themselves link-local addresses (after a second of duplicate address detection), find each other with neighbor
// solicitations, and talk ICMPv6, UDP and TCP over them.
static void test_link_local() {
  Lan lan;
  lan.configure_static();
  uint64_t bad_checksums = 0, v6_frames = 0;
  lan.wire.tap = [&](const std::vector<uint8_t>& f, int) {
    if (f.size() < 54 || f[12] != 0x86 || f[13] != 0xDD) return;
    ++v6_frames;
    uint32_t next = f[20], offset = 54;
    if (next == 0) { next = f[54]; offset += (f[55] + 1) * 8; }
    const uint32_t length = load16(&f[18]) - (offset - 54);
    if (next != 6 && next != 17 && next != 58) return;
    uint32_t sum = next + length;
    for (int i = 22; i < 54; i += 2) sum += (f[i] << 8) | f[i + 1];
    for (uint32_t i = 0; i < length; i += 2) sum += (f[offset + i] << 8) | (i + 1 < length ? f[offset + i + 1] : 0);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    if (sum != 0xFFFF) ++bad_checksums;
  };
  CHECK(!lan.a.configuration6().link_local_ready);
  lan.step(500);
  CHECK(!lan.a.configuration6().link_local_ready);  // still tentative
  step_until(lan, [&] { return lan.a.configuration6().link_local_ready && lan.b.configuration6().link_local_ready; }, 3000);
  const Ipv6Configuration ca = lan.a.configuration6(), cb = lan.b.configuration6();
  CHECK(ca.link_local_ready && cb.link_local_ready && ca.link_local == v6("fe80::ff:fe00:1") && cb.link_local == v6("fe80::ff:fe00:2"));
  CHECK(!ca.global_ready && ca.router.is_unspecified());
  uint32_t rtt = 99;
  CHECK(lan.a.ping6(cb.link_local, 2000, &rtt));
  CHECK(lan.b.ping6(ca.link_local, 2000, &rtt));   // (b learned a from its solicitation)
  CHECK(lan.a.ping6(kIpv6Loopback, 500, &rtt));    // ::1 never leaves the stack
  CHECK(lan.a.ping6(ca.link_local, 500, &rtt));    // nor does its own address
  CHECK(!lan.a.ping6(v6("fe80::99"), 4000, &rtt)); // nobody: neighbor discovery gives up
  CHECK(!lan.a.ping6(v6("2001:db8::1"), 500, &rtt));  // off-link with no router: no way there

  // UDP both ways
  auto server = lan.b.udp_open(7000);
  auto client = lan.a.udp_open(0);
  const uint8_t msg[] = "over IPv6";
  CHECK(client->send_to_ip(IpAddress(cb.link_local), 7000, ConstByteView(msg, sizeof msg)));
  CHECK(lan.run_until([&] { return server->pending() > 0; }, 1000));
  uint8_t buf[64];
  IpAddress from;
  uint16_t port = 0;
  uint32_t len = 0;
  CHECK(server->receive_from_ip(ByteView(buf, sizeof buf), &from, &port, &len) && len == sizeof msg && from == IpAddress(ca.link_local) &&
        memcmp(buf, msg, len) == 0);
  CHECK(server->send_to_ip(from, port, ConstByteView(buf, len)) && lan.run_until([&] { return client->pending() > 0; }, 1000));
  Ipv4Address from4 = Ipv4Address::make(1, 2, 3, 4);
  CHECK(client->receive_from(ByteView(buf, sizeof buf), &from4, &port, &len) && from4.is_unspecified());  // (the IPv4 call: no IPv4 sender)
  Bytes big(1453, 7);
  CHECK(!client->send_to_ip(IpAddress(cb.link_local), 7000, ConstByteView(big.data(), big.size())));      // over the IPv6 limit
  CHECK(client->send_to_ip(IpAddress(cb.link_local), 7000, ConstByteView(big.data(), 1452)));

  // TCP: 100 KB echoed over IPv6
  auto listener = lan.b.tcp_open();
  CHECK(listener->listen(8000));
  auto c = lan.a.tcp_open();
  CHECK(c->connect_ip(IpAddress(cb.link_local), 8000));
  SharedPointer<ITcpSocket> s;
  CHECK(lan.run_until([&] { if (!s) s = listener->accept(); return s && c->state() == TcpState::Established; }, 3000));
  CHECK(s && s->remote_ip() == IpAddress(ca.link_local) && s->remote_address().is_unspecified() && c->remote_ip() == IpAddress(cb.link_local));
  std::vector<uint8_t> data(100000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i * 13 + (i >> 8));
  size_t sent = 0, echoed = 0, got = 0;
  uint8_t rb[4096], echo[4096];
  uint32_t echo_have = 0, echo_sent = 0;
  bool same = true;
  CHECK(lan.run_until([&] {
    if (sent < data.size()) sent += c->send(ConstByteView(data.data() + sent, data.size() - sent));
    if (echo_sent == echo_have) { echo_have = s->receive(ByteView(echo)); echo_sent = 0; echoed += echo_have; }
    if (echo_sent < echo_have) echo_sent += s->send(ConstByteView(echo + echo_sent, echo_have - echo_sent));
    for (uint32_t n; (n = c->receive(ByteView(rb))) > 0; got += n) same = same && memcmp(rb, data.data() + got, n) == 0;
    return got == data.size(); }, 60000));
  CHECK(same && echoed == data.size());
  c->close();
  lan.step(100);
  CHECK(v6_frames > 50 && bad_checksums == 0);
  // the IPv4 side is untouched
  CHECK(lan.a.ping(Ipv4Address::make(10, 0, 0, 2), 2000, &rtt));
  lan.wire.tap = nullptr;
}

// A router advertises 2001:db8:1::/64: both stacks make a global address from it (again after duplicate address detection), take the
// router, the DNS server and the MTU, and reach each other's global address on the link; off-link packets go to the router's MAC.
static void test_router_advertisement() {
  Lan lan;
  lan.configure_static();
  step_until(lan, [&] { return lan.a.configuration6().link_local_ready && lan.b.configuration6().link_local_ready; }, 3000);
  const Ipv6Address prefix = v6("2001:db8:1::");
  // what does not count: a hop limit other than 255, a prefix without A, one longer than /64
  lan.nic_a.inbox.push_back(router_advertisement(prefix, 1800, 86400, 64));
  lan.nic_a.inbox.push_back(router_advertisement(v6("2001:db8:7::"), 1800, 86400, 255, false));
  lan.nic_a.inbox.push_back(router_advertisement(v6("2001:db8:8::"), 0, 86400, 255, true, 80));
  lan.step(1500);
  CHECK(!lan.a.configuration6().global_ready && lan.a.configuration6().router.is_unspecified());
  lan.nic_a.inbox.push_back(router_advertisement(prefix, 1800));
  lan.nic_b.inbox.push_back(router_advertisement(prefix, 1800));
  lan.step(2);
  CHECK(!lan.a.configuration6().global_ready && lan.a.configuration6().router == v6("fe80::1"));  // the router at once, the address after DAD
  step_until(lan, [&] { return lan.a.configuration6().global_ready && lan.b.configuration6().global_ready; }, 3000);
  const Ipv6Configuration ca = lan.a.configuration6(), cb = lan.b.configuration6();
  CHECK(ca.global == v6("2001:db8:1::ff:fe00:1") && cb.global == v6("2001:db8:1::ff:fe00:2") && ca.prefix == prefix && ca.prefix_length == 64);
  CHECK(ca.dns == v6("2001:db8:1::53") && ca.mtu == 1500);
  uint32_t rtt;
  CHECK(lan.a.ping6(cb.global, 2000, &rtt));  // on-link (the L flag)
  // off-link: the packet goes to the router's Ethernet address (learned from its source link-layer option); nobody answers
  std::vector<uint8_t> last;
  lan.wire.tap = [&](const std::vector<uint8_t>& f, int from) { if (from == 0 && f.size() > 54 && f[12] == 0x86 && f[54] == 128) last = f; };
  CHECK(!lan.a.ping6(v6("2001:db8:99::1"), 200, &rtt));
  CHECK(!last.empty() && memcmp(&last[0], kRouterMac, 6) == 0 && memcmp(&last[22], ca.global.bytes, 16) == 0);
  lan.wire.tap = nullptr;
  // TCP between global addresses, and the source is global for a global destination
  auto listener = lan.b.tcp_open();
  CHECK(listener->listen(8100));
  auto c = lan.a.tcp_open();
  CHECK(c->connect_ip(IpAddress(cb.global), 8100));
  SharedPointer<ITcpSocket> s;
  CHECK(lan.run_until([&] { if (!s) s = listener->accept(); return s && c->state() == TcpState::Established; }, 3000));
  CHECK(s && s->remote_ip() == IpAddress(ca.global));
  // the router leaves (lifetime 0): no default router any more
  lan.nic_a.inbox.push_back(router_advertisement(prefix, 0));
  lan.step(2);
  CHECK(lan.a.configuration6().router.is_unspecified() && lan.a.configuration6().global_ready);
  // a prefix that expires takes the address with it
  lan.nic_a.inbox.push_back(router_advertisement(prefix, 1800, 2));
  lan.step(2500);
  CHECK(!lan.a.configuration6().global_ready);
}

// Duplicate address detection: another node answering for our tentative address (or trying it too) means it is not ours.
static void test_duplicate_detection() {
  for (int kind = 0; kind < 2; ++kind) {
    Lan lan;
    const Ipv6Address mine = v6("fe80::ff:fe00:1");
    lan.step(1);  // (started: the address is tentative)
    Bytes m;
    if (kind == 0) {  // a neighbor advertisement for it
      m = {136, 0, 0, 0, 0x20, 0, 0, 0};
      m.insert(m.end(), mine.bytes, mine.bytes + 16);
      const uint8_t tlla[8] = {2, 1, 0x02, 0xBB, 0, 0, 0, 9};
      m.insert(m.end(), tlla, tlla + 8);
      lan.nic_a.inbox.push_back(ipv6_frame(kAllNodesMac, kRouterMac, v6("fe80::9"), kIpv6AllNodes, 58, 255, m));
    } else {  // someone else's duplicate address detection for it
      m = {135, 0, 0, 0, 0, 0, 0, 0};
      m.insert(m.end(), mine.bytes, mine.bytes + 16);
      const uint8_t solicited_mac[6] = {0x33, 0x33, 0xFF, 0x00, 0x00, 0x01};
      lan.nic_a.inbox.push_back(ipv6_frame(solicited_mac, kRouterMac, kIpv6Any, mine.solicited_node(), 58, 255, m));
    }
    lan.step(3000);
    CHECK(!lan.a.configuration6().link_local_ready);
    CHECK(lan.b.configuration6().link_local_ready);  // (the other stack is not concerned)
  }
  // a stack whose address is in use defends it: a DAD solicitation from someone else gets an advertisement to all nodes
  Lan lan;
  step_until(lan, [&] { return lan.a.configuration6().link_local_ready; }, 3000);
  const Ipv6Address mine = lan.a.configuration6().link_local;
  bool defended = false;
  lan.wire.tap = [&](const std::vector<uint8_t>& f, int from) {
    if (from == 0 && f.size() >= 78 && f[12] == 0x86 && f[54] == 136 && memcmp(&f[38], kIpv6AllNodes.bytes, 16) == 0 && memcmp(&f[62], mine.bytes, 16) == 0)
      defended = true;
  };
  Bytes m = {135, 0, 0, 0, 0, 0, 0, 0};
  m.insert(m.end(), mine.bytes, mine.bytes + 16);
  const uint8_t solicited_mac[6] = {0x33, 0x33, 0xFF, 0x00, 0x00, 0x01};
  lan.nic_a.inbox.push_back(ipv6_frame(solicited_mac, kRouterMac, kIpv6Any, mine.solicited_node(), 58, 255, m));
  lan.step(5);
  CHECK(defended && lan.a.configuration6().link_local_ready);
  lan.wire.tap = nullptr;
}

// Neighbor discovery protection: a solicitation that crossed a router (hop limit below 255) or has a wrong checksum is ignored; an MLD report
// with the router alert goes out for our solicited-node group.
static void test_rules() {
  Lan lan;
  bool mld = false, ns_answered = false;
  lan.wire.tap = [&](const std::vector<uint8_t>& f, int from) {
    if (from != 0 || f.size() < 62 || f[12] != 0x86) return;
    if (f[20] == 0 && f[21] == 1 && f[54] == 58 && f[56] == 5 && f[62] == 143 && memcmp(&f[38], kIpv6AllMldv2Routers.bytes, 16) == 0) mld = true;
    if (f[20] == 58 && f[54] == 136 && f[21] == 255) ns_answered = true;
  };
  step_until(lan, [&] { return lan.a.configuration6().link_local_ready; }, 3000);
  CHECK(mld);
  const Ipv6Address mine = lan.a.configuration6().link_local;
  const auto solicitation = [&](uint8_t hop_limit, bool break_checksum) {
    Bytes m = {135, 0, 0, 0, 0, 0, 0, 0};
    m.insert(m.end(), mine.bytes, mine.bytes + 16);
    const uint8_t slla[8] = {1, 1, kRouterMac[0], kRouterMac[1], kRouterMac[2], kRouterMac[3], kRouterMac[4], kRouterMac[5]};
    m.insert(m.end(), slla, slla + 8);
    const uint8_t solicited_mac[6] = {0x33, 0x33, 0xFF, 0x00, 0x00, 0x01};
    Bytes f = ipv6_frame(solicited_mac, kRouterMac, v6("fe80::1"), mine.solicited_node(), 58, hop_limit, m);
    if (break_checksum) f[57] ^= 1;
    return f;
  };
  lan.nic_a.inbox.push_back(solicitation(64, false));
  lan.nic_a.inbox.push_back(solicitation(255, true));
  lan.step(5);
  CHECK(!ns_answered);
  lan.nic_a.inbox.push_back(solicitation(255, false));
  lan.step(5);
  CHECK(ns_answered);
  // packets to someone else's unicast address, from a multicast source, or to ::1 from the wire are not taken
  auto server = lan.a.udp_open(9000);
  const auto udp_to = [&](const Ipv6Address& src, const Ipv6Address& dst) {
    Bytes u = {0x30, 0x39, 0x23, 0x28, 0, 12, 0, 0, 'h', 'i', '!', '!'};
    uint32_t sum = pseudo_header_sum(src, dst, 17, 12) + 0;
    sum = checksum_add(sum, ConstByteView(u.data(), u.size()));
    const uint16_t c = checksum_finish(sum);
    u[6] = (uint8_t)(c >> 8); u[7] = (uint8_t)c;
    const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};
    return ipv6_frame(mac, kRouterMac, src, dst, 17, 64, u);
  };
  lan.nic_a.inbox.push_back(udp_to(v6("fe80::1"), v6("fe80::77")));
  lan.nic_a.inbox.push_back(udp_to(v6("ff02::5"), mine));
  lan.nic_a.inbox.push_back(udp_to(v6("fe80::1"), kIpv6Loopback));
  lan.step(5);
  CHECK(server->pending() == 0);
  lan.nic_a.inbox.push_back(udp_to(v6("fe80::1"), mine));
  lan.step(5);
  CHECK(server->pending() == 1);
  // a zero UDP checksum is refused over IPv6
  Bytes zero = udp_to(v6("fe80::1"), mine);
  zero[60] = zero[61] = 0;
  lan.nic_a.inbox.push_back(zero);
  lan.step(5);
  CHECK(server->pending() == 1);
  // extension headers: a destination options header is skipped, a fragment header is not taken
  Bytes with_options = udp_to(v6("fe80::1"), mine);
  with_options[20] = 60;
  const uint8_t options[8] = {17, 0, 1, 4, 0, 0, 0, 0};
  with_options.insert(with_options.begin() + 54, options, options + 8);
  store16(&with_options[18], (uint16_t)(load16(&with_options[18]) + 8));
  lan.nic_a.inbox.push_back(with_options);
  lan.step(5);
  CHECK(server->pending() == 2);
  Bytes fragment = with_options;
  fragment[20] = 44;
  lan.nic_a.inbox.push_back(fragment);
  lan.step(5);
  CHECK(server->pending() == 2);
  lan.wire.tap = nullptr;
}

static void test_dns_aaaa() {
  uint8_t query[512];
  const uint32_t n = dns::build_query(0x1234, StringView("example.com"), ByteView(query), dns::kTypeAaaa);
  CHECK(n > 12 && query[n - 4] == 0 && query[n - 3] == 28);
  // an answer: the question, a CNAME, then the AAAA
  Bytes r(query, query + n);
  r[2] = 0x81; r[3] = 0x80;
  r[7] = 2;
  const uint8_t cname[] = {0xC0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 6, 3, 'w', 'w', 'w', 0xC0, 12};
  r.insert(r.end(), cname, cname + sizeof cname);
  const uint8_t aaaa_head[] = {3, 'w', 'w', 'w', 0xC0, 12, 0, 28, 0, 1, 0, 0, 0, 30, 0, 16};
  r.insert(r.end(), aaaa_head, aaaa_head + sizeof aaaa_head);
  const Ipv6Address expect = v6("2001:db8::abcd");
  r.insert(r.end(), expect.bytes, expect.bytes + 16);
  dns::Answer answer;
  CHECK(dns::parse_response(ConstByteView(r.data(), r.size()), 0x1234, StringView("example.com"), &answer, dns::kTypeAaaa) == dns::ParseResult::Answer);
  CHECK(answer.address == IpAddress(expect) && answer.ttl == 30);
  // asked for AAAA, the answer for an A question does not count; truncations and bit flips never read past the message
  CHECK(dns::parse_response(ConstByteView(r.data(), r.size()), 0x1234, StringView("example.com"), &answer, dns::kTypeA) == dns::ParseResult::Invalid);
  for (size_t cut = 0; cut < r.size(); ++cut) dns::parse_response(ConstByteView(r.data(), cut), 0x1234, StringView("example.com"), &answer, dns::kTypeAaaa);
  for (size_t bit = 0; bit < r.size() * 8; ++bit) {
    Bytes t = r;
    t[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    if (dns::parse_response(ConstByteView(t.data(), t.size()), 0x1234, StringView("example.com"), &answer, dns::kTypeAaaa) == dns::ParseResult::Answer)
      CHECK(answer.address.v6);
  }
  // the resolver: IPv6 text resolves to itself, for AAAA; an A query for it finds nothing
  Lan lan;
  IpAddress resolved;
  CHECK(lan.a.resolve_ip(StringView("fe80::5"), 100, &resolved, true) && resolved == IpAddress(v6("fe80::5")));
  auto q = lan.a.dns().query_aaaa(StringView("2001:db8::1"));
  CHECK(q && q->status() == DnsStatus::Resolved && q->ip_address() == IpAddress(v6("2001:db8::1")) && q->address().is_unspecified());
  auto q4 = lan.a.dns().query(StringView("2001:db8::1"));
  CHECK(!q4 || q4->status() == DnsStatus::NotFound);
}

// Hostile IPv6 frames: every message type, cut short and with bits flipped, never take the stack past a buffer.
static void test_hostile() {
  Lan lan;
  lan.configure_static();
  step_until(lan, [&] { return lan.a.configuration6().link_local_ready; }, 3000);
  const Ipv6Address mine = lan.a.configuration6().link_local;
  lan.nic_a.inbox.push_back(router_advertisement(v6("2001:db8:1::"), 1800));
  lan.step(1500);
  std::vector<Bytes> seeds;
  seeds.push_back(router_advertisement(v6("2001:db8:2::"), 1800));
  Bytes ns = {135, 0, 0, 0, 0, 0, 0, 0};
  ns.insert(ns.end(), mine.bytes, mine.bytes + 16);
  const uint8_t opt[8] = {1, 1, 2, 3, 4, 5, 6, 7};
  ns.insert(ns.end(), opt, opt + 8);
  seeds.push_back(ipv6_frame(kAllNodesMac, kRouterMac, v6("fe80::1"), mine.solicited_node(), 58, 255, ns));
  Bytes na = {136, 0, 0, 0, 0x60, 0, 0, 0};
  na.insert(na.end(), mine.bytes, mine.bytes + 16);
  na.insert(na.end(), opt, opt + 8);
  na[24] = 2;
  seeds.push_back(ipv6_frame(kAllNodesMac, kRouterMac, v6("fe80::1"), kIpv6AllNodes, 58, 255, na));
  Bytes echo = {128, 0, 0, 0, 0, 1, 0, 1, 'x', 'y', 'z'};
  seeds.push_back(ipv6_frame(kAllNodesMac, kRouterMac, v6("fe80::1"), mine, 58, 64, echo));
  Bytes udp = {0x30, 0x39, 0x00, 0x35, 0, 12, 0, 0, 'a', 'b', 'c', 'd'};
  seeds.push_back(ipv6_frame(kAllNodesMac, kRouterMac, v6("fe80::1"), mine, 17, 64, udp));
  Bytes tcp(20, 0);
  tcp[12] = 0x50; tcp[13] = 0x02; tcp[3] = 80;
  seeds.push_back(ipv6_frame(kAllNodesMac, kRouterMac, v6("fe80::1"), mine, 6, 64, tcp));
  Bytes hbh = {58, 0, 1, 4, 0, 0, 0, 0, 128, 0, 0, 0, 0, 2, 0, 2};
  seeds.push_back(ipv6_frame(kAllNodesMac, kRouterMac, v6("fe80::1"), mine, 0, 64, hbh));
  std::mt19937 rng(9);
  uint64_t injected = 0;
  for (int round = 0; round < 6000; ++round) {
    Bytes f = seeds[rng() % seeds.size()];
    const int kind = rng() % 3;
    if (kind == 0) f.resize(rng() % (f.size() + 1));
    else for (int k = 0; k < 1 + (int)(rng() % 4); ++k) f[rng() % f.size()] ^= (uint8_t)(1u << (rng() % 8));
    if (kind == 2 && f.size() > 22) store16(&f[18], (uint16_t)rng());  // a payload length that lies
    lan.nic_a.inbox.push_back(f);
    ++injected;
    if (round % 16 == 0) lan.step(1);
  }
  lan.step(10);
  uint32_t rtt;
  CHECK(lan.a.ping(Ipv4Address::make(10, 0, 0, 2), 2000, &rtt));  // still working
  printf("ipv6 fuzz: %llu hostile frames survived\n", (unsigned long long)injected);
}

// Offloads over IPv6: with checksum offload and segmentation on both fake interfaces, TCP sends large IPv6 segments the interface cuts and
// the receiver coalesces runs; every frame on the wire is a right-sized one with right checksums, and the data arrives whole.
static void test_offloads() {
  for (int mode = 0; mode < 3; ++mode) {  // 0: none, 1: both, 2: both, with loss and reordering
    Lan lan;
    lan.configure_static();
    lan.nic_a.offload = lan.nic_b.offload = mode >= 1;
    lan.nic_a.tso = lan.nic_b.tso = mode >= 1;
    if (mode == 2) { lan.wire.loss_percent = 3; lan.wire.reorder_percent = 5; }
    step_until(lan, [&] { return lan.a.configuration6().link_local_ready && lan.b.configuration6().link_local_ready; }, 3000);
    uint64_t bad = 0, big = 0, v6_frames = 0;
    lan.wire.tap = [&](const std::vector<uint8_t>& f, int) {
      if (f.size() > hardware::kEthernetMaxFrame + 0u) ++big;
      if (f.size() < 54 || f[12] != 0x86 || f[13] != 0xDD) return;
      ++v6_frames;
      uint32_t next = f[20], offset = 54;
      if (next == 0) { next = f[54]; offset += (f[55] + 1) * 8; }
      const uint32_t length = load16(&f[18]) - (offset - 54);
      if (next != 6 && next != 17 && next != 58) return;
      uint32_t sum = next + length;
      for (int i = 22; i < 54; i += 2) sum += (f[i] << 8) | f[i + 1];
      for (uint32_t i = 0; i < length; i += 2) sum += (f[offset + i] << 8) | (i + 1 < length ? f[offset + i + 1] : 0);
      while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
      if (sum != 0xFFFF) ++bad;
    };
    const Ipv6Address b = lan.b.configuration6().link_local;
    auto listener = lan.b.tcp_open();
    CHECK(listener->listen(8200));
    auto c = lan.a.tcp_open();
    CHECK(c->connect_ip(IpAddress(b), 8200));
    SharedPointer<ITcpSocket> s;
    CHECK(lan.run_until([&] { if (!s) s = listener->accept(); return s && c->state() == TcpState::Established; }, 5000));
    if (!s) continue;
    std::vector<uint8_t> data(200000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i * 7 + (i >> 9));
    size_t sent = 0, got = 0;
    uint8_t rb[4096], echo[4096];
    uint32_t echo_have = 0, echo_sent = 0;  // what the echo side read and has not passed on yet (it never waits for the window)
    bool same = true;
    CHECK(lan.run_until([&] {
      if (sent < data.size()) sent += c->send(ConstByteView(data.data() + sent, data.size() - sent));
      if (echo_sent == echo_have) { echo_have = s->receive(ByteView(echo)); echo_sent = 0; }
      if (echo_sent < echo_have) echo_sent += s->send(ConstByteView(echo + echo_sent, echo_have - echo_sent));
      for (uint32_t n; (n = c->receive(ByteView(rb))) > 0; got += n) same = same && memcmp(rb, data.data() + got, n) == 0;
      return got == data.size(); }, 120000));
    CHECK(same && bad == 0 && big == 0 && v6_frames > 100);
    CHECK(lan.nic_a.refused_segments == 0 && lan.nic_b.refused_segments == 0);
    if (mode >= 1) CHECK(lan.nic_a.segmented > 0 && lan.nic_a.offloaded > 0 && (lan.a.coalesced_segments() > 0 || lan.b.coalesced_segments() > 0));
    else CHECK(lan.nic_a.segmented == 0 && lan.b.coalesced_segments() > 0);
    lan.wire.tap = nullptr;
  }
}

int main() {
  test_text();
  test_link_local();
  test_router_advertisement();
  test_duplicate_detection();
  test_rules();
  test_dns_aaaa();
  test_hostile();
  test_offloads();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("Ipv6Test: all passed\n");
  return 0;
}
