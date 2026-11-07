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

// Fair sharing and latency in the network stack: the transmit queue's scheduler (EgressQueue: deficit round robin with new flows first,
// drops from the fattest flow, the sojourn limit), its flow classifier against truncated and bit-flipped frames, TCP congestion control
// (initial window, growth, the cut after loss), and two machines on a fake wire whose sending interface is a bottleneck: two bulk TCP
// connections share it evenly, and a request/answer connection keeps a short round trip while they run.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "NetworkHarness.hpp"
#include "Network/EgressQueue.hpp"
#include "Network/Tcp/TcpSocket.hpp"

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// An interface that records what it is given, and takes at most `room` frames (then refuses, as a full ring does).
struct RecordingNic final : hardware::INetworkInterface {
  std::vector<std::vector<uint8_t>> sent;
  uint32_t room = ~0u;
  const char* name() const override { return "recording nic"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  hardware::MacAddress mac_address() const override { return hardware::MacAddress{{2, 0, 0, 0, 0, 9}}; }
  bool link_up() const override { return true; }
  bool transmit(ConstByteView frame) override {
    if (room == 0) return false;
    --room;
    sent.emplace_back(frame.data(), frame.data() + frame.size());
    return true;
  }
  bool receive(ByteView, uint32_t*) override { return false; }
  hardware::NetworkStatistics statistics() const override { return hardware::NetworkStatistics{}; }
};

// An IPv4 TCP frame to 10.0.0.<host>:<port> from port 5000, `payload` bytes, the first payload byte `tag`.
static std::vector<uint8_t> tcp_frame(uint8_t host, uint16_t port, uint32_t payload, uint8_t tag) {
  std::vector<uint8_t> f(14 + 20 + 20 + payload, 0);
  f[12] = 0x08; f[13] = 0x00;
  f[14] = 0x45; f[23] = 6;
  f[26] = 10; f[29] = 1;
  f[30] = 10; f[33] = host;
  f[34] = 5000 >> 8; f[35] = 5000 & 0xFF;
  f[36] = uint8_t(port >> 8); f[37] = uint8_t(port);
  if (payload) f[54] = tag;
  return f;
}
static uint8_t tag_of(const std::vector<uint8_t>& f) { return f.size() > 54 ? f[54] : 0; }

static void test_scheduler() {
  TestAllocator alloc;
  {
    // a bulk flow of 20 frames that has been sending a while (its first turn is over), then one frame of a sparse flow: the sparse one goes
    // out next, not after the other 18; each flow keeps its order
    EgressQueue q(alloc);
    for (uint8_t i = 0; i < 20; ++i) {
      const auto f = tcp_frame(2, 80, 1400, uint8_t(i + 1));
      CHECK(q.enqueue(EgressQueue::flow_of_frame(ConstByteView(f.data(), f.size())), EgressQueue::Kind::Plain, ConstByteView(f.data(), f.size()), {}, 0, 0));
    }
    RecordingNic nic;
    nic.room = 2;
    CHECK(q.drain(nic, 1) == 2);
    const auto small = tcp_frame(3, 53, 40, 200);
    CHECK(q.enqueue(EgressQueue::flow_of_frame(ConstByteView(small.data(), small.size())), EgressQueue::Kind::Plain, ConstByteView(small.data(), small.size()), {}, 0, 0));
    CHECK(q.frames() == 19);
    nic.room = ~0u;
    CHECK(q.drain(nic, 1) == 19 && q.empty());
    int sparse_at = -1;
    uint8_t last = 0;
    bool ordered = true;
    for (size_t i = 0; i < nic.sent.size(); ++i) {
      const uint8_t t = tag_of(nic.sent[i]);
      if (t == 200) { sparse_at = int(i); continue; }
      ordered = ordered && t == last + 1;
      last = t;
    }
    CHECK(sparse_at == 2 && ordered && last == 20);
  }
  {
    // two bulk flows of equal frames: they alternate
    EgressQueue q(alloc);
    for (uint8_t i = 0; i < 10; ++i) {
      const auto a = tcp_frame(2, 80, 1400, 1), b = tcp_frame(4, 443, 1400, 2);
      q.enqueue(EgressQueue::flow_of_frame(ConstByteView(a.data(), a.size())), EgressQueue::Kind::Plain, ConstByteView(a.data(), a.size()), {}, 0, 0);
      q.enqueue(EgressQueue::flow_of_frame(ConstByteView(b.data(), b.size())), EgressQueue::Kind::Plain, ConstByteView(b.data(), b.size()), {}, 0, 0);
    }
    RecordingNic nic;
    q.drain(nic, 0);
    int a_in_first_half = 0;
    for (size_t i = 0; i < 10; ++i) a_in_first_half += tag_of(nic.sent[i]) == 1;
    CHECK(nic.sent.size() == 20 && a_in_first_half >= 4 && a_in_first_half <= 6);
  }
  {
    // a refusing interface: nothing is lost while it refuses, the order holds when it takes them again; past the sojourn limit they are dropped
    EgressQueue q(alloc);
    for (uint8_t i = 1; i <= 5; ++i) {
      const auto f = tcp_frame(2, 80, 100, i);
      q.enqueue(EgressQueue::flow_of_frame(ConstByteView(f.data(), f.size())), EgressQueue::Kind::Plain, ConstByteView(f.data(), f.size()), {}, 0, 0);
    }
    RecordingNic nic;
    nic.room = 2;
    CHECK(q.drain(nic, 10) == 2 && q.frames() == 3);
    nic.room = ~0u;
    CHECK(q.drain(nic, 20) == 3 && tag_of(nic.sent[2]) == 3 && tag_of(nic.sent[4]) == 5);
    const auto f = tcp_frame(2, 80, 100, 9);
    q.enqueue(EgressQueue::flow_of_frame(ConstByteView(f.data(), f.size())), EgressQueue::Kind::Plain, ConstByteView(f.data(), f.size()), {}, 0, 100);
    CHECK(q.drain(nic, 100 + EgressQueue::kMaxSojournMs + 1) == 0 && q.empty() && q.stats().dropped_stale == 1);
  }
  {
    // full: the fattest flow pays - a sparse flow's frame still gets in
    EgressQueue q(alloc);
    for (uint32_t i = 0; i < EgressQueue::kSlots + 10; ++i) {
      const auto f = tcp_frame(2, 80, 1000, 1);
      q.enqueue(EgressQueue::flow_of_frame(ConstByteView(f.data(), f.size())), EgressQueue::Kind::Plain, ConstByteView(f.data(), f.size()), {}, 0, 0);
    }
    CHECK(q.frames() == EgressQueue::kSlots && q.stats().dropped_full == 10);
    const auto small = tcp_frame(3, 53, 40, 7);
    const uint32_t flow = EgressQueue::flow_of_frame(ConstByteView(small.data(), small.size()));
    CHECK(q.enqueue(flow, EgressQueue::Kind::Plain, ConstByteView(small.data(), small.size()), {}, 0, 0) && q.queued_in_flow(flow) == 1);
    // bytes are bounded too: big frames
    EgressQueue big(alloc);
    std::vector<uint8_t> huge = tcp_frame(5, 80, 60000, 1);
    for (int i = 0; i < 30; ++i) big.enqueue(1, EgressQueue::Kind::Plain, ConstByteView(huge.data(), huge.size()), {}, 0, 0);
    CHECK(big.bytes() <= EgressQueue::kMaxBytes && big.frames() == EgressQueue::kMaxBytes / huge.size());
    big.clear();
    CHECK(big.empty() && big.bytes() == 0);
  }
  CHECK(g_live == 0);
}

// The classifier reads frames from the wire's side of the stack (its own, but parsed all the same): every truncation and bit flip of IPv4 and
// IPv6 TCP frames gives some flow without reading past the frame, and a frame's flow matches what its socket computes.
static void test_classifier() {
  const auto v4 = tcp_frame(2, 8080, 10, 0);
  CHECK(EgressQueue::flow_of_frame(ConstByteView(v4.data(), v4.size())) ==
        EgressQueue::flow_of(IpAddress(Ipv4Address::make(10, 0, 0, 2)), kProtocolTcp, 5000, 8080));
  std::vector<uint8_t> v6(14 + 40 + 20, 0);
  v6[12] = 0x86; v6[13] = 0xDD; v6[14] = 0x60; v6[20] = 6;
  const uint8_t dst[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7};
  memcpy(&v6[38], dst, 16);
  v6[54] = 0x13; v6[55] = 0x88; v6[56] = 0x01; v6[57] = 0xBB;
  Ipv6Address d;
  memcpy(d.bytes, dst, 16);
  CHECK(EgressQueue::flow_of_frame(ConstByteView(v6.data(), v6.size())) == EgressQueue::flow_of(IpAddress(d), kProtocolTcp, 5000, 443));
  const std::vector<uint8_t>* frames[] = {&v4, &v6};
  for (const std::vector<uint8_t>* frame : frames) {
    for (size_t cut = 0; cut < frame->size(); ++cut) CHECK(EgressQueue::flow_of_frame(ConstByteView(frame->data(), cut)) < EgressQueue::kFlows);
    for (size_t bit = 0; bit < frame->size() * 8; ++bit) {
      std::vector<uint8_t> f = *frame;
      f[bit / 8] ^= uint8_t(1u << (bit % 8));
      CHECK(EgressQueue::flow_of_frame(ConstByteView(f.data(), f.size())) < EgressQueue::kFlows);
    }
  }
  const auto arp = std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x08, 0x06, 0, 1};
  CHECK(EgressQueue::flow_of_frame(ConstByteView(arp.data(), arp.size())) == 0);
}

// A bulk sender on stack A to a sink on stack B (it reads everything); returns the bytes taken so far.
struct Bulk {
  SharedPointer<ITcpSocket> client, listener, server;
  uint64_t received = 0;
  std::vector<uint8_t> data = std::vector<uint8_t>(16384, 0x5A);
  Bulk(Lan& lan, uint16_t port) {
    listener = lan.b.tcp_open();
    CHECK(listener->listen(port));
    client = lan.a.tcp_open();
    CHECK(client->connect(Ipv4Address::make(10, 0, 0, 2), port));
  }
  void step() {
    if (!server) server = listener->accept();
    if (client->is_connected()) client->send(ConstByteView(data.data(), data.size()));
    uint8_t buffer[4096];
    if (server)
      for (uint32_t n; (n = server->receive(ByteView(buffer))) > 0;) received += n;
  }
};

static void test_congestion_window() {
  Lan lan;
  lan.configure_static();
  Bulk bulk(lan, 7000);
  auto* tcp = static_cast<TcpSocket*>(bulk.client.get());
  CHECK(tcp->congestion_window() == 0);   // (set when data first goes out)
  for (int i = 0; i < 20 && !bulk.client->is_connected(); ++i) { lan.step(); bulk.step(); }
  bulk.step();
  CHECK(tcp->congestion_window() == 14600);   // RFC 6928: min(10 * 1460, max(2 * 1460, 14600))
  for (int i = 0; i < 200; ++i) { lan.step(); bulk.step(); }
  CHECK(tcp->congestion_window() > 14600 && bulk.received > 100000);   // slow start grew it
  // losses: the window comes down (a fast retransmit halves it, a timeout drops it to a segment), and the data still all arrives
  const uint32_t before = tcp->congestion_window();
  lan.wire.loss_percent = 20;
  uint32_t lowest = before;
  for (int i = 0; i < 3000; ++i) {
    lan.step();
    bulk.step();
    if (tcp->congestion_window() < lowest) lowest = tcp->congestion_window();
  }
  CHECK(lowest < before && lowest >= 1460 && tcp->slow_start_threshold() < (1u << 20));
  lan.wire.loss_percent = 0;
  const uint64_t had = bulk.received;
  for (int i = 0; i < 2000; ++i) { lan.step(); bulk.step(); }
  CHECK(bulk.received > had + 100000 && !bulk.client->failed());
}

// Two bulk connections through an interface that takes 8 frames a millisecond: each gets close to half, the queue stays bounded, and a
// request/answer connection beside them is answered in a step or two instead of waiting behind them (or for a retransmission).
static void test_shared_bottleneck() {
  Lan lan;
  lan.configure_static();
  lan.nic_a.frames_per_ms = 8;
  Bulk one(lan, 7100), two(lan, 7200);
  SharedPointer<ITcpSocket> echo_listener = lan.b.tcp_open(), echo_server;
  CHECK(echo_listener->listen(7300));
  SharedPointer<ITcpSocket> asker = lan.a.tcp_open();
  CHECK(asker->connect(Ipv4Address::make(10, 0, 0, 2), 7300));
  uint64_t asked_at = 0, slowest = 0, total_wait = 0;
  uint32_t answers = 0;
  bool waiting = false;
  for (int ms = 0; ms < 8000; ++ms) {
    lan.step();
    one.step();
    two.step();
    if (!echo_server) echo_server = echo_listener->accept();
    uint8_t b[64];
    if (echo_server)
      for (uint32_t n; (n = echo_server->receive(ByteView(b))) > 0;) echo_server->send(ConstByteView(b, n));
    if (ms < 1000 || !asker->is_connected()) continue;   // (let the bulk connections fill the queue first)
    if (!waiting && ms % 50 == 0) {
      const uint8_t q[8] = {'p', 'i', 'n', 'g'};
      if (asker->send(ConstByteView(q)) == sizeof q) { waiting = true; asked_at = uint64_t(ms); }
    }
    if (waiting && asker->available() >= 8) {
      asker->receive(ByteView(b, 8));
      const uint64_t took = uint64_t(ms) - asked_at;   // (in steps of the wire: the fake clock also moves as it is read, more so under load)
      total_wait += took;
      if (took > slowest) slowest = took;
      ++answers;
      waiting = false;
    }
  }
  const double a = double(one.received), b = double(two.received);
  const double fairness = (a + b) * (a + b) / (2 * (a * a + b * b));   // Jain's index: 1 is an even split
  printf("bottleneck: %.0f and %.0f bytes (fairness %.3f), %u answers, %.2f steps on average, slowest %llu, %llu refused, egress dropped %llu\n", a, b, fairness,
         answers, answers ? double(total_wait) / answers : 0.0, (unsigned long long)slowest, (unsigned long long)lan.nic_a.refused,
         (unsigned long long)(lan.a.egress_stats().dropped_full + lan.a.egress_stats().dropped_stale));
  CHECK(a > 1000000 && b > 1000000);
  CHECK(fairness > 0.95);
  CHECK(answers > 100);
  CHECK(slowest <= 4 && total_wait <= 2 * uint64_t(answers));   // (a step is a millisecond; an RTO, what a dropped request costs without a fair queue, is 200)
  CHECK(lan.nic_a.refused > 0);   // (the bottleneck was really there)
}

int main() {
  test_scheduler();
  test_classifier();
  test_congestion_window();
  test_shared_bottleneck();
  if (g_failed) { printf("NetworkFairnessTest: %d failed\n", g_failed); return 1; }
  printf("NetworkFairnessTest: all passed\n");
  return 0;
}
