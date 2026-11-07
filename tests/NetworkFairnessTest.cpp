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
// drops from the fattest flow, the sojourn limit, CoDel's drops and ECN marks), its flow classifier against truncated and bit-flipped
// frames, TCP congestion control (initial window, growth, the cut after loss), SACK (the range set against a model, a lost segment that is
// the only one sent again), ECN end to end, pacing on a long path, and two machines on a fake wire whose sending interface is a bottleneck:
// two bulk TCP connections share it evenly, and a request/answer connection keeps a short round trip while they run.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "NetworkHarness.hpp"
#include "Network/EgressQueue.hpp"
#include "Network/Tcp/TcpSocket.hpp"
#include "Network/Tcp/SequenceRanges.hpp"
#include <set>

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

// SequenceRanges against a plain set of byte numbers: random adds and cuts, near the 2^32 wrap; every range stays sorted, disjoint and apart.
static void test_sequence_ranges() {
  uint32_t seed = 7;
  auto rnd = [&] { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (int round = 0; round < 200; ++round) {
    SequenceRanges r;
    std::set<uint32_t> model;   // offsets from base
    const uint32_t base = 0xFFFFF000u + rnd() % 0x2000;   // across the wrap
    uint32_t low = 0;           // everything below this offset was cut
    for (int op = 0; op < 60; ++op) {
      if (rnd() % 5 == 0) {
        low += rnd() % 300;
        r.remove_below(base + low);
        for (auto it = model.begin(); it != model.end();) it = *it < low ? model.erase(it) : std::next(it);
      } else {
        const uint32_t start = low + rnd() % 4000, length = 1 + rnd() % 400;
        std::vector<std::pair<uint32_t, uint32_t>> before;
        for (uint32_t i = 0; i < r.count(); ++i) before.emplace_back(r.at(i).start, r.at(i).end);
        if (r.add(base + start, base + start + length)) {
          for (uint32_t i = start; i < start + length; ++i) model.insert(i);
        } else {
          CHECK(r.count() == SequenceRanges::kMax);   // (refused only when full, and then unchanged)
          bool same = before.size() == r.count();
          for (uint32_t i = 0; same && i < r.count(); ++i) same = before[i].first == r.at(i).start && before[i].second == r.at(i).end;
          CHECK(same);
        }
      }
      uint32_t bytes = 0;
      for (uint32_t i = 0; i < r.count(); ++i) {
        CHECK(SequenceRanges::before(r.at(i).start, r.at(i).end));
        if (i) CHECK(SequenceRanges::before(r.at(i - 1).end, r.at(i).start));   // (apart: touching ones merge)
        bytes += r.at(i).end - r.at(i).start;
      }
      CHECK(bytes == model.size() && r.bytes() == bytes);
      for (int probe = 0; probe < 20; ++probe) {
        const uint32_t o = low + rnd() % 4500;
        CHECK(r.covers(base + o) == (model.count(o) == 1));
      }
      uint32_t gs, ge;
      if (r.next_gap(base + low, base + low + 5000, &gs, &ge)) CHECK(!r.covers(gs) && SequenceRanges::before(gs, ge) && (ge == base + low + 5000 || r.covers(ge)));
    }
  }
}

// A transfer in which exactly one data segment is lost: with SACK the sender sends that one again, at once and alone (none of the segments
// after it, which the receiver already holds); the receiver puts the held data in order.
static void test_sack_recovery() {
  Lan lan;
  lan.configure_static();
  int data_frames = 0, dropped = 0;
  std::set<uint32_t> sequences;
  uint32_t retransmitted = 0;
  int step = 0, dropped_at = -1, resent_at = -1;
  lan.wire.tap = [&](const std::vector<uint8_t>& f, int from) {
    if (from != 0 || f.size() < 54 || f[23] != 6) return;
    const uint32_t header = (f[46] >> 4) * 4u, total = uint32_t(f[16] << 8 | f[17]);
    if (total <= 20 + header) return;   // (no data)
    const uint32_t seq = uint32_t(f[38]) << 24 | uint32_t(f[39]) << 16 | uint32_t(f[40]) << 8 | f[41];
    if (!sequences.insert(seq).second && ++retransmitted == 1) resent_at = step;
    if (++data_frames == 5 && !dropped) {   // the fifth data segment is lost
      ++dropped;
      dropped_at = step;
      lan.wire.loss_percent = 100;   // (just this one: put back on the next frame)
    } else {
      lan.wire.loss_percent = 0;
    }
  };
  Bulk bulk(lan, 7400);
  auto* client = static_cast<TcpSocket*>(bulk.client.get());
  for (; step < 400 && bulk.received < 60000; ++step) { lan.step(); bulk.step(); }
  printf("sack: the lost segment was sent again %d steps after it was lost; %u retransmitted\n", resent_at - dropped_at, retransmitted);
  CHECK(client->sack_permitted() && client->ecn_capable());
  CHECK(dropped == 1 && bulk.received >= 60000);
  CHECK(retransmitted == 1);   // (go-back-N would have sent the four after it again as well)
  // (and at once: the segments after it arrived coalesced into one - one duplicate ACK - but what the receiver holds says it is lost; without
  // SACK only the retransmission timer finds it, 165 steps later)
  CHECK(resent_at >= dropped_at && resent_at - dropped_at <= 5);
  lan.wire.tap = nullptr;
}

// CoDel in the transmit queue: a flow whose frames keep waiting more than the target for an interval loses one, then more; frames of an
// ECN-capable packet are marked instead (and still sent).
static void test_codel() {
  TestAllocator alloc;
  for (int ect = 0; ect < 2; ++ect) {
    EgressQueue q(alloc);
    RecordingNic nic;
    uint64_t now = 0;
    for (int ms = 0; ms < 400; ++ms, ++now) {   // two frames in, one out per millisecond: a standing queue
      for (int k = 0; k < 2; ++k) {
        auto f = tcp_frame(2, 80, 1000, uint8_t(k));
        if (ect) f[15] = kEcnEct0;
        q.enqueue(EgressQueue::flow_of_frame(ConstByteView(f.data(), f.size())), EgressQueue::Kind::Plain, ConstByteView(f.data(), f.size()), {}, 0, now);
      }
      nic.room = 1;
      q.drain(nic, now);
    }
    const EgressQueue::Stats st = q.stats();
    if (ect) {
      int marked = 0;
      bool checksums = true;
      for (const auto& f : nic.sent) {
        if ((f[15] & 3) != kEcnCe) continue;
        ++marked;
        uint32_t sum = 0;   // (a marked frame's IPv4 header checksum is redone)
        for (int i = 14; i < 34; i += 2) sum += uint32_t(f[i] << 8 | f[i + 1]);
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        checksums = checksums && sum == 0xFFFF;
      }
      CHECK(st.marked > 0 && st.dropped_codel == 0 && marked == int(st.marked) && checksums);
    } else {
      CHECK(st.dropped_codel > 0 && st.marked == 0);
    }
    printf("codel (%s): %llu dropped by CoDel, %llu marked, %llu over the sojourn limit, %llu dropped full\n", ect ? "ECN" : "no ECN",
           (unsigned long long)st.dropped_codel, (unsigned long long)st.marked, (unsigned long long)st.dropped_stale, (unsigned long long)st.dropped_full);
  }
}

// ECN end to end: the receiver echoes a CE mark (ECE until the sender's CWR), and the sender cuts its window without retransmitting.
static void test_ecn() {
  Lan lan;
  lan.configure_static();
  bool mark_next = false;
  int ect = 0, ece = 0, cwr = 0, not_ect_data = 0;
  lan.wire.tap = [&](const std::vector<uint8_t>& f, int from) {
    if (f.size() < 54 || f[23] != 6) return;
    const uint32_t header = (f[46] >> 4) * 4u, total = uint32_t(f[16] << 8 | f[17]);
    if (from == 0 && total > 20 + header) {
      if ((f[15] & 3) == kEcnEct0) ++ect;
      else ++not_ect_data;
      if (f[47] & tcp_flags::kCwr) ++cwr;
      if (mark_next) {   // a congested router on the way marks CE (and redoes the IPv4 header checksum, which the receiver checks)
        auto& g = const_cast<std::vector<uint8_t>&>(f);
        g[15] |= kEcnCe;
        g[24] = g[25] = 0;
        uint32_t sum = 0;
        for (int i = 14; i < 34; i += 2) sum += uint32_t(g[i] << 8 | g[i + 1]);
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        g[24] = uint8_t(~sum >> 8);
        g[25] = uint8_t(~sum);
        mark_next = false;
      }
    }
    if (from == 1 && (f[47] & tcp_flags::kEce) && !(f[47] & tcp_flags::kSyn)) ++ece;
  };
  Bulk bulk(lan, 7500);
  auto* client = static_cast<TcpSocket*>(bulk.client.get());
  for (int i = 0; i < 300; ++i) { lan.step(); bulk.step(); }
  CHECK(client->ecn_capable() && ect > 20 && not_ect_data == 0);
  const uint32_t before = client->congestion_window();
  const uint64_t had = bulk.received;
  mark_next = true;
  for (int i = 0; i < 50; ++i) { lan.step(); bulk.step(); }
  printf("ecn: %d ACKs with ECE, %d CWR, window %u -> %u (ssthresh %u)\n", ece, cwr, before, client->congestion_window(), client->slow_start_threshold());
  CHECK(ece >= 1 && cwr == 1 && client->congestion_window() < before && client->slow_start_threshold() < before);
  for (int i = 0; i < 300; ++i) { lan.step(); bulk.step(); }
  CHECK(bulk.received > had + 100000);
  lan.wire.tap = nullptr;
}

// Pacing on a path with a 40 ms round trip: the window goes out spread over the round trip, not as one burst in a millisecond.
static void test_pacing() {
  Lan lan;
  lan.configure_static();
  lan.clock.auto_advance = false;   // (the clock is the wire's here: it moves by the step only)
  lan.wire.delay_us = 20000;
  std::vector<int> per_ms(20000, 0);
  lan.wire.tap = [&](const std::vector<uint8_t>& f, int from) {
    if (from != 0 || f.size() < 54 || f[23] != 6) return;
    const uint32_t header = (f[46] >> 4) * 4u, total = uint32_t(f[16] << 8 | f[17]);
    if (total > 20 + header && lan.clock.us / 1000 < per_ms.size()) ++per_ms[lan.clock.us / 1000];
  };
  Bulk bulk(lan, 7600);
  for (int i = 0; i < 6000; ++i) { lan.step(); bulk.step(); }
  int busiest_late = 0;   // (after the first round trips: the first window has no RTT to pace by)
  for (size_t ms = 1000; ms < 6000; ++ms) busiest_late = per_ms[ms] > busiest_late ? per_ms[ms] : busiest_late;
  printf("pacing: %llu bytes in 6 s over a 40 ms round trip, at most %d data segments in one millisecond\n", (unsigned long long)bulk.received, busiest_late);
  CHECK(bulk.received > 1000000);   // (a 16 KiB window per 40 ms: about 2.4 MB in 6 s)
  CHECK(busiest_late <= 5);   // (unpaced, the whole window - 11 segments - leaves in the millisecond its ACK arrives)
  lan.wire.tap = nullptr;
}

int main() {
  test_scheduler();
  test_classifier();
  test_congestion_window();
  test_sequence_ranges();
  test_sack_recovery();
  test_codel();
  test_ecn();
  test_pacing();
  test_shared_bottleneck();
  if (g_failed) { printf("NetworkFairnessTest: %d failed\n", g_failed); return 1; }
  printf("NetworkFairnessTest: all passed\n");
  return 0;
}
